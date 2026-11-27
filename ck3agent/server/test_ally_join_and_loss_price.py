"""The toolkit reads three 30-year seated games of the Dublin seat were lost on,
each reproduced from the recorded reply of the latest game, then
fixed.

The fixture (fixtures/ally_join_replies.json.gz) holds whole tool replies recorded in that run,
keyed by what they are; `line` is the record's line in the evidence's calls file. A value the
recording does not hold (an opinion, a defender's culture) is written into the test as such,
never into the fixture. Rules read from the game's own files are checked against the install
when one is present (CK3_GAME_DIR, or the default Steam path) and skipped otherwise.
"""
import asyncio
import gzip
import json
import os
import re
from pathlib import Path

import pytest

import declare_gates as DG
import news
import redesign as R
import tools as T
from test_ireland_walk_replies import Gw

HERE = Path(__file__).resolve().parent
BRIDGE = HERE / ".." / ".." / "bridge"
OFFSETS = HERE / ".." / ".." / "re" / "offsets" / "ck3_offsets.h"
LIVE = json.loads(gzip.open(HERE / "fixtures" / "ally_join_replies.json.gz", "rt").read())
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
ME, ATHLONE, FRANCE, ULSTER, DOMNALL, AILECH_LORD = 31506, 29585, 37067, 31266, 36124, 64351


def run(c):
    return asyncio.run(c)


def reply(name):
    """The tool's own envelope of a recorded call."""
    r = LIVE[name]["reply"]
    return r["result"] if isinstance(r, dict) and "result" in r and "tool_name" in r else r


def news_of(name):
    return LIVE[name]["reply"]["observation"]["world"]["news"]


def game_file(rel: str) -> str:
    p = GAME / rel
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    return p.read_text(encoding="utf-8-sig", errors="replace")


def block_of(text: str, name: str) -> str:
    """The top-level `name = { ... }` block of a game file."""
    i = text.index(f"\n{name} = {{") if not text.startswith(f"{name} = {{") else 0
    depth, j = 0, text.index("{", i)
    for k in range(j, len(text)):
        depth += {"{": 1, "}": -1}.get(text[k], 0)
        if depth == 0:
            return text[i:k + 1]
    raise AssertionError(name)


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    news._FABRICATING.clear()
    R.CB_COST_SEEN.clear()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()


# ── 1: the defender's allies, whether they answer, the price of losing ──────────────────

ATHLONE_CBS = reply("cbs_29585_1069_09_15")["result"]
FRANCE_CHR = {k: v for k, v in reply("inspect_37067_1069_09_15")["result"].items()
              if k not in ("subject", "request_id", "tick", "game_date")}
#: Not in the recording: the defender's own character read (Conchobar, count of Athlone; an AI
#: count of another culture than France's) and the seat's (Murchad, read is_ai true, tier 2,
#: income 2.34 at the start).
CONCHOBAR = {"id": ATHLONE, "is_ai": True, "ruler_tier": 2, "culture_id": 12, "spouses": []}
MURCHAD = {"id": ME, "is_ai": True, "ruler_tier": 2, "culture_id": 12, "gold": 6.67, "prestige": 433.4,
           "piety": 515.2, "age": 44, "faith_key": "catholic", "income_breakdown": {"total_monthly": 4.99}}


def _athlone_gw(opinion):
    da = ATHLONE_CBS["defender_allies"]

    def alliances(p):
        if p.get("character_id") == ATHLONE:
            return {"own_military_strength": da["his_own"], "total_callable_strength": da["with_allies"],
                    "alliances": [{"ally_id": a["id"], "military_strength": a["strength"]}
                                  for a in da["allies"] if not a.get("forced")],
                    "forced_allies": [{"ally_id": a["id"], "military_strength": a["strength"], "is_forced": True}
                                      for a in da["allies"] if a.get("forced")]}
        return {"own_military_strength": 850, "alliances": [{"ally_id": ULSTER, "military_strength": 754}]}

    def char(p):
        return {ATHLONE: CONCHOBAR, FRANCE: FRANCE_CHR, ME: MURCHAD}.get(p.get("character_id"), {"id": p.get("character_id")})
    return Gw({"query_alliances": alliances, "query_character": char,
               "query_opinion": lambda p: {"opinion": opinion, "actor_id": p["actor_id"], "target_id": p["target_id"]}})


