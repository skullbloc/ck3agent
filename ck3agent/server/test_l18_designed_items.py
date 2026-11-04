"""L18-B's designed-not-made items, made in the L18 integration.

  F  fabricate_targets empty: the reply names the council seat it used and what
     the bridge said (Cui 13191, an administrative ruler, got an empty list and
     no reason). The bridge reply keys are do_query_fabricate_targets' r.key
     lines (bridge/src/action_handlers.cpp: success, eligible_count,
     assignable_count, counties_considered, seat_vacant, targets).
  P  low_priority_pruned said "re-plan to surface"; the sort is deterministic,
     so the same call prunes the same rows: it says raise max_results, with the
     value that probes them all.
  M  empty_reason carries unchanged_since / times from a per-ruler memo: the
     pilots re-asked plan_war cycle after cycle and could not see it had not moved.
"""
import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
R = pytest.importorskip("redesign")
from test_lane_query import ScriptedGateway                       # noqa: E402
import test_plan_war_claim_rows as PW                              # noqa: E402

BRIDGE = Path(__file__).resolve().parents[2] / "bridge" / "src" / "action_handlers.cpp"


def _run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh_memo():
    R._PLAN_WAR_EMPTY_MEMO.clear()
    yield
    R._PLAN_WAR_EMPTY_MEMO.clear()


# ── F ─────────────────────────────────────────────────────────────────────────
def test_F_the_emitter_writes_the_keys_the_empty_reply_carries():
    src = BRIDGE.read_text(encoding="utf-8", errors="replace")
    seg = src[src.index("bool do_query_fabricate_targets("):]
    for k in ("eligible_count", "assignable_count", "counties_considered", "seat_vacant", "targets"):
        assert 'r.key("%s")' % k in seg, k


def test_F_an_empty_fabricate_list_names_the_seat_and_the_bridges_counts():
    gw = ScriptedGateway(replies={"query_fabricate_targets": {
        "success": True, "action": "query_fabricate_targets", "task": "task_fabricate_claim",
        "actor_id": 13191, "counties_scanned": 900, "counties_considered": 41,
        "eligible_count": 0, "assignable_count": 0, "seat_vacant": False,
        "assignable_only": False, "scan_truncated": False, "returned": 0, "targets": []}})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=13191))
    (w,) = [w for w in env["warnings"] if w["code"] == "fabricate_targets_empty"]
    assert w["seat"] == "court_chaplain" == R.FABRICATE_CLAIM_SEAT
    assert w["bridge"]["eligible_count"] == 0 and w["bridge"]["counties_considered"] == 41
    assert "minister_justice" in w["detail"]


def test_F_CONTROL_a_list_with_rows_carries_no_empty_warning():
    gw = ScriptedGateway(replies={"query_fabricate_targets": {
        "success": True, "targets": [{"title_id": 329, "title": "c_x", "assignable": True}]}})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=13191))
    assert "fabricate_targets_empty" not in [w["code"] for w in env["warnings"]]
    assert env["result"]["available"][0]["do_call"]["args"]["seat"] == R.FABRICATE_CLAIM_SEAT


# ── P ─────────────────────────────────────────────────────────────────────────
def _many_targets(n):
    # do_query_valid_war_targets rows (r.key("target_char_id"), as PW.C4 cites)
    return {"action": "query_valid_war_targets", "actor_id": PW.CHARLES, "targets": [
        {"target_char_id": 20000 + i, "target_name": "T%d" % i, "title_id": 3000 + i,
         "title_name": "t", "tier": 2, "tier_name": "county", "cb_type": "claim",
         "pressed": True, "implicit": False, "target_independent": True,
         "target_imprisoned": False, "target_in_alliance": False, "target_in_truce": False,
         "already_at_war_opposing": False, "declarable": True} for i in range(n)]}


def test_P_pruned_rows_say_raise_max_results_not_replan():
    gw = PW._gw(query_valid_war_targets=_many_targets(20), query_conquest_targets={"duchies": []})
    res = _run(R.plan_war_impl(gw, character_id=PW.CHARLES, goal_scoped=False,
                               ally_free_only=False, max_results=8))["result"]
    pruned = [e["blocked"] for e in res["excluded"]
              if (e.get("blocked") or {}).get("gate") == "low_priority_pruned"]
    assert len(pruned) == 20 - 16, res["excluded"]          # cap = max(12, 2*8)
    assert "re-plan" not in pruned[0]["summary"] and "raise max_results" in pruned[0]["summary"]
    assert pruned[0]["fix"] == {"max_results": 10} and pruned[0]["retryable"] is False


# ── M ─────────────────────────────────────────────────────────────────────────
def test_M_the_same_empty_reason_counts_its_times_and_keeps_its_first_date():
    calls = []
    for date in ("0867.01.01", "0867.02.01", "0867.03.01"):
        gw = PW._gw(query_conquest_targets=PW._cui_conquest(),
                    query_valid_war_targets={"action": "query_valid_war_targets",
                                             "actor_id": PW.CUI, "targets": []})
        gw.game = (lambda d=date: {"tick": 0, "date": d, "paused": True})
        calls.append(_run(R.plan_war_impl(gw, character_id=PW.CUI))["result"]["empty_reason"])
    assert [c["times"] for c in calls] == [1, 2, 3]
    assert {c["unchanged_since"] for c in calls} == {"0867.01.01"}


def test_M_CONTROL_a_different_call_or_a_plan_with_rows_starts_over():
    R.remember_empty_reason(1, {"gate": "no_valid_targets", "measured": {}}, "867.1.1", (True,))
    e = {"gate": "no_valid_targets", "measured": {}}
    R.remember_empty_reason(1, e, "867.2.1", (False,))          # another call's arguments
    assert e["times"] == 1 and e["unchanged_since"] == "867.2.1"
    R.remember_empty_reason(1, None, "867.3.1", (False,))        # rows came back
    e = {"gate": "no_valid_targets", "measured": {}}
    R.remember_empty_reason(1, e, "unknown", (False,))
    assert e["times"] == 1 and e["unchanged_since"] is None      # an unread date is None
