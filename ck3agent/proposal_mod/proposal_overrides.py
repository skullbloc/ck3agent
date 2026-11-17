#!/usr/bin/env python3
"""Incoming-proposal queue: the INSTALL-TIME GENERATOR of the game side's
interaction copies (lane QMOD, grown from lane QDESIGN's prototype; design:
docs/design/incoming-proposals.md).

It reads the INSTALLED game's own `common/character_interactions/*.txt` and
writes, for the mod `ck3agent_proposals` (ck3agent/proposal_mod/), files that
are never shipped and never committed:

  * whole-file copies of the files that define a first-set interaction, each
    identical to the source except for whole lines ending in MARK, so
    `strip_marked(copy) == source` BYTE FOR BYTE (the static half of the
    unseated-inertness argument). Each added line has one of the enumerated
    shapes ALLOWED_LINE matches, and nothing else:
      1  capture   first line of on_send (created when absent)
      2  hold      first line of the validity (created when absent)
      3  options   per send_option: exclusive -> a validity line; combinable ->
                   starts_enabled wrapped (or created) and can_be_changed headed
                   (or created), so while OUR call sends, the option is on
                   exactly when it was on in the captured offer
      4  on_send   a non-empty vanilla on_send body wrapped in "not while
                   answering", so our re-send does not run it a second time
  * common/scripted_effects/zz_ck3agent_proposal_kinds.txt and
    common/scripted_triggers/zz_ck3agent_proposal_kinds.txt: per kind the
    LIFTED bodies (on_accept, on_decline, auto_accept and the validity, comments
    dropped; a body that reads `root` or a file @constant is refused), and the
    dispatch chains the static mod calls (they name a kind or a class, so they
    are generated from FIRST_SET rather than written by hand);
  * proposal_manifest.json: the game's rawVersion, each source file's sha256,
    each generated file's sha256 (the witness compares them at launch).

It REFUSES (raises Refused with a code) rather than guessing: a source that
already holds MARK, a first-set key absent, defined twice or in two files, a
block that opens and closes on one line where a line must go inside it, a
closing brace that shares a line with content, an option without a plain flag,
a scalar where a block must be, an unreadable game version.

    proposal_overrides.py census DIR              # JSON rows, one per interaction
    proposal_overrides.py check DIR               # inject + strip over the first set's files
    proposal_overrides.py generate INSTALL OUT    # write the generated files under OUT
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

#: Every line the generator adds ends with this, and no vanilla line may.
MARK = "# ck3agent:proposal"

# ── THE FIRST SET ────────────────────────────────────────────────────────────
# interaction key -> (class, application family). The class is the unit a
# seat's answer policy is set on; the family is how an answer is applied
# (design, "Applying an answer"). One constant per class: a class the live rows
# show cannot be held (L7 call_to_war, L8 peace) is removed by DELETING its
# constant, its line in FIRST_SET, its rows in the design table and its row in
# bridge/include/script_templates.h kProposalClasses (the mirror tests red on
# any one left behind), never by a rewrite.
MARRIAGE = {"arrange_marriage_interaction": ("marriage", "resend")}
ALLIANCE = {"negotiate_alliance_interaction": ("alliance", "resend")}
CALL_TO_WAR = {"call_ally_interaction": ("call_to_war", "join_war")}
PEACE = {
    "end_war_attacker_victory_interaction": ("peace", "peace"),
    "end_war_attacker_white_peace_interaction": ("peace", "peace"),
    "end_war_attacker_defeat_interaction": ("peace", "peace"),
}
VASSALAGE = {"offer_vassalization_interaction": ("vassalage", "resend")}

FIRST_SET: dict[str, tuple[str, str]] = {**MARRIAGE, **ALLIANCE, **CALL_TO_WAR, **PEACE, **VASSALAGE}

#: The classes, in the order the tables list them.
CLASSES: tuple[str, ...] = tuple(dict.fromkeys(c for c, _ in FIRST_SET.values()))

#: Lane ACTORKEY (wave 15), L8: the classes whose every send is logged at the very
#: top of the capture (before every exclusion: the seat marker, the pass list,
#: one-per-sender, auto-accept) as CK3AGENT_PROPOSAL_SENT|<kind>|<class>|<actor>|
#: <recipient>. Nothing reads it but the live scenario (the bridge's proposal_lines
#: keep only HELD/RESOLVED/VOID): it tells "the AI sent no peace the capture could
#: hold" (SENT lines, none HELD) from "the game runs no created on_send for the
#: hidden end_war_*" (wars ended at a score the engine does not auto-accept, and
#: no SENT line). A class in it must be in CLASSES (test_proposal_overrides).
SENT_LOGGED_CLASSES: tuple[str, ...] = ("peace",)

#: Special interactions the ENGINE sends for the AI (no ai_targets block in
#: script): the marriage AI and the war AI's call to allies. A census that read
#: only ai_targets would call them "not AI-sendable" and drop the two offers that
#: matter most, so they are named here and the census says which rule matched.
ENGINE_SENT_SPECIALS = frozenset({"arrange_marriage_interaction", "call_ally_interaction"})

CAPTURE = "ck3agent_proposal_capture_effect = {{ KIND = {key} CLASS = {cls} }}"
NOT_HELD = "ck3agent_proposal_not_held_trigger = {{ CLASS = {cls} }}"
OPTION_VALID = "ck3agent_proposal_option_trigger = {{ FLAG = {flag} }}"
OPTION_SELECTED = "ck3agent_proposal_option_selected_trigger = {{ FLAG = {flag} }}"
OPTION_WRAP_OPEN = ("trigger_if = {{ limit = {{ ck3agent_proposal_sending_trigger = yes }} "
                    "ck3agent_proposal_option_selected_trigger = {{ FLAG = {flag} }} }} trigger_else = {{")
NOT_SENDING = "ck3agent_proposal_not_sending_trigger = yes"
ON_SEND_GUARD_OPEN = "if = { limit = { ck3agent_proposal_not_answering_trigger = yes }"
CLOSE = "}"

#: The generated files, relative to the mod root.
GEN_EFFECTS = "common/scripted_effects/zz_ck3agent_proposal_kinds.txt"
GEN_TRIGGERS = "common/scripted_triggers/zz_ck3agent_proposal_kinds.txt"
INTERACTIONS_DIR = "common/character_interactions"
MANIFEST = "proposal_manifest.json"
BOM = "﻿"


class Refused(Exception):
    def __init__(self, code: str, detail: str = ""):
        super().__init__(f"{code}: {detail}" if detail else code)
        self.code, self.detail = code, detail


# ── reading ──────────────────────────────────────────────────────────────────

_TOKEN = re.compile(r'#[^\n]*|"(?:\\.|[^"\\\n])*"|\?=|<=|>=|!=|==|[{}=<>]|[^\s{}=<>#"!?]+|[!?]')
_OPS = {"=", "?=", "<=", ">=", "!=", "==", "<", ">"}
_IDENT = re.compile(r"^[a-z][a-z0-9_]*$")


@dataclass
class Node:
    key: Optional[str]          # None for a bare value or an anonymous block
    start: int                  # offset of the key (or the bare value)
    open: int = -1              # offset of "{" (blocks only)
    close: int = -1             # offset of the matching "}" (blocks only)
    value: Optional[str] = None # scalar value
    children: list = field(default_factory=list)

    @property
    def is_block(self) -> bool:
        return self.open >= 0


def parse(text: str) -> list[Node]:
    """Top-level nodes of TEXT, offsets into TEXT. Unbalanced braces refuse."""
    toks = [(m.group(0), m.start()) for m in _TOKEN.finditer(text) if not m.group(0).startswith("#")]
    pos = 0

    def block(depth: int) -> tuple[list[Node], int]:
        nonlocal pos
        out: list[Node] = []
        while pos < len(toks):
            t, s = toks[pos]
            if t == "}":
                if depth == 0:
                    raise Refused("unbalanced_braces", f"stray '}}' at {s}")
                pos += 1
                return out, s
            if pos + 1 < len(toks) and toks[pos + 1][0] in _OPS and t not in _OPS and t != "{":
                key = t
                pos += 2
                if pos >= len(toks):
                    raise Refused("truncated", f"no value for {key!r}")
                v, vs = toks[pos]
                if v == "{":
                    pos += 1
                    kids, close = block(depth + 1)
                    out.append(Node(key, s, vs, close, None, kids))
                else:
                    pos += 1
                    out.append(Node(key, s, value=v))
            elif t == "{":
                pos += 1
                kids, close = block(depth + 1)
                out.append(Node(None, s, s, close, None, kids))
            else:
                pos += 1
                out.append(Node(None, s, value=t))
        if depth:
            raise Refused("unbalanced_braces", "end of text inside a block")
        return out, len(text)

    nodes, _ = block(0)
    return nodes


def child(node: Node, key: str) -> list[Node]:
    return [c for c in node.children if c.key == key]


def interactions(text: str) -> dict[str, list[Node]]:
    """key -> its definitions in TEXT (top-level blocks, @constants excluded)."""
    out: dict[str, list[Node]] = {}
    for n in parse(text):
        if n.is_block and n.key and not n.key.startswith("@"):
            out.setdefault(n.key, []).append(n)
    return out


def _scalar(n: Node, k: str) -> Optional[str]:
    c = child(n, k)
    if not c:
        return None
    return "block" if c[0].is_block else c[0].value


# ── census ───────────────────────────────────────────────────────────────────

def row(file: str, n: Node) -> dict:
    auto = _scalar(n, "auto_accept")
    decides = bool(child(n, "ai_accept")) and auto != "yes"
    ai_rule = ("ai_targets" if child(n, "ai_targets") else
               "engine_sent_special" if n.key in ENGINE_SENT_SPECIALS else None)
    return {
        "file": file, "key": n.key, "category": _scalar(n, "category"),
        "recipient_decides": decides, "auto_accept": auto,
        "ai_sendable": ai_rule is not None, "ai_rule": ai_rule,
        "special": _scalar(n, "special_interaction"),
        "reply_days": [_scalar(n, "ai_min_reply_days"), _scalar(n, "ai_max_reply_days")],
        "send_options": [o.value for s in child(n, "send_option") for o in child(s, "flag")],
        "first_set": FIRST_SET.get(n.key, (None, None))[0],
    }


def census(folder: Path) -> list[dict]:
    rows = []
    for f in sorted(Path(folder).glob("*.txt")):
        text = f.read_text(encoding="utf-8-sig")
        for key, defs in interactions(text).items():
            rows.append(row(f.name, defs[-1]))
    return rows


# ── injection ────────────────────────────────────────────────────────────────

def _line_end(text: str, at: int) -> int:
    """Offset just past the newline that ends the line holding AT."""
    e = text.find("\n", at)
    if e < 0:
        raise Refused("no_newline", f"line at {at} has no end")
    return e + 1


def _line_start(text: str, at: int) -> int:
    return text.rfind("\n", 0, at) + 1


def _indent(text: str, at: int) -> str:
    s = _line_start(text, at)
    return re.match(r"[ \t]*", text[s:]).group(0)


def _nl(text: str) -> str:
    return "\r\n" if "\r\n" in text else "\n"


def _head_insert(text: str, blk: Node, stmt: str) -> tuple[int, str]:
    """A whole line as the first statement of BLK."""
    if "\n" not in text[blk.open:blk.close]:
        raise Refused("one_line_block", f"{blk.key} at {blk.start} opens and closes on one line")
    at = _line_end(text, blk.open)
    return at, f"{_indent(text, blk.start)}\t{stmt} {MARK}{_nl(text)}"


def _tail_insert(text: str, blk: Node, stmt: str) -> tuple[int, str]:
    """A whole line as the last statement of BLK (its closing brace on its own line)."""
    if "\n" not in text[blk.open:blk.close]:
        raise Refused("one_line_block", f"{blk.key} at {blk.start} opens and closes on one line")
    s = _line_start(text, blk.close)
    if text[s:blk.close].strip():
        raise Refused("closing_brace_shares_line", f"{blk.key} at {blk.start}")
    return s, f"{_indent(text, blk.start)}\t{stmt} {MARK}{_nl(text)}"


def _new_block(text: str, obj: Node, key: str, stmt: str) -> tuple[int, str]:
    """A whole line `key = { stmt }` just before OBJ's closing brace line."""
    s = _line_start(text, obj.close)
    if text[s:obj.close].strip():
        raise Refused("closing_brace_shares_line", f"{obj.key}")
    return s, f"{_indent(text, obj.start)}\t{key} = {{ {stmt} }} {MARK}{_nl(text)}"


