"""The figures served text states as the game's rule, held against the game's own files.

Each test reads the installed game's `common/` and `events/` text files (CK3_GAME_DIR, or the default Steam path)
and checks that the figure a served string states is the one the file gives, for the scope the string states: the
defeat price of each casus belli family in the war-stance reply's surrender cost, the white peace and the declare
price in the notes, the claim-war yield's gold rule, the fabrication's rate terms, the coping trait's gold floor and
the disinherit price of a son who would take a share. Skipped where the game is not installed.
"""
from __future__ import annotations

import json
import os
import re
from pathlib import Path

import pytest

import news
import redesign as R
import seat_events

GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
HERE = Path(__file__).resolve().parent


def game_file(rel: str) -> str:
    p = GAME / rel
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    return p.read_text(encoding="utf-8-sig", errors="replace")


def block(text: str, name: str, start: int = 0) -> str:
    """The first `name = { ... }` block at or after `start` (any indentation)."""
    m = re.compile(r"(?m)^\s*" + re.escape(name) + r"\s*=\s*\{").search(text, start)
    assert m, name
    depth = 0
    for k in range(text.index("{", m.start()), len(text)):
        depth += {"{": 1, "}": -1}.get(text[k], 0)
        if depth == 0:
            return text[m.start():k + 1]
    raise AssertionError(name)


def value(text: str, name: str) -> float:
    """A script value given as `name = N` at the top level of a values file."""
    m = re.search(r"(?m)^" + re.escape(name) + r"\s*=\s*(-?[\d.]+)", text)
    assert m, name
    return float(m.group(1))


def note(note_id: str) -> str:
    texts = set()

    def walk(x):
        if isinstance(x, dict):
            if x.get("id") == note_id and isinstance(x.get("text"), str):
                texts.add(x["text"])
            for v in x.values():
                walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)
    walk(json.loads((HERE / "notes.json").read_text(encoding="utf-8")))
    (t,) = texts
    return t


def basic() -> str:
    return game_file("common/script_values/00_basic_values.txt")


def prestige(name: str) -> float:
    return value(basic(), name)


def cb_end(file: str, cb: str, ending: str) -> str:
    t = game_file(f"common/casus_belli_types/{file}")
    return block(block(t, cb), ending)


# ── the surrender cost: each family's defeat as the files give it ────────────────────────────

def _defeat(file, cb):
    d = cb_end(file, cb, "on_defeat")
    gold = re.search(r"pay_short_term_gold_reparations_effect\s*=\s*\{\s*GOLD_VALUE\s*=\s*(\d+)", d)
    years = re.search(r"pay_short_term_income\s*=\s*\{\s*years\s*=\s*(\d+)", d)
    return d, (int(gold.group(1)) if gold else None), (int(years.group(1)) if years else None)


