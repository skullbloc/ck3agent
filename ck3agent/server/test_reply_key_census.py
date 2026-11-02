"""L15 lane A (witness r4 N4-1): every unit-test double of a query_realm,
query_character, query_title_succession, query_title_hierarchy, query_council,
get_controlled_roster or query_succession_simulation reply carries only keys the
bridge writes.

HOST ONLY. Reads the bridge's C++ source and the test modules; no game.

Four doubles invented `query_realm` keys (titles[], monthly_income,
primary_title_tier, is_independent, ...) and the readers built on them were green
in every unit run while the live seat read titles_held 0 and an "independent"
vassal. The truth is the emitter: the `.key("...")` calls each handler makes, and
the calls of every ResponseBuilder helper it hands its builder to
(build_char_profile, emit_char_names, emit_faith_fields, emit_degraded, ...),
followed to a fixpoint. The ipc envelope (request_id, tick, game_date,
ipc_server.cpp) is added to every reply.

How the doubles are found: by the action name they answer. In each test module
the census takes (a) the value of every dict entry keyed by one of TARGETS, and (b) the body of every `if`
whose test compares something to one of those names, and collects every dict key
written in it (dict displays, `x["k"] = ...`, `dict(k=...)`, keyword arguments of
a call to a module function, and the bodies of the module functions and lambdas
it names). A value the census cannot follow is a RED row naming the site, never
a skip.

Fails OPEN: a parse that finds no handler, a handler with no key, a `.key(...)`
argument that is neither a literal nor PERSONALITY_AXIS_NAMES, or a test tree
with no double, each REDS.
"""
from __future__ import annotations

import ast
import functools
import re
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "bridge" / "scripts"))
import cpp_strip  # noqa: E402

#: The replies whose doubles the census holds to the emitter, and their handlers.
TARGETS = {
    "query_realm": "do_query_realm",
    "query_character": "do_query_character",
    "query_title_succession": "do_query_title_succession",
    # L15 A2: the danelaw duchy count read "titles"/"title_key"/"de_jure_kingdom",
    # none of which do_query_title_hierarchy (emit_title_info) writes.
    "query_title_hierarchy": "do_query_title_hierarchy",
    # L15 A2: good_guardian_candidates read seats/character_id from a double the
    # bridge never writes (do_query_council writes council[]/holder_id).
    "query_council": "do_query_council",
    # L15 witness M1: multi_actor() (now actor_gate) read `characters`/`controlled`; the bridge
    # writes `roster` (dispatch_router.cpp do_get_controlled_roster).
    "get_controlled_roster": "do_get_controlled_roster",
    # L15 witness m1: heir_plan read `succession_law`/`law`; the simulation
    # writes neither (query_handlers.cpp do_query_succession_simulation).
    "query_succession_simulation": "do_query_succession_simulation",
}

#: The fail-open floors, per reply: how many keys its emitter must be parsed to
#: write (beside the envelope) and how many doubles the finder must find.
#: do_get_controlled_roster writes four keys (action, primary_character_id,
#: roster, player_character_id) and two modules double it as it stood at
#: 03f19352; every other target keeps the floors it had.
MIN_EMITTED = {"get_controlled_roster": 4}
MIN_DOUBLES = {"get_controlled_roster": 2}

#: ipc_server.cpp stamps these into every object reply (request_id injection and
#: build_envelope_clock_fragment); test_envelope_keys_are_the_ipc_servers checks it.
ENVELOPE = frozenset({"request_id", "tick", "game_date"})

#: The reply a failed read gets instead of its handler's: dispatch_action_with_response
#: writes {"success":false,"action":...} (dispatch_router.cpp), the pipeline's
#: refusals add reason/phase/hint (pipeline.cpp, flush_apply.cpp), and the inline
#: drain's timeout is {"error","action","waited_ms","hint"} (ipc_server.cpp).
#: test_failure_keys_are_the_bridges checks each spelling is in those files.
FAILURE_KEYS = ("success", "action", "error", "reason", "phase", "waited_ms", "hint")
FAILURE_SOURCES = ("dispatch_router.cpp", "pipeline.cpp", "flush_apply.cpp", "ipc_server.cpp")
FAILURE = frozenset((k,) for k in FAILURE_KEYS)

_DEF = re.compile(r"^[ \t]*(?:static\s+|inline\s+)*(?:void|bool)\s+(\w+)\s*\(", re.M)


