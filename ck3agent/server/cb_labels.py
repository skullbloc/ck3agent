"""What a casus belli takes and grants, from the game's own files.

The cbs rows' `scope` and `grants` come from a static table in the bridge (query_handlers.cpp
cb_semantics_for). The map study found ducal_conquest_cb labelled "scope duchy, grants the targeted
de-jure duchy": the game's file says it targets a COUNTY next to your land, in an uncreated de jure
duchy where you hold a county, and only for a ruler with the Ducal Conquest perk; and its invalid row
gave no reason. Each row below was checked against CK3 1.18.3.1's common/casus_belli_types/ (the file
and lines named in `source`); `requires` names what the file's allowed_for_character and
valid_to_start ask. This table overrides the bridge's text for these casus belli (the bridge's table
carries the same words; test_map_digest_and_turn_one_notes checks the two agree), and `missing_now` says which requirement a
read shows unmet.

Not relabelled: claim_cb keeps scope "county" (declare_gates prices a claim by that scope when the
claimed title's key is not read; a claim on a duchy is priced by its key).
"""
from __future__ import annotations

from typing import Any, Optional

DUCAL_CONQUEST_PERK = "ducal_conquest_perk"

#: cb -> (scope, grants, requires, source)
LABELS: dict = {
    "ducal_conquest_cb": (
        "county",
        "one county next to your land in an uncreated de jure duchy where you hold a county",
        ["the Ducal Conquest perk (ducal_conquest_perk: diplomacy, Foreign Affairs, after Thoughtful)",
         "a county of yours in the target's de jure duchy, that duchy not created",
         "the target county borders your land (neighbor_land)",
         "50 prestige as a count, 100 above, times the common multipliers"],
        "00_conquest.txt ducal_conquest_cb (allowed_for_character, target_titles, valid_to_start, cost)"),
    "county_conquest_cb": (
        "county",
        "one de jure county (the targeted county)",
        ["conquest casus belli by government or faith (can_use_conquest_cbs_trigger: tribal, clan, "
         "nomadic or a faith's parameter)"],
        "00_conquest.txt county_conquest_cb (allowed_for_character, target_title_tier county)"),
    "duchy_conquest_cb": (
        "duchy",
        "the whole targeted de jure duchy (every county held by the defender or his vassals)",
        ["conquest casus belli by government or faith (can_use_conquest_cbs_trigger)"],
        "00_conquest.txt duchy_conquest_cb (allowed_for_character, target_title_tier duchy)"),
    "de_jure_cb": (
        "titles",
        "every title of the defender's whose de jure liege title, or one above it, you or your liege hold",
        ["the culture innovation innovation_divine_right"],
        "00_dejure_war.txt de_jure_cb (allowed_for_character, target_titles de_jure, valid_to_start)"),
    "individual_duchy_de_jure_cb": (
        "duchy",
        "one de jure duchy under a title you or your liege hold",
        ["the culture innovation innovation_casus_belli"],
        "00_dejure_war.txt individual_duchy_de_jure_cb (allowed_for_character, valid_to_start tier_duchy)"),
    "individual_county_de_jure_cb": (
        "county",
        "one county under a de jure title you or your liege hold",
        ["the culture innovation innovation_casus_belli", "100 prestige times the common multipliers"],
        "00_dejure_war.txt individual_county_de_jure_cb (allowed_for_character, valid_to_start, cost)"),
    # A holy war for one county (the bridge said "faith_counties"; the file's target is a county)
    "minor_religious_war": (
        "county",
        "one county next to your land or across water, held by the defender or his vassals, with what lies under it",
        ["your faith hostile or worse to his (faith_hostility_level), holy wars not forbidden, not unreformed",
         "he is not your liege nor under your top liege",
         "piety level 1 (0 with cb_lower_piety_level_requirement); a king or above: not piety level -1",
         "100 piety times the common and holy-war multipliers"],
        "00_religious_war.txt minor_religious_war (allowed_for_character(_display_regardless), "
        "allowed_against_character, target_titles, valid_to_start, cost)"),
}


