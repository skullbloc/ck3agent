"""What a Munster seated run found, each
row replayed from its recorded reply.

- vassalization_cb read `if_you_lose {"gold": "unknown"}`; its defeat cost 350 prestige and 133.88 gold.
- war-defeat gold missing from the purse (1083.08 "unexplained +49.56", 1093.10 "-109.8").
- a partition share at a death read "granted_to_vassal".
- cbs gave no ready call for individual_county_de_jure_cb.
- nine marriage sends, none accepted.
- the kingdom bill named no duchy; the veto list left out vassal_offer.
- turn-one notes written for a count of Dublin.
- a hire into debt with nothing said.
The veto carried at the handoff is the bridge's: test/test_ai_veto.cpp rows 16a-d.
Rules read from the game's own files are checked against the install when one is present, skipped otherwise.
"""
import asyncio
import os
import re
from pathlib import Path

import pytest

import declare_gates as DG
import news
import notes as N
import redesign as R
from test_ireland_walk_replies import Gw

MUNSTER, OSSORY, DESMOND, C_DESMOND, D_MUNSTER, CRONAN, BRIAN = 31762, 32287, 31416, 531, 518, 66896, 36303
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")


def run(c):
    return asyncio.run(c)


def need_game():
    if not (GAME / "common" / "casus_belli_types").is_dir():
        pytest.skip(f"no game install at {GAME}")


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    yield
    news.reset()


# ── Every casus belli a seat was offered has a priced loss ─────────────────────────────

#: the casus belli the recorded runs' cbs / plan_war offered as valid (recorded games
#: on the test host), and the wars they fought
OFFERED = ("claim_cb", "individual_county_de_jure_cb", "vassalization_cb", "make_tributary_cb", "ducal_conquest_cb")


def test_1a_vassalization_loss_is_priced_as_the_recorded_run_paid_it():
    """A seated game, 1068.03.04: the yield took 350 prestige and 133.88 gold (income then about 3.72 a month)."""
    lp = R.loss_price("vassalization_cb", 3.72)
    assert lp["gold"] == -133.9 and lp["prestige"] == -350, lp
    assert "unknown" not in str(lp)


def test_1b_every_offered_casus_belli_has_a_priced_loss():
    for cb in OFFERED:
        lp = R.loss_price(cb, 2.85)
        assert isinstance(lp.get("gold"), (int, float)), (cb, lp)


def test_1c_a_tributary_war_lost_costs_prestige_and_no_gold():
    lp = R.loss_price("make_tributary_cb", 2.85)
    assert lp["gold"] == 0 and lp["prestige"] == -350 and "no reparations" in lp["gold_basis"]


def test_1d_the_liberty_demand_war_has_its_ends_for_the_liege():
    p = R.peace_prices("refused_liege_demand_war", "defender", 2.0, ["we_yield", "white_peace"])
    assert "deposed" in p["we_yield"]["you"] and p["we_yield"]["gold"] == 0
    assert p["white_peace"]["you"].startswith("nothing")


def test_1e_CONTROL_a_casus_belli_with_no_table_says_why():
    p = R.peace_prices("liberty_faction_war", "attacker", 2.0, ["we_yield"])["we_yield"]
    assert p["price"] == "unknown" and "a faction's war" in p["why"]
    assert R.peace_prices("no_such_cb", "attacker", 2.0, ["we_yield"])["we_yield"]["price"] == "unknown"


def test_1f_the_declaration_shows_the_defeat_prestige():
    po = DG.prestige_outcomes({"cb": "vassalization_cb"}, 976.0)
    assert po["defeat"] == -350 and po["white_peace"] == -75 and po["win"] == 0


