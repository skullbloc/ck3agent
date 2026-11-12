"""cleanup 2026.11 F14: every tool call the agent is TAUGHT exists on the surface.

The instructions an agent reads are the shipped protocol (ck3agent/protocol/*.md,
in this tree release/overlay/ck3agent/protocol/), the server's own instructions
and tool descriptions, the live-state hook's output, the player docs (README.md and docs/*.md of the public tree; the ADR and
porting docs below docs/ name C functions, not tools), and, in the research tree,
ck3agent/CLAUDE.md, ck3agent/README.md, ck3agent/knowledge/*, SOUL.md and BOOT's
sources (ck3agent/boot/*.md), and the `next` line of every refusal
(redesign.GATE_NEXT). Before this test they
taught act(), intent(), game(), control() and bridge(), all retired.

THE SURFACE IS WHAT FASTMCP REGISTERS: mcp_server.registered_tool_definitions(),
the tools/list handler, as release/gen_public_reference.py renders the public
reference from. Nothing here lists a tool by hand.

WHAT COUNTS AS A CALL. In a text, every `name(`, whatever its arguments:
`act(create_title, k_ireland)` is as much a call as `act(type="war")`. It FAILS
OPEN: a call-shaped name that is not registered is a failure, whatever it is
called and however its arguments are written, so a retired tool cannot hide
behind a list of retired names or behind positional arguments. Two prose shapes
are not calls, each by a rule in not_a_call(): a CK3 title id followed by a
parenthesis (`d_connacht(3)`, a count beside a title), and a plural marker
(`heir(s)`). Each keyword must be a parameter of the
tool; a quoted value of a parameter that carries an enum must be in it; and
do(intent="X", args={...}) keys must be keys intent X reads (the table
do_intent_args extracts from redesign._intent_to_act_params, the one the `args`
description prints).
"""
from __future__ import annotations

import ast
import json
import os
import re
import sys
import time
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

ms = pytest.importorskip("mcp_server")
import do_intent_args  # noqa: E402

#: Call-shaped words that are not tool calls, each with why. Kept short on purpose:
#: a name here is never checked again.
NOT_TOOLS: dict[str, str] = {}


def _surface() -> dict:
    out = {}
    for t in ms.registered_tool_definitions():
        props = (t.get("input_schema") or {}).get("properties") or {}
        enums = {}
        for k, p in props.items():
            vals = p.get("enum") or (p.get("items") or {}).get("enum")
            for alt in p.get("anyOf") or []:
                vals = vals or alt.get("enum") or (alt.get("items") or {}).get("enum")
            if vals:
                enums[k] = {v for v in vals if v is not None}
        out[t["name"]] = {"params": set(props), "enums": enums,
                          "description": t.get("description") or ""}
    return out


SURFACE = _surface()
DO_ARGS = do_intent_args.extract(do_intent_args.REDESIGN.read_text(encoding="utf-8"))

CALL = re.compile(r"(?<![\w.$/-])([a-z_][a-z0-9_]*)\(")


def _close(text: str, i: int) -> int:
    """Index of the parenthesis closing the one before text[i], or -1."""
    depth, q = 1, None
    while i < len(text):
        c = text[i]
        if q:
            if c == "\\":
                i += 1
            elif c == q:
                q = None
        elif c in "\"'":
            q = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _split_top(s: str) -> list[str]:
    parts, depth, q, cur = [], 0, None, ""
    for i, c in enumerate(s):
        if q:
            cur += c
            if c == q and s[i - 1] != "\\":
                q = None
            continue
        if c in "\"'":
            q = c
        elif c in "([{<":
            depth += 1
        elif c in ")]}>":
            depth -= 1
        if c == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += c
    parts.append(cur)
    return [p.strip() for p in parts if p.strip()]


def _literal(v: str):
    m = re.fullmatch(r"""\s*(["'])([^"']*)\1\s*""", v)
    return m.group(2) if m else None


def _dict_keys(v: str) -> list[str] | None:
    v = v.strip()
    if not v.startswith("{"):
        return None
    end = _close(v, 1)
    if end < 0:
        return None
    keys = []
    for part in _split_top(v[1:end]):
        m = re.match(r"""(["'])(\w+)\1\s*:""", part)
        if m:
            keys.append(m.group(2))
    return keys


TITLE_ID = re.compile(r"[bcdke]_[a-z0-9_]+")


