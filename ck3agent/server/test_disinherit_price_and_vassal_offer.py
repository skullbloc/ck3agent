"""Disinherit prices and vassal offers, each row driven by recorded game values.

- (1077.08.16) succession_plan priced disinherit 150 prestige + 150 renown, `allowed_now: true`,
against 470 prestige and 1,474 renown; the game refused the send (send_cost_unaffordable). Live (the 1077
autosave, query_interaction_check): the game's own price for each of the three sons is 300 prestige + 300 renown
(each is heir of a held title under partition: any_heir_title), and the dynasty's SPENDABLE renown is 230.15
(query_dynasty.renown, dynasty+0x150); 1,470.65 is the accumulated total its level counts. The engine's words:
"Missing 70".
- (1075.04.02, replayed live from the 1075 autosave) Murchad became Munster's (31762) vassal with no war and
no event: Munster's AI offered vassalage and Murchad's AI accepted inside the send. The ai_veto hook gets a kind
for it (vassal_offer), liege_changed names the cause, and the threats news row carries who and how strong.
- (1071.09.22) `ai_veto set decision keys [take_vow_of_poverty_decision, convert_to_local_culture]` was
taken whole: an unknown key, and the list replaced.
"""
import asyncio
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import news  # noqa: E402
import partition_forecast as PF  # noqa: E402
import redesign as R  # noqa: E402
import seat_events as SE  # noqa: E402
import test_succession_study_fixes as T25  # noqa: E402


def run(c):
    return asyncio.run(c)


# ── Disinherit and vows, the game's price and verdict ────────────────────

#: the recorded reads of 1077 (live diag1077 and a recorded game's 1077.08.16 replies)
MURCHAD_1077 = {"id": 31506, "name": "Murchad", "alive": True, "is_female": False, "age": 52, "health": 2.52,
                "children": [36124, 37111, 37528], "ruler_tier": 2, "dynasty_id": 1838, "faith_id": 23,
                "piety": 893.66, "prestige": 470.33}
DYN_1077 = {"head_character_id": 31506, "prestige": 1470.65, "renown": 230.15, "renown_accumulated": 1470.65}


def sim_1077():
    c = T25.county
    return {"primary_title_id": 463, "heirs": [
        {"character_id": 36124, "name": "Domnall", "is_player_heir": True, "titles": [c(463, "c_dublin")]},
        {"character_id": 37111, "name": "Enna", "is_player_heir": False,
         "titles": [c(469, "c_athlone"), c(501, "c_breifne")]},
        {"character_id": 37528, "name": "Donnchad", "is_player_heir": False,
         "titles": [c(479, "c_oriel"), c(508, "c_leinster")]}]}


def kids_1077():
    k = T25.kid
    return {36124: k(36124, "Domnall", age=30), 37111: k(37111, "Enna", age=25),
            37528: k(37528, "Donnchad", age=22, faith_id=99)}


#: query_interaction_check as the bridge answered it live (diag1077, 1077.01.01), engine_cost included
CHECK_DIS = {"interaction": "disinherit_interaction", "actor_id": 31506, "results": [
    {"target_id": t, "sendable": False, "refused_by": "send_cost_unaffordable", "engine_reason": "Missing 70",
     "answer": "yes", "accept_score": 1.0, "answer_text": "Will accept",
     "engine_cost": {"prestige": 300.0, "renown": 300.0}} for t in (36124, 37111, 37528)]}
CHECK_VOWS = {"interaction": "take_vows_interaction", "actor_id": 31506, "results": [
    {"target_id": 37111, "sendable": False, "refused_by": "target_would_reject", "answer": "no",
     "accept_score": -205.0, "answer_text": "Will not accept", "engine_cost": {"piety": 350.0}},
    {"target_id": 37528, "sendable": False, "refused_by": "interaction_not_valid_for_target",
     "engine_reason": "Donnchad ua Cheinnselaig must not be a Commander or a Knight in an Army; "
                      "Donnchad ua Cheinnselaig is not Insular"}]}


def _check(p):
    src = CHECK_DIS if p.get("interaction_type") == "disinherit_interaction" else CHECK_VOWS
    want = set(p.get("candidates") or [])
    return {"success": True, "result": dict(src, results=[r for r in src["results"] if r["target_id"] in want])}


