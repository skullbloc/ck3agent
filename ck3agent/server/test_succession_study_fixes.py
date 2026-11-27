"""The succession study's fixes wired into the tools.

Recorded replies: fixtures/succession_study_replies.json.gz (built by a
research-tree script from the calls files of three seated 1066 Ireland games). What each fixture holds and what is inferred:

* game A (1071): the seat's observe at 1071.12.22 named the breaker 15819 (a granddaughter) taking Athlone;
  Murchad's own character read (1071.08.14: children [36124, 37111, 37528], tier 2, health 4.35). The
  game gave the son 37528 two counties at the 1082 death. INFERRED (not in any read): 36124 and 37111
  dead by 1071 (the primary heir 66780 is not a child, and the 1082 split gave 37111 nothing).
* game B (1071.09.16): breaker 32805, Murchad's brother; children [36124, 37111, 37528] read the same day.
  INFERRED: all three alive then (Donnchad 37528 became primary only in 1076).
* game C (1072.06.14): breaker 32805 again; at the 1078 death the son 37111 took his share.
  INFERRED: 37528 not alive then (the verdict counted 3 heirs: 36124, 32805, 37111).

Each recorded case drives succession_plan through the real code with the bridge's OLD simulation reply
(the line's split, exactly the breaker the seat was shown) and asserts the reply names the game's
heirs: red without the rule (the reply repeats the recorded breaker), green with it.
"""
import asyncio
import gzip
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import partition_forecast as PF  # noqa: E402
import redesign as R  # noqa: E402
try:  # absent without the rules: the forecast and lever rows run red, the death and threat rows are skipped
    import ruler_death as RD  # noqa: E402
except ImportError:  # pragma: no cover
    RD = None

REC = json.load(gzip.open(os.path.join(HERE, "fixtures", "succession_study_replies.json.gz")))
CONF, MALE_PREF = "confederate_partition_succession_law", "male_preference_law"
LAWS = [{"name": CONF, "group_name": "succession_order_laws"},
        {"name": MALE_PREF, "group_name": "succession_gender_laws"}]


def run(c):
    return asyncio.run(c)


def recorded_succession(key):
    return REC[key]["result"]["succession"]


def old_sim(succ, primary_titles, extra_heirs=()):
    """The bridge's superseded simulation reply as the seat's verdict read it: the player heir with his
    titles, the breaker with the titles the record says he takes, and the rest of the line's split."""
    p, b = succ["primary_heir"], succ["breaker"]
    heirs = [{"character_id": p["id"], "name": p["name"], "is_player_heir": True,
              "titles": primary_titles},
             {"character_id": b["id"], "name": b["name"], "is_player_heir": False, "titles": b["takes"]}]
    for h in extra_heirs:
        heirs.append({"character_id": h, "name": None, "is_player_heir": False, "titles": []})
    return {"primary_title_id": 463, "heirs": heirs}


def county(tid, key):
    return {"title_id": tid, "tier": 2, "key": key, "name": key[2:].title(), "succession_type": "partition"}


def tmap(sim):
    """query_title_succession_map as the bridge sends it under partition: every title's cached line head
    is the primary heir, splits_away false (a recorded run, 1088: three titles, all false)."""
    ph = sim["heirs"][0]["character_id"]
    rows = [dict(t, title_key=t["key"], title_name=t["name"], succession_law="partition",
                 title_division="partition", projected_heir_id=ph, projected_heir_name="x", splits_away=False)
            for h in sim["heirs"] for t in h["titles"]]
    return {"primary_title_id": 463, "primary_heir_id": ph, "titles": rows, "any_splits": False, "status": "ok"}


class Gw:
    health = "ok"

    def __init__(self, ruler, kids, sim, laws=LAWS, dynasty=None, extra=None):
        self.ruler = ruler
        self.kids = kids
        self.sim = sim
        self.laws = laws
        self.dynasty = dynasty if dynasty is not None else {"head_character_id": ruler["id"], "prestige": 420.0}
        self.extra = dict(extra or {})
        self.reads = []

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        p = dict(params or {})
        self.reads.append((action, p))
        if action in self.extra:
            r = self.extra[action]
            return r(p) if callable(r) else r
        if action == "query_character":
            cid = int(p.get("character_id") or self.ruler["id"])
            return self.ruler if cid == self.ruler["id"] else self.kids.get(cid, {})
        if action == "query_succession_simulation":
            return self.sim
        if action == "query_title_succession_map":
            return tmap(self.sim)
        if action == "query_realm_laws":
            return {"active_laws": self.laws}
        if action == "query_dynasty":
            return dict(self.dynasty, dynasty_id=p.get("dynasty_id"))
        return {}

    async def read_many(self, reqs):
        return [await self.read(a, p) for a, p in reqs]

    def game(self):
        return {"tick": 1, "date": "1071.12.22", "paused": True}