@functools.lru_cache(maxsize=1)
def _bridge_functions() -> dict:
    """name -> (builder names, parameter names, body text, where) for every
    void/bool function defined in bridge/src and bridge/include whose builder is a
    ResponseBuilder (a parameter, or a local declared in its body)."""
    out: dict = {}
    for path in _bridge_files():
        src = path.read_text(encoding="utf-8", errors="replace")
        code = cpp_strip.strip(src, keep_strings=True)   # comments blanked, same length
        for m in _DEF.finditer(code):
            p_open = m.end() - 1
            p_close = cpp_strip.match_close(src, p_open)
            if p_close < 0:
                continue
            j = p_close + 1
            while j < len(code) and code[j] in " \t\r\n":
                j += 1
            if j >= len(code) or code[j] != "{":
                continue                                     # a declaration
            b_close = cpp_strip.match_close(src, j)
            if b_close < 0:
                continue
            params = code[p_open:p_close + 1]
            body = code[j:b_close + 1]
            builders = set(re.findall(r"ResponseBuilder\s*&\s*(\w+)", params))
            builders |= set(re.findall(r"\bResponseBuilder\s+(\w+)\s*[;({]", body))
            if builders:
                names = [re.findall(r"(\w+)\s*(?:=[^,]*)?$", a.strip())[0] if
                         re.findall(r"(\w+)\s*(?:=[^,]*)?$", a.strip()) else ""
                         for a in (cpp_strip.split_args(code, p_open) or [])]
                line = src.count("\n", 0, m.start()) + 1
                out[m.group(1)] = (builders, names, body, f"{path.relative_to(REPO)}:{line}")
    return out


def _bridge_files() -> list:
    return sorted((REPO / "bridge" / "src").rglob("*.cpp")) + \
        sorted((REPO / "bridge" / "include").rglob("*.h"))


@functools.lru_cache(maxsize=1)
def _defaults() -> dict:
    """(function, parameter) -> its default string literal, from any declaration."""
    out: dict = {}
    for path in _bridge_files():
        code = cpp_strip.strip(path.read_text(encoding="utf-8", errors="replace"),
                               keep_strings=True)
        for m in re.finditer(r"\b(\w+)\s*\(([^;{]*?)\)\s*;", code, re.S):
            for pm in re.finditer(r"const\s+char\s*\*\s*(\w+)\s*=\s*\"(\w+)\"", m.group(2)):
                out[(m.group(1), pm.group(1))] = pm.group(2)
    return out


@functools.lru_cache(maxsize=1)
def _axis_names() -> tuple:
    src = (REPO / "bridge" / "include" / "personality_locks.h").read_text(encoding="utf-8")
    m = re.search(r"PERSONALITY_AXIS_NAMES\[[^\]]*\]\s*=\s*\{([^}]*)\}", src)
    assert m, "PERSONALITY_AXIS_NAMES table not found"
    return tuple(re.findall(r'"(\w+)"', m.group(1)))


def emitted_paths(handler: str) -> tuple[frozenset, list]:
    """(every key path the handler's reply can carry, unresolved sites).

    A path is a tuple of keys from the reply's top level; "[]" stands for an
    element of an array (a key-less begin_object inside begin_array). The body is
    read in source order: `.key(K)` names K at the current path, and the
    begin_object/begin_array that follows opens K; a ResponseBuilder helper is
    read in place at the path its call sits at. A helper's `.key(param)` resolves
    to the literal its caller passed in that position, else the parameter's
    declared default. Both arms of an if/else are read (each balances its own
    begin/end), so the set is every key the reply CAN carry."""
    funcs = _bridge_functions()
    defaults = _defaults()
    paths: set = set()
    unresolved: list = []

    def walk(name: str, bound: dict, stack: list, depth: int) -> None:
        if depth > 12:
            unresolved.append(f"{name}: helper recursion deeper than 12")
            return
        builders, params, body, where = funcs[name]
        bs = "|".join(re.escape(b) for b in builders)
        events = []
        for m in re.finditer(r"\b(?:%s)\s*\.\s*(key|begin_object|begin_array|end_object|end_array)\s*\(" % bs, body):
            events.append((m.start(), m.group(1), m))
        for m in re.finditer(r"\b(\w+)\s*\(\s*(?:%s)\s*[,)]" % bs, body):
            if m.group(1) in funcs:
                events.append((m.start(), "call", m))
        pending = None
        for _, kind, m in sorted(events, key=lambda e: e[0]):
            if kind == "key":
                arg = (cpp_strip.split_args(body, m.end() - 1) or [""])[0]
                lit = re.fullmatch(r'"(\w+)"', arg)
                if lit:
                    names = [lit.group(1)]
                elif re.fullmatch(r"PERSONALITY_AXIS_NAMES\s*\[[^\]]+\]", arg):
                    names = list(_axis_names())
                elif arg in params and (arg in bound or (name, arg) in defaults):
                    names = [bound.get(arg) or defaults[(name, arg)]]
                else:
                    unresolved.append(f"{where} ({name}): .key({arg})")
                    names = []
                for n in names:
                    paths.add(tuple(stack) + (n,))
                pending = names[0] if len(names) == 1 else None
            elif kind in ("begin_object", "begin_array"):
                stack.append(pending if pending is not None else "[]")
                pending = None
            elif kind in ("end_object", "end_array"):
                if stack:
                    stack.pop()
                pending = None
            else:
                callee = m.group(1)
                cargs = cpp_strip.split_args(body, body.index("(", m.start())) or []
                cparams = funcs[callee][1]
                passed = {}
                for i, a in enumerate(cargs):
                    lit = re.fullmatch(r'"(\w+)"', a)
                    if lit and i < len(cparams):
                        passed[cparams[i]] = lit.group(1)
                walk(callee, passed, stack, depth + 1)
                pending = None

    top: list = []
    walk(handler, {}, top, 0)
    # the handler's own reply object is the first begin_object: strip it
    out = set()
    for p in paths:
        if p and p[0] == "[]":
            p = p[1:]
        if p:
            out.add(p)
    return frozenset(out | {(k,) for k in ENVELOPE}), unresolved