def test_1_the_recorded_reply_could_not_say():
    """1069.09.15: France 3,941 beside the defender's 578, 'accepts not read'; declared two steps later."""
    da = ATHLONE_CBS["defender_allies"]
    assert da["accepts"].startswith("not read") and da["with_allies"] == 4519
    assert [a["id"] for a in da["allies"]] == [FRANCE, 37469, 30025]
    assert "if_you_lose" not in ATHLONE_CBS["valid_options"][0]


def test_1_each_ally_carries_would_join_and_likely_strength():
    gw = _athlone_gw(opinion=0)
    da = run(R.defender_allies(gw, ATHLONE, attacker_id=ME))
    fr, a0, forced = da["allies"]
    # 20 base + 50 defensive + France's honor 10 + opinion 0 (FRANCE_CHR personality.honor)
    assert FRANCE_CHR["personality"]["honor"] == 10
    assert fr["would_join"] is True and fr["score"] == 80, fr
    assert fr["terms"] == {"base": 20, "defensive_war": 50, "honor": 10, "opinion_of_defender": 0}
    assert "may_refuse" in fr          # a king (tier 4) for an AI count, both sides AI rulers
    assert a0["would_join"] is False and forced["would_join"] is False and "under 100" in a0["why"]
    assert da["likely_strength"] == 578 + 3941 and da["with_allies"] == 4519
    assert da["basis"].startswith("an estimate") and da["estimate_not_read"]
    # the opinion read is the ally's of the defender (GET_OPINION_OF(actor, target))
    assert ("query_opinion", {"actor_id": FRANCE, "target_id": ATHLONE}) in gw.reads


def test_1_a_hostile_ally_reads_not_joining_and_an_unread_opinion_is_unknown():
    da = run(R.defender_allies(_athlone_gw(opinion=-95), ATHLONE, attacker_id=ME))
    assert da["allies"][0]["would_join"] is False and da["allies"][0]["score"] == -15
    assert da["likely_strength"] == 578
    da = run(R.defender_allies(_athlone_gw(opinion=None), ATHLONE, attacker_id=ME))
    assert da["allies"][0]["would_join"] == "unknown" and da["allies"][0]["unread"] == ["opinion_of_defender"]
    assert da["unknown_strength"] == 3941 and da["likely_strength"] == 578


def test_1_an_ally_of_yours_is_reluctant():
    a = R.call_to_arms_estimate(FRANCE_CHR, strength=3941, opinion=0, defender=CONCHOBAR,
                                attacker=MURCHAD, allied_to_attacker=True)
    assert a["terms"]["allied_to_you"] == -25 and a["score"] == 55


def test_1_verdict_and_loss_price_on_cbs():
    """The war lost 1070.11: -176 gold (3 years of income). The row now says it before."""
    assert R.war_verdict(850, {"likely_strength": 4519, "with_allies": 4519}) == "outnumbered_with_likely_allies"
    assert R.war_verdict(850, {"likely_strength": 578, "with_allies": 4519}) == "outnumbered_only_if_every_ally_joins"
    assert R.war_verdict(5000, {"likely_strength": 578, "with_allies": 4519}) == "stronger_than_all_his_allies"
    lp = R.loss_price("claim_cb", 4.89)
    assert lp["gold"] == -round(3 * 12 * 4.89, 1) and lp["truce_days"] == R.TRUCE_DAYS
    assert R.loss_price("some_cb", 4.89)["gold"] == "unknown"


def test_1_cbs_reply_carries_them():
    raw = {"status": "ok", "target_id": ATHLONE, "target_state": ATHLONE_CBS["target_state"],
           "cbs": [{"cb_key": "claim_cb", "valid": True, "scope": "county", "target_mode": 6,
                    "cb_title_id": 469, "claimant_id": ME, "family": "claim", "cost_prestige": 0,
                    "cost_piety": 0, "affordable": True}]}
    gw = _athlone_gw(opinion=0)
    gw.replies.update({"query_cbs_for_targets": {"error": "unknown_action"},
                       "query_cbs_for_target": lambda p: dict(raw, target_id=p.get("target_id")),
                       "query_armies": {"armies": []},
                       "query_claims": {"claims": [{"title_id": 469, "type": "unpressed"}]}})
    env = run(R.cbs_impl(gw, target_id=ATHLONE, character_id=ME))
    res = env["result"]
    assert res["defender_allies"]["likely_strength"] == 4519
    assert res["strength_verdict"]["verdict"] == "outnumbered_with_likely_allies"
    assert res["valid_options"][0]["if_you_lose"]["gold"] == -round(3 * 12 * 4.99, 1)