def kid(cid, name, alive=True, female=False, age=20, traits=(), **kw):
    return {"id": cid, "name": name, "alive": alive, "is_female": female, "age": age,
            "traits": [{"name": t} for t in traits], "ruler_tier": 0, "faith_id": 23, "dynasty_id": 1838, **kw}


#: the recorded character reads' fields the code reads, as literals (the census of test doubles checks their keys
#: against the bridge's emitter); test_A0 checks each value against the recorded reply
RULER_A = {"id": 31506, "name": "Murchad", "alive": True, "is_female": False, "age": 46, "health": 4.35,
           "children": [36124, 37111, 37528], "ruler_tier": 2, "dynasty_id": 1838, "faith_id": 23,
           "piety": 588.8, "prestige": -39.71}
RULER_B = {"id": 31506, "name": "Murchad", "alive": True, "is_female": False, "age": 46, "health": 4.22,
           "children": [36124, 37111, 37528], "ruler_tier": 2, "dynasty_id": 1838, "faith_id": 23,
           "piety": 599.7, "prestige": 148.9}


def ruler_a():
    return dict(RULER_A)


def test_A0_the_ruler_doubles_are_the_recorded_reads():
    for lit, key in ((RULER_A, "a_character_1071_08_14"), (RULER_B, "b_character_1071_09_16")):
        rec = REC[key]["result"]
        assert {k: rec[k] for k in lit} == lit, key


def plan(gw):
    env = run(R.succession_plan_impl(gw, character_id=gw.ruler["id"]))
    assert env["ok"], env
    return env["result"]


# ── the partition forecast: the recorded cases ───────────────────────────────────────────────

def test_A1_run_a_names_the_son_who_took_land_not_the_granddaughter():
    succ = recorded_succession("a_observe_1071_12_22")
    assert succ["breaker"]["id"] == 15819 and succ["primary_heir"]["id"] == 66780      # the record
    ruler = ruler_a()
    assert ruler["children"] == [36124, 37111, 37528] and ruler["ruler_tier"] == 2
    sim = old_sim(succ, [county(463, "c_dublin"), county(508, "c_leinster")], [70552])
    kids = {36124: kid(36124, "Domnall", alive=False), 37111: kid(37111, "Enna", alive=False),
            37528: kid(37528, "Donnchad", age=16)}
    out = plan(Gw(ruler, kids, sim))
    assert out["outcome"] == "splits"
    assert out["breaker"]["id"] == 37528 and out["breaker"]["fate"] == "leaves_realm"
    assert out["breaker"]["source"] == "partition_forecast"
    assert out["breaker_simulation"]["id"] == 15819
    assert out["eligible_heir_count"] == 2
    assert out["partition_forecast"]["takers"] == [66780, 37528]
    # the old bridge's split is among other heirs: which county he takes is said to be unread
    assert out["breaker"]["takes"] == "unread"
    assert {t["fate"] for t in out["per_title_map"]["titles"]} == {"unassigned"}
    assert all(t["splits_away"] is None for t in out["per_title_map"]["titles"])


def test_A2_run_b_drops_the_brother_and_names_both_younger_sons():
    succ = recorded_succession("b_observe_1071_09_16")
    assert succ["breaker"]["id"] == 32805                                           # the record
    ruler = dict(RULER_B)
    sim = old_sim(succ, [county(463, "c_dublin"), county(508, "c_leinster")], [37111, 37528])
    kids = {c: kid(c, n) for c, n in ((36124, "Domnall"), (37111, "Enna"), (37528, "Donnchad"))}
    out = plan(Gw(ruler, kids, sim))
    assert out["breaker"]["id"] == 37111 and out["breaker_simulation"]["id"] == 32805
    assert out["partition_forecast"]["takers"] == [36124, 37111, 37528]
    assert 32805 in out["partition_forecast"]["simulation_disagrees"]["named_but_not_partition_heirs"]
    assert [h["id"] for h in out["levers"]["heirs"]] == [36124, 37111, 37528]