def emitted_keys(handler: str) -> tuple[frozenset, list]:
    """The flat key set of emitted_paths (any depth)."""
    ps, un = emitted_paths(handler)
    return frozenset(p[-1] for p in ps), un


# ── the doubles ──────────────────────────────────────────────────────────────

def _test_modules() -> list:
    here = Path(__file__).resolve().parent
    mods = sorted(here.glob("test_*.py")) + sorted((REPO / "ck3agent" / "hooks").glob("test_*.py"))
    return [m for m in mods if m.resolve() != Path(__file__).resolve()]


class _Module:
    """One test module, indexed so a double's value can be followed: its
    functions (by name), module assignments, `self.X = ...` assignments (by
    attribute name), parents (for the enclosing function of a name) and calls
    (for the arguments a parameter receives)."""

    def __init__(self, tree: ast.Module):
        self.funcs: dict = {}
        self.assigns: dict = {}
        self.attrs: dict = {}
        self.parent: dict = {}
        self.calls: list = []
        self._updates: list = []
        for node in ast.walk(tree):
            for ch in ast.iter_child_nodes(node):
                self.parent[ch] = node
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                self.funcs.setdefault(node.name, node)
            elif isinstance(node, ast.Call):
                self.calls.append(node)
                f = node.func
                if isinstance(f, ast.Attribute) and f.attr == "update" and node.args:
                    self._updates.append((f.value, node.args[0]))
            elif isinstance(node, ast.Assign):
                for t in node.targets:
                    if isinstance(t, ast.Attribute) and isinstance(t.value, ast.Name) \
                            and t.value.id == "self":
                        self.attrs.setdefault(t.attr, []).append(node.value)
        for node in tree.body:
            if isinstance(node, ast.Assign):
                for t in node.targets:
                    if isinstance(t, ast.Name):
                        self.assigns[t.id] = node.value
            elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.value:
                self.assigns[node.target.id] = node.value

    def updates_of(self, target) -> list:
        """The arguments of `X.update(arg)` for X = ("self", attr) or
        ("local", function, name)."""
        out = []
        for recv, arg in self._updates:
            if target[0] == "self" and isinstance(recv, ast.Attribute) and \
                    isinstance(recv.value, ast.Name) and recv.value.id == "self" and recv.attr == target[1]:
                out.append(arg)
            elif target[0] == "local" and isinstance(recv, ast.Name) and recv.id == target[2] \
                    and self.enclosing(recv) is target[1]:
                out.append(arg)
        return out

    def enclosing(self, node):
        n = self.parent.get(node)
        while n is not None and not isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            n = self.parent.get(n)
        return n

    def arg_values(self, fn, param: str) -> list:
        """Every expression passed to parameter `param` of `fn` (called by its
        name, or for __init__ by its class's name), and its default."""
        name = fn.name
        if name == "__init__":
            cls = self.parent.get(fn)
            name = cls.name if isinstance(cls, ast.ClassDef) else None
        a = fn.args
        pos = [x.arg for x in a.posonlyargs + a.args]
        if pos and pos[0] == "self":
            pos = pos[1:]
        out = []
        if a.defaults:
            out += [d for x, d in zip(a.args[-len(a.defaults):], a.defaults) if x.arg == param]
        out += [d for x, d in zip(a.kwonlyargs, a.kw_defaults) if x.arg == param and d is not None]
        for c in self.calls:
            f = c.func
            cname = f.id if isinstance(f, ast.Name) else (f.attr if isinstance(f, ast.Attribute) else None)
            if not name or cname != name:
                continue
            if param in pos:
                i = pos.index(param)
                if i < len(c.args) and not isinstance(c.args[i], ast.Starred):
                    out.append(c.args[i])
            out += [kw.value for kw in c.keywords if kw.arg == param]
        return out


def _parametrized(fn, param: str) -> list:
    """The values a @pytest.mark.parametrize decorator gives `param`."""
    out = []
    for d in getattr(fn, "decorator_list", []):
        if not (isinstance(d, ast.Call) and isinstance(d.func, ast.Attribute)
                and d.func.attr == "parametrize" and len(d.args) >= 2):
            continue
        names_node, rows = d.args[0], d.args[1]
        if not isinstance(names_node, ast.Constant) or not isinstance(rows, (ast.List, ast.Tuple)):
            continue
        names = [x.strip() for x in str(names_node.value).split(",")]
        if param not in names:
            continue
        i = names.index(param)
        for row in rows.elts:
            if len(names) == 1:
                out.append(row)
            elif isinstance(row, (ast.Tuple, ast.List)) and i < len(row.elts):
                out.append(row.elts[i])
    return out


