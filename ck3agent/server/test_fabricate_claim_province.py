"""What the reverse engineering of the fabricate-claim task gives the toolkit
(re/findings/re_fabricate_claim.md).

- A county task's target is a PROVINCE; a current bridge reads the county the game derives from
  it (title(province+0x318).DE_JURE_PARENT) and says so by sending `target_province_id` beside
  `target_title_id`. An older bridge's `target_title_id` is the province id read as a title.
- The task's progress is CActiveCouncilTask+0x20 (x100000, 100 % = 10,000,000); a current bridge
  sends it as `progress` in percent on the seat row.
- The claim letter's price is court_chaplain_fabricate_claim_county_cost: (40 - learning) x 3,
  at least a RANDOM 40-60 (`value = { 40 60 }`), so above learning 20 only a range is known.
- query_fabricate_targets of a current bridge judges each county through its own province
  (`target_scope: county_province`); proven live (test_fabricate_claim_reads.py), so it decides.

Rows quoted from the dumps are in the findings (§2, §3); the game-file rows are skipped without
an install.
"""
import asyncio
import os
import re
from pathlib import Path

import pytest

import news
import redesign as R
from test_ireland_walk_replies import Gw

ME = 31506
C_ATHLONE, C_LEINSTER = 469, 508
P_ATHLONE = 34           # c_athlone's capital province (dumps 867 and 1097; live)
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    news._FABRICATING.clear()
    news._FAB_CHAPLAIN.clear()
    news._FAB_SEEN.clear()
    yield
    news.reset()


def refab_seats(progress=52.0, province=P_ATHLONE, county=C_ATHLONE, key="c_athlone"):
    """query_council_tasks of a current bridge: the chaplain fabricating on Athlone."""
    return {"character_id": ME, "seats": [
        {"position_id": 9488, "position_key": "councillor_court_chaplain", "task_name": "task_fabricate_claim",
         "ruler_id": ME, "councillor_id": 46877, "target_province_id": province,
         "target_title_id": county, "target_title": "Athlone", "target_title_key": key,
         "progress": progress, "frozen": False},
        {"position_id": 9485, "position_key": "councillor_steward", "task_name": "task_collect_taxes",
         "ruler_id": ME, "councillor_id": 43294}]}


# ── the target: a current bridge names the county the game derives ─────────────────────

def test_seat_target_is_the_county_the_bridge_read():
    seats = R.council_task_seats(refab_seats())
    fab = next(s for s in seats if s["task_name"] == "task_fabricate_claim")
    assert fab["target_province_id"] == P_ATHLONE
    assert fab["target"] == {"title_id": C_ATHLONE, "title_key": "c_athlone"}
    assert fab["target_read"] == "game"
    out = R.council_task_rows(refab_seats(), "1066.10.20")
    assert out["target_note"] == R.COUNCIL_TARGET_NOTE_GAME


def test_an_older_bridge_row_keeps_target_unknown():
    """The reading stands for a reply without target_province_id: the id is a province."""
    old = {"character_id": ME, "seats": [{"task_name": "task_fabricate_claim", "position_key":
                                          "councillor_court_chaplain", "target_title_id": P_ATHLONE,
                                          "target_title": "Blue Horde", "target_title_key": "e_blue_horde"}]}
    fab = R.council_task_seats(old)[0]
    assert fab["target"] == "unknown" and fab["target_province_id"] == P_ATHLONE
    assert "target_read" not in fab


# ── the progress: read, and the ETA from two reads ────────────────────────────────────

def test_progress_is_read_when_the_bridge_sends_it():
    fs = news.fabrication_status(ME, R.council_task_seats(refab_seats(52.0)), "1066.10.20")
    assert fs["running"] is True and fs["progress"] == 52.0
    assert "progress_unread" not in fs
    assert fs["target"] == {"title_id": C_ATHLONE, "title_key": "c_athlone"}


def test_progress_unread_from_an_older_bridge():
    rows = R.council_task_seats({"character_id": ME, "seats": [{"task_name": "task_fabricate_claim",
                                                               "position_key": "councillor_court_chaplain",
                                                               "target_title_id": P_ATHLONE}]})
    fs = news.fabrication_status(ME, rows, "1066.10.20")
    assert fs["progress"] is None and fs["progress_unread"].startswith("unknown")


def test_measured_rate_gives_days_left():
    news.fabrication_status(ME, R.council_task_seats(refab_seats(40.0)), "1066.10.20")
    fs = news.fabrication_status(ME, R.council_task_seats(refab_seats(46.0)), "1066.11.19")
    # 6 points in 30 days: one monthly step of 6 (live: the progress moves once every 30 days), 54 left
    assert {k: fs["progress_rate"][k] for k in ("per_day", "from", "days", "steps", "monthly_step")} == \
        {"per_day": 0.2, "from": "1066.10.20", "days": 30, "steps": 1, "monthly_step": 6.0}
    assert fs["days_left_measured"] == 270


def test_a_progress_drop_restarts_the_measure():
    news.fabrication_status(ME, R.council_task_seats(refab_seats(80.0)), "1066.10.20")
    fs = news.fabrication_status(ME, R.council_task_seats(refab_seats(2.0)), "1066.11.19")
    assert "progress_rate" not in fs and "days_left_measured" not in fs
    # the measure starts again at the drop: 2 -> 8 in 30 days is one step of 6; 92 points are 16 steps
    fs = news.fabrication_status(ME, R.council_task_seats(refab_seats(8.0)), "1066.12.19")
    assert fs["progress_rate"]["from"] == "1066.11.19" and fs["progress_rate"]["monthly_step"] == 6.0
    assert fs["days_left_measured"] == 480


