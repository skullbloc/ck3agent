"""inspect(subject="threats"): who can declare war on you now, and on what.

For each ruler the reads can enumerate cheaply, the game's own casus-belli check is asked with HIM as
the attacker and your ruler as the target (query_cbs_for_target takes any attacker; the same check
cbs() asks for you), and the reply lists the casus belli it accepts, his strength against yours, and
the vassalization rule with your county count.

Candidates: the top lieges of the counties within two map steps of yours (query_map_graph over your
held counties, land and sea crossings of the baked map), strongest first, at most MAX_ATTACKERS.
Not covered, and said in every reply: realms farther than that (the game's AI looks wider for some
casus belli), your own liege and vassals' internal wars, and whether his AI wants the war (it skips a
target stronger than itself at peace, and vassalization_cb only against neighbours:
common/defines/ai/00_ai.txt, 00_vassalization.txt ai_only_against_neighbors).

vassalization_cb (common/casus_belli_types/00_vassalization.txt:10-92): an attacker of a higher tier
than yours may force vassalage on a realm of at most vassalization_size_limit counties, 4 + 3 for each
of casus_belli, chronicle_writing, divine_right and rightful_ownership his culture knows
(script_values/00_war_values.txt:680-720): 7 for most kings in 1066; a crown (kingdom tier) above a
duke-or-count attacker's tier, or more counties than his limit, closes it.
"""
from __future__ import annotations

from typing import Any, Optional

import partition_forecast as _pf

MAX_ATTACKERS = 8
HOPS = 2
NOT_COVERED = ("realms farther than two map steps from your counties that have not yet attacked you; your "
               "liege's and vassals' internal wars; whether his AI wants the war (it skips a target stronger than "
               "itself at peace, and considers vassalization only against neighbours)")
VASSALIZATION_CB = "vassalization_cb"

#: actor id -> {attacker id: [declarable cb keys]} at the last threats read (news)
_LAST: dict = {}
#: (William of England attacked Munster 1079.11 from beyond two map steps and was named
#: in attackers_new only at 1081.07, after his Irish vassals brought his realm into the ring): actor id -> the
#: leaders who have led a war against him, seen in any read here; they stay candidates however far they are
_ATTACKED_BY: dict = {}


def reset() -> None:
    _LAST.clear()
    _ATTACKED_BY.clear()


def _d(x) -> dict:
    return x if isinstance(x, dict) else {}


def _i(v) -> Optional[int]:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


async def candidates(gw, me: int) -> tuple[list, Optional[str], list]:
    """(attacker ids strongest first, why unread or None, my county keys)."""
    import map_view as _mv
    hier = _d(await gw.read("query_title_hierarchy", {"character_id": me}))
    mine = sorted({t.get("key") for t in hier.get("held_titles") or []
                   if isinstance(t, dict) and _i(t.get("tier")) == 2 and t.get("key")})
    if not mine:
        return [], "your held counties were not read (query_title_hierarchy)", []
    g = _d(await gw.read("query_map_graph", {"character_id": me, "scope": "explicit", "counties": ",".join(mine),
                                             "hops": HOPS, "max_nodes": 200}))
    graph = _mv.parse_graph(g.get("text")) if g.get("text") else {}
    if not graph:
        return [], ("the bridge's map graph did not answer"
                    + (f" ({g.get('error')})" if g.get("error") else "") + ": no neighbour is enumerated"), mine
    holders = sorted({n["holder"] for k, n in graph.items() if k not in mine and _i(n.get("holder"))})
    chars = await gw.read_many([("query_character", {"character_id": h}) for h in holders]) if holders else []
    tops = sorted({(_i(_d(c).get("top_liege_id")) or h) for h, c in zip(holders, chars) if _d(c)} - {me})
    return sorted(set(tops) | await attacked_by(gw, me)), None, mine


async def attacked_by(gw, me: int) -> set:
    """The leaders of the wars against `me` now (get_active_wars: the attacker of a war he
    defends), with every one seen before in this server; never `me`."""
    w = _d(await gw.read("get_active_wars", {"character_id": me}))
    seen = _ATTACKED_BY.setdefault(me, set())
    for x in w.get("wars") or []:
        if isinstance(x, dict) and _i(x.get("attacker_id")) and _i(x.get("attacker_id")) != me \
                and not x.get("is_attacker"):
            seen.add(_i(x.get("attacker_id")))
    return set(seen) - {me}


