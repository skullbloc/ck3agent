"""L20-B item 1 (smoke17 B3): a named decision comes back as a row, or as a
refusal naming it — never as nothing.

THE SMOKE. probe-paused.jsonl row 23 (Bhoja 11329, 0867.02.20, paused):
  do(action_class="decisions", candidates=["visit_local_shrine_decision"])
  -> 424 chars {"action_class":"decisions","available":[],"unavailable":[]}
and the bridge log beside it (bridge.log line 24130-24131):
  Action: list_valid_decisions params: {"action":"list_valid_decisions","params":
  {"character_id":11329,"decisions":["visit_local_shrine_decision"]},...}
  WARNING: SIGSEGV safety net triggered: ... rip_off=0x4a5b0d3 ... thread=game_thread

THE CAUSES, both read off enumerate_decisions_annotated (query_handlers.cpp):
  1. the decision's eval faulted, and the wedged row it emitted is named only from
     the hash-only table (resolve_decision_name), which does not hold this
     decision: a row {hash, eval_error, eval_wedged} with no name, which
     decision_eval_rows could not match to the name asked for and dropped;
  2. the server asked list_valid_decisions, which drops every evaluated decision
     that is not available (`if (valid_only && !available) continue`), so even a
     clean "no" could never reach `unavailable`.
The bridge now names every row from the decision's own key (read before the
eval) and lists the requested names it matched nothing for (`unmatched`); the
server reads the annotated list and turns each case into a row or a refusal.

MERGED WITH L20-D. The fault itself was the requirement-name read: for every block
that evaluated false the bridge destroyed EVAL_WITH_FAILURES' diag record through a
slipped DIAG_OBJECT_DTOR (0x04a5b060, mid-function); D's 0x0595b060 is the engine's
own callee, witnessed at runtime (bridge/include/diag_dtor.h). So on the merged tree:
  - a decision that fails a requirement is an `unavailable` row whose reason is the
    requirement's loc-key (`decision_unavailable`, the failures list);
  - `decision_eval_faulted` stays a typed state, reached through the eval loop's
    outer SIGSEGV/watchdog recovery (`eval_wedged`), the skip list (`eval_skipped`),
    or a block whose own guarded evaluation crashed (dv_evaluate's `crashed` path,
    query_handlers.cpp: `eval_error` alone, no `eval_wedged`; witness r1 item 10);
  - when resolve_diag_dtor() cannot witness the destructor the bridge skips the
    name read and the row carries `failure_key_unwitnessed` (query_handlers.cpp,
    enumerate_decisions_annotated): `decision_reason_unwitnessed`, never a
    reasonless unavailable row.

The bridge replies below are built from the emitter's keys
(query_handlers.cpp enumerate_decisions_annotated: the wedged row's name, hash,
eval_error, eval_wedged; the trailer's count, considered, partial, budget_ms,
eval_errors, valid_only, unmatched).
"""

import asyncio
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
R = pytest.importorskip("redesign")

ASK = ["visit_local_shrine_decision"]


def _reply(rows, *, unmatched=None, partial=False, valid_only=False, considered=None):
    out = {"action": "list_valid_decisions" if valid_only else "list_decisions",
           "character_id": 11329, "current": {"gold": 22.41, "piety": 49.31, "prestige": 1080.94},
           "cost_mapping_confidence": "medium", "decisions": rows, "count": len(rows),
           "considered": len(rows) if considered is None else considered,
           "partial": partial, "budget_ms": 2000,
           "eval_errors": sum(1 for r in rows if r.get("eval_error")), "valid_only": valid_only}
    if unmatched is not None:
        out["unmatched"] = unmatched
    return out


#: what the 2026.12.0 bridge sent for B3: the wedged row, no name, no witness
SMOKE17_B3 = _reply([{"hash": 3105402711, "eval_error": True, "eval_wedged": True}], valid_only=True)
#: the same fault through this lane's bridge: named, and nothing unmatched
WEDGED_NAMED = _reply([{"name": ASK[0], "hash": 3105402711, "eval_error": True, "eval_wedged": True}],
                      unmatched=[])
NOT_TAKEABLE = _reply([{"name": ASK[0], "hash": 3105402711, "shown": True, "valid": False,
                        "cost_ok": True, "afford_ok": True, "available": False,
                        "failures": ["VISIT_SHRINE_NOT_ENOUGH_PIETY"], "cost": {}}], unmatched=[])
UNKNOWN = _reply([], unmatched=["visit_lokal_shrine_decision"], considered=0)
#: the engine said no and the bridge refused the name read (diag dtor not witnessed)
UNWITNESSED = _reply([{"name": ASK[0], "hash": 3105402711, "shown": True, "valid": False,
                       "cost_ok": True, "afford_ok": True, "available": False,
                       "failures": [], "failure_key_unwitnessed": True, "cost": {}}], unmatched=[])


class GW:
    def __init__(self, reply):
        self.reply, self.reads, self.health = reply, [], "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": (params or {}).get("character_id"), "alive": True, "name": "Bhoja"}
        return self.reply

    def game(self):
        return {"tick": 50, "date": "0867.02.20", "paused": True}


def _do(reply, candidates=ASK):
    gw = GW(reply)
    env = asyncio.run(R.do_impl(gw, action_class="decisions", candidates=list(candidates),
                                character_id=11329))
    return gw, env


def test_the_annotated_read_is_asked_not_the_valid_only_one():
    gw, _ = _do(NOT_TAKEABLE)
    sent = [a for a, _ in gw.reads if "decision" in a]
    assert sent == ["list_decisions_annotated"], sent


