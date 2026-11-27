"""A way to earn prestige and gold as a human player does (activities, W1/W2), and the smaller defects
of a seated game (W3, W4), each reproduced from the recorded reply.

The fixture (fixtures/activity_income_replies.json.gz)
holds whole tool replies and step news recorded in that run; `line` is the record's line in the evidence's
calls file. The "recorded" tests read what the run's toolkit answered (they hold with or without the rules
tested here); the others drive the code and fail without those rules.
"""
import asyncio
import gzip
import json
from pathlib import Path

import pytest

import activities as A
import declare_gates as DG
import news
import redesign as R
from test_ireland_walk_replies import Gw

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "activity_income_replies.json.gz", "rt").read())
ME = 31506
#: the game's script, where a local install has it (the rule tests skip without it)
GAME = Path.home() / ".local" / "share" / "Steam" / "steamapps" / "common" / "Crusader Kings III" / "game"
ECHRAD = 64514


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
    getattr(R, "_CLAIM_FACTS", {}).clear()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()
    getattr(R, "_CLAIM_FACTS", {}).clear()


class ScriptGw(Gw):
    """Gw whose script_action answers by template: `marks[template]` (list) and `guard[template]`."""

    def __init__(self, replies=None, date="1066.09.15", marks=None, guard=None):
        super().__init__(replies, date)
        self.marks, self.guard = dict(marks or {}), dict(guard or {})

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action != "script_action":
            return {"success": True}
        t = p.get("template")
        return {"success": True, "template": t, "token": "SACT_9", "guard": self.guard.get(t, "ran"),
                "marks": list(self.marks.get(t, [])), "errors": []}


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


COUNT = {"query_character": {"id": ME, "name": "Murchad", "alive": True, "gold": 84.0, "ruler_tier": 2,
                             "age": 41},
         "query_ai_budget": {"success": True, "buckets": {"reserved": 0.0, "war_chest": 50.0, "long_term": 0.0,
                                                          "short_term": 34.0}, "reserved_max": 0}}


# ── W1: activities ─────────────────────────────────────────────────────────────

def test_w1_income_levers_names_the_activity_lever_and_its_class():
    # a recorded game: the only prestige the seat could act for was a war's; income_levers named gold writes only
    rows = [r["lever"] for r in R.INCOME_LEVERS]
    assert "host_activity" in rows and "activities" in R.ACTION_CLASSES


def test_w1_the_listing_names_each_gate_the_game_reads_at_the_1066_start():
    marks = ["EARLY", "BUILD_FOCUS", "COUNT_TIER", "FEAST_AXIS_BELOW_100", "HUNT_AXIS_BELOW_100",
             "PILGRIMAGE_AXIS_BELOW_100", "RAN"]
    gw = ScriptGw(COUNT, marks={"activity": marks})
    env = run(R.do_impl(gw, action_class="activities", character_id=ME))
    (sent,) = [p for a, p in gw.writes if a == "script_action"]
    assert sent == {"actor_id": ME, "template": "activity"}, sent          # the listing writes nothing
    res = env["result"]
    assert res["available"] == [] and [r["activity"] for r in res["unavailable"]] == ["feast", "hunt", "pilgrimage"]
    feast = res["unavailable"][0]
    conds = [g["condition"] for g in feast["gates"]]
    assert conds == ["early", "build_focus", "ai_sociability_below_100"]
    assert feast["blocked"]["gate"] == "ai_potential_not_met" and "3 years" in feast["blocked"]["summary"]
    assert feast["gates"][2]["fix"] == {"tool": "steer", "args": {"personality": {"sociability": 100}}}
    assert feast["gold"]["gold"] == 30 and feast["gold"]["short_term_gold"] == 34.0
    assert feast["do_call"] == {"tool": "do", "args": {"intent": "host_activity", "args": {"activity": "feast"}}}
    assert "225" in feast["gives"] and "5 years" in feast["cooldown"]
    pil = res["unavailable"][2]
    assert "build_focus" not in [g["condition"] for g in pil["gates"]]
    assert "event_rule" in res["events"]


