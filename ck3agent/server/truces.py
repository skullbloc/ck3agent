"""The truces of the seat's ruler, as the game holds them, on every war read.

a seated game (1094-1096): plan_war read `"truce": false` for 14492 three times and the game
refused each declaration for `has_truce`. The bridge searched GetTruceList(attacker) (a list of
character pointers that is not the ruler's truces); a truce is the ruler's slot in the pair's
CDiplomaticRelation (bridge/include/truce_read.h). The bridge's query_relations now lists those
slots (`source: relation_slot`) with their end dates; this module reads that ONE list once per ruler
and game date and lays it over every casus-belli read (cbs, plan_war, discover war_targets,
inspect title's next_call, news), so all of them and declare_war's refusal say the same truce and
when it ends, against the episode's end when the env registers one (CK3_EPISODE_HORIZON_END).

What a truce means (game files, 1.18.3.1): a war ending under most casus belli puts the attacker
under a one-way truce on the defender for standard_truce_duration_days, 1825 days (00_war_values.txt;
1375 with the flexible_truces_perk), whatever the outcome (00_war_effects.txt add_truce_*_effect). While it holds
the game refuses the declaration for a culture with cannot_attack_truces (tradition_astute_diplomats,
00_war.txt); otherwise breaking it costs 250 prestige, one prestige level and legitimacy, and the
broke_truce modifier (-50 opinion, 3 years). A seated game saw both: 1092.01.15 Domnall declared three
days inside a truce and lost about 280 prestige that month; 1094-1096 the game refused three times.
"""
from __future__ import annotations

from typing import Any, Optional

import cycle_cadence as _cc

TRUCE_DAYS = 1825
#: the game's words for what a truce does, said once where a truce is shown
TRUCE_RULE = ("a war's end under most casus belli puts its attacker under a one-way truce on the defender "
              "for 1825 days (standard_truce_duration_days, 00_war_values.txt), whatever the outcome; among other "
              "modifiers: 1375 with the flexible truces perk, 730 days less between two nomadic rulers, 900 days less or more "
              "in some struggles, at least 730 days, doubled after a border raid; during it the game "
              "refuses the declaration "
              "or, for some cultures, takes 250 prestige and a prestige level (neither with the flexible truces "
              "perk) and legitimacy to break it (00_war_effects.txt); the toolkit does not send one")
BREAK_PRICE = {"prestige": -250, "prestige_level": -1, "legitimacy": "medium_legitimacy_loss x your tier",
               "opinion": "broke_truce: -50 general opinion for 3 years",
               "source": "common/scripted_effects/00_war_effects.txt (break_truce_prestige_loss)"}

_CACHE: dict = {}


def _date_of(gw) -> Optional[str]:
    g = gw.game() if hasattr(gw, "game") else None
    return (g or {}).get("date") if isinstance(g, dict) else None


def horizon() -> tuple[Optional[int], Optional[str]]:
    """(ordinal, raw) of the episode's registered end, (None, None) when the env sets none."""
    o, raw, err = _cc.registered_horizon_end()
    return (o, raw) if not err else (None, None)


def facts(until: Any, today: Any, war: Any = None) -> dict:
    """{until, days_left, ends_after_episode_end} for one truce; days and the episode's end only
    when both dates read (no horizon registered: `episode_end: "not registered"`)."""
    out: dict = {"until": until}
    u, t = _cc.ordinal(until), _cc.ordinal(today)
    if u is not None and t is not None:
        out["days_left"] = max(0, u - t)
    h, raw = horizon()
    if h is not None and u is not None:
        out["episode_end"] = raw
        out["outlasts_episode"] = u >= h
    else:
        out["episode_end"] = "not registered (CK3_EPISODE_HORIZON_END unset)"
    if war:
        import redesign as _R  # noqa: PLC0415 — the one cleaner of the game's rendered text
        out["from_war"] = _R.engine_text(str(war))
    return out


#: the bridge's culls that mean a truce (its per-CB and target-state words)
TRUCE_CULLS = ("target_in_truce", "in_truce", "holder_truced", "target_truced")


