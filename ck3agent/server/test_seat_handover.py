"""2026.12.5: a seat's NEW MCP server takes over the bridge session its previous
server left, instead of minting a fresh one that the old session's claims block.

The defect (v3 pilot smoke, orders_hom, 2026-11-09; evidence
docs/cleanup-2026.11/evidence/v3pilot/ README finding B): K's claude ended at
867.09.30 and `ck3agent run` resumed it. The resumed claude started a new
server, which handshook a FRESH bridge session and pre-bound K's ruler 12015.
In an experiment episode the bridge pins a dropped seat session's claims and
refuses a control session's last release, so the old session still held 12015:
five `owned_by_other` refusals, K unbound for 94 game-days.

The fix: the server reads the previous session's resume token from the session
record (CK3_SESSION_OUT) BEFORE its first handshake rewrites it, and presents
it; the bridge resumes that session, claims intact (session.cpp
resume_by_token). A live session is not resumable (`session_attached`), so a
live holder still refuses the bind.

These drive `bridge_lifespan` itself (the path a server takes at start) with a
stand-in client whose handshake follows resume_by_token's three answers. The
end-to-end rows (a real server under a fake claude and a fake bridge that pins
claims) are in release/overlay/tests/test_cli.py and run on an exported tree.

Red on 5718f403 (the pre-fix tree, measured on the node): row 1, row 2 (no
token is presented), row 4 resumed=True (5 bound first). Row 3 and row 4
resumed=False are NON-DISCRIMINATING controls, green before and after: a server
with nothing to take over, or outside `ck3agent run`, handshakes and binds
exactly as before.
"""
import asyncio
import json
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


class _Client:
    """AsyncBridgeClient's surface the lifespan uses. The handshake answers as
    session.cpp resume_by_token does: a DETACHED session's token resumes it
    (`resumed: true`, status ok); an ATTACHED one's is refused
    `session_attached` and a fresh session is minted; an unknown token is
    `unknown_token`, fresh. ipc_server.cpp writes `resumed`/`resume_status`
    into the ack (handshake_ack format string)."""
    detached: dict = {}            # token -> session id, resumable
    attached: dict = {}            # token -> session id, live elsewhere
    made: list = []

    def __init__(self):
        self.handshake_ack = None
        self.handshake_kwargs = None
        _Client.made.append(self)

    async def connect(self):
        return True

    async def close(self):
        pass

    async def handshake(self, *a, **kw):
        self.handshake_kwargs = kw
        tok = kw.get("resume_token")
        if tok and tok in _Client.detached:
            sid, extra = _Client.detached[tok], {"resumed": True, "resume_status": "ok"}
        else:
            sid = f"fresh{len(_Client.made)}"
            extra = ({"resumed": False,
                      "resume_status": "session_attached" if tok in _Client.attached else "unknown_token"}
                     if tok else {"resumed": False, "resume_status": ""})
        tok_out = tok if extra.get("resumed") else f"tok-{sid}"
        self.handshake_ack = {"status": "ok", "session_id": sid, "resume_token": tok_out,
                              "session_state": "active", **extra}
        return self.handshake_ack

    @property
    def session_id(self):
        return (self.handshake_ack or {}).get("session_id")


@pytest.fixture
def seat(tmp_path, monkeypatch):
    """`ck3agent run`'s server env; the bridge stubbed at _send_and_wait."""
    rel, ses = tmp_path / "run.release.json", tmp_path / "run.session.json"
    monkeypatch.setenv(mcp_server.RELEASE_ON_EXIT_ENV, str(rel))
    monkeypatch.setenv(mcp_server.SESSION_OUT_ENV, str(ses))
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, "42")
    for name in ("_EXIT_SIGNAL", "_EXIT_TASK", "_RELEASE", "_STOPPER"):
        monkeypatch.setattr(mcp_server, name, [])
    monkeypatch.setattr(_Client, "detached", {}, raising=False)
    monkeypatch.setattr(_Client, "attached", {}, raising=False)
    monkeypatch.setattr(_Client, "made", [], raising=False)
    monkeypatch.setattr(async_bridge_client, "AsyncBridgeClient", _Client)

    async def listener(state):
        await asyncio.sleep(0)
    monkeypatch.setattr(mcp_server, "_bridge_listener", listener)
    sent: list = []
    primary: dict = {}

    async def send(state, action, params, timeout=15.0):
        sent.append((state._client.session_id, action, dict(params)))
        if action == "get_bridge_state":
            # session.cpp Session::to_json: session_id, primary_character_id,
            # claimed_character_ids
            sid = state._client.session_id
            return {"sessions": [{"session_id": sid, "primary_character_id": primary.get(sid, 0),
                                  "claimed_character_ids": []}]}
        if action == "release_character":
            return {"success": False, "reason": "last_claim_of_control_session"}
        return {"success": True, "added": bool(params.get("add")), "tick": 3, "game_date": "867.1.4"}
    monkeypatch.setattr(mcp_server, "_send_and_wait", send)
    return ses, sent, primary


