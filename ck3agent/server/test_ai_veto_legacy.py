"""The ai_veto kind `legacy` (re/findings/re_ai_veto.md section 15).

A seated run as Charles of West Francia (867): the game's AI bought dynasty legacy perks for him twice (867.04,
885.10), renown 1,578 -> 240, and no kind stopped it (the `perk` kind is the lifestyle perk). The AI's legacy pass
FUN_04dbc7e0 (image) builds CAddDynastyPerkCommand for the dynasty head and posts it with GameCommandDispatch flags 7.
The bridge's hook rows are ctest test_ai_veto 23a-i; here: the vtable and the two posting sites against the game
binary's bytes, the kind in the toolkit's list and the spending preset, the price row, the notes.
"""
import json
import os
import re
from pathlib import Path

import pytest

import notes as N
import seat_events as SE

ROOT = Path(__file__).resolve().parents[2]
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
BINARY = GAME.parent / "binaries" / "ck3"
CHARLES = 12015

#: header offsets = file offsets in the text segment (1.18.3.1, md5 09ba163b...). The (cmd, char id, perk) ctor of
#: CAddDynastyPerkCommand, the AI's legacy pass's call of it, the player's legacy window's call of it, and
#: GameCommandDispatch.
RW_SHIFT = 0x1000
CTOR = 0x66ca890
AI_CALL = 0x4cbcb49          # in FUN_04dbc7e0 (image)
WINDOW_CALL = 0x4551656      # in FUN_046515f0 (image)
DISPATCH = 0x7560390


def _first_lea(data: bytes, va: int) -> int:
    code = data[va:va + 0x40]
    at = code.index(b"\x48\x8d\x05")
    return va + at + 7 + int.from_bytes(code[at + 3:at + 7], "little", signed=True)


def _call_target(data: bytes, at: int) -> int:
    assert data[at] == 0xE8, hex(at)
    return at + 5 + int.from_bytes(data[at + 1:at + 5], "little", signed=True)


def _dispatch_flags(data: bytes, after: int) -> int:
    """The flags of the first GameCommandDispatch call after `after`: the `mov edx, imm32` just before it."""
    for j in range(after, after + 0x100):
        if data[j] == 0xE8 and _call_target(data, j) == DISPATCH:
            for k in range(j - 1, j - 0x20, -1):
                if data[k] == 0xBA:
                    return int.from_bytes(data[k + 1:k + 5], "little")
            break
    raise AssertionError("no GameCommandDispatch call within 0x100 bytes of " + hex(after))


def _binary() -> bytes:
    if not BINARY.is_file():
        pytest.skip("the game binary is not installed here")
    return BINARY.read_bytes()


def test_1_the_legacy_vtable_is_what_the_constructor_stores_at_cmd_0():
    h = (ROOT / "re" / "offsets" / "ck3_offsets.h").read_text()
    data = _binary()
    m = re.search(r"constexpr uintptr_t ADD_DYNASTY_PERK_CMD_VTABLE\s*=\s*(0x[0-9a-fA-F]+);", h)
    assert m and int(m.group(1), 16) == _first_lea(data, CTOR)
    vt = int(m.group(1), 16) - RW_SHIFT
    assert int.from_bytes(data[vt - 16:vt - 8], "little") == 0          # offset_to_top 0: the primary vtable
    ti = int.from_bytes(data[vt - 8:vt], "little") - RW_SHIFT
    nm = int.from_bytes(data[ti + 8:ti + 16], "little")
    assert data[nm:nm + 25] == b"22CAddDynastyPerkCommand\0"
    for name, want in (("ADD_DYNASTY_PERK_CMD_PERK", 0x28),):
        m2 = re.search(rf"constexpr size_t\s+{name}\s*=\s*(0x[0-9a-fA-F]+);", h)
        assert m2 and int(m2.group(1), 16) == want, name


