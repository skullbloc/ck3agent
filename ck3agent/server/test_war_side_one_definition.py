"""Which side of a war a character is on has ONE definition in the bridge:
bridge/include/war_side.h (witness #8 minor 5).

Until the final fix there were several readers of a war's participant arrays.
One of them (get_ai_evaluations' active_wars) indexed the array as inline
0x20-byte entries where the game stores POINTERS, and live, in both w13-obs
runs, it called the duke the ATTACKER of a war whose primary_defender he was,
while query(domain="wars") and observe's wars.active said defender.

A side computation has to read a participant's character id. So the rule is
checked where it can be seen: in bridge/src and bridge/include (comments and
strings blanked by the inline-safety audit's own stripper), the only reader of
war_participant::CHAR_ID is war_side.h, apart from war_score_hook.cpp's
pre-call validator, which walks the engine's side objects to refuse a bad
pointer before an engine call and answers no side question; and nothing indexes
a side by war_participant::ENTRY_SIZE (the inline layout). Every reply key
that names a side (`my_side`) takes its word from war_side::name.

Those rows key on the constants' spelling, so a reader that writes the offsets
as literals (`war_ptr + 0x28`, `j * 0x20 + 0x08`) would pass them (witness #9
minor 4). The anchor that does not share the spelling: the VALUES, read from
re/offsets/ck3_offsets.h (war_obj::ATK_PARTICIPANT / DEF_PARTICIPANT,
war_participant::CHAR_ID / ENTRY_SIZE), are looked for as literals -- a side
offset added to a war pointer, or the inline stride and the char-id offset --
anywhere outside war_side.h. (The live anchor is war_duke's: a war whose
primary_defender is the duke must be named his "defender" war.)
"""
from __future__ import annotations

import importlib.util
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BRIDGE = REPO / "bridge"
STATIC = BRIDGE / "scripts" / "inline_safety_static.py"

if not (BRIDGE / "src" / "query_handlers.cpp").is_file():
    pytest.skip(f"no bridge sources in this layout ({BRIDGE / 'src'})", allow_module_level=True)

_spec = importlib.util.spec_from_file_location("inline_safety_static", STATIC)
S = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(S)

#: Readers of a participant's character id that answer no side question.
VALIDATORS = {"war_score_hook.cpp"}
OFFSETS = REPO / "re" / "offsets" / "ck3_offsets.h"
_LIT = r"(?:0[xX][0-9a-fA-F]+|\d+)"


def offset_value(namespace: str, name: str) -> int:
    """`constexpr ... NAME = <int>;` inside `namespace <namespace> {` of
    ck3_offsets.h. A header this cannot read is an error, never a default."""
    text = S.strip_code(OFFSETS.read_text(errors="replace"))
    m = re.search(r"\bnamespace\s+" + namespace + r"\s*\{", text)
    assert m, f"no namespace {namespace} in {OFFSETS}"
    block = S._block_at(text, m.end() - 1) or ""
    v = re.search(r"\b" + name + r"\s*=\s*(" + _LIT + r")\s*;", block)
    assert v, f"no {namespace}::{name} in {OFFSETS}"
    return int(v.group(1), 0)


def literal_side_readers(bridge: Path) -> list[str]:
    """file:line of every literal-offset side read outside war_side.h: a war
    pointer (an identifier with "war" in it) plus the value of
    ATK_PARTICIPANT / DEF_PARTICIPANT, or an index times ENTRY_SIZE plus
    CHAR_ID, in one expression.

    Stated limit (witness #10 minor 11), not a pass: a pointer whose name does
    not contain "war" (`wp + 0x58`) and a stride split over two expressions
    (`e = a + j * 32; ... e + 8`) are NOT seen. Matching by offset value alone
    was measured and rejected: `+ 0x28` is added to six unrelated pointers in
    bridge/src today (action_handlers.cpp command buffers, ai_cb_selector_hook.cpp),
    and `+ 8` / `+ 0x20` to dozens, so a value-only row would red on code that
    reads no side. test_LIMIT_a_reader_through_wp_and_a_split_stride_escapes
    pins the limit (the witness's mutant stays green)."""
    sides = {offset_value("war_obj", "ATK_PARTICIPANT"), offset_value("war_obj", "DEF_PARTICIPANT")}
    entry = offset_value("war_participant", "ENTRY_SIZE")
    char_id = offset_value("war_participant", "CHAR_ID")
    side_add = re.compile(r"\b\w*war\w*\s*\+\s*(" + _LIT + r")\b", re.IGNORECASE)
    stride = re.compile(r"(?:\*\s*\(?\s*(" + _LIT + r")\s*\)?|(" + _LIT + r")\s*\*\s*[\w\[\]\.]+)"
                        r"\s*\)?\s*\+\s*(" + _LIT + r")\b")
    out = []
    for name, text in _sources(bridge).items():
        if name == "war_side.h":
            continue
        for m in side_add.finditer(text):
            if int(m.group(1), 0) in sides:
                out.append(f"{name}:{text[:m.start()].count(chr(10)) + 1}: {m.group(0)}")
        for m in stride.finditer(text):
            if int(m.group(1) or m.group(2), 0) == entry and int(m.group(3), 0) == char_id:
                out.append(f"{name}:{text[:m.start()].count(chr(10)) + 1}: {m.group(0)}")
    return out


