"""A seat's orders to its own ruler's armies, `do(intent="army")`.

Built from static evidence (re/findings/re_army_orders.md) and run on a live game
(disbands executed and the army gone at once, the game's
refusal reasons, a move the AI cleared within a day, a hold that kept the move for its days and
dropped the AI's orders, a release the AI took back the next day; no crash). OFF unless the
environment turns it on:

  CK3_BRIDGE_ARMY_ORDERS=1  disband, move and raid (the bridge's army_disband / army_move /
                            army_raid)
  CK3_BRIDGE_ARMY_HOLD=1    hold / release / holds (the bridge's army_hold, behind its
                            GameCommandDispatch hook)

The same two variables are read by the game process (the bridge) and by this server.
With both off (the default) `army` is not in do's intent list, the do description
does not change by a byte, and a call that names it anyway is refused here with
army_orders_off / army_hold_off before the bridge is asked.

  action=disband {army_id, check}         the game's own disband (CDisbandArmyCommand,
                                          the AI's src 2); check=true asks the game and
                                          sends nothing, and the refusal carries the game's
                                          reason (in combat, retreating, embarked, enemies
                                          around, hostile area, ...)
  action=move {army_id, title|province_id, check}
                                          the game's move order (CMoveUnitCommand); the
                                          reply says whether the army's path now ends there
  action=raid {army_id, on, check}        the army window's raid toggle (CToggleRaidingCommand,
                                          NOT RUN LIVE): on=true starts raiding, false
                                          stops it; sent only when the army is not already so; the
                                          reply reads the army's raiding flag back
  action=hold {army_id, days}             the AI's own move orders for that army are dropped
                                          for 1..365 days (it still fights and retreats)
  action=release {army_id} / action=holds
"""
from __future__ import annotations

import os
from typing import Any, Mapping, Optional

ARMY_INTENT = "army"
ENV_ORDERS = "CK3_BRIDGE_ARMY_ORDERS"
ENV_HOLD = "CK3_BRIDGE_ARMY_HOLD"
ARMY_ACTIONS = ("disband", "move", "raid", "hold", "release", "holds")
_ORDER_ACTIONS = ("disband", "move", "raid")
_HOLD_ACTIONS = ("hold", "release", "holds")
MAX_HOLD_DAYS = 365

#: The bridge's refusal codes (army_orders.cpp) and their gates; redesign's shared
#: table carries the same rows (test_army_orders.py checks the two agree).
REFUSALS = {
    "army_orders_off": "experimental_write_disabled",
    "army_hold_off": "experimental_write_disabled",
    "unsupported_os": "experimental_write_disabled",
    "army_id_required": "missing_required_argument",
    "army_not_found": "name_not_found",
    "not_your_army": "invalid_argument",
    "army_unit_unread": "engine_unreachable",
    "province_required": "missing_required_argument",
    "province_not_found": "name_not_found",
    "province_not_a_barony_or_county": "invalid_argument",
    "already_there": "already_in_that_state",
    "already_in_that_state": "already_in_that_state",
    "raid_on_required": "missing_required_argument",
    "in_combat": "engine_refused",
    "cannot_be_ordered_there": "engine_refused",
    "refused_by_game": "engine_refused",
    "days_required": "missing_required_argument",
    "bad_days": "invalid_argument",
    "date_unread": "engine_unreachable",
    "hold_table_full": "watchlist_full",
    "army_action_must_be_disband_move_hold_release_or_holds": "invalid_argument",
}

OFF_SUMMARY = {
    "army_orders_off": ("army orders are switched off in this build (CK3_BRIDGE_ARMY_ORDERS is not 1): "
                        "nothing was sent; the game's AI raises, moves and disbands your armies"),
    "army_hold_off": ("army holds are switched off in this build (CK3_BRIDGE_ARMY_HOLD is not 1): "
                      "nothing was held; the game's AI orders your armies"),
}


def _on(name: str, environ: Optional[Mapping[str, str]] = None) -> bool:
    env = os.environ if environ is None else environ
    return env.get(name) == "1"


def orders_on(environ=None) -> bool:
    return _on(ENV_ORDERS, environ)


def hold_on(environ=None) -> bool:
    return _on(ENV_HOLD, environ)


def listed(environ=None) -> bool:
    """`army` is in do's intent list only when one of the two switches is on."""
    return orders_on(environ) or hold_on(environ)


#: The args table's mark on an action no live run has proven. Raid is proven
#: (a test run, rows 9-10: check, apply off and on with the flag read back, and the game's refusal at war), so no
#: action is marked
UNPROVEN_LIVE: dict = {}


def listed_actions(environ=None) -> tuple:
    """The army actions do's args table lists: those of the switches that are on, an action not yet run on a live
    game marked so (UNPROVEN_LIVE); empty with both off."""
    acts = (_ORDER_ACTIONS if orders_on(environ) else ()) + (_HOLD_ACTIONS if hold_on(environ) else ())
    return tuple(UNPROVEN_LIVE.get(a, a) for a in acts)