def _code(text: str, blk: Node) -> str:
    """BLK's body with comments dropped (a '#' inside a quoted string survives)."""
    body = text[blk.open + 1:blk.close]
    return "".join(m.group(0) if not m.group(0).startswith("#") else ""
                   for m in re.finditer(r'#[^\n]*|"(?:\\.|[^"\\\n])*"|[^#"]+|"', body))


def _option_edits(text: str, obj: Node) -> list[tuple[int, str]]:
    """Shape 3: the send-option lines of OBJ."""
    exclusive = _scalar(obj, "send_options_exclusive") != "no"   # absent: exclusive
    edits = []
    for so in child(obj, "send_option"):
        if not so.is_block:
            raise Refused("send_option_not_a_block", obj.key)
        flags = [c for c in child(so, "flag") if not c.is_block]
        if len(flags) != 1 or not _IDENT.match(flags[0].value or ""):
            raise Refused("send_option_without_plain_flag", f"{obj.key} at {so.start}")
        flag = flags[0].value
        if exclusive:
            valid = [c for c in child(so, "is_valid") if c.is_block]
            if child(so, "is_valid") and not valid:
                raise Refused("scalar_where_block", f"{obj.key}.{flag}.is_valid")
            edits.append(_head_insert(text, valid[0], OPTION_VALID.format(flag=flag)) if valid
                         else _new_block(text, so, "is_valid", OPTION_VALID.format(flag=flag)))
            continue
        se = child(so, "starts_enabled")
        if se and not se[0].is_block:
            raise Refused("scalar_where_block", f"{obj.key}.{flag}.starts_enabled")
        if se:
            edits.append(_head_insert(text, se[0], OPTION_WRAP_OPEN.format(flag=flag)))
            edits.append(_tail_insert(text, se[0], CLOSE))
        else:
            edits.append(_new_block(text, so, "starts_enabled", OPTION_SELECTED.format(flag=flag)))
        cb = child(so, "can_be_changed")
        if cb and not cb[0].is_block:
            raise Refused("scalar_where_block", f"{obj.key}.{flag}.can_be_changed")
        edits.append(_head_insert(text, cb[0], NOT_SENDING) if cb
                     else _new_block(text, so, "can_be_changed", NOT_SENDING))
    return edits