def test_1g_the_tables_agree_with_the_game_files():
    need_game()
    ct = GAME / "common" / "casus_belli_types"
    v = (ct / "00_vassalization.txt").read_text(encoding="utf-8-sig")
    d = v[v.index("on_defeat = {"):v.index("on_primary_attacker_death")]
    assert "GOLD_VALUE = 3" in d and "value = major_prestige_value" in d
    t = (ct / "00_tributarize.txt").read_text(encoding="utf-8-sig")
    td = t[t.index("on_defeat = {"):t.index("tributary_independence_war")]
    assert "pay_short_term_gold" not in td and "major_prestige_value" in td
    vals = (GAME / "common" / "script_values" / "00_basic_values.txt").read_text(encoding="utf-8-sig")
    assert re.search(r"^major_prestige_value = 350", vals, re.M) and re.search(r"^minor_prestige_value = 75", vals, re.M)


# ── The gold a war's end moved is in the purse ──────────────────────────────────────────

def _purse(prev_gold, now_gold, ended, acts=()):
    for a in acts:
        news.note_act(MUNSTER, a[0], a[1], cost_known=True, gold=a[2])
    prev = {"actor_id": MUNSTER, "date": "1093.03.15", "gold": prev_gold, "monthly_income": 2.72}
    snap = {"actor_id": MUNSTER, "date": "1093.10.13", "gold": now_gold, "monthly_income": 2.72}
    return news._resource_change(prev, snap, "gold", {"wars_ended": ended})


ATHLONE_LOST = [{"war_id": 234881073, "name": "2nd Munsterian Claim on the Earldom of Athlone", "my_side": "attacker",
                 "outcome": "defender_victory", "attacker_id": BRIAN, "defender_id": 34580, "_gold_years": 3}]


def test_2a_a_lost_war_is_priced_although_an_older_end_against_him_was_recorded():
    """A seated game, 1093.10: the surrender's step read "unexplained -109.8": an end of an EARLIER war against 34580 (the
    white peace of 1080) was among the recorded acts, so the defeat was taken as counted already."""
    g = _purse(85.27, -5.41, ATHLONE_LOST, acts=[("1080.08.14", "ended the war against 34580: white_peace", -0.0001)])
    assert any("war lost as its attacker" in e["what"] for e in g.get("in_this_step") or []), g


def test_2a_CONTROL_an_end_recorded_in_this_step_with_its_gold_is_not_counted_twice():
    g = _purse(85.27, -5.41, ATHLONE_LOST, acts=[("1093.03.15", "ended the war against 34580: surrender", -106.0)])
    assert not any("war lost" in e["what"] for e in g.get("in_this_step") or []), g
    assert g["your_acts"] == [{"what": "ended the war against 34580: surrender", "gold": -106.0}]


def test_2b_the_end_records_the_charge_read_at_the_end_over_the_table_price():
    """A seated game, 1083.05.10: we_yield recorded -171.0 (3 x 12 x 4.75, the income read); the game took about 121."""
    ch = R.war_end_charged({"gold": 24.67, "prestige": 1100.0}, {"gold": -96.25, "prestige": 1085.0})
    assert ch["gold"] == -120.92 and ch["prestige"] == -15.0
    R.note_war_end_act(BRIAN, "1083.05.10", "ended the war against 32287: we_yield", {"gold": -171.0}, ch)
    (a,) = news._ACTS[BRIAN]
    assert a["gold"] == -120.92 and a["measured"] is True


def test_2b_CONTROL_with_no_read_the_table_price_is_recorded():
    R.note_war_end_act(BRIAN, "1083.05.10", "ended the war against 32287: we_yield", {"gold": -171.0}, None)
    assert news._ACTS[BRIAN][0]["gold"] == -171.0 and "measured" not in news._ACTS[BRIAN][0]


