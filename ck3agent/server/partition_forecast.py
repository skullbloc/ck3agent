"""Who takes land when a ruler under a partition law dies, by the game's own rule, and which
neighbour may vassalize the realm. Pure functions over reads the server already makes; nothing
here talks to the bridge. NOT RUN LIVE: no live game has yet produced the rows that would prove it.

Why this exists (re/findings/re_succession_survival.md, finding P1). The bridge's succession
simulation (do_query_succession_simulation, query_handlers.cpp) splits the realm among the first
entries of the PRIMARY TITLE's line of succession. That line runs depth-first through the whole
family: grandchildren, then brothers, cousins. The game divides partition land among the late
ruler's ELIGIBLE CHILDREN only ("all titles held by the late ruler will be divided amongst their
eligible children, with the player heir always being given the primary title",
localization/english/succession_laws_l_english.yml:7, partition_base_desc). Recorded misses:
one game read 1071.06-1082.01 named the granddaughter Duinsech 15819 as the breaker for eleven years;
at Murchad's death his adult son Donnchad 37528 took Athlone and Leinster and left the realm.
A test run named Murchad's BROTHER Enna 32805 as the breaker (a brother is never a
partition heir of a ruler who has children).

The rule as this module states it (grades in the findings):
  * the primary (player) heir is the game's own: the primary title's line, position 1;
  * every other living child of the ruler who is eligible takes a share:
      - not blocked by a trait with inheritance_blocker (common/traits/00_traits.txt:
        devoted 8304, order_member 9907, bastard 10088, bastard_founder 10276,
        disinherited 11094, gallivanter 11196, eunuch_1 6322, beardless_eunuch 6394);
      - allowed by the gender law (male_preference: daughters only when no eligible son);
  * a share of a count's land leaves the realm: a count cannot hold a count as vassal
    (redesign.GRANT_LEAVES_REALM; recorded in 1079.09 and in 1082.01);
  * a duke's or higher ruler's younger heirs get titles of the primary title's tier first
    (partition_base_desc), and confederate partition CREATES free duchies of that tier
    (create_primary_tier_titles, common/laws/00_succession_laws.txt:68 and the doc
    common/laws/_laws.info:271-276): a younger son given a created duchy ranks with his
    brother and leaves; one given only counties stays as a vassal.

What it does not decide (said in the reply, never guessed): which titles each heir gets
beyond the primary title, and whether a DEAD younger child's line takes a share.
"""
from __future__ import annotations

from typing import Any, Iterable, Optional

PARTITION_LAWS = frozenset({"confederate_partition_succession_law", "partition_succession_law",
                            "high_partition_succession_law"})
CONFEDERATE = "confederate_partition_succession_law"

#: Traits whose inheritance_blocker keeps the holder out of every line (common/traits/00_traits.txt).
INHERITANCE_BLOCKING_TRAITS = frozenset({"devoted", "order_member", "bastard", "bastard_founder",
                                         "disinherited", "gallivanter", "eunuch_1", "beardless_eunuch"})

GENDER_LAWS = frozenset({"male_only_law", "male_preference_law", "equal_law",
                         "female_preference_law", "female_only_law"})

TIER_COUNT, TIER_DUCHY = 2, 3

RULE_SOURCE = ("partition among the late ruler's eligible children (succession_laws_l_english.yml:7 "
               "partition_base_desc); inheritance_blocker traits (00_traits.txt); a count cannot hold a "
               "count as vassal; confederate partition creates free titles of the primary tier "
               "(00_succession_laws.txt:68)")


#: A younger son whose share is below the primary title's tier (a county under a duke)
STAYS_BELOW_TIER = ("no title of your tier is left for him (none held beyond the primary one, and the law creates "
                    "only a duchy whose counties your realm holds more than half of): his share is below your tier "
                    "and he stays in the realm as your heir's vassal")


class ForecastInputError(ValueError):
    """A read this forecast needs is missing or malformed: refused, never defaulted."""


def _trait_names(traits: Any) -> set:
    out = set()
    for t in traits or []:
        if isinstance(t, dict):
            name = t.get("name")
        else:
            name = t
        if isinstance(name, str) and name:
            out.add(name)
    return out


def _gender_filter(children: list, gender_law: str) -> list:
    sons = [c for c in children if c.get("is_female") is False]
    daughters = [c for c in children if c.get("is_female") is True]
    if gender_law == "male_only_law":
        return sons
    if gender_law == "female_only_law":
        return daughters
    if gender_law == "male_preference_law":
        return sons or daughters
    if gender_law == "female_preference_law":
        return daughters or sons
    return sons + daughters          # equal_law


def eligible_children(children: Iterable[dict], gender_law: str) -> dict[str, Any]:
    """The ruler's children who can take a share: {eligible: [...], excluded: [{id, why}]}.

    Each child row is query_character's shape: id, alive, is_female, traits. A row without
    `alive` or `is_female` is refused (ForecastInputError): an unread sex or life is not a son."""
    if gender_law not in GENDER_LAWS:
        raise ForecastInputError(f"gender law {gender_law!r} is not one of {sorted(GENDER_LAWS)}")
    living, excluded = [], []
    for c in children:
        if not isinstance(c, dict) or not isinstance(c.get("id"), int):
            raise ForecastInputError(f"child row without an integer id: {c!r}")
        if not isinstance(c.get("alive"), bool) or not isinstance(c.get("is_female"), bool):
            raise ForecastInputError(f"child {c['id']}: alive and is_female must both be read")
        if not c["alive"]:
            excluded.append({"id": c["id"], "why": "dead"})
            continue
        bars = sorted(_trait_names(c.get("traits")) & INHERITANCE_BLOCKING_TRAITS)
        if bars:
            excluded.append({"id": c["id"], "why": f"trait {', '.join(bars)} blocks inheritance"})
            continue
        living.append(c)
    kept = _gender_filter(living, gender_law)
    kept_ids = {c["id"] for c in kept}
    for c in living:
        if c["id"] not in kept_ids:
            excluded.append({"id": c["id"], "why": f"{gender_law}: a {'daughter' if c['is_female'] else 'son'} "
                                                   "inherits only when no eligible child of the other sex lives"})
    return {"eligible": kept, "excluded": excluded}