def test_w1_short_term_gold_below_the_base_price_is_a_gate_with_the_reserve_lever():
    gw = ScriptGw(dict(COUNT, query_ai_budget={"success": True, "buckets": {"reserved": 80.0, "war_chest": 0.0,
                                                                            "long_term": 0.0, "short_term": 4.0},
                                               "reserved_max": 200}),
                  marks={"activity": ["RAN"]})
    env = run(R.do_impl(gw, action_class="activities", character_id=ME))
    hunt = next(r for r in env["result"]["unavailable"] if r["activity"] == "hunt")
    assert hunt["blocked"]["gate"] == "insufficient_gold"
    assert hunt["gates"][0]["fix"]["args"] == {"intent": "gold_reserve", "args": {"amount": 0}}


def test_w1_a_type_the_game_allows_is_available_with_its_call():
    gw = ScriptGw(dict(COUNT, query_character=dict(COUNT["query_character"], ruler_tier=3),
                       query_ai_budget={"success": True, "buckets": {"short_term": 70.0}, "reserved_max": 0}),
                  marks={"activity": ["FEAST_CAN_HOST", "HUNT_AXIS_BELOW_100", "RAN"]})
    env = run(R.do_impl(gw, action_class="activities", character_id=ME))
    (f,) = env["result"]["available"]
    assert f["activity"] == "feast" and f["game_can_host"] is True and f["gold"]["gold"] == 60


def test_w1_host_puts_the_type_to_the_ai_and_is_pending_until_it_hosts():
    gw = ScriptGw(COUNT, date="1069.10.15", marks={"activity": ["FEAST_CAN_HOST", "COUNT_TIER", "RAN"]})
    env = run(R.do_impl(gw, intent="host_activity", character_id=ME, args={"activity": "feast"}))
    (sent,) = [p for a, p in gw.writes if a == "script_action"]
    assert sent == {"actor_id": ME, "template": "activity", "activity": "activity_feast", "host": True}
    assert env["verdict"] == "pending" and env["pending"]["drains_when"] == "clock_advances"
    assert "chance" in env["pending"]["why_pending"] and "event_rule" in env["result"]["events"]
    (act,) = news._ACTS[ME]
    assert act["what"].startswith("host_activity feast") and act["cost_known"] is False


def test_w1_host_refused_by_the_game_names_the_gate():
    gw = ScriptGw(COUNT, marks={"activity": ["EARLY", "BUILD_FOCUS"]}, guard={"activity": "refused"})
    env = run(R.do_impl(gw, intent="host_activity", character_id=ME, args={"activity": "hunt"}))
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "ai_potential_not_met"
    assert [g["condition"] for g in env["result"]["gates"]][:2] == ["early", "build_focus"]
    assert ME not in news._ACTS


def test_w1_an_activity_outside_the_set_is_refused_and_nothing_is_sent():
    gw = ScriptGw(COUNT)
    env = run(R.do_impl(gw, intent="host_activity", character_id=ME, args={"activity": "tournament"}))
    assert env["blocked"]["gate"] == "invalid_argument" and gw.writes == []


def test_w1_the_names_are_the_bridges_set():
    src = (HERE / ".." / ".." / "bridge" / "include" / "script_templates.h").read_text()
    for typ in A.ACTIVITIES.values():
        assert f'{{"{typ}", "' in src
    assert '{"activity", Template::kActivity}' in src


# ── W2: the sources income_levers lists ─────────────────────────────────────────

def test_w2_every_source_row_is_a_call_this_surface_takes():
    env = run(R.do_impl(ScriptGw(COUNT), action_class="income_levers", character_id=ME))
    rows = env["result"]["available"] + env["result"]["unavailable"]
    by = {r["lever"]: r for r in rows}
    for k in ("host_activity", "commission_epic_decision", "sale_of_titles_decision", "ransom", "hof_gold"):
        assert k in by, k
    for r in rows:
        dc = r.get("do_call")
        if dc:
            assert dc["tool"] == "do" and dc["args"]["intent"] in R.INTENTS, r
        en = (r.get("enumerate") or {}).get("args") or {}
        en = en.get("args", en) if en.get("tool") == "do" else en
        if "action_class" in en:
            assert en["action_class"] in R.ACTION_CLASSES, r
    assert "ransom_interaction" in R.DIRECT_DISPATCH_INTERACTIONS
    assert "demand_payment_interaction" not in R.DIRECT_DISPATCH_INTERACTIONS
    assert by["demand_payment"]["blocked"]["gate"] == "ai_path_no_target"


