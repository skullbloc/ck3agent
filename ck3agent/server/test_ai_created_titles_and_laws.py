"""Rows from the sixth seated Ireland run, each row replayed from
its recorded reply.

The fixture (fixtures/ai_created_title_replies.json.gz, built by a
research-tree script) holds whole tool replies and step news recorded in that run; `line` is the record's line in
the evidence's calls file. The "recorded" tests read what the run's toolkit answered (they hold with or
without the rules); the others drive the code and fail without the rules.
"""
import asyncio
import gzip
import json
from pathlib import Path

import pytest

import declare_gates as DG
import news
import redesign as R
from test_ireland_walk_replies import Gw
from test_war_intents import WarGateway, _active, _war_row

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "ai_created_title_replies.json.gz", "rt").read())
ME = 31506
MOTHER = 27052


def run(c):
    return asyncio.run(c)


def env_of(name):
    r = LIVE[name]["reply"]
    return r["result"] if isinstance(r, dict) and "tool_name" in r else r


def news_of(name):
    return LIVE[name]["news"]


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    news._FABRICATING.clear()
    news._FAB_CHAPLAIN.clear()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()


def snap(date, **kw):
    base = dict(actor_id=ME, date=date, chr_sect={"gold": 0.0, "prestige": 0.0, "piety": 0.0},
                wars={"wars": []}, claims={"claims": []}, council={"council": []}, heir=None,
                schemes={"schemes": []}, held={"held_titles": []}, council_tasks=[], laws={"active_laws": []},
                alliances={"alliances": []})
    base.update(kw)
    return news.snapshot(**base)


def two_reads(gw, a, b):
    run(news.block(gw, 0, a, offers_served=False))
    return run(news.block(gw, 0, b, offers_served=False))


# ── P2 / P1(a): the formable row said cannot_afford; the game's check passed and its AI created it ──

def meath_gw(domain_size=5, domain_limit=4, tier=2, engine_valid=True, have=29.6):
    return Gw({
        "query_dejure_progress": {"character_id": ME, "held": 2, "total": 2, "fraction": 1.0},
        "query_create_title_cost": {"success": True, "engine_valid": engine_valid,
                                    "engine_reasons": [] if engine_valid else ["Earl Murchad does not have gold 212"],
                                    "price": {"known": True, "currency": "gold", "cost": 212.5, "have": have,
                                              "affordable": have >= 212.5}},
        "query_realm": {"character_id": ME, "has_realm": True, "domain_size": domain_size,
                        "domain_limit": domain_limit},
        "query_character": {"id": ME, "name": "Murchad", "alive": True, "ruler_tier": tier, "gold": have},
    }, date="1074.11.11")


def test_p2_recorded_cannot_afford_beside_the_games_check_met():
    res = env_of("formable_1074_11_11")["result"]
    (meath,) = [r for r in res["unavailable"] if r["title_key"] == "d_meath"]
    reqs = {q["requirement"]: q for q in meath["requirements"]}
    assert reqs["price"]["met"] is False and reqs["price"]["have"] == 29.6
    assert reqs["game_check"]["met"] is True and reqs["game_check"]["reasons"] == []
    assert meath["blocked"]["gate"] == "cannot_afford" and "game_ai_may_create" not in meath
    # the same read at 1066.09.15 (one county) had the game's gold test in its words
    nt = env_of("observe_1066_09_15")["result"]["de_jure"]["next_title"]
    gc = [q for q in nt["requirements"] if q["requirement"] == "game_check"][0]
    assert gc["met"] is False and any("does not have gold 212" in x for x in gc["reasons"])
    # and the next step's news: created by the game's AI into debt, no lever named
    g = news_of("news_1075_02_09")["purse"]["gold"]
    row = [e for e in g["in_this_step"] if "d_meath" in e["what"]][0]
    assert g["now"] == -181.63 and "lever" not in row and "decided_by" not in row