def forecast(*, ruler_tier: int, succession_law: str, gender_law: str, primary_heir_id: Optional[int],
             children: list, sim_heir_ids: Iterable[int] = (), held_counties: Optional[int] = None,
             tier_shares: Optional[list] = None) -> dict[str, Any]:
    """Who takes land at this ruler's death and whether it leaves the realm.

    ruler_tier: query_character.ruler_tier; succession_law / gender_law: the realm's laws
    (query_realm_laws); primary_heir_id: the primary title's line position 1 (the game's own);
    children: query_character rows for each id in the ruler's `children`; sim_heir_ids: the ids
    the bridge simulation names as heirs, primary first (to flag the ones the game cannot use)."""
    if not isinstance(succession_law, str) or not succession_law:
        raise ForecastInputError("the realm's succession law was not read")
    if succession_law not in PARTITION_LAWS:
        return {"law": succession_law, "partition": False, "takers": [primary_heir_id] if primary_heir_id else [],
                "note": "not a partition law: the primary heir takes the realm's partitionable titles"}
    if not isinstance(ruler_tier, int) or ruler_tier < TIER_COUNT:
        raise ForecastInputError(f"ruler_tier {ruler_tier!r}: a landed ruler's tier (2 count .. 5 empire) is needed")
    if not isinstance(primary_heir_id, int) or primary_heir_id <= 0:
        return {"law": succession_law, "partition": True, "takers": [], "undetermined":
                "no primary heir read (the primary title's line is empty)"}
    el = eligible_children(children, gender_law)
    child_ids = [c["id"] for c in children]
    younger = [c for c in el["eligible"] if c["id"] != primary_heir_id]
    no_land = []
    # The younger children share the counties beyond the primary heir's (a count's own county; a
    # duke's or king's capital county, which goes with the primary title); with fewer counties than heirs the
    # later ones take nothing. Live: Murchad held Dublin alone in 1066, three sons: intact (every run); a duke
    # holding 2 counties with 6 sons (a live check, 1077.11): one younger son took one county, four took none
    if not isinstance(held_counties, int) or held_counties < 1:
        return {"law": succession_law, "partition": True, "takers": "unknown",
                "refused": "the counties you hold were not read: how many shares exist is unknown"}
    # (1089.02 and a live read of the 1089 save: a duke of 9 counties holding d_munster, d_ulster and 2 counties
    # of his own, two younger sons; the forecast gave one share by the counties held and excluded the other; the
    # game gave d_ulster to one and CREATED d_leinster for the other, 9 counties -> 5): under confederate partition a
    # duke's younger sons take the titles of his tier beyond the primary one, held or created by the law
    # (create_primary_tier_titles: every duchy he can create), one each, before any county
    room = max(0, held_counties - 1)
    if tier_shares is not None and succession_law == CONFEDERATE and ruler_tier >= TIER_DUCHY:
        room = max(room, len(tier_shares))
    no_land = younger[room:]
    younger = younger[:room]
    el["excluded"] += [{"id": c["id"], "why": ("no share left for him: younger children share the titles of your "
                                               "tier beyond the primary one (held or created by the law), then the "
                                               "counties beyond the primary heir's")
                        if tier_shares else "no county left for him: younger children share the counties "
                                            "beyond the primary heir's"} for c in no_land]
    primary_is_child = primary_heir_id in child_ids
    takers = [primary_heir_id] + [c["id"] for c in younger]
    shares = []
    for c in younger:
        if ruler_tier == TIER_COUNT:
            fate, why = "leaves_realm", ("a county is the primary title's tier and a count cannot hold a count "
                                         "as vassal: his share leaves with him as an independent realm")
        elif succession_law != CONFEDERATE:
            fate, why = "stays_vassal", ("a county or other title below your tier given to him stays in the "
                                         "realm as his vassal holding (a duke's younger son given a county stays "
                                         "as his brother's vassal)")
        elif succession_law == CONFEDERATE:
            fate, why = "may_leave_realm", ("confederate partition creates free titles of your tier for younger "
                                            "heirs when enough land is held: given one he ranks with your heir "
                                            "and leaves; given only lower titles he stays as a vassal")
        else:
            fate, why = "may_leave_realm", ("a title of your tier given to him leaves with him; lower titles stay "
                                            "as his vassal holdings")
        row = {"id": c["id"], "fate": fate, "why": why}
        k = len(shares)
        if tier_shares and k < len(tier_shares) and succession_law == CONFEDERATE and ruler_tier >= TIER_DUCHY:
            t = tier_shares[k]
            row.update(fate="leaves_realm", takes_title=t,
                       why=("a title of your tier is his share: he ranks with your heir and leaves the realm with "
                            "its counties" + (" (the law creates it at your death)" if t.get("created") else "")))
        elif tier_shares is not None and succession_law == CONFEDERATE and ruler_tier >= TIER_DUCHY:
            # (1088.06.03: forecast "leaves_realm" with d_leinster; the law could not create it, 1 of
            # its 2 counties held, and Donnchad took c_ennis as his brother's vassal): every title of your tier the law
            # gives is listed in tier_shares; a younger son past them takes titles below your tier and stays
            row.update(fate="stays_vassal", why=STAYS_BELOW_TIER)
        shares.append(row)
    sim = [int(h) for h in sim_heir_ids if isinstance(h, int)]
    not_takers = [h for h in sim if h not in takers]
    out: dict[str, Any] = {
        "law": succession_law, "partition": True,
        "primary_heir_id": primary_heir_id,
        "primary_heir_is_child": primary_is_child,
        "takers": takers,
        "younger_shares": shares,
        "intact": not younger,
        "excluded_children": el["excluded"],
        "rule": RULE_SOURCE,
        "not_decided": ["which titles each younger heir receives beyond the primary title",
                        "whether a dead younger child's own line takes a share"],
    }
    if not primary_is_child and not younger and not no_land:
        # No eligible child of the ruler takes a share and the line's head is not his child
        # (a sibling, a grandchild of a dead child, a cousin): whether the game then shares the
        # land among others of the line is not read anywhere (re_succession_survival.md section 6)
        out["intact"] = None
        out["undecided"] = ("you have no living eligible child; your primary heir is not your child, "
                            "and whether the game then shares partition land among others of his line "
                            "(siblings, cousins) is not read: unknown, not intact")
    elif not primary_is_child:
        out["primary_heir_note"] = ("your primary heir is not your child (a dead child's line): every "
                                    "living eligible child of yours takes a share besides him")
    if not_takers:
        out["simulation_disagrees"] = {
            "named_but_not_partition_heirs": not_takers,
            "why": ("the bridge simulation splits among the primary title's line (grandchildren, "
                    "siblings, cousins); the game splits among your eligible children"),
        }
    dead = [c["id"] for c in children if c.get("alive") is False]
    if dead:
        # the primary heir's own parent is one of them when he is a grandchild; whether the
        # other dead children's lines take a share is not decided here
        out["dead_children_lines_unknown"] = dead
    return out


