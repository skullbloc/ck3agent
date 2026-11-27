"""An empire's land requirement and tier rule, the crown-authority price, reply shaping, and the AI's spending
attributed in the purse. Fixture values are the pre-flight's recorded live replies at West Francia 867 (Charles
12015) and Munster 1066 (Murchad 31762), each named where used; the recordings themselves are walked by a
research-tree test."""
from __future__ import annotations

import asyncio
import json
import re
from pathlib import Path

import pytest

import event_prices as EP
import news
import redesign as R
import reply_fit as F
import seat_events as SE
from test_ireland_walk_replies import Gw

#: The game's files (CK3_GAME_DIR or the Steam default): the rows marked need_files read them.
need_files = pytest.mark.skipif(SE.game_dir() is None, reason="the game's files are not on this host")

ROOT = Path(__file__).resolve().parents[2]
CHARLES = 12015


def run(c):
    return asyncio.new_event_loop().run_until_complete(c)


# ── The land each tier needs, and the tier rule ────────────────────────────────────────────────

def test_1_the_empire_needs_more_than_80_percent_and_a_kingdom_half():
    """A recorded game, 867: the game said "You only have 89 of 94 required Counties" for e_france (117)."""
    assert R.dejure_need(117, 5) == 94
    assert R.mint_dejure_shortfall("e_france", 89, 117) == {"held": 89, "need": 94, "total": 117}
    assert R.mint_dejure_shortfall("e_france", 94, 117) is None
    assert R.dejure_need(100, 5) == 81                     # "more than 80%": 80 of 100 is not enough
    assert R.dejure_need(47, 4) == 24 and R.dejure_need(45, 4) == 23     # k_france, k_aquitaine as observe read
    # More than half (the game's own lines: d_don_valley 4 -> 3, k_ireland 14 -> 8), not the text's "at least"
    assert R.dejure_need(4, 3) == 3 and R.dejure_need(5, 3) == 3
    assert R.dejure_need(46, 4) == 24 == R.game_dejure_need(46, 4)


def test_1_the_shares_are_the_games_concept_texts():
    """The share by tier, read from the install's own text when it is there."""
    import seat_events as _se
    root = _se.game_dir()
    p = root / "localization" / "english" / "game_concepts_l_english.yml" if root else None
    if p is None or not p.is_file():
        pytest.skip("no game install here")
    text = p.read_text(encoding="utf-8-sig", errors="replace")

    def desc(k):
        return re.search(rf"^\s*{k}:\d*\s*\"(.*)\"\s*$", text, re.M).group(1)
    assert "at least half of its" in desc("game_concept_duchy_desc")
    assert "at least half of the" in desc("game_concept_kingdom_desc")
    assert "more than 80% of the" in desc("game_concept_empire_desc")
    # The texts say "at least half"; the game's own check counts more than half (a recorded game k_ireland 14 -> 8)
    assert R.GAME_DEJURE_SHARE == {3: (True, 1, 2), 4: (True, 1, 2), 5: (True, 4, 5)}


def test_1_the_bridge_mirrors_the_empire_row():
    src = (ROOT / "bridge" / "include" / "dejure_bar.h").read_text(encoding="utf-8")   # The rule moved here
    got = {k: int(re.search(rf"CREATE_TITLE_DEJURE_EMPIRE_{k}\s*=\s*(\d+)", src).group(1))
           for k in ("TIER", "NUM", "DEN")}
    strict, num, den = R.GAME_DEJURE_SHARE[got["TIER"]]
    assert strict and (got["NUM"], got["DEN"]) == (num, den)
    assert "held * CREATE_TITLE_DEJURE_EMPIRE_DEN <= total * CREATE_TITLE_DEJURE_EMPIRE_NUM" in src   # "more than"


#: A recorded game, 867 seq 22 (do formable_titles e_france): the game's check lines; seq 4 observe de_jure: the two
#: kingdom titles he holds (k_france 7052, k_aquitaine 7259); price 850 against 453 gold
E_FRANCE_CHECK = {"success": True, "engine_valid": False,
                  "engine_reasons": ["You only have 89 of 94 required Counties", "King Charles II does not have gold 850"],
                  "price": {"known": True, "currency": "gold", "cost": 850.0, "have": 453.0}}
HELD_867 = {"held_titles": [{"title_id": 7052, "key": "k_france", "tier": 4},
                            {"title_id": 7259, "key": "k_aquitaine", "tier": 4},
                            {"title_id": 7054, "key": "c_ile_de_france", "tier": 2}], "held_title_count": 3}


