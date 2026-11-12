"""v4fix B3: plan_war and cbs offer only the wars declare_war's own pre-check passes.

Set v3-registered-2: all six declare_war refusals in the set followed a plan_war
row (and cbs in three) that read "Independent, declarable per strict-CB inner
gate" / ready_to_declare true. The transcripts show why for five of them: the
dukes called plan_war() and cbs() WITHOUT character_id, and the casus-belli reads
(query_cb_validity_strict, query_cbs_for_target(s)) fall back to the bridge's
GLOBAL controlled primary (query_handlers.cpp `g_controlled_primary.load` in each
handler), the king, while declare_war resolves the seat's own ruler first and
checks as the duke: s1 D1 and D2 make_tributary_cb against 10735 and 12005
(engine_cb_validity_check_failed), s6 D2 minor_religious_war against 11232
(faith_gate), s1 D2 claim_cb against 13356 with claimant 12015, the king (the
cbs emitter writes the read's actor as claimant_id; the dispatch refused
claimant_not_in_court). Besides that, s3 D5 (Hugo 13586, plan_war with his own
id) was offered claim_cb on his liege Charles 12015 as its top row.

  P1  plan_war() and cbs() with no character_id read every casus belli for the
      bound ruler's id, and a row offered is one declare_war's pre-check passes
      on the same game state (s1 D1's tributary row)
  P2  the CB a row offers is the CB the pre-check passed: a cbs option the
      pre-check refused is not offered under another CB's verdict
  P3  a war on the actor's own liege is never offered (s3 D5)
  P4  a claim whose claimant is not in the actor's court is never offered (s1
      D2); CONTROL: a claimant whose liege is the actor is
  P5  declare_war refuses on the same pre-check: a claim CB the strict read
      marks declarable false is refused before anything is sent

Every reply carries the keys its EMITTER writes (bridge/src/query_handlers.cpp):
build_char_profile (`id`, `alive`, `age`, `stress`, `liege_id`, `top_liege_id`),
do_query_valid_war_targets (`targets[]`: `target_char_id`, `target_name`,
`title_id`, `cb_type`, `pressed`, `declarable`), do_query_conquest_targets
(`duchies[].remaining_counties[]`), do_query_cb_validity_strict (`results[]`:
`cb_index`, `cb_name`, `target_mode`, `outer_status`, `inner_status`,
`declarable`, `cull_reason`; `gates_unchecked`), emit_cbs_target_body (`cbs[]`:
`cb_key`, `family`, `valid`, `declarable`, `scope`, `target_mode`,
`cb_title_id`, `claimant_id`, `needs_cb_title_id`, costs).
"""
import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
R = pytest.importorskip("redesign")

KING, DUKE, TARGET = 12015, 12137, 10735
HUGO, LOTHAR, K_ITALY, C_IDF = 13586, 13356, 2189, 7054
GATES_UNCHECKED = ["per_cb_allowed_against_character", "valid_to_start",
                   "ai_only_against_neighbors", "max_ai_diplo_distance_to_title"]


@pytest.fixture(autouse=True)
def _instant_waits(monkeypatch):
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)


class GW:
    """Reads answer from `replies` (value or callable on the params); every read
    and write is recorded."""

    def __init__(self, replies):
        self.replies = dict(replies)
        self.reads, self.writes = [], []
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        r = self.replies.get(action, {})
        return r(params or {}) if callable(r) else r

    async def confirmed_write(self, action, params, *, readback, deadline_s=12.0):
        self.writes.append((action, dict(params or {})))
        return {"refused": "engine_refused", "raw": {}}

    def game(self):
        return {"tick": 0, "date": "0873.11.29", "paused": True}


def _run(coro):
    return asyncio.run(coro)


def _people(bound, lieges):
    """query_character: character_id 0 resolves to the seat's own ruler (the
    bridge's resolve_character, session-aware); `lieges` maps id -> (liege_id,
    top_liege_id)."""
    def character(p):
        cid = p.get("character_id") or bound
        liege, top = lieges.get(cid, (0, cid))
        return {"id": cid, "alive": True, "age": 40, "stress": 0,
                "liege_id": liege, "top_liege_id": top}
    return character


