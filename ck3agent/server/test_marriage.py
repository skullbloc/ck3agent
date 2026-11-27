"""Why no marriage sent through the seat tools applied, and the fixes.

Census of the recorded refused sends (fixtures/marriage_refused_sends.json.gz, built from
the run logs named in its provenance): every one is a refusal the game had grounds for,
and the tools gave the reader too little to see which:

  * a recorded game (Murchad, 1066.09.15): the listing offered landed rulers only;
    each answered `target_would_reject` (his ai_accept said no) and the reply said "the
    game gives no detail".
  * a recorded game (867): sends to the bridge's out-of-range listing (the driver
    asked require_in_range=0), each refused "too far away".
  * a recorded game (1068): Murchad's two sons paired with the two women of his
    court, who were Murchad's own wives (his profile: spouses [50191, 47793]); the game's
    is_shown said no and gave no words.

The fixes graded here: the refusal names the validity stage (failed_check) and the bars
the reads show; the listing checks every row's pair with the game (query_interaction_check
`pairs`) and offers foreign rulers' kin at court; the readback reads the alliance.
"""

import asyncio
import gzip
import json
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(__file__))

R = pytest.importorskip("redesign")
import tools  # noqa: E402

HERE = Path(__file__).resolve().parent
FX = json.loads(gzip.open(HERE / "fixtures" / "marriage_refused_sends.json.gz", "rt").read())
BRIDGE_SRC = HERE.parent.parent / "bridge" / "src"
MUNSTER = {"D", "E"}   # the fixture's two seated Munster games (1068), as its provenance names them


def run(coro):
    return asyncio.run(coro)


def _profiles(label):
    return {int(k): v for k, v in FX["profiles"].get(label, {}).items()}


def _reader(people, log=None, check=None):
    async def read(action, params):
        if log is not None:
            log.append((action, params))
        if action == "query_character":
            return people.get(int(params.get("character_id") or 0), {})
        if action == "query_interaction_check" and check is not None:
            return check(params)
        return {}
    return read


def _gate(s):
    return ((s["reply"] or {}).get("blocked") or {}).get("gate")


# ── the census: 64 recorded sends, each a refusal the game had grounds for ───────
def _cause(s):
    blk = (s["reply"] or {}).get("blocked") or {}
    detail, raw = str(blk.get("detail") or ""), str(blk.get("raw_reason") or "")
    if blk.get("gate") == "recipient_refuses":
        return "his answer was no"
    if "too far away" in detail:
        return "out of range"
    if "belongs to the clergy" in detail or "can not marry" in detail:
        return "the game named the bar"
    if raw.startswith("secondary_actor_not_living"):
        return "your side is dead"
    if raw == "send_conditions_not_met":
        return "send conditions, no words"
    if s["game"] in MUNSTER and detail == R.NO_GAME_DETAIL:
        return "validity, no words"
    return "other"


def test_census_every_recorded_send_is_a_refusal_with_a_cause():
    by = {}
    for s in FX["sends"]:
        assert (s["reply"] or {}).get("verdict") == "rejected"
        by[_cause(s)] = by.get(_cause(s), 0) + 1
    assert by == {"his answer was no": 16, "out of range": 33, "the game named the bar": 2,
                  "your side is dead": 3, "send conditions, no words": 2, "validity, no words": 8}, by
    # the 16 "no" answers and the 10 wordless refusals are the ones the old replies left unexplained
    assert all(s["reply"]["blocked"]["detail"] == R.NO_GAME_DETAIL for s in FX["sends"]
               if _cause(s) in ("his answer was no", "validity, no words", "send conditions, no words"))
    assert len(FX["sends"]) == 64