def blocked_words(cull: Any, truce: Any) -> Optional[str]:
    """Live (a live check, 1067.12.03: plan_war, war_targets, inspect title and declare_war said only
    "target_in_truce"): the truce refusal's words with its end, when the truce read; else None."""
    if str(cull or "") in TRUCE_CULLS and isinstance(truce, dict) and truce.get("until"):
        return fix_words(truce)
    return None


async def truce_map(gw, actor_id: Any) -> Optional[dict]:
    """{target_id: facts} of the ruler's own truces (he cannot attack target_id), or None when the
    read did not answer or comes from a bridge that does not read the relation slots."""
    me = int(actor_id or 0)
    if not me:
        return None
    today = _date_of(gw)
    key = (me, today)
    if key in _CACHE:
        return _CACHE[key]
    raw = await gw.read("query_relations", {"character_id": me})
    rows = raw.get("truces") if isinstance(raw, dict) else None
    if not isinstance(rows, list) or "truces_on_him" not in raw:
        return None                     # an older bridge: its truce list is not the has_truce rule
    out: dict = {}
    for r in rows:
        if not isinstance(r, dict) or r.get("source") != "relation_slot":
            continue
        tid = int(r.get("target_id") or 0)
        if tid:
            out[tid] = facts(r.get("end_date"), today, r.get("war_name"))
    if len(_CACHE) > 64:
        _CACHE.clear()
    _CACHE[key] = out
    return out


def forget() -> None:
    """Drop the cache (a war ended or was declared in this call: the next read is fresh)."""
    _CACHE.clear()


def overlay(target_state: Any, tmap: Optional[dict], target_id: Any) -> Any:
    """Lay the truce map over a casus-belli read's target_state: in_truce from the map, and the
    truce's facts under `truce`. Unchanged when the map was not read."""
    if not isinstance(target_state, dict) or tmap is None:
        return target_state
    t = tmap.get(int(target_id or 0))
    target_state["in_truce"] = t is not None
    target_state["truce_source"] = "relation_slot"
    if t is not None:
        target_state["truce"] = t
    else:
        target_state.pop("truce", None)
    return target_state


async def overlay_raw(gw, actor_id: Any, target_id: Any, raw: Any) -> Any:
    """overlay() on a query_cbs_for_target(s) row, reading the map once per ruler and date."""
    if not isinstance(raw, dict):
        return raw
    tmap = await truce_map(gw, actor_id)
    if tmap is None:
        return raw
    ts = raw.get("target_state")
    ts = dict(ts) if isinstance(ts, dict) else {}       # never the caller's own dict
    if isinstance(raw.get("target_state"), dict) or raw.get("cbs") is not None:
        raw["target_state"] = ts
    overlay(ts, tmap, target_id)
    return raw


def fix_words(truce: Any) -> str:
    """The refusal's words for a truce: until when, how many days, against the episode's end."""
    if not isinstance(truce, dict) or not truce.get("until"):
        return "a truce with this ruler: no war on him until it ends (its end did not read)"
    s = f"a truce with this ruler until {truce['until']}"
    if isinstance(truce.get("days_left"), int):
        s += f" ({truce['days_left']} days)"
    if truce.get("outlasts_episode") is True:
        s += f", past your episode's end {truce.get('episode_end')}: no war on him in this episode"
    else:
        s += ": no war on him until it ends"
    return s


#: The casus belli the peace prices price (redesign.PEACE_PRICE) whose on_victory, on_white_peace and on_defeat
#: each call a standard truce effect (00_war_effects.txt add_truce_attacker_victory_effect,
#: add_truce_white_peace_effect, add_truce_attacker_defeat_effect: add_truce_one_way, the attacker on the defender,
#: standard_truce_duration_days) and set no truce_days of their own, as common/casus_belli_types/ gives them
#: (1.18.3.1; eradicate_house_cb's white peace calls the victory one).
ONE_WAY_TRUCE_CBS = frozenset({
    "claim_cb", "de_jure_cb", "individual_county_de_jure_cb", "individual_duchy_de_jure_cb", "county_conquest_cb",
    "duchy_conquest_cb", "ducal_conquest_cb", "vassalization_cb", "make_tributary_cb", "minor_religious_war",
    "eradicate_house_cb"})