def do_sentence(environ=None) -> str:
    """do's sentence on armies, true for the switches this server booted with."""
    tail = "and the game's declare-war refuses while they are raised. "
    if not listed(environ):
        return "Armies: the game's AI raises and disbands them, no tool does, " + tail
    parts = []
    if orders_on(environ):
        parts.append("army action=disband (check=true asks the game, which names why it refuses) or move to "
                     "a barony/county (the AI replaces a move within a day unless the army is held)")
    if hold_on(environ):
        parts.append("army action=hold drops the AI's own move orders for days=1..365")
    return ("Armies: the game's AI raises them; " + "; ".join(parts) + "; " + tail)


async def title_key_of(gw, value) -> tuple:
    """(key, titles_named row, blocked) for a move's title: a key resolves through title_refs (the
    resolver every title argument uses); an id is read back to its key (query_de_jure), which the
    bridge's army_move takes."""
    import title_refs  # noqa: PLC0415
    tid, row, blk = await title_refs.resolve(gw, value, "title")
    if blk is not None and blk.get("gate") == "read_timeout" and title_refs.is_title_key(value):
        # the key lookup did not answer: the bridge resolves a key itself (and refuses one it lacks)
        return str(value).strip(), None, None
    if blk is not None:
        return None, None, blk
    if row is not None:
        return row["title_key"], row, None
    if not isinstance(tid, int) or tid <= 0:
        return None, None, _R().why_blocked("invalid_argument", summary=(
            f"title={value!r} is neither a title key nor a title id; nothing was sent"))
    raw = await gw.read("query_de_jure", {"title_id": tid})
    t = raw.get("title") if isinstance(raw, dict) else None
    key = t.get("key") if isinstance(t, dict) else None
    if not raw:
        return None, None, _R().why_blocked("read_timeout", summary=(
            f"title={value!r}: the bridge did not answer the title read; nothing was sent"))
    if not title_refs.is_title_key(key):
        return None, None, _R().why_blocked("target_not_assignable", raw_reason="title_id_not_found", summary=(
            f"title={value!r}: the game has no landed title with that id; nothing was sent"))
    return key, {"arg": "title", "title_key": key, "title_id": tid, "name": t.get("name")}, None


def _R():
    import redesign  # noqa: PLC0415 — redesign imports this module
    return redesign


def _d(x) -> dict:
    return x if isinstance(x, dict) else {}


def _int(v) -> Optional[int]:
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str) and v.strip().lstrip("-").isdigit():
        return int(v.strip())
    return None


#: A recorded game (1068.03-12) refused twelve disbands "hostile Armies are around" while no read named
#: whose. Where the game's disband check looks (ArmyDisbandCore: the army's county and the counties next to it,
#: every unit of a ruler hostile to you there; re/findings/re_armies_around.md)
AROUND_RULE = ("the game counts any unit of a ruler hostile to you (at war with you, or raiders) in your army's "
               "county or a county next to it, wherever that is: move away from them, then disband")
AROUND_UNPROVEN = "built, not proven live"


#: The game's own refusal words that name their cause, and that cause in full (the hostile-army rule is only
#: for the game's "hostile Armies are around")
GAME_REASON_WHY = (
    ("retreat", "the army is retreating after a lost battle: the game refuses to disband an army while it retreats; "
                "when it stops at the end of the retreat (inspect armies) it can be disbanded"),
)


def game_reason_why(reason: Any) -> Optional[str]:
    low = str(reason or "").lower()
    return next((w for needle, w in GAME_REASON_WHY if needle in low), None)


def around_words(raw: Mapping[str, Any]) -> Optional[str]:
    """The other rulers' armies the bridge read where the game's disband check looks, in words; None when the
    reply carries no such read (an older bridge)."""
    rows = raw.get("around")
    if rows == "unread":
        return "the county around your army was not read"
    if not isinstance(rows, list):
        return None
    if not rows:
        return "no other ruler's army stands in your army's county or next to it now"
    cells = [f"army {r.get('army_id', '?')} of {r.get('owner_id')} ({r.get('men')} men) at "
             f"{r.get('barony') or r.get('province_id')}" for r in rows if isinstance(r, Mapping)]
    more = _int(raw.get("around_total"))
    return ("other rulers' armies there: " + "; ".join(cells)
            + (f" (and {more - len(cells)} more)" if more is not None and more > len(cells) else ""))


async def position_of(gw, me: Optional[int], army_id: Optional[int]) -> Optional[str]:
    """Where the army stands now ("<location province>:<path steps>"), None when unread or gone."""
    if not me or army_id is None:
        return None
    raw = _d(await gw.read("query_armies", {"character_id": me}))
    for r in raw.get("armies") or []:
        if isinstance(r, Mapping) and _int(r.get("army_id")) == army_id:
            loc = _int(r.get("location_province_id"))
            return f"{loc}:{_int(r.get('path_steps'))}" if loc else None
    return None