def test_munster_the_spouses_to_be_were_the_rulers_own_wives():
    """The fixture's own reads say why: Murchad's recorded profile lists both women as his spouses."""
    murchad = _profiles("D")[31762]
    assert {x["id"] for x in murchad["spouses"]} == {47793, 50191}
    sons = {36303, 15611}
    assert set(murchad["children"]) == sons
    assert {(s["args"]["args"]["secondary_actor_id"], s["args"]["args"]["secondary_recipient_id"])
            for s in FX["sends"] if s["game"] in MUNSTER} == {(a, b) for a in sons for b in (47793, 50191)}


# ── replay: the refusal now names the bar the reads show ─────────────────────────
@pytest.mark.parametrize("i", range(8))
def test_replay_each_munster_refusal_names_the_rulers_wife(i):
    sends = [s for s in FX["sends"] if s["game"] in MUNSTER]
    s = sends[i]
    env = json.loads(json.dumps(s["reply"]))
    env["verdict"] = "rejected"
    a = s["args"]["args"]
    run(R.marriage_refusal_reads(env, a, _reader(_profiles(s["game"]))))
    bars = env["blocked"].get("pair_bars") or []
    sa, sr = a["secondary_actor_id"], a["secondary_recipient_id"]
    if sa == 15611 and sr == 50191:
        assert "50191 is the mother of 15611" in bars, bars
    assert f"{sr} is a spouse of 31762, the father of {sa}" in bars, bars


def test_replay_CONTROL_a_pair_the_reads_show_free_says_what_it_read():
    """Non-discriminating control: a free pair gets no bar, only what was read."""
    people = {1: {"id": 1, "is_female": False, "spouses": [], "father_id": 3},
              2: {"id": 2, "is_female": True, "spouses": []},
              3: {"id": 3, "spouses": [{"id": 4, "alive": True}]}}
    env = {"verdict": "rejected", "blocked": {"gate": "interaction_invalid"}}
    run(R.marriage_refusal_reads(env, {"interaction": "arrange_marriage_interaction",
                                       "secondary_actor_id": 1, "secondary_recipient_id": 2}, _reader(people)))
    assert "pair_bars" not in env["blocked"] and "faith, kinship" in env["blocked"]["pair_bars_read"]


def test_a_refusal_the_recipient_answered_is_not_rediagnosed():
    env = {"verdict": "rejected", "blocked": {"gate": "recipient_refuses"}}
    run(R.marriage_refusal_reads(env, {"interaction": "arrange_marriage_interaction",
                                       "secondary_actor_id": 1, "secondary_recipient_id": 2}, _reader({})))
    assert env["blocked"] == {"gate": "recipient_refuses"}


# ── the validity stage reaches the reader ────────────────────────────────────────
def test_a_wordless_refusal_names_the_failed_check():
    async def read(action, _p):
        return {"results": [{"intent_id": "si_1", "detail": "", "failed_check": "is_shown"}]}
    words = run(R._drain_detail(read, "si_1", {"status": "rejected"}))
    assert "is_shown" in words and words != R.NO_GAME_DETAIL
    # control: no failed_check -> the old sentence, never an invented stage
    async def read0(action, _p):
        return {"results": [{"intent_id": "si_1", "detail": ""}]}
    assert run(R._drain_detail(read0, "si_1", {})) == R.NO_GAME_DETAIL


def test_the_bridge_names_every_stage_the_server_words():
    """One vocabulary: every stage validity_failed_stage returns has words here."""
    src = (BRIDGE_SRC / "ai_eval_hook.cpp").read_text()
    body = src[src.index("static const char* validity_failed_stage"):]
    body = body[:body.index("\n}\n")]
    import re
    stages = set(re.findall(r'return "([a-z_]+)";', body))
    assert stages == {"unread", "actor_or_recipient_unavailable", "is_shown", "hardcoded",
                      "is_valid_showing_failures_only", "is_valid", "unattributed"}, stages
    assert stages <= set(R.FAILED_CHECK_WORDS)
    assert 'failed_check = "ai_accept"' in src and "ai_accept" in R.FAILED_CHECK_WORDS