def _strict_row(cb, declarable, cull=None, mode="none"):
    row = {"cb_index": 7, "cb_name": cb, "target_mode": mode, "outer_status": "pass",
           "inner_status": "pass" if declarable or cull == "claim_not_pressed" else "fail",
           "declarable": declarable}
    if cull:
        row["cull_reason"] = cull
    return row


def _strict(*rows):
    return {"action": "query_cb_validity_strict", "status": "ok",
            "gates_unchecked": GATES_UNCHECKED, "results": list(rows),
            "declarable_count": sum(1 for r in rows if r["declarable"])}


def _cbs_row(cb, valid, *, family="other", scope="realm", mode=0, title=0, claimant=None):
    row = {"cb_key": cb, "family": family, "valid": valid, "declarable": valid, "scope": scope,
           "grants": "", "requirements_unmet": [] if valid else ["engine_cb_validity_check_failed"],
           "notes": [], "cb_title_id": title, "needs_cb_title_id": mode != 0,
           "cost_prestige": 50, "cost_piety": 0, "affordable": True,
           "outer_status": "pass", "inner_status": "pass" if valid else "fail",
           "cb_index": 7, "target_mode": mode}
    if claimant:
        row["claimant_id"] = claimant
    return row


def _cbs(*rows):
    return {"status": "ok", "target_state": {"independent": True, "in_truce": False}, "cbs": list(rows)}


def _conquest_county(holder):
    return {"duchies": [{"duchy_id": 1, "duchy_key": "d_x", "remaining_counties": [
        {"county_id": 7311, "county_key": "c_ipuskoa", "holder_id": holder,
         "holder_top_liege_id": holder, "holder_name": "Gartzia", "is_allied": False,
         "is_truced": False, "adjacent": True, "conquerable_now": False}]}]}


def _base(**over):
    replies = {
        "query_valid_war_targets": {"action": "query_valid_war_targets", "targets": []},
        "query_conquest_targets": {"duchies": []},
        "query_cbs_for_targets": {"error": "unknown_action"},
        "query_war_stance": {"error": "unknown_action"},
        "query_alliances": lambda p: {"own_military_strength": 1700, "total_callable_strength": 1700},
        "get_active_wars": {"wars": []},
    }
    replies.update(over)
    return GW(replies)


# ── P1 ─────────────────────────────────────────────────────────────────────────

def _s1_d1():
    """s1 D1: the casus-belli reads answer for whoever they are made for. For the
    king (actor 0 -> the bridge's global primary) make_tributary_cb passes; for the
    duke the engine's check fails."""
    def strict(p):
        ok = p.get("actor_id") in (0, KING)
        return _strict(_strict_row("make_tributary_cb", ok,
                                   None if ok else "engine_cb_validity_check_failed"))

    def cbs(p):
        return _cbs(_cbs_row("make_tributary_cb", p.get("attacker_id") in (0, KING)))
    return _base(query_character=_people(DUKE, {DUKE: (KING, KING)}),
                 query_conquest_targets=_conquest_county(TARGET),
                 query_cb_validity_strict=strict, query_cbs_for_target=cbs)


