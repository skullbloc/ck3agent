"""Does the next succession pass intact? ONE definition (cleanup 2026.11 L14, finding 8).

The player was answered by five reads in three vocabularies: `scatters` (the
per-title map's summary, in observe and inspect(succession_preview)),
`is_partition` (the succession simulation's heir row, in the live-state hook and
query(partition_preview)) and `passes_intact` (succession_plan). They could
disagree: on the 867 save the simulation named no heir while the map projected
three (live867 l4-live-a-t1). The vocabulary is succession_plan's, and the
computation lives here so that every reader that states a verdict calls it:
succession_plan, observe's succession block and the live-state hook
(ck3agent/hooks/inject_state.py).

    verdict(sim, succ, map_summary) -> {
        outcome              one of OUTCOMES: "intact" (one heir takes every
                             title), "splits" (an other heir takes titles: the
                             breaker), "undetermined" (the reads say it does not
                             pass intact and name no heir who takes a title away,
                             or a crown title beside the primary has no heir read)
        passes_intact        True / False / None, the same answer: None is
                             undetermined, never read as either
        undetermined_reason  why, when undetermined (else absent)
        breaker              {id, name, takes[]} the first other heir who takes a
                             title, or None; `source: "per_title_map"` when it is
                             the map's heir for a crown title
        eligible_heir_count  heirs that land at least one title
        heirs                [{id, name, takes|titles, ...}] primary first
        heirs_source         "simulation" | "per_title_map"
        primary              the first heir row ({} when none)
        crown_titles         [{title_id, key, name, tier, simulation_heir_id,
                              map_heir_id, agree}] each held kingdom/empire
        scope                SCOPE: what the verdict covers
    }

NO DEFAULT ON EITHER SIDE. "intact" needs a heir read who takes every title,
and "splits" needs a breaker named. With no heir read at all (the simulation
empty and the map unavailable or naming no projected heir) the answer is
"undetermined", never "intact" (L14 FF2 N-MAJ-2: two failed reads answered
intact, with nobody named, and no warning), exactly as a "does not pass intact"
with nobody named is "undetermined", never "splits". Every reader that states
the verdict carries `warning(v)` when it is undetermined.

`sim` is query_succession_simulation's reply (sim_heirs reads it), `succ`
query_succession's (kept for the callers' signature; no field of it decides), and
`map_summary` is map_summary(query_title_succession_map's reply). The map is the
fallback for the heirs when the simulation names none, and then its split is the
verdict. Pure: no reads, no bridge, so a host test drives the same code.

EVERY CROWN TITLE IS CHECKED (v4fix B2, set v3-registered-2 s3). Charles held
k_france AND k_aquitaine (observe's de_jure block, read off the same held-title
array, named both on all 13 of his observes). All 25 of the king's observes said
"intact" with one eligible heir; at his death k_aquitaine went to Carloman and the
realm fell 89 -> 44 (e0-s5, with no agents, split the same way). Why the
simulation said one heir (do_query_succession_simulation, query_handlers.cpp):
its partition switch is the PRIMARY title's division alone (`is_partition =
primary_division == 1`), and a kingdom carrying its own law vector whose entries
leave division unset resolves to "unset" (resolve_effective_succ_rule does not
fall back to the realm law then), so every other title went to the primary heir.
Measured on the 867 core dump (ivar867, Charles 12015): k_france and k_aquitaine
each carry a title-law vector resolving (order, division, rank) = (unset, unset,
unset), and the engine's own per-title heir cache (title+0x290, what
query_title_succession_map projects) names Louis 14577 first on k_france and
Carloman 15319 first on k_aquitaine. So the verdict now holds the simulation
against the per-title map on every held title of kingdom tier or higher
(CROWN_TIER): a crown title the map projects to another heir is a breaker
(source per_title_map), and a crown title beside the primary that the map could
not project leaves the verdict undetermined, never intact. Lower tiers keep the
simulation's answer: there the map's cache names one heir for every county of a
partitioned duchy (s6 D2 succession_plan: map all Émenon, simulation Uzès to
Arnaut), so it is no check. `crown_titles` carries each crown title with both
reads' heirs, and `scope` says what no read here can see: a title created,
usurped or inherited after the read.
"""
from __future__ import annotations