# --------------------------------------------------------------------------------------------
# Vassalization exposure (common/casus_belli_types/00_vassalization.txt)
# --------------------------------------------------------------------------------------------

#: script_values/00_war_values.txt:680-720: vassalize_default_size 4, +3 per innovation below.
VASSALIZE_DEFAULT_SIZE = 4
VASSALIZE_GROWTH_PER_INNOVATION = 3
VASSALIZE_SIZE_INNOVATIONS = ("innovation_casus_belli", "innovation_chronicle_writing",
                              "innovation_divine_right", "innovation_rightful_ownership")


def vassalization_size_limit(attacker_innovations: Iterable[str]) -> int:
    """The most counties a realm may hold and still be the target of this attacker's
    vassalization_cb (the CB is refused when the defender's sub-realm has MORE than this)."""
    have = set(attacker_innovations)
    return VASSALIZE_DEFAULT_SIZE + VASSALIZE_GROWTH_PER_INNOVATION * sum(
        1 for i in VASSALIZE_SIZE_INNOVATIONS if i in have)


def vassalization_exposure(*, my_tier: int, my_sub_realm_counties: int, attacker_tier: int,
                           attacker_innovations: Iterable[str], attacker_unlocked: Optional[bool]) -> dict:
    """Whether one attacker's vassalization_cb can be declared on this independent realm.

    attacker_unlocked: the CB's allowed_for_character (forced_vassalage_perk, a struggle phase, or
    the conqueror variable): True / False when read, None when not read (then the verdict says so).
    The AI pre-filters this CB to neighbours (ai_only_against_neighbors); that is not checked here."""
    for k, v in (("my_tier", my_tier), ("my_sub_realm_counties", my_sub_realm_counties),
                 ("attacker_tier", attacker_tier)):
        if not isinstance(v, int) or v < 0:
            raise ForecastInputError(f"{k} must be a read non-negative integer, got {v!r}")
    limit = vassalization_size_limit(attacker_innovations)
    gates = []
    if attacker_tier <= my_tier:
        gates.append(f"attacker tier {attacker_tier} is not above yours ({my_tier})")
    if my_sub_realm_counties > limit:
        gates.append(f"your realm holds {my_sub_realm_counties} counties, above his limit {limit}")
    if attacker_unlocked is False:
        gates.append("he has neither forced_vassalage_perk, a struggle phase, nor the conqueror variable")
    if gates:
        verdict = "closed"
    elif attacker_unlocked is None:
        verdict = "open_if_unlocked"
    else:
        verdict = "open"
    return {"verdict": verdict, "limit": limit, "closed_by": gates,
            "counties_to_close": (None if my_sub_realm_counties > limit else limit + 1 - my_sub_realm_counties),
            "source": "casus_belli_types/00_vassalization.txt:10-92, script_values/00_war_values.txt:680-720"}


# --------------------------------------------------------------------------------------------
# Wiring: the forecast from the server's reads, and the verdict it corrects
# --------------------------------------------------------------------------------------------

def child_row(sect: Any, cid: int) -> dict:
    """One query_character reply as the forecast's child row (id, alive, is_female, traits, and
    what the levers read: name, age, landed, faith_id, dynasty_id, liege_id). A reply that did not
    answer keeps alive/is_female absent, so forecast() refuses it rather than guess."""
    s = sect if isinstance(sect, dict) else {}
    row: dict = {"id": cid}
    for k in ("alive", "is_female"):
        if isinstance(s.get(k), bool):
            row[k] = s[k]
    row["traits"] = s.get("traits") if isinstance(s.get("traits"), list) else []
    for k in ("name", "age", "faith_id", "dynasty_id", "liege_id", "ruler_tier", "has_realm"):
        if k in s:
            row[k] = s[k]
    return row


def laws_of(names: Iterable[str]) -> tuple[str, str]:
    """(succession law, gender law) among the realm's active law names; '' for one not read."""
    names = [str(n) for n in names or []]
    succ = next((n for n in names if n.endswith("_succession_law")), "")
    gender = next((n for n in names if n in GENDER_LAWS), "")
    return succ, gender


