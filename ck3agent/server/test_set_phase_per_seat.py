"""ck-zgh4 deploy-51 — `set_phase` writes nothing outside the calling seat.

RULED by the progenitor, verbatim: "Remove the world-wide write". The option
read: "set_phase stops touching the world-wide war switch. Seats lose that lever
but can still declare wars directly with declare_war. Removes the cross-seat
coupling and the A/B-vs-C imbalance; disclosed as a change in what seats can do,
with no pooling across it."

WHAT IT USED TO DO. Every phase pin called
`toggle_ai_subsystem(subsystem="DeclareWar")` — ONE engine byte gating offensive
war declarations for EVERY AI ruler in the world. The code labelled it "⚠
WORLD-WIDE blast radius" in its own result, which was honest about a thing that
should not have been on a per-seat verb.

WHY IT MATTERED MORE THAN A BLAST RADIUS:
  - SIX seats called it every cycle in arms A and B; ONE did in arm C. So the
    seats of a multi-seat arm were coupled through a global byte and were never
    independent — and the coupling itself DIFFERED BY ARM.
  - it is the only ENGINE-ENFORCED declaration lever, so removing it changes
    what a seat can DO, not only what it reads.

THESE TESTS DRIVE THE REAL `set_phase_impl` through a recording gateway, so they
observe the writes the function actually issues rather than reading its source.
A source assertion would pass on a file whose comments still describe the old
behaviour — and ours deliberately do, because the removal needs its reason kept.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")


class RecordingGateway:
    """Every write and read, in order. Nothing is dispatched anywhere."""

    def __init__(self, cid=30853):
        self.writes: list[tuple[str, dict]] = []
        self.reads: list[tuple[str, dict]] = []
        self._cid = cid
        self.health = {"ok": True}

    async def multi_actor(self):
        return False

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"character": {"id": self._cid, "alive": True, "is_player": False,
                                  "name": "seat", "martial": 20, "traits": []}}
        return {}

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return {"ok": True}

    def game(self):
        return {"tick": 1, "game_date": "1066.09.15"}


def _run(phase, cid=30853):
    gw = RecordingGateway(cid)
    env = asyncio.run(redesign.set_phase_impl(gw, phase=phase, character_id=cid))
    return gw, env


def test_set_phase_NEVER_writes_the_world_wide_DeclareWar_byte():
    """THE ROW THE RULING ASKED FOR. Driven, not grepped."""
    gw, env = _run("expansion")
    actions = [a for a, _ in gw.writes]
    assert "toggle_ai_subsystem" not in actions, (
        "set_phase issued a world-wide write: " + repr(gw.writes))
    assert actions, "the function wrote nothing at all — the test would be vacuous"


@pytest.mark.parametrize("phase", sorted(redesign.PHASE_GENOME))
def test_NO_phase_writes_the_global_byte(phase):
    """Every phase, not just the one that used to set it ON. The old code routed
    the phase's disposition to the global byte in BOTH directions — expansion
    turned it on, defensive phases turned it OFF, which froze the world's AI
    declarations — so a row that only checked `expansion` would miss half of it."""
    gw, _ = _run(phase)
    assert "toggle_ai_subsystem" not in [a for a, _ in gw.writes]


def test_TWO_SEATS_WITH_OPPOSITE_PHASES_leave_the_global_byte_untouched():
    """The control the ruling named. Two seats, opposite phases, as the six seats
    of an arm-A episode did to each other every cycle."""
    phases = sorted(redesign.PHASE_GENOME)
    a, b = phases[0], phases[-1]
    assert a != b, "need two distinct phases for this to be a real opposition"
    gw_a, _ = _run(a, cid=30853)
    gw_b, _ = _run(b, cid=35395)
    both = [x for x in gw_a.writes + gw_b.writes if x[0] == "toggle_ai_subsystem"]
    assert not both, f"a world-wide write survived: {both}"
    # and each seat wrote ONLY about its own character
    for gw, cid in ((gw_a, 30853), (gw_b, 35395)):
        foreign = [(act, p) for act, p in gw.writes
                   if "character_id" in p and p["character_id"] not in (0, cid)]
        assert not foreign, f"seat {cid} wrote about another character: {foreign}"


def test_the_PER_CHARACTER_bookkeeping_write_IS_STILL_MADE():
    """The ruling was explicit that the per-character map stays as it is. A fix
    that removed BOTH writes would be a different change — and would make the
    phase a no-op rather than a per-seat one."""
    gw, _ = _run("expansion")
    assert "set_character_subsystems" in [a for a, _ in gw.writes]


def test_the_RESULT_does_not_claim_a_global_write_and_is_HONEST_about_the_map():
    """A result that still reported `declare_war_global_byte` would describe a
    write that no longer happens — the shape of defect item (3) is about."""
    _, env = _run("expansion")
    res = (env or {}).get("result") or {}
    assert "declare_war_global_byte" not in res
    note = str(res.get("declare_war_note", ""))
    assert "WORLD-WIDE" not in note.upper()
    assert "this character only" in note and "does not" in note, (
        "the note must say the engine does not enforce the recorded value, "
        "because it does not: the per-char map is engine_enforced:false")
    assert "declare_war" in note, "it should name the call that DOES start a war"
