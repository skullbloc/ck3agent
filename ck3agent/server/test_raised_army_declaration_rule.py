"""The toolkit defects of the first 150-turn (30 game-year) seated game of
the Dublin seat, each row replayed from its recorded reply.

The fixture (fixtures/raised_army_replies.json.gz) holds whole tool replies and per-step news recorded
in that run, keyed by what they are; `line` is the record's line in the
evidence trajectory. A value the recording does not hold is written into the test as such,
never into the fixture. Rules read from the game's own files are checked against the install
when one is present (CK3_GAME_DIR, or the default Steam path) and skipped otherwise.
"""
import asyncio
import gzip
import json
import os
from pathlib import Path

import pytest

import declare_gates as DG
import news
import redesign as R
import test_script_actions as SA
from test_ireland_walk_replies import Gw, cbs_by_target

HERE = Path(__file__).resolve().parent
BRIDGE = HERE / ".." / ".." / "bridge"
LIVE = json.loads(gzip.open(HERE / "fixtures" / "raised_army_replies.json.gz", "rt").read())
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
ME, MAYO_LORD, MUNSTER, C_MAYO, C_AILECH = 31506, 34580, 31762, 491, 485


def run(c):
    return asyncio.run(c)


def reply(name):
    return LIVE[name]["reply"]["result"]


def news_at(date):
    return LIVE["news_" + date.replace(".", "_")]["news"]


def game_file(rel: str) -> str:
    p = GAME / rel
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    return p.read_text(encoding="utf-8-sig", errors="replace")


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


# ── 1: five years of claim declarations refused "the game gives no detail" ──────────────

MAYO_CBS_TOOL = reply("cbs_34580_1081_09_15")["result"]
DA_MAYO = {"cb": "claim_cb", "cb_title_id": C_MAYO, "claimant_id": ME, "target_id": MAYO_LORD}


def _mayo_cbs_raw():
    """query_cbs_for_target as the recorded cbs(34580) of 1081.09.15 shows it: claim_cb valid
    (claim on c_mayo) and make_tributary_cb valid; the bridge's own rows price claim_cb 0."""
    return {"status": "ok", "target_id": MAYO_LORD, "prestige": 79.78, "piety": 748.55,
            "target_state": MAYO_CBS_TOOL["target_state"],
            "cbs": [{"cb_key": "claim_cb", "valid": True, "scope": "county", "target_mode": 6,
                     "cb_title_id": C_MAYO, "claimant_id": ME, "family": "claim",
                     "cost_prestige": 0, "cost_piety": 0, "affordable": True},
                    {"cb_key": "make_tributary_cb", "valid": True, "scope": "realm", "target_mode": 0,
                     "family": "other", "cost_prestige": 50, "cost_piety": 0, "affordable": True}]}


ME_1081 = {"id": ME, "name": "Murchad", "alive": True, "age": 56, "is_female": False, "liege_id": 0,
           "top_liege_id": ME, "gold": 87.47, "prestige": 204.47, "piety": 771.65,
           "faith_key": "catholic", "dynasty_id": 0, "ruler_tier": 3,
           "income_breakdown": {"total_monthly": 5.6}}
MAYO_1082 = dict(reply("inspect_34580_1082_04_22")["result"])


def _declare_gw(armies, *, guard="refused", marks=None, me=ME_1081):
    class G(SA.Gw):
        async def read(self, action, params=None):
            p = dict(params or {})
            if action == "query_character":
                self.reads.append((action, p))
                return me if p.get("character_id") in (ME, 0, None) else MAYO_1082
            if action == "query_armies":
                self.reads.append((action, p))
                return armies
            if action == "query_claims":
                self.reads.append((action, p))
                return {"claims": [{"title_id": C_MAYO, "type": "unpressed", "holder_id": MAYO_LORD}]}
            if action in ("query_cbs_for_targets", "query_cbs_for_target"):
                self.reads.append((action, p))
                return ({"error": "unknown_action"} if action == "query_cbs_for_targets"
                        else dict(_mayo_cbs_raw(), target_id=p.get("target_id")))
            return await super().read(action, params)
    return G(SA.ran("declare_war", guard=guard, **({"marks": marks} if marks is not None else {})))


ARMIES_RAISED = reply("armies_1082_02_08")["result"]        # 1 army, 1,680 men, at peace
ARMIES_NONE = {"armies": [], "army_count": 0, "character_id": ME}


