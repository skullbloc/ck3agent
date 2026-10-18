"""ck-p3vd: independent reads must cost ONE flush span, not one each.

The agent paid a flush period per read — observe_v2(depth=full) fans out to ~10
reads awaited in turn, ~100s for one observation, which was the whole graded
episode. A flush serves its entire buffer in one span, so reads issued together
cost one period however many there are (measured live: 20.01s sequential vs
9.83s issued together).

The load-bearing properties are not "it is faster" — a unit test cannot see a
flush. They are: ONE scope rather than N (so the restores cannot race), polarity
preserved PER READ (so a fast ANY read is not dragged into the paused span), and
results returned in REQUEST ORDER (so nothing is silently transposed).
"""
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import mcp_server  # noqa: E402

P = mcp_server.BridgeGateway.Polarity


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


def _gw(polarities: dict):
    """A gateway whose polarity table we control, recording scope entries."""
    gw = mcp_server.BridgeGateway.__new__(mcp_server.BridgeGateway)
    gw.state = mcp_server.BridgeState()
    gw._bridge_polarity = dict(polarities)
    gw._polarity_warned = set()
    gw._strict_lock = asyncio.Lock()
    gw.scopes = []
    gw.issued = []

    from contextlib import asynccontextmanager

    @asynccontextmanager
    async def _scope(polarity):
        gw.scopes.append(polarity)
        yield {"recorded": {}}

    async def _call(action, params):
        gw.issued.append(("call", action))
        return {"action": action}

    async def _inline(action, params):
        gw.issued.append(("inline", action))
        return {"action": action}

    gw._scoped_pause = _scope
    gw._call = _call
    gw.read_inline = _inline
    return gw


def test_N_PAUSED_reads_open_exactly_ONE_scope():
    """THE CORRECTNESS PROPERTY. N per-read scopes would each sample was_paused
    and race their restores. In pipeline mode that race is unreachable because
    the flips are refused (ck-tygb) — but correct-because-a-refusal-saves-you is
    a coincidence with a timer on it, and in legacy the verbs are not refused."""
    gw = _gw({"a": P.PAUSED, "b": P.PAUSED, "c": P.PAUSED})
    out = _run(gw.read_many([("a", {}), ("b", {}), ("c", {})]))
    assert gw.scopes == [P.PAUSED], f"expected ONE scope, got {gw.scopes}"
    assert [r["action"] for r in out] == ["a", "b", "c"]


def test_an_ANY_read_is_NOT_dragged_into_the_paused_span():
    """ck-zgh4 measured query_realm at 0.22s alone and 9.83s gathered with slow
    ones. Sweeping ANY reads into the scope buys nothing and costs them a flush,
    so they are issued outside it and keep their inline fast path."""
    gw = _gw({"fast": P.ANY, "slow": P.PAUSED})
    _run(gw.read_many([("fast", {}), ("slow", {})]))
    assert ("call", "fast") in gw.issued, "the ANY read went through the scope"
    assert ("inline", "slow") in gw.issued
    # and it was issued BEFORE the scope opened
    assert gw.issued.index(("call", "fast")) < gw.issued.index(("inline", "slow"))


def test_NO_paused_reads_means_NO_scope_AT_ALL():
    """A batch of only ANY reads must not open a paused span for nothing."""
    gw = _gw({"x": P.ANY, "y": P.ANY})
    _run(gw.read_many([("x", {}), ("y", {})]))
    assert gw.scopes == [], f"opened a scope with nothing to pause for: {gw.scopes}"


def test_UNPAUSED_TICK_reads_are_NOT_batched_or_RECLASSIFIED():
    """They hold the single-flight lock and are serial by necessity —
    unpaused_window is their batching story. Silently sweeping one into a paused
    batch would be a polarity change wearing a performance fix's clothes."""
    gw = _gw({"deferred": P.UNPAUSED_TICK, "plain": P.PAUSED})
    _run(gw.read_many([("deferred", {}), ("plain", {})]))
    assert ("call", "deferred") in gw.issued
    assert gw.scopes == [P.PAUSED], "the deferred read must not add a scope"


def test_results_come_back_in_REQUEST_ORDER_across_polarities():
    """THE TRANSPOSITION GUARD. The batch is split by polarity and reassembled;
    a reassembly bug would pair one read's reply with another's slot — silently,
    and every downstream isinstance() check would still pass. ck-iu11 was
    exactly this class of defect."""
    gw = _gw({"p1": P.PAUSED, "a1": P.ANY, "p2": P.PAUSED, "a2": P.ANY})
    out = _run(gw.read_many([("p1", {}), ("a1", {}), ("p2", {}), ("a2", {})]))
    assert [r["action"] for r in out] == ["p1", "a1", "p2", "a2"]


def test_one_failing_read_does_not_take_the_BATCH_down():
    """A read that raises is recorded as an empty result, exactly as read_inline
    already does for its own timeouts. Callers guard with isinstance(x, dict)
    and treat {} as absent, so a batch must degrade the way the per-read path
    does rather than propagating."""
    gw = _gw({"ok": P.PAUSED, "bad": P.PAUSED})

    async def _inline(action, params):
        if action == "bad":
            raise ConnectionError("gone")
        return {"action": action}

    gw.read_inline = _inline
    out = _run(gw.read_many([("ok", {}), ("bad", {})]))
    assert out[0] == {"action": "ok"}
    assert out[1] == {}, "a failed read must be an absent result, not an exception"


def test_an_empty_batch_touches_nothing():
    gw = _gw({})
    assert _run(gw.read_many([])) == []
    assert gw.scopes == [] and gw.issued == []