def test_1_the_terms_are_the_game_files():
    t = block_of(game_file("common/character_interactions/00_alliance.txt"), "call_ally_interaction")
    acc = t[t.index("ai_accept = {"):t.index("ai_will_do = {")]
    assert re.search(r"ai_accept = \{\s*base = 20\b", acc)
    assert re.search(r"add = 50\s+scope:target\.casus_belli\.primary_defender = scope:actor\s+desc = DEFENSIVE_WAR_REASON", acc)
    assert "add = -25" in acc and "WAR_WITH_ALLY_REASON" in acc
    assert "ai_honor = 1" in acc and "min = 0" in acc
    assert "scope:recipient.current_military_strength >= 100" in t
    assert "ai_maybe" not in t                 # a score below 1 is no, any other yes
    assert "is_spouse_of = scope:actor" in t   # auto_accept


# ── 2: fabrication progress: the rate the game adds, overdue past it ───────────────────

def test_2_the_recorded_rows_said_nothing():
    fab = news_of("end_turn_1075_09_15").get("fabrication") or {}
    assert fab.get("progress") is None
    assert "expected" not in fab and "overdue" not in fab


def test_2_expected_months_from_learning_and_border():
    e = news.fabrication_expected(8, True)
    assert e["monthly_pct_at_least"] == 4.6 and e["months_at_most"] == 22
    e = news.fabrication_expected(8, False)
    assert e["monthly_pct_at_least"] == 2.3 and e["months_at_most"] == 44
    assert news.fabrication_expected(None, True) is None


def _holder_gw(neighbor_ruler, holder_liege, holder_top, actor_top=ME, holder=700):
    """A county held by `holder` (liege, top liege as given; same faith as the ruler) beside a neighbour of
    `neighbor_ruler`."""
    def char(p):
        cid = p.get("character_id")
        if cid == holder:
            return {"id": holder, "faith_id": 1, "liege_id": holder_liege, "top_liege_id": holder_top}
        return {"id": cid, "faith_id": 1, "liege_id": 0, "top_liege_id": actor_top}
    return Gw({"query_county_neighbors": {"neighbors": [{"realm_ruler_id": neighbor_ruler}]},
               "query_title_hierarchy": {"title": {"holder_id": holder}}, "query_character": char})


def test_2_breifne_four_years_is_not_overdue_while_a_nemesis_chaplain_could_take_longer():
    """Breifne set 1072.09.15, still running 1076.09.15 (retargeted once): 48 months. The most for a chaplain who is
    neither rival nor nemesis is 44; a nemesis (x0.7, not read) can take 63, so 48 is not overdue."""
    gw = _holder_gw(999, 999, 999)
    fs = {"running": True, "target": {"title_id": 498, "title_key": "c_breifne"}, "target_province_id": 77,
          "since": "1072.09.15"}
    snap = {"actor_id": ME, "date": "1076.09.15", "chaplain_learning": 8,
            "council": {"councillor_court_chaplain": {"holder_id": 500}}}
    run(news._fabrication_timing(gw, snap, {"date": "1076.07.04"}, fs))
    assert fs["expected"]["months_at_most"] == 44 and fs["expected"]["borders_your_realm"] is False
    assert fs["expected"]["months_elapsed"] == 48.0 and fs["overdue"] is False
    assert news.fabrication_overdue_after(fs["expected"]) == 63
    fs2 = dict(fs, expected=None)
    run(news._fabrication_timing(gw, dict(snap, date="1078.01.15"), {"date": "1077.11.04"}, fs2))
    assert fs2["overdue"] is True