def inject(text: str, kinds: dict[str, tuple[str, str]] = FIRST_SET) -> tuple[str, list[str]]:
    """TEXT with the added lines in every interaction of KINDS it defines.
    Returns (copy, the keys injected). A KINDS key defined twice in TEXT refuses
    (the game keeps one, and a copy cannot say which without the load order)."""
    if MARK in text:
        raise Refused("source_already_marked", "the source holds the generator's mark")
    defs = interactions(text)
    edits: list[tuple[int, str]] = []
    done: list[str] = []
    for key, (cls, _family) in kinds.items():
        if key not in defs:
            continue
        if len(defs[key]) > 1:
            raise Refused("defined_twice", key)
        obj = defs[key][0]
        capture = CAPTURE.format(key=key, cls=cls)
        sends = [c for c in child(obj, "on_send") if c.is_block]
        if child(obj, "on_send") and not sends:
            raise Refused("scalar_where_block", f"{key}.on_send")
        if sends:
            edits.append(_head_insert(text, sends[0], capture))
            if _code(text, sends[0]).strip():
                # shape 4: the vanilla body runs unless this is our re-send
                edits.append(_head_insert(text, sends[0], ON_SEND_GUARD_OPEN))
                edits.append(_tail_insert(text, sends[0], CLOSE))
        else:
            edits.append(_new_block(text, obj, "on_send", capture))
        held = NOT_HELD.format(cls=cls)
        valid = ([c for c in child(obj, "is_valid_showing_failures_only") if c.is_block]
                 or [c for c in child(obj, "is_valid") if c.is_block])
        edits.append(_head_insert(text, valid[0], held) if valid
                     else _new_block(text, obj, "is_valid_showing_failures_only", held))
        edits += _option_edits(text, obj)
        done.append(key)
    # Lines at one offset keep the order they were listed in.
    for (at, line), _ in sorted(((e, i) for i, e in enumerate(edits)), key=lambda x: (x[0][0], x[1]),
                                reverse=True):
        text = text[:at] + line + text[at:]
    return text, done


