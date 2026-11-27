"""The ai_veto kind `faction` (re/findings/re_ai_veto.md section 14).

The game's AI makes an AI-run ruler create a faction (CCreateFactionCommand, FUN_04ddea80) or join one
(CJoinFactionCommand, FUN_04dde690), both posted with GameCommandDispatch flags 7; a faction he leads declares
its war as his. The bridge's hook rows are ctest test_ai_veto 22a-j; here: the vtables against the constructors'
bytes, the kind in the toolkit's list (not in the spending preset), the news rows, the Lorraine notes and prompt.
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
GOTTFRIED, HENRY = 11030, 6878

#: the constructors (header offsets = file offsets in the text segment, 1.18.3.1, md5 09ba163b...): the
#: (cmd, faction id, character) ctor of CJoinFactionCommand and the (cmd, type index, actor, target) ctor of
#: CCreateFactionCommand. The first `lea rax, [rip+X]` stores the primary vtable at cmd+0x00.
RW_SHIFT = 0x1000
FACTION_CTORS = {"JOIN_FACTION_CMD_VTABLE": 0x5e99440, "CREATE_FACTION_CMD_VTABLE": 0x5e99b80}


def _first_lea(data: bytes, va: int) -> int:
    code = data[va:va + 0x40]
    at = code.index(b"\x48\x8d\x05")
    return va + at + 7 + int.from_bytes(code[at + 3:at + 7], "little", signed=True)


def test_1_the_faction_vtables_are_what_the_constructors_store_at_cmd_0():
    h = (ROOT / "re" / "offsets" / "ck3_offsets.h").read_text()
    if not BINARY.is_file():
        pytest.skip("the game binary is not installed here")
    data = BINARY.read_bytes()
    for name, va in FACTION_CTORS.items():
        m = re.search(rf"constexpr uintptr_t {name}\s*=\s*(0x[0-9a-fA-F]+);", h)
        assert m and int(m.group(1), 16) == _first_lea(data, va), name
        # offset_to_top 0 and a typeinfo whose name is the class (the RW segment's file offset is its address
        # less 0x1000: LOAD 0xb64c130 at 0xb64d130)
        vt = int(m.group(1), 16) - RW_SHIFT
        assert int.from_bytes(data[vt - 16:vt - 8], "little") == 0
        ti = int.from_bytes(data[vt - 8:vt], "little") - RW_SHIFT   # pointers in the file are header offsets
        nm = int.from_bytes(data[ti + 8:ti + 16], "little")
        want = b"19CJoinFactionCommand" if name.startswith("JOIN") else b"21CCreateFactionCommand"
        assert data[nm:nm + len(want) + 1] == want + b"\0", name


def test_1_CONTROL_the_decoder_reads_the_leave_ctor_as_the_leave_vtable():
    """CLeaveFactionCommand's ctor (FUN_05f996e0, image) stores the vtable re_factions.md section 2 read (image
    0x0bc17b50): the same decoder, a class the veto does not take (the AI's leave is never dropped)."""
    if not BINARY.is_file():
        pytest.skip("the game binary is not installed here")
    assert _first_lea(BINARY.read_bytes(), 0x5e996e0) == 0x0bc17b50 - 0x100000


def test_2_faction_is_a_kind_and_not_in_the_spending_preset():
    assert "faction" in SE.AI_VETO_WHATS and "faction" not in SE.AI_VETO_PRESETS["spending"]
    assert "faction" in SE.AI_VETO_LEFT_OUT and "never dropped" in SE.AI_VETO_LEFT_OUT["faction"]
    h = (ROOT / "bridge" / "include" / "ai_veto.h").read_text()
    assert re.search(r'case 11: return "faction";', h)


def _rec(seq, dropped, **kw):
    return dict({"seq": seq, "date": "1067.03.02", "character_id": GOTTFRIED, "what": "faction", "dropped": dropped,
                 "origin": "game", "priced": "no", "unpriced_why": "faction_no_price", "gold": 120.0,
                 "prestige": 300.0}, **kw)


def test_3_news_names_the_faction_post_and_the_call_that_vetoes_it():
    snap = {"ai_veto_records": [_rec(1, False, faction_post="join", faction_id=88, key="liberty_faction")],
            "ai_vetoes": ["war"]}
    acted = SE.veto_news({"ai_veto_records": []}, snap)["ai_acted"][0]
    assert acted["faction_post"] == "join" and acted["faction_id"] == 88 and acted["key"] == "liberty_faction"
    assert acted["veto"]["args"]["args"] == {"op": "set", "what": "faction"}
    assert acted["price_unread"] == SE.ACTED_PRICE_NOTE["faction"]


def test_3_a_dropped_create_names_its_target():
    snap = {"ai_veto_records": [_rec(2, True, faction_post="create", faction_type_index=0, target_id=HENRY,
                                     key="independence_faction")]}
    row = SE.veto_news({"ai_veto_records": []}, snap)["ai_vetoed"][0]
    assert row["what"] == "faction" and row["key"] == "independence_faction" and row["target_id"] == HENRY


def test_4_the_lorraine_set_says_to_set_it_on_turn_one_and_the_default_set_does_not():
    doc = N.load()
    lor = N.derive(doc, "lorraine1066")
    assert any("what=faction" in t for t in N.turn_one_texts(lor))
    assert not any("what=faction" in json.dumps(n) for n in N.derive(doc, None)["notes"])
    goal = [n for n in lor["notes"] if n["id"] == "lorraine-line-goal"][0]
    assert "built, not proven live" in goal["provenance"] and len(goal["text"].encode()) <= N.TEXT_MAX