def test_2_a_county_held_inside_the_realm_is_three_times_slower_and_unread_judges_no_overdue():
    """The game's x0.3 (court_chaplain_fabricate_claim_vassal_penalty) on a county whose holder has you as liege or
    above, with no overdue judged (whether the game's off-border x0.5 also applies inside the realm is not known
    from its files); with the holder unread, the rate outside the realm is given and overdue is not judged."""
    e = news.fabrication_expected(8, True, in_realm=True)
    assert e["monthly_pct_at_least"] == 1.38 and e["months_at_most"] == 73
    for liege, top in ((ME, ME), (601, ME)):
        fs = {"running": True, "target": {"title_id": 469}, "target_province_id": 5, "since": "1076.09.15"}
        snap = {"actor_id": ME, "date": "1079.09.15", "chaplain_learning": 8,
                "council": {"councillor_court_chaplain": {"holder_id": 500}}}
        run(news._fabrication_timing(_holder_gw(ME, liege, top), snap, {"date": "1079.07.04"}, fs))
        assert fs["expected"]["months_at_most"] == 73 and "overdue" not in fs, (liege, top, fs)
        assert fs["expected"]["unread"] == news.FABRICATION_IN_REALM_UNREAD
        # far past even twice the bound: still not judged
        fs = {"running": True, "target": {"title_id": 469}, "target_province_id": 5, "since": "1066.09.15"}
        run(news._fabrication_timing(_holder_gw(ME, liege, top), dict(snap, date="1085.09.15"),
                                     {"date": "1085.07.04"}, fs))
        assert fs["expected"]["months_elapsed"] > news.fabrication_overdue_after(fs["expected"]) and "overdue" not in fs
    news._FAB_CHAPLAIN.clear()
    fs = {"running": True, "target": {"title_id": 469}, "target_province_id": 5, "since": "1076.09.15"}
    run(news._fabrication_timing(Gw({"query_county_neighbors": {"neighbors": [{"realm_ruler_id": ME}]}}),
                                 snap, {"date": "1079.07.04"}, fs))
    assert fs["expected"]["months_at_most"] == 22 and "overdue" not in fs
    assert fs["expected"]["unread"] == news.FABRICATION_HOLDER_UNREAD


def test_2_CONTROL_a_holder_in_another_realm_is_judged_at_the_outside_rate():
    fs = {"running": True, "target": {"title_id": 469}, "target_province_id": 5, "since": "1076.09.15"}
    snap = {"actor_id": ME, "date": "1079.09.15", "chaplain_learning": 8,
            "council": {"councillor_court_chaplain": {"holder_id": 500}}}
    run(news._fabrication_timing(_holder_gw(ME, 999, 999), snap, {"date": "1079.07.04"}, fs))
    assert fs["expected"]["months_at_most"] == 22 and fs["overdue"] is True and "unread" not in fs["expected"]


def test_2_a_new_chaplain_restarts_the_count():
    gw = _holder_gw(ME, 999, 999)
    fs = {"running": True, "target": {"title_id": 469}, "target_province_id": 5, "since": "1076.09.15"}
    s1 = {"actor_id": ME, "date": "1077.09.15", "chaplain_learning": 8,
          "council": {"councillor_court_chaplain": {"holder_id": 500}}}
    run(news._fabrication_timing(gw, s1, {"date": "1077.07.04"}, dict(fs)))
    fs2 = dict(fs)
    s2 = dict(s1, date="1078.02.08", council={"councillor_court_chaplain": {"holder_id": 501}})
    run(news._fabrication_timing(gw, s2, {"date": "1077.11.27"}, fs2))
    assert fs2["chaplain_changed"] == {"from": 500, "to": 501, "on_or_before": "1078.02.08"}
    assert fs2["expected"]["counted_from"] == "1077.11.27" and fs2["overdue"] is False


def test_2_the_rate_is_the_game_files():
    t = block_of(game_file("common/council_tasks/00_court_chaplain_tasks.txt"), "task_fabricate_claim")
    assert "task_progress = task_progress_percentage" in t
    assert "court_chaplain_fabricate_claim_not_adjacent_penalty" in t
    v = game_file("common/script_values/99_court_chaplain_values.txt")
    assert re.search(r"court_chaplain_fabricate_claim_base = \{\s*add = 3\s*\}", v)
    assert re.search(r"court_chaplain_fabricate_claim_monthly_increase_base = \{\s*add = learning\s*divide = 5\s*\}", v)
    assert re.search(r"court_chaplain_fabricate_claim_not_adjacent_penalty = \{\s*value = 0\.5\s*\}", v)


# ── 3: mercenaries: the ruler sent, the companies in range, priced ──────────────────────

COMPANIES = {"companies": [
    {"company_id": 7, "label": "Mercenary company from Ulster, 900 men", "troops": 900, "distance_px": 120,
     "in_range": True, "same_culture": True, "regiments": [{"maa_type": "levies", "men": 800},
                                                           {"maa_type": "bowmen", "men": 100}]},
    {"company_id": 9, "label": "Mercenary company from Kaifeng, 4000 men", "troops": 4000,
     "distance_px": 9000, "in_range": False, "same_culture": False, "regiments": []}]}