def test_A3_run_c_drops_the_brother():
    succ = recorded_succession("c_observe_1072_06_14")
    assert succ["breaker"]["id"] == 32805
    ruler = dict(RULER_B)     # the same ruler, a run with no read of him in 1072
    sim = old_sim(succ, [county(463, "c_dublin")], [37111])
    kids = {36124: kid(36124, "Domnall"), 37111: kid(37111, "Enna"), 37528: kid(37528, "Donnchad", alive=False)}
    out = plan(Gw(ruler, kids, sim))
    assert out["breaker"]["id"] == 37111 and out["breaker_simulation"]["id"] == 32805
    assert out["partition_forecast"]["takers"] == [36124, 37111]


def test_A4_splits_away_is_true_on_the_titles_that_leave_with_the_fixed_bridge():
    # the bridge's simulation: shared among the children in the line, said by its basis
    ruler = ruler_a()
    sim = {"primary_title_id": 463, "partition_heir_basis": "children_in_line",
           "partition_heirs": [66780, 37528], "line_not_partition_heirs": [15819],
           "heirs": [{"character_id": 66780, "name": "Culdub", "is_player_heir": True,
                      "titles": [county(463, "c_dublin"), county(513, "c_ossory"), county(501, "c_breifne")]},
                     {"character_id": 37528, "name": "Donnchad", "is_player_heir": False,
                      "titles": [county(469, "c_athlone"), county(508, "c_leinster")]}]}
    kids = {36124: kid(36124, "Domnall", alive=False), 37111: kid(37111, "Enna", alive=False),
            37528: kid(37528, "Donnchad")}
    gw = Gw(ruler, kids, sim)
    gw.extra["query_title_succession_map"] = tmap(dict(sim, heirs=[dict(sim["heirs"][0], titles=[
        t for h in sim["heirs"] for t in h["titles"]])]))
    out = plan(gw)
    rows = {t["key"]: t for t in out["per_title_map"]["titles"]}
    assert rows["c_athlone"]["splits_away"] is True and rows["c_leinster"]["splits_away"] is True
    assert rows["c_athlone"]["heir_id"] == 37528
    assert rows["c_dublin"]["splits_away"] is False and rows["c_ossory"]["splits_away"] is False
    assert out["breaker"]["takes"] == [469, 508]
    assert "breaker_simulation" not in out


def test_A5_a_childless_ruler_is_undetermined_not_intact():
    # game A 1084: Culdub without a child; the line's head is his sister
    ruler = dict(ruler_a(), id=66780, children=[])
    sim = {"primary_title_id": 463, "heirs": [
        {"character_id": 15819, "is_player_heir": True, "titles": [county(463, "c_dublin")]},
        {"character_id": 37528, "is_player_heir": False, "titles": [county(513, "c_ossory")]}]}
    out = plan(Gw(ruler, {}, sim))
    assert out["outcome"] == "undetermined" and out["passes_intact"] is None
    assert "not read" in out["undetermined_reason"]


def test_A6_an_unread_child_refuses_and_keeps_the_verdict():
    succ = recorded_succession("b_observe_1071_09_16")
    ruler = dict(RULER_B)
    sim = old_sim(succ, [county(463, "c_dublin")])
    out = plan(Gw(ruler, {}, sim))     # no child answers
    assert out["partition_forecast"]["takers"] == "unknown"
    assert "alive and is_female must both be read" in out["partition_forecast"]["refused"]
    assert out["breaker"]["id"] == 32805           # unchanged, not guessed


def test_A7_run_a_1088_one_child_is_intact_where_the_bridge_said_splits():
    rec = REC["a_succession_plan_1088_07_04"]["result"]
    assert rec["outcome"] == "splits" and rec["breaker"]["id"] == 15819
    assert all(t["splits_away"] is False for t in rec["per_title_map"]["titles"])   # the record's defect
    ruler = dict(ruler_a(), id=66780, children=[72297], age=25)
    sim = {"primary_title_id": 463, "heirs": [
        {"character_id": 72297, "is_player_heir": True, "titles": [county(463, "c_dublin"), county(501, "c_breifne")]},
        {"character_id": 15819, "is_player_heir": False, "titles": [county(513, "c_ossory")]}]}
    out = plan(Gw(ruler, {72297: kid(72297, "Domnall", age=0)}, sim))
    assert out["outcome"] == "intact" and out["passes_intact"] is True
    assert out["breaker"] is None and out["breaker_simulation"]["id"] == 15819