def strip_marked(text: str) -> str:
    """The copy without every line the generator added."""
    return "".join(l for l in text.splitlines(keepends=True) if not l.rstrip("\r\n").endswith(MARK))


def strip_marked_bytes(raw: bytes) -> bytes:
    return b"".join(l for l in raw.splitlines(keepends=True)
                    if not l.rstrip(b"\r\n").endswith(MARK.encode()))


#: The only shapes an added line may have (the inertness argument enumerates them).
_I = r"[a-z_0-9]+"
ALLOWED_LINE = re.compile(
    r"^\s*(?:"
    # 1 capture
    r"(?:on_send = \{ )?ck3agent_proposal_capture_effect = \{ KIND = " + _I + r" CLASS = " + _I + r" \}(?: \})?"
    # 2 hold
    r"|(?:is_valid_showing_failures_only = \{ )?ck3agent_proposal_not_held_trigger = \{ CLASS = " + _I + r" \}(?: \})?"
    # 3 options
    r"|(?:is_valid = \{ )?ck3agent_proposal_option_trigger = \{ FLAG = " + _I + r" \}(?: \})?"
    r"|starts_enabled = \{ ck3agent_proposal_option_selected_trigger = \{ FLAG = " + _I + r" \} \}"
    r"|trigger_if = \{ limit = \{ ck3agent_proposal_sending_trigger = yes \} "
    r"ck3agent_proposal_option_selected_trigger = \{ FLAG = " + _I + r" \} \} trigger_else = \{"
    r"|(?:can_be_changed = \{ )?ck3agent_proposal_not_sending_trigger = yes(?: \})?"
    # 4 on_send guard
    r"|if = \{ limit = \{ ck3agent_proposal_not_answering_trigger = yes \}"
    # the close of a wrap (3, 4)
    r"|\}"
    r") " + re.escape(MARK) + r"$")


def added_lines(copy: str) -> list[str]:
    return [l.rstrip("\r\n") for l in copy.splitlines(keepends=True) if l.rstrip("\r\n").endswith(MARK)]


# ── lifting ──────────────────────────────────────────────────────────────────

LIFTED = ("on_accept", "on_decline", "auto_accept")
#: The interaction's own script conditions, every trigger of the engine's "Can Send"
#: test (_character_interactions.info, "How and when are the character interaction
#: triggers checked?": is_shown, is_valid_showing_failures_only,
#: has_valid_target_showing_failures_only, can_send, is_valid, has_valid_target;
#: the rest of that test is engine code: diplomatic range, the special
#: interaction's own checks, "already being considered"). Lifted and ANDed, they
#: are checked immediately before any answer is carried out.
VALIDITY = ("is_shown", "is_valid", "is_valid_showing_failures_only",
            "has_valid_target", "has_valid_target_showing_failures_only", "can_send")


def file_constants(text: str) -> dict[str, str]:
    """The file's top-level numeric @constants (a constant naming another is
    followed); one that resolves to no number is left out, so a body using it
    refuses."""
    raw = {n.key: n.value for n in parse(text) if n.key and n.key.startswith("@") and not n.is_block}

    def resolve(name: str, seen: frozenset = frozenset()) -> Optional[str]:
        v = raw.get(name)
        if v is None or name in seen:
            return None
        if re.match(r"^-?\d+(\.\d+)?$", v):
            return v
        return resolve(v, seen | {name}) if v.startswith("@") else None
    return {k: v for k in raw if (v := resolve(k)) is not None}