def _params(fn) -> list:
    a = fn.args
    return [x.arg for x in a.posonlyargs + a.args + a.kwonlyargs]


class _Paths:
    """The key paths of the replies an expression can evaluate to.

    A dict display adds its keys and descends into their values ({**base}
    follows base); a list, tuple or comprehension descends with "[]"; `dict(...)`
    adds its keywords; a module function is followed into what it returns and the
    keys it writes (`x["k"] = v`, `x.update(...)`, `x.setdefault("k", v)`), and
    the keywords a call passes to it land at the prefix (the `_realm(gold=...)`
    shape); a name is followed to its assignments, or as a parameter to every
    argument its callers pass (a class's __init__ through the class's calls); a
    `self.X` to every `self.X = ...`; a lookup (`t[k]`, `t.get(k)`) to every value
    of the table it reads. What leads nowhere is recorded in `gaps`."""

    def __init__(self, mod: _Module):
        self.mod = mod
        self.gaps: list = []

    def of(self, node, prefix: tuple = (), seen: frozenset = frozenset()) -> set:
        key = (id(node), prefix)
        if key in seen:
            return set()
        seen = seen | {key}
        out: set = set()
        if isinstance(node, ast.Dict):
            for k, v in zip(node.keys, node.values):
                if k is None:
                    out |= self.of(v, prefix, seen)
                elif isinstance(k, ast.Constant) and isinstance(k.value, str):
                    out.add(prefix + (k.value,))
                    out |= self.of(v, prefix + (k.value,), seen)
        elif isinstance(node, (ast.List, ast.Tuple, ast.Set)):
            for e in node.elts:
                out |= self.of(e, prefix + ("[]",), seen)
        elif isinstance(node, (ast.ListComp, ast.SetComp, ast.GeneratorExp)):
            out |= self.of(node.elt, prefix + ("[]",), seen)
        elif isinstance(node, ast.IfExp):
            out |= self.of(node.body, prefix, seen) | self.of(node.orelse, prefix, seen)
        elif isinstance(node, ast.BoolOp):
            for v in node.values:
                out |= self.of(v, prefix, seen)
        elif isinstance(node, (ast.Await, ast.Starred)):
            out |= self.of(node.value, prefix, seen)
        elif isinstance(node, ast.Lambda):
            out |= self.of(node.body, prefix, seen)
        elif isinstance(node, ast.Call):
            out |= self._call(node, prefix, seen)
        elif isinstance(node, ast.Subscript) and isinstance(node.ctx, ast.Load):
            for v in self._table_values(node.value, seen):
                out |= self.of(v, prefix, seen)
        elif isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) \
                and node.value.id == "self" and node.attr in self.mod.attrs:
            for v in self.mod.attrs[node.attr]:
                out |= self.of(v, prefix, seen)
        elif isinstance(node, ast.Name):
            out |= self._name(node, prefix, seen)
        elif not isinstance(node, ast.Constant):
            self.gaps.append(node)
        return out

    def _call(self, node, prefix, seen) -> set:
        m, f, out = self.mod, node.func, set()
        fname = f.id if isinstance(f, ast.Name) else None
        if fname == "dict":
            for kw in node.keywords:
                if kw.arg:
                    out.add(prefix + (kw.arg,))
                    out |= self.of(kw.value, prefix + (kw.arg,), seen)
                else:
                    out |= self.of(kw.value, prefix, seen)
            for a in node.args:
                out |= self.of(a, prefix, seen)
        elif fname in m.funcs:
            out |= {prefix + (kw.arg,) for kw in node.keywords if kw.arg}
            out |= self._function(m.funcs[fname], prefix, seen)
        elif isinstance(f, ast.Attribute) and f.attr == "copy":
            out |= self.of(f.value, prefix, seen)
        elif (fname == "deepcopy" or (isinstance(f, ast.Attribute) and f.attr == "deepcopy")) \
                and node.args:
            out |= self.of(node.args[0], prefix, seen)
        elif (fname or (f.attr if isinstance(f, ast.Attribute) else "")).endswith(("Error", "Exception")):
            pass                                        # a raised read, not a reply
        elif isinstance(f, ast.Attribute) and f.attr == "get" and node.args:
            for v in self._table_values(f.value, seen):
                out |= self.of(v, prefix, seen)
        else:
            self.gaps.append(node)
        return out

    def _table_values(self, node, seen) -> list:
        vals = []
        for src in self._bound(node):
            if isinstance(src, ast.Dict):
                vals.extend(src.values)
        if not vals:
            self.gaps.append(node)
        return vals

    def _bound(self, node, depth: int = 0) -> list:
        """The dict displays a name or self-attribute is bound to: its
        assignments, the arguments its parameter receives, the element a tuple
        unpack of a module function's return gives it, and what `.update(...)`
        adds to it."""
        m = self.mod
        if depth > 8:
            return []
        if isinstance(node, ast.Dict):
            return [node]
        if isinstance(node, ast.BoolOp):
            return [b for v in node.values for b in self._bound(v, depth + 1)]
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "dict":
            return [b for a in node.args for b in self._bound(a, depth + 1)]
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) \
                and node.value.id == "self":
            srcs = list(m.attrs.get(node.attr, [])) + m.updates_of(("self", node.attr))
            return [b for v in srcs for b in self._bound(v, depth + 1)]
        if not isinstance(node, ast.Name):
            return []
        if node.id in m.assigns:
            return self._bound(m.assigns[node.id], depth + 1)
        fn = m.enclosing(node)
        while fn is not None:
            srcs: list = []
            if not isinstance(fn, ast.Lambda) and node.id in _params(fn):
                srcs += m.arg_values(fn, node.id)
            for n in ast.walk(fn):
                if not isinstance(n, ast.Assign):
                    continue
                for t in n.targets:
                    if isinstance(t, ast.Name) and t.id == node.id:
                        srcs.append(n.value)
                    elif isinstance(t, ast.Tuple):
                        names = [e.id if isinstance(e, ast.Name) else None for e in t.elts]
                        if node.id in names:
                            srcs += self._unpacked(n.value, names.index(node.id))
            if srcs:
                srcs += m.updates_of(("local", fn, node.id))
                return [b for v in srcs for b in self._bound(v, depth + 1)]
            fn = m.enclosing(fn)
        return []

    def _unpacked(self, value, i: int) -> list:
        """Element i of what `value` (a tuple display or a module function's
        returned tuple) unpacks to."""
        rows = [value]
        if isinstance(value, ast.Call) and isinstance(value.func, ast.Name) \
                and value.func.id in self.mod.funcs:
            rows = [r.value for r in ast.walk(self.mod.funcs[value.func.id])
                    if isinstance(r, ast.Return) and r.value is not None]
        return [r.elts[i] for r in rows if isinstance(r, (ast.Tuple, ast.List)) and i < len(r.elts)]

    def _function(self, fn, prefix, seen) -> set:
        out: set = set()
        for n in ast.walk(fn):
            if isinstance(n, ast.Return) and n.value is not None:
                out |= self.of(n.value, prefix, seen)
            elif isinstance(n, ast.Assign):
                for t in n.targets:
                    if isinstance(t, ast.Subscript) and isinstance(t.slice, ast.Constant) \
                            and isinstance(t.slice.value, str):
                        out.add(prefix + (t.slice.value,))
                        out |= self.of(n.value, prefix + (t.slice.value,), seen)
            elif isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) \
                    and n.func.attr in ("update", "setdefault"):
                out |= {prefix + (kw.arg,) for kw in n.keywords if kw.arg}
                if n.func.attr == "update":
                    for a in n.args:
                        out |= self.of(a, prefix, seen)
                elif n.args and isinstance(n.args[0], ast.Constant) and isinstance(n.args[0].value, str):
                    out.add(prefix + (n.args[0].value,))
        return out

    def _name(self, node, prefix, seen) -> set:
        m = self.mod
        if node.id in m.funcs:
            return self._function(m.funcs[node.id], prefix, seen)
        if node.id in m.assigns:
            return self.of(m.assigns[node.id], prefix, seen)
        fn = m.enclosing(node)
        out: set = set()
        found = False
        while fn is not None and not found:        # a closure reads its outer scopes
            for n in ast.walk(fn):
                if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == node.id
                                                     for t in n.targets):
                    found = True
                    out |= self.of(n.value, prefix, seen)
                elif isinstance(n, ast.Assign) and len(n.targets) == 1 \
                        and isinstance(n.targets[0], ast.Tuple):
                    # reply, gate = TABLE[k]  /  reply, gate = (r, g)
                    names = [e.id if isinstance(e, ast.Name) else None for e in n.targets[0].elts]
                    if node.id in names:
                        found = True
                        i = names.index(node.id)
                        rows = [n.value] if isinstance(n.value, ast.Tuple) else (
                            self._table_values(n.value.value, seen)
                            if isinstance(n.value, ast.Subscript) else [])
                        for row in rows:
                            if isinstance(row, (ast.Tuple, ast.List)) and i < len(row.elts):
                                out |= self.of(row.elts[i], prefix, seen)
                            else:
                                self.gaps.append(row)
                        if not rows:
                            self.gaps.append(n.value)
                elif isinstance(n, (ast.For, ast.comprehension)) and isinstance(n.target, ast.Tuple) \
                        and isinstance(n.iter, (ast.Tuple, ast.List)):
                    # for (reply, gate) in ((r1, g1), ...): the element at the name's place
                    names = [e.id if isinstance(e, ast.Name) else None for e in n.target.elts]
                    if node.id in names:
                        found = True
                        i = names.index(node.id)
                        for row in n.iter.elts:
                            if isinstance(row, (ast.Tuple, ast.List)) and i < len(row.elts):
                                out |= self.of(row.elts[i], prefix, seen)
                elif isinstance(n, (ast.For, ast.comprehension)) and isinstance(n.target, ast.Name) \
                        and n.target.id == node.id and isinstance(n.iter, (ast.Tuple, ast.List)):
                    found = True
                    for e in n.iter.elts:
                        out |= self.of(e, prefix, seen)
            if not isinstance(fn, ast.Lambda) and node.id in _params(fn):
                found = True
                for v in m.arg_values(fn, node.id) + _parametrized(fn, node.id):
                    out |= self.of(v, prefix, seen)
            fn = m.enclosing(fn)
        if not found:
            self.gaps.append(node)
        return out


