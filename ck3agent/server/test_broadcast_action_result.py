"""ck-9rto: another session's reply must never reach this model.

THE HAZARD (ck-st70, measured by ck-zgh4, corroborated by ck-760i). The bridge
BROADCASTS a queue-bound reply to every client slot. A receive loop routes by
`request_id` first and `continue`s, so a frame that reaches the message-type
dispatch below it is UNCORRELATED BY CONSTRUCTION: it carries no request_id,
or one belonging to somebody else.

The bridge has never emitted a frame of type `action_result`. Cleanup 2026.11
deleted the protocol entry that described one (`ipc_protocol.json`
`action_response` and its tick_flow step) together with both clients' branches
for it. On the MCP server's listener (the B path, the runtime the arms run,
decision G6) such a frame now reaches the terminal else: it is counted in
`unhandled_frames`, delivered to nobody, and stored nowhere. The A-path rows
live in test_pipeline_ack_correlation.py, which imports agent.py.

Host-only: no bridge, no game, no spend.
"""
import asyncio
import dataclasses
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mcp_server as mcp                                        # noqa: E402
from test_unwitnessable_frame import _Bridge, _run              # noqa: E402

STRAY = {"type": "action_result", "request_id": "someone-elses",
         "success": True, "action": "declare_war", "message": "sent"}


def test_a_STRAY_action_result_is_counted_and_stored_nowhere():
    """Behaviour, not source text: drive the real listener with the frame."""
    st = _run([STRAY])
    assert st.unhandled_frames == {"action_result": 1}
    fields = {f.name for f in dataclasses.fields(mcp.BridgeState)}
    assert not any("action_result" in f for f in fields), (
        f"BridgeState grew a field for action_result: {sorted(fields)}")
    assert st.latest_observation == {}


def test_CONTROL_a_frame_carrying_OUR_request_id_still_reaches_its_waiter():
    """The row above is discriminating only if correlation still runs first: a
    frame with this session's request_id is delivered to the future, whatever
    its type, and is not counted as unhandled."""
    async def go():
        st = mcp.BridgeState()
        st._client = _Bridge([dict(STRAY, request_id="ours")])
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
    assert fut.done() and fut.result()["request_id"] == "ours"
    assert st.unhandled_frames == {}