def _merc_gw():
    return Gw({"query_hireable_mercenaries": COMPANIES,
               "query_character": lambda p: dict(MURCHAD, realm_size=2, gold=84.0),
               "query_maa_types": {"types": [{"maa_type": "bowmen", "recruit_cost": 45.0}]},
               # The game's realm size is the realm's counties (query_realm)
               "query_realm": {"realm_total_counties": 2}})


def test_3_the_listing_sends_the_ruler_and_keeps_the_range():
    gw = _merc_gw()
    env = run(R._do_enumerate(gw, "mercenaries", 0, [], {"id": ME}, {}))
    sent = [p for a, p in gw.reads if a == "query_hireable_mercenaries"]
    assert sent and sent[0]["actor_id"] == ME
    rows = env["result"]["available"]
    assert [r["company_id"] for r in rows] == [7]
    price = round((8 * 12 + 0.5 * 45) * (1 + 0.05 * 2 ** 0.5), 2)
    assert rows[0]["price_estimate"] == price and rows[0]["affordable"] is True
    assert "1 companies out of hire range" in env["result"]["unavailable"][0]["label"]


def test_3_affordability_is_the_games_debt_rule():
    assert R.merc_affordable(190.0, 84.0, 4.99) is True      # 84 - 190 x 1.03 = -111.7 >= -119.76
    assert R.merc_affordable(220.0, 84.0, 4.99) is False     # 84 - 220 x 0.97 = -129.4 < -119.76
    # Within the estimate's 3% margin of the line it does not say yes or no
    assert R.merc_affordable(200.0, 84.0, 4.99) == "unsure"
    assert R.merc_affordable(None, 84.0, 4.99) is None


def test_3_a_hire_refusal_names_its_gate():
    env = {"verdict": "rejected", "blocked": {"gate": "unknown", "raw_reason": "CanExecute=0"}}
    run(R._explain_hire_refusal(_merc_gw(), env, 9, ME))
    assert env["blocked"]["gate_named"]["gate"] == "MERCENARY_COMPANY_OUTSIDE_RANGE"
    gw = _merc_gw()
    gw.replies["query_character"] = lambda p: dict(MURCHAD, realm_size=2, gold=-150.0)
    env = {"verdict": "rejected", "blocked": {"gate": "unknown"}}
    run(R._explain_hire_refusal(gw, env, 7, ME))
    assert env["blocked"]["gate_named"]["gate"] == "HIRED_TROOPS_CANNOT_AFFORD"


def test_3_the_bridge_row_carries_the_range():
    src = (BRIDGE / "src" / "query_handlers.cpp").read_text(errors="replace")
    body = src[src.index("bool do_query_hireable_mercenaries("):]
    body = body[:body.index("\n}\n")]
    assert "::merc_company::CULTURE" in body and 'r.key("in_range")' in body
    assert "same ? 1500 : 500" in body
    assert re.search(r"constexpr size_t CULTURE\s+= 0x1c;", OFFSETS.read_text())
    d = game_file("common/defines/00_defines.txt")
    assert re.search(r"HIRE_RANGE = 500\b", d) and re.search(r"HIRE_RANGE_SAME_CULTURE = 1500\b", d)
    assert re.search(r"ALLOWED_DEBT_MONTHS = 24\b", d) and re.search(r"LEVY_COST_PER_100_SOLDIERS = 12\.0", d)


# ── 4: calling an ally costs prestige; every move over 50 has a cause or a ruled-out list ──

def test_4_the_recorded_purse_left_it_unexplained():
    p = news_of("end_turn_1069_09_15")["purse"]["prestige"]
    assert p["change"] == -198.1 and p["unexplained"] == -73.1
    assert news_of("end_turn_1084_11_27")["purse"]["prestige"]["change"] == -348.34
    assert "ruled_out" not in news_of("end_turn_1084_11_27")["purse"]["prestige"]


def test_4_the_price_is_the_allys_tier():
    assert R.call_ally_cost(2, "attacker") == -75 and R.call_ally_cost(4, "attacker") == -350
    assert R.call_ally_cost(3, "defender") == 0 and R.call_ally_cost(None, "attacker") is None
    eff = game_file("common/scripted_effects/00_interaction_effects.txt")
    body = eff[eff.index("call_ally_interaction_event_effect = {"):]
    body = body[:body.index("seduce_interaction_stress_effect")]
    assert "is_attacker = scope:actor" in body and "add_prestige = minor_prestige_loss" in body
    assert "add_prestige = major_prestige_loss" in body and "add_prestige = -10" in body
    vals = game_file("common/script_values/00_basic_values.txt")
    assert re.search(r"minor_prestige_value = 75\b", vals) and re.search(r"major_prestige_value = 350\b", vals)