def _names_target(test: ast.AST) -> set:
    """The TARGETS a comparison or membership test names."""
    return {n.value for n in ast.walk(test) if isinstance(n, ast.Constant) and n.value in TARGETS}


#: Server functions a test hands a reply directly, and the reply each argument
#: (keyword name or position) is.
_REPLY_PARAMS = {
    "shield_decision_why_blocked": {"realm": "query_realm", "character": "query_character"},
    "actor_echo": {0: "query_character", "char": "query_character"},
    "unbound_actor_block": {0: "query_character", "reply": "query_character"},
    "dejure_keys_for": {0: "query_realm", "realm": "query_realm"},
}


def _exports(tree: ast.Module) -> list:
    """[(class, param, position, action)]: a fake gateway class whose __init__
    parameter becomes the reply it serves for `action` (`self.X = param`, and an
    `if` naming the action returns or appends something reading self.X). A test
    module that imports the class hands its doubles in through that argument."""
    out = []
    for cls in [n for n in tree.body if isinstance(n, ast.ClassDef)]:
        init = next((f for f in cls.body if isinstance(f, ast.FunctionDef) and f.name == "__init__"), None)
        if init is None:
            continue
        params = [x.arg for x in init.args.args][1:]
        held = {}
        for n in ast.walk(init):
            if isinstance(n, ast.Assign) and isinstance(n.value, ast.Name) and n.value.id in params:
                for t in n.targets:
                    if isinstance(t, ast.Attribute) and isinstance(t.value, ast.Name) and t.value.id == "self":
                        held[t.attr] = n.value.id
        for n in ast.walk(cls):
            if isinstance(n, ast.If):
                for action in _names_target(n.test):
                    for x in ast.walk(ast.Module(body=n.body, type_ignores=[])):
                        if isinstance(x, ast.Attribute) and isinstance(x.value, ast.Name) \
                                and x.value.id == "self" and x.attr in held:
                            p = held[x.attr]
                            out.append((cls.name, p, params.index(p), action))
    return sorted(set(out))