# ── the declare gate's price ────────────────────────────────────────────

ECHRAD_CHR = {"id": ECHRAD, "name": "Echrad", "alive": True, "gold": 72.99, "prestige": 576.23, "piety": 94.15,
              "age": 17, "is_female": True, "faith_key": "insular_celtic", "dynasty_id": 1, "ruler_tier": 3}
AILECH_LORD = {"id": 67640, "age": 40, "is_female": False, "faith_key": "catholic", "dynasty_id": 2,
               "ruler_tier": 2}


def echrad_gw(marks):
    return ScriptGw({"query_character": ECHRAD_CHR, "query_armies": {"armies": []},
                     "query_claims": {"claims": [{"title_id": 485, "type": "unpressed"}]},
                     "get_ai_evaluations": {"diplomacy": {"legitimacy": 107.5}}},
                    date="1093.01.29", marks={"claim_price_facts": marks})


def test_w3a_recorded_the_gate_read_the_low_end_and_the_war_cost_the_high():
    chk = env_of("declare_refused_1093_01_29")["blocked"]["measured"]["checked_before_sending"]["prestige"]
    assert chk == {"cost": 234.38, "have": 576.23, "ok": True}
    assert env_of("declare_applied_1093_02_12")["result"]["declare_cost"]["prestige"] == 312.5
    s = json.dumps(env_of("plan_war_1093_01_29"))
    assert "x1.5-2 your legitimacy 107.5: your culture's era (1-4) is not read" in s


def test_w3a_the_legitimacy_flag_read_from_the_game_prices_it_at_312_5():
    gw = echrad_gw(["READ", "LEGIT_X2_0", "FAITH_MALE_DOMINATED"])
    purse = run(R.ruler_purse(gw, ECHRAD))
    opt = {"cb": "claim_cb", "valid": True, "scope": "county", "cb_title_id": 485}
    bad, chk = DG.failing(opt, purse, {"in_truce": False}, title_key="c_ailech",
                          facts=DG.facts_for(opt, purse, AILECH_LORD))
    assert chk["prestige"]["cost"] == 312.5, chk
    c = DG.cost_of(opt, title_key="c_ailech", facts=DG.facts_for(opt, purse, AILECH_LORD))
    assert "prestige_high" not in c
    assert [m["factor"] for m in c["multipliers"][1:]] == ["different_faith", "defender_lower_tier", "legitimacy"]
    assert not any(u.startswith("your legitimacy") for u in c["unread_multipliers"])
    assert [p for a, p in gw.writes if a == "script_action"] == [{"actor_id": ECHRAD,
                                                                  "template": "claim_price_facts"}]


def test_w3a_without_the_read_the_gate_still_spans_the_eras():
    # non-discriminating control: an unanswered read keeps the range (the low end is the least)
    gw = echrad_gw([])
    purse = run(R.ruler_purse(gw, ECHRAD))
    opt = {"cb": "claim_cb", "valid": True, "scope": "county", "cb_title_id": 485}
    c = DG.cost_of(opt, title_key="c_ailech", facts=DG.facts_for(opt, purse, AILECH_LORD))
    assert c["prestige"] == 234.38 and c["prestige_high"] == 312.5


def test_w3a_the_gender_rule_applies_when_the_faith_is_shared():
    gw = echrad_gw(["READ", "LEGIT_X2_0", "FAITH_MALE_DOMINATED"])
    purse = run(R.ruler_purse(gw, ECHRAD))
    lord = dict(AILECH_LORD, faith_key="insular_celtic")
    opt = {"cb": "claim_cb", "valid": True, "scope": "county", "cb_title_id": 485}
    c = DG.cost_of(opt, title_key="c_ailech", facts=DG.facts_for(opt, purse, lord))
    # 100 x 1.25 (a woman on a man, male-dominated) x 1.25 (tier) x 2.0 (legitimacy)
    assert c["prestige"] == 312.5 and any(m.get("factor") == "gender_doctrine" for m in c["multipliers"])