def test_1_the_recorded_blackout():
    """1081.09.15 to 1085.11.27: ten claim declarations refused can_declare_war_false with
    every condition this server checked passing, while cbs listed claim_cb valid and best; the
    one war-validity read of those years (1082.02.08) failed A4 no_raised_armies: an army of
    1,680 men raised with no war running. The same declare applied 1086.07.04."""
    for k in ("declare_mayo_1081_09_15", "declare_mayo_1084_07_04", "declare_ailech_1084_07_04",
              "declare_mayo_1085_11_27"):
        b = reply(k)["blocked"]
        assert (b["gate"], b["raw_reason"], b["detail"]) == \
            ("cb_invalid_for_target", "can_declare_war_false", "the game gives no detail"), k
        assert all(v["ok"] for v in b["checked_before_sending"].values() if isinstance(v, dict)), k
    assert MAYO_CBS_TOOL["best"] == "claim_cb"
    a4 = next(c for c in reply("war_validity_34580_1082_02_08")["result"]["conditions"] if c["id"] == "A4")
    assert (a4["condition"], a4["pass"]) == ("no_raised_armies", False)
    assert "max_troops=1680" in a4["detail"]
    (army,) = ARMIES_RAISED["armies"]
    assert (army["commander_id"], army["troop_count"]) == (ME, 1680)
    assert reply("declare_mayo_1086_07_04")["verdict"] == "applied"
    assert news_at("1081.09.15").get("wars_started") is None


def test_1_the_games_rule_refuses_a_declaration_with_raised_armies():
    """declare_war_interaction's is_valid_showing_failures_only: NOT has_raised_armies unless
    the government has can_start_war_with_raised_troops, which only the nomads' has."""
    f = game_file("common/character_interactions/00_war.txt")
    body = f[f.index("is_valid_showing_failures_only"):][:900]
    assert "can_start_war_with_raised_troops" in body and "has_raised_armies = yes" in body
    gov = game_file("common/governments/00_government_types.txt")
    import re
    i = gov.index("can_start_war_with_raised_troops")
    owners = re.findall(r"^(\w+) = \{", gov[:i], re.M)
    assert owners[-1] == "nomad_government" and gov.count("can_start_war_with_raised_troops") == 1


def test_1_raised_armies_refuse_the_declaration_before_it_is_sent():
    """The game refused every declaration sent with
    armies raised (one recorded game three times, another at 0867.06.30: its RAISED_ARMIES mark), so raised armies are a
    declare_gates refusal, the one the cbs and claim rows read too; nothing is sent."""
    gw = _declare_gw(ARMIES_RAISED, marks=["WHY_RAISED_ARMIES"])
    b = run(R.declare_war_impl(gw, ME, declare_args=dict(DA_MAYO), confirm_straddle=True))["blocked"]
    assert len(SA.scripts_sent(gw)) == 0
    assert (b["gate"], b["fix"]) == ("armies_raised", {"tool": "inspect", "args": {"subject": "armies"}})
    chk = b["measured"]["checked"]["armies_raised"]
    assert (chk["raised"], chk["men"], chk["ok"]) == (True, 1680, False)


def test_1_CONTROL_declare_without_raised_armies_is_sent():
    """Non-discriminating control: no army raised, the declare goes to the game."""
    gw = _declare_gw(ARMIES_NONE, guard="ran")
    run(R.declare_war_impl(gw, ME, declare_args=dict(DA_MAYO), confirm_straddle=True))
    assert len(SA.scripts_sent(gw)) == 1


def test_1_one_verdict_cbs_and_news_say_the_same_armies_reading():
    gw = Gw(cbs_by_target(_mayo_cbs_raw()), date="1082.02.08")
    gw.replies["query_character"] = lambda p: ME_1081 if p.get("character_id") == ME else MAYO_1082
    gw.replies["query_armies"] = ARMIES_RAISED
    gw.replies["query_claims"] = {"claims": [{"title_id": C_MAYO, "type": "unpressed"}]}
    res = run(R.cbs_impl(gw, target_id=MAYO_LORD, character_id=ME))["result"]
    # Raised armies are a refusal in both reads (blocked_now in cbs, declarable_now false in the claim row)
    (o,) = [x for x in res.get("blocked_now") or [] if x["cb"] == "claim_cb"]
    assert [b["condition"] for b in o["blocked_by"]] == ["armies_raised"] and o["blocked_by"][0]["have"] == 1680
    war: dict = {}
    nxt = run(news._claim_next(gw, ME, {"title_id": C_MAYO, "holder_id": MAYO_LORD}, war))
    assert nxt["tool"] == "cbs" and "armies are raised" in nxt.get("why", ""), nxt
    assert war["declarable_now"] is False and [g["condition"] for g in war["gates"]] == ["armies_raised"]
    gw.replies["query_armies"] = ARMIES_NONE
    res = run(R.cbs_impl(gw, target_id=MAYO_LORD, character_id=ME))["result"]
    assert "claim_cb" in [x["cb"] for x in res["valid_options"]]
    nxt = run(news._claim_next(gw, ME, {"title_id": C_MAYO, "holder_id": MAYO_LORD}, war))
    assert nxt["tool"] == "declare_war" and war["declarable_now"] is True