def plan_1077(check=True, dyn=DYN_1077, ruler=MURCHAD_1077):
    gw = T25.Gw(dict(ruler), kids_1077(), sim_1077(), dynasty=dict(dyn),
                extra={"query_interaction_check": _check} if check else {})
    return T25.plan(gw), gw


def test_1a_without_the_games_check_the_price_counts_the_title_heir_and_the_spendable_renown():
    out, _ = plan_1077(check=False)
    rows = {d["son_id"]: d for d in out["levers"]["take_out_of_split"]["disinherit"]}
    assert set(rows) == {37111, 37528}
    for d in rows.values():
        # base: 150 / 150 and allowed_now true against the accumulated 1,474
        assert d["price"]["prestige"] == 300 and d["price"]["renown"] == 300
        assert d["you_hold"]["renown_spendable"] == 230.15
        assert d["allowed_now"] is False and any("spendable renown 230 < 300" in w for w in d["why_not"])
    assert out["levers"]["disinherit"]["dynasty_renown"] == 230.15


def test_1b_the_games_own_check_decides_and_names_what_is_short():
    out, gw = plan_1077(check=True)
    assert ("query_interaction_check", {"actor_id": 31506, "interaction_type": "disinherit_interaction",
                                        "candidates": [37111, 37528]}) in gw.reads
    d = {r["son_id"]: r for r in out["levers"]["take_out_of_split"]["disinherit"]}[37528]
    assert d["price"] == {"prestige": 300.0, "renown": 300.0}
    assert d["allowed_now"] is False and d["game_check"]["refused"]["code"] == "send_cost_unaffordable"
    assert d["game_check"]["refused"]["short"] == {"renown": 69.85}
    assert "Missing 70" in d["why_not"][0] and "renown" in d["to_allow"]
    v = {r["son_id"]: r for r in out["levers"]["take_out_of_split"]["vows"]}
    assert v[37111]["allowed_now"] is False and "he would refuse" in v[37111]["why_not"][0] \
        and "-205" in v[37111]["why_not"][0] and v[37111]["price"] == {"piety": 350.0}
    assert v[37528]["allowed_now"] is False and "not Insular" in v[37528]["why_not"][0]


def test_1c_a_send_the_game_takes_reads_allowed():
    ok = {"interaction": "disinherit_interaction", "results": [
        {"target_id": 37528, "sendable": True, "answer": "yes", "engine_cost": {"prestige": 150.0, "renown": 150.0}}]}
    gw = T25.Gw(dict(MURCHAD_1077), kids_1077(), sim_1077(), dynasty=dict(DYN_1077, renown=400.0),
                extra={"query_interaction_check": lambda p: {"success": True, "result": ok}
                       if p.get("interaction_type") == "disinherit_interaction" else {}})
    out = T25.plan(gw)
    d = {r["son_id"]: r for r in out["levers"]["take_out_of_split"]["disinherit"]}[37528]
    assert d["allowed_now"] is True and d["why_not"] is None and d["price"] == {"prestige": 150.0, "renown": 150.0}


def test_1d_accumulated_renown_alone_is_not_a_price_holder():
    out, _ = plan_1077(check=False, dyn={"head_character_id": 31506, "prestige": 1470.65})
    d = out["levers"]["take_out_of_split"]["disinherit"][0]
    assert d["allowed_now"] is None and d["why_not"] == ["the dynasty's spendable renown was not read"]


# ── Becoming a vassal ────────────────────────────────────────────────────

def test_2a_vassal_offer_is_a_veto_kind_and_an_acted_row_names_who_offered():
    assert "vassal_offer" in SE.AI_VETO_WHATS
    rec = {"seq": 5, "character_id": 31506, "what": "vassal_offer", "dropped": False, "origin": "game",
           "offered_by": 31762, "date": "1075.04.01", "priced": "no", "unpriced_why": "no_price_read"}
    out = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [rec], "actor_id": 31506})
    a, = out["ai_acted"]
    assert a["offered_by"] == 31762 and "offered by another ruler" in a["sent_by"]
    assert a["veto"]["args"]["args"] == {"op": "set", "what": "vassal_offer"} and "price_unread" not in a


