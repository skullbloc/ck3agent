"""ck-3esm — `BridgeClient.recv`'s timeout is a DEADLINE, not a per-syscall budget.

THE BUG THIS PINS, measured on a real run rather than imagined. In the arm-C
smoke (ep-20260921T041851Z) the driver hung on its 10th `query_war_stance`,
part-way through its FIRST sampling pass, and stayed there ~4 minutes until the
operator killed it. Nothing blocked in the kernel and nothing raised: `recv`
set the socket timeout ONCE above its loop and `continue`d on every keepalive
ping WITHOUT RETURNING, so each `sock.recv` was handed the full timeout again
and the caller never regained control to check its own absolute deadline.

The bridge pings every open slot about every 5 s and `HarnessClient.call` asks
for `min(remaining, 5.0)` — so a ping landing just inside each window resets the
wait indefinitely. The consequence was not a slow call: the poll loop ran
exactly ONE iteration, `last_date` never advanced, and the episode's horizon
could never fire, turning a four-game-year run into an unbounded spend.

The ping-skip is correct. The defect is that skipping cost NOTHING.
"""
import json
import socket
import time

import pytest

from ck3agent.server.bridge_client import BridgeClient


PING = json.dumps({"type": "ping"}).encode() + b"\n"


class _PingFlood:
    """A socket that only ever delivers keepalive pings, like an idle bridge.

    Raises after `cap` deliveries so a REGRESSION FAILS THE SUITE INSTEAD OF
    HANGING IT — an unbounded loop is otherwise indistinguishable from a slow
    machine, and a test that hangs teaches nobody anything.
    """

    def __init__(self, *, interval: float = 0.01, cap: int = 100) -> None:
        self.interval, self.cap = interval, cap
        self.timeouts: list[float] = []
        self.calls = 0

    def settimeout(self, t):          # noqa: D102
        self.timeouts.append(t)

    def recv(self, _n):               # noqa: D102
        self.calls += 1
        if self.calls > self.cap:
            raise RuntimeError(
                f"recv() consumed {self.calls} pings without honouring its "
                "deadline — the timeout is being re-armed per syscall (ck-3esm)")
        time.sleep(self.interval)
        return PING


def _client(sock):
    c = BridgeClient.__new__(BridgeClient)   # no connect(); we supply the socket
    c.sock, c._buf = sock, b""
    return c


def test_a_ping_flood_does_not_outlast_the_deadline():
    """THE REGRESSION TEST. Pre-fix this raises from the cap; post-fix returns None."""
    sock = _PingFlood(interval=0.01, cap=100)
    started = time.monotonic()
    got = _client(sock).recv(timeout=0.2)
    elapsed = time.monotonic() - started

    assert got is None, "a flood of pings must expire the deadline, not return a ping"
    assert elapsed < 1.0, (
        f"recv() took {elapsed:.2f}s against a 0.2s timeout — the deadline is "
        "not bounding the loop")


def test_the_socket_timeout_SHRINKS_across_a_ping_flood():
    """The mechanism, not just the outcome: each wait asks for what is LEFT.

    Pre-fix there is exactly ONE settimeout call, carrying the full timeout.
    Post-fix there is one per syscall and they DECREASE — which is the actual
    difference between a deadline and a budget handed out afresh each time.
    """
    sock = _PingFlood(interval=0.01, cap=100)
    _client(sock).recv(timeout=0.2)

    assert len(sock.timeouts) > 1, (
        "the socket timeout was set once for the whole loop — that is a "
        "per-syscall budget, not a deadline")
    assert sock.timeouts == sorted(sock.timeouts, reverse=True), (
        f"remaining time must be non-increasing, got {sock.timeouts[:5]}")
    assert sock.timeouts[-1] <= sock.timeouts[0], "the last wait must not exceed the first"


def test_a_real_message_still_returns_immediately():
    """THE NON-DISCRIMINATING CONTROL, labelled as such.

    This passes both before and after the fix. It is here so a change that
    bounds the loop by BREAKING ordinary delivery cannot hide behind the two
    tests above.
    """
    payload = json.dumps({"type": "response", "request_id": "abc"}).encode() + b"\n"

    class _OneMessage(_PingFlood):
        def recv(self, _n):
            self.calls += 1
            return PING if self.calls == 1 else payload

    got = _client(_OneMessage()).recv(timeout=5.0)
    assert got == {"type": "response", "request_id": "abc"}


def test_a_socket_timeout_still_returns_None():
    """Also non-discriminating: the ordinary quiet-socket path is unchanged."""

    class _Silent(_PingFlood):
        def recv(self, _n):
            raise socket.timeout()

    assert _client(_Silent()).recv(timeout=0.05) is None