def test_2c_every_basis_pricing_reparations_from_the_monthly_income_says_it_is_an_estimate():
    """Each served row whose gold is years x 12 x the monthly income read says that figure is this tool's estimate,
    which can differ from the game's yearly-income charge: the news rows (a lost war, a moved loss price) and the
    peace-price and loss-price rows of every casus belli with reparations."""
    said = re.compile(r"estimate.*can differ", re.S)
    inc, rows = 2.72, []
    for e in _purse(85.27, -5.41, ATHLONE_LOST).get("in_this_step") or []:
        if e.get("expected") == -round(3 * 12 * inc, 2):
            rows.append(("war lost", e["expected"], e["basis"]))
    wars = {7: {"war_id": 7, "name": "Claim on Athlone", "attacker_id": MUNSTER, "is_attacker": True,
                "cb_type": "claim_cb"}}
    news._loss_price_moved({"actor_id": MUNSTER, "date": "1073.10.29", "wars": wars, "monthly_income": 3.33})
    for m in news._loss_price_moved({"actor_id": MUNSTER, "date": "1074.09.24", "wars": wars, "monthly_income": inc}):
        rows.append(("loss price moved", m["if_you_lose_now"], m["basis"]))
    assert [r[0] for r in rows] == ["war lost", "loss price moved"], rows
    for cb, row in R.PEACE_PRICE.items():
        yrs = row["gold_years"]
        if not yrs:
            continue
        p = R.peace_prices(cb, "attacker", inc, ["we_yield"])["we_yield"]
        assert p["gold"] == -round(yrs * 12 * inc, 1), cb
        rows.append((cb, p["gold"], p["gold_basis"]))
        lp = R.loss_price(cb, inc)
        assert lp["gold"] == p["gold"], cb
        rows.append((cb + " loss_price", lp["gold"], lp["gold_basis"]))
    for what, gold, basis in rows:
        assert said.search(basis), (what, gold, basis)


# ── A partition share is named as one ───────────────────────────────────────────────────

def _lost_gw(father):
    return Gw({"query_title_hierarchy": {"title": {"holder_id": CRONAN}},
               "query_character": lambda p: {"id": p["character_id"], "alive": True, "liege_id": BRIAN,
                                             "top_liege_id": BRIAN, "father_id": father}})


def test_3a_a_county_to_the_dead_rulers_child_at_the_succession_is_his_share():
    """A seated game, 1069.09.27: c_ennis to the infant Cronan 66896 read `"how": "granted_to_vassal"`."""
    how = run(news._lost_to(_lost_gw(MUNSTER), BRIAN, 524, None, MUNSTER))
    assert how["cause"] == "partition" and "younger child 66896" in how["how"] and "still_yours" in how


def test_3a_CONTROL_without_a_ruler_change_it_is_a_grant():
    how = run(news._lost_to(_lost_gw(MUNSTER), BRIAN, 524, None, None))
    assert how["how"] == "granted_to_vassal"


# ── cbs gives the de jure county war its ready call ─────────────────────────────────────

def _dejure_gw(desmond_holder=DESMOND):
    def hier(p):
        if p.get("character_id"):
            return {"held_titles": [{"title_id": D_MUNSTER, "key": "d_munster", "tier": 3},
                                    {"title_id": 524, "key": "c_thomond", "tier": 2}]}
        if p.get("title_id") == D_MUNSTER:
            return {"title": {"key": "d_munster", "tier": 3, "sub_titles": [
                {"title_id": 524, "key": "c_thomond", "tier": 2, "holder_id": MUNSTER},
                {"title_id": C_DESMOND, "key": "c_desmond", "tier": 2, "holder_id": desmond_holder},
                {"title_id": 409, "key": "c_ormond", "tier": 2, "holder_id": 30822}]}}
        return {}

    def char(p):
        cid = p["character_id"]
        top = {30822: MUNSTER, 777: 999}.get(cid, cid)
        return {"id": cid, "alive": True, "liege_id": 0 if top == cid else top, "top_liege_id": top}
    return Gw({"query_title_hierarchy": hier, "query_character": char})


def test_5a_the_de_jure_county_war_has_its_county():
    tr = run(R.title_rows(_dejure_gw(), MUNSTER, DESMOND, "individual_county_de_jure_cb"))
    assert tr["titles"] == [{"title": "c_desmond", "cb_title_id": C_DESMOND, "de_jure_of": "d_munster",
                             "holder_id": DESMOND}], tr
    assert "00_dejure_war.txt" in tr["rule"]