async def moved_since(gw, env: Mapping[str, Any]) -> bool:
    """("the tool won't resend the same order within one game month" after the army had
    moved): True when the army of a remembered refusal stands elsewhere now, or either reading is missing (the game
    is asked again rather than answered from memory)."""
    res = _d(env.get("result"))
    then = res.get("army_at")
    now = await position_of(gw, _int(_d(env.get("actor")).get("id")), _int(_d(res.get("raw")).get("army_id"))
                            or _int(res.get("army_id")))
    return then is None or now is None or now != then


async def do_army(gw, a: dict, character_id: int, actor, environ=None) -> dict[str, Any]:
    R = _R()
    me = character_id or R._as_int((actor or {}).get("id"))
    action = str(a.get("action") or "")
    army_id = _int(a.get("army_id"))

    def refuse(code: str, summary: str, fields=None, result=None):
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate=REFUSALS.get(code, "invalid_argument"), raw_reason=code,
                                                  summary=summary,
                                                  fix={"rejected_fields": fields} if fields else None),
                          bridge_health=gw.health)

    if action not in ARMY_ACTIONS:
        return refuse("army_action_must_be_disband_move_hold_release_or_holds",
                      f"args.action is one of {', '.join(ARMY_ACTIONS)}", ["action"])
    if action in _ORDER_ACTIONS and not orders_on(environ):
        return refuse("army_orders_off", OFF_SUMMARY["army_orders_off"])
    if action in _HOLD_ACTIONS and not hold_on(environ):
        return refuse("army_hold_off", OFF_SUMMARY["army_hold_off"])
    if action != "holds" and army_id is None:
        return refuse("army_id_required", "args.army_id is one of inspect(subject='armies') rows' army_id",
                      ["army_id"])

    params: dict = {"actor_id": me}
    echo: list = []
    if army_id is not None:
        params["army_id"] = army_id
    if action == "disband":
        params["check"] = a.get("check") is True
        verb = "army_disband"
    elif action == "move":
        pid = _int(a.get("province_id"))
        title = a.get("title")
        if pid is not None:
            params["province_id"] = pid
        elif title is not None and not isinstance(title, bool) and str(title).strip():
            # A title takes a
            # key or an id, as every title argument does (title_refs); the bridge takes the key
            key, row, blk = await title_key_of(gw, title)
            if blk is not None:
                return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                                  blocked=blk, bridge_health=gw.health)
            params["title"] = key
            if row is not None:
                echo.append(row)
        else:
            return refuse("province_required", "args.title (a barony or county key or id) or args.province_id",
                          ["title", "province_id"])
        params["check"] = a.get("check") is True
        verb = "army_move"
    elif action == "raid":
        on = a.get("on")
        if not isinstance(on, bool):
            return refuse("raid_on_required", "args.on is true (start raiding) or false (stop)", ["on"])
        params["on"] = 1 if on else 0
        params["check"] = a.get("check") is True
        verb = "army_raid"
    else:
        params["op"] = action
        if action == "hold":
            days = _int(a.get("days"))
            if days is None:
                return refuse("days_required", f"args.days is 1..{MAX_HOLD_DAYS}", ["days"])
            if not 1 <= days <= MAX_HOLD_DAYS:
                return refuse("bad_days", f"args.days is 1..{MAX_HOLD_DAYS}", ["days"])
            params["days"] = days
        verb = "army_hold"

    reply = R._unwrap_reply(await (gw.read(verb, params) if action == "holds" else gw.write(verb, params)))
    raw = _d(reply)
    result = {"intent": ARMY_INTENT, "action": action, "army_id": army_id, "raw": raw}
    if raw.get("success") is not True:
        code = str(raw.get("reason") or raw.get("error") or "no_reply")
        summary = f"the bridge refused: {code}"
        if code in OFF_SUMMARY:
            summary = OFF_SUMMARY[code]
        elif code == "refused_by_game" and raw.get("game_reason"):
            summary = f"the game refused: {raw.get('game_reason')}"
        elif code == "refused_by_game":
            summary = f"the game refused: {R.NO_GAME_DETAIL}" if hasattr(R, "NO_GAME_DETAIL") else summary
        words = around_words(raw)
        why = game_reason_why(raw.get("game_reason"))
        if why is not None:
            # "Cannot disband while in Retreat" is explained by the hostile-army rule too
            summary += f". {why}"
        elif words is not None:
            # Whose armies the game's check counts, and the rule
            summary += f". {words}. {AROUND_RULE} ({AROUND_UNPROVEN})"
        if echo:
            result["titles_named"] = echo
        if action in _ORDER_ACTIONS:
            # Where the army stood when refused; a repeat is answered from memory only while it stands there
            result["army_at"] = await position_of(gw, me, army_id)
        return refuse(code, summary, result=result)
    if action in ("disband", "move", "raid") and raw.get("check") is True:
        result["checked_only"] = True
    if echo:
        result["titles_named"] = echo
    return R.envelope("do", actor=actor, game=gw.game(), bridge_health=gw.health, result=result)
