"""bridge/include/engine_text.h, compiled and run against the game's own text.

The faction verdicts (query_joinable_factions, faction_action) carry the reason
the GAME gives. That text is rendered tooltip text with the engine's markup in
it; the samples below are verbatim replies from run l10-scout1 on the node's 867
save (live867-runs/l10-scout1/results/l10_scout.json). The header is the one
definition of the cleaning; this compiles it and checks what a player reads.
"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]

# L13 FF3 (witness X4): the one C++ tokenizer (bridge/scripts/cpp_strip.py).
import sys  # noqa: E402
sys.path.insert(0, str(REPO / "bridge" / "scripts"))
import cpp_strip  # noqa: E402
INC = REPO / "bridge" / "include"

#: (raw, what the seat reads). The first two are live; the rest pin the markup rules.
CASES = [
    ("\x15indent_newline:0 \x15!\x15indent_newline:6 \x16warning_icon! \x15X \x15ONCLICK:CHARACTER,12139 "
     "\x15TOOLTIP:CHARACTER,12139 \x15L \x15high Emir\x15! \x15high Abu Abdallah\x15!\x15!\x15!\x15! already "
     "has the lowest possible \x15E; \x15TOOLTIP:GAME_CONCEPT,crown_authority Crown Authority\x15!\x15!\x15!\x15!",
     "Emir Abu Abdallah already has the lowest possible Crown Authority"),
    ("\x15indent_newline:0 \x15!\x15indent_newline:6 \x16warning_icon! \x15X \x15ONCLICK:TITLE,8724 "
     "\x15TOOLTIP:LANDED_TITLE,8724 \x15L; The al-Umari Emirate\x15!\x15!\x15! has a \x15E; "
     "\x15TOOLTIP:GAME_CONCEPT,ceremonial_liege Ceremonial Monarch\x15!\x15!\x15!\x15!",
     "The al-Umari Emirate has a Ceremonial Monarch"),
    # two failure lines become two clauses
    ("\x15indent_newline:0 \x15!\x16warning_icon! \x15X first rule\x15!\x15indent_newline:0 \x15!"
     "\x16warning_icon! \x15X second rule\x15!",
     "first rule; second rule"),
    # a code with no text before the next markup byte keeps what follows
    ("\x15X\x15high strong\x15!\x15! text", "strong text"),
    # CAN_FIRE_ULTIMATUM (FUN_05f8ca90) appends each failing reason after a 1-byte
    # "\n" (the string at image 0x08f92ccd); two reasons were read joined with no
    # separator in run l10fx6-live-1 ("... is in war You have to wait until ...").
    ("\nCannot press demands while the faction is in war\nYou have to wait until 26 November, 869 "
     "before you can press the demands of this faction again",
     "Cannot press demands while the faction is in war; You have to wait until 26 November, 869 "
     "before you can press the demands of this faction again"),
    ("plain words", "plain words"),
    ("", ""),
    # L12: a compound header with nothing under it says nothing. The imprison
    # check's engine_reason read "All of these:" alone (w-l10-factions-6, every
    # refused target); imperial_policy_faction's create read "...; All of these:".
    ("\x15indent_newline:0 \x15!\x15indent_newline:6 \x16warning_icon! \x15X All of these:\x15!", ""),
    ("\x15indent_newline:0 \x15!\x15indent_newline:6 \x16warning_icon! \x15X Rank is not greater or "
     "equal to Empire Rank\x15!\x15indent_newline:6 All of these:\x15!",
     "Rank is not greater or equal to Empire Rank"),
    ("Rank too low\nAll of these:", "Rank too low"),
    # a header over a header over nothing goes with it
    ("All of these:\x15indent_newline:2 Any of these:\x15indent_newline:0 next rule", "next rule"),
    # THE CONTROL: a header with a deeper line under it is kept with that line
    ("\x15indent_newline:0 All of these:\x15indent_newline:2 \x16warning_icon! Is an adult\x15!",
     "All of these:; Is an adult"),
    # L12: a repeated line is said once (restore_ceremonial_liege_faction's create,
    # w-l10-factions-6: FACTION_CAN_CREATE_AS_COMMAND runs two checks of the same
    # condition into one string)
    ("\x15indent_newline:0 \x15!\x15indent_newline:6 The Aghlabid Sultanate has a \x15E; "
     "\x15TOOLTIP:GAME_CONCEPT,ceremonial_liege Ceremonial Monarch\x15!\x15indent_newline:0 \x15!"
     "\x15indent_newline:6 The Aghlabid Sultanate has a \x15E; "
     "\x15TOOLTIP:GAME_CONCEPT,ceremonial_liege Ceremonial Monarch\x15!",
     "The Aghlabid Sultanate has a Ceremonial Monarch"),
    # the control: two different lines both stay
    ("first\nsecond\nfirst", "first; second"),
    # L12 witness: a war's name is rendered text too. observe's wars.active[].name
    # read these raw (w12-full r1/r2, get_active_wars on the 867 save at 868-870).
    ("\x15E; \x15TOOLTIP:GAME_CONCEPT,holy_war Holy War\x15!\x15! for the \x15ONCLICK:TITLE,1158 "
     "\x15TOOLTIP:LANDED_TITLE,1158 \x15L; Chiefdom of Dessau\x15!\x15!\x15!",
     "Holy War for the Chiefdom of Dessau"),
    ("\x15ONCLICK:TITLE,998 \x15TOOLTIP:LANDED_TITLE,998 \x15L; East Franconian\x15!\x15!\x15!-"
     "\x15ONCLICK:TITLE,2189 \x15TOOLTIP:LANDED_TITLE,2189 \x15L; Italian\x15!\x15!\x15! Tributary War",
     "East Franconian-Italian Tributary War"),
]


def _driver(cases) -> str:
    lines = ['#include "engine_text.h"', "#include <cstdio>", "int main() {"]
    for raw, _ in cases:
        # split after every hex escape so a following hex digit is not swallowed
        lit = "".join(f'\\x{ord(ch):02x}""' if ord(ch) < 0x20 or ch in '"\\' else ch for ch in raw)
        lines.append(f'  std::printf("%s\\n", bridge::clean_engine_text(std::string("{lit}")).c_str());')
    lines.append("  return 0; }")
    return "\n".join(lines)


def test_the_game_text_reads_as_words():
    cxx = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler: engine_text.h cannot be compiled here")
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "drv.cpp"
        exe = Path(d) / "drv"
        src.write_text(_driver(CASES), encoding="utf-8")
        subprocess.run([cxx, "-std=c++17", "-I", str(INC), "-o", str(exe), str(src)],
                       check=True, capture_output=True, text=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    got = out.split("\n")[:len(CASES)]
    assert got == [want for _, want in CASES]


def test_the_server_cleaner_reads_the_same_lines():
    """redesign.engine_text cleans the texts the bridge passes raw (change_law,
    construction, intent verdicts). It applies the bridge's line rules (L12: a
    header over nothing goes, a repeat is said once) and reads every case above
    as the bridge does. It adds two rules those texts carry (a concept icon's
    name, a trigger line's met/not met mark), pinned in test_succession_levers.py
    and test_lane_query.py."""
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import redesign
    assert [redesign.engine_text(raw) for raw, _ in CASES] == [want for _, want in CASES]


def test_every_war_name_the_bridge_reads_goes_through_the_cleaner():
    """A war's name (war_obj::NAME_DATA_PTR) is the game's rendered text; every
    read of it in bridge/src is passed through clean_engine_text, the one cleaner.
    Enumerated from the source over whole statements (a read split over two lines
    counts), failing OPEN: too few reads found is a red."""
    import re
    src = REPO / "bridge" / "src"
    reads = wrapped = 0
    for p in sorted(src.glob("*.cpp")):
        code = cpp_strip.strip(p.read_text(), keep_strings=True)
        for m in re.finditer(r"[^;{}]*war_obj::NAME_DATA_PTR[^;]*;", code):
            reads += 1
            stmt = " ".join(m.group(0).split())
            assert re.search(r"clean_engine_text\(\s*(?:ad_)?read_std_string\(", stmt), \
                f"{p.name}: a war name read without the cleaner: {stmt}"
            wrapped += 1
    # 8 since the final fix: get_ai_evaluations' war rows are emit_war_entry
    # rows, so its own name read is gone.
    assert reads >= 8, f"found {reads} war-name reads (fail OPEN)"