def _lift_body(text: str, key: str, part: str, blk: Node) -> str:
    """BLK's code, comments dropped and the file's @constants put in (a lifted
    body lives in another file, where they are not defined)."""
    code = _code(text, blk)
    if re.search(r"(?<![\w:.])root\b", code):
        raise Refused("lifted_body_reads_root", f"{key}.{part}")
    consts = file_constants(text)
    missing = []

    def put(m: re.Match) -> str:
        t = m.group(0)
        if not t.startswith("@"):
            return t
        if t not in consts:
            missing.append(t)
            return t
        return consts[t]
    code = re.sub(r'"(?:\\.|[^"\\\n])*"|@[A-Za-z_]\w*', put, code)
    if missing:
        raise Refused("lifted_body_uses_unknown_constant", f"{key}.{part}: {missing[0]}")
    return code


def lift(text: str, key: str) -> dict[str, Optional[str]]:
    """KEY's bodies, comments dropped. A body that reads `root` refuses."""
    defs = interactions(text).get(key)
    if not defs:
        raise Refused("key_absent", key)
    obj = defs[-1]
    out: dict[str, Optional[str]] = {}
    for k in LIFTED:
        blocks = [c for c in child(obj, k) if c.is_block]
        out[k] = _lift_body(text, key, k, blocks[0]) if blocks else None
    return out


def lift_validity(text: str, key: str) -> list[str]:
    """KEY's validity bodies (VALIDITY order), each to be ANDed."""
    obj = interactions(text)[key][-1]
    out = []
    for k in VALIDITY:
        for b in child(obj, k):
            if not b.is_block:
                raise Refused("scalar_where_block", f"{key}.{k}")
            out.append(_lift_body(text, key, k, b))
    return out


def option_flags(text: str, key: str) -> list[str]:
    obj = interactions(text)[key][-1]
    return [c.value for so in child(obj, "send_option") for c in child(so, "flag")
            if not c.is_block and _IDENT.match(c.value or "")]


# ── a whole folder ───────────────────────────────────────────────────────────

def check(folder: Path, kinds: dict[str, tuple[str, str]] = FIRST_SET) -> dict:
    """Inject every file of FOLDER that defines a KINDS key; report per file and
    refuse when a KINDS key is found nowhere or in two files."""
    where: dict[str, list[str]] = {}
    files = {}
    for f in sorted(Path(folder).glob("*.txt")):
        raw = f.read_bytes()
        text = raw.decode("utf-8-sig")
        copy, done = inject(text, kinds)
        if not done:
            continue
        for k in done:
            where.setdefault(k, []).append(f.name)
        bad = [l for l in added_lines(copy) if not ALLOWED_LINE.match(l)]
        for k in done:
            lift(text, k)
            lift_validity(text, k)
        files[f.name] = {"keys": done, "added_lines": len(added_lines(copy)),
                         "strip_equals_source": strip_marked(copy) == text,
                         "disallowed_lines": bad, "objects_in_file": len(interactions(text))}
    missing = sorted(set(kinds) - set(where))
    twice = sorted(k for k, v in where.items() if len(v) > 1)
    if missing:
        raise Refused("first_set_key_absent", ", ".join(missing))
    if twice:
        raise Refused("first_set_key_in_two_files", ", ".join(twice))
    return {"files": files, "overridden_files": sorted(files)}


# ── the generated new-key files ──────────────────────────────────────────────

def _chain(items: list[tuple[str, str]], fallback: str = "") -> str:
    """if / else_if over (limit, body) pairs, with an optional else."""
    out = []
    for i, (limit, body) in enumerate(items):
        out.append(f"\t{'else_if' if i else 'if'} = {{\n\t\tlimit = {{ {limit} }}\n{body}\t}}\n")
    if fallback and items:
        out.append(f"\telse = {{\n{fallback}\t}}\n")
    elif fallback:
        out.append(fallback)
    return "".join(out)


def _is_kind(key: str) -> str:
    return f"var:ck3agent_proposal_kind = flag:{key}"


def _is_class(cls: str) -> str:
    return f"var:ck3agent_proposal_class = flag:{cls}"


#: What a log line prints. Live runs i11p3-i11p9: a debug_log string sees the
#: saved scopes the CONTEXT began with (the interaction's actor and recipient in
#: on_send: GetID printed the bridge's ids) and nothing the script saved during the
#: run: SCOPE.ScriptValue('x') is a script value DEFINITION (printed 0), a variable
#: on sC('recipient') printed in on_send and 0 in the list and the story's re-check,
#: and sC('actor') printed 4294967295 in the list although the scope dump showed it
#: saved. A GLOBAL variable read with GetGlobalVariable('x') printed the right
#: values in the list (i11p9: DIAG|forms|1|36122). So each line first copies what it
#: prints into global variables (named ck3a_*; set only while a marked seat's offer
#: is logged) and prints those.
PRINTED_VALUES = ("ck3a_pid", "ck3a_answer", "ck3a_by", "ck3a_void", "ck3a_age", "ck3a_y", "ck3a_m", "ck3a_d")
PRINTED_CHARACTERS = ("actor", "recipient", "secondary_actor", "secondary_recipient")