def _snap(date, prestige, attackers, **kw):
    return dict({"actor_id": ME, "date": date, "prestige": prestige, "piety": 515.2, "gold": 6.7,
                 "wars": {91: {"war_id": 91, "name": "Claim on Athlone", "attacker_id": ME, "defender_id": ATHLONE,
                               "is_attacker": True, "cb_type": "claim_cb", "attackers": attackers}},
                 "laws": [], "titles": {}, "claims": {}}, **kw)


def test_4_the_1069_step_is_attributed():
    """1069.09.15: declared (-125), called Cu Uladh (c_ulster, a count); prestige 433.4 -> 235.3."""
    news.note_act(ME, "1069.09.15", "declared war 91 (claim_cb)", cost_known=True, prestige=-125.0)
    news.note_call(ME, ULSTER, 91, "1069.09.15")
    gw = Gw({"query_character": lambda p: {"id": p["character_id"], "ruler_tier": 2}})
    run(news.block(gw, 0, _snap("1069.09.15", 433.4, [ME]), offers_served=False))
    out = run(news.block(gw, 0, _snap("1069.11.27", 235.3, [ME, ULSTER]), offers_served=False))
    (j,) = out["allies_joined"]
    assert j["ally_id"] == ULSTER and j["prestige"] == -75 and j["called_by"].startswith("you")
    p = out["purse"]["prestige"]
    assert "ruled_out" not in p and abs(p.get("unexplained", 0)) < 2, p


def test_4_a_join_the_server_did_not_send_is_the_ais():
    gw = Gw({"query_character": lambda p: {"id": p["character_id"], "ruler_tier": 3}})
    run(news.block(gw, 0, _snap("1082.09.15", 825.0, [ME]), offers_served=False))
    out = run(news.block(gw, 0, _snap("1082.11.27", 676.0, [ME, 777]), offers_served=False))
    assert out["allies_joined"][0]["called_by"].startswith("the game's AI")
    assert out["allies_joined"][0]["prestige"] == -150


def test_4_the_348_with_no_cause_names_what_was_ruled_out():
    gw = Gw({})
    run(news.block(gw, 0, _snap("1084.11.27", 592.82, [ME], wars={}), offers_served=False))
    out = run(news.block(gw, 0, _snap("1085.02.08", 244.48, [ME], wars={}), offers_served=False))
    p = out["purse"]["prestige"]
    assert p["unexplained"] == -348.34
    assert "no war declared by you" in p["ruled_out"] and "no ally joined a war you attack in" in p["ruled_out"]
    assert any("on_decline" in x for x in p["not_read"])


def test_4_call_ally_reply_prices_it():
    gw = Gw({"query_wars": {"wars": [{"war_id": 91, "primary_attacker": ME, "primary_defender": ATHLONE,
                                      "attackers": [ME], "defenders": [ATHLONE]}]},
             "query_character": lambda p: {"id": p["character_id"], "ruler_tier": 2}})

    async def cw(action, params, readback=None, deadline_s=0, max_polls=0):
        return {"sent": True, "raw": {"success": True, "status": "queued"}, "resolved": None,
                "drains_when": "clock_advances"}
    gw.confirmed_write = cw
    env = run(R._do_war_intent(gw, "call_ally", {"war_id": 91, "target_id": ULSTER}, ME, {"id": ME}))
    assert env["result"]["price"]["prestige"] == -75, env
    assert news._CALLS[ME][-1]["ally_id"] == ULSTER


# ── 5: claim_cb's legitimacy multiplier ──────────────────────────────────────────────────

DOMNALL_C = {"id": DOMNALL, "age": 30, "is_female": False, "faith_key": "catholic", "dynasty_id": 7,
             "ruler_tier": 2}
#: Not in the recording: the Ailech holder's faith and dynasty that give the recorded 62.5
#: (100 x 0.5 same dynasty x 1.25 another faith); Domnall's legitimacy just after succeeding.
AILECH_C = {"id": AILECH_LORD, "age": 30, "is_female": False, "faith_key": "insular_celtic", "dynasty_id": 7,
            "ruler_tier": 2}


def test_5_the_recorded_price_and_refusal():
    opt = reply("cbs_64351_1090_07_04")["result"]["valid_options"][0]
    assert opt["cost_prestige"] == 62.5
    dec = reply("declare_64351_1090_07_04")
    assert dec["ok"] is False and dec["blocked"]["raw_reason"] in ("cb_invalid_for_target", "can_declare_war_false") \
        or "cb_invalid" in json.dumps(dec["blocked"])