def _e_france_gw(held=HELD_867):
    return Gw({"query_dejure_progress": {"held": 89, "total": 117, "fraction": 0.76},
               "query_create_title_cost": E_FRANCE_CHECK,
               "query_title_hierarchy": lambda p: held if p.get("character_id") else {},
               "query_character": {"id": CHARLES, "gold": 453.0}})


@need_files
def test_1_e_france_reads_94_unmet_and_the_tier_rule_from_his_two_kingdoms():
    row, blk = run(R.title_requirements(_e_france_gw(), CHARLES, "e_france"))
    req = {q["requirement"]: q for q in row["requirements"]}
    assert req["de_jure_counties"] == {"requirement": "de_jure_counties", "have": 89, "need": 94, "of": 117,
                                       "met": False, "source": R.GAME_COUNTIES_SOURCE}   # The game's line
    assert row["measured"]["need"] == 94 and blk["gate"] == "de_jure_control"
    tr = req["tier_rule"]
    assert tr["met"] is True and "Kingdom" not in tr["rule"] and "hold 2 kingdom titles (you hold 2)" in tr["rule"]
    assert "CREATE_TITLE_OR_TIER_EMPIRE" in tr["source"]


@need_files
def test_1_CONTROL_one_kingdom_is_unmet_by_the_count_and_unknown_beside_a_silent_check():
    one = {"held_titles": HELD_867["held_titles"][:1], "held_title_count": 1}
    row, _ = run(R.title_requirements(_e_france_gw(one), CHARLES, "e_france"))
    tr = next(q for q in row["requirements"] if q["requirement"] == "tier_rule")
    assert tr["met"] is None and tr["unread"] == R.TIER_RULE_DISAGREE


def test_1_CONTROL_no_held_titles_read_keeps_the_games_check_reading():
    row, _ = run(R.title_requirements(_e_france_gw({}), CHARLES, "e_france"))
    tr = next(q for q in row["requirements"] if q["requirement"] == "tier_rule")
    assert tr == {"requirement": "tier_rule", "met": True, "rule": R.TIER_RULE_PASSED}


def test_1_the_defines_give_the_tier_rows():
    rows = EP.title_or_tier()
    if rows is None:
        pytest.skip("no game install here")
    assert rows[5] == [-1, -1, -1, -1, 2, 1, 1] and rows[4] == [-1, -1, -1, 2, 1, 1, 1]


# ── The crown-authority price ──────────────────────────────────────────────────────────────────

def test_2_the_rule_gives_both_live_prices():
    """Munster 1066.10.18 ai_acted crown_authority_1 143.0 (3 realm counties, learning 3); West Francia 867.03.23
    792.0 (89 counties, learning 18): recorded query realm / query character and the bridge's veto record."""
    assert EP.crown_authority_price(3, 3)["prestige"] == 143.0
    assert EP.crown_authority_price(89, 18)["prestige"] == 792.0
    assert EP.crown_authority_price(200, 8)["prestige"] == 1000.0       # the realm part stops at 900
    assert EP.crown_authority_price(10, 40)["prestige"] == 100.0        # learning takes at most half off
    assert EP.crown_authority_price(None, 3) is None


def test_2_the_constants_are_the_install_files():
    import seat_events as _se
    root = _se.game_dir()
    p = root / "common" / "script_values" / "00_law_values.txt" if root else None
    if p is None or not p.is_file():
        pytest.skip("no game install here")
    t = p.read_text(encoding="utf-8-sig", errors="replace")
    assert re.search(r"^increase_crown_authority_prestige_base = (\d+)", t, re.M).group(1) == str(EP.CROWN_AUTHORITY_BASE)
    sub = re.search(r"subrealm_size_modifier = \{(.*?)\n\}", t, re.S).group(1)
    assert f"add = {EP.CROWN_AUTHORITY_PER_COUNTY}" in sub and f"max = {EP.CROWN_AUTHORITY_REALM_MAX}" in sub
    lm = re.search(r"learning_law_cost_modifier = \{(.*?)\n\}", t, re.S).group(1)
    assert f"add = {EP.LAW_LEARNING_PIVOT}" in lm and f"divide = {EP.LAW_LEARNING_DIV}" in lm and \
        f"min = {EP.LAW_LEARNING_FLOOR}" in lm


def test_2_the_unset_law_veto_row_prices_the_raise_before_the_ai_sends_it():
    snap = {"ai_vetoes": [], "actor_id": CHARLES, "gold": 453.0, "prestige": 1050.0, "realm_counties": 89,
            "learning": 18, "law_levels": {"crown_authority": {"law": "crown_authority_0", "level": 0, "max_level": 3}}}
    row = news.veto_unset(snap)["law"]
    assert row["price_estimate"]["prestige"] == 792.0 and "price_unread" not in row
    assert row["next"] == news.ai_veto_call("law") and "(8 - his learning) / 50" in row["why"]
    # CONTROL: unread learning keeps the unread wording
    row = news.veto_unset(dict(snap, learning=None))["law"]
    assert "price_estimate" not in row and "price_unread" in row


