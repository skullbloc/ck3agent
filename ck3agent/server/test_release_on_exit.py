"""rc2 B7: the MCP server releases its OWN claims when it is stopped, and keeps
the session it claims through on disk so `ck3agent run` can resume it when the
server is SIGKILLed first.

The end-to-end tests live in release/overlay/tests/test_cli.py and run only on
an exported tree; these run under `mise run test`. They pin:

- _record_session_at_handshake: opt-in, 0600, rewritten at EVERY handshake;
- _try_reconnect: re-records the new session (CK3_SESSION_OUT must name the
  session holding the claims) and, under release-on-exit only, presents the old
  resume token;
- _release_on_exit_now: what it releases, the two-round floor, the
  RELEASE_ROUNDS cap, and the record it writes;
- _begin_exit: release FIRST, then cancel, including for a signal that arrived
  before the release was armed.
"""
import asyncio
import json
import os
import signal
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import async_bridge_client  # noqa: E402
import mcp_server  # noqa: E402


def _run(coro):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


@pytest.fixture
def armed(tmp_path, monkeypatch):
    """CK3_RELEASE_ON_EXIT + CK3_SESSION_OUT set, the module's exit state fresh."""
    rel, ses = tmp_path / "run.release.json", tmp_path / "run.session.json"
    monkeypatch.setenv(mcp_server.RELEASE_ON_EXIT_ENV, str(rel))
    monkeypatch.setenv(mcp_server.SESSION_OUT_ENV, str(ses))
    monkeypatch.delenv(mcp_server.AGENT_CHARACTER_ENV, raising=False)
    for name in ("_EXIT_SIGNAL", "_EXIT_TASK", "_RELEASE", "_STOPPER"):
        monkeypatch.setattr(mcp_server, name, [])
    return rel, ses


class _Client:
    """Stands in for AsyncBridgeClient: each handshake mints the next session."""
    minted = 0

    def __init__(self):
        self.handshake_ack = None
        self.handshake_kwargs = None
        self.closed = False

    async def connect(self):
        return True

    async def close(self):
        self.closed = True

    async def handshake(self, *a, **kw):
        self.handshake_kwargs = kw
        tok = kw.get("resume_token")
        if tok and tok == getattr(_Client, "resumable", None):
            sid, extra = tok.replace("tok-", ""), {"resumed": True, "resume_status": "ok"}
        else:
            _Client.minted += 1
            sid = f"s{_Client.minted}"
            extra = {"resumed": False, "resume_status": "unknown_token"} if tok else {}
        self.handshake_ack = {"status": "ok", "session_id": sid, "resume_token": f"tok-{sid}", **extra}
        return self.handshake_ack

    @property
    def session_id(self):
        return (self.handshake_ack or {}).get("session_id")


# ── _record_session_at_handshake ─────────────────────────────────────────────

def test_record_is_off_unless_both_variables_are_set(tmp_path, monkeypatch):
    for env in ({mcp_server.SESSION_OUT_ENV: str(tmp_path / "s.json")},
                {mcp_server.RELEASE_ON_EXIT_ENV: str(tmp_path / "r.json")}):
        monkeypatch.delenv(mcp_server.SESSION_OUT_ENV, raising=False)
        monkeypatch.delenv(mcp_server.RELEASE_ON_EXIT_ENV, raising=False)
        for k, v in env.items():
            monkeypatch.setenv(k, v)
        c = _Client()
        inner = c.handshake
        mcp_server._record_session_at_handshake(c)
        assert c.handshake == inner                      # not wrapped
    assert list(tmp_path.iterdir()) == []


