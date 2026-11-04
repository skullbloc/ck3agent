"""L18-B defect 1 (and the plan_war halves of defect 4): a plan_war row is
declarable by the same gate declare_war applies.

THE DEFECT, from the pilot transcripts (ck3agent 2026.11.0, budget0 e1/s2 and
e2/s2, Charles 12015, 0867.01.01). plan_war answered

  {"declare_args":{"target_id":13356,"cb":"claim_cb","cb_title_id":7448,
   "claimant_id":null},"title":"c_venaissin","claim_pressed":false,
   "cb_options":[{"cb":"claim_cb","valid":true,...,"cb_title_id":2189,...}]}

and declare_war(target 13356, claim_cb, cb_title_id 7448) came back
rejected / claim_not_pressed. The same seat later declared claim_cb with
cb_title_id 2189 and it was applied ("West Francian Claim on the Kingdom of
Italy", war 17). The row came from a CONQUEST county (c_venaissin 7448, held
under 13356); the CB the game accepts against 13356 is claim_cb on the title
Charles holds a PRESSED claim on, which the cbs emitter names as the claim
row's cb_title_id (query_handlers.cpp emit_cbs_target_body:
`cb_title_id = claimed_title` from resolve_pressed_claim_title). plan_war kept
the county's id. declare_war's gate (intent_dispatch.cpp step 2c) looks for a
pressed claim on exactly cb_title_id, so the row could never land.

Every double below carries the keys the EMITTER writes (cited per reply), with
the values of the transcript; nothing is a key a reader wished for.

  C1  the conquest-county row whose declarable CB is claim_cb declares the
      claim's title (2189), the claimant (the actor), claim_pressed true, and
      names the claim's title, not the county
  C2  the same row with no pressed-claim title read is excluded as
      claim_not_pressed, never offered
  C3  CONTROL: a conquest row whose declarable CB is the conquest CB keeps the
      county as cb_title_id (the fix touches claim CBs only)
  C4  the enumerator's rows reach plan_war: do_query_valid_war_targets writes
      `target_char_id` (query_handlers.cpp), and plan_war read `target_id`, so
      every claim/independence row was dropped before the strict probe
  C5  Cui 13191's goal-scoped plan: every remaining county of his partly-held
      duchies is held inside his own realm (holder_top_liege_id 13191 — the
      transcript's query(conquest_targets) rows). No target is probed as the
      actor himself, and the empty reason says so instead of
      "target_not_independent"
"""

import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
R = pytest.importorskip("redesign")

CHARLES, LOTHAR = 12015, 13356
C_VENAISSIN, K_ITALY = 7448, 2189
CUI = 13191


class GW:
    def __init__(self, replies):
        self.replies = dict(replies)
        self.reads = []
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        r = self.replies.get(action, {})
        return r(params or {}) if callable(r) else r

    def game(self):
        return {"tick": 0, "date": "0867.01.01", "paused": True}


def _run(coro):
    return asyncio.run(coro)


def _character(p):
    # build_char_profile keys (query_handlers.cpp): id, alive, age, stress
    return {"id": p.get("character_id"), "alive": True, "age": 43, "stress": 0}


def _venaissin_conquest():
    # do_query_conquest_targets row shape, as the e1/s1 seat printed it with jq
    return {"duchies": [{"duchy_id": 7440, "duchy_key": "d_provence", "remaining_counties": [
        {"county_id": C_VENAISSIN, "county_key": "c_venaissin", "county_name": "Venaissin",
         "holder_id": 13400, "holder_top_liege_id": LOTHAR, "holder_top_liege_independent": True,
         "is_independent": False, "is_kin": False, "is_allied": False, "is_truced": False,
         "already_at_war_opposing": False, "adjacent": True, "conquerable_now": False,
         "conquerable_now_source": "heuristic", "block_reason": "not_adjacent_unverified"}]}]}


def _strict(cb, declarable=True, cull=None):
    # do_query_cb_validity_strict results[] keys
    row = {"cb_index": 1, "cb_name": cb, "target_mode": "claim" if cb == "claim_cb" else "title",
           "outer_status": "pass", "inner_status": "pass", "declarable": declarable}
    if cull:
        row["cull_reason"] = cull
    return {"status": "ok", "results": [row], "declarable_count": int(declarable)}


def _cbs(rows):
    # do_query_cbs_for_target: emit_cbs_target_body keys
    return {"status": "ok", "target_state": {"in_truce": False}, "cbs": rows}