def sim_distribution(sim: Any) -> dict:
    """query_succession_simulation's per-title recipients {title_id: heir_id}, and whether the
    bridge already shares partitions among the ruler's children (it says partition_heir_basis;
    an older bridge does not, and its recipients come from the whole line)."""
    sim = sim if isinstance(sim, dict) else {}
    out: dict = {}
    for h in sim.get("heirs") or []:
        if not isinstance(h, dict):
            continue
        hid = h.get("character_id")
        for t in h.get("titles") or []:
            if isinstance(t, dict) and isinstance(t.get("title_id"), int):
                out[t["title_id"]] = hid
    return {"by_title": out, "basis": sim.get("partition_heir_basis")}


def held_counties_of(sim: Any, titles: Any = None) -> Optional[int]:
    """The counties the ruler holds: the simulation's titles of tier 2 (it lists every held title of
    tier 2 or more among its heirs), else the per-title map's rows; None when neither was read."""
    ids = set()
    for h in ((sim or {}).get("heirs") or []) if isinstance(sim, dict) else []:
        for t in (h.get("titles") or []) if isinstance(h, dict) else []:
            if isinstance(t, dict) and t.get("tier") == TIER_COUNT and isinstance(t.get("title_id"), int):
                ids.add(t["title_id"])
    if not ids and isinstance(titles, list):
        ids = {t.get("title_id") for t in titles if isinstance(t, dict) and t.get("tier") == TIER_COUNT}
    return len(ids) or None


#: (a test run: from 1086.12 to 1088.01 every read gave Donnchad d_ulster, c_ulster and c_ossory; at the
#: death, 1089.06, the game gave him d_ulster and c_ulster and kept c_ossory, de jure in neither duchy, with the
#: primary heir): a son's `takes` is the per-title reads of today, not the game's split at the death
TAKES_LIMIT = ("takes: the per-title heir reads of today, before the death; the game splits the land of that day "
               "and has kept with your primary heir a county named here outside the de jure land of the son's duchy")


def forecast_from_reads(*, ruler: dict, law_names: Iterable[str], children: list, sim: Any,
        primary_heir_id: Optional[int], held_counties: Optional[int] = None, map_titles: Any = None,
        tier_shares: Optional[list] = None) -> dict:
    """forecast() from the server's reads, or {refused: why} naming the unread fact."""
    succ, gender = laws_of(law_names)
    if held_counties is None:
        held_counties = held_counties_of(sim)
    sim_ids = [h.get("character_id") for h in ((sim or {}).get("heirs") or []) if isinstance(h, dict)]
    try:
        if not succ:
            raise ForecastInputError("the realm's succession law was not read (query_realm_laws)")
        if "partition" in succ and succ not in PARTITION_LAWS:
            # a live read: clan_impassive_partition_succession_law (a clan realm) read as "not a partition"
            raise ForecastInputError(f"{succ} is a partition law this forecast does not model")
        if succ in PARTITION_LAWS and not gender:
            raise ForecastInputError("the realm's gender law was not read (query_realm_laws)")
        kw = dict(ruler_tier=ruler.get("ruler_tier"), succession_law=succ, gender_law=gender or "equal_law",
                  primary_heir_id=primary_heir_id, children=children, held_counties=held_counties,
                  tier_shares=tier_shares)
        fc = forecast(**kw, sim_heir_ids=sim_ids)
    except ForecastInputError as e:
        return {"refused": str(e), "takers": "unknown"}
    fc["_inputs"] = kw          # for lever_rows' forecast-after; callers pop it before replying
    names = {c["id"]: c.get("name") for c in children if isinstance(c, dict)}
    for s in fc.get("younger_shares") or []:
        if names.get(s["id"]):
            s["name"] = names[s["id"]]
    dist = sim_distribution(sim)
    if fc.get("partition") and fc.get("younger_shares"):
        takers = set(fc["takers"]) if isinstance(fc.get("takers"), list) else set()
        # the engine's own per-title heir (query_title_succession_map) where it names a younger taker. In a live game
        # the duke's county c_naddula went to the son the map named, which the simulation kept
        # with the primary heir (it pre-assigns the primary title's de jure counties)
        by_map = {t.get("title_id"): t.get("heir_id") for t in (map_titles or []) if isinstance(t, dict)}
        younger_ids = {sh["id"] for sh in fc["younger_shares"]}
        map_named = {t: h for t, h in by_map.items() if h in younger_ids}
        if (dist["basis"] and set(dist["by_title"].values()) <= takers) or map_named:
            # (1071.01.12: both younger sons listed taking [469, 508]: the simulation
            # gave both to Énna, the engine's map both to Donnchad, and each share took the union): one
            # title, one taker. The engine's per-title heir decides where it names a younger child; where
            # the two sources name different younger children the title says so.
            owner: dict = (dict(dist["by_title"]) if dist["basis"] and set(dist["by_title"].values()) <= takers
                           else {})
            disagree = sorted(t for t, h in map_named.items() if owner.get(t) not in (None, h)
                              and owner.get(t) in younger_ids)
            owner.update(map_named)
            for sh in fc["younger_shares"]:
                sh["takes"] = sorted(t for t, h in owner.items() if h == sh["id"])
            reconcile_tier_titles(fc, tier_shares, owner)
            for sh in fc["younger_shares"]:
                if not sh["takes"] and sh.get("takes_title"):
                    # (1081: `takes: []` read as "takes nothing" for 8 years; the law created a
                    # duchy for him): the title of his tier the forecast gives him
                    sh["takes"] = [sh["takes_title"]["title"]]
            if disagree:
                fc["titles_disagree"] = {"titles": disagree, "kept": "the engine's per-title heir "
                                         "(query_title_succession_map) over the simulation's"}
            fc["titles_by"] = ("the bridge's simulation shared among these heirs (its de jure scoring), and the "
                               "engine's per-title heir where it names a younger child")
            if any(len(sh.get("takes") or []) > 1 for sh in fc["younger_shares"]):
                fc["takes_limit"] = TAKES_LIMIT
        else:
            for sh in fc["younger_shares"]:
                if sh.get("takes_title"):
                    sh["takes"] = [sh["takes_title"]["title"]]   # The title of his tier he takes
            fc["titles_by"] = None
            fc["titles_unread"] = ("which titles each younger heir takes is not read: the bridge's simulation "
                                   "shares among another set of heirs" if dist["by_title"] else
                                   "which titles each younger heir takes is not read")
    return fc