def test_p2_the_row_says_the_games_ai_may_create_it_into_debt():
    env = run(R.do_impl(meath_gw(), action_class="formable_titles", character_id=ME, candidates=["d_meath"]))
    (row,) = [r for r in env["result"]["unavailable"] if r["title_key"] == "d_meath"]
    ai = row["game_ai_may_create"]
    assert ai["matches"] is True and ai["conditions"] == {
        "title_is_duchy": True, "your_highest_tier_is_county": True, "over_domain_limit": True}
    assert ai["domain"] == {"counties_you_hold": 5, "limit": 4}
    assert ai["debt_if_created"] == -182.9
    assert "into debt" in ai["why"] and "gold_reserve does not" in ai["lever"]["no_veto"]
    # a live read: a count's grant of a county is refused inside the realm; no grant call is offered
    assert "call" not in ai["lever"] and "leaves the realm" in ai["lever"]["what"]
    assert ai["lever"]["mint_yourself"]["args"]["args"]["title_key"] == "d_meath"


def test_p2_within_the_domain_limit_the_rule_does_not_match_and_says_so():
    env = run(R.do_impl(meath_gw(domain_size=4), action_class="formable_titles", character_id=ME,
                        candidates=["d_meath"]))
    (row,) = [r for r in env["result"]["unavailable"] if r["title_key"] == "d_meath"]
    assert row["game_ai_may_create"]["matches"] is False and "not read" in row["game_ai_may_create"]["note"]


def test_p2_no_row_when_the_games_check_asks_for_the_gold():
    env = run(R.do_impl(meath_gw(domain_size=1, engine_valid=False), action_class="formable_titles",
                        character_id=ME, candidates=["d_meath"]))
    (row,) = [r for r in env["result"]["unavailable"] if r["title_key"] == "d_meath"]
    assert "game_ai_may_create" not in row


def test_p2_the_rule_is_the_decompile_and_the_runs_numbers():
    # 1070.12.12 inspect economy: 3 counties against a limit of 4 (the limit the rule compares)
    s = json.dumps(env_of("economy_1070_12_12"))
    assert '"domain_limit": 4' in s and '"domain_size": 3' in s
    assert "domain limit" in news.AI_CREATE_RULE and "duchy" in news.AI_CREATE_RULE


# ── P1(c): every AI-spent row names who decided and its lever ──────────────────

def test_p1_the_ai_created_title_row_names_the_lever_and_its_prestige():
    news.note_creation_price(ME, "d_meath", "gold", 212.5, "1074.11.11")
    a = snap("1074.11.11", chr_sect={"gold": 29.6, "prestige": 150.0, "piety": 700.0},
             held={"held_titles": [{"title_id": 463, "key": "c_dublin"}]})
    b = snap("1075.02.09", chr_sect={"gold": -181.63, "prestige": 454.37, "piety": 700.0},
             held={"held_titles": [{"title_id": 463, "key": "c_dublin"}, {"title_id": 900, "key": "d_meath"}]})
    out = two_reads(Gw({}), a, b)
    (g,) = [e for e in out["purse"]["gold"]["in_this_step"] if "d_meath" in e["what"]]
    assert g["expected"] == -212.5 and g["decided_by"].startswith("the game's AI for your ruler")
    assert g["lever"]["no_veto"] == news.AI_CREATE_NO_VETO
    (p,) = [e for e in out["purse"]["prestige"]["in_this_step"] if "d_meath" in e["what"]]
    assert p["expected"] == 300
    assert abs(out["purse"]["prestige"].get("unexplained", 0)) < 10