def test_5a_CONTROL_a_county_not_in_his_realm_is_not_his():
    tr = run(R.title_rows(_dejure_gw(desmond_holder=777), MUNSTER, DESMOND, "individual_county_de_jure_cb"))
    assert tr["titles"] == []


def test_5b_the_cbs_row_carries_the_ready_call():
    opt = {"cb": "individual_county_de_jure_cb", "needs_cb_title_id": True, "cb_title_id": 0,
           "declare_args": {"target_id": DESMOND, "cb": "individual_county_de_jure_cb", "cb_title_id": 0}}
    tr = run(R.title_rows(_dejure_gw(), MUNSTER, DESMOND, "individual_county_de_jure_cb"))
    R.title_declare_rows(opt, DESMOND, tr)
    assert opt["declare_args"] == {"target_id": DESMOND, "cb": "individual_county_de_jure_cb",
                                   "cb_title_id": C_DESMOND, "claimant_id": None}


def test_5c_the_rule_is_the_game_files():
    need_game()
    t = (GAME / "common" / "casus_belli_types" / "00_dejure_war.txt").read_text(encoding="utf-8-sig")
    blk = t[t.index("individual_county_de_jure_cb = {"):t.index("individual_duchy_de_jure_cb = {")]
    assert "target_de_jure_regions_above = yes" in blk and "tier = tier_county" in blk
    assert "this = scope:attacker" in blk


# ── Marriage ────────────────────────────────────────────────────────────────────────────

def _marriage_read(sex_reply=True):
    people = {BRIAN: {"id": BRIAN, "name": "Brian", "alive": True, "is_female": False, "age": 35, "spouses": [],
                      "children": []},
              900: {"id": 900, "alive": True, "is_female": True, "age": 22, "traits": []},
              901: {"id": 901, "alive": True, "is_female": True, "age": 23, "is_imprisoned": True, "traits": []},
              902: {"id": 902, "alive": True, "is_female": True, "age": 23, "traits": [{"name": "devoted"}]},
              800: {"id": 800, "alive": True, "is_female": False, "age": 30, "traits": []}}
    calls = []

    async def read(action, params):
        calls.append((action, dict(params)))
        if action == "query_character":
            return people.get(params["character_id"], {})
        if action == "query_marriage_candidates":
            if not sex_reply:
                return {"actor_id": BRIAN, "candidates": [{"id": 800, "age": 30}]}
            return {"actor_id": BRIAN, "sex": params.get("sex"), "sort": "military", "candidates": [
                {"id": 901, "age": 23}, {"id": 902, "age": 23}, {"id": 900, "age": 22}]}
        return {}
    return read, calls


def test_6a_a_male_seat_is_listed_women_the_game_would_take():
    """A seated game, 1083.08.24 / 1084.07.29: the ten rows returned were all men; candidates []."""
    read, calls = _marriage_read()
    out = run(R.marriage_rows({"actor_id": BRIAN, "sort": "military", "require_in_range": True,
                               "candidates": [{"id": 800, "age": 30}]}, read))
    assert [c["id"] for c in out["candidates"]] == [900] and out["sex"] == "female"
    assert ("query_marriage_candidates", {"character_id": BRIAN, "sort": "military", "sex": "female",
                                          "skip_republics": 1, "max_results": 10,
                                          "require_in_range": True}) in calls
    nl = out["not_listed"]
    assert any(k.startswith("imprisoned") for k in nl) and any("devoted" in k for k in nl)


def test_6a_CONTROL_a_bridge_without_the_filter_says_so():
    read, _ = _marriage_read(sex_reply=False)
    out = run(R.marriage_rows({"actor_id": BRIAN, "candidates": [{"id": 800, "age": 30}]}, read))
    assert out["candidates"] == [] and out["sex"].startswith("not filtered")


