"""refusal_memo.py — a write the game refused is not sent again while nothing has changed.

THE HAZARD. A king's seat sent 12 war declarations the game refused
(`cb_invalid_for_target` / `can_declare_war_false` on William 31808, 30162 and
63899), some identical three times (title 7127 x3, 7146 x3, 7808 x3), and another
king re-sent refused white-peace and call-ally offers. Every one reached the
game again and was refused again. plan_war kept offering several of them.

THE RULE (keyed on the game-month). A write whose reply was
`verdict: rejected` with a gate of a class the GAME decides (`GAME_CLASSES`: the
target's state, the actor's resources, the territory) is remembered with the
MOMENT it was refused at: the game tick the reply carries (its date when no tick
was read), and how many writes of this server had applied by then. The same
write (tool, actor, canonical arguments) asked again is answered with the
earlier refusal, marked `repeat_of`, and does not reach the game, only while
nothing the refusal depends on can have changed: the same tick (the game has not
run a day; a seat's turn is played paused) and no write of this server applied
or pending since (the seat's own act may have changed what the game checks).
plan_war moves a declaration it would offer that the game refused at this moment
into `excluded` with the gate `refused_unchanged`.

The disband refused on 0869.12.06 ("in
Retreat", tick 1069) was answered from memory at tick 1083, two weeks later,
right after a check=true call said the game would now allow it; the month had not
turned. A check (args.check true) is a read: never answered from memory and never
remembered.

NOT REMEMBERED: a refusal the bridge or the server made (class `bridge`,
`timing`, `pause`, ...), a pending or unknown verdict, an applied write, a check.

The memory is the server's (one per seat server); a seat's resumed claude starts
a new server, and with it an empty memory. One definition: the key and the moment
are computed here and read by declare_war, do and plan_war alike.
"""
from __future__ import annotations

import copy
import json
from typing import Any, Mapping, Optional

#: The gate classes whose refusals are the game's answer about the world.
GAME_CLASSES = frozenset({"target_state", "resource", "territory"})
#: plan_war's exclusion gate for a declaration the game refused at this moment (nothing changed since).
GATE_REFUSED_UNCHANGED = "refused_unchanged"


def moment(game: Any) -> Optional[tuple]:
    """The game's moment from an envelope's `game` block (or a bare date string): ("tick", n) when a tick
    was read, else ("date", "YYYY.MM.DD"); None when neither was read (then nothing is answered from memory)."""
    if isinstance(game, Mapping):
        t = game.get("tick")
        if isinstance(t, int) and not isinstance(t, bool):
            return ("tick", t)
        game = game.get("date")
    if isinstance(game, str):
        parts = game.split(".")
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            return ("date", f"{int(parts[0]):04d}.{int(parts[1]):02d}.{int(parts[2]):02d}")
    return None


def may_have_changed(env: Any) -> bool:
    """A write's reply that may have changed the game's state: anything but a refusal (verdict rejected, or
    ok false). Fails open: a reply without a verdict (the army replies carry none: turn 19's applied
    disband) counts as a change, so a remembered refusal is asked again rather than repeated."""
    if not isinstance(env, Mapping):
        return True
    return env.get("verdict") != "rejected" and env.get("ok") is not False


def declare_key(actor: int, args: Mapping[str, Any]) -> tuple:
    """The identity of one declaration: actor, target, cb, cb title, claimant
    (null and 0 are the same claimant, as declare_war reads them)."""
    return ("declare_war", _ident(actor), _ident(args.get("target_id")), str(args.get("cb") or ""),
            _ident(args.get("cb_title_id")), _ident(args.get("claimant_id")))


def _ident(v: Any) -> Any:
    """An id as the key holds it: an integer (null and 0 are 0), or the text as sent
    when it is not one (a title key such as "c_athlone": in a live 1075.09.15 read the
    int() of it raised before declare_war ran)."""
    if v is None or v == "" or isinstance(v, bool):
        return 0
    try:
        return int(v)
    except (TypeError, ValueError):
        return str(v).strip()


def do_key(actor: int, intent: Optional[str], args: Any) -> Optional[tuple]:
    """The identity of one `do` write: actor, intent, its arguments (canonical JSON). None for a check
    (args.check true: a read the game answers from its state now, never from memory)."""
    if isinstance(args, Mapping) and args.get("check") is True:
        return None
    return ("do", int(actor or 0), str(intent or ""), json.dumps(args or {}, sort_keys=True, default=str))


#: The intents whose refusal is about where the thing stands now; a repeat is answered from memory
#: only while it has not moved (mcp_server asks army_orders.moved_since)
MOVING_INTENTS = frozenset({"army"})


def game_refusal(env: Any) -> bool:
    """The reply is a write the GAME refused (see the module doc)."""
    if not isinstance(env, Mapping) or env.get("verdict") != "rejected":
        return False
    b = env.get("blocked")
    return isinstance(b, Mapping) and b.get("class") in GAME_CLASSES


class Memo:
    """The refused writes of this server, by key, with the moment they were refused at."""

    def __init__(self) -> None:
        self._rows: dict = {}
        self._calls = 0
        #: writes of this server that applied (or were queued to apply): a refusal older than one is not repeated
        self._changes = 0

    def note(self, key: tuple, env: Any, game: Any) -> None:
        """Remember `env` if it is a game refusal at a readable moment; count it if it may have changed the
        game's state (may_have_changed). `game` is the reply's `game` block (or its date)."""
        self._calls += 1
        if may_have_changed(env):
            self._changes += 1
            return
        at = moment(game)
        if at is None or not game_refusal(env):
            return
        date = game.get("date") if isinstance(game, Mapping) else game
        self._rows[key] = {"at": at, "game_date": date, "call": self._calls, "changes": self._changes,
                           "env": copy.deepcopy(dict(env))}

    def earlier(self, key: tuple, game: Any) -> Optional[dict]:
        """The earlier refusal of this write if nothing has changed since: the same moment (`game`: the
        current `game` block, or its date) and no write of this server applied since; else None."""
        row = self._rows.get(key)
        now = moment(game)
        if row is None or now is None or now != row["at"] or row["changes"] != self._changes:
            return None
        return row

    def repeat_reply(self, key: tuple, game: Any) -> Optional[dict]:
        """The reply to a repeat: the earlier refusal, marked `repeat_of`, with a
        warning; None when this write was not refused at this moment."""
        row = self.earlier(key, game)
        if row is None:
            return None
        env = copy.deepcopy(row["env"])
        # THIS call reached nothing (live, once: the replay carried
        # the earlier call's `sent: true` and token beside "not sent"): the result
        # says so, and what the earlier call did is kept under repeat_of.
        res = env.get("result")
        earlier = {"token": None, "sent": "not_stated"}
        if isinstance(res, dict):
            res = env["result"] = dict(res)
            earlier["token"] = res.get("token")
            if "token" in res:
                res["token"] = None
            if "sent" in res:
                earlier["sent"] = res["sent"]
                res["sent"] = False
        env["repeat_of"] = {"game_date": row["game_date"], "call": row["call"], "earlier": earlier,
                            "rule": REPEAT_RULE}
        warnings = list(env.get("warnings") or [])
        warnings.append({"code": "repeat_of_refused_write",
                         "detail": f"identical to a write the game refused on {row['game_date']}, and no game day "
                                   "has passed and no write of yours applied since; not sent. It is sent again "
                                   "once either happens (args.check=true asks the game now)."})
        env["warnings"] = warnings
        return env


#: the rule, as a repeat's reply states it
REPEAT_RULE = ("the game refused this exact write at this game tick and no write of yours applied since; "
               "not sent again")