async def read(gw, me: int) -> dict:
    """The threats result (no envelope): {attackers: [...], vassalization: {...}, not_covered, unread?}."""
    import redesign as R
    tops, why, mine = await candidates(gw, me)
    me_realm = _d(await gw.read("query_realm", {"character_id": me}))
    me_chr = _d(await gw.read("query_character", {"character_id": me}))
    my_str = _i(me_realm.get("military_strength")) or _i(me_chr.get("military_strength"))
    my_tier = _i(me_chr.get("ruler_tier")) or _i(me_realm.get("ruler_tier"))
    my_counties = _i(me_realm.get("realm_total_counties"))
    out: dict[str, Any] = {"you": {"id": me, "tier": my_tier, "strength": my_str,
                                   "counties_in_realm": my_counties, "held_counties": len(mine)},
                           "not_covered": NOT_COVERED}
    if why:
        out["unread"] = why
    # each ruler's strength from his own character read (query_character military_strength, the read observe's
    # realms line and the map digest take it from too)
    chars = await gw.read_many([("query_character", {"character_id": t}) for t in tops]) if tops else []
    by_id = {t: _d(c) for t, c in zip(tops, chars)}
    strengths: dict = {t: _i(c.get("military_strength")) for t, c in by_id.items()}
    ranked = sorted(tops, key=lambda t: -(strengths.get(t) or 0))
    out["candidates_seen"] = len(ranked)
    rows = []
    for t in ranked[:MAX_ATTACKERS]:
        ch = by_id.get(t) or {}
        raw = await R._cb_read_with_retry(gw, "query_cbs_for_target",
                                          {"attacker_id": t, "target_id": me, "timeout_ms": 6000})
        row: dict[str, Any] = {"id": t, "name": ch.get("name"), "tier": _i(ch.get("ruler_tier")),
                               "strength": strengths.get(t)}
        if my_str and strengths.get(t) is not None:
            row["x_yours"] = round(strengths[t] / my_str, 1)
        if not isinstance(raw, dict) or not isinstance(raw.get("cbs"), list):
            row["can_declare"] = None
            row["unread"] = "the casus-belli check did not answer for him"
        else:
            valid = [c for c in raw["cbs"] if isinstance(c, dict) and c.get("valid")]
            row["can_declare"] = sorted({str(c.get("cb_key")) for c in valid})
            titles = sorted({_i(c.get("cb_title_id") or c.get("title_id")) for c in valid
                             if _i(c.get("cb_title_id") or c.get("title_id"))})
            if titles:
                row["on_titles"] = titles[:8]
            listed = {str(c.get("cb_key")) for c in raw["cbs"] if isinstance(c, dict)}
            if VASSALIZATION_CB in listed or VASSALIZATION_CB in row["can_declare"]:
                row["vassalization"] = await _vassalization(gw, ch, my_tier, my_counties,
                                                            VASSALIZATION_CB in row["can_declare"])
        rows.append(row)
    out["attackers"] = rows
    if len(ranked) > MAX_ATTACKERS:
        out["more"] = f"{len(ranked) - MAX_ATTACKERS} weaker rulers in range were not checked"
    out["vassalization"] = vassalization_summary(
        my_tier, my_counties, [(r.get("vassalization") or {}).get("limit") for r in rows])
    out["can_declare_now"] = [{"id": r["id"], "cbs": r["can_declare"]} for r in rows if r.get("can_declare")]
    _LAST[me] = {r["id"]: list(r.get("can_declare") or []) for r in rows if r.get("can_declare") is not None}
    return out


async def _vassalization(gw, ch: dict, my_tier, my_counties, valid: bool) -> dict:
    inn = _d(await gw.read("query_innovation_progress", {"culture_id": _i(ch.get("culture_id")) or 0,
                                                          "character_id": _i(ch.get("id")) or 0}))
    rows = inn.get("innovations")
    known = ([str(x.get("name")) for x in rows if isinstance(x, dict) and x.get("is_known")]
             if isinstance(rows, list) else None)
    if known is None or not isinstance(my_tier, int) or not isinstance(my_counties, int):
        return {"valid_now": valid, "limit": None,
                "unread": "his culture's innovations or your county count were not read"}
    ex = _pf.vassalization_exposure(my_tier=my_tier, my_sub_realm_counties=my_counties,
                                    attacker_tier=_i(ch.get("ruler_tier")) or 0, attacker_innovations=known,
                                    attacker_unlocked=True if valid else None)
    return {"valid_now": valid, "limit": ex["limit"], "counties_to_close": ex["counties_to_close"],
            "closed_by": ex["closed_by"]}


def vassalization_summary(my_tier, my_counties, limits=None) -> dict:
    """The vassalization verdict for your realm. (867: every attackers_new row said "a crown
    for a 1066 king (limit 7)"): the limit is the highest one READ this read, from each attacker's culture's
    innovations (_vassalization), never a start's constant; none read: the rule without a number."""
    read = [x for x in (limits or []) if isinstance(x, int) and not isinstance(x, bool)]
    lim = max(read) if read else None
    out = {"rule": ("a ruler of a higher tier may force vassalage on a realm of at most 4 + 3 per war "
                    "innovation of his culture counties; more counties or a crown closes it"),
           "your_counties": my_counties, "source": "00_vassalization.txt:10-92, 00_war_values.txt:680-720"}
    if isinstance(my_tier, int) and my_tier >= 4:
        out["verdict"] = "closed: you hold a crown (no king outranks a king for this casus belli)"
    elif isinstance(my_counties, int) and lim is not None:
        out["verdict"] = (f"closed at {lim + 1} counties or a crown (limit {lim}, the highest of the "
                          f"{len(read)} read here); "
                          + ("you are above it" if my_counties > lim else
                             f"you hold {my_counties}: {lim + 1 - my_counties} more close it"))
        out["limit_read"] = lim
    elif isinstance(my_counties, int):
        out["verdict"] = (f"unknown: no higher-tier neighbour's innovations were read here (you hold "
                          f"{my_counties}; a crown closes it)")
    else:
        out["verdict"] = "unknown: your realm's county count was not read"
    return out


def new_attackers(me: int, before: Optional[dict]) -> list:
    """Rulers able to declare now who were not (or not with that casus belli) at `before`."""
    now = _LAST.get(me) or {}
    if before is None:
        return []
    out = []
    for a, cbs in now.items():
        fresh = sorted(set(cbs) - set(before.get(a) or []))
        if fresh:
            out.append({"id": a, "cbs": fresh})
    return out
