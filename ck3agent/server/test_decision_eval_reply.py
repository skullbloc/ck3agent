"""L18-B defects 3 (the 186K decisions reply) and 6 (decisions_annotated /
valid_decisions answering the plain list): one root cause, two doors.

THE TRANSCRIPTS. scale0 n8/s2 (Charles 12015, 0867.05.28):
  do(action_class="decisions", candidates=["merge_aquitaine_decision"])
  -> "Error: result (186,485 characters) exceeds maximum allowed tokens"
     {"result":{"action_class":"decisions","available":[],"unavailable":[
      {"label":"favored_movement_consults_heaven_decision", ... "blocked":{"gate":
      "gate_introspection_unavailable", ... "summary":"blocked behind the frozen gate
      path (O3)"}}, ... every decision in the game ...]}}
budget0 e1/s1 (Cui 13191, 0867.11.25): discover(what="decisions_annotated",
decisions=[6 keys]) and discover(what="valid_decisions", decisions=[8 keys]) both
answered the same 26,285-character {"decisions":[{"name":..,"hash":..}, ...]} that
discover(what="decisions") answers.

THE CAUSE (bridge). The bridge log has the request framed as every MCP call is:
  Action: list_valid_decisions params: {"action":"list_valid_decisions","params":
  {"character_id":11329,"decisions":["visit_local_shrine_decision"]},...}
(budget0 e1 bridge-full.log line 143872). submit_decision_eval read
d["decisions"] at the top level only, found none, and served enumerate_decisions():
the light name+hash list of every decision, no `available`, no `failures`. The
server then printed each light row as "blocked behind the frozen gate path".
The bridge half of the fix (bridge::decision_request through action_args) is in
the decisions-params patch with its own test (test/test_action_args.cpp).

THIS FILE: the server never passes an unevaluated list through again.
  E1  the pilot's light reply to do(decisions) is refused not_evaluated, and the
      reply is small
  E2  an evaluated reply keeps only the decisions asked for
  E3  discover(decisions_annotated|valid_decisions) without decisions is a
      missing argument and sends nothing (it used to fetch the light list)
  E4  discover's annotated read refuses the light reply the same way (one reading,
      redesign.decision_eval_rows, behind both doors)
"""

import asyncio
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
R = pytest.importorskip("redesign")
ms = pytest.importorskip("mcp_server")

#: the first rows of the reply every pilot seat got (enumerate_decisions keys:
#: name, hash; then count), as discover(what="decisions") printed them
LIGHT_ROWS = [
    {"name": "favored_movement_consults_heaven_decision", "hash": 2728205313},
    {"name": "stress_loss_comfort_eater_decision", "hash": 2699260930},
    {"name": "foster_integration_decision", "hash": 222084098},
    {"name": "commission_legend_artifact_decision", "hash": 3437383172},
    {"name": "merge_aquitaine_decision", "hash": 1},
]
LIGHT = {"decisions": LIGHT_ROWS * 220, "count": 1100}


def _evaluated(rows):
    # enumerate_decisions_annotated keys (query_handlers.cpp): action, character_id,
    # current, cost_mapping_confidence, decisions[name, hash, shown, valid, cost_ok,
    # afford_ok, available, failures], count, considered, partial, budget_ms,
    # eval_errors, valid_only
    return {"action": "list_valid_decisions", "character_id": 12015,
            "current": {"gold": 10.0, "piety": 5.0, "prestige": 100.0},
            "cost_mapping_confidence": "medium", "decisions": rows, "count": len(rows),
            "considered": len(rows), "partial": False, "budget_ms": 2000,
            "eval_errors": 0, "valid_only": True}


class GW:
    def __init__(self, reply):
        self.reply, self.reads, self.health = reply, [], "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": (params or {}).get("character_id"), "alive": True}
        return self.reply

    def game(self):
        return {"tick": 148, "date": "0867.05.28", "paused": False}


def run(coro):
    return asyncio.run(coro)


def test_E1_the_pilots_light_reply_is_refused_not_evaluated():
    env = run(R.do_impl(GW(LIGHT), action_class="decisions",
                        candidates=["merge_aquitaine_decision"], character_id=12015))
    assert env["ok"] is False and env["blocked"]["gate"] == "not_evaluated", env["blocked"]
    assert env["blocked"]["measured"] == {"requested": 1, "listed_unevaluated": 1100}
    assert len(json.dumps(env)) < 3000, len(json.dumps(env))


def test_E2_an_evaluated_reply_keeps_only_what_was_asked():
    rows = [{"name": "merge_aquitaine_decision", "hash": 1, "shown": True, "valid": False,
             "cost_ok": True, "afford_ok": True, "available": False,
             "failures": ["MERGE_AQUITAINE_REQ"]},
            {"name": "stress_loss_comfort_eater_decision", "hash": 2, "available": True}]
    env = run(R.do_impl(GW(_evaluated(rows)), action_class="decisions",
                        candidates=["merge_aquitaine_decision"], character_id=12015))
    res = env["result"]
    labels = [r["label"] for r in res["available"] + res["unavailable"]]
    assert labels == ["merge_aquitaine_decision"], res


class Bridge:
    def __init__(self, reply):
        self.reply, self.sent = reply, []

    def answer(self, path, action, params, timeout):
        if action == "get_tick_info":
            return {"paused": True, "game_speed": 1}
        self.sent.append(action)
        return self.reply


@pytest.fixture
def bridge(monkeypatch):
    # test_discover_tool's wiring: _query_bridge / _action_bridge answer from the
    # fake; the unbound check is that file's subject, not this one's
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 12, "867.1.9"
    b = Bridge(LIGHT)
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def qb(_s, action, params, timeout=15.0):
        return b.answer("query", action, params, timeout)

    async def ab(_s, action, params, timeout=5.0):
        return b.answer("action", action, params, timeout)

    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_action_bridge", ab)

    async def bound(_gw, _tool, _action, _params):
        return None
    monkeypatch.setattr(R, "unbound_refusal", bound)
    return b


@pytest.mark.parametrize("what", ["decisions_annotated", "valid_decisions"])
def test_E3_an_annotated_read_of_nothing_is_a_missing_argument(bridge, what):
    env = run(ms.discover(what=what, character_id=13191))
    assert env["ok"] is False and env["blocked"]["gate"] == "missing_required_argument", env
    assert not [a for a in bridge.sent if "decision" in a], bridge.sent


@pytest.mark.parametrize("what", ["decisions_annotated", "valid_decisions"])
def test_E4_discover_refuses_the_light_reply_too(bridge, what):
    env = run(ms.discover(what=what, character_id=13191,
                          decisions=["situation_dynastic_cycle_claim_mandate_decision"]))
    assert env["ok"] is False and env["blocked"]["gate"] == "not_evaluated", env