# ── the levers ───────────────────────────────────────────────────────

def lever_sim():
    return old_sim(recorded_succession("a_observe_1071_12_22"), [county(463, "c_dublin")])


def lever_kids():
    return {36124: kid(36124, "Domnall", alive=False), 37111: kid(37111, "Enna", alive=False),
            37528: kid(37528, "Donnchad")}


def test_B1_vows_and_disinherit_are_priced_and_the_forecast_after_is_intact():
    ruler = dict(RULER_A)          # piety 588.8 (recorded), prestige -39.71 (recorded)
    kids = lever_kids()
    sim = lever_sim()
    # query_dynasty's `prestige` is the ACCUMULATED renown; `renown` is what a price is paid from
    out = plan(Gw(ruler, kids, sim, dynasty={"head_character_id": 31506, "prestige": 1470.0, "renown": 420.0}))
    t = out["levers"]["take_out_of_split"]
    v, = t["vows"]
    assert v["son_id"] == 37528 and v["price"] == {"piety": 350} and v["you_hold"]["piety"] == 588.8
    assert v["allowed_now"] is None and v["why_not"] is None          # nothing read refuses; his answer unread
    assert v["call"]["args"]["args"] == {"interaction": "take_vows_interaction", "target_id": 37528}
    # after: no eligible child left and the primary heir a grandson: who of his line shares is unread
    assert v["after"] == {"takers": [66780], "intact": None, "leaves_realm": []}
    d, = t["disinherit"]
    # A younger share under partition is heir of the titles he takes (any_heir_title): +150 prestige,
    # +75 x era renown (live: the game priced each son 300 / 300). A test run's 150 was a son
    # who was heir of nothing (one county, no split)
    assert d["price"]["prestige"] == 300 and d["price"]["renown"] == 300
    assert d["allowed_now"] is False and any("prestige -39 < 300" in w for w in d["why_not"])
    assert out["levers"]["disinherit"]["dynasty_renown"] == 420.0


def test_B2_disinherit_needs_the_dynasty_head_and_renown():
    ruler = dict(RULER_A, prestige=500.0)
    kids = lever_kids()
    sim = lever_sim()
    out = plan(Gw(ruler, kids, sim, dynasty={"head_character_id": 999, "prestige": 1400.0, "renown": 100.0}))
    d, = out["levers"]["take_out_of_split"]["disinherit"]
    assert d["allowed_now"] is False
    assert any("not your dynasty's head" in w for w in d["why_not"])
    assert any("renown 100 < 300" in w for w in d["why_not"])
    ok = plan(Gw(ruler, kids, sim, dynasty={"head_character_id": 31506, "prestige": 1400.0, "renown": 400.0}))
    assert ok["levers"]["take_out_of_split"]["disinherit"][0]["allowed_now"] is True


def test_B3_vows_refused_for_a_landed_or_poor_ruler():
    ruler = dict(RULER_A, piety=100.0)
    kids = lever_kids()
    sim = lever_sim()
    kids[37528] = kid(37528, "Donnchad", ruler_tier=2)
    out = plan(Gw(ruler, kids, sim))
    v, = out["levers"]["take_out_of_split"]["vows"]
    assert v["allowed_now"] is False
    assert any("holds land" in w for w in v["why_not"]) and any("piety 100 < 350" in w for w in v["why_not"])
    d, = out["levers"]["take_out_of_split"]["disinherit"]
    assert d["price"] == {"prestige": 900, "renown": 450, "era_factor": d["price"]["era_factor"]}   # landed


def test_B4_no_lever_rows_when_nothing_splits():
    ruler = dict(ruler_a(), children=[37528])
    sim = {"primary_title_id": 463, "heirs": [{"character_id": 37528, "is_player_heir": True,
                                               "titles": [county(463, "c_dublin")]}]}
    out = plan(Gw(ruler, {37528: kid(37528, "Donnchad")}, sim))
    assert out["outcome"] == "intact" and "take_out_of_split" not in out["levers"]