def test_6b_a_refused_or_misnamed_marriage_names_who_decides_and_the_listing():
    env = {"verdict": "rejected", "blocked": {"raw_reason": "interaction_not_valid_for_target",
                                              "detail": R.NO_GAME_DETAIL}}
    R._explain_marriage_refusal(env, {"interaction": "arrange_marriage_interaction", "target_id": 36124})
    assert "the ruler whose court he is in" in env["blocked"]["marriage_rule"]
    assert env["blocked"]["who_can_be_asked"]["args"] == {"domain": "marriage_candidates"}
    env2 = {"verdict": "rejected", "blocked": {"raw_reason": "interaction_not_found"}}
    R._explain_marriage_refusal(env2, {"interaction": "marry_interaction"})
    assert env2["blocked"]["did_you_mean"] == "arrange_marriage_interaction"


def test_6c_the_refusals_are_the_game_files():
    need_game()
    t = (GAME / "common" / "character_interactions" / "00_marriage_interactions.txt").read_text(encoding="utf-8-sig")
    assert "is_imprisoned = no" in t and "is_at_war_with = scope:recipient" in t
    tr = (GAME / "common" / "traits" / "00_traits.txt").read_text(encoding="utf-8-sig")
    for k in R.CAN_NOT_MARRY_TRAITS:
        blk = tr[tr.index(f"\n{k} = {{"):]
        assert "can_not_marry" in blk[:blk.index("\n}")], k


# ── The kingdom bill, the veto list ─────────────────────────────────────────────────────

def test_7a_an_unpriced_bill_names_the_duchies_it_read():
    king = {"title_key": "k_ireland", "price": {"cost": 425.0, "currency": "gold"},
            "requirements": [{"requirement": "tier_rule", "met": False,
                              "one_of": [{"tier": "duchy", "need": 2, "have": 1}]}]}
    lein = {"title_key": "d_leinster", "price": {"cost": None},
            "blocked": {"gate": "engine_refused", "summary": "d_leinster has a holder"}}
    R.attach_tier_bill([king, lein])
    bill = king["requirements"][0]["bill"]
    assert bill["cost"] is None and bill["duchies_read"] == [{"title_key": "d_leinster",
                                                              "why_no_price": "d_leinster has a holder"}]


def test_7a_CONTROL_with_nothing_read_it_says_none():
    king = {"title_key": "k_ireland", "price": {"cost": 425.0, "currency": "gold"},
            "requirements": [{"requirement": "tier_rule", "met": False,
                              "one_of": [{"tier": "duchy", "need": 2, "have": 1}]}]}
    R.attach_tier_bill([king])
    assert king["requirements"][0]["bill"]["duchies_read"].startswith("none")


def test_7b_the_veto_list_names_vassal_offer():
    assert "vassal_offer" in news.ai_spends(levers_only=True)["veto"]["args"]["args"]["what"]
    assert news.ai_spends()["vassal_offers"]["lever"]["args"]["args"]["what"] == "vassal_offer"


# ── The turn-one notes hold for a count of Dublin and a duke of Munster ─────────────────

def test_8a_the_notes_state_rank_dependent_prices_and_levers_as_such():
    doc = N.derive(N.load(), "ireland1066")
    text = " ".join(N.turn_one_texts(doc))
    assert "50 prestige as a count, 100 above" in text and "50-prestige" not in text
    # In one test series every seat vetoes create_title on turn one and mints its duchies and k_ireland itself (the AI
    # spent the ruler's gold on d_meath and k_ireland at its own time; the seat's own mint_title proven live)
    assert "create_title only as a count" not in text and "do ai_veto what=spending" in text
    assert "do intent=mint_title args.title_key=k_ireland" in text and "2 duchy titles" in text
    assert "convert_to_local_culture_decision]" not in text
    assert N.problems(doc) == []