def _tier_id(t: Any) -> Any:
    return t.get("title_id") if isinstance(t, dict) else None


def reconcile_tier_titles(fc: dict, tier_shares: Optional[list], owner: dict) -> dict:
    """A recorded case (two Carinthia test runs, 1066.09.15: Berthold jr `takes_title d_verona` beside `takes [982]` (d_krain),
    Gebhard `takes_title d_krain` beside d_verona's counties): forecast() hands the tier titles out in list order,
    the per-title map names who takes which. One answer: a son's `takes_title` is the tier title in his `takes`;
    a son whose `takes` holds none gets, in order, a tier title no one is named for; a son left with none takes
    titles below your tier and stays (STAYS_BELOW_TIER). `owner` is {title_id: heir_id} as read."""
    shares = [t for t in (tier_shares or []) if isinstance(t, dict)]
    if not shares:
        return fc
    by_id = {_tier_id(t): t for t in shares if _tier_id(t) is not None}
    named = {t for t in by_id if t in owner}
    free = [t for t in shares if _tier_id(t) not in named]
    for sh in fc.get("younger_shares") or []:
        mine = [by_id[t] for t in sh.get("takes") or [] if t in by_id]
        if mine:
            t = mine[0]
        elif sh.get("takes_title") is not None or sh.get("fate") == "leaves_realm":
            t = free.pop(0) if free else None
        else:
            continue
        if t is not None:
            sh["takes_title"] = t
            sh["fate"] = "leaves_realm"
            sh["why"] = ("a title of your tier is his share: he ranks with your heir and leaves the realm with its "
                         "counties" + (" (the law creates it at your death)" if t.get("created") else ""))
        elif sh.get("takes_title") is not None:
            sh.pop("takes_title", None)
            sh.update(fate="stays_vassal", why=STAYS_BELOW_TIER)
    return fc


def takes_title_consistent(fc: Any) -> list:
    """The contradictions between each younger share's `takes_title` and the `takes` lists (empty when none): a
    HELD takes_title missing from his own non-empty takes (a held title is on the per-title map, so his takes
    would name it), and a takes list naming another son's takes_title. A created title is not on the map: only
    the second applies to it. Used by the test over every recorded forecast."""
    bad = []
    rows = [s for s in (fc or {}).get("younger_shares") or [] if isinstance(s, dict)] if isinstance(fc, dict) else []
    tt = {s.get("id"): _tier_id(s.get("takes_title")) for s in rows if isinstance(s.get("takes_title"), dict)}
    for s in rows:
        takes = s.get("takes") if isinstance(s.get("takes"), list) else None
        mine = tt.get(s.get("id"))
        if takes is None:
            continue
        t = s.get("takes_title") if isinstance(s.get("takes_title"), dict) else {}
        if (mine is not None and t.get("held") is True and takes and mine not in takes
                and t.get("title") not in takes):
            bad.append({"id": s.get("id"), "takes_title": mine, "takes": takes})
        for other, t in tt.items():
            if other != s.get("id") and t is not None and t in takes:
                bad.append({"id": s.get("id"), "takes_other_sons_title": t, "of": other})
    return bad


def apply(sv: dict, fc: dict) -> dict:
    """The succession verdict (succession_verdict.verdict) with the forecast's partition heirs in
    place of the simulation's line (a copy). Not a partition, or a refused forecast: unchanged.
    The simulation's own breaker stays under `breaker_simulation` when it names someone else."""
    if not isinstance(fc, dict) or fc.get("refused") or not fc.get("partition"):
        return sv
    out = dict(sv)
    old = sv.get("breaker")
    if fc.get("undecided"):
        out.update(outcome="undetermined", passes_intact=None, breaker=None,
                   undetermined_reason=fc["undecided"], eligible_heir_count=1)
    elif fc.get("younger_shares"):
        s = fc["younger_shares"][0]
        out.update(outcome="splits", passes_intact=False, eligible_heir_count=len(fc["takers"]),
                   breaker={"id": s["id"], "name": s.get("name"), "fate": s["fate"],
                            "takes": s.get("takes") if s.get("takes") is not None else "unread",
                            "source": "partition_forecast"})
        out.pop("undetermined_reason", None)
    else:
        if (old or {}).get("source") == "per_title_map":
            return sv          # a crown title the engine's own cache gives away: kept as read
        out.update(outcome="intact", passes_intact=True, breaker=None, eligible_heir_count=1)
        out.pop("undetermined_reason", None)
    if old and (out.get("breaker") or {}).get("id") != old.get("id"):
        out["breaker_simulation"] = {"id": old.get("id"), "name": old.get("name"),
                                     "why_not": "named from the title's line, which holds grandchildren, "
                                                "siblings and cousins who take no partition share"}
    return out


