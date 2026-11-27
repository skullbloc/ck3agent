"""A title named by key or by id, wherever a tool takes a title id.

Every title-taking argument on the surface was an integer, and nothing turned
`c_dublin` into one: a seat that read a key in one reply could not pass it to the
next call. `resolve` turns a key into the id the bridge takes, through the bridge's
own key lookup (query_de_jure with title_key: resolve_title_id_by_key, the
LANDED_TITLE_DB scan create_title and the de-jure meter use), and returns the key
and the game's name beside the id so the reply can echo them.

An integer, or a string of digits, is an id and is passed through unread. A key the
game does not have is refused `target_not_assignable` (raw_reason title_key_not_found), never sent as 0.
"""
from __future__ import annotations

import re
from typing import Any, Optional

#: A landed title key: the tier letter, an underscore, then the game's own spelling.
TITLE_KEY_RE = re.compile(r"^[bcdkeh]_[A-Za-z0-9_\-']+$")

#: The argument names, in a tool's arguments or in do's `args`, that carry a title.
TITLE_ARGS = ("title_id", "cb_title_id", "target_title_id", "location_id")


def is_title_key(v: Any) -> bool:
    return isinstance(v, str) and bool(TITLE_KEY_RE.match(v.strip()))


def _blocked(gate: str, summary: str, **kw) -> dict:
    import redesign
    return redesign.why_blocked(gate, summary=summary, **kw)


async def resolve(gw, value: Any, arg: str = "title_id") -> tuple[Optional[int], Optional[dict], Optional[dict]]:
    """(title_id, echo, blocked). echo is {arg, title_key, title_id, name} for a key
    that resolved, None for a value passed as an id. blocked is a why_blocked."""
    if value is None or isinstance(value, bool):
        return None, None, None
    if isinstance(value, int):
        return value, None, None
    if isinstance(value, float) and value.is_integer():
        return int(value), None, None
    s = str(value).strip()
    if s == "":
        return 0, None, None
    if re.fullmatch(r"-?\d+", s):
        return int(s), None, None
    if not is_title_key(s):
        return None, None, _blocked(
            "invalid_argument",
            f"{arg}={value!r} is neither a title id nor a title key (b_/c_/d_/k_/e_/h_ then the "
            "game's spelling, e.g. 'c_dublin'); nothing was sent")
    raw = await gw.read("query_de_jure", {"title_key": s})
    title = raw.get("title") if isinstance(raw, dict) else None
    tid = title.get("title_id") if isinstance(title, dict) else None
    if not raw:
        # nobody answered: not "no such title"
        return None, None, _blocked(
            "read_timeout", f"{arg}={s!r}: the bridge did not answer the key lookup; nothing was sent")
    if not isinstance(tid, int) or isinstance(tid, bool) or tid <= 0:
        why = raw.get("error") if isinstance(raw, dict) and raw.get("error") else "no title in the reply"
        return None, None, _blocked(
            "target_not_assignable",
            f"{arg}={s!r}: the game has no landed title with that key ({why}); nothing was sent",
            raw_reason=str(why))
    return tid, {"arg": arg, "title_key": title.get("key") or s, "title_id": tid,
                 "name": title.get("name")}, None


async def resolve_args(gw, args: Optional[dict], keys=TITLE_ARGS) -> tuple[dict, list, Optional[dict]]:
    """Resolve every title argument in `args` that is a key. (args with ids, echo
    rows, blocked). The first key that does not resolve refuses the call."""
    out = dict(args or {})
    echo: list = []
    for k in keys:
        if k not in out or not isinstance(out[k], str):
            continue
        tid, row, blk = await resolve(gw, out[k], k)
        if blk is not None:
            return out, echo, blk
        out[k] = tid
        if row is not None:
            echo.append(row)
    return out, echo, None


def attach(env: Any, echo: list) -> Any:
    """Put the resolved keys beside the reply: result.titles_named (one row each)."""
    if echo and isinstance(env, dict):
        res = env.get("result")
        if isinstance(res, dict):
            res["titles_named"] = echo
        elif res in (None, {}, []):
            env["result"] = {"titles_named": echo}
    return env