def test_8b_the_prices_are_the_game_files():
    need_game()
    t = (GAME / "common" / "casus_belli_types" / "00_conquest.txt").read_text(encoding="utf-8-sig")
    blk = t[t.index("ducal_conquest_cb = {"):]
    blk = blk[:blk.index("on_declaration")]
    assert "tier <= tier_county" in blk and "value = 50" in blk and "value = 100" in blk


# ── A hire into debt says so where it is chosen and sent ────────────────────────────────

def test_9a_a_row_the_purse_does_not_cover_says_the_debt():
    """A seated game, 1078.08.16: company 55 listed affordable at 194.99 with 143 gold; charged 214."""
    d = R.merc_debt_after(194.99, 143.0, 2.4)
    assert d["gold_after"] == -51.99 and d["debt"].startswith("in debt: no declaration is possible")


def test_9a_CONTROL_a_covered_row_says_nothing():
    assert R.merc_debt_after(120.0, 143.0, 2.4) is None


# ── A declaration's prestige is what the game took ───────────────────────────────────

def _declare(prestige_after):
    import test_script_actions as SA

    class G(SA.Gw):
        async def read(self, action, params=None):
            r = await super().read(action, params)
            if action == "query_character" and self.state["wars"]:
                r = dict(r, prestige=prestige_after)
            return r

    def war(st, p):
        if p.get("template") != "declare_war":
            return                     # the activity listing read before the send is a script too
        st["wars"].append({"war_id": 77, "attacker_id": SA.ME, "defender_id": SA.FOE, "cb_type": "claim_cb",
                           "target_titles": [1141], "name": "War"})
    gw = G(SA.ran("declare_war"), on_write=war)
    env = run(R.declare_war_impl(gw, SA.ME, target_id=SA.FOE, cb="claim_cb", cb_title_id=1141,
                                 confirm_straddle=True))
    return env, news._ACTS.get(SA.ME) or []


def test_7c_the_declaration_records_the_prestige_the_game_took():
    """A live read of the 1066 save: two declarations priced 100 and 50 took 321.84 (the step's "unexplained" in 1067.08)."""
    env, acts = _declare(85.5)
    assert env["verdict"] == "applied"
    assert env["result"]["declare_cost"]["charged"]["prestige"] == -214.5
    (a,) = acts
    assert a["prestige"] == -214.5 and a["measured"] is True


def test_7c_CONTROL_no_change_read_keeps_the_estimate_path():
    env, acts = _declare(300)
    assert "charged" not in (env["result"].get("declare_cost") or {})
    assert all("measured" not in a for a in acts)


# ── the AI's hires and the reserve ───────────────────────────────────────────────────

def test_A_the_ai_hire_row_names_the_veto_not_the_reserve():
    lv = news.ai_spend_veto("mercenary")
    assert lv["args"] == {"intent": "ai_veto", "args": {"op": "set", "what": "mercenary"}}
    # the reason as a rule (a reserve set above the gold held does not stop a hire), no one game's figures
    assert "above the gold held" in lv["not_gold_reserve"] and "238.74" not in lv["not_gold_reserve"]
    assert news.ai_spends()["buildings_and_mercenaries"]["lever"]["args"]["intent"] == "ai_veto"


#: a live read of the 1089 save (1089.01.01): the simulation's titles (d_munster primary, d_ulster held) and the kingdom
SIM_1089 = {"primary_title_id": 518, "heirs": [
    {"character_id": BRIAN, "titles": [{"title_id": 518, "tier": 3, "key": "d_munster"},
                                       {"title_id": 519, "tier": 2, "key": "c_thomond"},
                                       {"title_id": 524, "tier": 2, "key": "c_ennis"}]},
    {"character_id": 71992, "titles": [{"title_id": 473, "tier": 3, "key": "d_ulster"}]}]}