def test_1_the_game_names_its_failing_condition_in_the_refused_branch():
    """The refused branch's WHY marks (the template asks each condition of declare_war's
    validity) become the gate; no WHY mark says none of those failed."""
    gw = _declare_gw(ARMIES_NONE, marks=["WHY_RAISED_ARMIES"])
    b = run(R.declare_war_impl(gw, ME, declare_args=dict(DA_MAYO), confirm_straddle=True))["blocked"]
    assert (b["gate"], b["raw_reason"]) == ("armies_raised", "can_declare_war_false"), b
    assert "armies are raised" in b["summary"]
    gw = _declare_gw(ARMIES_NONE, marks=[])
    b = run(R.declare_war_impl(gw, ME, declare_args=dict(DA_MAYO), confirm_straddle=True))["blocked"]
    assert b["gate"] == "cb_invalid_for_target" and b["game_checked"]["failed"] == []
    assert "none of those conditions failed" in b["game_checked"]["none_failed"]


def test_1_the_bridge_asks_every_condition_the_server_reads():
    """One list, two readers: the template's WHY words (script_templates.h kDeclareWhy) are
    exactly the keys DECLARE_WHY maps, and each trigger is the game file's own wording."""
    h = (BRIDGE / "include" / "script_templates.h").read_text()
    body = h[h.index("kDeclareWhy[] = {"):h.index("};", h.index("kDeclareWhy[] = {"))]
    import re
    words = re.findall(r'\{"([A-Z_0-9]+)", "([^"]+)"\}', body)
    assert {w for w, _t in words} == set(R.DECLARE_WHY), words
    f = game_file("common/character_interactions/00_war.txt")
    # is_shown, is_valid_showing_failures_only and has_valid_target (the defender's side is asked too)
    valid = f[:f.index("send_options_exclusive")]
    for w, trig in words:
        core = trig.replace("NOT = { ", "").replace(" }", "").replace(" D", " scope:recipient")
        # The defender's side is asked as `D = { ... }`, the liege's war as `liege ?= { ... }`
        for head in ("D = { ", "liege ?= { ", "culture = { "):
            core = core[len(head):] if core.startswith(head) else core
        if w == "PRESTIGE_BELOW_0":
            continue            # not in the interaction: the casus belli's cost cannot be paid below 0
        assert core.split(" = ")[0].split(" ?= ")[0] in valid, (w, core)


# ── 2: yielding a war stated no price ─────────────────────────────────────────────────────

from test_end_war import gw_with as _war_gw, _war as _war_row  # noqa: E402


def _priced_war_gw(*, terms_marks, cb="claim_cb", monthly=6.63, ended=None):
    gw = _war_gw(SA.ran("end_war", guard="ran"), marks=terms_marks,
                 wars=[_war_row(atk=SA.ME, dfn=SA.FOE)], ended=ended or [])
    base_read = gw.read

    async def read(action, params=None):
        if action == "get_active_wars":
            gw.reads.append((action, dict(params or {})))
            return {"wars": [{"war_id": 41, "attacker_id": SA.ME, "defender_id": SA.FOE, "cb_type": cb}]}
        if action == "query_character" and (params or {}).get("character_id") == SA.ME:
            gw.reads.append((action, dict(params or {})))
            return {"id": SA.ME, "gold": 53.11, "prestige": 456.46, "piety": 1042.0,
                    "income_breakdown": {"total_monthly": monthly}}
        return await base_read(action, params)
    gw.read = read
    return gw


def test_2_the_recorded_yield_had_no_price():
    """1086.09.15: the check named the outcome only; 1088.02.08 the seat yielded the Mayo claim
    war (it called that "only prestige"); the next step's purse: gold -226.84, unexplained
    -242.74, at 6.63 a month."""
    chk = reply("end_war_check_1086_09_15")["result"]
    assert "prices" not in chk and "we_yield" in chk["ends_with"]
    y = reply("end_war_yield_1088_02_08")
    assert y["verdict"] == "applied" and "price" not in y["result"]
    g = news_at("1088.04.22")["purse"]["gold"]
    assert (g["change"], g["unexplained"], g["income"]["monthly"]) == (-226.84, -242.74, 6.63)