# ── the death estimate and the news rows ────────────────────────────

GAME_DEFINES = os.path.expanduser("~/.local/share/Steam/steamapps/common/Crusader Kings III/game/common/defines/00_defines.txt")


@pytest.mark.skipif(not os.path.exists(GAME_DEFINES), reason="the game's defines are not installed here")
def test_C1_the_constants_are_the_game_files():
    assert RD is not None
    txt = open(GAME_DEFINES, encoding="utf-8-sig").read()
    for name, val in (("DIE_HEALTH_TRESHOLD", RD.DIE_HEALTH_THRESHOLD), ("DIE_HEALTH_CHANCE_ZERO", RD.DIE_HEALTH_CHANCE_ZERO),
                      ("LOWER_HEALTH_START_AGE", RD.LOWER_HEALTH_START_AGE),
                      ("LOWER_HEALTH_BASE_CHANCE", RD.LOWER_HEALTH_BASE_CHANCE),
                      ("LOWER_HEALTH_YEARLY_INCREASE", RD.LOWER_HEALTH_YEARLY_INCREASE)):
        line = next(x for x in txt.splitlines() if x.strip().startswith(name + " "))
        assert float(line.split("=")[1].split("#")[0]) == val, name
    line = next(x for x in txt.splitlines() if x.strip().startswith("LOWER_HEALTH_AMOUNT "))
    assert float(line.split("=")[1].split("#")[0]) == -RD.LOWER_HEALTH_AMOUNT


def test_C2_the_estimate_uses_health_and_takes_the_earlier_year():
    assert RD.monthly_death_chance(3.0) == 0 and RD.monthly_death_chance(0) == 0.25
    assert abs(RD.monthly_death_chance(1.5) - 0.0625) < 1e-9
    # game A 1071: age 46, health 4.35. this read does not give 19 years by
    # the age heuristic, the health check alone being later still; the recorded deaths (test_death_estimate_and_faction_threat.py) show
    # both far too long, so the band's 25 % year (with the fitted outside death rate) is now the earlier
    eta, basis, est = RD.eta_years(46, 4.35)
    assert eta < 19 and basis.startswith("health") and est["years"]["p25"] < 19
    # an ill ruler: the health check is the earlier year
    eta, basis, est = RD.eta_years(55, 1.5)
    assert eta < 2 and basis.startswith("health")
    assert RD.eta_years(46, None)[1].startswith("age_heuristic") and RD.eta_years(None, 4)[0] is None


def test_C2b_past_65_the_health_check_speaks():
    # a live check 1066.09.15: rulers of 71-82 with health 2.5-3.3 all read eta 0 by the age heuristic
    eta, basis, est = RD.eta_years(82, 2.5)
    # With the fitted outside death rate an 82-year-old's p25 is ~1.1 years
    assert eta == int(est["years"]["p25"]) and eta >= 1 and basis.startswith("health")


def test_A9_a_partition_law_the_forecast_does_not_model_refuses():
    # a live read: a clan realm's clan_impassive_partition_succession_law read as "not a partition"
    fc = PF.forecast_from_reads(ruler={"ruler_tier": 3}, law_names=["clan_impassive_partition_succession_law", MALE_PREF],
                children=[], sim={}, primary_heir_id=1)
    assert fc["takers"] == "unknown" and "does not model" in fc["refused"]


def test_C3_succession_plan_carries_the_estimate():
    ruler = dict(RULER_A, health=1.0, age=60)
    kids = lever_kids()
    sim = lever_sim()
    out = plan(Gw(ruler, kids, sim))
    assert out["succession_eta_years"] == 0 and out["death_estimate"]["health"] == 1.0
    assert out["succession_eta_basis"].startswith("health")


def _snap(age, health, split, titles=None):
    import news as N
    return N.snapshot(actor_id=31506, date="1078.01.01", chr_sect={"age": age, "health": health},
                      wars={}, claims={}, council=None, heir=None, split=split,
                      held={"held_titles": titles or []})


