"""Suite-wide fixtures for the ck3agent server unit tests.

THE DRAIN SCHEDULE IS ZERO IN TESTS. redesign.DRAIN_SCHEDULE_S is the one
definition of stage 1's waits (L13 amendment 3: the bridge runs a queued write
on its next pass; the server re-reads the verdict on this schedule, ~2.75 s in
all). Every write that goes through confirm_write / await_drain against a fake
that never answers a verdict would sleep the whole schedule for real: ~650 s
across the suite (gate l13ff1-g1). Here the schedule keeps its LENGTH (so a row
can count `1 + len(DRAIN_SCHEDULE_S)` reads) and every wait is 0.

Stage 2's world-poll interval (redesign.WORLD_POLL_INTERVAL_S, 2.5 s, the
default of both gateways' confirmed_write) is zeroed the same way, so a fake
that reports the game running pays nothing per world poll; a row that passes
its own poll_interval_s keeps it.

A test that needs the real waits says so with
`@pytest.mark.real_drain_schedule` (both are left real).
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "real_drain_schedule: run with the real DRAIN_SCHEDULE_S and WORLD_POLL_INTERVAL_S")


@pytest.fixture(autouse=True)
def _zero_drain_schedule(request, monkeypatch):
    if request.node.get_closest_marker("real_drain_schedule"):
        yield
        return
    try:
        import redesign
    except Exception:                       # noqa: BLE001  (a module that cannot import is its own red)
        yield
        return
    monkeypatch.setattr(redesign, "DRAIN_SCHEDULE_S",
                        tuple(0.0 for _ in redesign.DRAIN_SCHEDULE_S))
    monkeypatch.setattr(redesign, "WORLD_POLL_INTERVAL_S", 0.0)
    yield
