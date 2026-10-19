"""lane-scheme 2026-10-18: the default-off write gate, read from the code.

The persona test (release/REVIEW-2026.10.0-persona-gamer.md, finding 10) found
`do(intent="scheme")` refused `experimental_write_disabled` by an allow-list
that no public doc named, with a summary pointing at a field the reply did not
carry. Three places now state the gate, and these tests make them agree:

  - THE GATE: `kExperimental` in bridge/src/dispatch_router.cpp, read out of
    the source (comments stripped, so a commented-out name is not counted);
  - THE SEAT: redesign.EXPERIMENTAL_WRITES, the server's mirror;
  - THE OPERATOR: the "Default-off writes" table in docs/research.md.

Any one moving without the others reds. The tests also hold the scheme
refusal vocabulary to the code: every reason string the scheme handlers can
emit is enumerated from their bodies and must resolve to a typed gate, so a
new reject() spelling reds here instead of reaching the seat as `unknown`.

Runs in the source tree (release/overlay/docs) and in the export (docs/).
"""

import os
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(__file__))

import redesign  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BRIDGE_SRC = ROOT / "bridge" / "src"


def _doc() -> str:
    for rel in ("release/overlay/docs/research.md", "docs/research.md"):
        p = ROOT / rel
        if p.is_file():
            return p.read_text(encoding="utf-8")
    pytest.fail("research.md not found in the source or the export layout")


