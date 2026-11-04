"""_bridge against a fake NDJSON bridge on a Unix socket (tests/_fakes.py)."""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import threading
import time

import pytest

from ck3agent import __version__, cli, _bridge as br


def test_teardown_pauses_and_finds_no_ruler(bridge):
    fb = bridge()
    assert br.teardown(br.socket_path(), wait=0, sessions=["s1"]) == [("pause", True, ""),
                                                                     ("release", True, "no ruler bound")]
    assert ("handshake", "control") in fb.seen and fb.paused


def test_teardown_names_the_holder_when_release_is_refused(bridge):
    # The ruler belongs to the MCP server's session, now gone: the CLI cannot release it.
    fb = bridge()
    fb.sessions["s99"] = {"state": "detached", "client_name": "ck3agent-mcp", "mode": "control"}
    fb.owner[42] = "s99"
    steps = br.teardown(br.socket_path(), wait=0, sessions=["s99"])
    assert steps[-1] == ("release", False, "42 still held by session s99 (detached, ck3agent-mcp): "
                                           "not_owned_by_session")
    assert ("release_character", {"character_id": 42}) in fb.seen and fb.owner == {42: "s99"}


def test_teardown_waits_for_the_owner_to_release(bridge):
    fb = bridge()
    fb.owner[42] = "s99"
    threading.Timer(1.5, lambda: fb.owner.clear()).start()
    assert br.teardown(br.socket_path(), wait=10, sessions=["s99"])[-1] == ("release", True, "no ruler bound")
    assert ("release_character", {"character_id": 42}) not in fb.seen


def test_teardown_leaves_another_runs_ruler_alone_and_names_it(bridge):
    """L20-B (smoke17: consolidate's teardown warned "13902 still held by session
    ...14: not_owned_by_session", expand's ruler). Two sessions hold one ruler
    each; the teardown of the run whose session is s1 releases nothing of s2's,
    asks the bridge for nothing of s2's, and does not warn."""
    fb = bridge()
    fb.sessions["s1"] = {"state": "detached", "client_name": "ck3agent-mcp", "mode": "control"}
    fb.sessions["s2"] = {"state": "attached", "client_name": "ck3agent", "mode": "control"}
    fb.owner[13902] = "s2"                       # the other run's ruler
    steps = br.teardown(br.socket_path(), wait=0, sessions=["s1"])
    assert steps[-1][0] == "release" and steps[-1][1] is True, steps
    assert "13902 held by session s2 (attached, ck3agent), not this run's" in steps[-1][2]
    assert ("release_character", {"character_id": 13902}) not in fb.seen
    assert fb.owner == {13902: "s2"}


def test_teardown_releases_its_own_ruler_beside_another_runs(bridge):
    fb = bridge()
    fb.sessions["s1"] = {"state": "detached", "client_name": "ck3agent-mcp", "mode": "control"}
    fb.sessions["s2"] = {"state": "attached", "client_name": "ck3agent", "mode": "control"}
    fb.owner[12145] = "s1"
    fb.owner[13902] = "s2"
    steps = br.teardown(br.socket_path(), wait=0, sessions=["s1"])
    assert ("release_character", {"character_id": 12145}) in fb.seen
    assert ("release_character", {"character_id": 13902}) not in fb.seen


def test_teardown_without_a_session_record_releases_nothing_and_is_not_clean(bridge):
    """L20 r3, witness item 2: with no session record (`sessions` empty) every
    claim read as another run's, and the teardown returned ("release", True,
    "no ruler of this run bound"). The record is the only attribution: nothing
    is released, the step is not clean, and the claims seen are unattributed."""
    fb = bridge()
    fb.sessions["s99"] = {"state": "detached", "client_name": "ck3agent-mcp", "mode": "control"}
    fb.owner[42] = "s99"
    for sessions in ((), [None], [""]):
        steps = br.teardown(br.socket_path(), wait=0, sessions=sessions)
        what, ok, detail = steps[-1]
        assert what == "release" and ok is False, steps
        assert detail.startswith("session_record_missing:"), detail
        assert "unattributed: 42 held by session s99 (detached, ck3agent-mcp)" in detail, detail
    assert not any(v == "release_character" for v, _ in fb.seen), fb.seen
    assert fb.owner == {42: "s99"}


def test_teardown_without_a_session_record_and_no_claims_is_still_not_clean(bridge):
    fb = bridge()
    what, ok, detail = br.teardown(br.socket_path(), wait=0)[-1]
    assert (what, ok) == ("release", False)
    assert detail == ("session_record_missing: no session record of this run was read, so "
                      "nothing was released; no ruler is bound in the game")
    assert fb.paused


