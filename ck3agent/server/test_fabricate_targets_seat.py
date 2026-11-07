"""L20 r3, witness item 3: do(action_class="fabricate_targets") and the seat.

THE DEFECT (c9610822, redesign.py:5888-5900). The enumeration never passed
`seat` to query_fabricate_targets, and every row's do_call printed `seat:
court_chaplain` (FABRICATE_CLAIM_SEAT). For B5's ruler (Cui 13191, ministries:
smoke17 probe-paused.jsonl:32, set_council_task minister_justice /
task_fabricate_claim) that call resolves to the chaplain's task. The bridge has
taken `seat` since L20-A and names the version it evaluated.

THE REPLY KEYS are do_query_fabricate_targets' r.key lines
(bridge/src/action_handlers.cpp): the success reply's task, task_used,
task_seat, task_seats, counts and targets; the seat_cannot_take_task refusal's
requested, requested_seat, task_seats; the no_matching_position refusal's
requested, task_seat, task_seats (the last two added in this round). The
2026.12.0 empty reply (smoke17 probe-paused.jsonl:22, B6.fab, Cui 13191) carried
no task_seats; that shape is kept as a row too.
"""
import asyncio
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
R = pytest.importorskip("redesign")
from test_lane_query import ScriptedGateway                       # noqa: E402

BRIDGE = Path(__file__).resolve().parents[2] / "bridge" / "src" / "action_handlers.cpp"
CUI = 13191
CHAPLAIN, JUSTICE = "councillor_court_chaplain", "minister_justice"


def _run(c):
    return asyncio.run(c)


def _seg():
    src = BRIDGE.read_text(encoding="utf-8", errors="replace")
    a = src.index("bool do_query_fabricate_targets(")
    b = src.find("\nbool do_", a + 10)
    return src[a: b if b > 0 else len(src)]


def test_the_emitter_writes_the_keys_these_rows_carry():
    seg = _seg()
    for k in ("task_used", "task_seat", "task_seats", "requested_seat", "targets"):
        assert 'r.key("%s")' % k in seg, k
    nm = seg[seg.index('"no_matching_position"'):]
    nm = nm[: nm.index("r.end_object()")]
    assert 'r.key("task_seats")' in nm and 'r.key("task_seat")' in nm, nm


def _ok(task_used, task_seat, targets):
    return {"success": True, "action": "query_fabricate_targets", "task": "task_fabricate_claim",
            "task_used": task_used, "task_seat": task_seat, "task_seats": [CHAPLAIN, JUSTICE],
            "actor_id": CUI, "counties_scanned": 900, "counties_considered": 3985,
            "eligible_count": len(targets), "assignable_count": len(targets), "seat_vacant": False,
            "assignable_only": True, "scan_truncated": False, "returned": len(targets),
            "targets": targets}


def test_the_seat_reaches_the_bridge_and_the_do_call_names_the_seat_it_resolved():
    gw = ScriptedGateway(cid=CUI, replies={"query_fabricate_targets": _ok(
        "task_fabricate_claim_minister_of_rites", JUSTICE,
        [{"title_id": 14606, "title": "c_x", "assignable": True}])})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=CUI,
                         args={"seat": JUSTICE}))
    (params,) = [p for a, p in gw.reads if a == "query_fabricate_targets"]
    assert params["seat"] == JUSTICE, params
    call = env["result"]["available"][0]["do_call"]
    assert call == {"intent": "council_task",
                    "args": {"seat": JUSTICE, "task": "task_fabricate_claim", "target_title_id": 14606}}


def test_CONTROL_with_no_seat_the_bridge_is_asked_without_one_and_the_row_names_the_chaplain():
    # CONTROL: passes on c9610822 too (the no-seat path is the old behaviour); labelled, not discriminating.
    gw = ScriptedGateway(cid=13902, replies={"query_fabricate_targets": _ok(
        "task_fabricate_claim", CHAPLAIN, [{"title_id": 329, "title": "c_y", "assignable": True}])})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=13902))
    (params,) = [p for a, p in gw.reads if a == "query_fabricate_targets"]
    assert "seat" not in params
    assert env["result"]["available"][0]["do_call"]["args"]["seat"] == "court_chaplain"


def test_a_missing_chaplain_version_warns_with_the_bridges_task_seats():
    gw = ScriptedGateway(cid=CUI, replies={"query_fabricate_targets": {
        "success": False, "action": "query_fabricate_targets", "reason": "no_matching_position",
        "requested": "task_fabricate_claim", "task_seat": CHAPLAIN, "task_seats": [CHAPLAIN, JUSTICE]}})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=CUI))
    (w,) = [w for w in env["warnings"] if w["code"] == "fabricate_targets_empty"]
    assert w["seat"] == "court_chaplain"
    assert w["task_seats"] == ["court_chaplain", JUSTICE]
    assert '"seat": "minister_justice"' in w["detail"], w["detail"]
    assert w["bridge"]["reason"] == "no_matching_position"


def test_the_2026_12_0_empty_reply_still_says_which_seat_and_how_to_pass_one():
    # smoke17 probe-paused.jsonl:22 (B6.fab): success, eligible 1093, assignable 0, no task_seats
    gw = ScriptedGateway(cid=CUI, replies={"query_fabricate_targets": {
        "success": True, "eligible_count": 1093, "assignable_count": 0, "counties_considered": 3985,
        "seat_vacant": False, "scan_truncated": False, "targets": []}})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=CUI))
    (w,) = [w for w in env["warnings"] if w["code"] == "fabricate_targets_empty"]
    assert "not looked up by the bridge" not in w["detail"], w["detail"]
    assert "seat" in w["detail"] and "minister_justice" in w["detail"]


def test_a_seat_with_no_version_is_refused_naming_the_seats_that_have_one():
    gw = ScriptedGateway(cid=CUI, replies={"query_fabricate_targets": {
        "success": False, "action": "query_fabricate_targets", "reason": "seat_cannot_take_task",
        "requested": "task_fabricate_claim", "requested_seat": "marshal",
        "task_seats": [CHAPLAIN, JUSTICE]}})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=CUI,
                         args={"seat": "marshal"}))
    assert env["ok"] is False
    b = env["blocked"]
    assert b["raw_reason"] == "seat_cannot_take_task" and b["gate"] == "target_ineligible", b
    assert b["measured"]["task_seats"] == ["court_chaplain", JUSTICE]
    assert re.search(r"court_chaplain, minister_justice", b["summary"]), b["summary"]


def test_each_row_names_the_task_version_the_bridge_evaluated():
    """L22 (smoke19 B9): with seat minister_justice the bridge evaluated
    task_fabricate_claim_minister_of_rites, but the rows dropped task_used."""
    gw = ScriptedGateway(cid=CUI, replies={"query_fabricate_targets": _ok(
        "task_fabricate_claim_minister_of_rites", JUSTICE,
        [{"title_id": 621, "title": "c_a", "assignable": True},
         {"title_id": 14606, "title": "c_b", "assignable": False}])})
    env = _run(R.do_impl(gw, action_class="fabricate_targets", character_id=CUI,
                         args={"seat": JUSTICE}))
    rows = env["result"]["available"] + env["result"]["unavailable"]
    assert len(rows) == 2
    assert {r.get("task_used") for r in rows} == {"task_fabricate_claim_minister_of_rites"}, rows