def _sv(name: str) -> str:
    if name not in PRINTED_VALUES:
        raise ValueError(f"{name} is not a printed value")
    return f"[GetGlobalVariable('{name}').GetValue|0]"


def _ch(scope: str) -> str:
    if scope not in PRINTED_CHARACTERS:
        raise ValueError(f"{scope} is not a printed character")
    return f"[GetGlobalVariable('ck3a_c_{scope}').GetCharacter.GetID]"


def carry_lines(line: str, indent: str = "\t\t") -> str:
    """The set_global_variable lines that put every value and character LINE prints
    where the line can read it (a character absent from the scopes is removed, so
    the line never prints a stale one)."""
    out = []
    for n in dict.fromkeys(re.findall(r"GetGlobalVariable\('([a-z0-9_]+)'\)", line)):
        if n.startswith("ck3a_c_"):
            sc = n[len("ck3a_c_"):]
            out.append(f"{indent}if = {{ limit = {{ exists = scope:{sc} }} set_global_variable = "
                       f"{{ name = {n} value = scope:{sc} }} }} else = {{ remove_global_variable = {n} }}\n")
        else:
            out.append(f"{indent}set_global_variable = {{ name = {n} value = scope:{n} }}\n")
    return "".join(out)


def _logged(line: str) -> str:
    """LINE preceded by the copy of what it prints."""
    return carry_lines(line) + line


DATE = f"{_sv('ck3a_y')}.{_sv('ck3a_m')}.{_sv('ck3a_d')}"


