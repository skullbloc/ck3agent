"""lane-namekey: a name the bridge has not localized yet is PENDING, never a key.

The bridge's reader thread cannot call the game's localizer; on a name-cache
miss it queues the id for the game thread, which resolves it on its next tick
(bridge/src/name_tables.cpp). Before this lane, ad_read_char_name answered the
miss with the localization KEY stored on CCharacter (`E_thelred` for Æthelred),
so the first reads of a session — the game paused before any tick — handed
every client a key where a name belongs. The bridge now omits `name` and sends
`name_key` + `name_pending: true` (bridge/test/name_pending_probe.cpp drives the
real reader); these rows are the server's half: the echo every actor-bearing
tool returns carries that state through, and a localized reply is unchanged.
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

redesign = pytest.importorskip("redesign")
from test_no_ruler_bound import Gw, run  # noqa: E402  (the whoami fake gateway)


PENDING = {"request_id": "r3", "character": {"id": 13902, "alive": True,
                                              "name_key": "E_thelred", "name_pending": True}}
LOCALIZED = {"request_id": "r4", "character": {"id": 13902, "alive": True, "name": "Æthelred"}}


def test_actor_echo_carries_the_pending_state_and_never_the_key():
    out = redesign.actor_echo(PENDING, 13902)
    assert out["name"] is None
    assert out["name_pending"] is True and out["name_key"] == "E_thelred"


def test_actor_echo_of_a_localized_name_is_the_shape_of_before():
    out = redesign.actor_echo(LOCALIZED, 13902)
    assert out == {"id": 13902, "name": "Æthelred", "alive": True,
                   "is_controlled": True, "is_player": False}


def test_actor_echo_a_flat_section_also_carries_it():
    out = redesign.actor_echo({"id": 7, "name_key": "Bj_rn", "name_pending": True}, 7)
    assert out["name"] is None and out["name_pending"] is True and out["name_key"] == "Bj_rn"


def test_actor_echo_without_the_flag_adds_nothing():
    """A reply that names nobody and says nothing about it is the old shape:
    `name: None` and no pending keys. Absence is not pending."""
    out = redesign.actor_echo({"id": 7, "alive": True}, 7)
    assert out["name"] is None and "name_pending" not in out and "name_key" not in out


def test_whoami_controlled_echo_says_the_name_is_pending():
    gw = Gw(PENDING, sid="me",
            sessions=[{"session_id": "me", "primary_character_id": 13902,
                       "claimed_character_ids": [13902]}])
    out = run(redesign.session_impl(gw, "whoami"))
    assert out["ok"] is True and out["blocked"] is None
    ctrl = out["result"]["controlled"]
    assert ctrl["id"] == 13902 and ctrl["name"] is None
    assert ctrl["name_pending"] is True and ctrl["name_key"] == "E_thelred"


def test_whoami_controlled_echo_of_a_localized_name_has_no_pending_keys():
    gw = Gw(LOCALIZED, sid="me",
            sessions=[{"session_id": "me", "primary_character_id": 13902,
                       "claimed_character_ids": [13902]}])
    out = run(redesign.session_impl(gw, "whoami"))
    ctrl = out["result"]["controlled"]
    assert ctrl["name"] == "Æthelred"
    assert "name_pending" not in ctrl and "name_key" not in ctrl