def not_a_call(name: str, inner: str) -> bool:
    """The two prose shapes that look like calls. Nothing else is exempt."""
    if name in SURFACE:
        return False
    return bool(TITLE_ID.fullmatch(name)) or inner == "s"


def calls(text: str):
    """(name, args text) for every call-shaped `name(` in text."""
    for m in CALL.finditer(text):
        name, start = m.group(1), m.end()
        end = _close(text, start)
        inner = text[start:end] if end >= 0 else text[start:start + 60]
        if not not_a_call(name, inner):
            yield name, inner


def problems(text: str, where: str) -> list[str]:
    bad = []
    for name, inner in calls(text):
        if name in NOT_TOOLS:
            continue
        if name not in SURFACE:
            bad.append(f"{where}: {name}({inner[:60]}) is not a registered tool")
            continue
        tool = SURFACE[name]
        kws = {}
        for part in _split_top(inner):
            m = re.match(r"([a-z_]+)\s*=(?!=)\s*(.*)$", part, re.S)
            if not m:
                continue
            k, v = m.group(1), m.group(2)
            kws[k] = v
            if k not in tool["params"]:
                bad.append(f"{where}: {name}({k}=...) is not a parameter of {name}")
                continue
            lit = _literal(v)
            if lit is not None and k in tool["enums"] and lit not in tool["enums"][k]:
                bad.append(f"{where}: {name}({k}={lit!r}) is not in its enum")
        if name == "do":
            intent = _literal(kws.get("intent", ""))
            keys = _dict_keys(kws.get("args", ""))
            if intent in DO_ARGS and keys:
                extra = [k for k in keys if k not in DO_ARGS[intent]]
                if extra:
                    bad.append(f"{where}: do(intent={intent!r}) args {extra} are not read "
                               f"(it reads {DO_ARGS[intent]})")
    return bad


# ── the texts ─────────────────────────────────────────────────────────────────

def _first(*rels: str) -> Path | None:
    for r in rels:
        if (ROOT / r).is_dir():
            return ROOT / r
    return None


def instruction_files() -> list[Path]:
    files = []
    proto = _first("release/overlay/ck3agent/protocol", "ck3agent/protocol")
    assert proto is not None, "the shipped protocol is not in this tree"
    files += sorted(proto.glob("*.md"))
    # The player docs: release/overlay/ in this tree, the export's root otherwise.
    overlay = ROOT / "release/overlay"
    pub = overlay if overlay.is_dir() else ROOT
    files += [pub / "README.md"] if (pub / "README.md").is_file() else []
    files += sorted((pub / "docs").glob("*.md")) if (pub / "docs").is_dir() else []
    agent = ROOT / "ck3agent"
    if overlay.is_dir() and (agent / "README.md").is_file():
        files.append(agent / "README.md")
    if (agent / "CLAUDE.md").is_file() and (agent / "knowledge").is_dir():  # research tree
        files.append(agent / "CLAUDE.md")
        files += sorted(agent.glob("SOUL*.md"))
        files += sorted(p for p in (agent / "knowledge").rglob("*") if p.suffix in (".md", ".json"))
        # BOOT's sources: gen_boot.py pastes them into every episode's first prompt.
        files += sorted((agent / "boot").glob("*.md")) if (agent / "boot").is_dir() else []
    # v4instr: the experiment seats' play files and guide pages (research tree)
    guide = ROOT / "scripts" / "prompts" / "v4-1066"
    files += sorted(guide.rglob("*.md")) if guide.is_dir() else []
    return files


#: Calls in a HUMAN-OWNED file (its header says it is edited only by operator
#: ruling, so this lane does not rewrite it). Each pair is reported nowhere else
#: and is checked to be still there: when the operator fixes the line, the pin
#: reds and is removed, so the list cannot outlive what it covers.
HUMAN_OWNED_CALLS: set = set()   # L12: the operator-directives bridge() line was fixed