def _sources(bridge: Path) -> dict[str, str]:
    files = sorted((bridge / "src").glob("*.cpp")) + sorted((bridge / "include").glob("*.h"))
    return {p.name: S.strip_code(p.read_text(errors="replace")) for p in files}


def char_id_readers(bridge: Path) -> set[str]:
    return {n for n, t in _sources(bridge).items() if re.search(r"war_participant::CHAR_ID\b", t)}


def inline_layout_users(bridge: Path) -> set[str]:
    return {n for n, t in _sources(bridge).items() if re.search(r"war_participant::ENTRY_SIZE\b", t)}


def side_words_not_from_war_side(bridge: Path) -> list[str]:
    """Every r.key("my_side") / json.key("my_side") whose value on the same
    statement is not war_side::name(...)."""
    out = []
    for p in sorted((bridge / "src").glob("*.cpp")):
        kept = S.strip_code(p.read_text(errors="replace"), keep_strings=True)
        for m in re.finditer(r'\.key\(\s*"my_side"\s*\)\s*;([^;]*);', kept):
            if "war_side::name(" not in m.group(1):
                out.append(f"{p.name}:{kept[:m.start()].count(chr(10)) + 1}")
    return out


def test_war_side_h_is_the_one_reader_of_a_participants_character_id():
    assert char_id_readers(BRIDGE) == {"war_side.h"} | VALIDATORS


def test_nothing_indexes_a_side_as_inline_entries():
    assert inline_layout_users(BRIDGE) == set()


def test_no_side_is_read_through_literal_offsets():
    assert literal_side_readers(BRIDGE) == []


#: Witness #9's mutant: a second side reader, offsets written as literals.
LITERAL_READER = """
static bool ff3_literal_side(uintptr_t war_ptr, uint32_t cid) {
    const uintptr_t arr = safe_read<uintptr_t>(war_ptr + 0x28);
    for (int j = 0; j < 4; j++)
        if ((safe_read<uint32_t>(arr + j * 0x20 + 0x08) & 0xFFFFFF) == cid) return true;
    return false;
}
"""


def test_MUTANT_a_literal_offset_side_reader_reds(tmp_path):
    import shutil
    dst = tmp_path / "bridge"
    shutil.copytree(BRIDGE / "src", dst / "src")
    shutil.copytree(BRIDGE / "include", dst / "include")
    qh = dst / "src" / "query_handlers.cpp"
    qh.write_text(qh.read_text() + "\nnamespace bridge {" + LITERAL_READER + "}\n")
    hits = literal_side_readers(dst)
    assert len(hits) == 2 and all(h.startswith("query_handlers.cpp:") for h in hits), hits
    # The spelling-keyed rows cannot see it (labelled: not discriminating).
    assert char_id_readers(dst) == {"war_side.h"} | VALIDATORS
    assert inline_layout_users(dst) == set()


def test_the_offsets_are_read_from_the_header():
    """Not a discriminating control: the values the literal anchor looks for
    are the header's (a parse that found other numbers would look for them)."""
    assert offset_value("war_obj", "ATK_PARTICIPANT") == 0x28
    assert offset_value("war_obj", "DEF_PARTICIPANT") == 0x58
    assert offset_value("war_participant", "CHAR_ID") == 0x08
    assert offset_value("war_participant", "ENTRY_SIZE") == 0x20


def test_every_my_side_word_comes_from_war_side_name():
    assert side_words_not_from_war_side(BRIDGE) == []


def test_the_side_emitters_ask_war_side():
    """Not a discriminating control: the replies that name a side are found
    (a parse that matched nothing would pass the row above vacuously)."""
    n = 0
    for p in (BRIDGE / "src").glob("*.cpp"):
        n += len(re.findall(r'\.key\(\s*"my_side"\s*\)', S.strip_code(p.read_text(), keep_strings=True)))
    assert n >= 2, n
    qh = S.strip_code((BRIDGE / "src" / "query_handlers.cpp").read_text())
    body = qh[qh.index("bool do_get_ai_evaluations("):]
    body = body[:body.index("\n}\n")]
    assert "emit_war_entry(" in body, "get_ai_evaluations' war rows are emit_war_entry rows"


#: Witness #10's mutant: the same reader through a pointer named `wp` and a
#: stride split over two expressions.
LITERAL_READER_WP = """
static bool w15_lit2(uintptr_t wp, uint32_t c) {
  const uintptr_t a = safe_read<uintptr_t>(wp + 0x58);
  for (int j = 0; j < 4; j++) { const uintptr_t e = a + j * 32; if ((safe_read<uint32_t>(e + 8) & 0xFFFFFF) == c) return true; }
  return false;
}
"""


def test_LIMIT_a_reader_through_wp_and_a_split_stride_escapes(tmp_path):
    """Not a pass, a stated limit (literal_side_readers' docstring): this
    reader is not seen. If a later parser sees it, this test reds and the
    docstring's limit is to be deleted with it."""
    import shutil
    dst = tmp_path / "bridge"
    shutil.copytree(BRIDGE / "src", dst / "src")
    shutil.copytree(BRIDGE / "include", dst / "include")
    qh = dst / "src" / "query_handlers.cpp"
    qh.write_text(qh.read_text() + "\nnamespace bridge {" + LITERAL_READER_WP + "}\n")
    assert literal_side_readers(dst) == []