def test_record_is_0600_and_rewritten_at_every_handshake(armed):
    _, ses = armed
    c = _Client()
    mcp_server._record_session_at_handshake(c)
    a1 = _run(c.handshake(client_name="ck3agent"))
    assert stat.S_IMODE(ses.stat().st_mode) == 0o600
    rec = json.loads(ses.read_text())
    assert rec["session_id"] == a1["session_id"] and rec["resume_token"] == a1["resume_token"]
    assert rec["pid"] == os.getpid()
    a2 = _run(c.handshake(client_name="ck3agent"))
    assert a2["session_id"] != a1["session_id"]
    assert json.loads(ses.read_text())["session_id"] == a2["session_id"]
    assert [p.name for p in ses.parent.iterdir() if ".tmp" in p.name] == []


def test_a_failed_handshake_leaves_the_record_alone(armed):
    _, ses = armed
    c = _Client()

    async def refused(*a, **kw):
        return None
    c.handshake = refused
    mcp_server._record_session_at_handshake(c)
    assert _run(c.handshake()) is None and not ses.exists()


# ── _try_reconnect ───────────────────────────────────────────────────────────

def _reconnect(monkeypatch, old):
    """Drive _try_reconnect with `old` as the dropped client; return the new one."""
    async def listener(state):
        await asyncio.sleep(0)
    monkeypatch.setattr(async_bridge_client, "AsyncBridgeClient", _Client)
    monkeypatch.setattr(mcp_server, "_bridge_listener", listener)
    st = mcp_server.BridgeState()
    st._client, st.connected = old, False

    async def go():
        ok = await mcp_server._try_reconnect(st)
        if st._listener_task:
            await st._listener_task
        return ok
    assert _run(go()) is True and st.connected
    assert st._client is not old and old.closed
    return st._client, st


def test_a_reconnect_rerecords_the_new_session(armed, monkeypatch):
    """The resume is refused (the bridge destroyed the session): the fresh
    session is the one holding the claims, so it is the one on disk."""
    _, ses = armed
    monkeypatch.setattr(_Client, "resumable", None, raising=False)
    old = _Client()
    mcp_server._record_session_at_handshake(old)
    first = _run(old.handshake())["session_id"]
    assert json.loads(ses.read_text())["session_id"] == first
    new, st = _reconnect(monkeypatch, old)
    assert new.handshake_kwargs["resume_token"] == f"tok-{first}"
    assert new.session_id != first
    assert json.loads(ses.read_text())["session_id"] == new.session_id
    assert st.handshake_info["resume_status"] == "unknown_token"


def test_a_reconnect_resumes_the_old_session_when_the_bridge_kept_it(armed, monkeypatch):
    _, ses = armed
    old = _Client()
    first = _run(old.handshake())["session_id"]
    monkeypatch.setattr(_Client, "resumable", f"tok-{first}", raising=False)
    new, st = _reconnect(monkeypatch, old)
    assert new.session_id == first and st.handshake_info["resumed"] is True
    assert json.loads(ses.read_text())["session_id"] == first


def test_without_release_on_exit_a_reconnect_presents_no_token(tmp_path, monkeypatch):
    """An experiment episode never sets CK3_RELEASE_ON_EXIT; its reconnect is unchanged."""
    monkeypatch.delenv(mcp_server.RELEASE_ON_EXIT_ENV, raising=False)
    monkeypatch.setenv(mcp_server.SESSION_OUT_ENV, str(tmp_path / "s.json"))
    monkeypatch.delenv(mcp_server.AGENT_CHARACTER_ENV, raising=False)
    old = _Client()
    _run(old.handshake())
    new, _ = _reconnect(monkeypatch, old)
    assert "resume_token" not in new.handshake_kwargs
    assert not (tmp_path / "s.json").exists()


# ── _release_on_exit_now / RELEASE_ROUNDS ────────────────────────────────────