def test_2b_liege_changed_names_the_offer_or_says_what_was_looked_at():
    acted = {"ai_acted": [{"what": "vassal_offer", "offered_by": 31762, "date": "1075.04.01"}]}
    c = news.liege_cause({"from": 0, "to": 31762}, acted, {}, {})
    assert c["cause"] == "offer_accepted" and "31762 offered vassalage on 1075.04.01" in c["how"]
    assert c["closes_it"]["args"]["args"] == {"op": "set", "what": "vassal_offer"}
    war = {"wars_ended": [{"war_id": 9, "name": "x", "attacker_id": 31762, "defender_id": 31506,
                           "outcome": "attacker_victory"}]}
    assert news.liege_cause({"from": 0, "to": 31762}, war, {}, {})["cause"] == "war"
    none = news.liege_cause({"from": 0, "to": 31762}, {}, {}, {})
    assert "cause" not in none and "no war with him ended" in none["cause_unread"]
    assert news.liege_cause({"from": 31762, "to": 0}, {}, {}, {})["cause"].startswith("independent")


class _ThreatGw:
    def __init__(self):
        self.reads = []

    def game(self):
        return {"date": "1075.01.01"}


def test_2c_the_first_threats_read_names_who_can_vassalize_and_how_strong(monkeypatch):
    import threats as TH
    res = {"you": {"id": 31506, "strength": 1126}, "attackers": [
        {"id": 31762, "name": "Murchad", "tier": 3, "strength": 1086, "x_yours": 1.0,
         "can_declare": ["ducal_conquest_cb", "vassalization_cb"], "vassalization": {"valid_now": True}},
        {"id": 31266, "name": "Cu Uladh", "tier": 2, "strength": 743, "x_yours": 0.7, "can_declare": [],
         "vassalization": {"valid_now": False}}],
        "vassalization": {"verdict": "closed at 8 counties"}}

    async def fake_read(gw, me):
        TH._LAST[me] = {31762: ["ducal_conquest_cb", "vassalization_cb"]}
        return res
    monkeypatch.setattr(TH, "read", fake_read)
    news._THREAT_READ.clear(); news._THREAT_SEEN.clear()
    row = run(news.threat_rows(_ThreatGw(), {"actor_id": 31506, "date": "1075.01.01"}, "observe"))
    a, = row["attackers"]
    assert a["id"] == 31762 and a["strength"] == 1086 and a["x_yours"] == 1.0 and a["already_able"] is True
    assert row["your_strength"] == 1126 and row["by_offer"]["closes_it"]["args"]["args"]["what"] == "vassal_offer"


# ── ai_veto decision keys ────────────────────────────────────────────────

class _VetoGw:
    health = "ok"

    def __init__(self, keys=(), vetoes=("decision",)):
        self.keys = list(keys)
        self.vetoes = list(vetoes)
        self.writes = []

    def game(self):
        return {"tick": 1, "date": "1071.09.22"}

    async def write(self, action, params):
        self.writes.append((action, dict(params)))
        if params.get("op") == "set":
            self.keys = params["keys"].split(",") if params.get("keys") else []
            if "decision" not in self.vetoes:
                self.vetoes.append("decision")
        return {"success": True}

    async def read(self, action, params=None):
        return {"success": True, "rulers": [{"character_id": 31506, "vetoes": list(self.vetoes),
                                             "decision_keys": list(self.keys)}]}


def _veto(gw, args):
    return run(SE.do_ai_veto(gw, args, 31506, {"id": 31506}))


def test_3a_an_unknown_key_is_refused_with_the_nearest_named():
    if SE.__dict__.get("_ep_decision_keys") is None or SE._ep_decision_keys() is None:
        pytest.skip("the game's decision files are not on this host")
    gw = _VetoGw(keys=["convert_to_local_culture_decision"])
    out = _veto(gw, {"op": "set", "what": "decision",
                     "keys": ["take_vow_of_poverty_decision", "convert_to_local_culture"]})
    assert out["ok"] is False and out["blocked"]["raw_reason"] == "unknown_decision_key"
    assert out["result"]["nearest"] == {"convert_to_local_culture": "convert_to_local_culture_decision"}
    assert gw.writes == [] and gw.keys == ["convert_to_local_culture_decision"]


