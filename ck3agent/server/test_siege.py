"""What holds a siege, and which side of a war the seat is on.

a recorded game (Björn, 867): five offensive wars, none won; "siege progress 20" for months (13 reads, 3 wars white-peaced on
`siege_full_not_fallen`); war 5 read "attacker" in war_stance and "defender" in the committed-strength block of cbs,
declare_war and plan_war. The bridge read the progress at CSiege+0x1b0, which is the next pooled siege's 20-day event
timer (re/findings/re_siege_progress_struct.md §10); the committed-strength reader called a co-attacker a defender.

The replay (fixtures/siege_replies_sweden867.json.gz, the run's recorded tool replies, reply fields only) runs every recorded
war_stance siege row and the recorded war 5 through the new code. The controls run the rule this replaces on the same input:
the committed-strength side expression it read (copied below and labelled) and the siege verdict that
called 19.5 of 20 "won".
"""
from __future__ import annotations

import asyncio
import gzip
import json
from pathlib import Path

import redesign as R
from test_ireland_walk_replies import Gw

HERE = Path(__file__).resolve().parent
FX = json.loads(gzip.open(HERE / "fixtures" / "siege_replies_sweden867.json.gz", "rt").read())
BJORN = FX["seat_ruler"]


def run(c):
    return asyncio.new_event_loop().run_until_complete(c)


# ── the replay: every recorded siege row is a row of the superseded read, and none is judged ──────────────────────────────

def _recorded_sieges():
    return [s for r in FX["war_stance_rows"] for s in r["sieges"]]


def test_replay_fixture_is_the_run():
    rows = FX["war_stance_rows"]
    assert len(rows) == 81 and len(_recorded_sieges()) == 47
    # the recorded verdict the seat white-peaced on
    assert sum("siege_full_not_fallen" in r["limitation_codes"] for r in rows) == 11


def test_replay_every_recorded_siege_row_has_its_progress_unread():
    for s in _recorded_sieges():
        (row,), verdict = R.siege_rows([s], 0)
        assert verdict == "unread", s
        assert row["progress_unread"] == R.SIEGE_PROGRESS_MISREAD
        assert not {"progress", "progress_pct", "progress_max", "status", "state"} & set(row), row


def test_replay_the_eleven_full_not_fallen_rows_read_progress_unread_now():
    hit = 0
    for r in FX["war_stance_rows"]:
        if "siege_full_not_fallen" not in r["limitation_codes"]:
            continue
        war = {"war_id": r["war_id"], "attacker_id": r["attacker_id"], "defender_id": r["defender_id"],
               "is_attacker": r["side"] == "attacker", "name": "w"}
        gw = Gw({"get_active_wars": {"wars": [war]},
                 "query_wars": {"wars": [{"war_id": r["war_id"], "sieges": r["sieges"]}]},
                 "query_war_score_breakdown": {"total_warscore": 0, "occupation_score": r["occupation_score"] or 0},
                 "query_war_stance": {}})
        R._WAR_STANCE_UNSERVED.clear()
        (w,) = run(R._war_stance_view(gw, BJORN))["wars"]
        codes = [x["code"] for x in w["limitations"]]
        assert "siege_full_not_fallen" not in codes and "siege_progress_unread" in codes, codes
        # an attacker's war: no stall verdict on an unread siege; a defender's war is never a stall
        assert w["stall_suspect"] is (None if r["side"] == "attacker" else False)
        hit += 1
    assert hit == 11


def test_CONTROL_the_pre_fix_verdict_said_won_on_the_same_rows():
    """The rule this replaces: status "won" when progress >= 20 - 0.5, which the server
    relabelled "progress_full". Runs on the recorded rows: the recorded label is
    what the rule gives on their recorded progress, 22 rows of 47 at the "cap"."""
    def pre_fix(p):
        return "progress_full" if p >= 19.5 else "other"
    rec = _recorded_sieges()
    assert all((pre_fix(s["progress"]) == "progress_full") == (s["status"] == "progress_full") for s in rec)
    assert sum(s["status"] == "progress_full" for s in rec) == 22


# ── the bridge's new rows: what holds each siege, and the lever ───────────────────────────────────────────

