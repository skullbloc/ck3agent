"""The `do` tool's per-intent argument table, READ FROM THE CODE.

`do.args` was described only as "intent-specific keys (see SPEC §2)", a document
the seat cannot open, and seats learned the calling convention by refusal (396
missing_required_argument in the measured corpus). `_intent_to_act_params` in
redesign.py is the function that turns `do(intent, args)` into the bridge's
parameters, so WHAT IT READS OFF `args` IS THE ARGUMENT LIST, by construction.
This module extracts the table from that function's AST, and mcp_server.py
appends `table_text()` to the registered `do` description at import time: the
description an MCP client is sent cannot drift from the code because it is
computed from it. (The block once lived in a hand-kept schema
in tools.py that only the retired API loop read; MCP seats never received it.)

READ RATHER THAN GUESSED: a key read as `a.get("k")` is listed; nothing here
infers whether a key is MANDATORY: the bridge decides that and answers with
`*_required` codes.
"""
from __future__ import annotations

import ast
import pathlib
import re

REDESIGN = pathlib.Path(__file__).resolve().parent / "redesign.py"
FUNC = "_intent_to_act_params"
MARKER_OPEN = "ARGUMENTS BY INTENT (read from the code)"

#: Intents served by their own function rather than `_intent_to_act_params`:
#: intent -> (module, the functions that read `args`, the name `args` has there).
#: The same rule applies — what the function reads off its args IS the list.
DEDICATED: dict = {
    "faction": (pathlib.Path(__file__).resolve().parent / "factions.py",
                ("do_faction", "_respond_demand"), "args"),
    "set_policy": (REDESIGN, ("_do_set_policy",), "a"),
    "answer_proposal": (REDESIGN, ("_do_answer_proposal",), "a"),
    "end_war": (REDESIGN, ("_do_end_war",), "a"),
    "event_rule": (pathlib.Path(__file__).resolve().parent / "seat_events.py", ("do_event_rule",), "a"),
    "gold_reserve": (pathlib.Path(__file__).resolve().parent / "seat_events.py", ("do_gold_reserve",), "a"),
    "ai_veto": (pathlib.Path(__file__).resolve().parent / "seat_events.py", ("do_ai_veto",), "a"),
    "host_activity": (pathlib.Path(__file__).resolve().parent / "activities.py", ("do_host_activity",), "a"),
}
# Listed only when the server booted with an army switch on (army_orders.listed)
import army_orders as _army_orders  # noqa: E402
if _army_orders.listed():
    DEDICATED["army"] = (pathlib.Path(__file__).resolve().parent / "army_orders.py", ("do_army",), "a")


def _arg_keys(node: ast.AST, var: str = "a") -> list[tuple[str, bool]]:
    """Every `<var>.get("k")` under `node`, in source order, with an int flag."""
    out: list[tuple[str, bool]] = []
    for n in ast.walk(node):
        if not (isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                and n.func.attr == "get"
                and isinstance(n.func.value, ast.Name) and n.func.value.id == var):
            continue
        if not (n.args and isinstance(n.args[0], ast.Constant)
                and isinstance(n.args[0].value, str)):
            continue
        out.append((n.args[0].value, False))
    return out


def _declared_intents(src: str) -> list[str]:
    """The keys of INTENT_TO_ACT plus the three dedicated-branch intents."""
    tree = ast.parse(src)
    out: list[str] = []
    for n in ast.walk(tree):
        # AnnAssign, not Assign: the declaration is annotated
        # (`INTENT_TO_ACT: dict[str, dict[str, Any]] = {...}`), and matching only
        # ast.Assign silently found nothing — which rendered an EMPTY table that
        # looked like a clean result rather than a broken extractor.
        name = None
        value = None
        if isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name):
            name, value = n.target.id, n.value
        elif isinstance(n, ast.Assign) and len(n.targets) == 1 \
                and isinstance(n.targets[0], ast.Name):
            name, value = n.targets[0].id, n.value
        if name == "INTENT_TO_ACT" and isinstance(value, ast.Dict):
            out += [k.value for k in value.keys
                    if isinstance(k, ast.Constant) and isinstance(k.value, str)]
    for m in re.finditer(r'^_(?:[A-Z_]+)_INTENT\s*=\s*"([a-z_]+)"', src, re.M):
        out.append(m.group(1))
    if not out:
        raise SystemExit("INTENT_TO_ACT is absent — the intent declaration has "
                         "moved and this table cannot be filtered honestly")
    return out


def extract(src: str) -> dict[str, list[str]]:
    """{intent: [keys]} from the dispatch function's own branches."""
    tree = ast.parse(src)
    fn = next((n for n in ast.walk(tree)
               if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
               and n.name == FUNC), None)
    if fn is None:
        raise SystemExit(f"{FUNC} is absent from {REDESIGN} — the generator's "
                         "source has moved and this table cannot be trusted")
    # FILTERED AGAINST THE DECLARED INTENTS, because an `if` test inside this
    # function can compare a non-intent string (`scope == "title"`) and a walk
    # would otherwise invent `scope` and `title` as intents. Enumerate from the
    # declaration, never from whatever the walk happened to find.
    declared = set(_declared_intents(src))
    table: dict[str, list[str]] = {}
    for n in ast.walk(fn):
        if not isinstance(n, ast.If):
            continue
        intents = [c.value for c in ast.walk(n.test)
                   if isinstance(c, ast.Constant) and isinstance(c.value, str)]
        if not intents:
            continue
        keys: list[str] = []
        for b in n.body:
            for k, _ in _arg_keys(b):
                if k not in keys and k != "actor_id":
                    keys.append(k)
        for i in intents:
            if keys and i in declared:
                table.setdefault(i, [])
                for k in keys:
                    if k not in table[i]:
                        table[i].append(k)
    return table


