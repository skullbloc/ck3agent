"""A grant reads its new holder once before answering (L14 minor 9).

The evaluator's grant 12618 -> 18679 (county 8182) answered pending
clock_advances ("an AI answers within days") while the very next read, with no
day passed (paused, tick 4 both sides), showed the courtier as holder. LEVERS
now names the holder readback for grant_titles_interaction; it stays a `days`
lever, so a holder not changed after the pass still waits for the clock.
"""
import asyncio

import pytest

import redesign as R

RULER, COURTIER, COUNTY = 12618, 18679, 8182


@pytest.fixture(autouse=True)
def _instant(monkeypatch):
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)


class Bridge:
    def __init__(self, holder, paused=True):
        self.holder, self.paused, self.health = holder, paused, "ok"
        self.reads = []

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append(action)
        if action == "query_character":
            return {"id": (params or {}).get("character_id") or RULER, "alive": True}
        if action == "query_dispatch_result":
            return {"success": True, "status": "success", "dispatch_id": (params or {}).get("dispatch_id")}
        if action == "query_title_succession":
            return {"title_id": COUNTY, "current_holder_id": self.holder} if self.holder else {}
        if action == "get_tick_info":
            return {"paused": self.paused, "game_date": "867.01.05", "tick_id": 4}
        return {}

    async def write(self, action, params=None):
        return {"success": True, "status": "queued", "intent_request_id": "si_12618_4_1"}

    async def clock(self):
        return R.clock_of(await self.read("get_tick_info", {}))

    def game(self):
        return {"tick": 4, "date": "867.01.05", "paused": self.paused}


def _grant(gw):
    return asyncio.run(R.do_impl(gw, intent="interact", character_id=RULER,
                                 args={"interaction": "grant_titles_interaction",
                                       "target_id": COURTIER, "title_id": COUNTY}))


def test_a_grant_whose_holder_changed_on_the_pass_is_applied_with_the_holder():
    env = _grant(Bridge(holder=COURTIER))
    assert env["verdict"] == "applied", env
    assert env["result"]["readback"] == {"title_id": COUNTY, "holder_id": COURTIER}


def test_a_grant_not_yet_held_paused_still_waits_for_the_clock():
    env = _grant(Bridge(holder=RULER, paused=True))
    assert env["verdict"] == "pending" and env["pending"]["drains_when"] == "clock_advances", env
    assert env["result"]["sent"] is True


def test_a_holder_that_could_not_be_read_is_pending_readback_never_rejected():
    env = _grant(Bridge(holder=0))
    assert env["verdict"] == "pending" and env["pending"]["drains_when"] == "readback", env


def test_the_lever_row_names_the_readback_and_stays_days():
    key, row = R.lever_of("interact", {"interaction_type": "grant_titles_interaction"})
    assert key == ("interact", "grant_titles_interaction")
    assert row["shows"] == "days" and "current_holder_id" in row["readback"]