def test_the_surrender_cost_states_each_casus_belli_family_s_defeat_as_its_file_gives_it():
    src = (HERE / "redesign.py").read_text(encoding="utf-8")
    i = src.index('"surrender_cost": (')
    s = "".join(re.findall(r'"([^"]*)"', src[i:src.index("),", i)]))
    # claim and de jure: 10 x the factor, 3 years
    for file, cb in (("00_claim.txt", "claim_cb"), ("00_dejure_war.txt", "de_jure_cb")):
        d, gold, _ = _defeat(file, cb)
        assert "LOSER_FAME_SCALE = -10" in d and gold == 3, cb
        assert "add_legitimacy_attacker_defeat_effect = yes" in d, cb
    assert re.search(r"remove_claim\s*=\s*prev", cb_end("00_claim.txt", "claim_cb", "on_defeat"))
    assert ("a claim or de jure war: 10 x the war's prestige factor in prestige, legitimacy and 3 years of your "
            "income, and a claim war's claim is lost") in s
    # conquest: 150 or 350, 2 or 3 years
    got = set()
    for cb in ("county_conquest_cb", "duchy_conquest_cb", "ducal_conquest_cb"):
        d, gold, _ = _defeat("00_conquest.txt", cb)
        assert "add_legitimacy_attacker_defeat_effect = yes" in d, cb
        loss = re.search(r"add_prestige\s*=\s*(medium_prestige_loss|\{\s*value\s*=\s*(\w+)\s*multiply\s*=\s*-1)", d)
        name = "medium_prestige_value" if loss.group(1) == "medium_prestige_loss" else loss.group(2)
        got.add((prestige(name), gold))
    assert got == {(150, 2), (350, 3)}, got
    assert "a conquest: 150 or 350 prestige, legitimacy and 2 or 3 years by its casus belli" in s
    # vassalization: 350 and 3 years; tributary: 350, none
    d, gold, _ = _defeat("00_vassalization.txt", "vassalization_cb")
    assert "value = major_prestige_value" in block(d, "add_prestige") and gold == 3 and "legitimacy" not in d
    d, gold, years = _defeat("00_tributarize.txt", "make_tributary_cb")
    assert "value = major_prestige_value" in block(d, "add_prestige") and gold is None and years is None
    assert "legitimacy" not in d
    assert prestige("major_prestige_value") == 350
    assert "a vassalization war: 350 prestige and 3 years; a tributary war: 350 prestige" in s
    # holy wars: no prestige, legitimacy, 2 to 4 years by tier
    yrs = []
    for cb in ("minor_religious_war", "religious_war", "major_religious_war"):
        d, gold, years = _defeat("00_religious_war.txt", cb)
        assert "add_legitimacy_attacker_defeat_effect = yes" in d and gold is None
        assert not re.search(r"(?m)^\s*scope:attacker\s*=\s*\{\s*add_prestige", d), cb
        yrs.append(years)
    assert yrs == [2, 3, 4]
    assert "a holy war: no prestige, legitimacy and 2 to 4 years of income" in s


def test_CONTROL_the_reader_finds_a_figure_the_file_does_not_give():
    d, gold, _ = _defeat("00_conquest.txt", "county_conquest_cb")
    assert gold != 3, "the reader must tell 2 years from 3"


# ── the yield line's gold and the white peace note ────────────────────────────────────────────

def test_reparations_are_years_of_the_game_s_yearly_income_and_the_yield_line_says_its_read_differs():
    t = block(game_file("common/scripted_effects/00_war_effects.txt"), "pay_short_term_gold_reparations_effect")
    assert "yearly_income = yes" in t and "more_gold_for_successful_defensive_wars" in t
    s = R.PEACE_PRICE_MEASURED
    assert "3 years of the game's yearly income" in s and "can differ" in s and "within" not in s


STANDARD_TRUCE_EFFECTS = ("add_truce_attacker_victory_effect", "add_truce_white_peace_effect",
                          "add_truce_attacker_defeat_effect")
TRUCE_ENDINGS = (("victory", "on_victory"), ("white_peace", "on_white_peace"), ("defeat", "on_defeat"))


def _truce_calls(file: str, cb: str, ending: str) -> tuple[bool, bool, bool]:
    """(calls a standard truce effect, adds a truce both ways of 1825 days, adds any truce) for one ending."""
    b = re.sub(r"#[^\n]*", "", cb_end(file, cb, ending))
    both = re.search(r"add_truce_both_ways\s*=\s*\{[^{}]*days\s*=\s*1825\b", b) is not None
    return any(e in b for e in STANDARD_TRUCE_EFFECTS), both, "add_truce" in b


def _own_truce_days(file: str, cb: str) -> bool:
    """The casus belli sets a truce length of its own (truce_days, _casus_belli.info)."""
    t = re.sub(r"#[^\n]*", "", game_file(f"common/casus_belli_types/{file}"))
    return re.search(r"\btruce_days\s*=", block(t, cb)) is not None