# ── W3(b): the seat's own mint is the seat's ───────────────────────────────────

def test_w3b_recorded_the_purse_called_the_seats_mint_the_ais_and_counted_it_twice():
    g = news_of("news_1075_10_06")["purse"]["gold"]
    row = [e for e in g["in_this_step"] if "d_leinster" in e["what"]][0]
    assert "created for you by the game's AI" in row["what"]
    assert g["your_acts"] == [{"gold": -212.5, "what": "mint_title d_leinster"}]
    assert g["unexplained"] == 189.01


def test_w3b_the_seats_own_mint_is_named_as_its_own_and_counted_once():
    news.note_creation_price(ME, "d_leinster", "gold", 212.5, "1075.06.08")
    a = snap("1075.06.08", chr_sect={"gold": 215.97, "prestige": 110.99, "piety": 839.2})
    news.note_act(ME, "1075.06.08", "mint_title d_leinster", cost_known=True, gold=-212.5, prestige=300.0)
    b = snap("1075.10.06", chr_sect={"gold": -0.66, "prestige": 410.99, "piety": 852.0},
             held={"held_titles": [{"title_id": 777, "key": "d_leinster"}]})
    out = two_reads(Gw({}), a, b)
    g = out["purse"]["gold"]
    (row,) = [e for e in g["in_this_step"] if "d_leinster" in e["what"]]
    assert row["decided_by"] == "you" and "game's AI" not in row["what"] and "expected" not in row
    assert abs(g.get("unexplained", 0)) < 10
    (p,) = [e for e in out["purse"]["prestige"]["in_this_step"] if "d_leinster" in e["what"]]
    assert p["decided_by"] == "you" and "expected" not in p


def test_w3b_a_title_with_no_act_of_the_seat_is_still_the_ais():
    # control: the row stands when the seat sent no mint for that title
    news.note_creation_price(ME, "d_meath", "gold", 212.5, "1074.11.11")
    news.note_act(ME, "1074.11.11", "mint_title d_leinster", cost_known=True, gold=-212.5)
    a = snap("1074.11.11", chr_sect={"gold": 29.6, "prestige": 150.0, "piety": 700.0})
    b = snap("1075.02.09", chr_sect={"gold": -395.0, "prestige": 454.37, "piety": 700.0},
             held={"held_titles": [{"title_id": 900, "key": "d_meath"}]})
    out = two_reads(Gw({}), a, b)
    (g,) = [e for e in out["purse"]["gold"]["in_this_step"] if "d_meath" in e["what"]]
    assert g["decided_by"].startswith("the game's AI")


# ── W3(d): peace terms say what the game needs ─────────────────────────────────

def test_w3d_recorded_four_refusals_named_no_requirement():
    for k in ("make_peace_1095_02_12", "make_peace_1095_08_11", "make_peace_1095_11_19", "make_peace_1096_02_17"):
        e = env_of(k)
        assert e["blocked"]["gate"] == "recipient_refuses" and "term_needs" not in e["result"]
        assert 11 <= e["result"]["war_score"] <= 37


def test_w3d_the_refusal_states_the_games_rule_with_the_score():
    war = {"war_id": 301989948, "name": "Ormond", "primary_attacker": ECHRAD, "primary_defender": 73537,
           "attackers": [ECHRAD], "defenders": [73537]}
    gw = Gw({"query_wars": {"wars": [war]}, "get_active_wars": {"wars": [war]},
             "query_war_score_breakdown": {"total_warscore": 37, "total_warscore_source": "engine_cached",
                                           "occupation_score": 37},
             "query_war_history": {"wars": []},
             "query_intent_results": {"results": []}}, date="1095.02.12")

    async def refuse(action, params=None, **kw):
        return {"raw": {"success": True, "status": "queued"}, "refused": "target_would_reject",
                "resolved": None}
    gw.confirmed_write = refuse
    env = run(R._do_war_intent(gw, "make_peace", {"war_id": 301989948, "terms": "enforce_demands"}, ECHRAD,
                               {"id": ECHRAD}))
    tn = env["result"]["term_needs"]
    assert tn["your_war_score"] == 37 and tn["acceptance_from_score"] == -62 and "100" in tn["needs"]
    peace_white = R.peace_term_needs("white_peace", 37, 400)
    assert peace_white["acceptance_from_score_and_length"] == -30 + 37 + 4
    assert R.peace_term_needs("surrender", None)["needs"].startswith("nothing")