# ── the listing: every row put to the game's check with its own pair ─────────────
ME, SON, KING, PRINCESS, COUNTESS, COURT = 100, 101, 200, 201, 300, 400
PEOPLE = {ME: {"id": ME, "name": "Me", "is_female": False, "age": 40, "spouses": [{"id": 9, "alive": True}],
               "spouse_id": 9, "children": [SON]},
          SON: {"id": SON, "name": "Son", "is_female": False, "age": 20, "spouses": [], "father_id": ME},
          PRINCESS: {"id": PRINCESS, "name": "Princess", "is_female": True, "age": 18, "spouses": []},
          COUNTESS: {"id": COUNTESS, "name": "Countess", "is_female": True, "age": 30, "spouses": []},
          KING: {"id": KING, "name": "King", "is_female": False, "age": 50, "spouses": []}}
LISTING = {"actor_id": ME, "sort": "military", "require_in_range": True, "sex": "female", "skipped_republics": 0,
           "include_courtiers": True, "kin_only": True, "skipped_betrothed": 2,
           "candidates": [{"id": COUNTESS, "age": 30, "is_ruler": True, "tier": 2},
                          {"id": PRINCESS, "age": 18, "is_ruler": False, "court_of": KING, "court_tier": 4,
                           "kin_of_court": "child"}]}


def _check(answers):
    def check(params):
        rows = []
        for p in params["pairs"]:
            rows.append(dict({"target_id": p["target_id"], "secondary_actor_id": p["secondary_actor_id"],
                              "secondary_recipient_id": p["secondary_recipient_id"]},
                             **answers[p["secondary_recipient_id"]]))
        return {"success": True, "result": {"results": rows}}
    return check


def test_a_courtier_row_goes_to_her_ruler_and_both_rows_are_checked_with_their_pairs():
    log = []
    out = run(R.marriage_rows(dict(LISTING), _reader(PEOPLE, log, _check({
        PRINCESS: {"sendable": True, "answer": "yes", "accept_score": 40.0},
        COUNTESS: {"sendable": True, "answer": "maybe", "accept_score": 12.0}}))))
    (chk,) = [p for a, p in log if a == "query_interaction_check"]
    assert chk["interaction_type"] == "arrange_marriage_interaction" and chk["actor_id"] == ME
    assert sorted(chk["pairs"], key=lambda p: p["target_id"]) == [
        {"target_id": KING, "secondary_actor_id": SON, "secondary_recipient_id": PRINCESS},
        {"target_id": COUNTESS, "secondary_actor_id": SON, "secondary_recipient_id": COUNTESS}]
    first, second = out["candidates"]
    assert first["id"] == PRINCESS and first["game"] == {"answer": "yes", "accept_score": 40.0}
    assert first["do_call"]["args"]["args"] == {"interaction": "arrange_marriage_interaction", "target_id": KING,
                                                "secondary_actor_id": SON, "secondary_recipient_id": PRINCESS}
    assert first["court_of"] == KING and first["kin_of_court"] == "child"
    assert second["game"]["chance_pct"] == 12.0
    assert out["check"] == R.MARRIAGE_CHECKED and out["not_listed"]["betrothed"] == 2


def test_rows_the_game_refuses_leave_the_list_with_the_games_reason():
    out = run(R.marriage_rows(dict(LISTING), _reader(PEOPLE, None, _check({
        PRINCESS: {"sendable": False, "refused_by": "interaction_not_valid_for_target", "failed_check": "is_shown"},
        COUNTESS: {"sendable": False, "refused_by": "target_would_reject",
                   "engine_reason": "ai_accept -25.00: Will refuse"}}))))
    assert out["candidates"] == []
    whys = {e["id"]: e["why"] for e in out["excluded"]}
    assert "is_shown" in whys[PRINCESS] and "ai_accept -25.00" in whys[COUNTESS]
    assert out["not_listed"]["the game does not allow this match"] == 1
    assert out["not_listed"]["he would refuse"] == 1