def test_3b_set_adds_to_the_list_and_replies_the_whole_list():
    gw = _VetoGw(keys=["convert_to_local_culture_decision"])
    out = _veto(gw, {"op": "set", "what": "decision", "keys": ["take_vow_of_poverty_decision"]})
    assert out["ok"] is True, out
    assert out["result"]["decision_keys"] == ["convert_to_local_culture_decision", "take_vow_of_poverty_decision"]
    assert out["result"]["keys_added_to"] == ["convert_to_local_culture_decision"]
    rep = _veto(gw, {"op": "set", "what": "decision", "keys": ["take_vow_of_poverty_decision"], "replace": True})
    assert rep["ok"] is True and rep["result"]["decision_keys"] == ["take_vow_of_poverty_decision"]
    assert "keys_added_to" not in rep["result"]


def test_3c_a_keyed_set_on_an_every_decision_veto_needs_replace():
    gw = _VetoGw(keys=[], vetoes=["decision"])
    out = _veto(gw, {"op": "set", "what": "decision", "keys": ["take_vow_of_poverty_decision"]})
    assert out["ok"] is False and out["blocked"]["raw_reason"] == "keys_would_narrow_every_decision"
    assert gw.writes == []


def test_1e_the_law_lever_names_its_two_gates():
    gw = T25.Gw(dict(MURCHAD_1077, culture_id=74), kids_1077(), sim_1077(), dynasty=dict(DYN_1077),
                extra={"query_innovation_progress": {"innovations": [{"name": "innovation_barracks", "is_known": True}]}})
    lv = T25.plan(gw)["levers"]["succession_law"]
    assert lv["law"] == "partition_succession_law" and lv["price"] == {"prestige": 500}
    assert lv["allowed_now"] is False and len(lv["needs"]) == 2 and "hold a duchy first" in lv["keeps"]
    gw2 = T25.Gw(dict(MURCHAD_1077, culture_id=74), kids_1077(), sim_1077(), dynasty=dict(DYN_1077),
                 laws=T25.LAWS + [{"name": "crown_authority_1", "group_name": "crown_authority"}],
                 extra={"query_innovation_progress": {"innovations": [{"name": "innovation_hereditary_rule",
                                                                       "is_known": True}]}})
    assert T25.plan(gw2)["levers"]["succession_law"]["allowed_now"] is True


# ── The forecast's two lists agree ───────────────────────────────────────

def test_5d_primary_keeps_no_title_a_younger_share_takes():
    sim = sim_1077()
    sim["heirs"][0]["titles"] = sim["heirs"][0]["titles"] + [T25.county(469, "c_athlone")]   # the 1077 reply

    def tmap(_p):
        m = T25.tmap(sim)
        for t in m["titles"]:
            if t["title_id"] in (469, 501):
                t["projected_heir_id"] = 37111
            if t["title_id"] in (479, 508):
                t["projected_heir_id"] = 37528
        return m
    gw = T25.Gw(dict(MURCHAD_1077), kids_1077(), sim, dynasty=dict(DYN_1077),
                extra={"query_title_succession_map": tmap})
    out = T25.plan(gw)
    takes = {x for s in out["partition_forecast"]["younger_shares"] for x in s.get("takes") or []}
    keeps = {t["title_id"] for t in out["primary_heir"]["keeps_titles"]}
    assert 469 in takes and not (keeps & takes), (keeps, takes)


# ── Free decisions that cost for years ───────────────────────────────────

def test_4a_the_vow_of_poverty_is_named_once_with_its_keyed_veto():
    import test_ai_veto as TV
    news.reset()
    out = TV.two(TV.snap("1066.09.15", []), TV.snap("1066.10.15", []))     # the first news with a diff
    fd = out["free_decisions_ahead"]
    # isolate_family_decision (no cost block, fires a -350 prestige event) is named beside it
    assert [r["decision"] for r in fd["decisions"]] == ["take_vow_of_poverty_decision", "isolate_family_decision"]
    assert "-20%" in fd["decisions"][0]["effect"] and "-350" in fd["decisions"][1]["effect"]
    assert fd["veto"]["args"]["args"] == {"op": "set", "what": "decision",
                                          "keys": ["take_vow_of_poverty_decision", "isolate_family_decision"]}
    again = run(news.block(TV.Gw({}), 0, TV.snap("1066.11.15", []), offers_served=False))
    assert "free_decisions_ahead" not in again
    news.reset()