def kinds_files(lifted: dict[str, dict], kinds: dict[str, tuple[str, str]] = FIRST_SET) -> dict[str, str]:
    """The two generated new-key files' text. LIFTED: key -> {on_accept,
    on_decline, auto_accept, validity: [bodies], flags: [send option flags]}."""
    head = ("# GENERATED at install by scripts/proposal_overrides.py from the installed game's\n"
            "# common/character_interactions. Never edit, never ship (lane QMOD).\n")
    classes = list(dict.fromkeys(c for c, _ in kinds.values()))
    eff, trg = [head], [head]
    for key, (cls, family) in kinds.items():
        b = lifted[key]
        trg.append(f"ck3agent_proposal_k_{key}_auto_accept_trigger = {{\n"
                   f"{b['auto_accept'] if b['auto_accept'] is not None else chr(9) + 'always = no' + chr(10)}}}\n")
        valid = "".join(f"\tAND = {{\n{v}\n\t}}\n" for v in b["validity"]) or "\talways = yes\n"
        trg.append(f"ck3agent_proposal_k_{key}_valid_trigger = {{\n{valid}}}\n")
        eff.append(f"ck3agent_proposal_k_{key}_decline_effect = {{\n{b['on_decline'] or ''}\n}}\n")
        if family != "resend":
            eff.append(f"ck3agent_proposal_k_{key}_accept_effect = {{\n{b['on_accept'] or ''}\n}}\n")
    # the kind's own conditions (story scope, scopes and options restored); the
    # lifted bodies are evaluated with the ACTOR as root, as the engine does
    # ("Root is scope:actor for the interaction", _character_interactions.info)
    trg.append("ck3agent_proposal_kind_valid_trigger = {\n" + "".join(
        f"\t{'trigger_else_if' if i else 'trigger_if'} = {{\n\t\tlimit = {{ {_is_kind(k)} }}\n"
        f"\t\tscope:actor = {{ ck3agent_proposal_k_{k}_valid_trigger = yes }}\n\t}}\n" for i, k in enumerate(kinds))
        + "\ttrigger_else = { always = no }\n}\n")

    def effect(name: str, items, fallback: str = "") -> None:
        eff.append(f"{name} = {{\n{_chain(items, fallback)}}}\n")

    effect("ck3agent_proposal_set_family_effect", [
        (_is_kind(k), f"\t\tset_variable = {{ name = ck3agent_proposal_family value = flag:{f} }}\n")
        for k, (_c, f) in kinds.items()])
    effect("ck3agent_proposal_capture_options_effect", [
        (_is_kind(k), "".join(
            f"\t\tif = {{ limit = {{ scope:{fl} ?= yes }} add_to_variable_list = "
            f"{{ name = ck3agent_proposal_options target = flag:{fl} }} }}\n" for fl in lifted[k]["flags"]))
        for k in kinds if lifted[k]["flags"]])
    effect("ck3agent_proposal_restore_options_effect", [
        (_is_kind(k), "".join(
            f"\t\tsave_scope_value_as = {{ name = {fl} value = no }}\n"
            f"\t\tif = {{ limit = {{ is_target_in_variable_list = {{ name = ck3agent_proposal_options "
            f"target = flag:{fl} }} }} save_scope_value_as = {{ name = {fl} value = yes }} }}\n"
            for fl in lifted[k]["flags"]))
        for k in kinds if lifted[k]["flags"]])
    effect("ck3agent_proposal_copy_options_effect", [
        (_is_kind(k), "".join(
            f"\t\tif = {{ limit = {{ is_target_in_variable_list = {{ name = ck3agent_proposal_options "
            f"target = flag:{fl} }} }} scope:actor = {{ add_to_variable_list = {{ name = ck3agent_proposal_options "
            f"target = flag:{fl} }} }} }}\n" for fl in lifted[k]["flags"]))
        for k in kinds if lifted[k]["flags"]])
    effect("ck3agent_proposal_log_options_effect", [
        (_is_kind(k), "".join(
            f"\t\tif = {{ limit = {{ is_target_in_variable_list = {{ name = ck3agent_proposal_options "
            f"target = flag:{fl} }} }} debug_log = \"CK3AGENT_PROPOSAL_ITEM_OPTION|{fl}\" }}\n"
            for fl in lifted[k]["flags"]))
        for k in kinds if lifted[k]["flags"]])
    actor, recip = _ch("actor"), _ch("recipient")
    effect("ck3agent_proposal_log_held_effect", [
        (_is_kind(k), _logged(f"\t\tdebug_log = \"CK3AGENT_PROPOSAL_HELD|{_sv('ck3a_pid')}|{k}|{c}|{actor}|{recip}|{DATE}\"\n")
                      + "\t\tdebug_log_scopes = yes\n")
        for k, (c, _f) in kinds.items()])
    effect("ck3agent_proposal_log_resolved_effect", [
        (_is_kind(k), _logged(f"\t\tdebug_log = \"CK3AGENT_PROPOSAL_RESOLVED|{_sv('ck3a_pid')}|{_sv('ck3a_answer')}|"
                              f"{_sv('ck3a_by')}|{k}|{c}|{DATE}\"\n"))
        for k, (c, _f) in kinds.items()])
    effect("ck3agent_proposal_log_void_effect", [
        (_is_kind(k), _logged(f"\t\tdebug_log = \"CK3AGENT_PROPOSAL_VOID|{_sv('ck3a_pid')}|{_sv('ck3a_void')}|{k}|{c}|{DATE}\"\n"))
        for k, (c, _f) in kinds.items()])
    effect("ck3agent_proposal_log_item_effect", [
        (_is_kind(k), _logged(f"\t\tdebug_log = \"CK3AGENT_PROPOSAL_ITEM|{_sv('ck3a_pid')}|{k}|{c}|{actor}|{recip}|"
                              f"{_sv('ck3a_age')}\"\n"))
        for k, (c, _f) in kinds.items()])
    # L8 (SENT_LOGGED_CLASSES): one effect per kind, called as the capture's FIRST
    # statement; empty for a kind whose class is not logged. It runs for every send,
    # seated or not, so it leaves no state: the two globals it prints are removed
    # after the line (the save holds nothing of it; only debug.log does).
    for k, (c, _f) in kinds.items():
        body = (_logged(f"\t\tdebug_log = \"CK3AGENT_PROPOSAL_SENT|{k}|{c}|{actor}|{recip}\"\n")
                + "\t\tremove_global_variable = ck3a_c_actor\n\t\tremove_global_variable = ck3a_c_recipient\n"
                if c in SENT_LOGGED_CLASSES else "")
        eff.append(f"ck3agent_proposal_k_{k}_sent_effect = {{\n{body}}}\n")
    effect("ck3agent_proposal_release_effect", [
        (_is_class(c), f"\t\tscope:recipient ?= {{ remove_list_variable = {{ name = ck3agent_proposal_held_{c} "
                       "target = scope:actor } }\n")
        for c in classes])
    effect("ck3agent_proposal_story_policy_effect", [
        (_is_class(c), f"\t\tck3agent_proposal_read_policy_effect = {{ CLASS = {c} }}\n") for c in classes])
    lapse_body = lambda c: (  # noqa: E731
        f"\t\tsave_scope_value_as = {{ name = ck3a_lapse value = ck3agent_proposal_lapse_{c} }}\n"
        "\t\tif = { limit = { scope:ck3a_lapse = 1 } ck3agent_proposal_answer_effect = { ANSWER = 1 BY = 4 } }\n"
        "\t\telse_if = { limit = { scope:ck3a_lapse = 2 } ck3agent_proposal_answer_effect = { ANSWER = 2 BY = 4 } }\n"
        "\t\telse = { ck3agent_proposal_answer_effect = { ANSWER = 4 BY = 4 } }\n")
    effect("ck3agent_proposal_lapse_effect", [(_is_class(c), lapse_body(c)) for c in classes])
    effect("ck3agent_proposal_dispatch_resend_effect", [
        (_is_kind(k), f"\t\tck3agent_proposal_run_effect = {{ KIND = {k} THRESHOLD = $THRESHOLD$ }}\n")
        for k, (_c, f) in kinds.items() if f == "resend"])
    acc = []
    for k, (_c, f) in kinds.items():
        if f == "resend":
            acc.append((_is_kind(k), f"\t\tck3agent_proposal_run_effect = {{ KIND = {k} THRESHOLD = execute_threshold }}\n"))
        elif f == "join_war":
            # the lifted on_accept joins the war itself (call_ally_interaction_effect
            # adds the recipient on the caller's side), in the sender's scope
            acc.append((_is_kind(k), f"\t\tscope:actor = {{ ck3agent_proposal_k_{k}_accept_effect = yes }}\n"))
        else:
            # the lifted on_accept reads the war's casus_belli and ends the war itself
            acc.append((_is_kind(k), f"\t\tscope:war = {{ ck3agent_proposal_k_{k}_accept_effect = yes }}\n"))
    effect("ck3agent_proposal_dispatch_accept_effect", acc)
    effect("ck3agent_proposal_dispatch_decline_effect", [
        (_is_kind(k), f"\t\tscope:actor = {{ ck3agent_proposal_k_{k}_decline_effect = yes }}\n") for k in kinds])
    return {GEN_EFFECTS: BOM + "\n".join(eff), GEN_TRIGGERS: BOM + "\n".join(trg)}