def test_p1_the_first_observe_names_what_the_ai_spends_and_each_lever(monkeypatch):
    # the whole prose when the notes are off (with them on, the turn-one notes carry it; test_map_digest_and_turn_one_notes)
    import notes as _notes
    monkeypatch.setattr(_notes, "SERVED", None)
    out = run(news.block(Gw({}), 0, snap("1066.09.15"), offers_served=False))
    ai = out["ai_spends"]
    assert ai["claim_letters"]["lever"]["args"]["intent"] == "event_rule"
    assert ai["title_creation"]["lever"]["no_veto"] == news.AI_CREATE_NO_VETO
    # The veto is the lever; the reserve is named as not one (a recorded game 1089: a hire under a reserve of 500)
    assert ai["buildings_and_mercenaries"]["lever"]["args"]["intent"] == "ai_veto"
    assert "gold_reserve is not this lever" in ai["buildings_and_mercenaries"]["not_gold_reserve"]
    # Laws have a lever now (ai_veto what=law); ally calls still none
    assert ai["laws"]["lever"]["args"]["intent"] == "ai_veto" and ai["allies_called_into_your_wars"]["lever"]["args"]["args"]["what"] == "ally_call"
    assert "ai_spends" in news.compact(out)


def test_p1_a_law_the_ai_passed_says_so_and_names_the_veto():
    a = snap("1066.09.15", chr_sect={"gold": 84.0, "prestige": 300.0, "piety": 0.0})
    b = snap("1067.09.15", chr_sect={"gold": 114.0, "prestige": 110.9, "piety": 0.0},
             laws={"active_laws": [{"name": "crown_authority_1"}]})
    out = two_reads(Gw({}), a, b)
    (row,) = [e for e in out["purse"]["prestige"]["in_this_step"] if "crown_authority_1" in e["what"]]
    # The lever is ai_veto what=law
    assert row["decided_by"].startswith("the game's AI") and row["lever"] == news.ai_veto_call("law")


# ── P3: the prestige ledger ─────────────────────────────────────────────────

def test_p3_recorded_price_rows_had_one_sentence_and_the_falls_no_cause():
    s = json.dumps(env_of("cbs_1082_03_03"))
    assert "x2 your legitimacy 0.0 at your tier" in s and "cost_multipliers" not in s
    for name in ("news_1089_08_22", "news_1090_02_10"):
        p = news_of(name)["purse"]["prestige"]
        assert p["cause"].startswith("unknown") and p["change"] < -100


def test_p3_the_claim_price_carries_each_multiplier_and_the_legitimacy_level():
    opt = {"cb": "claim_cb", "valid": True, "scope": "county", "cb_title_id": 485}
    purse = {"gold": 10.0, "prestige": 548.0, "piety": 0.0,
             "me": {"id": ME, "age": 56, "is_female": False, "faith_key": "catholic", "dynasty_id": 1,
                    "ruler_tier": 3, "legitimacy": 0.0}, "claims": {485: "unpressed"}}
    defender = {"id": 32421, "age": 40, "is_female": False, "faith_key": "insular_celtic", "dynasty_id": 2,
                "ruler_tier": 2}
    c = DG.cost_of(opt, facts=DG.facts_for(opt, purse, defender))
    assert c["prestige"] == 312.5
    names = [m.get("factor") for m in c["multipliers"][1:]]
    assert c["multipliers"][0] == {"base": 100, "pressed": False}
    assert names == ["different_faith", "defender_lower_tier", "legitimacy"]
    assert c["legitimacy"]["level"] == [1, 1] and c["legitimacy"]["x"] == [2.0, 2.0]
    assert c["legitimacy"]["next_level_at"][0] > 0
    v, allopts = R._shape_cb_options({"cbs": [{"cb_key": "claim_cb", "valid": True, "scope": "county",
                                                "cb_title_id": 485, "target_mode": 6}]},
                                     target_id=32421, purse=purse, defender=defender)
    o = allopts[0]
    assert o["cost_prestige"] == 312.5 and o["cost_multipliers"] and o["cost_legitimacy"]["x"] == [2.0, 2.0]
    assert R._affordability(o)["cost_multipliers"] == o["cost_multipliers"]