def test_the_truce_each_priced_casus_belli_s_ending_makes_is_the_one_its_file_calls():
    import truces
    fx = game_file("common/scripted_effects/00_war_effects.txt")
    for e in STANDARD_TRUCE_EFFECTS:
        assert re.search(r"add_truce_one_way\s*=\s*\{[^{}]*days = standard_truce_duration_days", block(fx, e)), e
    for cb, row in R.PEACE_PRICE.items():
        for ending, name in TRUCE_ENDINGS:
            std, both, anyt = _truce_calls(row["file"], cb, name)
            kind = truces.peace_truce_kind(cb, ending)
            made = truces.made_by_peace("1070.01.01", cb, ending)
            if kind == "one_way":
                assert std and not both and not _own_truce_days(row["file"], cb), (cb, ending)
                assert made["days"] == 1825 and "(one way)" in made["on"], (cb, ending)
            elif kind == "both_ways":
                assert both and not std and made["days"] == 1825 and row["file"] in made["source"], (cb, ending)
            elif kind == "none":
                assert not anyt and made["days"] == 0 and row["file"] in made["source"], (cb, ending)
            else:
                pytest.fail(f"{cb} {ending}: a priced casus belli whose truce is not read")


def test_CONTROL_the_truce_reader_sees_an_ending_without_the_standard_effects_and_a_truce_length_of_its_own():
    import truces
    std, both, _ = _truce_calls("00_civil_war.txt", "liberty_faction_war", "on_white_peace")
    assert not std and both and truces.peace_truce_kind("liberty_faction_war", "white_peace") is None
    assert truces.made_by_peace("1070.01.01", "liberty_faction_war", "white_peace")["days"] == "not read"
    assert _own_truce_days("05_fp3_wars.txt", "fp3_seljuk_invasion_cb")


def test_the_white_peace_and_yield_notes_are_the_claim_family_s_rule():
    for file, cb in (("00_claim.txt", "claim_cb"), ("00_dejure_war.txt", "de_jure_cb")):
        w = cb_end(file, cb, "on_white_peace")
        assert re.search(r"multiply\s*=\s*-5\.0", w), cb
    for file, cb in (("00_conquest.txt", "county_conquest_cb"), ("00_vassalization.txt", "vassalization_cb")):
        w = cb_end(file, cb, "on_white_peace")
        assert "value = minor_prestige_value" in w and "multiply = -5" not in w, cb
    assert "A claim or de jure war's white peace costs its attacker 5 x the war's prestige factor" in \
        note("truce-after-any-war-end")
    assert "a claim war's yield costs 10 x the war's prestige factor" in note("prestige-flow")
    t = game_file("common/script_values/00_war_values.txt")
    std = block(t, "standard_truce_duration_days")
    assert re.search(r"value = 1825", std) and "flexible_truces_perk" in std and "add = -450" in std
    assert "government_is_nomadic" in std and "subtract = 730" in std and "add = 900" in std
    assert std.count("has_perk") == 1, "one perk shortens the truce"
    assert "usually bars its attacker from declaring on that defender for 1825 days (less with the flexible truces perk)" \
        in note("truce-after-any-war-end")
    import truces
    assert all(x in truces.TRUCE_RULE for x in ("1375 with the flexible truces perk", "730 days less between two "
                                                "nomadic rulers", "at least 730 days", "among other modifiers"))


def test_a_war_won_as_attacker_gives_fame_experience_not_prestige():
    t = block(game_file("common/scripted_effects/00_casus_belli_effects.txt"), "modify_all_participants_fame_values")
    first = t[:t.index("else = {\n\t\t# If the defender is the winner")]
    assert "add_prestige_experience" in first and "add_prestige = {" not in first
    assert "a war won as attacker gives fame, not prestige" in note("prestige-flow")


