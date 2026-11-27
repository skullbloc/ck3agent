"""The task's goal kind, the one source for what a seat's observations offer.

Under a goal that forbids offensive war (`line_kept_peacefully`: no ruler of the line may declare an
offensive war) a recorded run's observations still carried conquest material on most steps: the ruler change briefing's k_bavaria
creation price and its conquest-perk note, `map_changes` of the k_bavaria region on 21 steps, the
`idle.no_fabrication` / `idle.claims_no_war` calls, and claims and fabrication rows saying "declare before then".

Whoever starts this server (a harness, from its task's goal) names the goal kind in CK3_GOAL_KIND. Under a goal that
forbids offensive war (PEACEFUL_GOALS) the news leaves out the goal-title and creation lines and the calls that offer
a war or a claim to war on, and keeps the facts without a call. Unset, or any other goal: nothing changes.
"""
from __future__ import annotations

import os
import re
from typing import Any, Optional

GOAL_KIND_ENV = "CK3_GOAL_KIND"
#: the goals scored on no offensive war
PEACEFUL_GOALS = frozenset({"line_kept_peacefully"})
#: the plain fact of an unpressed claim's end, without "declare before then"
CLAIM_DIES_FACT = "lost when you die: an unpressed claim does not pass to your heir"
#: the action classes and tools a call offers that lead to a war or a claim to war on
WAR_CALLS = frozenset({"fabricate_targets", "cbs", "plan_war", "declare_war"})


def goal_kind() -> Optional[str]:
    """The task's goal kind (CK3_GOAL_KIND), or None."""
    k = (os.environ.get(GOAL_KIND_ENV) or "").strip()
    return k if re.fullmatch(r"[a-z][a-z0-9_]*", k) else None


def peaceful() -> bool:
    """The task's goal forbids an offensive war."""
    return goal_kind() in PEACEFUL_GOALS


def _offers_war(call: Any) -> bool:
    if not isinstance(call, dict):
        return False
    tool = str(call.get("tool") or "").rsplit("__", 1)[-1]
    args = call.get("args") if isinstance(call.get("args"), dict) else {}
    return tool in WAR_CALLS or args.get("action_class") in WAR_CALLS or \
        (args.get("intent") == "council_task" and "fabricate" in str((args.get("args") or {}).get("task") or ""))


def _plain_claim(row: Any) -> Any:
    if not isinstance(row, dict):
        return row
    out = {k: v for k, v in row.items() if k not in ("next", "war")}
    if "on_your_death" in out:
        out["on_your_death"] = CLAIM_DIES_FACT
    return out


def news(out: dict) -> dict:
    """The news block under a peaceful goal: the goal-title and creation lines left out, the war-offering calls
    dropped, the facts kept. Any other goal: `out` unchanged (the same object)."""
    if not peaceful() or not isinstance(out, dict):
        return out
    out.pop("map_changes", None)                     # the region of the goal kingdom a conquest would build
    idle = out.get("idle")
    if isinstance(idle, dict):
        idle.pop("no_fabrication", None)
        for k in ("fabrication_overdue", "fabrication_reset"):
            idle.pop(k, None)
        cnw = idle.get("claims_no_war")
        if isinstance(cnw, dict):
            idle["claims_no_war"] = {k: v for k, v in cnw.items() if k != "next"}
        if not idle:
            out.pop("idle")
    if isinstance(out.get("claims_gained"), list):
        out["claims_gained"] = [_plain_claim(r) for r in out["claims_gained"]]
    fab = out.get("fabrication")
    if isinstance(fab, dict):
        fab.pop("next_call", None)
        cim = fab.get("claim_it_makes")
        if isinstance(cim, dict) and "on_your_death" in cim:
            cim["on_your_death"] = CLAIM_DIES_FACT
    for r in out.get("council_changed") or []:
        if isinstance(r, dict) and _offers_war(r.get("next")):
            r["next"] = {"tool": "inspect", "args": {"subject": "council"}}
    rc = out.get("ruler_changed")
    br = rc.get("briefing") if isinstance(rc, dict) else None
    if isinstance(br, dict):
        for k in ("price", "perk_note", "casus_belli"):
            br.pop(k, None)
    return out


def observe(result: dict) -> dict:
    """observe's result under a peaceful goal: de_jure.next_title (the kingdom to create and its call) left out."""
    if not peaceful() or not isinstance(result, dict):
        return result
    dj = result.get("de_jure")
    if isinstance(dj, dict):
        dj.pop("next_title", None)
    return result