def test_p3_news_names_a_legitimacy_move_and_the_price_factor_it_moved():
    a = snap("1074.11.11", chr_sect={"gold": 29.6, "prestige": 150.0, "piety": 0.0, "ruler_tier": 2},
             legitimacy=309.0)
    b = snap("1075.02.09", chr_sect={"gold": 29.6, "prestige": 150.0, "piety": 0.0, "ruler_tier": 3},
             legitimacy=0.0)
    out = two_reads(Gw({}), a, b)
    lg = out["purse"]["legitimacy"]
    assert lg["tier"] == {"was": "county", "now": "duchy"} and lg["change"] == -309.0
    assert lg["claim_war_price_factor"]["now"] == [2.0, 2.0]
    assert lg["claim_war_price_factor"]["was"] == list(DG.legitimacy_factor(309.0, 2))


def test_p3_your_own_decision_whose_cost_comes_later_is_named_in_the_next_step():
    news.note_act(ME, "1089.08.22", "take_decision stress_loss_drunkard_decision", cost_known=False,
                  unpriced_kind="deferred")
    a = snap("1089.08.22", chr_sect={"gold": 115.0, "prestige": -65.91, "piety": 273.0})
    b = snap("1090.02.10", chr_sect={"gold": 54.0, "prestige": -205.83, "piety": 280.0})
    out = two_reads(Gw({}), a, b)
    rows = out["purse"]["prestige"]["in_this_step"]
    assert any("stress_loss_drunkard_decision" in r["what"] for r in rows)


def test_p3_a_decision_with_no_purse_move_is_noted_as_unpriced():
    gw = Gw({"query_character": {"id": ME, "gold": 100.0, "prestige": 50.0, "piety": 10.0}})
    before = {"id": ME, "gold": 100.0, "prestige": 50.0, "piety": 10.0}
    run(R._note_own_spend(gw, "take_decision", {"decision": "stress_loss_drunkard_decision"}, before,
                          {"verdict": "applied", "result": {}}))
    (act,) = news._ACTS[ME]
    assert act["cost_known"] is False and act["unpriced_kind"] == "deferred"


# ── P4: claims: their kind, the ruler's age and health, whether the war is declarable ──────

def test_p4_recorded_claim_rows_carried_no_kind_age_or_gate():
    for c in news_of("news_1091_05_10")["claims_gained"]:
        assert c["type"] == "unpressed" and "kind" not in c and "war" not in c
        assert "no casus belli on this title is declarable" in c["next"]["why"]


def claim_gw(prestige=-141.16, gold=133.92):
    def cbs(p):
        return {"target_id": p.get("target_id"), "prestige": prestige, "piety": 290.0,
                "cbs": [{"cb_key": "claim_cb", "valid": True, "scope": "county", "cb_title_id": 496,
                         "target_mode": 6}], "target_state": {"in_truce": False}}
    return Gw({"query_cbs_for_targets": {"error": "unknown_action"}, "query_cbs_for_target": cbs,
               "query_character": lambda p: {"id": p.get("character_id"), "gold": gold, "prestige": prestige,
                                             "piety": 290.0, "age": 36, "ruler_tier": 3,
                                             "faith_key": "catholic", "dynasty_id": 1, "is_female": False},
               "get_ai_evaluations": {"diplomacy": {"legitimacy": 0.0}},
               "query_armies": {"armies": []},
               "query_claims": {"claims": [{"title_id": 496, "type": "unpressed"}]}}, date="1091.05.10")