def test_5_legitimacy_level_one_is_x1_5():
    assert DG.claim_cost("county", False, DOMNALL_C, AILECH_C)["cost"] == 62.5
    cc = DG.claim_cost("county", False, dict(DOMNALL_C, legitimacy=10.0), AILECH_C)
    assert cc["cost"] == 93.75 and 78.56 < cc["cost"]         # refused at 78.56 held
    assert not any(u.startswith("your legitimacy") for u in cc["unread"])
    # a count at legitimacy 100: level 2 in eras 2-4 (threshold 90..150), level 1 in none:
    # 100 >= 30 x (era + 1) for era <= 2 only -> x1 or x1.5 by era
    cc = DG.claim_cost("county", False, dict(DOMNALL_C, legitimacy=100.0), AILECH_C)
    assert cc["cost"] == 62.5 and cc["cost_high"] == 93.75


def test_5_the_flags_are_the_game_files():
    t = game_file("common/legitimacy/00_legitimacy.txt")
    for tier_name, tier in (("count_legitimacy", 2), ("duke_legitimacy", 3), ("king_legitimacy", 4),
                            ("emperor_legitimacy", 5)):
        b = block_of(t, tier_name)
        levels = re.split(r"\n\tlevel = \{", b)[1:]
        got = []
        for lv in levels:
            m = re.search(r"flag = (\w+)_claim_cb_cost", lv)
            got.append(m.group(1) if m else None)
        assert tuple(got) == DG.LEGITIMACY_CLAIM_FLAGS[tier], tier_name
    v = game_file("common/script_values/00_legitimacy_values.txt")
    for i, base in enumerate(DG.LEGITIMACY_LEVEL_BASE[1:], start=2):
        assert re.search(rf"legitimacy_level_{i} = \{{[^}}]*value = {base}\b", v), i
    c = game_file("common/casus_belli_types/00_claim.txt")
    for f, x in DG.LEGITIMACY_FLAG_X.items():
        assert re.search(rf"has_legitimacy_flag = {f}_claim_cb_cost \}}\s*add = {x:g}\b", c), f


def test_5_the_purse_reads_legitimacy():
    gw = Gw({"query_character": dict(DOMNALL_C, gold=10.0, prestige=78.56, piety=1.0),
             "get_ai_evaluations": {"success": True, "diplomacy": {"legitimacy": 10.0}},
             "query_armies": {"armies": []}, "query_claims": {"claims": []}})
    p = run(R.ruler_purse(gw, DOMNALL))
    assert p["me"]["legitimacy"] == 10.0


# ── 6: small ones ────────────────────────────────────────────────────────────────────────

def test_6_the_hof_gold_hint_stops_after_a_no():
    assert reply("hof_gold_send_1070_11_27")["blocked"]["gate"] in ("recipient_refuses", "target_would_reject")
    acc = reply("hof_gold_acceptance_1072_02_08")["result"]
    assert acc.get("would_accept") is False
    snap = {"actor_id": ME, "piety": 522.0}
    assert news.idle(snap)["piety"]["lever"] == "hof_gold"
    news.note_acceptance(ME, news.HOF_GOLD_SEND, False, acc.get("accept_score"), "1072.02.08")
    row = news.idle(snap)["piety"]
    assert "lever" not in row and "next" not in row and row["hof_gold_refused"]["date"] == "1072.02.08"
    news.note_acceptance(ME, news.HOF_GOLD_SEND, True, 40, "1074.02.08")
    assert news.idle(snap)["piety"]["lever"] == "hof_gold"


def test_6_the_acceptance_read_feeds_it():
    gw = Gw({"query_interaction_check": {"result": {"results": [
        {"target_id": 29012, "sendable": False, "refused_by": "target_would_reject", "answer": "no",
         "accept_score": -5}]}}})
    run(R.acceptance_impl(gw, news.HOF_GOLD_SEND, 29012, ME))
    assert (ME, news.HOF_GOLD_SEND) in news._ACCEPT_NO