def test_2_the_check_prices_every_term_for_your_side():
    gw = _priced_war_gw(terms_marks=["LEADS", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD"])
    res = run(R.do_impl(gw, intent="end_war", character_id=SA.ME, args={"enemy_id": SA.FOE, "check": True}))
    pr = res["result"]["prices"]
    assert pr["we_yield"]["gold"] == -238.7          # 3 x 12 x 6.63; measured 242.74
    assert "loses the claim" in pr["we_yield"]["you"] and "reparations" in pr["we_yield"]["they"]
    assert pr["white_peace"]["gold"] == 0 and "pressed" in pr["white_peace"]["you"]
    assert "takes the claimed" in pr["we_win"]["you"] and pr["we_win"]["truce_days"] == 1825


def test_2_a_defender_yielding_pays_no_reparations_and_loses_the_title():
    p = R.peace_prices("claim_cb", "defender", 5.0, ["we_yield", "we_win"])
    assert p["we_yield"]["gold"] == 0 and "loses the claimed" in p["we_yield"]["you"]
    assert p["we_win"]["gold"].startswith("+3 years")
    assert R.peace_prices("holy_war_cb", "attacker", 5.0, ["we_yield"])["we_yield"]["price"] == "unknown"


def test_2_the_send_carries_its_price_and_the_purse_attributes_it():
    gw = _priced_war_gw(terms_marks=["LEADS", "OPEN_WE_YIELD"])
    gw.state["war_list_after"] = []
    env = run(R.do_impl(gw, intent="end_war", character_id=SA.ME, args={"enemy_id": SA.FOE, "terms": "we_yield"}))
    assert env["result"]["price"]["gold"] == -238.7, env


def test_2_the_tables_reparations_are_the_game_files():
    """Each casus belli's on_defeat GOLD_VALUE (years of the attacker's yearly income)."""
    import re
    for cb, row in R.PEACE_PRICE.items():
        f = game_file("common/casus_belli_types/" + row["file"])
        start = f.index("\n", re.search(rf"^{cb} = {{", f, re.M).start())
        nxt = re.search(r"^\w+ = \{", f[start:], re.M)
        body = f[start:start + (nxt.start() if nxt else len(f))]
        defeat = body[body.index("on_defeat = {"):]
        if row["gold_years"] == 0:
            # A casus belli whose on_defeat pays no reparations (make_tributary_cb,
            # refused_liege_demand_war): its defeat block calls no gold effect
            end = re.search(r"\n\t\w+ = ", defeat[1:])
            assert "pay_short_term_gold" not in defeat[:end.start() if end else 3000], cb
            continue
        if "pay_short_term_income" in str(row.get("gold_basis") or ""):
            # minor_religious_war pays pay_short_term_income years = N, not the reparations effect
            assert re.search(rf"pay_short_term_income = \{{\s*years = {row['gold_years']}\b", defeat[:3000]), cb
            continue
        assert f"GOLD_VALUE = {row['gold_years']}" in defeat[:3000], cb
    eff = game_file("common/scripted_effects/00_war_effects.txt")
    rep = eff[eff.index("pay_short_term_gold_reparations_effect = {"):][:1500]
    assert "yearly_income = yes" in rep and "multiply = $GOLD_VALUE$" not in rep.split("else")[1][:200]
    assert "value = 1825" in game_file("common/script_values/00_war_values.txt")


def test_2_the_three_refused_offers_were_the_acceptance_reads_own_verdict():
    """1072.09.15-1073.02.08: make_peace enforce_demands refused three times; each refusal is
    the recipient's acceptance read, made before sending: it predicted it, nothing was sent."""
    b = reply("make_peace_1072_09_15")["blocked"]
    assert (b["gate"], b["raw_reason"]) == ("recipient_refuses", "target_would_reject")
    assert "so nothing was sent" in b["why"]


# ── 3: the claim war's price for THIS target ──────────────────────────────────────────────

def _price(attacker, defender, pressed=False, tier="county"):
    return DG.claim_cost(tier, pressed, attacker, defender)["cost"]


MURCHAD = {"age": 61, "is_female": False, "faith_key": "catholic", "dynasty_id": 11, "ruler_tier": 3}


def test_3_the_formula_against_the_recorded_charges():
    """Mayo 1086.07.04 (duke on duke, catholic on insular_celtic, unpressed): prestige 632.69
    -> 515.14 with ~+5 income, a charge of ~122.5; the formula 100 x 1.25 = 125. The same war
    by Domnall 1094.02.08: -123.43 with ~+2.5. Both within 2-3% (innovations, unread)."""
    assert news_at("1086.09.15")["purse"]["prestige"]["change"] == -117.55
    assert _price(MURCHAD, dict(MAYO_1082, ruler_tier=3)) == 125.0
    assert MAYO_1082["faith_key"] == "insular_celtic" and MAYO_1082["ruler_tier"] == 3
    # a count Murchad on the same lord: no lower-tier surcharge either way
    assert _price(dict(MURCHAD, ruler_tier=2), dict(MAYO_1082, ruler_tier=3)) == 125.0


def test_3_each_multiplier_the_reads_decide():
    d = {"age": 30, "is_female": False, "faith_key": "catholic", "dynasty_id": 7, "ruler_tier": 2}
    assert _price(MURCHAD, d) == 125.0                                   # duke on a count: x1.25
    assert _price(MURCHAD, d, pressed=True) == 93.75                     # pressed: 75 x 1.25
    assert _price(MURCHAD, dict(d, age=9)) == 62.5                       # on a child: x0.5
    assert _price(MURCHAD, dict(d, dynasty_id=11)) == 62.5               # own dynasty: x0.5
    assert _price(MURCHAD, dict(d, faith_key="insular_celtic")) == 156.25
    assert _price(MURCHAD, d, tier="duchy") == 250.0
    c = DG.claim_cost("county", False, MURCHAD, dict(d, is_female=True))
    assert any(u.startswith("a gender doctrine") for u in c["unread"])
    assert any("legitimacy" in u for u in c["unread"])


def test_3_the_big_declaring_steps_are_said_as_not_this_price():
    for d, drop in (("1070.07.04", -472.02), ("1073.07.04", -334.72), ("1076.09.15", -370.67)):
        assert news_at(d)["purse"]["prestige"]["change"] == drop and news_at(d)["wars_started"]
    o = {"cb": "claim_cb", "scope": "county", "valid": True}
    c = DG.cost_of(o, facts={"attacker": MURCHAD, "defender": MAYO_1082, "pressed": False})
    assert "several times this price from causes not read" in c["measured"] and "x1.25 the defender is of another faith" in c["basis"]


def test_3_the_multipliers_are_the_game_files():
    f = game_file("common/script_values/99_casus_belli_values.txt")
    body = f[f.index("claim_cb_prestige_cost = {"):f.index("tribal_subjugation_cb_prestige_cost")]
    for desc, x in (("CB_COST_DEFENDER_IS_CHILD", "0.5"), ("CB_COST_ATTACKER_IS_CHILD", "1.5"),
                    ("CB_COST_SAME_DYNASTY", "0.5"), ("CB_COST_DIFFERENT_FAITH", "1.25"),
                    ("CB_DEFNDER_LOWER_TIER", "1.25")):
        i = body.index(desc)
        assert f"value = {x}" in body[i - 120:i], desc
    assert "value = 100" in body and "add = -25" in body and "value = 200" in body


# ── gold: the head of faith's gold, a lever the seat could not send ───────────────────────

def test_gold_the_ai_asked_the_pope_and_the_seats_own_asks_never_reached_him():
    """1074.09.15: gold +136.46 and piety -241.9 in one step with no call of the seat's; its
    own two asks (1078.04.22, 1082.11.27, target the Pope 71438) came back ai_path_no_target:
    the bridge sent them through the game's AI decision, which ignores the target."""
    p = news_at("1074.09.15")["purse"]
    assert (p["gold"]["change"], p["piety"]["change"]) == (136.46, -241.9)
    b = reply("hof_gold_1078_04_22")["blocked"]
    assert b["gate"] == "ai_path_no_target"
    assert "hof_ask_for_gold_interaction" in R.DIRECT_DISPATCH_INTERACTIONS


def test_gold_the_lever_is_priced_for_the_ruler():
    row = R.hof_gold_lever({"monthly_income": 7.0, "piety": 709.0})
    assert (row["gold"], row["piety_cost"]) == (126.0, 250) and "blocked" not in row
    assert R.hof_gold_lever({"monthly_income": 3.0, "piety": 900.0})["gold"] == 100
    assert R.hof_gold_lever({"monthly_income": 7.0, "piety": 200.0})["blocked"]["gate"] == "insufficient_piety"
    f = game_file("common/scripted_effects/00_interaction_effects.txt")
    eff = f[f.index("hof_ask_for_gold_interaction_effect = {"):][:700]
    assert "gold = scope:actor.head_of_faith_gold_value" in eff and "add_piety = major_piety_loss" in eff
    v = game_file("common/script_values/01_dynamic_values.txt")
    hv = v[v.index("head_of_faith_gold_value = {"):][:200]
    assert "monthly_character_income" in hv and "massive_income_multiplier_value" in hv
    b = game_file("common/script_values/00_basic_values.txt")
    assert "massive_income_multiplier_value = 18" in b and "massive_gold_min_value = 100" in b
    assert "major_piety_value = 250" in b


# ── 4: the purse names the big moves ──────────────────────────────────────────────────────

def _snap(date, gold, prestige, piety, *, actor=ME, income=5.0, titles=None, wars=None):
    return {"actor_id": actor, "date": date, "gold": gold, "prestige": prestige, "piety": piety,
            "monthly_income": income, "claims": {}, "wars": wars if wars is not None else {},
            "titles": titles if titles is not None else {463: "c_dublin"}, "title_parents": {},
            "schemes": {}, "council": {}, "liege_id": 0, "heir": None}


def _diff(a, b, gw=None):
    gw = gw or Gw({}, date=b["date"])
    run(news.block(gw, 0, a, offers_served=False))
    return run(news.block(gw, 0, b, offers_served=False))


def test_4_the_recorded_purse_left_them_unexplained():
    assert news_at("1074.02.08")["purse"]["gold"]["unexplained"] == -214.03
    assert news_at("1074.09.15")["purse"]["gold"]["unexplained"] == 126.56
    assert news_at("1088.04.22")["purse"]["gold"]["unexplained"] == -242.74
    p = news_at("1091.02.08")["purse"]
    assert p["prestige"]["change"] == 1510.63 and p["piety"]["change"] == -892.95
    assert news_at("1091.02.08")["ruler_changed"]["from"] == ME


def test_4_a_duchy_created_for_you_is_priced_and_counted():
    """1074.02.08: d_meath gained, no war for it, gold 18.56 -> -185.81 (income ~+9)."""
    news.note_creation_price(ME, "d_meath", "gold", 212.5, "1066.09.15")
    out = _diff(_snap("1073.11.27", 18.56, -252.8, 686.8, income=4.6),
                _snap("1074.02.08", -185.81, 50.8, 695.9, income=4.6,
                      titles={463: "c_dublin", 462: "d_meath"}))
    g = out["purse"]["gold"]
    (ev,) = [e for e in g["in_this_step"] if "d_meath" in e["what"]]
    assert ev["expected"] == -212.5 and "rule of two duchy titles" in ev["counts"]
    assert abs(g.get("unexplained", 0)) < 15, g


def _hof_letter_picks():
    return {"event_watched": True, "event_picks": [
        {"seq": 1, "date": "1074.08.20", "character_id": ME, "event": news.HOF_GOLD_LETTER,
         "options": ["religious_interaction.3.a"], "ai_pick": {"index": 0, "key": "religious_interaction.3.a"},
         "applied": {"index": 0, "key": "religious_interaction.3.a"}, "rule": None, "gold": 0.0}]}


def test_4_the_head_of_faiths_gold_is_named_on_both_sides_with_his_letter():
    out = _diff(dict(_snap("1074.07.04", -43.24, 56.5, 706.7, income=7.0), event_watched=True, event_picks=[]),
                dict(_snap("1074.09.15", 93.22, 60.7, 464.8, income=7.0), **_hof_letter_picks()))
    g, p = out["purse"]["gold"], out["purse"]["piety"]
    assert any(e.get("expected") == 126.0 for e in g["in_this_step"])
    assert any(e.get("expected") == -250 for e in p["in_this_step"])
    assert abs(g["unexplained"] if "unexplained" in g else 0) < 30


def test_4_without_his_letter_the_same_purse_names_no_head_of_faith():
    out = _diff(dict(_snap("1074.07.04", -43.24, 56.5, 706.7, income=7.0), event_watched=True, event_picks=[]),
                dict(_snap("1074.09.15", 93.22, 60.7, 464.8, income=7.0), event_watched=True, event_picks=[]))
    for k in ("gold", "piety"):
        assert not any("head of faith" in str(e.get("what")) for e in out["purse"][k].get("in_this_step") or [])


def test_4_a_lost_attack_names_its_reparations():
    war = {7: {"war_id": 7, "name": "Claim on Mayo", "attacker_id": ME, "defender_id": MAYO_LORD,
               "is_attacker": True, "cb_type": "claim_cb"}}
    gw = Gw({"query_war_history": {"wars": [{"outcome": "defender_victory", "end_date": "1088.02.08"}]}},
            date="1088.04.22")
    out = _diff(_snap("1088.02.08", 53.11, 456.5, 1042.3, income=6.63, wars=war),
                _snap("1088.04.22", -173.73, 441.1, 1050.0, income=6.63), gw)
    g = out["purse"]["gold"]
    (ev,) = [e for e in g["in_this_step"] if "war lost" in e["what"]]
    assert ev["expected"] == -238.68 and "3 years" in ev["basis"]
    assert "cb_type" not in out["wars_ended"][0] and "_gold_years" not in out["wars_ended"][0]


def test_4_a_succession_starts_a_new_purse():
    out = _diff(_snap("1090.11.27", -31.75, 564.4, 1150.2),
                _snap("1091.02.08", 0.0, 2075.0, 257.2, actor=36124))
    nr = out["purse"]["new_ruler"]
    assert (nr["from"], nr["to"], nr["prestige"]) == (ME, 36124, 2075.0)
    assert set(out["purse"]) == {"new_ruler"}


def test_4_the_seats_own_spend_is_its_own_act():
    """A pre-flight run: two men-at-arms regiments bought, gold -85.54, the purse 'unexplained -93.31'.
    The purse is read before and after a spending write; what moved is the write's own."""
    gold = {"v": 120.0}

    class G(SA.Gw):
        async def read(self, action, params=None):
            if action == "query_character":
                self.reads.append((action, dict(params or {})))
                return {"id": ME, "gold": gold["v"], "prestige": 400.0, "piety": 500.0}
            return await super().read(action, params)
    gw = G(SA.ran("x"))
    before = run(R._purse_witness(gw, ME))
    gold["v"] = 30.0                                     # the write took 90
    env = {"result": {}}
    run(R._note_own_spend(gw, "men_at_arms", {"maa_type": "light_footmen"}, before, env))
    (act,) = news._ACTS[ME]
    assert (act["what"], act["gold"], act["cost_known"]) == ("men_at_arms light_footmen", -90.0, True)
    assert env["result"]["purse_moved"] == {"gold": -90.0}
    assert "men_at_arms" in R.PURSE_WITNESSED_INTENTS


# ── 5: the head of faith's claim is named only for a holder whose pre-check passed ────────

def test_5_the_recorded_hint_rearmed_after_its_quiet_year():
    assert "hof_ask_refused" in news_at("1074.09.15")["idle"]["piety"]
    assert news_at("1075.04.22")["idle"]["piety"]["next"]["args"]["candidates"] == ["hof_ask"]
    assert reply("hof_claim_1073_11_27")["blocked"]["raw_reason"] == "faith_differs"


def test_5_idle_piety_points_at_the_gold_ask_and_never_at_an_unchecked_claim():
    snap = {"actor_id": ME, "date": "1083.11.27", "piety": 846.0}
    p = news.idle(snap)["piety"]
    assert p["next"]["args"] == {"action_class": "income_levers"} and "hof_claim" not in p
    news.note_hof_claim_ok(ME, 30822, 527)
    p = news.idle(snap)["piety"]
    assert p["hof_claim"]["args"]["args"] == {"interaction": news.HOF_CLAIM_SEND, "target_id": 30822,
                                              "title_id": 527}
    assert news.idle({"actor_id": ME, "date": "1083.11.27", "piety": 50.0}) is None


# ── 6: a grant to a vassal is not a loss ────────────────────────────────────────────────

def test_6_the_recorded_grant_read_as_lost():
    (row,) = [r for r in news_at("1075.04.22")["titles_lost"] if r["key"] == "c_oriel"]
    assert row["how"] is None


def test_6_a_title_granted_inside_the_realm_says_so():
    gw = Gw({"query_title_hierarchy": {"title": {"holder_id": 37528, "key": "c_oriel"}}}, date="1075.04.22")
    gw.replies["query_character"] = lambda p: {"id": 37528, "liege_id": ME, "top_liege_id": ME}
    out = _diff(_snap("1075.02.08", 89.2, 67.9, 478.3, titles={463: "c_dublin", 479: "c_oriel"}),
                _snap("1075.04.22", 86.8, 70.8, 483.7), gw)
    (row,) = out["titles_lost"]
    assert (row["how"], row["holder_id"]) == ("granted_to_vassal", 37528) and "still_yours" in row


def test_6_the_recorded_empty_cbs_and_the_reason_it_now_gives():
    e = reply("cbs_29703_1081_02_08")
    r = e["result"]
    assert r["target_state"] == {} and r["valid_options"] == []
    (w0,) = e["warnings"]
    assert w0["code"] == "no_valid_targets" and "why_empty" not in w0
    gw = Gw({"query_cbs_for_target": {"status": "ok", "cbs": [], "target_state": {}}}, date="1081.02.08")
    gw.replies["query_character"] = lambda p: ({"id": 29703, "alive": True, "has_realm": False}
                                               if p.get("character_id") == 29703 else ME_1081)
    env = run(R.cbs_impl(gw, target_id=29703, character_id=ME))
    (w,) = [w for w in env["result"].get("warnings") or env.get("warnings") or [] if w.get("code") == "no_valid_targets"]
    assert "holds no land" in w["why_empty"]


# ── 7: fabricate_targets says which counties border the realm ─────────────────────────────

def test_7_a_county_off_the_border_says_its_half_rate():
    def nb(p):
        return {"neighbors": [{"title_id": 1, "realm_ruler_id": ME if p["title_id"] == 469 else 999}]}
    gw = Gw({"query_county_neighbors": nb})
    assert run(R.fabrication_border(gw, ME, 469)) == {"borders_your_realm": True}
    far = run(R.fabrication_border(gw, ME, 531))
    assert far["borders_your_realm"] is False and "x0.5" in far["progress"]
    assert run(R.fabrication_border(Gw({}), ME, 531)) == {"borders_your_realm": None}
    f = game_file("common/council_tasks/00_court_chaplain_tasks.txt")
    i = f.index("court_chaplain_fabricate_claim_not_adjacent_penalty")
    assert "is_neighbor_to_realm" in f[i - 400:i]
    assert "value = 0.5" in game_file("common/script_values/99_court_chaplain_values.txt").split(
        "court_chaplain_fabricate_claim_not_adjacent_penalty = {")[1][:40]


def test_gold_a_held_duchy_opens_its_counties_without_a_claim():
    """individual_county_de_jure_cb: valid_to_start wants the county's de-jure duchy (or above)
    held by you or your liege; allowed with innovation_casus_belli. 1081.02.08 the seat held
    2 of d_connacht's 3 counties and paid ~95 gold and 15+ months for a claim on the third."""
    f = game_file("common/casus_belli_types/00_dejure_war.txt")
    body = f[f.index("individual_county_de_jure_cb = {"):][:2500]
    assert "has_innovation = innovation_casus_belli" in body and "any_this_title_or_de_jure_above" in body
    import title_table as TT
    assert TT.DEJURE_COUNTY_CB == "individual_county_de_jure_cb"
    t = reply("cbs_34580_1081_09_15")["result"]
    assert t["invalid_by_gate"]["de_jure_not_liege"] == 4


def test_gold_the_title_table_names_the_counties_a_held_duchy_opens():
    """Recorded k_ireland inputs (1068.11.27): d_leinster unheld, Leinster's other county
    (Ossory) outside the realm; the duchy row says holding it needs no claim on Ossory."""
    from test_war_score_and_news_depth import _title_inputs
    gw = Gw(_title_inputs(), date="1068.11.27")
    env = run(R.inspect_impl(gw, subject="title", title_key="k_ireland", character_id=ME))
    rows = {d["key"]: d for d in env["result"]["duchies"]}
    held = [k for k, d in rows.items() if "if_you_hold_it" in d]
    for k in held:
        d = rows[k]
        assert not d["you_hold_it"] and d["counties_you_control"] >= 1
        assert d["if_you_hold_it"]["cb"] == "individual_county_de_jure_cb"
        mine = {c["key"] for c in env["result"]["counties"] if c.get("you_control")}
        assert not set(d["if_you_hold_it"]["no_claim_needed_on"]) & mine
    assert held, rows


def test_4_the_seats_own_ask_is_not_counted_twice():
    news.note_act(ME, "1074.08.01", "interact hof_ask_for_gold_interaction", cost_known=True,
                  gold=126.0, piety=-250.0)
    out = _diff(_snap("1074.07.04", -43.24, 56.5, 706.7, income=7.0),
                _snap("1074.09.15", 93.22, 60.7, 464.8, income=7.0))
    g = out["purse"]["gold"]
    assert g["your_acts"] and not any("head of faith" in e["what"] for e in g.get("in_this_step") or [])


# ── 8: a fabricated claim dies with the ruler; one word for the claim ─────────────────────

def test_8_the_recorded_claims_died_at_the_succession():
    lost = news_at("1091.02.08")["claims_lost"]
    assert {c["title_id"] for c in lost} == {474, 485} and all(c["type"] == "unpressed" for c in lost)
    assert MAYO_CBS_TOOL["target_state"]["pressed_claim_on_target"] is True     # the same claim: unpressed


def test_8_the_chaplains_claim_is_unpressed_in_the_games_file():
    f = game_file("events/councillor_task_events/court_chaplain_task_events.txt")
    body = f[f.index("court_chaplain_task.0301 = {"):][:1200]
    assert "add_unpressed_claim = scope:county" in body and "add_pressed_claim" not in body
    w = game_file("common/casus_belli_types/00_claim.txt")
    assert "make_claim_strong = prev" in w[w.index("on_white_peace = {"):][:900]


def test_8_cbs_says_one_word_and_that_it_dies_with_you():
    t = R.claim_state_words({"pressed_claim_on_target": True, "in_truce": False},
                            {"claims": {C_MAYO: "unpressed"}},
                            [{"cb": "claim_cb", "valid": True, "cb_title_id": C_MAYO}])
    assert t["claim_on_target"] == "unpressed" and "pressed_claim_on_target" not in t
    assert "dies with you" in t["claim_note"]
    gw = Gw({}, date="1080.04.22")
    snap0 = dict(_snap("1080.01.01", 10.0, 78.8, 500.0), claims={})
    snap1 = dict(_snap("1080.04.22", 12.0, 80.0, 505.0), age=64, health=2.1,
                 claims={479: {"title_id": 479, "title_name": "Oriel", "type": "unpressed", "holder_id": 32422}})
    out = _diff(snap0, snap1, gw)
    assert "lost when you die" in out["claims_gained"][0]["on_your_death"]


# ── 9: a grant that takes the title out of the realm ─────────────────────────────────────

def test_9_a_count_granting_a_county_is_told_it_leaves_the_realm():
    gw = Gw({"query_title_hierarchy": {"title": {"title_id": 508, "key": "c_leinster", "tier": 2}}})
    gw.replies["query_character"] = {"id": ME, "ruler_tier": 2}
    a = {"interaction": "grant_titles_interaction", "target_id": 37528, "title_id": 508}
    b = run(R._grant_leaves_realm(gw, ME, dict(a)))
    assert (b["gate"], b["raw_reason"]) == ("target_ineligible", "leaves_realm") and "LEAVES your realm" in b["summary"]
    assert b["fix"]["args"]["args"]["confirm_leaves_realm"] is True
    assert run(R._grant_leaves_realm(gw, ME, dict(a, confirm_leaves_realm=True))) is None
    gw.replies["query_character"] = {"id": ME, "ruler_tier": 3}          # a duke keeps a count as vassal
    assert run(R._grant_leaves_realm(gw, ME, dict(a))) is None


# ── 10: the defender's allies beside his own strength ───────────────────────────────────

def test_10_cbs_names_the_defenders_allies_and_what_is_not_read():
    al = {"own_military_strength": 1434, "total_callable_strength": 6695,
          "alliances": [{"ally_id": 900, "military_strength": 5261}], "forced_allies": []}
    gw = Gw(cbs_by_target(_mayo_cbs_raw()), date="1088.04.22")
    gw.replies["query_character"] = lambda p: ME_1081 if p.get("character_id") == ME else MAYO_1082
    gw.replies["query_armies"] = ARMIES_NONE
    gw.replies["query_alliances"] = al
    res = run(R.cbs_impl(gw, target_id=MAYO_LORD, character_id=ME))["result"]
    da = res["defender_allies"]
    assert (da["his_own"], da["with_allies"]) == (1434, 6695)
    assert {k: da["allies"][0][k] for k in ("id", "strength")} == {"id": 900, "strength": 5261}
    # `accepts: "not read"` is replaced by each ally's would_join (test_ally_join_and_loss_price)
    assert "accepts" not in da and "would_join" in da["allies"][0]
    assert res["target_state"]["claim_on_target"] in ("unpressed", "held")