def test_p4_a_new_claim_row_says_its_kind_the_rulers_age_and_the_wars_gates():
    a = snap("1091.02.10", chr_sect={"gold": 109.26, "prestige": -154.64, "piety": 288.0, "age": 36,
                                     "health": 2.81})
    b = snap("1091.05.10", chr_sect={"gold": 133.92, "prestige": -141.16, "piety": 290.0, "age": 36,
                                     "health": 2.81},
             claims={"claims": [{"title_id": 496, "title_name": "Uí Mháine", "type": "unpressed",
                                 "holder_id": 34580, "war_target_id": 34580}]})
    out = two_reads(claim_gw(), a, b)
    (c,) = out["claims_gained"]
    assert c["kind"] == {"type": "unpressed", "inheritable": False,
                         "at_your_death": "lost (an unpressed claim does not pass to your heir)"}
    assert c["holder_is_your_ruler"] == {"age": 36, "health": 2.81}
    assert c["war"]["declarable_now"] is False
    (g,) = [x for x in c["war"]["gates"] if x["condition"] == "prestige"]
    assert g["have"] == -141.16 and g["missing"] > 141 and c["war"]["price_prestige"] > 0


def test_p4_before_the_letter_the_fabrication_row_puts_its_price_beside_the_gates():
    s = snap("1090.02.10", chr_sect={"gold": 54.65, "prestige": -205.83, "piety": 0.0, "age": 35,
                                     "health": 2.9, "ruler_tier": 3},
             council={"council": [{"position_key": "councillor_court_chaplain", "holder_id": 56090,
                                   "current_task": "task_fabricate_claim", "skills": {"learning": 8}}]},
             legitimacy=0.0)
    f = news.letter_facts(s)
    assert f["letter_price"]["gold"] == 96.0 and f["letter_price"]["gold_after"] == -41.35
    conds = {g["condition"]: g for g in f["war_gates_now"]}
    assert conds["in_debt"]["gold_after_letter"] == -41.35
    assert conds["prestige"]["price_at_least"] == 200.0 and conds["prestige"]["missing"] == 405.83


def test_p4_claims_lost_at_a_succession_say_they_died_with_their_holder():
    a = snap("1087.05.22", claims={"claims": [{"title_id": 474, "title_name": "Ulster", "type": "unpressed"}]})
    b = snap("1087.08.22", actor_id=37528, claims={"claims": []})
    out = two_reads(Gw({}), a, b)
    (c,) = out["claims_lost"]
    assert c["lost_at_succession"] == {"ruler_before": ME, "why": "unpressed: it died with its holder"}
    rec = news_of("news_1087_08_22")
    assert rec["ruler_changed"]["to"] == 37528 and len(rec["claims_lost"]) == 2


# ── P5: call_ally refused with no reason ─────────────────────────────────────

def test_p5_recorded_the_mother_was_an_ally_and_the_refusal_gave_no_words():
    al = json.dumps(env_of("alliances_1071_07_19"))
    assert f'"ally_id": {MOTHER}' in al
    for name in ("call_ally_1071_07_19", "call_ally_1073_06_01", "call_ally_1073_08_30"):
        b = env_of(name)["blocked"]
        assert b["raw_reason"] == "interaction_not_valid_for_target" and b["detail"] == R.NO_GAME_DETAIL
        assert "game_check" not in env_of(name)["result"]


def test_p5_the_refusal_carries_the_games_check_with_the_war_and_the_conditions():
    rid = "si_100_1_1"
    gw = WarGateway(replies={
        "query_wars": _active(_war_row(atk=R._as_int(100), dfn=200)),
        "query_intent_results": {"results": [{"intent_id": rid, "status": "rejected",
                                              "rejection_reason": "interaction_not_valid_for_target"}]},
        "query_interaction_check": lambda p: {"success": True, "result": {"results": [
            {"target_id": p["target_id"], "sendable": False, "refused_by": "interaction_not_valid_for_target",
             "engine_reason": "Diarmait is already fighting in a war against a participant"}]}},
        "query_alliances": {"alliances": [{"ally_id": 300}]},
        "query_character": lambda p: {"id": p["character_id"], "liege_id": 0, "house_id": 7 if p["character_id"] == 100 else 9,
                                      "gold": 50, "prestige": 300, "age": 30},
    })
    env = run(R.do_impl(gw, intent="call_ally", character_id=100, args={"war_id": 16777282, "ally_id": 300}))
    assert env["verdict"] == "rejected"
    chk = env["result"]["game_check"]
    assert chk["engine_reason"].startswith("Diarmait is already fighting")
    assert chk["in_your_alliances"] is True and chk["your_liege"] is False and chk["same_house"] is False
    assert env["blocked"]["detail"] == chk["engine_reason"]
    assert env["result"]["ai_calls_for_you"] == R.CALL_ALLY_AI_NO_LEVER
    (sent,) = [p for a, p in gw.reads if a == "query_interaction_check"]
    assert sent["war_id"] == 16777282 and sent["interaction_type"] == "call_ally_interaction"


