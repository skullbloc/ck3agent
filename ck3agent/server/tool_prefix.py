"""The name a client calls this server's tools by, in every hint a reply carries.

A reply names a tool to call next in two forms: a call object ({"tool": "do", "args": ...},
under `next`, `next_call`, `do_call`, `fix`, `lever` and the like) and prose
("re-verify via inspect(subject='council')"). The server names its tools bare. A client
that lists them under a prefix (Claude Code: `mcp__<server>__do`) refuses a bare name
(15 calls refused "No such tool available: observe"). The server cannot know the
client's prefix, so the host says it: CK3_TOOL_PREFIX (a harness's seat runner sets
`mcp__ck3__`). Unset or empty: every reply is unchanged, byte for byte.

One pass over the finished reply (mcp_server's call_tool), so every hint site is covered,
not a list of them. What it rewrites, and only that:
  * a dict below the reply's root whose "tool" is one of this server's tool names
    (the root's own "tool", and an embedded envelope's (a dict with "ok"), name a reply,
    not a call);
  * in a string, a tool name directly followed by "(" and not preceded by a word
    character, a dot or the prefix itself.
Bridge text (the `raw` reply, a script, the engine's result) is left as the game wrote it.
"""
from __future__ import annotations

import os
import re
from typing import Any, Iterable

ENV = "CK3_TOOL_PREFIX"
#: keys whose values are the bridge's or the game's own text, never a hint of this server
_VERBATIM = frozenset({"raw", "script", "engine_result", "said", "reject_reason"})


def prefix() -> str:
    return (os.environ.get(ENV) or "").strip()


def apply(reply: Any, names: Iterable[str], pre: str | None = None) -> Any:
    """`reply` with every hint naming one of `names` prefixed by `pre` (default: the
    environment's). The reply itself is returned unchanged when there is no prefix."""
    pre = prefix() if pre is None else pre
    names = frozenset(n for n in names if n)
    if not pre or not names:
        return reply
    alt = "|".join(sorted((re.escape(n) for n in names), key=len, reverse=True))
    prose = re.compile(r"(?<![\w.])(" + alt + r")\(")

    def text(s: str) -> str:
        return prose.sub(lambda m: (m.group(0) if s[max(0, m.start() - len(pre)):m.start()] == pre
                                    else pre + m.group(0)), s)

    def walk(o: Any, root: bool) -> Any:
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                if k in _VERBATIM:
                    out[k] = v
                elif k == "tool" and not root and "ok" not in o and isinstance(v, str) and v in names:
                    out[k] = pre + v
                else:
                    out[k] = walk(v, False)
            return out
        if isinstance(o, list):
            return [walk(v, False) for v in o]
        if isinstance(o, str):
            return text(o)
        return o
    return walk(reply, True)