def find_doubles() -> list:
    """find_doubles_in over every test module (this one excluded), with the fake
    gateways each module imports from another (_exports)."""
    mods = {p.stem: p for p in _test_modules()}
    srcs = {stem: p.read_text(encoding="utf-8") for stem, p in mods.items()}
    exported = {stem: _exports(ast.parse(src)) for stem, src in srcs.items()}
    out = []
    for stem, path in mods.items():
        imported = []
        for n in ast.walk(ast.parse(srcs[stem])):
            if isinstance(n, ast.ImportFrom) and n.module in exported:
                names = {a.asname or a.name: a.name for a in n.names}
                imported += [(local, p, i, act) for local, orig in names.items()
                             for (c, p, i, act) in exported[n.module] if c == orig]
        out += find_doubles_in(srcs[stem], str(path.relative_to(REPO)), imported)
    return out


def find_doubles_in(src: str, rel: str, imported: list = ()) -> list:
    """[(where, action, key paths, followed)] for every double the census finds:
    (a) the value of a dict entry keyed by a TARGETS name, (b) the value assigned
    to `x["<target>"]`, (c) what the body of an `if` whose test names a TARGETS
    action returns or appends, (d) a reply handed to a _REPLY_PARAMS argument,
    (e) the argument an imported fake gateway serves as a reply (`imported`,
    from _exports). `followed` is False when the value yields no path AND some
    part of it led nowhere the census can read (a literal {} / None, or a raised
    exception, is a reply with no keys, and is followed)."""
    out = []
    tree = ast.parse(src)
    mod = _Module(tree)

    def add(node, action, values):
        pr = _Paths(mod)
        paths: set = set()
        for v in values:
            paths |= pr.of(v)
        out.append((f"{rel}:{node.lineno}", action, paths, bool(paths) or not pr.gaps))

    for n in ast.walk(tree):
        if isinstance(n, ast.Dict):
            for k, v in zip(n.keys, n.values):
                if isinstance(k, ast.Constant) and k.value in TARGETS:
                    add(k, k.value, [v])
        elif isinstance(n, ast.Assign):
            for t in n.targets:
                if isinstance(t, ast.Subscript) and isinstance(t.slice, ast.Constant) \
                        and t.slice.value in TARGETS:
                    add(t, t.slice.value, [n.value])
        elif isinstance(n, ast.If):
            for action in _names_target(n.test):
                vals = []
                for st in n.body:
                    for x in ast.walk(st):
                        if isinstance(x, ast.Return) and x.value is not None:
                            vals.append(x.value)
                        elif isinstance(x, ast.Call) and isinstance(x.func, ast.Attribute) \
                                and x.func.attr == "append" and x.args:
                            vals.append(x.args[0])
                if vals:
                    add(n, action, vals)
        elif isinstance(n, ast.Call):
            f = n.func
            fname = f.id if isinstance(f, ast.Name) else (f.attr if isinstance(f, ast.Attribute) else None)
            table = _REPLY_PARAMS.get(fname, {})
            for kw in n.keywords:
                if kw.arg in table:
                    add(kw.value, table[kw.arg], [kw.value])
            for i, a in enumerate(n.args):
                if i in table:
                    add(a, table[i], [a])
            for local, p, i, action in imported:
                if fname == local:
                    vals = [a for j, a in enumerate(n.args) if j == i] + \
                        [kw.value for kw in n.keywords if kw.arg == p]
                    if vals:
                        add(n, action, vals)
    return out


