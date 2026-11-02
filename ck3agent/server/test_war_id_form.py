"""Every war id a player can see is the same full handle (cleanup 2026.11, L12 item 6).

A war's id is its WAR_ID: generation byte << 24 | slot index (ck3_offsets.h
war_db::WAR_ID_*). The slot is reused after a war ends, so the index alone
names a different war later, and two reads that emit the two forms never join.
Before L12, query_war_stance (inspect war_stance, observe's stance join),
query_call_to_arms (the calls a player answers) and the WAR_CREATE_CORE reply
(declare_war) emitted the slot index; query_wars, get_active_wars, faction rows
(faction_war.h), war history, war declarations and observe emitted the handle.

  bridge census  every `key("war_id")` (and the one printf'd "war_id") in
                 bridge/src emits a value that is the WAR_ID read unmasked, a
                 record field filled from it, or the caller's own argument
                 echoed back; enumerated from the source, failing OPEN
  python join    _war_stance_view joins the native stance row to the war row on
                 the full handle (the join the slot index broke)
  python census  no server or hook module masks a war id to its slot
"""
import asyncio
import re
from pathlib import Path

import redesign as R

REPO = Path(__file__).resolve().parents[2]

# L13 FF3 (witness X4): the one C++ tokenizer (bridge/scripts/cpp_strip.py).
import sys  # noqa: E402
sys.path.insert(0, str(REPO / "bridge" / "scripts"))
import cpp_strip  # noqa: E402
SRC = REPO / "bridge" / "src"

HANDLE = (0x0B << 24) | 2         # generation 11, slot 2 (the docstring's 0x0B000002)

#: emitter -> why its value is the full handle. A new emitter fails the census
#: until it is read and classified here (fail OPEN).
EMITTERS = {
    ("query_handlers.cpp", "emit_war_entry", "war_id"): "WAR_ID read (query_wars rows)",
    ("query_handlers.cpp", "do_get_active_wars", "war_id"): "WAR_ID read",
    ("query_handlers.cpp", "do_query_war_db_scan", "stored_ref"): "WAR_ID read",
    ("query_handlers.cpp", "do_query_war_score_breakdown", "wid"): "the breakdown's own war id",
    ("query_handlers.cpp", "do_query_war_score_breakdown", "war_id"): "the caller's war_id echoed",
    ("query_handlers.cpp", "do_query_war_stance", "stored"): "WAR_ID read",
    ("query_handlers.cpp", "do_query_call_to_arms", "stored"): "WAR_ID read",
    ("war_declaration_hook.cpp", "do_query_war_declarations", "it->war_id"): "record filled from WAR_ID",
    ("war_history_hook.cpp", "do_query_war_history", "rec.war_id"): "record filled from WAR_ID",
    ("factions.cpp", "emit_faction_war", "w.war_id"): "faction_war.h: the war's full handle",
    ("action_handlers.cpp", "do_send_interaction", "intent.target_war_id"): "the caller's war echoed",
    ("observation.cpp", "build_wars", "safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID)"): "WAR_ID read",
    ("intent_dispatch.cpp", "execute_war_create_core_intent", "war_id"): "WAR_ID read (printf'd)",
}

_FN = re.compile(r"^(?:static\s+)?(?:inline\s+)?[\w:<>]+[\s\*&]+(\w+)\s*\((?!.*;\s*$)")
#: one emitter, on one line or split across several (\s spans the newline)
_EMIT = re.compile(r'key\("war_id"\);\s*\w+\.value_u?int\(([^;]+)\);')
#: the 24-bit slot mask in any spelling; the 32-bit 0xFFFFFFFF sentinel is not it
_MASK = re.compile(r"&\s*\(?\s*(?:0x0*ffffff(?![0-9a-f])|16777215\b|\w*IDX_MASK\b)", re.I)


def _code_lines(src: str) -> list:
    """The source's lines with comments blanked (cpp_strip), so a comment
    cannot emit and a `//` inside a string is not one."""
    return cpp_strip.strip(src, keep_strings=True).split("\n")


def _functions(lines: list) -> list:
    """For each line, the name of the function it sits in and that function's first line."""
    fn, start, out = "?", 0, []
    for i, line in enumerate(lines):
        m = _FN.match(line)
        if m and not line.startswith((" ", "\t")):
            fn, start = m.group(1), i
        out.append((fn, start))
    return out


def _emitters(texts: dict) -> dict:
    out = {}
    for name, src in texts.items():
        lines = _code_lines(src)
        where = _functions(lines)
        code = "\n".join(lines)
        for e in _EMIT.finditer(code):
            expr = " ".join(e.group(1).split())
            if expr.endswith(")") and expr.count("(") < expr.count(")"):
                expr = expr[:-1]
            line = code.count("\n", 0, e.start()) + 1
            out[(name, where[line - 1][0], expr)] = line
        for i, line in enumerate(src.split("\n")):
            if '"war_id\\":%u' in line:
                out[(name, where[i][0], "war_id")] = i + 1
    return out


