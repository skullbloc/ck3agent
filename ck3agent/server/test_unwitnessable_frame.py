"""ck-ey8p — a frame the listener does not handle must leave a TRACE.

ck-760i's trace of ep-02: `_bridge_listener`'s type dispatch had branches for
tick_start, observation, action_result, handshake_ack, notification, ping and
error — and no `result` branch and no terminal else. The ledger record is
`"type":"result"`, so one that matched no pending request_id fell through every
branch and was discarded with NO log line and NO counter.

WHY THAT IS P1 RATHER THAN UNTIDY. ep-02 holds zero ledger frames. Two very
different worlds produce that zero — none was ever emitted (the deployed .so
lacked the emitter), or they arrived and went nowhere — and this client could
not tell them apart even in principle. An instrument that cannot distinguish
"nothing happened" from "I could not see it" turns an absence into evidence.

Host-only: no bridge, no game, no spend.
"""

import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mcp_server as mcp                                        # noqa: E402


class _Bridge:
    def __init__(self, frames):
        self.frames = list(frames)

    async def recv(self, timeout=None):
        if self.frames:
            return self.frames.pop(0)
        await asyncio.sleep(0)
        return None


def _run(frames, pending=None):
    async def go():
        st = mcp.BridgeState()
        st._client = _Bridge(frames)
        if pending:
            for rid in pending:
                st.pending_queries[rid] = asyncio.get_running_loop().create_future()
        task = asyncio.ensure_future(mcp._bridge_listener(st))
        for _ in range(len(frames) * 4 + 4):
            await asyncio.sleep(0)
            if not st._client.frames:
                break
        await asyncio.sleep(0)
        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):
            pass
        return st
    return asyncio.run(go())


LEDGER = {"type": "result", "request_id": "not-ours",
          "action": "session_control", "ledger_source": "inline_management",
          "bridge_record": {"session_id": "sess_mercia"}}


class AnUnmatchedFrameIsWitnessed(unittest.TestCase):

    def test_an_unmatched_LEDGER_record_is_counted(self):
        """THE BEAD. Before the terminal else this frame left nothing behind."""
        st = _run([LEDGER])
        self.assertEqual(st.unhandled_frames.get("result"), 1)

    def test_it_is_LOUD_because_it_is_evidence_that_went_nowhere(self):
        """A count nobody reads is the same silence one level over. The ledger
        case logs at WARNING; an unknown type does not, so the loud line stays
        rare enough to be read."""
        with self.assertLogs("ck3-mcp", level="WARNING") as cm:
            _run([LEDGER])
        self.assertTrue(any("UNMATCHED LEDGER FRAME" in m for m in cm.output),
                        cm.output)

    def test_an_ordinary_unknown_type_is_counted_but_NOT_loud(self):
        """The control on the row above: if everything were loud, the warning
        would carry no information. An unknown type is forward-compatibility
        noise — counted, not shouted."""
        st = _run([{"type": "some_future_type"}])
        self.assertEqual(st.unhandled_frames.get("some_future_type"), 1)
        with self.assertLogs("ck3-mcp", level="DEBUG") as cm:
            _run([{"type": "some_future_type"}])
        self.assertFalse(any("WARNING" in m and "UNMATCHED LEDGER" in m
                             for m in cm.output), cm.output)

    def test_ABSENT_is_not_ZERO(self):
        """The dict starts EMPTY and is never pre-seeded. `unhandled_frames`
        with no `result` key means none arrived; a 0 would claim a reading
        nobody took, and this whole bead exists because an absence was read as
        a measurement."""
        st = _run([{"type": "ping"}])
        self.assertEqual(st.unhandled_frames, {})
        self.assertNotIn("result", st.unhandled_frames)

    def test_a_HANDLED_type_is_not_counted_as_unhandled(self):
        """Otherwise the counter measures traffic rather than blindness."""
        st = _run([{"type": "tick_start", "tick_id": 3, "game_date": "1066.09.15"},
                   {"type": "observation", "game_date": "1066.09.16"},
                   {"type": "ping"}])
        self.assertEqual(st.unhandled_frames, {})
        self.assertEqual(st.tick_id, 3)

    def test_a_MATCHED_ledger_frame_is_delivered_not_counted(self):
        """The other half of the distinction: a frame that DOES correlate is
        not an unhandled one. If this failed, the counter would be counting
        successful deliveries and its non-zero would mean nothing."""
        async def go():
            st = mcp.BridgeState()
            st._client = _Bridge([dict(LEDGER, request_id="ours")])
            fut = asyncio.get_running_loop().create_future()
            st.pending_queries["ours"] = fut
            task = asyncio.ensure_future(mcp._bridge_listener(st))
            for _ in range(8):
                await asyncio.sleep(0)
                if not st._client.frames:
                    break
            await asyncio.sleep(0)
            task.cancel()
            try:
                await task
            except (asyncio.CancelledError, Exception):
                pass
            return st, fut
        st, fut = asyncio.run(go())
        self.assertTrue(fut.done())
        self.assertEqual(st.unhandled_frames, {})


if __name__ == "__main__":
    unittest.main()