def test_a_won_war_s_served_text_and_table_give_the_attacker_no_prestige():
    """The peace-price rows' winning attacker and the outcome table's `win` (served where the files are not read)
    agree with the files: a win gives prestige experience, which is not prestige."""
    import declare_gates as dg
    import event_prices as ep
    for cb, row in R.PEACE_PRICE.items():
        if cb in dg.OUTCOME_PRICE or cb in ("claim_cb", "de_jure_cb"):
            r = ep.cb_end_prestige(cb, "attacker_victory", "attacker")
            if r is None:
                pytest.skip(f"no game install at {GAME}")
            assert not r.get("prestige"), (cb, r)
            v = row["victory_attacker"]
            assert "gains prestige" not in v or "gains prestige experience (fame), not prestige" in v, cb
    for cb, row in dg.OUTCOME_PRICE.items():
        r = ep.cb_end_prestige(cb, "attacker_victory", "attacker")
        assert r is not None and not r.get("prestige") and row["win"] == 0, cb
    for cb, word in (("ducal_conquest_cb", "minor_prestige_value"), ("duchy_conquest_cb", "medium_prestige_value")):
        v = cb_end("00_conquest.txt", cb, "on_victory")
        assert re.search(r"every_in_list\s*=\s*\{\s*list\s*=\s*target_titles\s*scope:attacker\s*=\s*\{\s*"
                         r"add_prestige_experience\s*=\s*" + word, v), cb
        assert not re.search(r"scope:attacker\s*=\s*\{\s*add_prestige\s*=", v), cb
        assert f"a win gives {prestige(word):g} prestige experience per title taken" in dg.OUTCOME_PRICE[cb]["basis"]


def test_the_hunt_s_prestige_names_its_main_modifiers_and_says_there_are_others():
    t = block(game_file("common/script_values/04_ep2_hunt_values.txt"), "hunt_prestige_reward_value")
    assert t.count("divide = 2") == 2 and "hunt_success ?= flag:no" in t and "female_quarry" in t
    assert "multiply = 1.25" in t and "multiply = 1.5" in t
    assert "hunt_type_legendary" in t and "hunt_type_falconry" in t
    import activities
    g = activities.FACTS["hunt"]["gives"]
    assert "among other modifiers: halved for a failed hunt or a female quarry, x1.25 or x1.5 by the party's size, " \
           "x2 for a legendary hunt or falconry" in g


# ── the declare price of a claim war by tier ─────────────────────────────────────────────────

def test_the_claim_price_note_gives_the_file_s_price_per_title_tier_and_the_pressed_quarter():
    t = block(game_file("common/script_values/99_casus_belli_values.txt"), "claim_cb_prestige_cost")
    own = t[:t.index("else = {\n\t\t# When pressing a 3rd party's claims")]
    got = re.findall(r"tier = tier_(\w+) \}\s*add = \{\s*value = (\d+).*?add = (-\d+)", own, re.S)
    assert [(k, int(v), int(p)) for k, v, p in got] == [
        ("county", 100, -25), ("duchy", 200, -50), ("kingdom", 400, -100), ("empire", 800, -200)]
    assert all(-p * 4 == v for _, v, p in [(k, int(v), int(p)) for k, v, p in got])
    n = note("claim-war-prestige-price")
    assert "per title 100 a county, 200 a duchy, 400 a kingdom, 800 an empire, a quarter less pressed" in n
    assert "value = 1.25" in own and "x1.25 vs another faith" in n


# ── the fabrication's rate ───────────────────────────────────────────────────────────────────

