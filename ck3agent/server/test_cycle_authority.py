"""One derivation of the episode cycle (lane v4instr item 4a).

`channel.send_cycle` (a message's cycle) and `notepad.cycle_at` (a pad
record's, and the guide log's) used to derive the cycle each by its own copy of
`cell_schedule` + `ordinal` + `cycle_index`. Both now call
`cycle_cadence.episode_cycle`; this file checks the mirror both ways: they agree
with it on every case, and neither module derives the cycle another way.
"""
from __future__ import annotations

import ast
from pathlib import Path

import pytest

from ck3agent.server import channel as C
from ck3agent.server import cycle_cadence as CC
from ck3agent.server import notepad as N

HERE = Path(__file__).resolve().parent
CELL = {"CK3_EPISODE_HORIZON_START": "1066.9.15", "CK3_CYCLE_GAME_DAYS": "90",
        "CK3_EPISODE_HORIZON_END": "1091.9.15"}
CASES = [
    (CELL, "1066.9.15"), (CELL, "1066.12.13"), (CELL, "1066.12.14"), (CELL, "1070.1.1"),
    (CELL, "not a date"), (CELL, None),
    ({**CELL, "CK3_CYCLE_GAME_DAYS": ""}, "1067.1.1"),            # a cell with no cadence
    ({**CELL, "CK3_EPISODE_HORIZON_END": "1060.1.1"}, "1067.1.1"),  # end before start
    ({}, "1067.1.1"),                                             # free play
]


@pytest.mark.parametrize("env,date", CASES)
def test_both_stamps_are_the_authoritys_cycle(env, date):
    want, why = CC.episode_cycle(env, date)
    assert N.cycle_at(date, env) == want
    counter = CC.CycleCounter(cycle=7)
    got = C.send_cycle(env, date, counter)
    if why == CC.CYCLE_NOT_A_CELL:
        assert got == (7, None)                 # free play: the channel's published rule
    else:
        assert got == (want, why)


def test_the_authoritys_known_values():
    assert CC.episode_cycle(CELL, "1066.9.15") == (0, None)
    assert CC.episode_cycle(CELL, "1066.12.14")[0] == 1          # day 90
    assert CC.episode_cycle({}, "1066.9.15") == (None, CC.CYCLE_NOT_A_CELL)
    assert CC.episode_cycle(CELL, "x")[0] is None


@pytest.mark.parametrize("module", ["channel.py", "notepad.py"])
def test_no_reader_derives_the_cycle_itself(module):
    """The mirror, checked from the code: neither module calls the schedule's
    pieces (`cell_schedule`, `ordinal`, `cycle_index`) directly."""
    tree = ast.parse((HERE / module).read_text(encoding="utf-8"))
    called = {n.func.attr if isinstance(n.func, ast.Attribute) else getattr(n.func, "id", None)
              for n in ast.walk(tree) if isinstance(n, ast.Call)}
    assert not called & {"cell_schedule", "ordinal", "cycle_index"}, (module, called)
    assert "episode_cycle" in called
