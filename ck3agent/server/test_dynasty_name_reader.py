"""One reader per name: dynasty (L14 S4, FB M-2), house, culture, faith (L14 FB M-1).

The rules (re/offsets/ck3_offsets.h): a dynasty is named by dynasty_obj::NAME
(+0x80) only (CUSTOM_NAME +0x110 would overlap HOUSES_ARRAY, LOW); a house by
dynasty_house_obj::NAME (+0x248, House_GetName); a culture by culture::NAME
(+0x260) / SCRIPT_KEY (+0x1C8); a faith through faith_lookup (FAITH_DB keyed at
faith::FAITH_ID +0x08; ad_db_lookup's +0x10 key never matches a faith) and
DISPLAY_NAME / FAITH_KEY. On every one of these INSTANCE objects +0x10 is the
masked id, never a string. The behaviour (offsets, lookups, the emitted row)
is test_fault_reply [8]/[9] (ctest, stand-in objects); these rows read the
sites from the source, so a second spelling of a rule anywhere reds."""
import re
import sys
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[2] / "bridge" / "src"
if not (SRC / "dispatch_router.cpp").is_file():
    pytest.skip("no bridge sources in this layout", allow_module_level=True)
sys.path.insert(0, str(SRC.parent / "scripts"))
import cpp_strip  # noqa: E402  (the one C++ tokenizer)


def _strip(text: str) -> str:
    return cpp_strip.strip(text, keep_strings=True)


def _sources():
    """Every bridge translation unit and header, src/os/** included."""
    root = SRC.parent
    files = sorted(SRC.rglob("*.cpp")) + sorted((root / "include").rglob("*.h"))
    assert len(files) > 50, files   # fail open: an empty census is not a pass
    return files


def _body(file: str, sig: str) -> str:
    s = _strip((SRC / file).read_text(encoding="utf-8"))
    i = s.index(sig)
    return s[i:s.index("\n}\n", i)]


def _reads_outside(pattern: str, owners: dict) -> list:
    """Sites matching `pattern` outside the owning functions ({file: [sig]})."""
    outside = []
    for f in _sources():
        s = _strip(f.read_text(encoding="utf-8", errors="replace"))
        for sig in owners.get(f.name, []):
            i = s.index(sig)
            s = s[:i] + s[s.index("\n}\n", i):]
        for m in re.finditer(pattern, s):
            outside.append(f"{f.name}:{s.count(chr(10), 0, m.start()) + 1}")
    return outside


def test_the_dynasty_reader_reads_name_only():
    b = _body("dispatch_router.cpp", "std::string dynasty_name(uintptr_t dyn_obj) {")
    assert "dynasty_obj::NAME)" in b, b
    assert "CUSTOM_NAME" not in b and "0x110" not in b and "0x130" not in b, b


def test_the_dynasty_name_offsets_are_read_in_one_function_only():
    """dynasty_obj::NAME is read inside dynasty_name only; CUSTOM_NAME and
    HAS_CUSTOM_NAME are read nowhere (enumerated over every file)."""
    assert not _reads_outside(
        r"dynasty_obj::(?:NAME|CUSTOM_NAME|HAS_CUSTOM_NAME)\b",
        {"dispatch_router.cpp": ["std::string dynasty_name(uintptr_t dyn_obj) {"]})
    assert not _reads_outside(r"dynasty_obj::(?:CUSTOM_NAME|HAS_CUSTOM_NAME)\b", {})


def test_the_house_name_is_read_in_one_function_only():
    b = _body("dispatch_router.cpp", "std::string house_name(uintptr_t house_obj) {")
    assert "dynasty_house_obj::NAME)" in b, b
    assert not _reads_outside(
        r"dynasty_house_obj::NAME\b",
        {"dispatch_router.cpp": ["std::string house_name(uintptr_t house_obj) {"]})


def test_the_faith_db_is_looked_up_through_faith_lookup_only():
    """ad_db_lookup / db_lookup on FAITH_DB (key at +0x10) answers 0 for every
    faith; FAITH_INDEXED_DB has no table. Neither is used anywhere."""
    assert not _reads_outside(r"\b(?:ad_)?db_lookup\w*\(\s*ck3::globals::FAITH_DB\b", {})
    assert not _reads_outside(r"\bFAITH_INDEXED_DB\b", {})
    b = _body("dispatch_router.cpp", "uintptr_t faith_lookup(uint32_t faith_id) {")
    assert "faith::FAITH_ID" in b and "ck3::globals::FAITH_DB" in b, b