def test_plan_war_and_cbs_read_the_casus_belli_for_the_bound_ruler():
    """RED on 8cef520a: plan_war() sent actor_id 0 and cbs() attacker_id 0, so
    both answered for the king and offered make_tributary_cb, which declare_war
    (checking as the duke) refused engine_cb_validity_check_failed."""
    gw = _s1_d1()
    env = _run(R.plan_war_impl(gw, character_id=0))
    cb_reads = [(a, p) for a, p in gw.reads
                if a in ("query_cb_validity_strict", "query_cbs_for_target", "query_cbs_for_targets")]
    assert cb_reads, gw.reads
    for a, p in cb_reads:
        assert (p.get("actor_id") or p.get("attacker_id")) == DUKE, (a, p)
    rows = env["result"]["targets"]
    for row in rows:      # every row offered passes declare_war on the same state
        d = _run(R.declare_war_impl(gw, character_id=0, declare_args=row["declare_args"]))
        assert (d.get("blocked") or {}).get("gate") != "cb_invalid_for_target", (row, d)
    assert rows == [], rows
    gates = [(x.get("blocked") or {}).get("raw_reason") for x in env["result"].get("excluded") or []]
    assert "engine_cb_validity_check_failed" in gates or any(
        "engine_cb_validity_check_failed" in str(x) for x in env["result"].get("excluded") or []), gates

    gw = _s1_d1()
    res = _run(R.cbs_impl(gw, target_id=TARGET, character_id=0))["result"]
    reads = [p for a, p in gw.reads if a == "query_cbs_for_target"]
    assert reads and all(p["attacker_id"] == DUKE for p in reads), reads
    assert res["valid_options"] == [] and res["best"] is None, res


def test_a_cb_read_with_no_actor_resolved_is_refused():
    """With no id asked for and none read for the bound ruler, no casus-belli read
    is sent (it would answer for another seat's ruler)."""
    gw = _base(query_character=lambda p: {"alive": True})
    env = _run(R.plan_war_impl(gw, character_id=0))
    assert env["ok"] is False and env["blocked"]["gate"] == "actor_unresolved", env
    assert not [a for a, _ in gw.reads if a.startswith("query_cb")], gw.reads


# ── P2 ─────────────────────────────────────────────────────────────────────────

def test_the_cb_offered_is_the_cb_the_precheck_passed():
    """The strict read passes county_conquest_cb and refuses make_tributary_cb; the
    cbs read (taken separately) lists only make_tributary_cb as valid. RED on
    8cef520a: plan_war checked the strict read's FIRST declarable CB and then
    offered the widest cbs option, make_tributary_cb, as "declarable per
    strict-CB inner gate"."""
    gw = _base(query_character=_people(DUKE, {DUKE: (KING, KING)}),
               query_conquest_targets=_conquest_county(TARGET),
               query_cb_validity_strict=_strict(
                   _strict_row("county_conquest_cb", True, mode="title"),
                   _strict_row("make_tributary_cb", False, "engine_cb_validity_check_failed")),
               query_cbs_for_target=_cbs(_cbs_row("make_tributary_cb", True)))
    env = _run(R.plan_war_impl(gw, character_id=DUKE))
    offered = [r["declare_args"]["cb"] for r in env["result"]["targets"]]
    assert "make_tributary_cb" not in offered, env["result"]["targets"]
    for row in env["result"]["targets"]:
        assert R.cb_precheck(_strict(_strict_row("county_conquest_cb", True)),
                             row["declare_args"]["cb"])["passed"] is True


def test_a_row_offered_says_what_the_precheck_covers():
    """CONTROL (the fix does not empty plan_war): the duke's own read passes
    make_tributary_cb, the row is offered, and it carries the pre-check's date and
    the gates the strict read says it does not evaluate; no row text claims more."""
    gw = _base(query_character=_people(DUKE, {DUKE: (KING, KING)}),
               query_conquest_targets=_conquest_county(TARGET),
               query_cb_validity_strict=_strict(_strict_row("make_tributary_cb", True)),
               query_cbs_for_target=_cbs(_cbs_row("make_tributary_cb", True)))
    rows = _run(R.plan_war_impl(gw, character_id=0))["result"]["targets"]
    assert [r["declare_args"]["cb"] for r in rows] == ["make_tributary_cb"], rows
    row = rows[0]
    assert row["precheck"]["passed"] is True and row["precheck"]["as_of"] == "0873.11.29"
    assert row["precheck"]["not_checked"] == GATES_UNCHECKED
    assert row["precheck_by_cb"] == {"make_tributary_cb": True}
    assert "ready_to_declare_by_cb" not in row
    assert "declarable per strict-CB inner gate" not in row["notes"], row["notes"]


# ── P3 ─────────────────────────────────────────────────────────────────────────

