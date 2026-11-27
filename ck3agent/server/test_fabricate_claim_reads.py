"""Live: the fabricate-claim reads proved, and corrected, on a live game (node, CK3 1.18.3.1, the
1066.09.15 bookmark save, Murchad of Dublin 31506). The fixture (fixtures/fabricate_claim_replies_1066.json, built by
From the run records) holds the recorded replies.

- The game's own fabricate check for one county (query_fabricate_targets title_id) is the rule: it refused
  c_leinster, whose task the game reset within 15 days (script path) or refused at once (native), and over 40
  pairs of relatives it refused exactly the counties a living parent holds (10/10) or an explicit claim of the
  actor covers (2/2), accepting the rest (28/28). The kinship guess refused children's and siblings' counties too.
- The chaplain's learning is his learning: query_council's lean `skill` is the councillor's MARTIAL (8 against
  learning 12); the letter charged (40 - 12) x 3 = 84 gold, the toolkit had predicted 96.
- The progress moves in one step of the whole monthly rate every 30 days, (3 + 12/5) x 1.3 (the holder of
  another faith) = 7.02 %: the claim came on day 435 (15 steps). Days left measured from a per-day rate
  (429 at day 70) missed by 64 days; whole steps x 30 (390) bound the 365 that came.
"""
import asyncio
import json
from pathlib import Path

import pytest

import news
import redesign as R
from test_ireland_walk_replies import Gw

HERE = Path(__file__).parent
LIVE = json.loads((HERE / "fixtures" / "fabricate_claim_replies_1066.json").read_text())
ME, FATHER = 31506, 27052
C_ATHLONE, C_LEINSTER = 469, 508


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    news._FABRICATING.clear()
    news._FAB_CHAPLAIN.clear()
    news._FAB_SEEN.clear()
    news._FAB_PROGRESS.clear()
    yield
    news.reset()


def _ord(d):
    import cycle_cadence as cc
    return cc.ordinal(d)


# ── the game's verdict is the rule ────────────────────────────────────────────────────

def _pair_gw(pair):
    """The pair's actor with the recorded relation to the holder, and the game's recorded verdict."""
    a, h, rel = pair["actor"], pair["holder"], pair["rel"]

    def char(p):
        cid = p["character_id"]
        if cid == a:
            return {"id": a, "father_id": h if rel == "father" else (777 if rel == "sibling" else 701),
                    "mother_id": h if rel == "mother" else 702, "children": [h] if rel == "child" else []}
        return {"id": cid, "father_id": 777 if rel == "sibling" else 801, "mother_id": 802, "children": []}
    verdict = {"target_scope": "county_province",
               "verdict": {"title_id": pair["title_id"], "holder_id": h, "eligible": pair["eligible"],
                           "assignable": pair["eligible"]}}
    return Gw({"query_character": char, "query_fabricate_targets": verdict})


def test_live_the_games_verdict_decides_over_every_recorded_pair():
    pairs = LIVE["kin_pairs"]
    assert len(pairs) == 40
    parents = [p for p in pairs if p["rel"] in ("father", "mother")]
    assert parents and all(p["eligible"] is False for p in parents), "the game refused every parent's county"
    others = [p for p in pairs if p["rel"] in ("child", "sibling")]
    assert sum(p["eligible"] for p in others) == 28 and len(others) == 30
    # the 2 the game refused among children's and siblings' counties: the actor's explicit claims
    refused = {str(p["title_id"]) for p in others if not p["eligible"]}
    assert refused == {"586", "596"} and all(30841 in LIVE["explicit_claims"][t] for t in refused)
    for p in pairs:
        chk = run(news.fabricate_check(_pair_gw(p), p["actor"], p["holder"], title_id=p["title_id"]))
        assert (chk["game_check"] == "refuses") is (p["eligible"] is False), p
        assert chk["game_check"] in ("refuses", "accepts")


def test_live_leinster_is_refused_with_the_rule_that_holds():
    lein = LIVE["leinster"]
    # the game: the native set refused at once, the script set reset between +10 and +15 days
    assert lein["native_reply"]["reason"] == "target_rejected" and lein["native_reply"]["applied"] is False
    tasks = [(w["date"], w["task"]) for w in lein["script_watch"]]
    assert tasks[1] == ("1066.09.25", "task_fabricate_claim") and tasks[2] == ("1066.09.30", "task_religious_relations")
    assert all(w["verdict"]["eligible"] is False for w in lein["script_watch"])
    # every clause of potential_county read on its own holds, but for has_claim_on, which no claim read shows
    assert lein["in_diplomatic_range"] is True and lein["claims_of_murchad"] == 0
    assert lein["claimants_of_c_leinster"] == 0
    fam = LIVE["family"]
    assert fam["father_id"] == FATHER and fam["mother_id"] != FATHER
    gw = Gw({"query_character": lambda p: ({"id": ME, **fam} if p["character_id"] == ME else {"id": p["character_id"]}),
             "query_fabricate_targets": {"target_scope": "county_province", "verdict": LIVE["verdicts"]["508"]},
             "query_title_hierarchy": {"title": {"title_id": C_LEINSTER, "holder_id": FATHER}}})
    refused, chk = run(R._fabrication_game_check(gw, ME, {"target_title_id": C_LEINSTER}))
    assert refused["blocked"]["gate"] == "target_ineligible" and "your parent" in refused["blocked"]["summary"]
    assert chk["game_verdict"]["eligible"] is False and "proven_live" not in chk["game_verdict"]
    assert "not read" not in refused["blocked"]["summary"]


