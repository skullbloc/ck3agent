"""ck-p3vd: independent reads must cost ONE flush span, not one each.

The agent paid a flush period per read — observe_v2(depth=full) fans out to ~10
reads awaited in turn, ~100s for one observation, which was the whole graded
episode. A flush serves its entire buffer in one span, so reads issued together
cost one period however many there are (measured live: 20.01s sequential vs
9.83s issued together).

The load-bearing properties are not "it is faster" — a unit test cannot see a
flush. They are: the reads are ISSUED TOGETHER (all in flight before any
returns), results come back in REQUEST ORDER (so nothing is silently
transposed), and one failure does not take the batch down. Since L13 there is
no scope and no partition: each read keeps its own path.
"""
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import mcp_server  # noqa: E402


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


def _gw(delays: dict = None, fail: set = frozenset()):
    """A gateway whose `read` we control, recording issue and completion order."""
    gw = mcp_server.BridgeGateway.__new__(mcp_server.BridgeGateway)
    gw.state = mcp_server.BridgeState()
    gw._strict_lock = asyncio.Lock()
    gw.events = []

    async def _read(action, params):
        gw.events.append(("issue", action))
        await asyncio.sleep((delays or {}).get(action, 0))
        if action in fail:
            raise ConnectionError("gone")
        gw.events.append(("done", action))
        return {"action": action}

    gw.read = _read
    return gw


def test_the_reads_are_issued_together():
    """Every read is in flight before the first one returns: the concurrency the
    ck-zgh4 measurement bought."""
    gw = _gw({"a": 0.05, "b": 0.05, "c": 0.05})
    _run(gw.read_many([("a", {}), ("b", {}), ("c", {})]))
    issues = [i for i, e in enumerate(gw.events) if e[0] == "issue"]
    first_done = min(i for i, e in enumerate(gw.events) if e[0] == "done")
    assert len(issues) == 3 and max(issues) < first_done, gw.events


def test_results_come_back_in_REQUEST_ORDER():
    """THE TRANSPOSITION GUARD. Reads that finish out of order must still be
    paired with their own slot — silently transposed replies would pass every
    downstream isinstance() check. ck-iu11 was exactly this class of defect."""
    gw = _gw({"p1": 0.04, "a1": 0.0, "p2": 0.02, "a2": 0.01})
    out = _run(gw.read_many([("p1", {}), ("a1", {}), ("p2", {}), ("a2", {})]))
    assert [r["action"] for r in out] == ["p1", "a1", "p2", "a2"]


def test_one_failing_read_does_not_take_the_BATCH_down():
    """A read that raises is recorded as an empty result. Callers guard with
    isinstance(x, dict) and treat {} as absent, so a batch must degrade the way
    the per-read path does rather than propagating."""
    gw = _gw(fail={"bad"})
    out = _run(gw.read_many([("ok", {}), ("bad", {})]))
    assert out[0] == {"action": "ok"}
    assert out[1] == {}, "a failed read must be an absent result, not an exception"


def test_an_empty_batch_touches_nothing():
    gw = _gw()
    assert _run(gw.read_many([])) == []
    assert gw.events == []