def _release(monkeypatch, snapshots, refuse=(), connected=True, sid="s1"):
    """Run _release_on_exit_now; `snapshots(round)` gives the session's held ids."""
    sent = []
    rounds = [0]
    held = set()

    async def send(state, action, params, timeout=15.0):
        sent.append((action, params))
        if action == "get_bridge_state":
            rounds[0] += 1
            held.update(snapshots(rounds[0]))
            return {"data": {"sessions": [{"session_id": "other", "claimed_character_ids": [7]},
                                          {"session_id": sid, "claimed_character_ids": sorted(held)}]}}
        cid = params["character_id"]
        if cid in refuse:
            return {"success": False, "reason": "not_owned_by_session"}
        held.discard(cid)
        return {"success": True}
    monkeypatch.setattr(mcp_server, "_send_and_wait", send)
    st = mcp_server.BridgeState()
    c = _Client()
    c.handshake_ack = {"session_id": sid}
    st._client, st.connected = c, connected
    _run(mcp_server._release_on_exit_now(st))
    return sent


def test_release_frees_only_this_sessions_claims_and_writes_the_record(armed, monkeypatch):
    rel, _ = armed
    sent = _release(monkeypatch, lambda r: [42, 43] if r == 1 else [], refuse={43})
    assert ("release_character", {"character_id": 7}) not in sent
    rec = json.loads(rel.read_text())
    assert rec["trigger"] == "shutdown" and rec["session_id"] == "s1"
    assert rec["released"] == [42] and rec["refused"] == {"43": "not_owned_by_session"}
    assert rec["claims"] == [42, 43] and rec["held_after"] == [43] and rec["error"] is None
    assert rec["rounds"] == 2                     # the floor: a second snapshot, even with nothing new


def test_a_claim_that_lands_after_the_first_snapshot_is_released(armed, monkeypatch):
    rel, _ = armed
    _release(monkeypatch, lambda r: [42] if r == 2 else [])
    rec = json.loads(rel.read_text())
    assert rec["released"] == [42] and rec["rounds"] == 3 and rec["held_after"] == []


def test_release_rounds_caps_a_session_that_keeps_claiming(armed, monkeypatch):
    rel, _ = armed
    sent = _release(monkeypatch, lambda r: [100 + r])
    rec = json.loads(rel.read_text())
    assert rec["rounds"] == mcp_server.RELEASE_ROUNDS + 1
    assert rec["released"] == [100 + r for r in range(1, mcp_server.RELEASE_ROUNDS + 1)]
    assert sum(1 for a, _ in sent if a == "get_bridge_state") == mcp_server.RELEASE_ROUNDS + 1


def test_no_bridge_session_is_an_error_in_the_record_not_a_raise(armed, monkeypatch):
    rel, _ = armed
    sent = _release(monkeypatch, lambda r: [42], connected=False)
    rec = json.loads(rel.read_text())
    assert sent == [] and rec["error"].startswith("ConnectionError") and rec["claims"] is None


# ── _begin_exit: release, THEN cancel ───────────────────────────────────────

def test_a_signal_before_arming_releases_before_the_main_task_is_cancelled(armed, monkeypatch):
    """SIGINT lands during startup (before the ARMED line): _arm_exit_signals
    starts the exit. The release must complete before the main task is
    cancelled — cancel-only would leave the release to an unwind that Claude
    Code's SIGKILL may not wait for."""
    events = []

    async def release_now(state):
        events.append("release")
        await asyncio.sleep(0.05)
        events.append("released")
    monkeypatch.setattr(mcp_server, "_release_on_exit_now", release_now)

    async def main():
        mcp_server._on_exit_signal(signal.SIGINT, None)      # held: nothing armed yet
        assert mcp_server._EXIT_SIGNAL == [int(signal.SIGINT)] and events == []
        mcp_server._arm_exit_signals(mcp_server.BridgeState())
        try:
            await asyncio.sleep(5)
            events.append("not cancelled")
        except asyncio.CancelledError:
            events.append("cancelled")
        await asyncio.gather(*mcp_server._STOPPER, return_exceptions=True)
    _run(main())
    assert events == ["release", "released", "cancelled"]
