"""rc2 B1: BridgeClient.connect() runs the socket-owner and peer-uid checks.

A foreign uid needs root to produce, so the uid is forged: os.lstat reports a
socket owned by another uid, or SO_PEERCRED reports a foreign peer. Each row
fails if its check is deleted from bridge_client.py:connect():

  * owner check gone -> connect() is dialled on the foreign socket;
  * peer check gone  -> connect() returns True with a foreign peer.

socket.socket.connect is replaced, so no real socket is ever dialled.
"""
from __future__ import annotations

import os
import socket
import stat
import struct

import pytest

try:
    from ck3agent.server import bridge_client as bc
    from ck3agent.server import instance as inst
except ImportError:                                   # bare: cwd is ck3agent/server
    import bridge_client as bc                        # type: ignore
    import instance as inst                           # type: ignore

PATH = "/nonexistent/ck3-test/agent.sock"


def _fake_stat(uid: int) -> os.stat_result:
    # (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime)
    return os.stat_result((stat.S_IFSOCK | 0o600, 1, 1, 1, uid, 0, 0, 0, 0, 0))


@pytest.fixture
def user_layout(monkeypatch):
    monkeypatch.delenv("CK3_INSTANCE_LAYOUT", raising=False)


@pytest.fixture
def dial(monkeypatch):
    """Record every connect(); succeed without touching the filesystem."""
    calls: list = []
    monkeypatch.setattr(socket.socket, "connect", lambda self, addr: calls.append(addr))
    return calls


def _forge_lstat(monkeypatch, uid: int):
    real = os.lstat

    def lstat(p, *a, **k):
        return _fake_stat(uid) if str(p) == PATH else real(p, *a, **k)

    monkeypatch.setattr(os, "lstat", lstat)


def _forge_peer(monkeypatch, uid: int):
    real = socket.socket.getsockopt

    def getsockopt(self, level, opt, *a):
        if level == socket.SOL_SOCKET and opt == socket.SO_PEERCRED:
            return struct.pack("3i", 4242, uid, uid)
        return real(self, level, opt, *a)

    monkeypatch.setattr(socket.socket, "getsockopt", getsockopt)


def test_a_socket_owned_by_another_uid_is_never_dialled(monkeypatch, user_layout, dial):
    _forge_lstat(monkeypatch, os.geteuid() + 1)
    with pytest.raises(inst.UntrustedPath):
        bc._check_socket_owner(PATH)
    c = bc.BridgeClient(PATH)
    assert c.connect(timeout=0.2) is False
    assert dial == [], "connect() dialled a socket owned by another uid"
    assert c.sock is None


def test_a_foreign_peer_is_refused_after_connect(monkeypatch, user_layout, dial):
    _forge_lstat(monkeypatch, os.geteuid())
    _forge_peer(monkeypatch, os.geteuid() + 1)
    c = bc.BridgeClient(PATH)
    assert c.connect(timeout=0.2) is False, "connected to a peer with another uid"
    assert dial == [PATH]
    assert c.sock is None


def test_control_our_socket_and_our_peer_connect(monkeypatch, user_layout, dial):
    """Control: the same forgery with OUR uid connects, so the two refusals
    above come from the uid and not from the harness."""
    _forge_lstat(monkeypatch, os.geteuid())
    _forge_peer(monkeypatch, os.geteuid())
    c = bc.BridgeClient(PATH)
    assert c.connect(timeout=0.2) is True
    assert dial == [PATH]
    c.close()


def test_legacy_layout_skips_both_checks(monkeypatch, dial):
    """Legacy is the documented opt-out (SECURITY.md): any uid is dialled."""
    monkeypatch.setenv("CK3_INSTANCE_LAYOUT", "legacy")
    _forge_lstat(monkeypatch, os.geteuid() + 1)
    _forge_peer(monkeypatch, os.geteuid() + 1)
    c = bc.BridgeClient(PATH)
    assert c.connect(timeout=0.2) is True
    c.close()


def test_the_checks_compare_the_effective_uid(monkeypatch, user_layout, dial):
    """The bridge compares against geteuid(); so does the client. With uid and
    euid forged apart, a socket owned by the euid is accepted."""
    euid = os.geteuid()
    monkeypatch.setattr(os, "getuid", lambda: euid + 7)
    _forge_lstat(monkeypatch, euid)
    _forge_peer(monkeypatch, euid)
    c = bc.BridgeClient(PATH)
    assert c.connect(timeout=0.2) is True
    c.close()