#: refused_liege_demand_war (00_civil_war.txt) calls none of them: its on_victory and on_white_peace add a truce
#: both ways (add_truce_both_ways, days = 1825), its on_defeat none. Any other casus belli's truce is not read.
BOTH_WAYS_TRUCE_CB = "refused_liege_demand_war"
BOTH_WAYS_TRUCE_ENDINGS = ("victory", "white_peace")


#: Each ending (victory|white_peace|defeat, the attacker's), by the war-history outcome the bridge names it
#: (war_outcome_name) and by its prefix in a peace-price row (redesign.PEACE_PRICE)
ENDINGS = (("victory", "attacker_victory", "victory_"), ("white_peace", "white_peace", "white_"),
           ("defeat", "defender_victory", "defeat_"))


def ending_of(word: Any) -> Optional[str]:
    """The ending a war-history outcome or a peace-price prefix names; None for any other word."""
    return next((e for e, *names in ENDINGS if word in names), None)


def peace_truce_kind(cb: Any, ending: Any) -> Optional[str]:
    """"one_way", "both_ways" or "none": the truce `ending` (victory|white_peace|defeat, the attacker's) of a war of
    casus belli `cb` makes; None when it is not read (another casus belli, or the ending not read)."""
    if ending not in ("victory", "white_peace", "defeat") or not isinstance(cb, str):
        return None
    if cb in ONE_WAY_TRUCE_CBS:
        return "one_way"
    if cb == BOTH_WAYS_TRUCE_CB:
        return "both_ways" if ending in BOTH_WAYS_TRUCE_ENDINGS else "none"
    return None


def bound_by_peace(kind: Optional[str], made: dict, me_attacker: Optional[bool]) -> str:
    """Who a peace truce of `kind` binds, said to the ruler (me_attacker None: his side not read)."""
    if kind == "both_ways":
        return "neither of you can declare on the other"
    if kind != "one_way":
        return made["on"]
    if me_attacker is None:
        return "the war's attacker on its defender"
    return "you cannot declare on him" if me_attacker else "he cannot declare on you (you may on him)"


def made_by_peace(today: Any, cb: Any, ending: Any) -> dict:
    """The truce the `ending` (victory|white_peace|defeat) of a war of casus belli `cb` makes (peace_truce_kind):
    how long, on whom, until when, against the episode's end; its days "not read" where the kind is not."""
    kind = peace_truce_kind(cb, ending)
    if kind is None:
        return {"days": "not read", "on": f"not read: the truce a {cb or 'casus belli not read'} war's "
                                          f"{ending or 'ending (not read)'} makes (this tool reads it only for the "
                                          "casus belli its peace prices list)",
                "source": "not read"}
    if kind == "none":
        return {"days": 0, "on": f"no truce: a {cb} war's {ending} adds none",
                "source": f"common/casus_belli_types/00_civil_war.txt ({cb} on_defeat)"}
    t = _cc.ordinal(today)
    out: dict = {"days": TRUCE_DAYS, "on": "the war's attacker against its defender (one way)",
                 "source": "common/script_values/00_war_values.txt standard_truce_duration_days "
                           "(among other modifiers: 1375 with the flexible_truces_perk)"}
    if kind == "both_ways":
        out["on"] = "the war's attacker and its defender, each on the other (both ways)"
        out["source"] = f"common/casus_belli_types/00_civil_war.txt ({cb}: add_truce_both_ways, days = 1825)"
    if t is not None:
        out["until"] = _cc.date_from_ordinal(t + TRUCE_DAYS)
        h, raw = horizon()
        if h is not None:
            out["episode_end"] = raw
            out["outlasts_episode"] = t + TRUCE_DAYS >= h
    return out