def test_CONTROL_a_check_that_does_not_answer_lists_the_rows_unjudged():
    out = run(R.marriage_rows(dict(LISTING), _reader(PEOPLE)))
    assert [r["id"] for r in out["candidates"]] == [COUNTESS, PRINCESS]
    assert out["check"] == R.MARRIAGE_CHECK_UNREAD and "excluded" not in out
    assert all("game" not in r for r in out["candidates"])


def test_a_child_on_your_side_is_offered_a_betrothal():
    people = {**PEOPLE, SON: dict(PEOPLE[SON], age=6)}
    kid = {"id": 500, "age": 5, "is_ruler": False, "court_of": KING, "kin_of_court": "child"}
    people[500] = {"id": 500, "name": "Girl", "is_female": True, "age": 5, "spouses": []}
    out = run(R.marriage_rows(dict(LISTING, candidates=[kid]), _reader(people)))
    (row,) = out["candidates"]
    assert row["tie"] == "betrothal" and row["pair"]["id"] == SON
    assert out["betrothal_side"][0]["id"] == SON and out["your_side"] == []


def test_both_listings_ask_for_kin_at_foreign_courts():
    action, params = tools.resolve_query("marriage_candidates", {"character_id": 5})
    assert action == "query_marriage_candidates" and params["include_courtiers"] == 1 and params["kin_only"] == 1
    src = (HERE / "redesign.py").read_text()
    assert '"include_courtiers": 1, "kin_only": 1' in src.replace("\n", " ").replace("  ", " ") or \
        '"include_courtiers": 1,' in src


# ── the readback reads the alliance ──────────────────────────────────────────────
class _Gw:
    def __init__(self, allied_after):
        self.allied_after, self.after = allied_after, False

    async def read(self, action, params=None):
        if action == "query_character":
            tie = [{"id": 2, "alive": True, "matrilineal": False}] if self.after else []
            return {"id": params["character_id"], "spouses": tie, "betrothed": None}
        if action == "query_alliances":
            ally = [{"ally_id": 7}] if (self.after and self.allied_after) else []
            return {"alliances": ally}
        return {}


@pytest.mark.parametrize("allied", [True, False])
def test_the_readback_reads_the_alliance_and_never_assumes_it(allied):
    gw = _Gw(allied)
    p = {"actor_id": 5, "target_id": 7, "secondary_actor_id": 1, "secondary_recipient_id": 2}
    before = run(R._marriage_before(gw, p))
    gw.after = True
    rb = run(R._marriage_landed(gw, p, before))
    assert rb["tie"] == "married"
    assert rb["alliance"] == {"with": 7, "allied": allied, "before": False, "read": True}


def test_an_own_court_match_reads_no_alliance():
    gw = _Gw(True)
    p = {"actor_id": 5, "target_id": 5, "secondary_actor_id": 1, "secondary_recipient_id": 2}
    before = run(R._marriage_before(gw, p))
    gw.after = True
    rb = run(R._marriage_landed(gw, p, before))
    assert rb["alliance"] == {"with": 5, "read": False, "why": "own court: no alliance"}


def test_a_match_with_a_woman_past_45_is_listed_after_the_others():
    """A live West Francia check (867): the game's best answer for the heir Louis (23) was Aslaug, 65."""
    people = {**PEOPLE, COUNTESS: dict(PEOPLE[COUNTESS], age=65)}
    lst = dict(LISTING, candidates=[dict(LISTING["candidates"][0], age=65), LISTING["candidates"][1]])
    out = run(R.marriage_rows(lst, _reader(people, None, _check({
        PRINCESS: {"sendable": True, "answer": "yes", "accept_score": 40.0},
        COUNTESS: {"sendable": True, "answer": "yes", "accept_score": 94.0}}))))
    first, second = out["candidates"]
    assert first["id"] == PRINCESS and second["id"] == COUNTESS
    assert "fertility x0.1" in second["no_children"] and "no_children" not in first