def extract_dedicated() -> dict[str, list[str]]:
    """{intent: [keys]} for the DEDICATED intents, read from their own functions.
    A listed function that is gone is an error, never an empty row."""
    table: dict[str, list[str]] = {}
    for intent, (path, funcs, var) in DEDICATED.items():
        tree = ast.parse(path.read_text(encoding="utf-8"))
        found = {n.name: n for n in ast.walk(tree)
                 if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name in funcs}
        missing = set(funcs) - set(found)
        if missing:
            raise SystemExit(f"{sorted(missing)} absent from {path.name}: the args of "
                             f"intent {intent!r} cannot be read")
        keys: list[str] = []
        for f in funcs:
            for k, _ in _arg_keys(found[f], var):
                if k not in keys:
                    keys.append(k)
        table[intent] = keys
    return table


#: (a seated game, step 48: make_peace {"terms": "surrender", "check": true} ran the surrender;
#: `check` is end_war's): every intent REFUSES an argument it does not read. The accepted names are
#: the listed table's, plus the intents do_impl answers that the listing leaves out (read from their
#: own functions by the same rule), plus PATH_EXTRA: a key a guard reads before the intent's own
#: function (test_do_intent_census walks the do path's call graph and reds on a read key no intent accepts).
ACCEPT_ONLY: dict = {
    "cancel_pending": (REDESIGN, ("_do_cancel_pending",), "a"),
    "army": (pathlib.Path(__file__).resolve().parent / "army_orders.py", ("do_army",), "a"),
}
PATH_EXTRA: dict = {"interact": ("confirm_leaves_realm",)}
_ACCEPTED: dict = {}


def accepted() -> dict[str, tuple]:
    """{intent: (accepted arg names, in the listing's order)} for every intent do_impl answers."""
    if not _ACCEPTED:
        table = extract(REDESIGN.read_text(encoding="utf-8"))
        table.update(extract_dedicated())
        for intent, (path, funcs, var) in ACCEPT_ONLY.items():
            tree = ast.parse(path.read_text(encoding="utf-8"))
            found = [n for n in ast.walk(tree)
                     if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name in funcs]
            if len(found) != len(funcs):
                raise SystemExit(f"{funcs} absent from {path.name}: the args of intent {intent!r} cannot be read")
            keys = table.setdefault(intent, [])
            for f in found:
                keys += [k for k, _ in _arg_keys(f, var) if k not in keys]
        for intent, extra in PATH_EXTRA.items():
            table.setdefault(intent, [])
            table[intent] += [k for k in extra if k not in table[intent]]
        _ACCEPTED.update({i: tuple(k) for i, k in table.items()})
    return _ACCEPTED


def unknown_args(intent: str, args) -> tuple[list, list]:
    """(the arg names `args` carries that `intent` does not read, sorted; the names it reads).
    ([], []) for an intent with no table (an unknown intent is the caller's own refusal)."""
    acc = accepted().get(intent)
    if acc is None or not isinstance(args, dict):
        return [], list(acc or ())
    return sorted(str(k) for k in args if k not in acc), list(acc)


def arg_values() -> dict[tuple[str, str], tuple]:
    """The closed value sets of intent arguments, read from the
    constants the code validates against (never typed here)."""
    import activities
    import factions
    import proposals
    import redesign
    import seat_events
    import tools
    return {
        ("make_peace", "terms"): tuple(redesign.PEACE_TERMS),
        ("end_war", "terms"): tuple(redesign.END_WAR_TERMS),
        ("faction", "action"): tuple(factions.FACTION_ACTIONS),
        ("faction", "response"): tuple(factions.DEMAND_RESPONSES),
        ("scheme", "action"): tuple(redesign.SCHEME_ACTIONS),
        ("men_at_arms", "action"): tuple(tools.MAA_ACTIONS),
        ("set_policy", "policy"): tuple(redesign.POLICIES) + ("proposals",),
        ("set_policy", "class"): tuple(proposals.CLASS_SPECS),
        ("set_policy", "answer"): tuple(proposals.POLICY_ANSWERS),
        ("answer_proposal", "answer"): tuple(proposals.ANSWERS),
        ("change_law", "scope"): ("title",),
        ("event_rule", "op"): tuple(seat_events.EVENT_RULE_OPS),
        ("event_rule", "answer"): tuple(seat_events.EVENT_ANSWERS),
        ("event_rule", "currency"): tuple(seat_events.EVENT_CURRENCIES),
        ("ai_veto", "what"): tuple(seat_events.AI_VETO_WHATS) + tuple(seat_events.AI_VETO_PRESETS),
        ("host_activity", "activity"): tuple(activities.ACTIVITIES) + (activities.LEAVE,),
        **({("army", "action"): _army_orders.listed_actions()} if _army_orders.listed() else {}),
    }


def render(table: dict[str, list[str]], values: dict | None = None) -> str:
    values = values or {}
    lines = [MARKER_OPEN + ":"]
    for intent in sorted(table):
        keys = table[intent]
        if keys:
            shown = [f"{k}={'|'.join(values[(intent, k)])}" if (intent, k) in values else k
                     for k in keys]
            lines.append(f"  {intent}: args.{{{', '.join(shown)}}}")
    return "\n".join(lines)



def table_text() -> str:
    """The rendered table for redesign.py (and the dedicated intents) beside this module."""
    table = extract(REDESIGN.read_text(encoding="utf-8"))
    table.update(extract_dedicated())
    return render(table, arg_values())