def test_live_athlone_is_sent_on_the_games_word():
    gw = Gw({"query_fabricate_targets": {"target_scope": "county_province", "verdict": LIVE["verdicts"]["469"]},
             "query_title_hierarchy": {"title": {"title_id": C_ATHLONE, "holder_id": 29585}}})
    refused, chk = run(R._fabrication_game_check(gw, ME, {"target_title_id": C_ATHLONE}))
    assert refused is None and chk["game_check"] == "accepts"
    assert chk["game_verdict"]["target_province_id"] == 34


def test_live_the_scan_lists_irelands_counties_at_their_provinces():
    scan = LIVE["scan"]
    assert scan["target_scope"] == "county_province"
    ids = {r["title_id"] for r in scan["irish_rows"]}
    assert C_ATHLONE in ids and C_LEINSTER not in ids and len(ids) == 12
    assert next(r for r in scan["irish_rows"] if r["title_id"] == C_ATHLONE)["target_province_id"] == 34
    env = run(R.do_impl(Gw({"query_fabricate_targets": dict(scan, success=True, targets=scan["irish_rows"],
                                                             assignable_count=12)}),
                        action_class="council_tasks", character_id=ME))
    codes = [w["code"] for w in env.get("warnings") or []]
    assert "targets_unproven_live" not in codes and "targets_not_ruler_specific" not in codes


# ── the chaplain's learning, the letter's price ───────────────────────────────────────

def test_live_the_chaplains_learning_is_his_learning_not_the_martial_scalar():
    row, chap = LIVE["council_chaplain_row"], LIVE["chaplain"]
    assert row["skill"] == chap["skills"]["martial"] == 8 and chap["skills"]["learning"] == 12
    gw = Gw({"query_character": lambda p: {"id": p["character_id"], "skills": chap["skills"]}})
    assert run(news.chaplain_learning(gw, {"council": [row]})) == 12
    snap = news.snapshot(actor_id=ME, date="1066.09.15", chr_sect={}, wars={}, claims={},
                         council={"council": [row]}, heir=None)
    assert snap["chaplain_learning"] is None, "the lean row's `skill` is not the learning"
    # the letter: gold 141.01 at the pick, 57.01 after it on the same day
    pick = LIVE["letter_pick"]
    after = next(r["gold"] for r in LIVE["athlone_reads"] if r["date"] == LIVE["claim_date"])
    charged = round(pick["gold_at_pick"] - after, 2)
    assert charged == news.claim_letter_price(12)["gold"] == 84.0
    assert LIVE["toolkit_news_day70"]["letter_price"]["gold"] == 96.0, "the base read the martial 8"


# ── the progress: monthly steps, and the ETA ──────────────────────────────────────────

def _seats(progress):
    return R.council_task_seats({"character_id": ME, "seats": [
        {"position_id": 9488, "position_key": "councillor_court_chaplain", "task_name": "task_fabricate_claim",
         "ruler_id": ME, "councillor_id": 56090, "target_province_id": 34, "target_title_id": C_ATHLONE,
         "target_title": "Athlone", "target_title_key": "c_athlone", "progress": progress, "frozen": False}]})


def test_live_progress_moves_in_monthly_steps_of_the_expected_rate():
    reads = {}
    for r in LIVE["athlone_reads"]:
        if isinstance(r["progress"], (int, float)):
            reads[r["date"]] = r["progress"]
    days = {d: _ord(d) - _ord("1066.09.15") for d in reads}
    for d, p in reads.items():
        assert abs(p - 7.02 * (days[d] // 30)) < 1e-6, (d, p)     # one step of 7.02 every 30 days
    exp = news.fabrication_expected(12, True, other_faith=(LIVE["athlone_holder"]["faith_id"]
                                                           != LIVE["murchad_faith_id"]))
    assert exp["monthly_pct_at_least"] == 7.02 and exp["holder_other_faith"] is True


def test_live_days_left_from_two_reads_bound_the_claim_date():
    holder = LIVE["athlone_holder"]
    gw = Gw({"query_county_neighbors": {"neighbors": [{"realm_ruler_id": ME}]},
             "query_title_hierarchy": {"title": {"title_id": C_ATHLONE, "holder_id": holder["id"]}},
             "query_character": lambda p: {"id": p["character_id"],
                                           "faith_id": (LIVE["murchad_faith_id"] if p["character_id"] == ME
                                                        else holder["faith_id"])}})
    news.fabrication_status(ME, _seats(0.0), "1066.09.15")
    news.fabrication_status(ME, _seats(7.02), "1066.10.20")
    fs = news.fabrication_status(ME, _seats(14.04), "1066.11.24")
    snap = {"actor_id": ME, "date": "1066.11.24", "chaplain_learning": 12,
            "council": {"councillor_court_chaplain": {"holder_id": 56090}}}
    run(news._fabrication_timing(gw, snap, {"date": "1066.10.20"}, fs))
    came = _ord(LIVE["claim_date"]) - _ord("1066.11.24")
    assert came == 365
    assert LIVE["toolkit_news_day70"]["days_left_measured"] == 429      # the base: 64 days late
    left = fs["days_left_measured"]
    assert came <= left <= came * 1.10, left
    assert fs["progress_rate"]["monthly_step"] == 7.02 and fs["progress_rate"]["steps"] == 2
    assert fs["expected"]["days_left_at_most"] >= left