def forged(paths: set, action: str) -> list:
    """The paths of a double the emitter never writes, nor the failure reply."""
    em = emitted_paths(TARGETS[action])[0] | FAILURE
    return sorted(p for p in paths if p not in em)


# ── the rows ─────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("action", sorted(TARGETS))
def test_the_emitter_parses(action):
    """Fails OPEN: a handler the parse cannot find, or finds no key in, or a
    `.key(...)` it cannot name, reds here instead of passing every double."""
    paths, unresolved = emitted_paths(TARGETS[action])
    assert not unresolved, unresolved
    assert len(paths - {(k,) for k in ENVELOPE}) >= MIN_EMITTED.get(action, 15), \
        (action, sorted(paths))


def test_the_parse_reads_nesting_and_helpers():
    """Known facts of the emitters, each from a different mechanism: a nested
    object (skills), an array of objects (vassals), a helper (emit_char_names),
    a helper keyed by its caller's default argument (emit_faith_fields), the
    personality axis table, and emit_degraded."""
    ch = emitted_paths("do_query_character")[0]
    realm = emitted_paths("do_query_realm")[0]
    assert ("skills", "martial") in ch and ("martial",) not in ch
    assert ("vassals", "[]", "id") in realm
    assert {("house_name",), ("faith",), ("faith_key",), ("personality", "boldness"),
            ("degraded",)} <= ch
    assert ("held_title_count",) in realm and ("titles",) not in realm
    # L15 A3: core_data+0x170 is the merit stock and +0x2E8 base health: query_character
    # writes `merit` and no `monthly_income`; query_realm's income has no `accumulated`.
    assert ("merit",) in ch and ("monthly_income",) not in ch
    assert ("income_breakdown", "total_monthly") in ch
    assert ("income", "monthly") in realm and ("income", "accumulated") not in realm
    th = emitted_paths("do_query_title_hierarchy")[0]   # emit_title_info, nested twice
    assert {("held_titles", "[]", "de_jure_parent_key"), ("held_titles", "[]", "key"),
            ("held_titles", "[]", "sub_titles", "[]", "tier"), ("title", "sub_titles", "[]", "key")} <= th
    assert not {("titles",), ("held_titles", "[]", "title_key"),
                ("held_titles", "[]", "de_jure_kingdom")} & th
    # L16 R9: a count out of range or a held title that did not resolve is an
    # error with the raw count (title_count_clamped, a helper handed the builder),
    # never an empty held_titles. RED on 24cac589 (the count was zeroed silently).
    assert {("error",), ("count_field",), ("raw_count",), ("max_count",), ("index",)} <= th


def _reply_paths(o, pre=()) -> set:
    out: set = set()
    if isinstance(o, dict):
        for k, v in o.items():
            out.add(pre + (k,))
            out |= _reply_paths(v, pre + (k,))
    elif isinstance(o, list):
        for x in o:
            out |= _reply_paths(x, pre + ("[]",))
    return out


@pytest.mark.parametrize("fixture", ["state_867_duke_at_war.json",
                                     "state_867_liege_with_factions.json"])
def test_CONTROL_live_replies_sit_inside_the_parse(fixture):
    """The parse against the game: the hook fixtures are RAW bridge replies from
    a live 867 launch (l7-hook-1, provenance in each file). Every key path they
    carry for a TARGETS action is one the parse found, so the parse is not
    narrower than the bridge (a too-narrow parse would red honest doubles)."""
    import json
    replies = json.loads((REPO / "ck3agent" / "hooks" / "fixtures" / fixture)
                         .read_text(encoding="utf-8"))["replies"]
    seen = [a for a in TARGETS if a in replies]
    assert seen, "the fixture carries no TARGETS reply"
    for a in seen:
        extra = sorted(_reply_paths(replies[a]) - emitted_paths(TARGETS[a])[0])
        assert not extra, (a, extra)


