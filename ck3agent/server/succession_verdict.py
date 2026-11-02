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
                             pass intact and name no heir who takes a title away)
        passes_intact        True / False / None, the same answer: None is
                             undetermined, never read as either
        undetermined_reason  why, when undetermined (else absent)
        breaker              {id, name, takes[]} the first other heir who takes a
                             title, or None
        eligible_heir_count  heirs that land at least one title
        heirs                [{id, name, takes|titles, ...}] primary first
        heirs_source         "simulation" | "per_title_map"
        primary              the first heir row ({} when none)
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
"""
from __future__ import annotations

from typing import Any

#: The verdict's three states. "splits" always carries a breaker: a "does not
#: pass intact" with nobody named is not a reading, it is undetermined (L14 FF1
#: M-3: the map fallback said false with breaker None and one heir).
OUTCOMES = ("intact", "splits", "undetermined")


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
        }
    primary = heirs[0]
    breaker = None
    for h in heirs[1:]:
        takes = h.get("takes") or []
        if takes:
            breaker = {"id": h.get("id"), "name": h.get("name"), "takes": takes}
            break
    splits = breaker is not None or bool(eligible_heir_count and eligible_heir_count > 1)
    passes_intact = not splits
    if from_map:
        passes_intact = not msum.get("splits")
    out_extra: dict[str, Any] = {}
    if passes_intact:
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
    }


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