def test_teardown_on_a_dead_bridge_reports_and_does_not_raise():
    assert br.teardown(br.socket_path()) == [("bridge", False, "socket not listening")]
    assert br.game_date(br.socket_path()) is None


def test_link_holds_one_session_for_many_calls(bridge):
    fb = bridge()
    link = br.Link(br.socket_path(), mode="control")
    for _ in range(5):
        assert br._date(br._tick(link.call("get_tick_info"))) == "900.1.1"
    link.close()
    assert fb.handshakes() == 1


def test_link_retries_once_on_a_stale_session(bridge):
    fb = bridge()
    link = br.Link(br.socket_path(), mode="control")
    assert link.call("get_tick_info")
    link.bc.close()                                     # the bridge dropped us while idle
    assert br._date(br._tick(link.call("get_tick_info"))) == "900.1.1"
    link.close()
    assert fb.handshakes() == 2 and link.opened == 2


def test_release_session_resumes_the_servers_session_once_it_lets_go(bridge):
    # pin_claims: the bridge keeps a closed seat resumable with its claims, as
    # experiment mode does. The last claim of a control session is refused.
    fb = bridge(pin_claims=True)
    sock = br.socket_path()
    server = br.session(sock, "ck3agent", mode="control")
    bc = server.__enter__()
    br.call(bc, "set_controlled_character", {"character_id": 42})
    br.call(bc, "set_controlled_character", {"character_id": 43, "add": True})   # add: control switches
    sid = bc.session_id
    rec = {"session_id": sid, "resume_token": bc.handshake_ack["resume_token"]}
    link = br.Link(sock, mode="control")
    try:
        # Still attached: resuming would mint a fresh session that cannot release 42.
        r = br.release_session(sock, rec, link, wait=0.5)
        assert r["resumed"] is False and r["resume_status"] == "session_attached"
        assert r["error"] == "resume refused: session_attached" and fb.owner == {42: sid, 43: sid}
        # Let go after a moment: the wait sees it detach, then the resume releases
        # 42 and is refused 43, the session's last.
        threading.Timer(0.8, lambda: server.__exit__(None, None, None)).start()
        r = br.release_session(sock, rec, link, wait=10)
        assert r["resumed"] is True and r["claims"] == [42, 43] and r["released"] == [42]
        assert r["refused"] == {"43": "last_claim_of_control_session"} and r["error"] is None
        assert r["waited_s"] > 0.5 and fb.owner == {43: sid}
        # Again: the last claim is still refused, not lost.
        r = br.release_session(sock, rec, link, wait=1)
        assert r["claims"] == [43] and r["refused"] == {"43": "last_claim_of_control_session"}
    finally:
        link.close()


def test_release_session_finds_a_free_play_session_gone_with_its_claims(bridge):
    # Free play: a closed session is destroyed, and its claims with it.
    fb = bridge()
    sock = br.socket_path()
    with br.session(sock, "ck3agent", mode="control") as bc:
        br.call(bc, "set_controlled_character", {"character_id": 42})
        rec = {"session_id": bc.session_id, "resume_token": bc.handshake_ack["resume_token"]}
    link = br.Link(sock, mode="control")
    try:
        deadline = time.monotonic() + 5
        while fb.owner and time.monotonic() < deadline:     # the close lands on the bridge's thread
            time.sleep(0.05)
        r = br.release_session(sock, rec, link, wait=1)
        assert r["resume_status"] == "session_gone" and r["claims"] == [] and r["resumed"] is False
        assert r["error"] is None and fb.owner == {} and fb.resumes == []
    finally:
        link.close()


def test_release_session_on_a_dead_bridge_does_not_raise():
    r = br.release_session(br.socket_path(), {"session_id": "s1", "resume_token": "t"}, br.Link(br.socket_path()))
    assert r["error"].startswith("bridge:") and r["resumed"] is False


def test_peer_pid_is_the_process_serving_the_socket(bridge):
    bridge()
    assert br.peer_pid(br.socket_path()) == os.getpid()


def test_doctor_flags_a_bridge_version_mismatch(bridge, capsys):
    bridge(ack={"session_state": "active", "bridge_version": "1999.1.0"})
    assert br.handshake_report(br.socket_path()) is False
    assert "bridge 1999.1.0" in capsys.readouterr().out


@pytest.mark.parametrize("ack", [{"bridge_version": __version__}, {}])
def test_doctor_accepts_a_matching_or_absent_bridge_version(bridge, ack):
    bridge(ack={"session_state": "dormant", **ack})
    assert br.handshake_report(br.socket_path()) is True