def _claim_cbs_row(title=K_ITALY, claimant=CHARLES, valid=True):
    row = {"cb_key": "claim_cb", "family": "claim", "valid": valid, "declarable": valid,
           "scope": "county", "grants": "the claimed title(s) and the counties under them (installs the claimant)",
           "requirements_unmet": [], "notes": ["target_mode=6 (claim): pass claimant_id + cb_title_id to dispatch"],
           "cb_title_id": title, "needs_cb_title_id": True, "cost_prestige": 0, "cost_piety": 0,
           "affordable": True, "outer_status": "pass", "inner_status": "pass", "target_mode": 6}
    if claimant:
        row["claimant_id"] = claimant
    return row


def _gw(**over):
    replies = {
        "query_character": _character,
        "query_valid_war_targets": {"action": "query_valid_war_targets", "actor_id": CHARLES, "targets": []},
        "query_conquest_targets": _venaissin_conquest(),
        "query_cb_validity_strict": _strict("claim_cb"),
        "query_cbs_for_targets": {"error": "unknown_action"},
        "query_cbs_for_target": _cbs([_claim_cbs_row()]),
        "query_war_stance": {"error": "unknown_action"},
        "query_alliances": lambda p: {"own_military_strength": 2000, "total_callable_strength": 2800},
    }
    replies.update(over)
    return GW(replies)


def test_C1_a_claim_row_declares_the_pressed_claims_title_not_the_county():
    env = _run(R.plan_war_impl(_gw(), character_id=CHARLES, goal_scoped=False, ally_free_only=False))
    (row,) = [t for t in env["result"]["targets"] if t["declare_args"]["cb"] == "claim_cb"]
    assert row["declare_args"] == {"target_id": LOTHAR, "cb": "claim_cb",
                                   "cb_title_id": K_ITALY, "claimant_id": CHARLES}, row
    assert row["claim_pressed"] is True, row
    assert row["title"] == K_ITALY, row


def test_C2_a_claim_cb_with_no_pressed_claim_title_is_not_offered():
    gw = _gw(query_cbs_for_target=_cbs([_claim_cbs_row(title=0, claimant=None)]))
    res = _run(R.plan_war_impl(gw, character_id=CHARLES, goal_scoped=False, ally_free_only=False))["result"]
    assert not [t for t in res["targets"] if t["declare_args"]["cb"] == "claim_cb"], res["targets"]
    gates = [(e.get("blocked") or {}).get("gate") for e in res["excluded"]]
    assert "claim_not_pressed" in gates, res["excluded"]


def test_C3_CONTROL_a_conquest_cb_row_keeps_the_county():
    conquest = _venaissin_conquest()
    rc = conquest["duchies"][0]["remaining_counties"][0]
    rc.update(conquerable_now=True, block_reason="",
              declare={"casus_belli": "county_conquest_cb", "target_id": LOTHAR, "cb_title_id": C_VENAISSIN})
    cc = {"cb_key": "county_conquest_cb", "family": "conquest", "valid": True, "scope": "county",
          "cb_title_id": 0, "needs_cb_title_id": True, "target_mode": 1, "cost_prestige": 100,
          "cost_piety": 0, "affordable": True}
    gw = _gw(query_conquest_targets=conquest, query_cb_validity_strict=_strict("county_conquest_cb"),
             query_cbs_for_target=_cbs([cc]))
    env = _run(R.plan_war_impl(gw, character_id=CHARLES, goal_scoped=False, ally_free_only=False))
    (row,) = env["result"]["targets"]
    assert row["declare_args"] == {"target_id": LOTHAR, "cb": "county_conquest_cb",
                                   "cb_title_id": C_VENAISSIN, "claimant_id": None}, row
    assert row["claim_pressed"] is False


def test_C4_the_enumerators_rows_reach_the_strict_probe():
    # do_query_valid_war_targets: r.key("target_char_id"), title_id, cb_type "claim", pressed
    vt = {"action": "query_valid_war_targets", "actor_id": CHARLES, "targets": [
        {"target_char_id": LOTHAR, "target_name": "Lothar", "title_id": K_ITALY,
         "title_name": "Italy", "tier": 4, "tier_name": "kingdom", "cb_type": "claim",
         "pressed": True, "implicit": False, "target_independent": True,
         "target_imprisoned": False, "target_in_alliance": False, "target_in_truce": False,
         "already_at_war_opposing": False, "declarable": True}]}
    gw = _gw(query_valid_war_targets=vt, query_conquest_targets={"duchies": []})
    _run(R.plan_war_impl(gw, character_id=CHARLES, goal_scoped=False, ally_free_only=False))
    probed = [p.get("target_id") for a, p in gw.reads if a == "query_cb_validity_strict"]
    assert probed == [LOTHAR], gw.reads