def relabel(opt: dict) -> dict:
    """The shaped cbs option with the game file's scope, grants and requires for a casus belli the table
    names (the bridge's own words kept under bridge_label when they differ); others unchanged."""
    lab = LABELS.get(str(opt.get("cb") or ""))
    if lab is None:
        return opt
    scope, grants, requires, source = lab
    if (opt.get("scope"), opt.get("grants")) != (scope, grants):
        opt["bridge_label"] = {"scope": opt.get("scope"), "grants": opt.get("grants")}
    opt["scope"], opt["grants"] = scope, grants
    opt["requires"] = list(requires)
    opt["label_source"] = source
    return opt


def missing_now(opt: dict, perks: Optional[list]) -> Optional[list]:
    """For an invalid ducal_conquest_cb row: the requirements a read shows unmet (the perk from
    query_lifestyle's perks; the duchy and the border are not read here: inspect(subject='map') shows
    them). None for another casus belli or a valid row."""
    if opt.get("cb") != "ducal_conquest_cb" or opt.get("valid"):
        return None
    out = []
    if perks is None:
        out.append("the perk: unread (inspect lifestyle did not answer)")
    elif DUCAL_CONQUEST_PERK not in perks:
        out.append(f"the perk: {DUCAL_CONQUEST_PERK} is not among your ruler's perks (inspect lifestyle)")
    out.append("the duchy and the border: not read here; inspect(subject='map') shows your counties, their "
               "duchies (layer duchy) and neighbours")
    return out


async def ducal_conquest_row(gw, me: int, all_opts: list) -> Optional[dict]:
    """cbs' `ducal_conquest` row when that casus belli is listed and not declarable: what it grants, what it
    requires and what a read shows missing. None otherwise."""
    row = next((o for o in all_opts or [] if o.get("cb") == "ducal_conquest_cb"), None)
    if row is None or row.get("valid"):
        return None
    lf = await gw.read("query_lifestyle", {"character_id": me})
    perks = lf.get("perks") if isinstance(lf, dict) and isinstance(lf.get("perks"), list) else None
    return {"valid": False, "grants": row.get("grants"), "requires": row.get("requires"),
            "missing_now": missing_now(row, perks),
            "game_says": row.get("requirements_unmet") or []}


#: Item f (a seated game, step 6, 1068.06: vassalization_cb read engine_cb_validity_check_failed against all 11
#: targets for the new ruler Brian, "the tool doesn't say why"): casus belli whose allowed_for_character asks
#: for a perk the ruler holds or not (query_lifestyle reads it). cb -> (perk, the alternatives and other
#: conditions the reads here do not evaluate, the file). Built, not proven live.
PERK_GATED: dict = {
    # cb -> (perk, the alternatives and target conditions not read; source 00_vassalization.txt allowed_for_character,
    # allowed_against_character: a struggle that unlocks it, the conqueror variable, the defender independent,
    # bordering your realm, at most vassalization_size_limit counties)
    "vassalization_cb": ("forced_vassalage_perk", "struggle, conqueror, his size, border"),
}
#: kept short: the cbs reply is bounded (test_declare_war_cost_gates)
ENGINE_FAILED_UNREAD = "no condition named; allowed_for_character, valid_to_start not evaluated"


async def engine_failed_rows(gw, me: int, all_opts: list) -> Optional[dict]:
    """cbs' `engine_failed_why`: for each casus belli the game's check refused that PERK_GATED names, the perk as
    read (held true/false, or unread) and what else is not read; `others`: the rest name no condition. None when
    no option failed the game's own check."""
    failed = sorted({str(o.get("cb")) for o in all_opts or [] if not o.get("valid")
                     and "engine_cb_validity_check_failed" in (o.get("requirements_unmet") or [])})
    if not failed:
        return None
    out: dict = {"others": ENGINE_FAILED_UNREAD}
    gated = [c for c in failed if c in PERK_GATED]
    if gated:
        lf = await gw.read("query_lifestyle", {"character_id": me})
        perks = lf.get("perks") if isinstance(lf, dict) and isinstance(lf.get("perks"), list) else None
        for c in gated:
            perk, rest = PERK_GATED[c]
            out[c] = {perk: (perk in perks) if perks is not None else "unread", "else_unread": rest}
    return out