def test_2_the_turn_one_note_states_the_rule_and_the_preset_covers_law():
    import notes as N
    text = " ".join(N.turn_one_texts(N.load()))
    # No price a read gives (the unset law row prices the raise: test above), so the note names laws only
    assert "~140" not in text and "100 prestige +10 a realm county" not in text
    assert text.index("Before step 1: do ai_veto what=spending") < text.index("laws, ")
    assert "law" in SE.AI_VETO_PRESETS["spending"]


def test_2_the_laws_listing_prices_each_crown_authority_raise():
    gw = Gw({"query_available_law_changes": {"groups": [
        {"group_name": "crown_authority", "current_law": "crown_authority_0",
         "options": [{"name": "crown_authority_1"}, {"name": "crown_authority_2"}]},
        {"group_name": "succession_order_laws", "current_law": "confederate_partition_succession_law",
         "options": [{"name": "partition_succession_law"}]}]},
        "query_realm": {"realm_total_counties": 89},
        "query_character": {"id": CHARLES, "skills": {"learning": 18}}})
    env = run(R.do_impl(gw, intent=None, action_class="laws", candidates=None, character_id=CHARLES, args=None))
    rows = env["result"]["available"]
    assert [r.get("prestige_price") for r in rows] == [792.0, 792.0, None]
    assert any(w.get("code") == "law_price_rule" for w in env["warnings"])


# ── The shaping rules (the recordings are walked in the kit) ────────────────────────────────────

def test_3_a_reply_under_the_cap_is_the_same_object():
    env = {"ok": True, "result": {"rows": [1, 2, 3]}}
    assert F.fit(env, "inspect", {"subject": "vassals"}) is env


def test_3_counties_still_missing_are_never_hidden():
    """A title of 117 counties, 28 not in the realm: every missing county is shown or named."""
    rows = [{"title_id": i, "key": f"c_{i}", "name": "N" * 20, "holder_id": i, "holder_name": "H" * 10,
             "realm_ruler_id": 1 if i < 89 else 2, "realm_ruler_name": "R", "realm_ruler_strength": 999,
             "you_control": i < 89, "your_claim": False,
             **({} if i < 89 else {"next_call": {"tool": "do", "args": {"intent": "council_task", "args": {
                 "seat": "court_chaplain", "task": "task_fabricate_claim", "target_title_id": i}}}})}
            for i in range(117)]
    env = {"ok": True, "tool": "inspect", "result": {"subject": "title", "counties": rows,
                                                     "duchies": [{"key": f"d_{i}", "x": "y" * 300} for i in range(23)]}}
    out = F.fit(env, "inspect", {"subject": "title"})
    res = out["result"]
    assert F.size(out) <= F.CAP
    shown = {r["key"] for r in res["counties"] if r.get("you_control") is False}
    (cut,) = [x for x in res["truncated"]["lists"] if x["list"] == "counties"]
    assert shown | set(cut.get("not_shown") or []) == {f"c_{i}" for i in range(89, 117)}
    assert cut["of"] == 117 and res["counties"][0]["you_control"] is False
    tpl = res["next_call_templates"]
    assert tpl[res["counties"][0]["next_call"]]["args"]["args"]["target_title_id"] == "<title_id>"


def test_3_CONTROL_without_the_rank_the_missing_counties_go_first():
    spec = dict(F.SPECS["inspect:title"])
    keep = F.SPECS["inspect:title"]
    try:
        F.SPECS["inspect:title"] = {k: v for k, v in spec.items() if k not in ("rank", "cut")}
        rows = [{"key": f"c_{i}", "you_control": i < 89, "pad": "p" * 200} for i in range(117)]
        out = F.fit({"ok": True, "result": {"counties": rows}}, "inspect", {"subject": "title"})
        assert all(r["you_control"] for r in out["result"]["counties"])     # the missing ones were cut
    finally:
        F.SPECS["inspect:title"] = keep


# ── The AI's spending in the purse ──────────────────────────────────────────────────────────────

#: A recorded game, Munster observe 1066.12.01: the news' ai_acted row and the gold row (before the join was read)
MUNSTER_ACTED = [{"date": "1066.11.03", "what": "construction", "location_id": 45,
                  "price_unread": "the bridge does not read a building's price; gold_at_attempt is your gold when the AI sent it",
                  "gold_at_attempt": 208.83}]