from typing import Any

#: The verdict's three states. "splits" always carries a breaker: a "does not
#: pass intact" with nobody named is not a reading, it is undetermined (L14 FF1
#: M-3: the map fallback said false with breaker None and one heir).
OUTCOMES = ("intact", "splits", "undetermined")

#: The tier from which a title is a crown of its own, checked title by title:
#: kingdom (4) and empire (5). Both reads write the bridge's tier (1 barony ..
#: 5 empire; get_title_tier / TITLE_TYPE_TIER) as `tier`.
CROWN_TIER = 4

#: What every verdict covers. ONE spelling, carried by every reader that states it.
SCOPE = ("the titles held at this read; a title created, usurped or inherited "
         "after it is not in this verdict")


def _as_int(v) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


def sim_heirs(sim: dict) -> list[dict]:
    """query_succession_simulation's heirs as the bridge emits them
    (do_query_succession_simulation): `heirs`, the player heir first, each
    {character_id, name, is_player_heir, title_count, titles[]}. Each becomes
    {id, name, titles}, the player heir first; the others carry `takes` (the titles
    they would take away). Until L14 the server read `eligible_heirs`, a key the
    bridge never emits, so every simulation read as "no heirs" and the verdict
    fell back to the per-title map (or to none)."""
    rows = sim.get("heirs") if isinstance(sim, dict) else None
    out: list[dict] = []
    for h in rows if isinstance(rows, list) else []:
        if not isinstance(h, dict):
            continue
        hid = _as_int(h.get("character_id"))
        if not hid:
            continue
        titles = h.get("titles") if isinstance(h.get("titles"), list) else []
        row = {"id": hid, "name": h.get("name") or None, "titles": titles,
               "is_player_heir": bool(h.get("is_player_heir"))}
        out.append(row)
    out.sort(key=lambda r: not r["is_player_heir"])   # stable: the bridge's order otherwise
    for r in out[1:]:
        r["takes"] = r["titles"]
    return out