def _assignments(src: str, line: int, var: str) -> list:
    """Every right-hand side assigned to `var` in the emitting function, up to the
    emit: the value an emitter prints is wherever that variable was filled."""
    lines = _code_lines(src)
    start = _functions(lines)[line - 1][1]
    body = "\n".join(lines[start:line])
    pat = re.compile(r"(?<![\w.>])" + re.escape(var) + r"\s*=(?!=)\s*([^;]+);")
    return [" ".join(m.group(1).split()) for m in pat.finditer(body)]


def _sources() -> dict:
    return {p.name: p.read_text() for p in sorted(SRC.glob("*.cpp"))}


def test_the_census_finds_the_emitters():
    found = _emitters(_sources())
    assert len(found) >= 12, f"the census found too few war_id emitters (fail OPEN): {found}"
    unclassified = sorted(set(found) - set(EMITTERS))
    assert not unclassified, f"read and classify these war_id emitters: {unclassified}"
    gone = sorted(set(EMITTERS) - set(found))
    assert not gone, f"classified emitters that no longer exist: {gone}"


def test_no_emitted_war_id_is_a_slot_index_or_masked():
    texts = _sources()
    for (name, fn, expr), line in _emitters(texts).items():
        assert expr not in ("idx", "war_idx", "slot", "slot_index"), (name, fn, expr, line)
        assert not _MASK.search(expr), (name, fn, expr, line)
        if re.fullmatch(r"\w+", expr):
            for rhs in _assignments(texts[name], line, expr):
                assert not _MASK.search(rhs), (name, fn, expr, line, rhs)
        # the printf'd reply (WAR_CREATE_CORE): its argument is not masked
        if expr == "war_id" and name == "intent_dispatch.cpp":
            seg = texts[name].split("\n")[line - 1:line + 4]
            assert not any(_MASK.search(s) for s in seg), seg


def test_control_the_mask_check_reads_every_spelling():
    for masked in ("stored & 0xFFFFFF", "stored & 0xffffff", "stored&0x00FFFFFF",
                   "w & 16777215", "id & WAR_IDX_MASK", "(war_id & 0xFFFFFFu)"):
        assert _MASK.search(masked), masked
    for whole in ("stored", "war_created ? war_id : 0xFFFFFFFFu", "w.war_id"):
        assert not _MASK.search(whole), whole


def test_control_a_mask_at_the_assignment_is_seen():
    src = ('void emit_x(ResponseBuilder& r, uintptr_t war_ptr) {\n'
           '    uint32_t war_id = safe_read<uint32_t>(war_ptr + 4) & 0xFFFFFF;\n'
           '    r.key("war_id"); r.value_uint(war_id);\n}\n')
    found = _emitters({"x.cpp": src})
    assert found == {("x.cpp", "emit_x", "war_id"): 3}
    assert [_MASK.search(rhs) is not None for rhs in _assignments(src, 3, "war_id")] == [True]


def test_control_the_census_reads_an_emitter_split_over_two_lines():
    src = ('bool do_query_x(const char* p) {\n'
           '    r.key("war_id");\n'
           '      r.value_uint(idx & 0xFFFFFF);\n}\n')
    found = _emitters({"x.cpp": src})
    assert found == {("x.cpp", "do_query_x", "idx & 0xFFFFFF"): 2}


def test_control_the_census_reds_on_a_slot_index_emitter():
    src = {"x.cpp": 'bool do_query_x(const char* p) {\n'
                    '    r.key("war_id"); r.value_uint(idx);\n}\n'}
    found = _emitters(src)
    assert found == {("x.cpp", "do_query_x", "idx"): 2}
    assert set(found) - set(EMITTERS), "an unclassified emitter must red the census"


def test_war_stance_joins_on_the_full_handle():
    """observe/plan_war join the engine stance row to the war row by war_id."""
    class GW:
        def __init__(self, stance_id):
            self.stance_id = stance_id

        async def read(self, verb, params):
            if verb == "get_active_wars":
                return {"wars": [{"war_id": HANDLE, "name": "W", "is_attacker": True,
                                  "war_score": 10}]}
            if verb == "query_war_stance":
                return {"wars": [{"war_id": self.stance_id, "stance": "offensive",
                                  "will_siege": True}]}
            return {}

        def game(self):
            return {"tick": 1}

    def view(stance_id):
        R._WAR_STANCE_UNSERVED.clear()
        return asyncio.run(R._war_stance_view(GW(stance_id), 1))["wars"][0]

    row = view(HANDLE)
    assert row["war_id"] == HANDLE
    assert row["stance_source"] == "engine_stance_db" and row["stance"] == "offensive"
    # the control: the slot index the bridge used to emit does not join
    assert view(HANDLE & 0xFFFFFF)["stance_source"] == "inferred_from_occupation"


def test_no_python_module_masks_a_war_id():
    mask = re.compile(r"war\w*[^\n=]{0,40}&\s*(0x[fF]{6}\b|16777215)")
    for p in sorted((REPO / "ck3agent" / "server").glob("*.py")) + \
            sorted((REPO / "ck3agent" / "hooks").glob("*.py")):
        if p.name.startswith("test_"):
            continue
        for i, line in enumerate(p.read_text().split("\n"), 1):
            assert not mask.search(line.split("#")[0]), f"{p.name}:{i}: {line.strip()}"