def title_fates(titles: Any, fc: dict) -> Any:
    """per_title_map's rows with splits_away from the forecast: True on a title a younger heir takes
    and leaves the realm with, False on the primary heir's, None (with `fate`) where the forecast
    cannot say (a duke's younger share may stay as a vassal; titles not assigned)."""
    if not isinstance(titles, list) or not isinstance(fc, dict) or not fc.get("partition") or fc.get("refused"):
        return titles
    owner: dict = {}
    for s in fc.get("younger_shares") or []:
        for t in s.get("takes") or []:
            owner[t] = s
    out = []
    for t in titles:
        if not isinstance(t, dict):
            out.append(t)
            continue
        r = dict(t)
        s = owner.get(t.get("title_id"))
        if fc.get("undecided"):
            r["splits_away"], r["fate"] = None, "unknown"
        elif s is not None:
            r["heir_id"], r["heir_name"] = s["id"], s.get("name")
            r["splits_away"] = (True if s["fate"] == "leaves_realm" else False if s["fate"] == "stays_vassal"
                                else None)
            r["fate"] = s["fate"]
        elif fc.get("younger_shares") and fc.get("titles_by") is None:
            r["splits_away"], r["fate"] = None, "unassigned"
        else:
            r["splits_away"] = False
        out.append(r)
    return out


# --------------------------------------------------------------------------------------------
# The levers that take a son out of the split
# --------------------------------------------------------------------------------------------

#: ask_to_take_vows_piety_cost (common/script_values/02_religion_values.txt:4323-4343):
#: medium_piety_value 100 + major_piety_value 250 for your child or one of your title's 3 first heirs
VOWS_PIETY_CHILD = 350
VOWS_SOURCE = ("take_vows_interaction (00_religious_interactions.txt:3882): your courtier, not your court "
               "chaplain, 10 or older, not married or betrothed matri/patrilineally; piety "
               "ask_to_take_vows_piety_cost (02_religion_values.txt:4323) = 100 + 250 for your child; his AI "
               "answers in 4-9 days (ai_maybe) unless you spend a strong hook; once in 10 years per son; the "
               "piety is taken at the send and given back if he refuses")
#: The son answers a vows ask (ai_maybe, VOWS_SOURCE): "allowed now" is the game letting it be sent, not his yes
VOWS_HE_DECIDES = "he decides: the game lets you send it; his AI answers in 4-9 days and may refuse"
#: disinherit_interaction cost (00_dynast_interactions.txt:176-255; 00_basic_values.txt:995-1005, 1485-1521)
DISINHERIT_SOURCE = ("disinherit_interaction (00_dynast_interactions.txt): you are the dynasty's head; price prestige "
                     "150, +750 if he is landed, +150 if landless and heir of any title (any_heir_title: under "
                     "partition a younger son is heir of the titles he takes); renown 75, +150 landed, +75 landless "
                     "heir, x the era factor; renown is paid from the dynasty's SPENDABLE renown, not the total its "
                     "level counts. game_check is the game's own price and verdict for that son")
#: scale_dynasty_prestige_by_era for an early medieval culture (00_basic_values.txt:1505-1521); the era is not read
ERA_FACTOR_EARLY = 2


def _without(fc_inputs: dict, cid: int, trait: str) -> dict:
    kids = [dict(c, traits=list(c.get("traits") or []) + [{"name": trait}]) if c.get("id") == cid else c
            for c in fc_inputs["children"]]
    try:
        f = forecast(**{**fc_inputs, "children": kids})
    except ForecastInputError as e:
        return {"refused": str(e)}
    return {"takers": f.get("takers"), "intact": f.get("intact"),
            "leaves_realm": [s["id"] for s in f.get("younger_shares") or [] if s["fate"] == "leaves_realm"]}


#: query_interaction_check's refusal codes as a player reads them
_CHECK_WORDS = {"send_cost_unaffordable": "you cannot pay its price",
                "interaction_not_valid_for_target": "the game does not allow it",
                "send_conditions_not_met": "the game will not let you send it now",
                "target_would_reject": "he would refuse"}


#: (a test run: "your culture has the Mystical Ancestors tradition" on its first turn, 1066.09.15, only the
#: price on turns 16 to 44). One reading takes the game's sentence "Culture does not have the Mystical Ancestors Tradition." as the
#: condition the send FAILS. The bridge log's raw text of that first check shows otherwise: the game's tooltip prints
#: disinherit_interaction's is_valid line (00_dynast_interactions.txt: the culture has no cannot_disherit parameter)
#: with its PASS marker ("trigger_pass_inactive") beside the line that failed, with its fail marker ("warning_icon X":
#: "Dal gCais Dynasty does not have 150 Renown", is_valid_showing_failures_only: dynasty_prestige >=
#: medium_dynasty_prestige_value, 75 x the era's 2). The bridge's cleaned words keep both lines and drop the markers.
#: Irish culture has no such tradition and cannot pick it (00_goidelic.txt traditions; 00_ritual_traditions.txt
#: tradition_mystical_ancestors can_pick: an ancestor culture had it). So this line is printed whether it holds or
#: not, and is never the reason by itself: the other lines are; alone, the failing condition gave no words.
MYSTICAL_ANCESTORS_WORDS = "Mystical Ancestors"
MYSTICAL_ANCESTORS_WHY = ("the game does not allow it and its words name no failing condition (it prints its "
                          "culture line, Mystical Ancestors, pass or fail); the others: your dynasty's renown at "
                          "least 150 unless he is a bastard, no strong hook of his on you, he is not your diarch")


def engine_why(reason: Any) -> tuple:
    """(the game's refusal words without its always-printed culture line, whether that line was there)."""
    parts = [p.strip() for p in str(reason or "").split(";") if p.strip()]
    kept = [p for p in parts if MYSTICAL_ANCESTORS_WORDS not in p]
    return "; ".join(kept), len(kept) < len(parts)


