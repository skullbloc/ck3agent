"""ck-2r46 — the REGISTERED board tools must round-trip through a REAL gateway.

The deploy-40 arm-B smoke delivered the tools for the first time and every
board call failed inside the seat's MCP server with

    'BridgeGateway' object has no attribute 'call'

`board_post`/`board_read` called `_get_gateway().call(...)`; the gateway's
public surface is read / read_unpaused / write / read_inline / read_many /
confirmed_write, with `_call` private. The tools had never been registered in a
live seat before ck-2r46's arm fix, so the line had never executed. Six seats
called board_read 2-3 times and board_post 1-2 times; board.jsonl has 0
records.

THESE ROWS DRIVE THE REGISTERED TOOL OBJECTS through a REAL `BridgeGateway`
against a stub bridge — not the handler with a fake client. A row that called
the module function with a mock gateway would have passed on the broken tree,
because the mock would have grown whatever attribute the code asked for.
"""
from __future__ import annotations

import asyncio
import os

import pytest

os.environ.setdefault("CK3_EPISODE_ARM", "b")   # the board tools only register in arm B

import mcp_server as ms                                            # noqa: E402


class StubBridge:
    """The far end of the socket. Records what the gateway actually sent."""

    def __init__(self):
        self.sent: list[tuple[str, dict]] = []

    async def send(self, action: str, params: dict) -> dict:
        self.sent.append((action, dict(params or {})))
        if action == "board_post":
            return {"success": True, "data": {"seq": 1, "flush_id": 7}}
        if action == "board_read":
            return {"success": True, "data": {"posts": [], "next_seq": 1}}
        return {"success": True, "data": {}}


def _wire(monkeypatch):
    """A REAL BridgeGateway over a stub bridge, wired where the tools look."""
    state = ms.BridgeState()
    state.connected = True
    state.mock_mode = False
    stub = StubBridge()
    gw = ms.BridgeGateway(state)          # the real class, not a double

    async def _fake_call(self, action, params, *, write=False, **kw):
        # Stand in for the SOCKET, one layer below the method under test, so
        # read()/write() — the thing the fix picks — still execute for real.
        return await stub.send(action, params)

    monkeypatch.setattr(ms.BridgeGateway, "_call", _fake_call, raising=True)
    monkeypatch.setattr(ms, "_get_state", lambda: state, raising=True)
    monkeypatch.setattr(ms, "_get_gateway", lambda: gw, raising=True)

    async def _ok(_state):
        return None
    monkeypatch.setattr(ms, "_ensure_connected", _ok, raising=True)
    return stub, gw


def _registered(name):
    """The tool object the SERVER registered, not the module attribute.

    e2's requirement, and it matters: `mcp.add_tool(board_post)` is what a seat
    actually calls. Falling back to the module function would test a different
    object from the one the CLI invokes.
    """
    tools = asyncio.run(ms.mcp.list_tools())
    names = [getattr(t, "name", str(t)) for t in tools]
    assert name in names, (
        f"{name} is not REGISTERED (registered: {names}). The board tools "
        "register only when the arm says so — this row needs CK3_EPISODE_ARM=b.")
    return getattr(ms, name)


def test_board_post_round_trips_through_a_real_gateway(monkeypatch):
    stub, _ = _wire(monkeypatch)
    fn = _registered("board_post")
    out = asyncio.run(fn("my lords, Mercia moves north"))
    assert stub.sent == [("board_post", {"text": "my lords, Mercia moves north"})], stub.sent
    assert out.get("success") is True, out
    # THE SHAPE OF THE SHIPPED FAULT, asserted directly: an AttributeError here
    # surfaces as an error envelope or an exception, never a success.
    assert "has no attribute" not in str(out)


def test_board_read_round_trips_through_a_real_gateway(monkeypatch):
    stub, _ = _wire(monkeypatch)
    fn = _registered("board_read")
    out = asyncio.run(fn(since_seq=3, limit=10))
    assert stub.sent == [("board_read", {"since_seq": 3, "limit": 10})], stub.sent
    assert out.get("success") is True, out
    assert "has no attribute" not in str(out)


def test_the_tools_use_a_method_the_gateway_ACTUALLY_HAS(monkeypatch):
    """The class of defect, not the instance.

    `.call` was the only production attribute named on the gateway that the
    class does not define — swept across ck3agent/server. This row keeps it
    that way: every gateway attribute the SERVER MODULE names must exist.
    """
    import re
    src = open(ms.__file__, encoding="utf-8").read()
    i = src.find("class BridgeGateway")
    j = src.find("\nclass ", i + 10)
    body = src[i:j if j > 0 else len(src)]
    defined = set(re.findall(r"\n    (?:async )?def (\w+)", body))
    defined |= set(re.findall(r"\n        self\.(\w+)\s*=", body))
    defined |= {p for p in re.findall(r"\n    (\w+):", body)}
    used = set(re.findall(r"_get_gateway\(\)\.(\w+)", src))
    missing = sorted(u for u in used if u not in defined)
    assert not missing, (
        f"mcp_server calls {missing} on BridgeGateway and the class has no such "
        f"attribute. Defined: {sorted(defined)}")


def test_board_post_takes_the_WRITE_path_and_read_takes_the_READ_path(monkeypatch):
    """Polarity is the gateway's job, and which wrapper is chosen decides it.

    The bridge is the authority and records the reason in snapshot.cpp: board
    actions are bridge-internal, so they are served INLINE and never handed to
    the game-thread drain — but `board_post` still classifies as TICK_WRITE,
    because a post is buffered and executed in a flush like any other write and
    carries the same flush_id as the actions around it. So a post takes
    write() and a read takes read(); routing a post through read() would ask
    the gateway for the wrong polarity.
    """
    state = ms.BridgeState()
    state.connected = True
    state.mock_mode = False
    seen: list[tuple[str, bool]] = []

    async def _rec(self, action, params, *, write=False, **kw):
        seen.append((action, bool(write)))
        return {"success": True, "data": {}}

    monkeypatch.setattr(ms.BridgeGateway, "_call", _rec, raising=True)
    gw = ms.BridgeGateway(state)
    monkeypatch.setattr(ms, "_get_state", lambda: state, raising=True)
    monkeypatch.setattr(ms, "_get_gateway", lambda: gw, raising=True)

    async def _ok(_state):
        return None
    monkeypatch.setattr(ms, "_ensure_connected", _ok, raising=True)

    asyncio.run(ms.board_post("x"))
    asyncio.run(ms.board_read())
    assert seen == [("board_post", True), ("board_read", False)], seen
