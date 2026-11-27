"""query(domain="buildings") answers a holding's slots and what can be built there, not the
building database.

In one test run `query(domain="buildings", title_id=463)` returned the whole building
database, 125,588 characters, whatever the title (the domain passed no parameter to the bridge); the CLI
would not hand it to the seat, so the read was lost. Now:

  no title_id   the building types by name only (filtered by `filter`, a name fragment), at most
                BUILDING_NAMES_SHOWN, with `truncated` naming the rest
  title_id      a county or barony of your own domain (query_county_details lists only yours): each
                holding of the county with its slots, the buildings in them and the empty ones; then, at
                the barony named (or the county's first holding, as the bridge reads a county's barony),
                what can be built there: the level-1 buildings not built there and the next level of each
                built one, each with the game's own verdict and price (construction_check, one call for
                the list) and the ready do(intent="build") call; the refused ones counted by the game's
                reason. At most BUILDABLE_SHOWN rows (max_results raises it), `truncated` the rest.

Every row the reply leaves out is counted; nothing is dropped silently.
"""
from __future__ import annotations

import re
from typing import Any, Optional

BUILDING_NAMES_SHOWN = 60
BUILDABLE_SHOWN = 20
REFUSAL_KINDS_SHOWN = 6
_LEVEL = re.compile(r"^(.*)_(\d\d)$")
#: a refusal whose only words are the gold missing (the game's construction validity, as _reason renders it)
_GOLD_ONLY = re.compile(r"^the game refuses: Missing gold (\d+)$")
NO_REASON = "the game's construction validity refuses, with no reason text"


def _i(v) -> Optional[int]:
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str) and v.strip().isdigit():
        return int(v.strip())
    return None


def _names(types_reply: Any) -> Optional[list]:
    if not isinstance(types_reply, dict) or types_reply.get("error"):
        return None
    rows = types_reply.get("building_types") or []
    if not isinstance(rows, list):
        return None
    return [str(b["name"]) for b in rows if isinstance(b, dict) and b.get("name")]


def holding_row(h: dict) -> dict:
    """One holding: its barony, type, slots, the buildings in them and how many are empty (the bridge's
    building_slots_used counts slots, not buildings, so the empty ones are counted here from the slots)."""
    slots = [s for s in h.get("buildings") or [] if isinstance(s, dict)]
    built = [f"{s['type']}" for s in slots if s.get("type")]
    return {"barony_title_id": h.get("barony_title_id"), "holding_type": h.get("holding_type"),
            "slots": h.get("building_slots_total", len(slots)), "built": built,
            "empty_slots": sum(1 for s in slots if not s.get("type"))}


def find_county(details: Any, title_id: int) -> tuple[Optional[dict], Optional[dict]]:
    """(county, holding) for a county id or one of its baronies' ids in query_county_details."""
    for c in (details or {}).get("counties") or [] if isinstance(details, dict) else []:
        if not isinstance(c, dict):
            continue
        if _i(c.get("title_id")) == title_id:
            return c, None
        for h in c.get("holdings") or []:
            if isinstance(h, dict) and _i(h.get("barony_title_id")) == title_id:
                return c, h
    return None, None


def candidates(all_names: list, built: list) -> list:
    """The level-1 buildings whose line is not built here, and the next level of each built one."""
    have = {}
    for b in built:
        m = _LEVEL.match(b)
        if m:
            have[m.group(1)] = int(m.group(2))
    known = set(all_names)
    out = []
    for n in all_names:
        m = _LEVEL.match(n)
        if not m:
            continue
        line, lvl = m.group(1), int(m.group(2))
        if (line not in have and lvl == 1) or (line in have and lvl == have[line] + 1):
            out.append(n)
    return [n for n in out if n in known]


def _reason(chk: dict) -> str:
    import redesign as R  # noqa: PLC0415
    if chk.get("success") is not True:
        return str(chk.get("reason") or chk.get("error") or "not checked")
    return R._build_refusal_summary(chk) or NO_REASON


