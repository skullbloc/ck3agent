"""L20-B item 6 (smoke16/17 trip-over, STILL in smoke17): `ck3agent serve`
printed the whole handshake_ack to stderr at every start.

THE SMOKE. smoke17 probe-idle.out lines 2-66: one log record,
"[INFO] bridge_client: Handshake OK: {" followed by 64 lines of indented JSON
(bridge_client.py BridgeClient.handshake: log.info("Handshake OK: %s",
json.dumps(resp, indent=2))). The start line now names bridge_version,
build_git_sha, build_id_status and session_id, and says where the whole ack is
kept: <telemetry dir>/handshake_ack.json (the instance's, beside the action log).
"""

import json
import logging
import os
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bridge_client as bc  # noqa: E402
from test_handshake_v2 import FakeBridgeSocket  # noqa: E402

#: the summary fields as probe-idle.out shows them (lines 4, 44, 51, 58)
SMOKE17 = {"bridge_version": "2026.12.0", "build_id_status": "match",
           "build_git_sha": "7f3bf3f29104", "session_id": "sess_000000004b6c821b_0000001c"}


class AckBridge(FakeBridgeSocket):
    """The test_handshake_v2 fake, its ack carrying the smoke's summary fields
    and the bulk the real ack has (the 64-line JSON was mostly these)."""

    def _handle(self, msg):
        super()._handle(msg)
        if msg.get("type") == "handshake" and self._out:
            ack = json.loads(self._out.decode().splitlines()[-1])
            ack.update(SMOKE17)
            ack.update({f"capability_{i}": True for i in range(40)})
            self._out = (json.dumps(ack) + "\n").encode()


def _client(monkeypatch, tmp_path):
    monkeypatch.setenv("CK3_TELE", str(tmp_path / "tele"))
    fake = AckBridge(session_id=SMOKE17["session_id"])
    client = bc.BridgeClient("/nonexistent/ck3-agent.sock")
    client.sock = fake  # type: ignore[assignment]
    return client


def test_the_start_line_is_one_line_naming_the_four_fields(monkeypatch, tmp_path, caplog):
    client = _client(monkeypatch, tmp_path)
    with caplog.at_level(logging.INFO, logger="bridge_client"):
        ack = client.handshake()
    assert ack["type"] == "handshake_ack"
    lines = [r.getMessage() for r in caplog.records
             if r.levelno >= logging.INFO and r.getMessage().startswith("Handshake OK")]
    assert len(lines) == 1, lines
    line = lines[0]
    assert "\n" not in line, line
    for k, v in SMOKE17.items():
        assert f"{k}={v}" in line, (k, line)
    assert len(line) < 300, len(line)


def test_the_whole_ack_is_kept_in_the_telemetry_dir(monkeypatch, tmp_path, caplog):
    client = _client(monkeypatch, tmp_path)
    with caplog.at_level(logging.INFO, logger="bridge_client"):
        ack = client.handshake()
    path = tmp_path / "tele" / bc.HANDSHAKE_ACK_FILE
    assert json.loads(path.read_text()) == {k: v for k, v in ack.items() if k != "resume_token"}
    assert str(path) in next(r.getMessage() for r in caplog.records
                             if r.getMessage().startswith("Handshake OK"))


def test_an_unwritable_dir_is_said_and_the_ack_is_still_at_debug(monkeypatch, tmp_path, caplog):
    blocker = tmp_path / "file"
    blocker.write_text("x")
    monkeypatch.setenv("CK3_TELE", str(blocker / "tele"))   # a dir under a file: unwritable
    fake = AckBridge(session_id=SMOKE17["session_id"])
    client = bc.BridgeClient("/nonexistent/ck3-agent.sock")
    client.sock = fake  # type: ignore[assignment]
    with caplog.at_level(logging.DEBUG, logger="bridge_client"):
        client.handshake()
    msgs = [r.getMessage() for r in caplog.records]
    assert any(m.startswith("Handshake OK") and "not written" in m for m in msgs), msgs
    assert any(m.startswith("handshake_ack: {") for m in msgs)


def test_a_field_the_ack_lacks_is_said_absent():
    assert "build_git_sha=absent" in bc.handshake_summary({"bridge_version": "x"}, None)


# ── L20 r3, witness item 1: the kept ack is for reading the build ────────────
# bridge_client.keep_handshake_ack wrote the WHOLE ack, the server-minted
# resume_token included (ipc_server.cpp:1784, "an authorisation over a claim
# set"), with a plain open(): umask mode, symlinks followed, the dir at the
# default mode; the DEBUG line logged the token too.

def test_the_kept_ack_has_no_resume_token_and_the_debug_line_neither(monkeypatch, tmp_path, caplog):
    client = _client(monkeypatch, tmp_path)
    with caplog.at_level(logging.DEBUG, logger="bridge_client"):
        ack = client.handshake()
    assert ack.get("resume_token"), "the fake must mint a token for this row to mean anything"
    kept = json.loads((tmp_path / "tele" / bc.HANDSHAKE_ACK_FILE).read_text())
    assert "resume_token" not in kept, kept.keys()
    assert kept["session_id"] == SMOKE17["session_id"]
    for r in caplog.records:
        assert ack["resume_token"] not in r.getMessage(), r.getMessage()


def test_the_kept_ack_is_0600_in_a_0700_dir(monkeypatch, tmp_path):
    old = os.umask(0o022)
    try:
        client = _client(monkeypatch, tmp_path)
        client.handshake()
    finally:
        os.umask(old)
    path = tmp_path / "tele" / bc.HANDSHAKE_ACK_FILE
    assert stat.S_IMODE(path.stat().st_mode) == 0o600, oct(path.stat().st_mode)
    assert stat.S_IMODE((tmp_path / "tele").stat().st_mode) == 0o700


def test_a_symlinked_tmp_is_not_followed(monkeypatch, tmp_path):
    (tmp_path / "tele").mkdir(mode=0o700)
    victim = tmp_path / "victim"
    victim.write_text("untouched\n")
    tmp = tmp_path / "tele" / f"{bc.HANDSHAKE_ACK_FILE}.{os.getpid()}.tmp"
    tmp.symlink_to(victim)
    client = _client(monkeypatch, tmp_path)
    client.handshake()
    assert victim.read_text() == "untouched\n"
    assert not (tmp_path / "tele" / bc.HANDSHAKE_ACK_FILE).exists()


def test_the_overview_resource_withholds_the_resume_token():
    """ck3://state/overview carries the ack less HANDSHAKE_ACK_WITHHELD: a resource
    body ends up in the client's transcript. Red before L20 r3 follow-up b: the
    raw handshake_info (token included) was returned."""
    import types
    import mcp_server  # noqa: E402
    ack = {"type": "handshake_ack", "bridge_version": "x", "session_id": "s1", "resume_token": "SECRET"}
    state = types.SimpleNamespace(connected=True, mock_mode=False, tick_id=3, game_date="0867.01.01", handshake_info=ack)
    body = mcp_server.overview_payload(state)
    assert "resume_token" not in json.dumps(body)
    assert body["handshake"]["session_id"] == "s1"
    assert ack["resume_token"] == "SECRET"  # the state's own copy is untouched
