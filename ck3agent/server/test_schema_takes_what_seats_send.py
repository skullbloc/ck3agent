"""L18-B defects 2 and 5: the registered schema accepts what the toolkit itself
tells a seat to send, and drops nothing silently.

D2 (budget0 e1/s4 0867.01.01 Eung-ryeom 14942, scale0 n8/s7 Nasr 12843). plan_war
writes `"claimant_id": null` in every non-claim row's declare_args
(redesign.plan_war_impl). Two seats passed the row's fields flat and the MCP
boundary refused before the tool ran:

  declare_war(character_id=14942, target_id=14660, cb="individual_county_de_jure_cb",
              cb_title_id=16938, claimant_id=null)
  -> "1 validation error for declare_warArguments / claimant_id / Input should be a
      valid integer [type=int_type, input_value=None, input_type=NoneType]"

declare_war_impl already reads a null claimant as none (`_as_int(... or claimant_id)`);
only the signature said int.

D5. The protocol (release/overlay/ck3agent/protocol/run-prompt.md, CLAUDE-desktop.md)
says pass character_id on every call. pass_turn's schema had only `reason`; 174
pilot calls passed `pass_turn(character_id=...)` (20 seats) and FastMCP dropped the
argument without a word. Census of the pilot `.tools.json` schemas against every
call's arguments: pass_turn.character_id 174, steer's flat axes 1 call, nothing else.
"""

import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
ms = pytest.importorskip("mcp_server")


def run(coro):
    return asyncio.run(coro)


def _defs():
    return {t["name"]: t for t in ms.registered_tool_definitions()}


@pytest.fixture
def refusing(monkeypatch):
    """The tool body answers bridge_disconnected at once: a call that reaches it
    has passed the MCP argument validation, which is what these rows measure."""
    async def _refuse(state, tool):
        return {"ok": False, "tool": tool, "blocked": {"gate": "bridge_disconnected"}}
    state = ms.BridgeState()
    monkeypatch.setattr(ms, "_get_state", lambda: state)
    monkeypatch.setattr(ms, "_ensure_connected", _refuse)


def _payload(out):
    # FastMCP call_tool returns content blocks (and, in newer versions, a structured
    # copy); either way the tool's envelope is in there as JSON text or a dict.
    import json
    if isinstance(out, tuple):
        out = out[-1] if isinstance(out[-1], dict) else out[0]
    if isinstance(out, dict):
        return out.get("result", out)
    for block in out:
        text = getattr(block, "text", None)
        if text:
            return json.loads(text)
    raise AssertionError(out)


def test_D2_declare_war_takes_the_null_claimant_plan_war_writes(refusing):
    # the exact arguments of budget0 e1/s4 line 31
    args = {"character_id": 14942, "target_id": 14660, "cb": "individual_county_de_jure_cb",
            "cb_title_id": 16938, "claimant_id": None}
    env = _payload(run(ms.mcp.call_tool("declare_war", args)))
    assert env["blocked"]["gate"] == "bridge_disconnected", env


def test_D2_CONTROL_a_claimant_that_is_not_an_id_is_still_refused(refusing):
    with pytest.raises(Exception) as e:
        run(ms.mcp.call_tool("declare_war", {"target_id": 14660, "cb": "claim_cb",
                                             "claimant_id": "the duke"}))
    assert "claimant_id" in str(e.value)


def test_D5_every_registered_tool_takes_character_id():
    """The protocol's rule is 'character_id on every call': every tool the server
    registers takes it, so a seat that follows the rule is never silently dropped."""
    missing = sorted(n for n, t in _defs().items()
                     if "character_id" not in (t["input_schema"].get("properties") or {}))
    assert missing == [], missing


def test_D5_pass_turn_records_and_echoes_who_passed(monkeypatch):
    state = ms.BridgeState()
    state.connected, state.tick_id = True, 7
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def _now():
        return None
    monkeypatch.setattr(ms, "_gateway_now", _now)
    env = run(ms.pass_turn(reason="truce holds", character_id=13191))
    assert env["result"]["character_id"] == 13191, env
    assert state.action_log[-1]["action"]["character_id"] == 13191
    env = run(ms.pass_turn(reason="truce holds"))      # CONTROL: the primary, unchanged shape
    assert "character_id" not in env["result"], env
