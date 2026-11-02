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

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": self._cid, "alive": True,
                    "name": "seat", "skills": {"martial": 20}, "traits": []}
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


def test_the_phase_writes_the_genome_for_this_seat_and_no_toggle():
    """cleanup 2026.11: the per-character subsystem mask (a record the engine
    never read) is deleted. The phase is still per-seat, not a no-op: it writes
    this seat's genome."""
    gw, env = _run("expansion")
    actions = [a for a, _ in gw.writes]
    assert "set_ai_personality" in actions
    assert not [a for a in actions if "subsystem" in a], actions
    res = (env or {}).get("result") or {}
    assert not [k for k in res if "subsystem" in k or k.startswith("declare_war_")], res


# ── overrides go through the same check steer uses ───────────────────────────

@pytest.mark.parametrize("overrides,needle", [
    ({"CallAllies": False}, "CallAllies"),
    ({"bravery": 50}, "bravery"),
    ({"boldness": "high"}, "boldness='high'"),
    ({"boldness": True}, "boldness=True"),
    ({"boldness": 500}, "boldness=500"),
    ({"greed": -101}, "greed=-101"),
])
def test_bad_overrides_are_refused_and_nothing_is_written(overrides, needle):
    gw = RecordingGateway()
    env = asyncio.run(redesign.set_phase_impl(gw, phase="expansion", character_id=30853,
                                              overrides=overrides))
    assert env["ok"] is False and env["blocked"]["gate"] == "invalid_argument"
    assert needle in env["blocked"]["summary"]
    assert gw.writes == [], gw.writes
    # the same words steer refuses with: one check, two callers
    assert redesign.personality_refusal(overrides) in env["blocked"]["summary"]


def test_control_good_overrides_are_written_over_the_phase():
    gw = RecordingGateway()
    env = asyncio.run(redesign.set_phase_impl(gw, phase="expansion", character_id=30853,
                                              overrides={"boldness": 100, "greed": -100}))
    assert env["ok"] is True
    (_, p), = [w for w in gw.writes if w[0] == "set_ai_personality"]
    assert p["boldness"] == 100 and p["greed"] == -100


class FaultingGateway(RecordingGateway):
    """set_ai_personality answers in a shape the bridge really sends for a write
    that did not run: MEMORY_ACCESS_FAULT_REPLY {"error":"memory_access_fault"}
    (bridge/include/bridge.h; answer_memory_access_fault in the personality
    handlers), or the client's {"status":"sent_no_confirmation"} on a timeout
    (mcp_server.py _action_bridge)."""

    def __init__(self, reply, cid=30853):
        super().__init__(cid)
        self.reply = reply

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "set_ai_personality":
            return dict(self.reply)
        return {"success": True, "applied": True, "ai_enabled": True}


def test_a_faulted_personality_write_blocks_the_phase():
    """L14 FF2 N-MAJ-1: the personality write IS the phase. Its fault used to
    become a `personality_write_warning` beside verdict applied. RED on b84e3634."""
    gw = FaultingGateway({"error": "memory_access_fault"})
    env = asyncio.run(redesign.set_phase_impl(gw, phase="expansion", character_id=30853))
    assert env.get("verdict") == "rejected" and env.get("ok") is False, env
    assert env["blocked"]["raw_reason"] == "memory_access_fault", env["blocked"]
    assert [a for a, _ in gw.writes] == ["set_ai_personality"], "nothing more is written"


def test_an_unconfirmed_personality_write_is_not_applied():
    gw = FaultingGateway({"status": "sent_no_confirmation", "action": "set_ai_personality"})
    env = asyncio.run(redesign.set_phase_impl(gw, phase="expansion", character_id=30853))
    assert env.get("verdict") != "applied", env
    assert env["blocked"]["gate"] == "confirmation_pending", env["blocked"]