def map_summary(sp: dict) -> dict[str, Any]:
    """The bridge's query_title_succession_map payload as per-title detail. Guarded:
    a missing or error payload is available=False with a reason, never a guessed
    split.

    The bridge emits a TITLE-keyed shape:
        {primary_title_id, primary_title_tier, primary_heir_id, primary_heir_name,
         titles: [{title_id, title_key, title_name, tier, succession_law,
                   title_division, partition_rank?, projected_heir_id,
                   projected_heir_name, splits_away}, ...],
         any_splits: bool, status: "ok"}
    `splits` (internal: verdict() reads it, no reply carries it) is the map's own
    answer: the bridge's any_splits, or a title projected to an heir other than the
    primary's, or more than one heir landing a title.
    """
    if not isinstance(sp, dict) or sp.get("error"):
        reason = (sp or {}).get("error") if isinstance(sp, dict) else "no_response"
        return {"available": False, "reason": reason or "no_response"}

    titles = sp.get("titles")
    if not isinstance(titles, list):
        return {"available": False, "reason": "no_titles"}

    primary_heir_id = sp.get("primary_heir_id")
    if isinstance(primary_heir_id, int) and primary_heir_id < 0:
        primary_heir_id = None
    primary_title_id = sp.get("primary_title_id")

    rows = [t for t in titles if isinstance(t, dict)]

    by_heir: dict[int, dict] = {}
    primary_title_count = 0
    for t in rows:
        hid = t.get("projected_heir_id")
        if not isinstance(hid, int) or hid <= 0:
            continue  # unknown projected heir for this title: not attributed
        slot = by_heir.setdefault(hid, {"id": hid, "name": t.get("projected_heir_name"),
                                        "title_count": 0, "titles": []})
        if not slot.get("name") and t.get("projected_heir_name"):
            slot["name"] = t.get("projected_heir_name")
        slot["title_count"] += 1
        slot["titles"].append({"title_id": t.get("title_id"), "key": t.get("title_key"),
                               "name": t.get("title_name"), "tier": t.get("tier")})
        if isinstance(primary_heir_id, int) and hid == primary_heir_id:
            primary_title_count += 1

    eligible_heir_count = len(by_heir)
    primary = None
    if isinstance(primary_heir_id, int) and primary_heir_id in by_heir:
        ph = by_heir[primary_heir_id]
        primary = {"id": ph["id"], "name": ph["name"], "title_count": ph["title_count"]}
    elif sp.get("primary_heir_name"):
        primary = {"id": primary_heir_id, "name": sp.get("primary_heir_name"),
                   "title_count": primary_title_count}

    other_heir = None
    for hid, slot in by_heir.items():
        if isinstance(primary_heir_id, int) and hid == primary_heir_id:
            continue
        other_heir = slot
        break
    splits = bool(sp.get("any_splits")) or other_heir is not None or eligible_heir_count > 1

    partition_titles: list = []
    single_heir_titles: list = []
    for t in rows:
        row = {"title_id": t.get("title_id"), "key": t.get("title_key"),
               "name": t.get("title_name"), "tier": t.get("tier"),
               "law": t.get("succession_law")}
        div = t.get("title_division")
        if div == "partition":
            if t.get("partition_rank"):
                row["partition_rank"] = t.get("partition_rank")
            partition_titles.append(row)
        elif div == "single_heir":
            single_heir_titles.append(row)
        # "unset"/"n/a"/None: unclassified (no guess).

    top_title = None
    for t in rows:
        if t.get("title_id") == primary_title_id:
            top_title = {"title_id": primary_title_id, "key": t.get("title_key"),
                         "name": t.get("title_name"), "tier": t.get("tier"),
                         "law": t.get("succession_law")}
            break

    return {
        "available": True,
        "splits": splits,
        "eligible_heir_count": eligible_heir_count,
        "primary_heir": primary,
        "partition_titles": partition_titles,
        "single_heir_titles": single_heir_titles,
        "top_title": top_title,
        # Each title with the heir it is projected to.
        "titles": [{"title_id": t.get("title_id"), "key": t.get("title_key"),
                    "name": t.get("title_name"), "tier": t.get("tier"),
                    "law": t.get("succession_law"),
                    "heir_id": t.get("projected_heir_id"),
                    "heir_name": t.get("projected_heir_name"),
                    "splits_away": t.get("splits_away")} for t in rows],
        # Every projected heir, primary first (the heirs when the simulation names none).
        "heirs": sorted(by_heir.values(),
                        key=lambda h: (h["id"] != primary_heir_id, -h["title_count"])),
    }