def hook_outputs() -> dict[str, str]:
    """What the live-state hook prints: its fixed lines, and its report on every
    state recorded under ck3agent/hooks/fixtures (research tree)."""
    import importlib.util
    path = ROOT / "ck3agent/hooks/inject_state.py"
    spec = importlib.util.spec_from_file_location("inject_state_for_f14", path)
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)
    out = {"hook constants": "\n".join(str(v) for k, v in vars(hook).items()
                                        if k.isupper() and isinstance(v, str))}

    class Fake:
        def __init__(self, replies):
            self.replies = replies

        def send_actions(self, tick_id, actions):
            self.rid, self.action = actions[0].get("request_id"), actions[0].get("action")

        def recv(self, timeout=0.8):
            return {**self.replies.get(self.action, {}), "request_id": self.rid}

        def close(self):
            pass

    def report(replies):
        p = hook.BridgePuller(time.monotonic() + 30)
        p.client = Fake(replies)
        return hook.build_report(p)

    out["hook: no ruler"] = report({"get_tick_info": {"paused": True, "controlled_character_id": 0}})
    out["hook: dead ruler"] = report({"get_tick_info": {"paused": False, "controlled_character_id": 7,
                                                        "controlled_alive": False}})
    out["hook: unpaused"] = report({"get_tick_info": {"paused": False, "controlled_character_id": 7}})
    fx = ROOT / "ck3agent/hooks/fixtures"
    for f in sorted(fx.glob("state_*.json")) if fx.is_dir() else []:
        out[f"hook: {f.name}"] = report(json.loads(f.read_text(encoding="utf-8"))["replies"])
    return out


def all_texts() -> dict[str, str]:
    texts = {str(p.relative_to(ROOT)): p.read_text(encoding="utf-8") for p in instruction_files()}
    texts["mcp_server instructions"] = ms.mcp.instructions or ""
    # The `next` line every refusal carries tells the player what to call.
    import redesign
    texts["redesign.GATE_NEXT"] = "\n".join(redesign.GATE_NEXT.values())
    for name, t in SURFACE.items():
        texts[f"{name} description"] = t["description"]
    texts.update(hook_outputs())
    return texts


# ── the rows ──────────────────────────────────────────────────────────────────

def test_every_tool_call_taught_exists_on_the_registered_surface():
    bad = []
    for where, text in all_texts().items():
        bad += [b for b in problems(text, where)
                if not any(b.startswith(f"{f}: {n}(") for f, n in HUMAN_OWNED_CALLS)]
    assert not bad, "\n".join(bad)


def test_each_human_owned_pin_is_still_in_a_human_owned_file():
    texts = all_texts()
    for f, n in HUMAN_OWNED_CALLS:
        if f not in texts:  # the export tree ships no boot/
            continue
        assert "HUMAN-OWNED" in texts[f][:600], f
        assert any(b.startswith(f"{f}: {n}(") for b in problems(texts[f], f)), \
            f"{f} no longer calls {n}(): remove the pin"


def test_the_texts_are_read_and_teach_calls():
    """Not vacuous: the protocol and the descriptions are found and name calls."""
    texts = all_texts()
    proto = [k for k in texts if k.endswith("protocol/CLAUDE.md")]
    assert proto, sorted(texts)
    names = {n for t in texts.values() for n, _ in calls(t)}
    assert {"observe", "session", "do", "inspect", "succession_plan"} <= names, names


@pytest.mark.parametrize("text, want", [
    ('act(type="war", target_id=1)', "act(type=\"war\", target_id=1) is not a registered tool"),
    ("intent(action='queue')", "intent(action='queue') is not a registered tool"),
    ("call bridge() to diagnose", "bridge() is not a registered tool"),
    ("Disinherit with `intent(queue, disinherit)`", "intent(queue, disinherit) is not a registered tool"),
    ("`act(create_title, k_ireland)` at peace", "act(create_title, k_ireland) is not a registered tool"),
    ("then `bridge()` / `observe()`", "bridge() is not a registered tool"),
    ("game(42) and control(x)", "game(42) is not a registered tool"),
    ('observe(include=["tick"])', "observe(include=...) is not a parameter of observe"),
    ('inspect(subject="research_tree")', "inspect(subject='research_tree') is not in its enum"),
    ('do(intent="interact", args={"interaction_type": "x", "foo": 1})',
     "do(intent='interact') args ['foo'] are not read"),
    ('session(action="pause")', None),
    ('do(intent="change_law", args={"law": "x", "scope": "title", "title_id": 3})', None),
    ("d_connacht(holder 9) and a prose aside (see above)", None),
    ("the heir(s) of k_england(2)", None),
])
def test_the_checker_reds_on_each_kind_of_wrong_call(text, want):
    """Controls: each defect the checker exists for is caught, and right calls pass."""
    got = problems(text, "x")
    if want is None:
        assert got == []
    else:
        assert any(want in g for g in got), got