def test_p5_the_bridge_check_takes_the_war_as_send_interaction_does():
    src = (HERE / ".." / ".." / "bridge" / "src" / "action_handlers.cpp").read_text()
    body = src[src.index("bool do_query_interaction_check("):src.index("bool do_send_decision(")]
    assert 'json_has_key(params_json, "war_id")' in body and "intent.target_war_id" in body


# ── P6: three claims priced as letters while gold rose ─────────────────────────

def test_p6_recorded_three_claims_priced_while_the_fabrication_ran_on():
    n = news_of("news_1091_05_10")
    g = n["purse"]["gold"]
    priced = [e for e in g["in_this_step"] if e.get("expected") == -105.0]
    assert len(priced) == 3 and g["change"] == 24.66 and g["unexplained"] == 323.75
    f = n["fabrication"]
    assert f["running"] is True and f["since"] == "1090.02.10" and f["target_province_id"] == 8745


def test_p6_claims_that_are_not_the_chaplains_letter_are_not_priced_as_one():
    tasks = [{"task_name": "task_fabricate_claim", "target_province_id": 8745, "councillor_id": 44208}]
    claims = [{"title_id": t, "title_name": n, "type": "unpressed", "holder_id": h, "war_target_id": h}
              for t, n, h in ((496, "Uí Mháine", 34580), (524, "Ennis", 48828), (527, "Ormond", 48828))]
    income = {"total_monthly": 5.44}
    a = snap("1091.02.10", chr_sect={"gold": 109.26, "prestige": -154.64, "piety": 288.0,
                                     "income_breakdown": income}, council_tasks=tasks)
    b = snap("1091.05.10", chr_sect={"gold": 133.92, "prestige": -141.16, "piety": 290.0,
                                     "income_breakdown": income}, council_tasks=tasks, claims={"claims": claims})
    out = two_reads(claim_gw(), a, b)
    g = out["purse"]["gold"]
    assert not [e for e in g["in_this_step"] if "expected" in e]
    assert all("not by your chaplain's letter" in e["what"] for e in g["in_this_step"])
    assert "province 8745" in g["in_this_step"][0]["basis"]
    assert abs(g.get("unexplained", 0)) < 10


def test_p6_a_claim_the_fabrication_ended_with_is_still_priced():
    a = snap("1067.09.15", chr_sect={"gold": 113.9, "prestige": 0.0, "piety": 0.0},
             council_tasks=[{"task_name": "task_fabricate_claim", "target_province_id": 1}],
             council={"council": [{"position_key": "councillor_court_chaplain", "holder_id": 5, "skills": {"learning": 8},
                                   "current_task": "task_fabricate_claim"}]})
    b = snap("1067.11.29", chr_sect={"gold": 34.74, "prestige": 0.0, "piety": 0.0}, council_tasks=[],
             claims={"claims": [{"title_id": 469, "title_name": "Athlone", "type": "unpressed"}]})
    out = two_reads(Gw({}), a, b)
    (e,) = out["purse"]["gold"]["in_this_step"]
    assert "expected" in e and e["what"] == "claim on Athlone arrived"