def verdict(sim: dict, succ: dict, msum: dict) -> dict[str, Any]:
    """The one answer to "does the next succession pass intact?" (module docstring)."""
    sim = sim if isinstance(sim, dict) else {}
    succ = succ if isinstance(succ, dict) else {}
    msum = msum if isinstance(msum, dict) else {}
    # The simulation's per-heir titles, never is_partition (B2).
    heirs = sim_heirs(sim)
    eligible_heir_count = len(heirs) if heirs else None
    # Lane l4, measured on the 867 save (live867 l4-live-a-t1, ruler 10465): the
    # simulation named no heir while the per-title map projected Karlmann on two titles
    # and Ludwig on k_bavaria. When the simulation is empty and the map is available,
    # the map's projected heirs are the heirs and its split is the verdict.
    from_map = False
    if not heirs and msum.get("available") and msum.get("heirs"):
        heirs = [{"id": h["id"], "name": h.get("name"), "titles": h.get("titles") or [],
                  "takes": [] if i == 0 else h.get("titles") or []}
                 for i, h in enumerate(msum["heirs"])]
        eligible_heir_count = len(heirs)
        from_map = True
    if eligible_heir_count is None:
        eligible_heir_count = 0
    if not heirs:
        # Nobody read, so nobody is known to take every title (module docstring).
        if msum.get("available"):
            map_why = "it projects no heir"
        else:
            map_why = str(msum.get("reason") or "not read")
        return {
            "outcome": "undetermined",
            "undetermined_reason": f"no heir was read (simulation empty; map: {map_why})",
            "passes_intact": None,
            "breaker": None,
            "eligible_heir_count": 0,
            "heirs": [],
            "heirs_source": "simulation",
            "primary": {},
            "crown_titles": [],
            "scope": SCOPE,
        }
    primary = heirs[0]
    breaker = None
    for h in heirs[1:]:
        takes = h.get("takes") or []
        if takes:
            breaker = {"id": h.get("id"), "name": h.get("name"), "takes": takes}
            break
    crown, map_breakers, unchecked = _crown_check(
        heirs, msum, from_map,
        _as_int(sim.get("primary_title_id")) or _as_int((msum.get("top_title") or {}).get("title_id")))
    if breaker is None and map_breakers:
        # The simulation gave a crown title to the primary heir and the engine's own
        # per-title heir names another (module docstring): that heir is the breaker.
        heirs = _with_map_breakers(heirs, map_breakers)
        primary = heirs[0]
        eligible_heir_count = sum(1 for h in heirs if h.get("titles"))
        mb = map_breakers[0]
        breaker = {"id": mb["id"], "name": mb.get("name"), "takes": mb["takes"],
                   "source": "per_title_map"}
    splits = breaker is not None or bool(eligible_heir_count and eligible_heir_count > 1)
    passes_intact = not splits
    if from_map:
        passes_intact = not msum.get("splits")
    out_extra: dict[str, Any] = {}
    if passes_intact and unchecked:
        outcome, passes_intact = "undetermined", None
        if msum.get("available"):
            why = "the per-title map projects no heir for it"
        else:
            why = f"the per-title map was not read ({msum.get('reason') or 'no_response'})"
        out_extra["undetermined_reason"] = (
            "the succession simulation gives every title to the primary heir, but it "
            "decides partition from the primary title alone, and "
            + ", ".join(str(t.get("key") or t.get("title_id")) for t in unchecked)
            + " is a crown title beside it whose own heir was not read: " + why)
    elif passes_intact:
        outcome = "intact"
    elif breaker is not None:
        outcome = "splits"
    else:
        outcome, passes_intact = "undetermined", None
        out_extra["undetermined_reason"] = (
            ("the per-title map says the titles split" if from_map else
             f"the succession simulation names {eligible_heir_count} heirs")
            + ", and no heir but the primary is projected to take a title")
    return {
        "outcome": outcome,
        **out_extra,
        "passes_intact": passes_intact,
        "breaker": breaker,
        "eligible_heir_count": eligible_heir_count,
        "heirs": heirs,
        "heirs_source": "per_title_map" if from_map else "simulation",
        "primary": primary,
        "crown_titles": crown,
        "scope": SCOPE,
    }