def _start_server():
    """Enter and leave the server's lifespan; return its state."""
    async def go():
        async with mcp_server.bridge_lifespan(None) as st:
            await asyncio.sleep(0)
            return st
    return _run(go())


def _binds(sent):
    return [(s, p["character_id"], bool(p.get("add"))) for s, a, p in sent
            if a == "set_controlled_character"]


def test_a_new_server_takes_over_the_session_its_predecessor_left(seat):
    """Row 1. The record names the old session (detached, holding 42): the new
    server presents its token, the session is resumed and the pre-bind of 42
    lands on THAT session. Red on 5718f403: no token presented, a fresh session
    binds 42 (on the bridge: owned_by_other)."""
    ses, sent, _ = seat
    ses.write_text(json.dumps({"session_id": "old7", "resume_token": "tok-old7", "pid": 1}))
    _Client.detached["tok-old7"] = "old7"
    st = _start_server()
    assert _Client.made[0].handshake_kwargs.get("resume_token") == "tok-old7"
    assert st.resumed_session is True and st.handshake_info["resumed"] is True
    assert _binds(sent) == [("old7", 42, False)]
    rec = json.loads(ses.read_text())
    assert rec["session_id"] == "old7" and rec["resumed"] is True and rec["resume_status"] == "ok"


@pytest.mark.parametrize("held", ["attached", "unknown"])
def test_a_session_that_cannot_be_taken_over_is_not_and_says_why(seat, held):
    """Row 2 (red on 5718f403: no token presented; the bind's outcome is the
    same before and after, which is the point). A LIVE session (attached elsewhere) is not
    resumable: the bridge mints a fresh one, and a bind of a ruler the live one
    holds stays the bridge's refusal. An unknown token (the bridge dropped the
    session) is the same fresh start. Never a steal."""
    ses, sent, _ = seat
    ses.write_text(json.dumps({"session_id": "live3", "resume_token": "tok-live3"}))
    if held == "attached":
        _Client.attached["tok-live3"] = "live3"
    st = _start_server()
    assert _Client.made[0].handshake_kwargs.get("resume_token") == "tok-live3"
    assert st.resumed_session is False
    sid = _Client.made[0].session_id
    assert sid != "live3" and _binds(sent) == [(sid, 42, False)]
    rec = json.loads(ses.read_text())
    assert rec["session_id"] == sid and rec["resumed"] is False
    assert rec["resume_status"] == ("session_attached" if held == "attached" else "unknown_token")


@pytest.mark.parametrize("case", ["no_record", "not_a_run"])
def test_with_nothing_to_take_over_the_handshake_is_unchanged(seat, monkeypatch, case):
    """Row 3 (NON-DISCRIMINATING control: green on 5718f403 too). The first
    server of a run has no record; a server outside `ck3agent run`
    (CK3_RELEASE_ON_EXIT unset, e.g. an experiment driver's or an interactive
    one) never reads one. Either way no token is presented."""
    ses, sent, _ = seat
    if case == "not_a_run":
        ses.write_text(json.dumps({"session_id": "old7", "resume_token": "tok-old7"}))
        _Client.detached["tok-old7"] = "old7"
        monkeypatch.delenv(mcp_server.RELEASE_ON_EXIT_ENV)
    st = _start_server()
    assert "resume_token" not in _Client.made[0].handshake_kwargs
    assert getattr(st, "resumed_session", False) is False      # the field is new in 2026.12.5
    assert _binds(sent) == [(_Client.made[0].session_id, 42, False)]


@pytest.mark.parametrize("resumed", [True, False])
def test_a_taken_over_session_keeps_its_primary_first(seat, monkeypatch, resumed):
    """Row 4. The carried list is in id order ("5,9"); the old session's primary
    is 9. On a resumed session the first pre-bind (a switch, no `add`) is 9, so
    the switch does not drop the primary (session.cpp swap_character moves the
    claim off `from`). A fresh session keeps the declared order and reads
    nothing. Red on 5718f403 for resumed=True: 5 first, no get_bridge_state."""
    ses, sent, primary = seat
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, "5,9")
    if resumed:
        ses.write_text(json.dumps({"session_id": "old7", "resume_token": "tok-old7"}))
        _Client.detached["tok-old7"] = "old7"
        primary["old7"] = 9
    _start_server()
    sid = _Client.made[0].session_id
    want = [(sid, 9, False), (sid, 5, True)] if resumed else [(sid, 5, False), (sid, 9, True)]
    assert _binds(sent) == want
    first_bind = next(i for i, (_, a, _p) in enumerate(sent) if a == "set_controlled_character")
    reads = [a for _, a, _p in sent[:first_bind] if a == "get_bridge_state"]
    assert reads == (["get_bridge_state"] if resumed else [])
