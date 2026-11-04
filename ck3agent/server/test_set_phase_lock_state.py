"""L18 ruling (b): set_phase surfaces the bridge's `lock_state`, and observe
carries it under posture.genome.

The 2026.11 pilots' first set_phase reverted in 12 of 32 seats while the reply
returned the declared genome (redesign.set_phase_impl, L18-A's note). L18-A's
bridge now says lock_state (live / quarantined / none) and world_epoch in both
personality replies (bridge/src/personality.cpp do_get_ai_personality,
do_set_ai_personality); the server passes the bridge's word through, one reading
(redesign.lock_state_of), and a reply without it reads not_reported, never live.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
R = pytest.importorskip("redesign")

BRIDGE = os.path.join(os.path.dirname(__file__), "..", "..", "bridge")


class GW:
    def __init__(self, write_reply, read_reply=None, cid=30853):
        self.write_reply, self.read_reply, self._cid = write_reply, read_reply or {}, cid
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        if action == "query_character":
            return {"id": self._cid, "alive": True, "name": "seat", "skills": {}, "traits": []}
        if action == "get_ai_personality":
            return dict(self.read_reply)
        return {}

    async def write(self, action, params=None):
        if action == "set_ai_personality":
            return dict(self.write_reply)
        return {"success": True, "applied": True, "ai_enabled": True}

    def game(self):
        return {"tick": 1, "game_date": "867.01.01"}


def _set(reply):
    return asyncio.run(R.set_phase_impl(GW(reply), phase="expansion", character_id=30853))


def test_the_emitter_says_the_keys_this_reads():
    src = open(os.path.join(BRIDGE, "src", "personality.cpp"), encoding="utf-8").read()
    assert src.count('r.key("lock_state")') == 2, "get and set must both emit lock_state"
    assert src.count('r.key("world_epoch")') == 2


@pytest.mark.parametrize("state,warned", [("live", False), ("quarantined", True), ("none", True)])
def test_set_phase_reply_carries_the_bridge_lock_state(state, warned):
    env = _set({"success": True, "character_id": 30853, "axes_written": 9,
                "lock_state": state, "world_epoch": 3})
    assert env["result"]["lock_state"] == state and env["result"]["world_epoch"] == 3, env["result"]
    codes = [w["code"] for w in env["warnings"]]
    assert ("personality_lock_not_live" in codes) is warned, codes


def test_CONTROL_a_bridge_that_does_not_say_reads_not_reported_never_live():
    env = _set({"success": True, "character_id": 30853, "axes_written": 9})
    assert env["result"]["lock_state"] == "not_reported"
    assert "personality_lock_not_live" not in [w["code"] for w in env["warnings"]]


def test_set_phase_read_carries_lock_state():
    gw = GW({}, read_reply={"character_id": 30853, "lock_state": "quarantined", "world_epoch": 2})
    env = asyncio.run(R.set_phase_impl(gw, phase=None, character_id=30853))
    assert env["result"]["lock_state"] == "quarantined"
    assert env["result"]["genome"]["lock_state"] == "quarantined"


def test_lock_state_of_reads_one_way():
    """observe's posture.genome uses the same reading (driven in
    test_observe_heir_guardian.py::test_observe_posture_genome_carries_the_bridge_lock_state)."""
    assert R.lock_state_of({"lock_state": "live"}) == "live"
    assert R.lock_state_of({"locked": True}) == "not_reported"
    assert R.lock_state_of(None) == "not_reported"