MUNSTER_BUILT = [{"building": "hunting_grounds_01", "started": "1066.11.03", "ordered_by": 31762, "county": "Thomond",
                  "barony_title_id": 520, "price_estimate": -100.0}]


def test_4_the_ai_construction_is_joined_and_counted_in_the_step():
    acted = json.loads(json.dumps(MUNSTER_ACTED))
    g = {"was": 208.83, "now": 111.73, "change": -97.1, "income": {"monthly": 3.72, "expected": 3.67},
         "unexplained": -100.77, "cause": "partly unknown: ..."}
    news.join_constructions(acted, MUNSTER_BUILT, 31762)
    assert acted[0]["price_estimate"] == {"gold": -100.0} and acted[0]["building"] == "hunting_grounds_01"
    news.count_constructions_in_step(g, MUNSTER_BUILT, 31762, acted)
    assert g["in_this_step"][0]["expected"] == -100.0 and "ai_acted" in g["in_this_step"][0]["what"]
    assert "unexplained" not in g and "cause" not in g          # -0.77 left: under the floor


def test_4_CONTROL_a_construction_ordered_by_another_is_not_counted():
    g = {"change": -97.1, "unexplained": -100.77}
    news.count_constructions_in_step(g, [dict(MUNSTER_BUILT[0], ordered_by=999)], 31762, [])
    assert g == {"change": -97.1, "unexplained": -100.77}


#: A recorded game, 867 query_ai_veto (0868.01.01): the census of the AI's interactions for Charles
CENSUS_867 = [{"character_id": CHARLES, "interaction": "grant_titles_interaction", "count": 4, "first": "0867.04.08",
               "last": "0867.05.02"},
              {"character_id": CHARLES, "interaction": "grant_vassal_interaction", "count": 1, "first": "0867.05.10",
               "last": "0867.05.10"},
              {"character_id": CHARLES, "interaction": "sway_interaction", "count": 1}]


def test_4_a_grant_names_the_ais_census_count():
    prev = SE.snapshot_part(CHARLES, None, None, {"interactions": []})
    snap = SE.snapshot_part(CHARLES, None, None, {"interactions": CENSUS_867})
    assert [x["interaction"] for x in snap["ai_grants_sent"]] == ["grant_titles_interaction", "grant_vassal_interaction"]
    lost = [{"key": "c_berry", "how": "granted_to_vassal", "holder_id": 15319}, {"key": "b_deols", "how": "granted_to_vassal"},
            {"key": "c_x", "how": "conquered"}]
    news.granted_by_ai(prev, snap, lost)
    assert "grant_titles_interaction x4" in lost[0]["sent_by"] and lost[0]["veto"] == news.GRANT_NO_VETO
    assert "sent_by" not in lost[2]
    # CONTROL: no new grant in the step names no sender
    lost2 = [{"key": "c_orleans", "how": "granted_to_vassal"}]
    news.granted_by_ai(snap, snap, lost2)
    assert "sent_by" not in lost2[0]


def test_4_the_unclassified_census_reaches_the_purse_row():
    prev = SE.snapshot_part(CHARLES, None, None, {"unclassified_commands": []})
    snap = SE.snapshot_part(CHARLES, None, None, {"unclassified_commands": [
        {"character_id": CHARLES, "class": "26CRazeHoldingCommand", "vtable_rva": "0xbc62c40", "count": 2, "last": "0867.03.01"},
        {"character_id": 999, "class": "X", "vtable_rva": "0x1", "count": 5}]})
    assert SE.unclassified_in_step(prev, snap) == [{"class": "26CRazeHoldingCommand", "vtable_rva": "0xbc62c40", "posts": 2,
                                                   "last": "0867.03.01"}]
    assert SE.unclassified_in_step(snap, snap) == []
    assert SE.unclassified_in_step(prev, SE.snapshot_part(CHARLES, None, None, {})) is None


def test_3_every_read_tool_sends_through_the_one_rule():
    """mcp_server: each read tool's returns pass reply_fit.sent (the rule the recorded-reply walk exercises)."""
    import ast
    tree = ast.parse((Path(__file__).with_name("mcp_server.py")).read_text(encoding="utf-8"))
    fns = {n.name: n for n in tree.body if isinstance(n, ast.AsyncFunctionDef)}
    for name in ("observe", "plan_war", "succession_plan", "do", "inspect", "cbs", "discover"):
        calls = [c for c in ast.walk(fns[name]) if isinstance(c, ast.Call) and isinstance(c.func, ast.Attribute)
                 and c.func.attr == "sent" and getattr(c.func.value, "id", None) == "_fit"]
        assert calls, name