def _b_gw(leinster_held=2):
    def hier(p):
        if p.get("title_id") == 518:
            return {"title": {"key": "d_munster", "tier": 3, "de_jure_parent_id": 450}}
        if p.get("title_id") == 450:
            return {"title": {"key": "k_ireland", "tier": 4, "sub_titles": [
                {"title_id": 518, "key": "d_munster", "tier": 3, "holder_id": MUNSTER},
                {"title_id": 473, "key": "d_ulster", "tier": 3, "holder_id": MUNSTER},
                {"title_id": 505, "key": "d_leinster", "tier": 3, "holder_id": 0},
                {"title_id": 490, "key": "d_meath", "tier": 3, "holder_id": 0}]}}
        return {}

    def prog(p):
        return {"held": leinster_held, "total": 3} if p["title_id"] == 505 else {"held": 0, "total": 3}
    return Gw({"query_title_hierarchy": hier, "query_dejure_progress": prog})


def _b_forecast(gw):
    import partition_forecast as PF
    shares = run(R.tier_shares_for(gw, {"id": MUNSTER, "ruler_tier": 3},
                                   ["confederate_partition_succession_law", "male_preference_law"], SIM_1089))
    kids = [{"id": BRIAN, "alive": True, "is_female": False, "traits": []},
            {"id": 71992, "alive": True, "is_female": False, "traits": []},
            {"id": 64065, "alive": True, "is_female": False, "traits": []}]
    return shares, PF.forecast_from_reads(ruler={"ruler_tier": 3}, law_names=[
        "confederate_partition_succession_law", "male_preference_law"], children=kids, sim=SIM_1089,
        primary_heir_id=BRIAN, tier_shares=shares)


def test_B_a_dukes_younger_sons_take_his_held_and_created_duchies():
    """A seated game, 1081, and a live read of the 1089 save: 71992 'takes []', 64065 'no county left'; the game: d_ulster and a created d_leinster."""
    shares, fc = _b_forecast(_b_gw())
    assert [s["title"] for s in shares] == ["d_ulster", "d_leinster"] and shares[1]["created"] is True
    ys = {s["id"]: s for s in fc["younger_shares"]}
    assert set(ys) == {71992, 64065} and all(s["fate"] == "leaves_realm" for s in ys.values())
    assert ys[64065]["takes"] == ["d_leinster"] and not fc["excluded_children"]


def test_B_CONTROL_a_duchy_below_the_creation_bar_is_no_share():
    shares, fc = _b_forecast(_b_gw(leinster_held=1))
    assert [s["title"] for s in shares] == ["d_ulster"]
    assert [e["id"] for e in fc["excluded_children"]] == [64065]


def test_D_the_activity_is_named_and_its_refusal_is_its_own_class():
    import activities as A
    a = A.in_activity(["IN_ACTIVITY", "IN_T_2"])
    assert a["type"] == "activity_coronation" and "end date is not read" in a["ends"]
    blk = R.why_blocked("in_activity", summary="x", raw_reason="involved_activity")
    assert blk["gate"] == "in_activity" and blk["retryable"] is True


def test_D_the_activity_list_is_the_bridges():
    import activities as A
    h = (Path(__file__).resolve().parents[2] / "bridge" / "include" / "script_templates.h").read_text()
    body = h[h.index("kInvolvedActivityTypes[] = {"):]
    body = body[:body.index("};")]
    assert tuple(re.findall(r'"(activity_[a-z_]+)"', body)) == A.INVOLVED_ACTIVITY_TYPES
    if (GAME / "common" / "activities" / "activity_types").is_dir():
        keys = set()
        for f in (GAME / "common" / "activities" / "activity_types").glob("*.txt"):
            keys |= set(re.findall(r"^(activity_[a-z_]+) = \{", f.read_text(encoding="utf-8-sig"), re.M))
        assert keys == set(A.INVOLVED_ACTIVITY_TYPES)