def test_C4_split_ahead_fires_once_per_taker_set_at_50_or_poor_health():
    import news as N
    split = {"takers": [66780, 37528], "intact": False, "leaves_realm": [37528]}
    young = _snap(46, 4.35, split)
    assert N.split_rows(None, young, {}) == {}
    old = _snap(52, 3.75, split)
    rows = N.split_rows(young, old, {})
    assert rows["heir_split_ahead"]["leaves_realm"] == [37528]
    assert rows["heir_split_ahead"]["next"]["tool"] == "succession_plan"
    again = _snap(53, 3.7, split)
    assert N.split_rows(old, again, {}) == {}                   # once per taker set
    more = _snap(53, 3.7, dict(split, takers=[66780, 37528, 80000], leaves_realm=[37528, 80000]))
    assert "heir_split_ahead" in N.split_rows(again, more, {})


def test_C5_inheriting_a_county_turns_the_succession_split():
    import news as N
    before = _snap(42, 4.6, {"takers": [36124], "intact": True})
    after = _snap(42, 4.6, {"takers": [36124, 37111, 37528], "intact": False, "leaves_realm": [37111, 37528]})
    rows = N.split_rows(before, after, {"titles_gained": [{"title_id": 508, "key": "c_leinster"}]})
    assert rows["succession_turned_split"]["titles_gained_this_step"] == ["c_leinster"]
    assert "heir_split_ahead" not in rows          # 42 and healthy


# ── threats ──────────────────────────────────────────────────────────

GRAPH = """# NODES
@dublin +meath/ireland !31506
@athlone +meath/ireland !29585
@leinster +leinster/ireland !27052
# EDGES
@dublin > athlone leinster
"""


def _threat_gw(cbs_by_attacker, my_counties=1, tier=2):
    me = {"id": 31506, "ruler_tier": tier, "military_strength": 500, "culture_id": 7}

    def char(p):
        cid = p.get("character_id")
        return {31506: me, 29585: {"id": 29585, "name": "Conchobar", "top_liege_id": 29585, "ruler_tier": 2,
                                   "military_strength": 571},
                27052: {"id": 27052, "name": "Diarmait", "top_liege_id": 27052, "ruler_tier": 2,
                        "culture_id": 7, "military_strength": 1400}}.get(cid, {})

    def realm(p):
        return {"military_strength": 500, "realm_total_counties": my_counties, "ruler_tier": tier}

    extra = {"query_title_hierarchy": {"held_titles": [{"key": "c_dublin", "tier": 2}]},
             "query_map_graph": {"text": GRAPH}, "query_character": char, "query_realm": realm,
             "query_cbs_for_target": lambda p: {"status": "ok", "cbs": cbs_by_attacker.get(p["attacker_id"], [])},
             "query_innovation_progress": {"innovations": [{"name": "innovation_casus_belli", "is_known": True}]}}
    return Gw(me, {}, {}, extra=extra)


def test_D1_threats_lists_who_can_declare_with_his_check_and_the_size_rule():
    import threats as T
    T.reset()
    gw = _threat_gw({27052: [{"cb_key": "claim_cb", "valid": True, "cb_title_id": 463},
                             {"cb_key": "vassalization_cb", "valid": False}],
                     29585: [{"cb_key": "claim_cb", "valid": False}]})
    res = run(T.read(gw, 31506))
    a = {r["id"]: r for r in res["attackers"]}
    assert [r["id"] for r in res["attackers"]] == [27052, 29585]            # strongest first
    assert a[27052]["can_declare"] == ["claim_cb"] and a[27052]["x_yours"] == 2.8
    assert a[29585]["can_declare"] == []
    assert a[27052]["vassalization"]["limit"] == 7 and a[27052]["vassalization"]["valid_now"] is False
    assert "8 counties" in res["vassalization"]["verdict"] and "7 more close it" in res["vassalization"]["verdict"]
    asked = [p for x, p in gw.reads if x == "query_cbs_for_target"]
    assert {p["attacker_id"] for p in asked} == {27052, 29585} and {p["target_id"] for p in asked} == {31506}
    assert "farther than two map steps" in res["not_covered"]


def test_D2_a_crown_closes_vassalization_and_new_attackers_are_news():
    import threats as T
    T.reset()
    assert "crown" in T.vassalization_summary(4, 3)["verdict"]
    gw = _threat_gw({27052: []})
    run(T.read(gw, 31506))
    before = dict(T._LAST[31506])
    gw2 = _threat_gw({27052: [{"cb_key": "claim_cb", "valid": True}]})
    run(T.read(gw2, 31506))
    assert T.new_attackers(31506, before) == [{"id": 27052, "cbs": ["claim_cb"]}]


