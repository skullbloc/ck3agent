"""refusal_memo.py — a write the game refused this game-month is not sent again (fix10).

THE DEFECT (v4 pilot 2). COUNCIL's king sent 12 war declarations the game refused
(`cb_invalid_for_target` / `can_declare_war_false` on William 31808, 30162 and
63899), some identical three times (title 7127 x3, 7146 x3, 7808 x3), and K1's
king re-sent refused white-peace and call-ally offers. Every one reached the
game again and was refused again. plan_war kept offering several of them.

THE RULE. A write whose reply was `verdict: rejected` with a gate of a class the
GAME decides (`GAME_CLASSES`: the target's state, the actor's resources, the
territory) is remembered with the game-month it was refused in. The same write
(tool, actor, canonical arguments) asked again in the same game-month is answered
with the earlier refusal, marked `repeat_of`, and does not reach the game.
plan_war moves a declaration it would offer that the game refused this month
into `excluded` with the gate `refused_this_month`.

NOT REMEMBERED: a refusal the bridge or the server made (class `bridge`,
`timing`, `pause`, ...), a pending or unknown verdict, an applied write. A new
game-month clears the memory for that write: the game's answer may change.

The memory is the server's (one per seat server); a seat's resumed claude starts
a new server, and with it an empty memory. One definition: the key and the month
are computed here and read by declare_war, do and plan_war alike.
"""
from __future__ import annotations

import copy
import json
from typing import Any, Mapping, Optional

#: The gate classes whose refusals are the game's answer about the world.
GAME_CLASSES = frozenset({"target_state", "resource", "territory"})
#: plan_war's exclusion gate for a declaration the game refused this month.
GATE_REFUSED_THIS_MONTH = "refused_this_month"


def game_month(date: Any) -> Optional[str]:
    """"1070.08.26" -> "1070.08"; None when the date is not a game date."""
    if not isinstance(date, str):
        return None
    parts = date.split(".")
    if len(parts) < 2 or not parts[0].isdigit() or not parts[1].isdigit():
        return None
    return f"{int(parts[0])}.{int(parts[1]):02d}"


def declare_key(actor: int, args: Mapping[str, Any]) -> tuple:
    """The identity of one declaration: actor, target, cb, cb title, claimant
    (null and 0 are the same claimant, as declare_war reads them)."""
    return ("declare_war", int(actor or 0), int(args.get("target_id") or 0), str(args.get("cb") or ""),
            int(args.get("cb_title_id") or 0), int(args.get("claimant_id") or 0))


def do_key(actor: int, intent: Optional[str], args: Any) -> tuple:
    """The identity of one `do` write: actor, intent, its arguments (canonical JSON)."""
    return ("do", int(actor or 0), str(intent or ""), json.dumps(args or {}, sort_keys=True, default=str))


def game_refusal(env: Any) -> bool:
    """The reply is a write the GAME refused (see the module doc)."""
    if not isinstance(env, Mapping) or env.get("verdict") != "rejected":
        return False
    b = env.get("blocked")
    return isinstance(b, Mapping) and b.get("class") in GAME_CLASSES


class Memo:
    """The refused writes of this server, by key, with the month they were refused in."""

    def __init__(self) -> None:
        self._rows: dict = {}
        self._calls = 0

    def note(self, key: tuple, env: Any, date: Any) -> None:
        """Remember `env` if it is a game refusal with a readable game date."""
        self._calls += 1
        month = game_month(date)
        if month is None or not game_refusal(env):
            return
        self._rows[key] = {"month": month, "game_date": date, "call": self._calls,
                           "env": copy.deepcopy(dict(env))}

    def earlier(self, key: tuple, date: Any) -> Optional[dict]:
        """The earlier refusal of this write in the month of `date`, or None."""
        row = self._rows.get(key)
        month = game_month(date)
        if row is None or month is None or row["month"] != month:
            return None
        return row

    def repeat_reply(self, key: tuple, date: Any) -> Optional[dict]:
        """The reply to a repeat: the earlier refusal, marked `repeat_of`, with a
        warning; None when this write was not refused this month."""
        row = self.earlier(key, date)
        if row is None:
            return None
        env = copy.deepcopy(row["env"])
        # THIS call reached nothing (live qf1 Q3s: the replay carried
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
                            "rule": "the game refused this exact write this game-month; not sent again"}
        warnings = list(env.get("warnings") or [])
        warnings.append({"code": "repeat_of_refused_write",
                         "detail": f"identical to a write the game refused on {row['game_date']}; "
                                   "not sent. The game's answer can change next month, or after the "
                                   "blocking condition clears."})
        env["warnings"] = warnings
        return env