def test_the_fabrication_rate_terms_are_the_task_s_progress_block():
    task = block(game_file("common/council_tasks/00_court_chaplain_tasks.txt"), "task_fabricate_claim")
    prog = block(task, "progress")
    vals = game_file("common/script_values/99_court_chaplain_values.txt")
    council = game_file("common/script_values/00_council_values.txt")
    # every multiply in the block, by the value it names
    mults = re.findall(r"multiply = \{\s*value = ([\w.:]+)", prog)
    by = {(m if m[0].isdigit() else m.split(".")[-1]): m for m in mults}
    assert set(by) == {"court_chaplain_heretic_bonus", "court_chaplain_fabricate_claim_not_adjacent_penalty",
                       "court_chaplain_fabricate_claim_vassal_penalty", "1.3"}, mults
    assert float(re.search(r"court_chaplain_fabricate_claim_vassal_penalty = \{\s*value = ([\d.]+)", vals).group(1)) \
        == news.FABRICATION_VASSAL == 0.3
    assert float(re.search(r"court_chaplain_fabricate_claim_not_adjacent_penalty = \{\s*value = ([\d.]+)",
                           vals).group(1)) == 0.5
    assert float(re.search(r"court_chaplain_heretic_bonus = \{\s*value = ([\d.]+)", vals).group(1)) \
        == news.FABRICATION_OTHER_FAITH
    # the vassal term is on a county whose holder has the ruler as liege or above
    assert "target_is_liege_or_above = scope:councillor.liege" in prog
    # the two relation terms that slow it, as shares of the base
    assert value(council, "council_rival_impact_percentage") == -20
    assert value(council, "council_nemesis_impact_percentage") == -30 == round((news.FABRICATION_NEMESIS - 1) * 100)
    assert "monthly_increase_rival_bonus" in prog and "monthly_increase_nemesis_bonus" in prog
    # the base: 3 + learning / 5
    assert re.search(r"court_chaplain_fabricate_claim_base = \{\s*add = 3", vals)
    assert re.search(r"court_chaplain_fabricate_claim_monthly_increase_base = \{\s*add = learning\s*divide = 5", vals)
    for s in (news.FABRICATION_MEASURED, news.FABRICATION_RULE):
        assert "x0.3 on a county held inside your realm" in s and "x0.7" in s and "x0.8" in s, s


# ── the coping trait's gold ──────────────────────────────────────────────────────────────────

def test_the_coping_trait_gold_floor_is_the_least_floor_of_the_options_that_add_the_trait():
    ev = game_file("events/stress_events/stress_threshold_events.txt")
    golds = set()
    for m in re.finditer(r"add_trait = (?:improvident|profligate)\b", ev):
        opt = ev[max(0, ev.rfind("option = {", 0, m.start())):m.start() + 400]
        g = re.search(r"remove_short_term_gold = (\w+)_gold_value\b(?!_check)", opt)
        if g:
            golds.add(g.group(1))
    floors = {k: value(basic(), f"{k}_gold_min_value") for k in golds}
    assert min(floors.values()) == 50 and floors.get("medium") == 50, floors
    assert "takes 50 gold or more at once" in seat_events.SPENDING_EVENTS_ROW["not_covered"]


# ── the disinherit price of a son who would take a share ─────────────────────────────────────

def test_disinheriting_a_landless_title_heir_is_300_prestige_and_150_renown_x_the_era_factor():
    t = block(game_file("common/character_interactions/00_dynast_interactions.txt"), "disinherit_interaction")
    cost = block(t, "cost")
    pres, ren = block(cost, "prestige"), block(cost, "renown")
    for b, unit in ((pres, "medium_prestige_value"), (ren, "medium_dynasty_prestige_value")):
        heir = b[b.index("is_landed_or_landless_administrative = no"):]
        assert "any_heir_title" in heir and unit in heir and b.count(unit) == 2, b
    assert prestige("medium_prestige_value") == 150
    assert re.search(r"medium_dynasty_prestige_value = \{\s*value = 75", basic())
    era = block(basic(), "scale_dynasty_prestige_by_era")
    assert re.findall(r"add = (\d)", era) == ["3", "2", "1"]   # late, high, early: factors 4, 3, 2
    n = note("vows-or-disinherit")
    assert "a share's heir: 300 prestige, 150 renown x the era factor" in n