def test_4b_not_named_when_already_keyed():
    news.reset()
    snap = {"actor_id": 31506, "ai_vetoes": ["decision"], "ai_veto_decision_keys": ["take_vow_of_poverty_decision"]}
    # The unkeyed isolate_family_decision is still named; with both keyed, nothing
    assert [r["decision"] for r in news.free_decisions_ahead(snap)["decisions"]] == ["isolate_family_decision"]
    news.reset()
    snap["ai_veto_decision_keys"] = ["take_vow_of_poverty_decision", "isolate_family_decision"]
    assert news.free_decisions_ahead(snap) is None
    news.reset()


# ── Small ones ───────────────────────────────────────────────────────────

def test_5a_a_county_won_by_war_is_attributed_from_its_own_record_even_a_diff_later():
    import test_ai_veto as TV
    news.reset()
    war = {"war_id": 16777258, "name": "Claim on Athlone", "attacker_id": 31506, "defender_id": 29585,
           "is_attacker": True, "cb_type": "claim_cb"}
    hist = {"wars": [{"war_id": 42, "outcome": "defender_victory", "end_date": "1069.09.24", "target_titles": [508]},
                     {"war_id": 16777258, "outcome": "attacker_victory", "end_date": "1070.11.08",
                      "target_titles": [469]}]}
    gw = TV.Gw({"query_war_history": hist})
    s1 = TV.snap("1070.08.01", [], wars={"wars": [war]}, held={"held_titles": [{"title_id": 463, "key": "c_dublin"}]})
    s2 = TV.snap("1070.11.09", [], wars={"wars": []}, held={"held_titles": [{"title_id": 463, "key": "c_dublin"}]})
    s3 = TV.snap("1071.01.09", [], wars={"wars": []}, held={"held_titles": [
        {"title_id": 463, "key": "c_dublin"}, {"title_id": 469, "key": "c_athlone"}]})
    run(news.block(gw, 0, s1, offers_served=False))
    out2 = run(news.block(gw, 0, s2, offers_served=False))
    assert out2["wars_ended"][0]["outcome"] == "attacker_victory"          # base: war 42's record
    out3 = run(news.block(gw, 0, s3, offers_served=False))
    g, = out3["titles_gained"]
    assert g["how"] == "war 16777258 ended attacker_victory", g
    news.reset()


def test_5b_set_policy_is_not_offered_when_the_mod_is_not_loaded():
    class P:
        async def read(self, a, p=None):
            return {"policies": {"join_liege_wars": False, "stay_out_of_liege_factions": False}, "mod_enabled": False}
    v = run(R.policies_of(P(), 31506))
    assert v["set_with"] is None and "not_offered" in v


def test_5c_a_second_create_of_the_same_faction_is_refused_naming_the_one_he_is_in():
    import factions as F

    class G:
        health = "ok"
        writes = []

        def game(self):
            return {"date": "1078.01.01"}

        async def read(self, a, p=None):
            if a == "query_character":
                return {"id": 31506, "liege_id": 31762}
            if a == "query_factions":
                return {"factions": [{"faction_id": 7, "faction_type": "independence_faction",
                                      "members": [31506, 99]}]}
            return {}

        async def write(self, a, p=None):
            self.writes.append(a)
            return {"success": True}
    g = G()
    out = run(F.do_faction(g, {"action": "create", "faction_type": "independence_faction"}, 31506, {"id": 31506}))
    assert out["ok"] is False and out["blocked"]["raw_reason"] == "already_member" and g.writes == []


def test_2d_the_first_observation_names_who_can_vassalize_now(monkeypatch):
    import threats as TH
    import test_ai_veto as TV
    res = {"you": {"id": 31506, "strength": 1126}, "attackers": [
        {"id": 31762, "name": "Murchad", "tier": 3, "strength": 1086, "x_yours": 1.0,
         "can_declare": ["vassalization_cb"], "vassalization": {"valid_now": True}}], "vassalization": {}}

    async def fake_read(gw, me):
        TH._LAST[me] = {31762: ["vassalization_cb"]}
        return res
    monkeypatch.setattr(TH, "read", fake_read)
    news.reset()
    out = run(news.block(TV.Gw({}), 0, TV.snap("1075.01.01", []), offers_served=False))
    assert out["first_observe"] is True and out["vassalizers_now"]["attackers"][0]["strength"] == 1086
    news.reset()