def test_w3d_the_rule_is_the_files():
    g = GAME / "common" / "character_interactions" / "00_war.txt"
    if not g.exists():
        pytest.skip("no game install here")
    t = g.read_text(encoding="utf-8-sig")
    vic = t[t.index("end_war_attacker_victory_interaction = {"):t.index("end_war_attacker_white_peace_interaction = {")]
    assert "base = -99" in vic and "attacker_war_score >= 100" in vic
    wp = t[t.index("end_war_attacker_white_peace_interaction = {"):t.index("end_war_attacker_defeat_interaction = {")]
    assert "base = -30" in wp and "divide = 91" in wp


# ── W4: a child ruler's first news ─────────────────────────────────────────────

def test_w4_recorded_the_succession_row_said_only_who():
    rc = news_of("news_1078_02_03")["ruler_changed"]
    assert rc["to"] == ECHRAD and set(rc) == {"from", "to", "next"}


def test_w4_a_child_ruler_row_states_the_war_rule_the_price_multipliers_and_their_end():
    gw = ScriptGw({}, date="1078.02.03", marks={"claim_price_facts": ["READ", "CHILD", "FAITH_MALE_DOMINATED"]})
    a = snap("1078.01.10", actor_id=42431, chr_sect={"gold": 10.0, "prestige": 300.0, "piety": 0.0, "age": 30})
    b = snap("1078.02.03", actor_id=ECHRAD, chr_sect={"gold": 0.0, "prestige": 200.0, "piety": 2.2, "age": 2})
    out = two_reads(gw, a, b)
    c = out["ruler_changed"]["child_ruler"]
    assert c["war_declaration"].startswith("allowed")
    xs = [(m["x"], m["ends"]) for m in c["claim_price_multipliers"]]
    assert xs[0][0] == 1.5 and "1091 or 1092" in xs[0][1]
    assert xs[1] == (1.25, "never: not age")
    assert "CB_COST_ATTACKER_IS_CHILD" in c["source"]


def test_w4_an_adult_successor_gets_no_child_row():
    a = snap("1078.01.10", actor_id=31506, chr_sect={"gold": 10.0, "prestige": 300.0, "piety": 0.0, "age": 52})
    b = snap("1078.01.10", actor_id=42431, chr_sect={"gold": 0.0, "prestige": 200.0, "piety": 2.2, "age": 30})
    out = two_reads(Gw({}), a, b)
    assert "child_ruler" not in out["ruler_changed"]


def test_w4_the_multipliers_are_the_files():
    f = GAME / "common" / "script_values" / "99_casus_belli_values.txt"
    if not f.exists():
        pytest.skip("no game install here")
    t = f.read_text(encoding="utf-8-sig")
    i = t.index("CB_COST_ATTACKER_IS_CHILD")
    assert "value = 1.5" in t[i - 120:i]
    assert "value = 1.25" in t[t.index("25% more expensive for women"):][:400]


def test_w1_live_the_games_can_host_stands_over_the_price_estimate():
    # A live check, 1069.09.18, ruler 29803: can_host_activity said yes for the feast while the estimate of
    # the base price (tier only; the game's own value is rooted at the ruler) read short-term gold below it
    gw = ScriptGw(dict(COUNT, query_character=dict(COUNT["query_character"], ruler_tier=5),
                       query_ai_budget={"success": True, "buckets": {"short_term": 40.0}, "reserved_max": 0}),
                  marks={"activity": ["FEAST_CAN_HOST", "RAN"]})
    env = run(R.do_impl(gw, action_class="activities", character_id=ME))
    (f,) = env["result"]["available"]
    assert f["activity"] == "feast" and "gates" not in f
    assert "can_host_activity" in f["gold"]["estimate_below"]
    hunt = next(r for r in env["result"]["unavailable"] if r["activity"] == "hunt")
    assert hunt["blocked"]["gate"] == "insufficient_gold"     # the game said no: the estimate is a reason
