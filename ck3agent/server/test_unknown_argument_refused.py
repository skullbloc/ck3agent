"""L18 ruling (a): every tool refuses an argument its schema does not declare,
with the typed gate `unknown_argument` naming it, before the tool runs.

FastMCP's argument model ignores an extra key, so a call that carried one ran as
if the key were absent: pass_turn(character_id=...) on 174 pilot calls (that tool
now takes character_id: the explicit case, test_schema_takes_what_seats_send.py)
and steer's flat axes once (L18-B). The refusal is one place
(mcp_server._StrictArgumentsFastMCP.call_tool); these rows drive every registered
tool through FastMCP's own call_tool, the route a client's tools/call takes.
"""

import asyncio
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
ms = pytest.importorskip("mcp_server")
R = pytest.importorskip("redesign")

TOOLS = sorted(t["name"] for t in ms.registered_tool_definitions())
BOGUS = "target_character"          # a plausible misspelling no tool declares


def _payload(out):
    if isinstance(out, tuple):
        out = out[-1] if isinstance(out[-1], dict) else out[0]
    if isinstance(out, dict):
        return out.get("result", out) if "ok" not in out else out
    for block in out:
        text = getattr(block, "text", None)
        if text:
            return json.loads(text)
    raise AssertionError(out)


@pytest.fixture
def recorded(monkeypatch):
    """Every tool body replaced by a recorder: a call that reaches it was not refused."""
    ran = []
    for name in TOOLS:
        tool = ms.mcp._tool_manager.get_tool(name)

        async def _rec(*a, _n=name, **k):
            ran.append((_n, k))
            return {"ok": True, "tool": _n, "ran": True}
        monkeypatch.setattr(tool, "fn", _rec)
    monkeypatch.setattr(ms, "_get_state", lambda: ms.BridgeState())
    return ran


def test_the_surface_is_the_thirteen():
    assert len(TOOLS) == 13, TOOLS


@pytest.mark.parametrize("name", TOOLS)
def test_an_unknown_argument_is_refused_by_name(name, recorded):
    props = {t["name"]: t for t in ms.registered_tool_definitions()}[name]["input_schema"].get("properties", {})
    assert BOGUS not in props
    env = _payload(asyncio.run(ms.mcp.call_tool(name, {BOGUS: 1})))
    assert env["ok"] is False and env["tool"] == name, env
    assert env["blocked"]["gate"] == "unknown_argument", env
    assert env["blocked"]["fix"]["rejected_fields"] == [BOGUS]
    assert BOGUS in env["blocked"]["summary"]
    assert recorded == [], "the tool ran on a call carrying an unknown argument"


@pytest.mark.parametrize("name", TOOLS)
def test_CONTROL_declared_arguments_reach_the_tool(name, recorded):
    """The refusal is not blanket: a call with only declared names (character_id,
    which every tool takes) runs the tool."""
    env = _payload(asyncio.run(ms.mcp.call_tool(name, {"character_id": 7})))
    assert env.get("ran") is True, env
    assert recorded and recorded[0][0] == name and recorded[0][1].get("character_id") == 7


def test_the_gate_is_typed_in_the_one_table():
    row = R.GATE_TABLE["unknown_argument"]
    assert row["retryable"] is False and row["severity"] == "invalid"
    assert "unknown_argument" in R.GATE_WHY and "unknown_argument" in R.GATE_NEXT


def test_unknown_arguments_reads_the_schema():
    schema = {"properties": {"a": {}, "b": {}}}
    assert ms.unknown_arguments(schema, {"a": 1, "z": 2, "c": 3}) == ["c", "z"]
    assert ms.unknown_arguments(schema, {"a": 1}) == []
    assert ms.unknown_arguments(schema, None) == []
