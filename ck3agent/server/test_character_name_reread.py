"""L20-B item 4 (smoke17 new item 4): query(domain="character") names the
character, or says the name is pending — never a bare null.

THE SMOKE. probe-paused.jsonl lines 17 and 19 (id.12015, id.11329; 0867.02.20,
paused): the result had `name_key` "Charles" / "Bhoja" and `name_pending: true`
and no `name`, while the actor block of B3 (line 24, a few seconds later) named
11329 "Bhoja". 13191 (line 18) was named: an id the session had read before.

THE CAUSE. The bridge's reader thread cannot call the game's localizer: a name
cache miss answers name_key + name_pending and queues the id, and the drain on
the game thread (bridge_init.cpp populate_name_cache, after each pass; the
smoke's bridge.log has "Name cache: resolved N names" lines while the date
stood at 0867.02.20) fills the cache. query(domain="character") made ONE read,
so the first read of an id in a session was always the pending one.
THE FIX re-reads while the drain catches up (NAME_PENDING_REREADS_S), and a
name still pending after that is said: name null, name_pending, name_note.
"""

import asyncio
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
ms = pytest.importorskip("mcp_server")
from test_query_tool import bridge  # noqa: E402,F401  (the fake-bridge fixture)

#: the keys of probe-paused.jsonl line 17's result that bear on the name
PENDING = {"id": 12015, "name_key": "Charles", "name_pending": True, "alive": True,
           "house_name": "Karling", "primary_title_name": "West Francia"}
NAMED = {"id": 12015, "name": "Charles", "alive": True, "house_name": "Karling",
         "primary_title_name": "West Francia"}


@pytest.fixture(autouse=True)
def no_sleep(monkeypatch):
    async def instant(_s):
        return None
    monkeypatch.setattr(ms.asyncio, "sleep", instant)


#: how many query_character reads the doubles below have answered
_READS = []


@pytest.fixture(autouse=True)
def fresh_reads():
    _READS.clear()


def pending_twice_then_named(_params):
    _READS.append(1)
    return NAMED if len(_READS) > 2 else PENDING


def always_pending(_params):
    return PENDING


def always_named(_params):
    return NAMED


def test_a_name_the_drain_resolves_is_the_name(bridge):
    bridge.replies["query_character"] = pending_twice_then_named
    env = asyncio.run(ms.query(domain="character", character_id=12015))
    assert env["ok"] is True
    assert env["result"]["name"] == "Charles", env["result"]
    assert "name_pending" not in env["result"]
    assert bridge.actions().count("query_character") == 3


def test_a_name_still_pending_is_said_never_a_bare_null(bridge):
    bridge.replies["query_character"] = always_pending
    env = asyncio.run(ms.query(domain="character", character_id=12015))
    r = env["result"]
    assert "name" in r and r["name"] is None
    assert r["name_pending"] is True and r["name_key"] == "Charles"
    assert r["name_note"] == ms.NAME_PENDING_NOTE
    assert bridge.actions().count("query_character") == 1 + len(ms.NAME_PENDING_REREADS_S)


def test_control_a_named_character_is_read_once_and_unchanged(bridge):
    bridge.replies["query_character"] = always_named
    env = asyncio.run(ms.query(domain="character", character_id=12015))
    assert env["result"] == NAMED
    assert bridge.actions().count("query_character") == 1