def _strip_c_comments(src: str) -> str:
    """Drop // and /* */ comments, leaving string literals intact."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _function_body(src: str, signature_re: str) -> str:
    """The brace-matched body of the first function whose header matches."""
    m = re.search(signature_re, src)
    assert m, f"function not found: {signature_re}"
    i = src.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError(f"unbalanced body: {signature_re}")


def _gate_list_from_bridge() -> list:
    src = _strip_c_comments((BRIDGE_SRC / "dispatch_router.cpp").read_text(encoding="utf-8"))
    m = re.search(r"kExperimental\[\]\s*=\s*\{(.*?)\};", src, re.S)
    assert m, "kExperimental[] not found in dispatch_router.cpp"
    names = re.findall(r'"([^"]+)"', m.group(1))
    assert names, "kExperimental[] parsed empty; the parser, not the gate, is broken"
    return names


def _gate_list_from_doc() -> list:
    doc = _doc()
    m = re.search(r"^## Default-off writes\s*$(.*?)(?=^## )", doc, re.S | re.M)
    assert m, "docs/research.md has no '## Default-off writes' section"
    rows = [ln for ln in m.group(1).splitlines() if ln.startswith("| `")]
    names = [re.match(r"\| `([^`]+)`", ln).group(1) for ln in rows]
    assert names, "the Default-off writes table has no rows"
    return names


def test_the_bridge_gate_and_the_server_mirror_agree():
    assert sorted(_gate_list_from_bridge()) == sorted(redesign.EXPERIMENTAL_WRITES)


def test_the_operator_docs_list_exactly_the_gated_writes():
    assert sorted(_gate_list_from_doc()) == sorted(_gate_list_from_bridge())


def test_the_docs_name_the_file_and_the_reply_field():
    """The operator needs the file's name, where it lives, and how to find it
    from a refusal: the docs must name the file, the reply field, and say it
    is re-read without a relaunch (the property that makes arming safe to do
    on a running game)."""
    m = re.search(r"^## Default-off writes\s*$(.*?)(?=^## )", _doc(), re.S | re.M)
    section = m.group(1)
    bridge_src = (BRIDGE_SRC / "dispatch_router.cpp").read_text(encoding="utf-8")
    name = re.search(r'kEnabledWritesName\s*=\s*"([^"]+)"', bridge_src).group(1)
    assert name in section
    assert "enabled_writes_file" in section
    assert "experimental_write_disabled" in section
    assert re.search(r"re-read", section)


def test_the_refusal_carries_the_path_the_bridge_sent():
    """The persona's reply said 'enabled_writes_file in this reply' and did not
    carry it. The envelope's fix now holds the bridge's own path."""
    raw = {"success": False, "reason": "experimental_write_disabled",
           "enabled_writes_file": "/run/user/1000/ck3agent/ck3-enabled-writes"}
    b = redesign.experimental_gate_blocked(raw, "faction_action")
    assert b["gate"] == "experimental_write_disabled"
    assert b["fix"]["enabled_writes_file"] == raw["enabled_writes_file"]
    assert b["fix"]["add_line"] == "faction_action"
    assert b["fix"]["who"] == "operator"
    assert raw["enabled_writes_file"] in b["summary"]
    assert b["retryable"] is False


def test_a_refusal_without_a_path_says_so_rather_than_inventing_one():
    b = redesign.experimental_gate_blocked({"reason": "experimental_write_disabled"},
                                           "faction_action")
    assert b["fix"]["enabled_writes_file"] == ""
    assert "/tmp" not in b["summary"]


# --- the scheme refusal vocabulary, enumerated from the handlers ------------

_SCHEME_FUNCS = (
    ("action_handlers.cpp", r"bool\s+do_dispatch_scheme_intent\s*\([^)]*\)\s*\{"),
    ("intent_dispatch.cpp", r"bool\s+execute_scheme_intent\s*\([^)]*\)\s*\{"),
    ("intent_dispatch.cpp", r"bool\s+execute_scheme_cancel_intent\s*\([^)]*\)\s*\{"),
)

# A reason-shaped literal: snake_case, optionally `code:%fmt...`.
_REASON_RE = re.compile(r'"([a-z][a-z0-9_]{5,})(?::[^"]*)?"')
# Literals in those bodies that are NOT refusal reasons (JSON keys, actions,
# discriminators). Listed, so a new literal must be classified by a human.
_NOT_REASONS = {
    "success", "action", "reason", "actor_id", "target_id", "scheme",
    "scheme_type", "scheme_id", "scheme_action", "intent_request_id",
    "dispatch_id", "result_raw", "cancel", "start", "params",
}


def _scheme_reasons() -> set:
    found = set()
    for fname, sig in _SCHEME_FUNCS:
        body = _strip_c_comments((BRIDGE_SRC / fname).read_text(encoding="utf-8"))
        body = _function_body(body, sig)
        for lit in _REASON_RE.findall(body):
            if lit not in _NOT_REASONS:
                found.add(lit)
    return found


def test_the_enumeration_sees_the_reasons_it_must():
    """Guard against a vacuous green: the parser must find the codes known to
    be there (a regex that matched nothing would pass the test below)."""
    r = _scheme_reasons()
    for must in ("game_paused", "engine_refused_scheme_null_sentinel",
                 "intent_timeout_cancelled_not_executed", "scheme_call_crashed",
                 "not_scheme_owner"):
        assert must in r, (must, sorted(r))


def test_every_scheme_refusal_resolves_to_a_typed_gate():
    unmapped = {}
    for code in sorted(_scheme_reasons()):
        g = redesign.map_raw_reason(code)["gate"]
        if g == "unknown":
            unmapped[code] = g
    assert not unmapped, f"scheme reasons reaching the seat as 'unknown': {unmapped}"


@pytest.mark.parametrize("raw,gate", [
    ("scheme_type_not_found:murdr", "name_not_found"),
    ("scheme_call_crashed:SIGSEGV", "scheme_bridge_fault"),
    ("scheme_type_needs_contract_or_artifact:murder", "scheme_type_unsupported"),
    # "timeout" is a code too; the timeout reasons must not fall to it.
    ("intent_timeout_no_result", "confirmation_pending"),
    ("intent_timeout_cancelled_not_executed", "drain_timeout"),
    ("game_paused", "requires_unpaused"),
])
def test_prefixed_and_timeout_codes_map_on_the_code(raw, gate):
    assert redesign.map_raw_reason(raw)["gate"] == gate


def test_every_new_scheme_gate_has_a_table_row_and_a_why():
    for g in ("scheme_start_refused", "scheme_type_unsupported", "scheme_bridge_fault"):
        assert g in redesign.GATE_TABLE
        assert g in redesign.GATE_WHY


# --- the actor the `do` path sends -------------------------------------------

import asyncio  # noqa: E402


class _Gateway:
    """Bound ruler 12491; records every write."""

    def __init__(self, reply):
        self._reply = reply
        self.writes = []
        self.health = {"ok": True}

    async def multi_actor(self):
        return False

    async def read(self, action, params=None):
        if action == "query_character":
            return {"character": {"id": 12491, "alive": True, "is_player": False,
                                  "name": "Halfdan"}}
        return {}

    async def read_many(self, specs):
        return [{} for _ in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self._reply

    def game(self):
        return {"tick": 1, "game_date": "0867.06.06"}


def _do_scheme(reply, character_id=0, **args):
    gw = _Gateway(reply)
    a = {"scheme_type": "murder", "target_id": 12037}
    a.update(args)
    env = asyncio.run(redesign.do_impl(gw, intent="scheme", character_id=character_id,
                                       args=a))
    return env, gw.writes


def test_a_scheme_with_no_character_id_dispatches_the_bound_ruler():
    """Live 2026-10-18, gate armed: 6/6 do(intent="scheme") calls without a
    character_id were refused actor_id_required, because the server sent
    actor_id 0 and the scheme handler has no controlled-primary fallback."""
    env, writes = _do_scheme({"success": True, "action": "scheme_action",
                              "result_raw": "{}"})
    assert writes and writes[0][0] == "scheme_action"
    assert writes[0][1]["actor_id"] == 12491, writes
    assert writes[0][1]["target_id"] == 12037
    assert writes[0][1]["scheme"] == "murder"


def test_an_explicit_character_id_still_wins():
    env, writes = _do_scheme({"success": True}, character_id=12038)
    assert writes[0][1]["actor_id"] == 12038


def test_the_do_gate_refusal_carries_the_operator_fix():
    env, _ = _do_scheme({"success": False, "action": "scheme_action",
                         "reason": "experimental_write_disabled",
                         "enabled_writes_file": "/x/ck3agent/ck3-enabled-writes"})
    b = env["blocked"]
    assert env["verdict"] == "rejected"
    assert b["gate"] == "experimental_write_disabled"
    assert b["fix"]["enabled_writes_file"] == "/x/ck3agent/ck3-enabled-writes"
    assert b["fix"]["add_line"] == "scheme_action"


def test_the_engine_refusal_reaches_the_seat_typed():
    env, _ = _do_scheme({"success": False, "action": "scheme_action",
                         "reason": "engine_refused_scheme_null_sentinel"})
    assert env["blocked"]["gate"] == "scheme_start_refused"
    assert env["blocked"]["retry"] == "never"


def test_the_explicit_actor_intents_are_exactly_the_handlers_without_a_fallback():
    """The mirror, checked: an intent belongs in _EXPLICIT_ACTOR_INTENTS iff its
    bridge handler refuses actor_id <= 0 instead of resolving the controlled
    primary. Read from the handler bodies."""
    src = _strip_c_comments((BRIDGE_SRC / "action_handlers.cpp").read_text(encoding="utf-8"))
    handlers = {"scheme": r"bool\s+do_dispatch_scheme_intent\s*\([^)]*\)\s*\{",
                "faction": r"bool\s+do_dispatch_faction_intent\s*\([^)]*\)\s*\{"}
    for intent, sig in handlers.items():
        body = _function_body(src, sig)
        assert re.search(r'actor_raw\s*<=\s*0\)\s*\{\s*short_error\("actor_id_required"\)',
                         body), intent
        assert "resolve_actor(" not in body, intent
    assert set(handlers) == set(redesign._EXPLICIT_ACTOR_INTENTS)


def test_scheme_action_is_on_by_default_since_its_live_validation():
    """2026-10-18, lane-scheme: murders created and owned for four rulers across
    four timed runs, no fault, engine refusals typed. The gate list, the server
    mirror and the operator docs all stopped naming it on the same commit."""
    assert "scheme_action" not in redesign.EXPERIMENTAL_WRITES
    assert "scheme_action" not in _gate_list_from_bridge()
    assert "scheme_action" not in _gate_list_from_doc()