async def buildings_reply(gw, me: int, title_id: int = 0, name_filter: str = "",
                          max_results: int = 0) -> tuple[Optional[dict], list]:
    """(data, errors) for query(domain="buildings")."""
    errors: list = []
    names = _names(await gw.read("query_building_types", {}))
    if names is None:
        return None, [{"code": "read_timeout", "detail": "query_building_types did not answer"}]
    frag = (name_filter or "").strip().lower()
    if frag:
        names_f = [n for n in names if frag in n.lower()]
    else:
        names_f = names
    if not title_id:
        shown = names_f[:BUILDING_NAMES_SHOWN]
        out = {"building_types": len(names), "names": shown,
               "how": ("pass title_id (a county or barony of yours) for its holdings' slots and what can be "
                       "built there with the price; filter narrows the names")}
        if frag:
            out["filter"] = frag
            out["matching"] = len(names_f)
        if len(names_f) > len(shown):
            out["truncated"] = len(names_f) - len(shown)
        return out, errors
    details = await gw.read("query_county_details", {"character_id": me})
    if not isinstance(details, dict) or details.get("error"):
        return None, [{"code": "read_timeout", "detail": "query_county_details did not answer"}]
    county, holding = find_county(details, title_id)
    if county is None:
        return {"title_id": title_id, "holdings": None,
                "why": ("not a county or barony of your own domain: query_county_details lists only the "
                        "counties you hold yourself")}, errors
    rows = [holding_row(h) for h in county.get("holdings") or [] if isinstance(h, dict)]
    target = holding_row(holding) if holding is not None else (rows[0] if rows else None)
    out: dict = {"county": {"title_id": county.get("title_id"), "key": county.get("key"),
                            "name": county.get("name")},
                 "holdings": rows,
                 "empty_slots": sum(r["empty_slots"] for r in rows)}
    if target is None:
        out["can_build"] = None
        out["why"] = "the county read lists no holding"
        return out, errors
    loc = _i(target.get("barony_title_id"))
    cand = candidates(names, target["built"])
    if frag:
        cand = [n for n in cand if frag in n.lower()]
    at = {"barony_title_id": loc, "holding_type": target.get("holding_type"),
          "empty_slots": target["empty_slots"],
          **({"chosen": "the county's first holding (its capital barony, as the bridge reads a county)"}
             if holding is None else {})}
    out["at"] = at
    if not cand or not loc:
        out["can_build"] = []
        out["checked"] = 0
        return out, errors
    chk = await gw.read("construction_check", {"actor_id": me, "location_id": loc, "buildings": cand})
    chk = chk if isinstance(chk, dict) else {}
    verdicts = {v["building"]: v for v in chk.get("rows") or [] if isinstance(v, dict) and v.get("building")}
    ok_rows, gold_rows, refused = [], [], {}
    for n in cand:
        v = verdicts.get(n) or chk
        call = {"tool": "do", "args": {"intent": "build", "args": {"building": n, "location_id": loc}}}
        if v.get("success") is True and v.get("buildable"):
            ok_rows.append({"building": n, "price": v.get("price"), "do_call": call})
            continue
        r = _reason(v)
        m = _GOLD_ONLY.match(r)
        if m:
            # a live test run: farm_estates_01 "Missing gold 66", hill_farms_01 "Missing gold 16" each made a
            # refusal kind of its own and fell past the kinds shown; the gold is their only gate
            gold_rows.append({"building": n, "price": v.get("price"), "gold_missing": int(m.group(1)),
                              "do_call": call})
            continue
        refused.setdefault(r, []).append(n)

    def gold(r):
        p = r.get("price") if isinstance(r.get("price"), dict) else {}
        g = p.get("gold")
        return g if isinstance(g, (int, float)) else 1e9
    ok_rows.sort(key=gold)
    cap = max_results if isinstance(max_results, int) and max_results > 0 else BUILDABLE_SHOWN
    out["can_build"] = ok_rows[:cap]
    gold_rows.sort(key=lambda r: r["gold_missing"])
    out["can_build_with_gold"] = gold_rows[:cap]
    if len(ok_rows) > cap or len(gold_rows) > cap:
        out["truncated"] = max(len(ok_rows) - cap, 0) + max(len(gold_rows) - cap, 0)
    kinds = sorted(refused.items(), key=lambda kv: -len(kv[1]))
    out["refused"] = [{"reason": k, "count": len(v), "e.g.": v[:3]} for k, v in kinds[:REFUSAL_KINDS_SHOWN]]
    if len(kinds) > REFUSAL_KINDS_SHOWN:
        out["refused_kinds_not_shown"] = len(kinds) - REFUSAL_KINDS_SHOWN
    out["checked"] = len(cand)
    if chk.get("success") is not True:
        errors.append({"code": "engine_unreachable",
                       "detail": f"construction_check did not answer: {chk.get('reason') or 'no reply'}"})
    return out, errors