def test_days_left_at_most_from_the_rate_and_no_overdue_guess():
    gw = Gw({"query_county_neighbors": {"neighbors": [{"realm_ruler_id": ME}]}})
    fs = news.fabrication_status(ME, R.council_task_seats(refab_seats(50.0)), "1068.09.15")
    snap = {"actor_id": ME, "date": "1068.09.15", "chaplain_learning": 10,
            "council": {"councillor_court_chaplain": {"holder_id": 46877}}}
    run(news._fabrication_timing(gw, snap, {"date": "1068.08.15"}, fs))
    # 3 + 10/5 = 5 %/month at least, applied daily as /30: 50 points take at most 300 days
    assert fs["expected"]["monthly_pct_at_least"] == 5.0
    assert fs["expected"]["days_left_at_most"] == 300
    assert "overdue" not in fs, "with the progress read, months elapsed are not a guess at it"
    assert "progress_unread" not in fs


# ── the letter's price ────────────────────────────────────────────────────────────────

def test_letter_price_exact_up_to_learning_20_then_a_range():
    assert news.claim_letter_price(10)["gold"] == 90.0 and news.claim_letter_price(10)["exact"] is True
    assert news.claim_letter_price(20)["gold"] == 60.0
    p = news.claim_letter_price(25)
    assert p["exact"] is False and (p["gold_low"], p["gold_high"]) == (45.0, 60.0)
    p = news.claim_letter_price(35)
    assert (p["gold_low"], p["gold_high"]) == (40.0, 60.0)
    assert news.claim_letter_price(None) is None
    assert "40-60" in news.CLAIM_PRICE_RULE


def test_letter_price_rides_the_running_fabrication():
    gw = Gw({"query_county_neighbors": {"neighbors": [{"realm_ruler_id": ME}]}})
    fs = news.fabrication_status(ME, R.council_task_seats(refab_seats(50.0)), "1068.09.15")
    snap = {"actor_id": ME, "date": "1068.09.15", "chaplain_learning": 12,
            "council": {"councillor_court_chaplain": {"holder_id": 46877}}}
    run(news._fabrication_timing(gw, snap, {"date": "1068.08.15"}, fs))
    assert fs["letter_price"]["gold"] == 84.0


def test_price_formula_is_the_game_files():
    p = GAME / "common/script_values/99_court_chaplain_values.txt"
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    t = p.read_text(encoding="utf-8-sig", errors="replace")
    block = t[t.index("court_chaplain_fabricate_claim_county_cost = {"):]
    block = block[:block.index("\n}")]
    assert "add = 40" in block and "subtract = learning" in block
    assert "multiply = court_chaplain_fabricate_claim_county_base" in block
    assert re.search(r"court_chaplain_fabricate_claim_county_base = 3\b", t)
    assert re.search(r"court_chaplain_fabricate_claim_county_min_cost = \{\s*value = \{ 40 60 \}", t)


# ── the game's verdict: from a current bridge, acted on ─────────────────

def test_council_tasks_list_from_a_refab_bridge_carries_no_list_warning():
    scan = {"success": True, "target_scope": "county_province", "task_seat": "councillor_court_chaplain",
            "assignable_count": 1, "targets": [{"title_id": C_ATHLONE, "title_key": "c_athlone",
                                                 "holder_id": 29585, "target_province_id": P_ATHLONE,
                                                 "assignable": True, "relevance": 0, "de_jure": True}]}
    gw = Gw({"query_fabricate_targets": scan})
    env = run(R.do_impl(gw, action_class="council_tasks", character_id=ME))
    codes = [w["code"] for w in env.get("warnings") or []]
    # Live: the county_province scan is proven live (test_fabricate_claim_reads.py), so no warning of either kind
    assert "targets_unproven_live" not in codes and "targets_not_ruler_specific" not in codes
    row = env["result"]["available"][0]
    assert row["target_province_id"] == P_ATHLONE


def test_council_tasks_list_from_an_older_bridge_keeps_its_warning():
    scan = {"success": True, "assignable_count": 1, "targets": [{"title_id": 223, "assignable": True}]}
    gw = Gw({"query_fabricate_targets": scan})
    env = run(R.do_impl(gw, action_class="council_tasks", character_id=ME))
    codes = [w["code"] for w in env.get("warnings") or []]
    assert "targets_not_ruler_specific" in codes and "targets_unproven_live" not in codes


def test_one_county_verdict_refuses_the_send():
    verdict = {"target_scope": "county_province",
               "verdict": {"title_id": C_ATHLONE, "holder_id": 29585, "target_province_id": P_ATHLONE,
                           "eligible": False, "assignable": False}}
    gw = Gw({"query_fabricate_targets": verdict,
             "query_title_hierarchy": {"title": {"title_id": C_ATHLONE, "holder_id": 29585}}})
    refused, chk = run(R._fabrication_game_check(gw, ME, {"target_title_id": C_ATHLONE}))
    # Live: the verdict is proven live (test_fabricate_claim_reads.py), so it decides
    assert refused["blocked"]["raw_reason"] == "game_check_refuses" and refused["verdict"] == "rejected"
    assert chk["game_verdict"] == {"eligible": False, "assignable": False, "target_province_id": P_ATHLONE}
    assert gw.writes == []
    assert ("query_fabricate_targets", {"actor_id": ME, "title_id": C_ATHLONE}) in gw.reads


def test_one_county_verdict_from_an_older_bridge_is_not_read():
    gw = Gw({"query_fabricate_targets": {"success": True, "targets": []},
             "query_title_hierarchy": {"title": {"title_id": C_ATHLONE, "holder_id": 29585}}})
    refused, chk = run(R._fabrication_game_check(gw, ME, {"target_title_id": C_ATHLONE}))
    assert refused is None and "game_verdict" not in chk