def test_a_war_on_the_actors_own_liege_is_never_offered():
    """s3 D5: Hugo 13586 (liege Charles 12015) was offered claim_cb on Charles for
    c_ile_de_france as plan_war's top row. RED on 8cef520a: offered."""
    vt = {"action": "query_valid_war_targets", "actor_id": HUGO, "targets": [
        {"target_char_id": KING, "target_name": "Charles", "title_id": C_IDF,
         "cb_type": "claim", "pressed": True, "implicit": False, "declarable": True}]}
    gw = _base(query_character=_people(HUGO, {HUGO: (KING, KING)}),
               query_valid_war_targets=vt,
               query_cb_validity_strict=_strict(_strict_row("claim_cb", True, mode="claim")),
               query_cbs_for_target=_cbs(_cbs_row("claim_cb", True, family="claim", scope="county",
                                                  mode=6, title=C_IDF, claimant=HUGO)))
    res = _run(R.plan_war_impl(gw, character_id=HUGO, goal_scoped=False))["result"]
    assert [r["declare_args"]["target_id"] for r in res["targets"]] == [], res["targets"]
    blocked = [x["blocked"] for x in res.get("excluded") or []] + [res.get("empty_reason") or {}]
    assert any(b.get("raw_reason") == "target_is_own_liege" for b in blocked if b), blocked


# ── P4 ─────────────────────────────────────────────────────────────────────────

def _claim_gw(claimant, lieges):
    return _base(query_character=_people(DUKE, lieges),
                 query_conquest_targets=_conquest_county(LOTHAR),
                 query_cb_validity_strict=_strict(_strict_row("claim_cb", True, mode="claim")),
                 query_cbs_for_target=_cbs(_cbs_row("claim_cb", True, family="claim", scope="county",
                                                    mode=6, title=K_ITALY, claimant=claimant)))


def test_a_claimant_outside_the_actors_court_is_never_offered():
    """s1 D2: the row named the king 12015 (independent: top_liege_id his own id)
    as claimant; the dispatch refused claimant_not_in_court. RED on 8cef520a:
    offered."""
    res = _run(R.plan_war_impl(_claim_gw(KING, {DUKE: (KING, KING)}), character_id=DUKE,
                               ally_free_only=False))["result"]
    assert res["targets"] == [], res["targets"]
    raws = [(x.get("blocked") or {}).get("raw_reason") for x in res.get("excluded") or []]
    assert "claimant_not_in_court" in raws, res.get("excluded")


def test_a_claimant_in_the_actors_court_is_offered():
    """CONTROL: a courtier 777 whose liege is the duke presses through him."""
    res = _run(R.plan_war_impl(_claim_gw(777, {DUKE: (KING, KING), 777: (DUKE, KING)}),
                               character_id=DUKE, ally_free_only=False))["result"]
    assert [r["declare_args"]["claimant_id"] for r in res["targets"]] == [777], res


# ── P5 ─────────────────────────────────────────────────────────────────────────

def test_declare_war_refuses_on_the_same_precheck_before_sending():
    """The strict read marks claim_cb declarable false (claim_not_pressed) with its
    engine verdicts passing. RED on 8cef520a: declare_war passed it (its rule read
    outer+inner pass over `declarable`) and sent the war; now cb_precheck refuses
    it, the same answer plan_war gives, and nothing is sent."""
    gw = _base(query_character=_people(DUKE, {DUKE: (KING, KING)}),
               query_cb_validity_strict=_strict(
                   _strict_row("claim_cb", False, "claim_not_pressed", mode="claim")))
    env = _run(R.declare_war_impl(gw, character_id=DUKE, target_id=LOTHAR, cb="claim_cb",
                                  cb_title_id=K_ITALY))
    assert env["ok"] is False and env["blocked"]["gate"] == "claim_not_pressed", env
    assert gw.writes == [], gw.writes
    assert R.cb_precheck(_strict(_strict_row("claim_cb", False, "claim_not_pressed")),
                         "claim_cb") == {"passed": False, "reason": "claim_not_pressed"}
    assert R.cb_precheck(_strict(), "claim_cb") == {"passed": None, "reason": "cb_not_in_precheck"}