def disinherit_allowed(head: Any, ruler_id: Any, gc: Optional[dict]) -> tuple:
    """A recorded case (the GOAL line's lever said disinherit not allowed, by the game's check, while
    succession_plan's levers.disinherit said available, by the dynasty-head rule alone): THE disinherit verdict for
    one son, (allowed_now, why_not), for every reader (succession_plan's levers.disinherit, take_out_of_split, the
    death forecast's levers). The game's own check of the send decides when it was read (it holds the culture's
    Mystical Ancestors bar, the renown, a strong hook, the diarchy); without it the head rule can only refuse."""
    if head is not None and ruler_id is not None and head != ruler_id:
        return False, [f"you are not your dynasty's head ({head} is): only the head can disinherit"]
    if gc is not None:
        return gc["sendable"], ([gc["refused"]["why"]] if "refused" in gc else None)
    if head is None:
        return None, ["the dynasty's head was not read"]
    return None, ["the game's check of the send was not read (you are the dynasty's head, which it needs)"]


def vows_verdict(static_why: list, gc: Optional[dict]) -> dict:
    """A recorded case (two test runs, turns 23 and 46: a vows row read `allowed_now false`, "younger than 10",
    `game_check.sendable false` AND "he decides: the game lets you send it"): THE vows verdict for one son, one
    computation for `allowed_now`, `why_not`, `he_decides` and the pre-answer. The game's own check of the send
    decides when it was read; "he decides" only when that check says the ask can be sent. Without the check the
    script's own bars can only refuse, and nothing says the game lets it be sent."""
    if gc is None:
        return {"allowed_now": False if static_why else None, "why_not": list(static_why) or None}
    out: dict = {"allowed_now": gc["sendable"], "why_not": [gc["refused"]["why"]] if "refused" in gc else None}
    if gc["sendable"] is True:
        out["he_decides"] = VOWS_HE_DECIDES
        out.update({k: gc[k] for k in ("answer", "accept_score", "chance_pct") if k in gc})
    return out


def disinherit_summary(head: Any, ruler_id: Any, gcs: Iterable) -> tuple:
    """(available, why_not) over the candidate sons, by disinherit_allowed: available when the game allows it for
    one of them; the first son's reason otherwise."""
    return disinherit_by_son(head, ruler_id, [(None, gc) for gc in gcs])[:2]


def disinherit_by_son(head: Any, ruler_id: Any, pairs: Iterable) -> tuple:
    """(available, why_not, why_not_of, allowed_for) over [(son id, game check)], by disinherit_allowed for each son:
    `why_not_of` is the son whose reason `why_not` is (the sons' prices differ: a landed son's is the highest), and
    `allowed_for` the sons the game allows it for."""
    pairs = list(pairs)
    verdicts = [(son, disinherit_allowed(head, ruler_id, gc)) for son, gc in pairs] or \
        [(None, disinherit_allowed(head, ruler_id, None))]
    ok = [son for son, v in verdicts if v[0] is True]
    if ok:
        return True, None, None, [s for s in ok if s is not None]
    known = [x for x in verdicts if x[1][0] is False] or verdicts
    son, v = known[0]
    return v[0], "; ".join(v[1] or []) or None, son, []


def _game_check(chk: Any, held: dict) -> Optional[dict]:
    """The game's own check of one send, nothing sent (query_interaction_check): its verdict, its price for
    this son (engine_cost) and, for a price it refuses, each currency short against what is held."""
    if not isinstance(chk, dict) or "sendable" not in chk:
        return None
    out: dict = {"sendable": chk.get("sendable") is True}
    # (A seated game, turn 35: vows 65888 read "allowed now"; the bridge's check had scored it maybe, 25, and he
    # declined): the recipient's pre-answer and score, kept; a "maybe" in the band (0, 100) is the game's chance
    if chk.get("answer") in ("yes", "maybe", "no"):
        out["answer"] = chk["answer"]
        score = chk.get("accept_score")
        if isinstance(score, (int, float)) and not isinstance(score, bool):
            out["accept_score"] = round(float(score), 2)
            if chk["answer"] == "maybe" and 0 < score < 100:
                out["chance_pct"] = round(float(score), 1)
    cost = chk.get("engine_cost") if isinstance(chk.get("engine_cost"), dict) else None
    if cost is not None:
        out["price"] = cost
    if chk.get("sendable") is not True:
        code = str(chk.get("refused_by") or "unknown")
        why = _CHECK_WORDS.get(code, code)
        words, culture_line = engine_why(chk.get("engine_reason"))
        if words:
            why += ": " + words[:160]
        if code == "target_would_reject" and isinstance(chk.get("accept_score"), (int, float)):
            why += f" (his answer {chk['accept_score']:+.0f})"
        if culture_line and not words:
            why = MYSTICAL_ANCESTORS_WHY
        out["refused"] = {"code": code, "why": why}
        import diarchy as _diarchy  # noqa: PLC0415 - the diarch block named as the reason
        if _diarchy.is_block_words(chk.get("engine_reason")):
            note = _diarchy.lever_note(chk.get("target_id"))
            out["refused"].update(blocked_by="diarchy", why=note["reason"] + " (" + why + ")", diarchy=note)
        if code == "send_cost_unaffordable" and cost:
            short = {k: round(v - held[k], 2) for k, v in cost.items()
                     if isinstance(held.get(k), (int, float)) and v > held[k]}
            if short:
                out["refused"]["short"] = short
    return out