def test_6_single_heir_refusal_names_the_price():
    env = json.loads(json.dumps(reply("change_law_single_heir_1085_04_22")))
    assert env["blocked"]["raw_reason"] == "not_allowed_or_unaffordable"
    run(R._explain_law_refusal(Gw({"query_character": {"id": ME, "prestige": -168.14}}), env,
                               "single_heir_succession_law", ME))
    assert env["blocked"]["gate"] == "insufficient_prestige"
    assert env["blocked"]["measured"] == {"cost_prestige": 500, "have": -168.14}
    env = json.loads(json.dumps(reply("change_law_single_heir_1085_04_22")))
    run(R._explain_law_refusal(Gw({"query_character": {"id": ME, "prestige": 900.0}}), env,
                               "single_heir_succession_law", ME))
    assert env["blocked"]["not_checked"] and env["blocked"]["checked"]["prestige"]["ok"] is True
    v = game_file("common/script_values/00_law_values.txt")
    assert re.search(r"change_succession_law_prestige_base = 500\b", v)


def test_6_decision_why_blocked_points_at_nothing_unshipped():
    out = T.shield_decision_why_blocked("negotiate_the_danelaw_decision", realm={}, character={})
    assert out.get("clauses") is not None            # the modelled branch, the one that had the pointer
    assert "doc" not in out and "evolving-lessons" not in json.dumps(out)


def test_6_the_claim_steps_gold_is_looked_into():
    """pre-flight 1067.11.27: -226.9 with -142.6 unexplained at the Athlone claim's arrival."""
    g = news_of("pf_end_turn_1067_09_15")["purse"]["gold"]
    assert g["change"] == -226.9 and g["unexplained"] == -142.6
    gw = Gw({"query_county_details": {"counties": [{"county_key": "c_dublin", "holdings": [
        {"construction": {"building": "castle_02", "started": "1067.10.03", "ordered_by": ME}}]}]}})
    base = {"actor_id": ME, "laws": [], "titles": {}, "claims": {}, "wars": {}, "prestige": 410.0,
            "piety": 438.4, "monthly_income": 4.88}
    run(news.block(gw, 0, dict(base, date="1067.09.15", gold=231.41), offers_served=False))
    out = run(news.block(gw, 0, dict(base, date="1067.11.27", gold=4.51), offers_served=False))
    gold = out["purse"]["gold"]
    assert gold["constructions_started"] == [{"building": "castle_02", "started": "1067.10.03",
                                               "ordered_by": ME, "county": "c_dublin"}]
    assert gold["not_read"]


def test_5_live_a_range_shows_its_high_end():
    """A live read, 1067.11.27: Murchad (count, legitimacy 159, unread era) priced 62.5-125 on
    Athlone, shown 62.5; declared, the step's prestige fell 122.03 (the game charged 125)."""
    raw = {"status": "ok", "target_id": ATHLONE, "target_state": {"in_truce": False},
           "cbs": [{"cb_key": "claim_cb", "valid": True, "scope": "county", "target_mode": 6,
                    "cb_title_id": 469, "claimant_id": ME, "family": "claim", "cost_prestige": 0,
                    "cost_piety": 0, "affordable": True}]}
    me = dict(MURCHAD, prestige=412.89, dynasty_id=1838, age=43, is_female=False)
    gw = Gw({"query_cbs_for_targets": {"error": "unknown_action"},
             "query_cbs_for_target": lambda p: dict(raw, target_id=p.get("target_id")),
             "query_character": lambda p: me if p.get("character_id") == ME else dict(
                 CONCHOBAR, age=40, is_female=False, faith_key="insular_celtic", dynasty_id=5),
             "get_ai_evaluations": {"success": True, "diplomacy": {"legitimacy": 159.0}},
             "query_armies": {"armies": []}, "query_alliances": {"own_military_strength": 608, "alliances": []},
             "query_claims": {"claims": [{"title_id": 469, "type": "unpressed"}]}})
    opt = run(R.cbs_impl(gw, target_id=ATHLONE, character_id=ME))["result"]["valid_options"][0]
    assert opt["cost_prestige"] == 125.0 and opt["cost_prestige_low"] == 62.5
    assert R.CB_COST_SEEN[(ATHLONE, "claim_cb", 469)]["prestige"] == 125.0


def test_2_the_per_turn_news_keeps_the_numbers_not_the_rule():
    """The rule's 270-byte sentence rode in every end_turn's news."""
    fs = {"running": True, "expected": {"months_at_most": 22, "months_elapsed": 2.4, "rule": news.FABRICATION_RULE}}
    out = news.compact({"since": "1066.09.15", "fabrication": fs})
    assert out["fabrication"]["expected"] == {"months_at_most": 22, "months_elapsed": 2.4}