def test_F_nobody_who_can_vassalize_is_said():
    import threats as TH

    async def no_one(gw, me):
        TH._LAST[me] = {}
        return {"attackers": [{"id": 5, "can_declare": []}], "you": {"strength": 800}}
    import pytest as _p
    mp = _p.MonkeyPatch()
    mp.setattr(TH, "read", no_one)
    try:
        row = run(news.threat_rows(Gw({}), {"actor_id": MUNSTER, "date": "1066.09.15"}, "observe"))
    finally:
        mp.undo()
    assert row == {"attackers": "none", "neighbours_read": 1}


def test_F_a_vassal_who_joined_by_the_ais_offer_is_named():
    prev = {"actor_id": MUNSTER, "realm_counties": 6, "ai_vassal_offers_sent": [{"count": 2, "last": "1069.11.01"}]}
    snap = {"actor_id": MUNSTER, "realm_counties": 7, "ai_vassal_offers_sent": [{"count": 3, "last": "1070.02.20"}]}
    row = news.vassals_joined(prev, snap, {})
    assert row["cause"] == "your_ai_offered_vassalage" and "1 offer(s)" in row["how"]
    snap2 = dict(snap, ai_vassal_offers_sent=prev["ai_vassal_offers_sent"])
    assert "cause_unread" in news.vassals_joined(prev, snap2, {})
    won = {"wars_ended": [{"outcome": "attacker_victory", "my_side": "attacker"}]}
    assert news.vassals_joined(prev, snap, won) is None


def test_E_a_dukes_ducal_conquest_row_has_its_county():
    """A recorded read: cbs gave Munster (a duke) ducal_conquest_cb with cb_title_id 0; the rule reads
    the realm's counties and the duchy's holder, not the attacker's tier."""
    ct = {"duchies": [{"duchy_id": 505, "duchy_key": "d_leinster", "partially_held": True, "held_by_ruler_count": 1,
                       "remaining_counties": [{"county_id": 508, "county_key": "c_leinster", "holder_id": 37111,
                                               "holder_top_liege_id": 37111, "adjacent": True}]}]}
    gw = Gw({"query_conquest_targets": ct, "query_title_hierarchy": {"title": {"holder_id": 0}}})
    tr = run(R.title_rows(gw, MUNSTER, 37111, "ducal_conquest_cb"))
    assert tr["titles"] == [{"title": "c_leinster", "cb_title_id": 508, "duchy": "d_leinster"}]


def _snap(actor, date):
    return news.snapshot(actor_id=actor, date=date, chr_sect={"gold": 0.0, "prestige": 0.0, "piety": 0.0},
                         wars={"wars": []}, claims={"claims": []}, council={"council": []}, heir=None,
                         schemes={"schemes": []}, held={"held_titles": []}, council_tasks=[],
                         laws={"active_laws": []}, alliances={"alliances": []})


def test_B_the_counties_that_left_at_a_succession_are_named():
    """A recorded game, 1089.02: 9 counties -> 5 at Murchad's death and no news row named the four."""
    reads = {"n": 0}

    def prog(p):
        reads["n"] += 1
        ids = [519, 524, 473, 485, 508, 513] if reads["n"] == 1 else [519, 524]
        return {"counties": [{"county_id": c, "holder_id": 1, "you_control": True} for c in ids]}
    gw = Gw({"query_dejure_progress": prog,
             "query_title_hierarchy": lambda p: {"title": {"key": f"c_{p.get('title_id')}", "holder_id": 71992}}})
    run(news.block(gw, 0, _snap(MUNSTER, "1089.01.01"), offers_served=False, fabricate_within="k_ireland"))
    out = run(news.block(gw, 0, _snap(BRIAN, "1089.12.27"), offers_served=False, fabricate_within="k_ireland"))
    left = out["ruler_changed"]["counties_left_realm"]
    assert (left["was"], left["now"]) == (6, 2) and [c["title_id"] for c in left["counties"]] == [473, 485, 508, 513]
    assert left["counties"][0]["holder_id"] == 71992 and "partition" in left["why"]