# The two reply shapes the bridge sends, keyed as bridge/src/ipc_server.cpp emits
# them: `bridge_version` from handshake_provenance_fields (CK3_BRIDGE_VERSION,
# bridge/CMakeLists.txt) and build_git_* from the handshake_ack snprintf
# (bridge_provenance::, bridge/cmake/gen_provenance.cmake).
_PUBLIC_ACK = {"session_state": "active", "bridge_version": __version__,
               "build_git_sha": "unknown", "build_git_branch": "unknown", "build_git_dirty": "unknown"}
_RESEARCH_ACK = {"session_state": "active", "bridge_version": br.RESEARCH_BRIDGE_VERSION,
                 "build_git_sha": "627af763abcd", "build_git_branch": "release-prep-2026.10",
                 "build_git_dirty": "clean"}


def test_doctor_names_a_research_bridge_by_its_commit(bridge, capsys):
    bridge(ack=_RESEARCH_ACK)
    assert br.handshake_report(br.socket_path()) is True
    out = capsys.readouterr().out
    assert f"research build {br.RESEARCH_BRIDGE_VERSION}" in out
    assert "git 627af763abcd" in out
    assert "reinstall" not in out


def test_doctor_passes_a_public_bridge_of_its_own_version(bridge, capsys):
    bridge(ack=_PUBLIC_ACK)
    assert br.handshake_report(br.socket_path()) is True
    assert "research build" not in capsys.readouterr().out


def test_doctor_mismatch_names_both_numbers(bridge, capsys):
    bridge(ack={**_PUBLIC_ACK, "bridge_version": "2026.9.0"})
    assert br.handshake_report(br.socket_path()) is False
    out = capsys.readouterr().out
    assert "bridge 2026.9.0" in out and f"ck3agent {__version__}" in out


def test_research_bridge_version_mirrors_the_cmake_default():
    """One definition: the CMake default is the value; the CLI's constant mirrors it."""
    import re
    from pathlib import Path
    cm = (Path(__file__).resolve().parents[1] / "bridge/CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r'^set\(CK3_BRIDGE_VERSION "([^"]+)"\)', cm, re.M)
    assert m, "bridge/CMakeLists.txt: no set(CK3_BRIDGE_VERSION ...) default"
    assert m.group(1) == br.RESEARCH_BRIDGE_VERSION


def test_wait_for_active_returns_0_when_active(bridge):
    class Proc:
        def poll(self):
            return None

    bridge(replies={"get_tick_info": {"session_state": "active", "game_date": "867.1.1"}})
    assert br.wait_for_active(br.socket_path(), time.time() + 5, Proc()) == 0


class FakeProc:
    """poll/wait/send_signal; exits on the signals in `dies_on`."""

    def __init__(self, dies_on=(signal.SIGINT,)):
        self.rc, self.dies_on, self.sent = None, dies_on, []

    def poll(self):
        return self.rc

    def send_signal(self, s):
        self.sent.append(s)
        if s in self.dies_on:
            self.rc = -s

    def wait(self, t=None):
        if self.rc is None:
            raise subprocess.TimeoutExpired("claude", t)
        return self.rc


def test_watch_interrupts_then_pauses_at_the_until_date(bridge):
    fb = bridge()
    fb.game_date = "900.1.2"
    p = FakeProc()
    w = cli._Watch(p, br.socket_path(), (900, 1, 1), None)
    w.start()
    w.join(10)
    assert w.reason == "until_date" and w.how == "SIGINT" and p.sent == [signal.SIGINT]
    assert fb.paused and fb.handshakes() == 1


def test_watch_escalates_when_claude_ignores_sigint(bridge, monkeypatch):
    bridge()
    monkeypatch.setattr(cli, "STOP_GRACE", 0.2)
    p = FakeProc(dies_on=(signal.SIGTERM,))
    w = cli._Watch(p, br.socket_path(), None, 1)
    w.start()
    w.join(10)
    assert w.reason == "wall_seconds" and w.how == "SIGTERM"
    assert p.sent == [signal.SIGINT, signal.SIGTERM]


def test_watch_stops_a_real_process_that_never_exits(bridge, monkeypatch):
    bridge()
    monkeypatch.setattr(cli, "STOP_GRACE", 0.5)
    monkeypatch.setattr(cli, "TERM_GRACE", 0.5)
    code = "import signal,time; signal.signal(signal.SIGINT, signal.SIG_IGN); time.sleep(600)"
    proc = subprocess.Popen([sys.executable, "-c", code])
    try:
        w = cli._Watch(proc, br.socket_path(), None, 1)
        t0 = time.monotonic()
        w.start()
        w.join(15)
        assert w.reason == "wall_seconds" and w.how == "SIGTERM" and proc.poll() == -signal.SIGTERM
        assert time.monotonic() - t0 < 10
    finally:
        proc.kill()