def lever_rows(fc: dict, fc_inputs: dict, *, ruler: dict, dynasty: Any, children: list,
               checks: Optional[dict] = None, title_heirs: Optional[set] = None) -> Optional[dict]:
    """{vows: [...], disinherit: [...]}: for each younger heir of a split, the price against what is
    held, whether the game allows it now, the ready call, and the forecast after. None when nothing
    splits.

    In a seated game (1077.08.16) disinherit was priced 150 prestige + 150 renown against 1,474 renown,
    `allowed_now: true`, and the game refused the send (send_cost_unaffordable). Two errors: under partition
    a younger son is heir of the titles he takes (any_heir_title: +150 prestige, +75 x era renown), and the
    renown read was the dynasty's ACCUMULATED total (its level), not the spendable renown a price is paid
    from (230 that day). `checks` {(interaction, son_id): query_interaction_check row} is the game's own
    check of each send with its own price: when present it decides `allowed_now` and the price; without it
    the price is computed from the script and the spendable renown (`dynasty.renown`)."""
    shares = [s for s in (fc or {}).get("younger_shares") or []]
    if not shares or fc.get("refused"):
        return None
    checks = checks or {}
    title_heirs = title_heirs or set()
    by_id = {c["id"]: c for c in children if isinstance(c, dict)}
    rid = ruler.get("id")
    piety, prestige = ruler.get("piety"), ruler.get("prestige")
    dyn = dynasty if isinstance(dynasty, dict) else {}
    head = dyn.get("head_character_id")
    # the spendable renown (query_dynasty.renown, dynasty+0x150); `prestige` is the accumulated total
    renown = dyn.get("renown") if isinstance(dyn.get("renown"), (int, float)) else None
    held = {"prestige": prestige, "piety": piety, "renown": renown}
    e = ERA_FACTOR_EARLY
    vows, dis = [], []
    for s in shares:
        c = by_id.get(s["id"], {})
        landed = bool(c.get("ruler_tier")) or c.get("has_realm") is True
        age = c.get("age")
        why = []
        if landed:
            why.append("he holds land (a courtier is asked, not a ruler)")
        if isinstance(age, (int, float)) and age < 10:
            why.append(f"he is {int(age)}: 10 or older")
        if c.get("faith_id") is not None and ruler.get("faith_id") is not None and c["faith_id"] != ruler["faith_id"]:
            why.append("another faith than yours")
        if isinstance(piety, (int, float)) and piety < VOWS_PIETY_CHILD:
            why.append(f"piety {int(piety)} < {VOWS_PIETY_CHILD}")
        row = {"son_id": s["id"], "name": s.get("name"), "price": {"piety": VOWS_PIETY_CHILD},
               "you_hold": {"piety": piety},
               "not_checked": "at your court, not your chaplain, his marriage type; and his answer (he may "
                              "refuse: discover(what='acceptance', interaction_type='take_vows_interaction', "
                              "target_id=<him>) reads his chance)",
               "call": {"tool": "do", "args": {"intent": "interact", "args": {
                   "interaction": "take_vows_interaction", "target_id": s["id"]}}},
               "after": _without(fc_inputs, s["id"], "devoted")}
        gc = _game_check(checks.get(("take_vows_interaction", s["id"])), held)
        row.update(vows_verdict(why, gc))
        if gc is not None:
            row["game_check"] = gc
            row["price"] = gc.get("price") or row["price"]
            row.pop("not_checked")
        vows.append(row)
        # any_heir_title: under partition each title has its own heir, and a younger son is heir of the titles
        # he takes (per_title_map heir_id); the eldest's landless brother with no title is priced 150 (live
        # A live check, 1066.09.15, one county)
        heir_term = (not landed) and s["id"] in title_heirs
        p_price = 150 + (750 if landed else 0) + (150 if heir_term else 0)
        r_price = (75 + (150 if landed else 0) + (75 if heir_term else 0)) * e
        dwhy = []
        if head is not None and head != rid:
            dwhy.append(f"you are not your dynasty's head ({head} is): only the head can disinherit")
        if head is None:
            dwhy.append("the dynasty's head was not read")
        if c.get("dynasty_id") is not None and ruler.get("dynasty_id") is not None and c["dynasty_id"] != ruler["dynasty_id"]:
            dwhy.append("he is not of your dynasty")
        if renown is not None and renown < r_price:
            dwhy.append(f"spendable renown {int(renown)} < {r_price}")
        if isinstance(prestige, (int, float)) and prestige < p_price:
            dwhy.append(f"prestige {int(prestige)} < {p_price}")
        row = {"son_id": s["id"], "name": s.get("name"),
               "price": {"prestige": p_price, "renown": r_price,
                         "era_factor": f"{e} (early medieval; the era is not read)"},
               "you_hold": {"prestige": prestige, "renown_spendable": renown},
               "allowed_now": (False if dwhy else True if head == rid and renown is not None else None),
               "why_not": dwhy or (None if renown is not None else ["the dynasty's spendable renown was not read"]),
               "call": {"tool": "do", "args": {"intent": "interact", "args": {
                   "interaction": "disinherit_interaction", "target_id": s["id"]}}},
               "after": _without(fc_inputs, s["id"], "disinherited")}
        gc = _game_check(checks.get(("disinherit_interaction", s["id"])), held)
        if gc is not None:
            row["game_check"] = gc
            if gc.get("price"):
                row["price"] = gc["price"]
            row["allowed_now"], row["why_not"] = disinherit_allowed(head, rid, gc)
            if "he is not of your dynasty" in dwhy:
                # The game's check named another condition (the culture's); the dynasty
                # bars it first (is_shown)
                row["allowed_now"] = False
                row["why_not"] = ["he is not of your dynasty"] + (row["why_not"] or [])
        if row["allowed_now"] is False and isinstance(row.get("why_not"), list):
            short = ((gc or {}).get("refused") or {}).get("short") or {}
            if "renown" in short or any("renown" in w for w in row["why_not"]):
                row["to_allow"] = ("more spendable renown: your dynasty's members earn it each month, a "
                                   "legacy bought spends it")
        dis.append(row)
    return {"vows": vows, "disinherit": dis, "vows_rule": VOWS_SOURCE, "disinherit_rule": DISINHERIT_SOURCE}