def test_A8_a_count_with_one_county_passes_it_intact_whatever_his_sons():
    # 1066.09.15, every run: Murchad held Dublin alone with three living sons; the verdict read intact
    out = PF.forecast(ruler_tier=2, succession_law=CONF, gender_law=MALE_PREF, primary_heir_id=36124,
                      children=[{"id": c, "alive": True, "is_female": False} for c in (36124, 37111, 37528)],
                      held_counties=1)
    assert out["takers"] == [36124] and out["intact"] is True
    assert {e["id"] for e in out["excluded_children"]} == {37111, 37528}
    two = PF.forecast(ruler_tier=2, succession_law=CONF, gender_law=MALE_PREF, primary_heir_id=36124,
                      children=[{"id": c, "alive": True, "is_female": False} for c in (36124, 37111, 37528)],
                      held_counties=2)
    assert two["takers"] == [36124, 37111]       # Leinster inherited in 1068: one more share, the next son
    unread = PF.forecast(ruler_tier=2, succession_law=CONF, gender_law=MALE_PREF, primary_heir_id=36124,
                         children=[], held_counties=None)
    assert unread["takers"] == "unknown" and "counties you hold" in unread["refused"]


def test_D2b_each_news_baseline_sees_a_new_attacker():
    # a live check 1067-1068: the environment's turn news read first; the seat's observe diffed against that
    # shared read and never said it
    import news as N
    import threats as T
    N.reset()
    snap = {"actor_id": 31506, "date": "1066.09.15"}
    run(N.threat_rows(_threat_gw({27052: []}), snap, "turn"))
    run(N.threat_rows(_threat_gw({27052: []}), snap, "observe"))
    later = {"actor_id": 31506, "date": "1067.03.14"}
    gw = _threat_gw({27052: [{"cb_key": "make_tributary_cb", "valid": True}]})
    a = run(N.threat_rows(gw, later, "turn"))
    b = run(N.threat_rows(gw, later, "observe"))
    assert a["attackers"][0]["cbs"] == ["make_tributary_cb"] and b["attackers"][0]["cbs"] == ["make_tributary_cb"]
    assert run(N.threat_rows(gw, {"actor_id": 31506, "date": "1067.04.14"}, "observe")) is None   # within 90 days
    T.reset()


def test_A10_a_duke_with_two_counties_and_six_sons():
    # a live check (the forecast of turn 20 and the reads at turn 24, 1077.11.05): 27221, duke of
    # Maru under partition_succession_law, held d_maru, c_mandavyapura, c_naddula; 6 sons. The game: the primary heir
    # 29889 took d_maru and c_mandavyapura (independent), 30022 took c_naddula as his VASSAL, the other four nothing.
    # The live reply had listed all six as takers (the cap was a count's only) and the county with the primary.
    sons = [29889, 30022, 30613, 64378, 40743, 13124]
    kids = [{"id": c, "alive": True, "is_female": False} for c in sons]
    sim = {"partition_heir_basis": "children_in_line", "heirs": [
        {"character_id": 29889, "is_player_heir": True, "titles": [
            {"title_id": 10693, "tier": 3}, {"title_id": 10694, "tier": 2}, {"title_id": 10707, "tier": 2}]}]}
    fc = PF.forecast_from_reads(ruler={"ruler_tier": 3}, law_names=["partition_succession_law", MALE_PREF], children=kids, sim=sim,
                primary_heir_id=29889, map_titles=[{"title_id": 10693, "heir_id": 29889},
                                                   {"title_id": 10694, "heir_id": 29889},
                                                   {"title_id": 10707, "heir_id": 30022}])
    assert fc["takers"] == [29889, 30022]
    sh, = fc["younger_shares"]
    assert sh["id"] == 30022 and sh["fate"] == "stays_vassal" and sh["takes"] == [10707]
    rows = PF.title_fates([{"title_id": 10693}, {"title_id": 10694}, {"title_id": 10707}], fc)
    assert [r["splits_away"] for r in rows] == [False, False, False] and rows[2]["heir_id"] == 30022


def test_D3_inspect_threats_is_a_subject():
    assert "threats" in R.INSPECT_SUBJECTS