def test_1_CONTROL_the_decoder_reads_the_lifestyle_perk_ctor_as_its_own_vtable():
    """NON-DISCRIMINATING CONTROL (it passes the superseded tree too): the same decoder applied to
    CAddLifestylePerkCommand's ctor (add_lifestyle_perk_cmd::CTOR, image 0x067c9f50) reads the
    header's add_lifestyle_perk_cmd::VTABLE: the decoder is sound, and the two perk kinds are two classes."""
    h = (ROOT / "re" / "offsets" / "ck3_offsets.h").read_text()
    data = _binary()
    blk = h[h.index("namespace add_lifestyle_perk_cmd"):]
    lv = int(re.search(r"VTABLE\s*=\s*(0x[0-9a-fA-F]+);", blk).group(1), 16)
    ctor = int(re.search(r"CTOR\s*=\s*(0x[0-9a-fA-F]+);", blk).group(1), 16)
    assert _first_lea(data, ctor) == lv
    assert lv != _first_lea(data, CTOR)


def test_1_the_ai_posts_it_with_flags_7_and_the_player_window_with_0xe():
    data = _binary()
    assert _call_target(data, AI_CALL) == CTOR and _dispatch_flags(data, AI_CALL) == 7
    # the CONTROL: the same reader finds the player's window's flags, 0xe (never dropped: decide() wants 7)
    assert _call_target(data, WINDOW_CALL) == CTOR and _dispatch_flags(data, WINDOW_CALL) == 0xE


def test_2_legacy_is_a_kind_and_in_the_spending_preset():
    assert "legacy" in SE.AI_VETO_WHATS and "legacy" in SE.AI_VETO_PRESETS["spending"]
    assert "legacy" not in SE.AI_VETO_LEFT_OUT and "perk" in SE.AI_VETO_LEFT_OUT
    h = (ROOT / "bridge" / "include" / "ai_veto.h").read_text()
    assert re.search(r'case 12: return "legacy";', h)


def _rec(seq, dropped, **kw):
    return dict({"seq": seq, "date": "0885.10.02", "character_id": CHARLES, "what": "legacy", "dropped": dropped,
                 "origin": "game", "key": "kin_legacy_2", "priced": "yes",
                 "price": {"gold": 0.0, "prestige": 0.0, "piety": 0.0, "renown": 1250.0, "currency": "renown"},
                 "gold": 300.0, "prestige": 900.0}, **kw)


def test_3_a_dropped_legacy_is_priced_in_renown_and_says_no_call_buys_one():
    row = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, True)]})["ai_vetoed"][0]
    assert row["what"] == "legacy" and row["key"] == "kin_legacy_2" and row["price"] == {"renown": 1250.0}
    assert "next" not in row and row["note"] == SE.LEGACY_VETO_NOTE and "op=clear" in SE.LEGACY_VETO_NOTE


def test_3_a_passed_legacy_names_the_call_that_vetoes_it():
    snap = {"ai_veto_records": [_rec(2, False)], "ai_vetoes": ["law"]}
    acted = SE.veto_news({"ai_veto_records": []}, snap)["ai_acted"][0]
    assert acted["price"] == {"renown": 1250.0}
    assert acted["veto"]["args"]["args"] == {"op": "set", "what": "legacy"}


def test_3_CONTROL_the_other_kinds_prices_carry_no_renown_key():
    rec = _rec(3, True, what="law", key="crown_authority_1",
               price={"gold": 0.0, "prestige": 792.0, "piety": 0.0, "currency": "gold"})
    assert SE.price_of(rec) == {"prestige": 792.0}


def test_4_the_turn_one_notes_name_the_preset_and_renown():
    doc = N.load()
    for s in (None, "francia867"):
        texts = N.turn_one_texts(N.derive(doc, s))
        spend = [t for t in texts if "what=spending" in t]
        assert spend and all("renown" in t for t in spend), s
    assert N.problems(doc) == []
    for n in doc["notes"]:
        assert len(n["text"].encode()) <= N.TEXT_MAX
    assert "(sets seven)" not in json.dumps(doc) and "(sets eight)" not in json.dumps(doc)   # the count: test_notes_general