def test_no_name_is_read_at_plus_0x10_of_a_house_culture_or_faith():
    """The five sites the witness named read through the readers, not +0x10."""
    obs = _strip((SRC / "observation.cpp").read_text(encoding="utf-8"))
    for sig in ("static std::string resolve_house_name(uintptr_t char_ptr) {",
                "static NamePair resolve_culture_names(uintptr_t char_ptr) {",
                "static NamePair resolve_faith_names(uintptr_t char_ptr) {"):
        i = obs.index(sig)
        b = obs[i:obs.index("\n}\n", i)]
        assert "NAME_KEY" not in b and "db_lookup" not in b, b
    names = _body("dispatch_router.cpp", "void emit_char_names(ResponseBuilder& r, uintptr_t c) {")
    assert "char_house_name(c)" in names and "culture_names(" in names, names
    assert "emit_faith_fields(r," in names, names
    assert "0x10" not in names and "NAME_KEY" not in names, names
    mg = _strip((SRC / "map_graph.cpp").read_text(encoding="utf-8"))
    for sig in ("static std::string county_faith_name(uintptr_t cdata) {",
                "static std::string county_culture_name(uintptr_t cdata) {"):
        i = mg.index(sig)
        b = mg[i:mg.index("\n}\n", i)]
        assert "NAME_KEY" not in b and "db_lookup" not in b, b


def test_query_succession_reads_the_title_key_off_the_type():
    b = _body("query_handlers.cpp", "bool do_query_succession(")
    i = b.index('"primary_title_key"')
    head = b[:i]
    assert "best_title_ptr + ck3::db_object::NAME_KEY" not in head, head[-600:]
    assert "best_title_ptr + ck3::title_obj::TITLE_TYPE_PTR" in head, head[-600:]


def test_the_observation_feed_reads_the_dynasty_through_the_reader():
    b = _body("observation.cpp", "static std::string resolve_dynasty_name(uintptr_t char_ptr) {")
    assert "char_dynasty_name(" in b and "NAME_KEY" not in b, b


def test_query_dynasty_says_no_stand_in_word():
    b = _body("query_handlers.cpp", "bool do_query_dynasty(")
    assert "dynasty_name(" in b
    assert '"(unknown)"' not in (SRC / "query_handlers.cpp").read_text(encoding="utf-8")


# ── one de-jure count (L14 FB m-2) ────────────────────────────────────────────
# The meter (do_query_dejure_progress) counted a county held when its holder's
# TOP liege was the ruler; the #131 create_title gate (dejure_control_fraction)
# when the holder's liege chain reached him. A ruler who is himself a vassal read
# 0 on the meter and a share at the gate. Both now count through dejure_share.

def _intent_body(sig: str) -> str:
    return _body("intent_dispatch.cpp", sig)


def test_the_meter_and_the_gate_count_through_dejure_share():
    meter = _body("query_handlers.cpp", "bool do_query_dejure_progress(")
    assert "dejure_share(" in meter, meter[:400]
    assert "held++" not in meter and "get_top_realm_ruler_id(" not in meter.split("dejure_share(")[0]
    gate = _intent_body("static double dejure_control_fraction(")
    assert "dejure_share(" in gate and "held++" not in gate, gate


def test_the_realm_predicate_is_defined_once():
    """char_in_realm_of is the one realm-membership walk; dejure_share is the
    only place a de-jure county is counted held."""
    defs = _reads_outside(r"\bbool\s+(?:char_)?in_realm_of\s*\([^)]*\)\s*\{", {})
    assert len(defs) == 1 and defs[0].startswith("intent_dispatch.cpp"), defs
    share = _intent_body("DejureShare dejure_share(")
    assert "char_in_realm_of(" in share and "out.held++" in share, share


# ── query_intents lists writes only (L14 FB m-6) ─────────────────────────────
# A queued check is a game-thread read delivered to its asker. pending_writes
# (IntentQueue, ctest test_intent_lookup row 10) is the one list; query_intents'
# pending[] and pending_count both come from it.

def test_query_intents_lists_and_counts_pending_writes_only():
    b = _body("action_handlers.cpp", "bool do_query_intents(")
    assert "g_intent_queue.pending_writes()" in b and "g_intent_queue.snapshot()" not in b, b
    assert 'r.key("pending_count"); r.value_uint(pending.size());' in b, b
    assert "pending_count()" not in b, b