def _cui_conquest():
    # scale0 n2/s1 line 117 (jq .result.duchies[0]): d_yongxing, every county a vassal's
    return {"duchies": [{"duchy_id": 14324, "duchy_key": "d_yongxing", "partially_held": True,
                         "remaining_counties": [
        {"county_id": cid, "county_key": key, "holder_id": hid, "holder_top_liege_id": CUI,
         "holder_top_liege_independent": False, "is_independent": False, "is_kin": False,
         "is_allied": False, "is_truced": False, "already_at_war_opposing": False,
         "adjacent": True, "conquerable_now": False, "conquerable_now_source": "heuristic",
         "block_reason": "holder_not_independent"}
        for cid, key, hid in ((14331, "c_huayin", 12284), (14335, "c_tongzhou", 9260),
                              (14339, "c_yaozhou", 9355))]}]}


def test_C5_a_county_inside_the_actors_own_realm_is_no_target():
    gw = _gw(query_conquest_targets=_cui_conquest(),
             query_valid_war_targets={"action": "query_valid_war_targets", "actor_id": CUI, "targets": []})
    res = _run(R.plan_war_impl(gw, character_id=CUI))["result"]
    probed = [p.get("target_id") for a, p in gw.reads if a == "query_cb_validity_strict"]
    assert CUI not in probed, probed
    er = res["empty_reason"]
    assert er["gate"] != "target_not_independent", er
    assert er["measured"]["own_realm_counties"] == 3, er
    assert er["fix"] == {"retry_with": {"goal_scoped": False}}, er


# ── L18 round 2 (witness 1b): the claim gate holds with no cbs options read ────

def test_C6_a_claim_row_with_no_cbs_options_and_no_pressed_claim_is_excluded():
    """query_cbs_for_target answered no rows (emit_cbs_target_body with an empty
    `cbs`), so `chosen` is None and the claim rewrite never runs. The strict probe
    passed claim_cb; the conquest row carries no `pressed`. Before the fix the row
    went out with claim_pressed false and declare_war refused it claim_not_pressed."""
    gw = _gw(query_cbs_for_target=_cbs([]))
    res = _run(R.plan_war_impl(gw, character_id=CHARLES, goal_scoped=False, ally_free_only=False))["result"]
    assert not [t for t in res["targets"] if t["declare_args"]["cb"] == "claim_cb"], res["targets"]
    gates = [(e.get("blocked") or {}).get("gate") for e in res["excluded"]]
    assert "claim_not_pressed" in gates, res["excluded"]


def test_C7_CONTROL_a_pressed_claim_row_with_no_cbs_options_is_offered():
    """The enumerator's row says pressed: true (do_query_valid_war_targets r.key("pressed"));
    with no cbs options read it is still offered, claim_pressed true."""
    vt = {"action": "query_valid_war_targets", "actor_id": CHARLES, "targets": [
        {"target_char_id": LOTHAR, "target_name": "Lothar", "title_id": K_ITALY,
         "title_name": "Italy", "tier": 4, "tier_name": "kingdom", "cb_type": "claim",
         "pressed": True, "implicit": False, "target_independent": True,
         "target_imprisoned": False, "target_in_alliance": False, "target_in_truce": False,
         "already_at_war_opposing": False, "declarable": True}]}
    gw = _gw(query_valid_war_targets=vt, query_conquest_targets={"duchies": []},
             query_cbs_for_target=_cbs([]))
    res = _run(R.plan_war_impl(gw, character_id=CHARLES, goal_scoped=False, ally_free_only=False))["result"]
    (row,) = res["targets"]
    assert row["declare_args"]["cb"] == "claim_cb" and row["claim_pressed"] is True, row


def test_C8_every_offered_claim_row_is_pressed():
    """The property over the module's tests' shapes: no offered row needs a
    claimant with claim_pressed false (C1, C3, C6, C7 replies)."""
    shapes = [_gw(), _gw(query_cbs_for_target=_cbs([]))]
    for gw in shapes:
        res = _run(R.plan_war_impl(gw, character_id=CHARLES, goal_scoped=False, ally_free_only=False))["result"]
        for t in res["targets"]:
            if R.is_claim_cb_name(t["declare_args"]["cb"]):
                assert t["claim_pressed"] is True, t
