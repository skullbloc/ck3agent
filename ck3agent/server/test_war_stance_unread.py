"""WSTANCE: a war row the bridge could not label is not the engine's word.

v3pilot5/6 (2026-11-10): of 32 inspect(war_stance) reads (30 in pilot5, 2 in
pilot6), 29 came back stance "unavailable" under stance_source
"engine_stance_db" (wars 10, 11 and 27; K seats and one D seat), and 3 were
inferred. The pilot bridge's coordinator-record walk was default OFF and
nothing armed it, so do_query_war_stance answers every row with the placeholder
"unavailable", stance_known false, and will_siege = occupation > 0 (an
occupation bit, not a stance bit). The server passed the placeholder through
as the engine's stance. Now the row is inferred, and `stance_unread` names
which step did not see.
"""
import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
R = pytest.importorskip("redesign")

ME, ENEMY, WAR = 12015, 10465, 10


def _bridge_row(**kw):
    """The row shape do_query_war_stance emits (query_handlers.cpp)."""
    row = {"war_id": WAR, "primary_attacker": ME, "primary_defender": ENEMY,
           "is_attacker": True, "stance": "unavailable", "stance_known": False,
           "occupation_score": 0, "will_siege": False, "stuck": False,
           "advice": "indeterminate"}
    row.update(kw)
    return row


def _bridge(walk_enabled=False, walk_ran=False, db=True, rows=None):
    return {"success": True, "action": "query_war_stance", "character_id": ME,
            "stance_db_resolved": db, "record_walk_enabled": walk_enabled,
            "record_walk_ran": walk_ran, "wars": [_bridge_row()] if rows is None else rows}


def _view(native, occupation=0):
    class GW:
        async def read(self, verb, params):
            if verb == "get_active_wars":
                return {"wars": [{"war_id": WAR, "name": "W", "is_attacker": True,
                                  "attacker_id": ME, "defender_id": ENEMY, "war_score": 0}]}
            if verb == "query_war_score_breakdown":
                return {"occupation_atk": occupation}
            if verb == "query_war_stance":
                return native
            return {}

        def game(self):
            return {"tick": 1}

    R._WAR_STANCE_UNSERVED.clear()
    return asyncio.run(R._war_stance_view(GW(), ME))


def test_the_pilot_reply_is_not_shown_as_the_engines_stance():
    """The pilot case: walk off, the placeholder row."""
    view = _view(_bridge())
    row = view["wars"][0]
    assert row["stance"] != "unavailable", row
    assert row["stance"] == "attacker_not_sieging", row
    assert row["stance_source"] == "inferred_from_occupation", row
    assert view["stance_source"] == "inferred_from_occupation", view
    assert row["stance_unread"] == "record_walk_disabled", row
    assert row["will_siege"] is None, row      # the bridge's bit was occupation, not stance
    lim = [x for x in row["limitations"] if x["code"] == "stance_unread"]
    assert lim and lim[0]["reason_code"] == "record_walk_disabled", row


@pytest.mark.parametrize("native,reason", [
    (None, "query_war_stance_unserved"),
    (_bridge(rows=[]), "no_native_row"),
    (_bridge(walk_enabled=True, walk_ran=False, db=False), "stance_db_unresolved"),
    (_bridge(walk_enabled=True, walk_ran=False), "record_walk_not_run"),
    (_bridge(walk_enabled=True, walk_ran=True), "no_coordinator_record"),
    (_bridge(walk_enabled=True, walk_ran=True,
             rows=[_bridge_row(stance="unknown")]), "stance_ptr_unmapped"),
])
def test_each_unread_step_has_its_own_reason(native, reason):
    row = _view(native or {})["wars"][0]
    assert row["stance_source"] == "inferred_from_occupation", row
    assert row["stance_unread"] == reason, row
    assert reason in R._STANCE_UNREAD_DETAIL


def test_a_read_stance_is_still_the_engines_word():
    """Control: walk armed and matched (smoke21: 88/88, attacker_offensive)."""
    native = _bridge(walk_enabled=True, walk_ran=True, rows=[_bridge_row(
        stance="attacker_offensive", stance_known=True, will_siege=True,
        advice="wait_mobilize")])
    view = _view(native)
    row = view["wars"][0]
    assert row["stance"] == "attacker_offensive", row
    assert row["stance_source"] == "engine_stance_db", row
    assert view["stance_source"] == "engine_stance_db", view
    assert row["stance_unread"] is None, row
    assert row["will_siege"] is True and row["stall_suspect"] is False, row
    assert "stance_unread" not in [x["code"] for x in row["limitations"]], row


def test_a_row_without_stance_known_falls_back_on_the_placeholder_names():
    """An older bridge row with no stance_known: a real name is read, a placeholder is not."""
    assert R._stance_unread_reason({"wars": []}, {"stance": "attacker_defensive"}) is None
    assert R._stance_unread_reason({"wars": []}, {"stance": "unavailable"}) is not None


def test_a_reply_without_walk_keys_claims_no_walk():
    """5a: an older bridge's reply carries no record_walk_* keys. The reason must not
    say the walk ran (no_coordinator_record) when nothing reported that."""
    native = {"success": True, "wars": [_bridge_row()]}
    reason = R._stance_unread_reason(native, native["wars"][0])
    assert reason == "walk_status_unreported", reason
    detail = R._STANCE_UNREAD_DETAIL[reason]
    assert "walk ran" not in detail and "found no" not in detail, detail


def test_walk_not_run_names_every_cause_the_bridge_folds_into_it():
    """5b: record_walk_ran=false also covers 0 records and an unmapped record array
    (query_handlers.cpp, the n > 0 && is_mapped(arr) gate), not only an uncaptured
    AI manager."""
    detail = R._STANCE_UNREAD_DETAIL["record_walk_not_run"]
    for cause in ("not captured", "no records", "unreadable"):
        assert cause in detail, (cause, detail)


def test_the_walks_own_result_wins_over_a_partial_stance_table():
    """5c: stance_db_resolved is false for fewer than 7 mapped slots, yet the walk runs
    on any mapped DB. When it ran, its result is the reason, not the table."""
    ran = _bridge(walk_enabled=True, walk_ran=True, db=False)
    assert R._stance_unread_reason(ran, ran["wars"][0]) == "no_coordinator_record"
    unmapped = _bridge(walk_enabled=True, walk_ran=True, db=False,
                       rows=[_bridge_row(stance="unknown")])
    assert R._stance_unread_reason(unmapped, unmapped["wars"][0]) == "stance_ptr_unmapped"
    # control: the walk did not run and the table is unresolved -> the table is the reason
    not_ran = _bridge(walk_enabled=True, walk_ran=False, db=False)
    assert R._stance_unread_reason(not_ran, not_ran["wars"][0]) == "stance_db_unresolved"