def _crown_check(heirs: list, msum: dict, from_map: bool, primary_title_id: int):
    """Each held title of CROWN_TIER or higher with the heir each read gives it:
    rows {title_id, key, name, tier, simulation_heir_id, map_heir_id, agree}.
    Returns (rows, map_breakers, unchecked): the heirs the map gives a crown title
    the simulation gave the primary heir, each {id, name, takes[]}, and the crown
    titles beside the primary title whose map heir was not read."""
    primary_id = heirs[0].get("id") if heirs else None
    sim_owner: dict[int, Any] = {}
    sim_rows: dict[int, dict] = {}
    if not from_map:
        for h in heirs:
            for t in h.get("titles") or []:
                if isinstance(t, dict) and _as_int(t.get("title_id")):
                    sim_owner[_as_int(t["title_id"])] = h.get("id")
                    sim_rows[_as_int(t["title_id"])] = t
    map_rows: dict[int, dict] = {}
    if msum.get("available"):
        for t in msum.get("titles") or []:
            if isinstance(t, dict) and _as_int(t.get("title_id")):
                map_rows[_as_int(t["title_id"])] = t
    order: list[int] = []
    for tid in list(sim_rows) + list(map_rows):
        if tid not in order:
            order.append(tid)
    rows, map_breakers, unchecked = [], [], []
    by_heir: dict[int, dict] = {}
    for tid in order:
        src = map_rows.get(tid) or sim_rows.get(tid) or {}
        if _as_int(src.get("tier")) < CROWN_TIER:
            continue
        m = map_rows.get(tid)
        map_heir = _as_int(m.get("heir_id")) if m else 0
        map_heir = map_heir if map_heir > 0 else None
        sim_heir = None if from_map else sim_owner.get(tid)
        row = {"title_id": tid, "key": src.get("key"), "name": src.get("name"),
               "tier": _as_int(src.get("tier")), "simulation_heir_id": sim_heir,
               "map_heir_id": map_heir,
               "agree": (None if from_map or map_heir is None or sim_heir is None
                         else map_heir == sim_heir)}
        rows.append(row)
        if from_map or tid == primary_title_id:
            continue
        if map_heir is None:
            unchecked.append(row)
        elif map_heir != primary_id and sim_heir in (primary_id, None):
            slot = by_heir.get(map_heir)
            if slot is None:
                slot = {"id": map_heir, "name": (m or {}).get("heir_name"), "takes": []}
                by_heir[map_heir] = slot
                map_breakers.append(slot)
            slot["takes"].append({"title_id": tid, "key": row["key"], "name": row["name"],
                                  "tier": row["tier"], "source": "per_title_map"})
    return rows, map_breakers, unchecked


def _with_map_breakers(heirs: list, map_breakers: list) -> list:
    """The heir rows with each map breaker's crown titles moved off the primary heir
    onto that breaker (a copy; the rows passed in are not changed)."""
    moved = {t["title_id"] for mb in map_breakers for t in mb["takes"]}
    out = [dict(h) for h in heirs]
    out[0]["titles"] = [t for t in out[0].get("titles") or []
                        if not (isinstance(t, dict) and _as_int(t.get("title_id")) in moved)]
    for mb in map_breakers:
        row = next((h for h in out[1:] if h.get("id") == mb["id"]), None)
        if row is None:
            row = {"id": mb["id"], "name": mb.get("name"), "titles": [], "takes": []}
            out.append(row)
        row["titles"] = list(row.get("titles") or []) + mb["takes"]
        row["takes"] = list(row.get("takes") or []) + mb["takes"]
    return out


#: The warning every reader that states the verdict carries when it is
#: undetermined (observe, succession_plan). ONE spelling for the code doctrine
#: keys on (knowledge/britannia-campaign.md, prompts.py).
UNDETERMINED_WARNING = "succession_undetermined"


def warning(v: dict) -> "dict[str, Any] | None":
    """The `succession_undetermined` warning for an undetermined verdict, else None."""
    if not isinstance(v, dict) or v.get("outcome") != "undetermined":
        return None
    return {"code": UNDETERMINED_WARNING, "is_known_limitation": False,
            "detail": ("whether the next succession passes intact is undetermined: "
                       f"{v.get('undetermined_reason')}. passes_intact is null, which is "
                       "neither true nor false: read succession_plan's per_title_map and "
                       "heirs before acting on the succession.")}