ROW = {"siege_id": 7, "besieger_id": BJORN, "besieger_side": "attacker", "holding_title_id": 141, "progress": 13.7,
       "progress_needed_max": 400.0, "progress_pct": 3, "fort_level": 4, "tier_needed": 1, "fort_speed": 0.7,
       "garrison": 113, "men": 554, "siege_tier": 0, "walls": 0, "next_event_days": 9}


def test_progressing_names_the_days_and_the_siege_weapons_it_wants():
    (row,), verdict = R.siege_rows([dict(ROW, status="progressing", eta_days=542)])
    assert verdict is None
    assert "542 days" in row["next"] and "siege tier 1" in row["next"] and "x0.7" in row["next"]


def test_blocked_rows_name_what_holds_them():
    cases = {"garrison_larger": "garrison (1000) outnumbers your 775", "in_battle": "battle",
             "moving": "marching on", "no_besieger": "held army sieges only where it stands"}
    for blocker, want in cases.items():
        x = dict(ROW, status="blocked", blocker=blocker, garrison=1000, men=775 if blocker == "garrison_larger" else 0)
        (row,), verdict = R.siege_rows([x])
        assert verdict == "all_blocked" and want in row["next"], (blocker, row["next"])


def test_the_stance_view_on_new_rows():
    war = {"war_id": 77, "attacker_id": BJORN, "defender_id": 14131, "is_attacker": True, "name": "w"}

    def view(sieges):
        gw = Gw({"get_active_wars": {"wars": [war]}, "query_wars": {"wars": [{"war_id": 77, "sieges": sieges}]},
                 "query_war_score_breakdown": {"total_warscore": 0, "occupation_score": 0}, "query_war_stance": {}})
        R._WAR_STANCE_UNSERVED.clear()
        return run(R._war_stance_view(gw, BJORN))["wars"][0]

    w = view([dict(ROW, status="progressing", eta_days=542)])
    assert w["stall_suspect"] is False and not any(x["code"].startswith("siege") for x in w["limitations"])
    w = view([dict(ROW, status="blocked", blocker="garrison_larger", garrison=1000, men=775)])
    assert w["stall_suspect"] is None and "sieges_blocked" in [x["code"] for x in w["limitations"]]


# ── war side: one definition ──────────────────────────────────────────────────────────────────────────────

def _war5():
    (r,) = [x for x in FX["war_stance_rows"] if x["war_id"] == 5][:1]
    return {"war_id": 5, "name": "w5", "attacker_id": r["attacker_id"], "defender_id": r["defender_id"],
            "is_attacker": r["side"] == "attacker"}


def test_replay_war5_recorded_two_sides():
    assert {x["side"] for x in FX["war_stance_rows"] if x["war_id"] == 5} == {"attacker"}
    assert {w["side"] for c in FX["committed"] for w in c["wars"] if w["war_id"] == 5} == {"defender"}
    assert _war5()["attacker_id"] != BJORN


def test_war5_reads_attacker_in_both_readers_now():
    w = _war5()
    assert R.my_war_side(w, BJORN) == "attacker"
    gw = Gw({"query_character": lambda p: {"id": p["character_id"], "military_strength": 1000}})
    cs = run(R.committed_strength(gw, BJORN, {"wars": [w]}))
    assert cs["wars"][0]["side"] == "attacker" and 5 in cs["offensive_open"]


def test_CONTROL_the_pre_fix_expression_reads_defender_for_war5():
    """The committed_strength expression this replaces, copied: the recorded "defender"."""
    w, me = _war5(), BJORN
    att = R._as_int(w.get("attacker_id")) == me or (bool(w.get("is_attacker")) and R._as_int(w.get("attacker_id")) in (0, me))
    assert att is False


def test_my_war_side_unread_is_none_and_counts_nothing():
    assert R.my_war_side({"war_id": 1, "attacker_id": 5, "defender_id": 6}, BJORN) is None
    gw = Gw({"query_character": lambda p: {"id": p["character_id"], "military_strength": 1000}})
    cs = run(R.committed_strength(gw, BJORN, {"wars": [{"war_id": 1, "attacker_id": 5, "defender_id": 6}]}))
    assert cs["wars"][0]["side"] is None and "unread" in cs["wars"][0] and cs["committed"] == 0