def test_envelope_keys_are_the_ipc_servers():
    src = (REPO / "bridge" / "src" / "ipc_server.cpp").read_text(encoding="utf-8")
    for k in ENVELOPE:
        assert '\\"%s\\":' % k in src, k


def test_failure_keys_are_the_bridges():
    srcs = "".join((REPO / "bridge" / "src" / f).read_text(encoding="utf-8")
                   for f in FAILURE_SOURCES)
    for k in FAILURE_KEYS:
        assert '\\"%s\\":' % k in srcs, k


def test_the_census_finds_the_doubles():
    """Fails OPEN: a finder that stopped matching the tests' shapes reds here."""
    found = find_doubles()
    for action in TARGETS:
        assert sum(1 for d in found if d[1] == action) >= MIN_DOUBLES.get(action, 3), action


def test_every_double_is_followed():
    lost = [(w, a) for w, a, _, followed in find_doubles() if not followed]
    assert not lost, f"doubles whose value the census cannot read: {lost}"


def test_every_double_carries_only_emitted_keys():
    bad = [(w, a, forged(p, a)) for w, a, p, _ in find_doubles() if forged(p, a)]
    assert not bad, "doubles carrying keys the bridge never writes:\n" + \
        "\n".join(f"  {w} {a}: {f}" for w, a, f in bad)


@pytest.mark.parametrize("src,want", [
    # the four L14 shapes: a dict entry, a subscript assignment, an if-branch, a kwarg
    ('G = {"query_realm": {"titles": [{"title_key": "d_x"}]}}', [("titles",), ("titles", "[]", "title_key")]),
    ('def t(b):\n    b.replies["query_realm"] = {"is_independent": True}', [("is_independent",)]),
    ('class G:\n    def read(self, a):\n        if a == "query_character":\n'
     '            return {"character": {"age": 3}}', [("character",), ("character", "age")]),
    ('shield_decision_why_blocked("x", realm={"primary_title_tier": 2}, character={})',
     [("primary_title_tier",)]),
    # followed through a helper's keywords and a constructor parameter
    ('def _r(**more):\n    r = {"gold": 1}\n    r.update(more)\n    return r\n'
     'G = {"query_realm": _r(titles=[])}', [("gold",), ("titles",)]),
    ('class W:\n    def __init__(self, realm):\n        self._realm = realm\n'
     '    def read(self, a):\n        if a == "query_realm":\n            return self._realm\n'
     'W({"monthly_income": 1})', [("monthly_income",)]),
    # L15 A2: the danelaw reader's three invented title keys
    ('def t(b):\n    b.replies["query_title_hierarchy"] = {"titles": [{"title_key": "d_x",'
     ' "de_jure_kingdom": "k_england", "tier": 3}]}',
     [("titles",), ("titles", "[]", "de_jure_kingdom"), ("titles", "[]", "tier"),
      ("titles", "[]", "title_key")]),
    ('G = {"query_title_hierarchy": {"held_titles": [{"key": "d_x", "de_jure_kingdom": "k"}]}}',
     [("held_titles", "[]", "de_jure_kingdom")]),
    # L15 A3: the merit emitted as income, and base health emitted as accumulated income
    ('G = {"query_character": {"gold": 1.0, "monthly_income": 4.2}}', [("monthly_income",)]),
    ('G = {"query_realm": {"income": {"monthly": 1.0, "accumulated": 4.4}}}',
     [("income", "accumulated")]),
    # the guardian council double
    ('R = {"query_council": {"seats": [{"character_id": 401}]}}',
     [("seats",), ("seats", "[]", "character_id")]),
])
def test_CONTROL_the_census_reds_on_a_forged_double(src, want):
    """The mutant rows: each L14 forging shape, written fresh, is found and its
    forged keys named; the census cannot pass a tree that still carries one."""
    found = find_doubles_in(src, "<control>")
    assert found, src
    got = sorted({p for w, a, paths, _ in found for p in forged(paths, a)})
    assert got == sorted(want), (got, want)


def test_CONTROL_an_emitted_double_is_clean():
    """The non-discriminating control: a double built from emitted keys only
    (top-level, nested, array element) is clean."""
    src = ('G = {"query_realm": {"held_title_count": 4, "income": {"monthly": 1.0},'
           ' "vassals": [{"id": 9}]}, "query_character": {"id": 1, "skills": {"martial": 9},'
           ' "liege_id": 0, "top_liege_id": 1},'
           ' "query_title_succession": {"title_id": 3, "current_holder_id": 1},'
           ' "query_title_hierarchy": {"held_titles": [{"key": "d_x", "tier": 3,'
           ' "de_jure_parent_key": "k_england", "sub_titles": [{"key": "c_y"}]}]},'
           ' "query_council": {"character_id": 1, "council": [{"position_id": 1,'
           ' "holder_id": 401}, {"position_id": 2, "holder_id": 16777215, "vacant": True}]}}')
    found = find_doubles_in(src, "<control>")
    assert len(found) == 5 and all(f[3] for f in found)
    assert [forged(p, a) for _, a, p, _ in found] == [[], [], [], [], []]