def _sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def game_dirs(install: Path) -> tuple[Path, Path]:
    """(character_interactions dir, launcher-settings.json) of an install root
    (the directory holding game/ and launcher/)."""
    install = Path(install)
    return (install / "game" / INTERACTIONS_DIR, install / "launcher" / "launcher-settings.json")


def game_version(install: Path) -> str:
    _, ls = game_dirs(install)
    try:
        v = json.loads(ls.read_text(encoding="utf-8-sig")).get("rawVersion")
    except (OSError, ValueError) as e:
        raise Refused("game_version_unreadable", f"{ls}: {e}")
    if not isinstance(v, str) or not re.match(r"^\d+(\.\d+)+$", v):
        raise Refused("game_version_unreadable", f"{ls}: rawVersion {v!r}")
    return v


def source_shas(install: Path, names) -> dict[str, Optional[str]]:
    folder, _ = game_dirs(install)
    out = {}
    for n in names:
        p = folder / Path(n).name
        out[f"{INTERACTIONS_DIR}/{Path(n).name}"] = _sha(p.read_bytes()) if p.is_file() else None
    return out


def generate(install: Path, kinds: dict[str, tuple[str, str]] = FIRST_SET) -> tuple[dict[str, bytes], dict]:
    """Every generated file (relpath under the mod root -> bytes) and the
    manifest, from the installed game at INSTALL. Refuses rather than writes a
    copy that is not the source plus enumerated lines."""
    folder, _ = game_dirs(install)
    if not folder.is_dir():
        raise Refused("game_interactions_missing", str(folder))
    version = game_version(install)
    rep = check(folder, kinds)
    out: dict[str, bytes] = {}
    lifted: dict[str, dict] = {}
    sources = {}
    for name in rep["overridden_files"]:
        raw = (folder / name).read_bytes()
        bom = raw.startswith(BOM.encode())
        text = raw.decode("utf-8-sig")
        copy, done = inject(text, kinds)
        data = (BOM if bom else "").encode() + copy.encode("utf-8")
        if strip_marked_bytes(data) != raw:
            raise Refused("strip_differs_from_source", name)
        bad = [l for l in added_lines(copy) if not ALLOWED_LINE.match(l)]
        if bad:
            raise Refused("disallowed_line", f"{name}: {bad[0]}")
        out[f"{INTERACTIONS_DIR}/{name}"] = data
        sources[f"{INTERACTIONS_DIR}/{name}"] = _sha(raw)
        for k in done:
            b = lift(text, k)
            b["validity"] = lift_validity(text, k)
            b["flags"] = option_flags(text, k)
            lifted[k] = b
    for rel, t in kinds_files(lifted, kinds).items():
        out[rel] = t.encode("utf-8")
    manifest = {
        "generator": "scripts/proposal_overrides.py",
        "game_raw_version": version,
        "first_set": {k: list(v) for k, v in kinds.items()},
        "sources": sources,
        "generated": {rel: _sha(b) for rel, b in sorted(out.items())},
        "added_lines": {rel: len(added_lines(b.decode("utf-8-sig"))) for rel, b in sorted(out.items())
                        if rel.startswith(INTERACTIONS_DIR)},
    }
    return out, manifest


def main(argv: list[str]) -> int:
    if not argv or argv[0] not in ("census", "check", "generate") or \
            len(argv) != (3 if argv[0] == "generate" else 2):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        if argv[0] == "census":
            out = census(Path(argv[1]))
        elif argv[0] == "check":
            out = check(Path(argv[1]))
        else:
            files, out = generate(Path(argv[1]))
            dst = Path(argv[2])
            for rel, b in files.items():
                (dst / rel).parent.mkdir(parents=True, exist_ok=True)
                (dst / rel).write_bytes(b)
            (dst / MANIFEST).write_text(json.dumps(out, indent=1, sort_keys=True), encoding="utf-8")
    except Refused as e:
        print(json.dumps({"refused": e.code, "detail": e.detail}))
        return 1
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