def test_a_decision_the_engine_judged_no_is_an_unavailable_row_with_its_reasons():
    _, env = _do(NOT_TAKEABLE)
    assert env["ok"] is True, env
    rows = env["result"]["unavailable"]
    assert [r["label"] for r in rows] == ASK and env["result"]["available"] == []
    b = rows[0]["blocked"]
    assert b["gate"] == "decision_unavailable", b
    assert "VISIT_SHRINE_NOT_ENOUGH_PIETY" in b["summary"]


def test_a_faulted_eval_is_a_row_naming_the_fault():
    _, env = _do(WEDGED_NAMED)
    rows = env["result"]["unavailable"]
    assert [r["label"] for r in rows] == ASK, env["result"]
    assert rows[0]["blocked"]["gate"] == "decision_eval_faulted"
    assert "frozen gate path" not in json.dumps(env)


def test_a_name_the_engine_does_not_know_is_refused_naming_it():
    _, env = _do(UNKNOWN, candidates=["visit_lokal_shrine_decision"])
    assert env["ok"] is False, env
    assert env["blocked"]["gate"] == "unknown_decision"
    assert "visit_lokal_shrine_decision" in env["blocked"]["summary"]


def test_a_known_and_an_unknown_name_give_the_row_and_name_the_other():
    reply = dict(NOT_TAKEABLE, unmatched=["no_such_decision"])
    _, env = _do(reply, candidates=ASK + ["no_such_decision"])
    assert env["ok"] is True
    assert [r["label"] for r in env["result"]["unavailable"]] == ASK
    w = [x for x in env["warnings"] if x["code"] == "unknown_decision"]
    assert w and w[0]["names"] == ["no_such_decision"], env["warnings"]


def test_unmatched_after_a_budget_stop_is_not_reached_never_unknown():
    reply = dict(UNKNOWN, partial=True)
    _, env = _do(reply, candidates=["visit_lokal_shrine_decision"])
    assert env["ok"] is True and env["blocked"] is None
    assert [x["code"] for x in env["warnings"]] == ["decisions_not_reached"]


def test_the_smoke17_reply_from_a_bridge_without_the_witness_says_the_rows_are_unexplained():
    """The 2026.12.0 bridge's B3 reply: an unnamed row, no `unmatched`. Nothing is
    claimed unknown (nobody witnessed it); the missing row is said, not silent."""
    _, env = _do(SMOKE17_B3)
    assert env["ok"] is True
    w = [x for x in env["warnings"] if x["code"] == "decision_rows_unexplained"]
    assert w and w[0]["names"] == ASK, env["warnings"]
    assert not [x for x in env["warnings"] if x["code"] == "unknown_decision"]


def test_control_an_available_decision_is_an_available_row():
    reply = _reply([{"name": ASK[0], "hash": 1, "shown": True, "valid": True, "cost_ok": True,
                     "afford_ok": True, "available": True, "failures": [], "cost": {}}], unmatched=[])
    _, env = _do(reply)
    assert [r["label"] for r in env["result"]["available"]] == ASK
    assert env["result"]["available"][0]["blocked"] is None and env["warnings"] == []


def test_an_unwitnessed_dtor_is_its_own_typed_state_not_a_reasonless_row():
    """L20-B x L20-D: resolve_diag_dtor() returned 0, so the failed requirement's
    name was not read. The verdict (not takeable) stands; the row says why it has
    no reason, and it is neither the fault nor the bridge-gave-no-reason gate."""
    _, env = _do(UNWITNESSED)
    rows = env["result"]["unavailable"]
    assert [r["label"] for r in rows] == ASK, env["result"]
    b = rows[0]["blocked"]
    assert b["gate"] == "decision_reason_unwitnessed", b
    assert b["measured"]["failure_key_unwitnessed"] is True
    # control: the same row without the witness flag is the generic no-reason gate
    plain = _reply([dict(UNWITNESSED["decisions"][0], failure_key_unwitnessed=False)], unmatched=[])
    _, env2 = _do(plain)
    assert env2["result"]["unavailable"][0]["blocked"]["gate"] == "gate_introspection_unavailable"


def test_a_decision_the_engine_does_not_show_is_typed_not_a_missing_reason():
    """L20 r3, witness item 6: shown:false (the engine's is_shown verdict,
    query_handlers.cpp rb.key("shown")) with no failures was labelled
    gate_introspection_unavailable, "the engine gave no reason"."""
    hidden = _reply([{"name": ASK[0], "hash": 3105402711, "shown": False, "valid": True,
                      "cost_ok": True, "afford_ok": True, "available": False,
                      "failures": [], "cost": {}}], unmatched=[])
    _, env = _do(hidden)
    b = env["result"]["unavailable"][0]["blocked"]
    assert b["gate"] == "decision_not_shown", b
    assert b["measured"] == {"shown": False}


def test_a_crashed_block_alone_is_also_a_faulted_row():
    """dv_evaluate's `crashed` path (query_handlers.cpp): a guarded block that
    crashed sets eval_error and nothing else; that row is decision_eval_faulted
    too, not only eval_wedged/eval_skipped (witness r1 item 10)."""
    _, env = _do(_reply([{"name": ASK[0], "hash": 3105402711, "eval_error": True}], unmatched=[]))
    b = env["result"]["unavailable"][0]["blocked"]
    assert b["gate"] == "decision_eval_faulted", b
