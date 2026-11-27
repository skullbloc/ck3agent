"""What slowed the first model-driven seat on the Ireland task, each row replayed
from its recorded reply.

The fixture (fixtures/war_score_replies.json.gz) holds replies recorded on a live
1066 game (toolkit 2026.12.12) as Murchad, Count of Dublin (31506), played for 20 steps
of 73 days by a model-driven seat. Bridge replies the toolkit received are rebuilt from
the tool replies it gave; a value the recording does not hold is written into the test
as such, never into the fixture.
"""
import asyncio
import gzip
import json
from pathlib import Path

import pytest

import news
import redesign as R
import title_table as TT
from test_ireland_walk_replies import Gw, NewsGw, cbs_by_target
from test_war_intents import WarGateway, _active, _war_row

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "war_score_replies.json.gz", "rt").read())
ME, ATHLONE_LORD, WAR = 31506, 29585, 46
C_DUBLIN, C_ATHLONE, C_LEINSTER, C_OSSORY = 463, 469, 508, 513
END_WAR_WE_WIN = {"tool": "do", "args": {"intent": "end_war", "args": {"enemy_id": ATHLONE_LORD,
                                                                        "terms": "we_win"}}}


def run(c):
    return asyncio.run(c)


def size(x) -> int:
    return len(json.dumps(x))


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    news._FABRICATING.clear()
    getattr(R, "_NEWS_WITHIN", {}).clear()
    yield
    news.reset()
    news._FABRICATING.clear()


def war_scores(obj, path=""):
    """Every (path, value) of a key named war_score, at any depth."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "war_score":
                yield path + "." + k, v
            yield from war_scores(v, path + "." + k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from war_scores(v, f"{path}[{i}]")


# ── 1: war_score is the total, the parts beside it, the ready call at we_win ──

#: The breakdown on 1068.09.15, as the recorded inspect(war_score) gave it.
BD_0915 = LIVE["war_score_reply_1068_09_15"]["breakdowns"][0]


def _single(bd, cached=True):
    """query_war_score_breakdown for one war (query_handlers.cpp do_query_war_score_breakdown
    single-war keys), rebuilt from the every-war row."""
    return {"found": True, "war_id": bd["war_id"], "live": True,
            "total_warscore": bd["total"] if cached else bd["battle"] + bd["ticking"],
            "total_warscore_source": "engine_cached" if cached else "live_estimate",
            "cache_status": "cached" if cached else "live_only",
            "battle_score": bd["battle"], "ticking_warscore": bd["ticking"],
            "occupation_score": bd["occupation"] if cached else 0,
            "imprisonment_score": bd["imprisonment"] if cached else 0, "landless_score": 0}


def _gaw(score=None):
    row = {"war_id": WAR, "name": "Dubhlinner Claim on the Earldom of Athlone", "attacker_id": ME,
           "defender_id": ATHLONE_LORD, "is_attacker": True}
    if score is not None:
        row.update(war_score=score, favored_side="attacker")
    return {"wars": [row]}


def _stance_gw(bd_single, gaw):
    rec = LIVE["war_stance_reply_1068_02_08"]["wars"][0]
    return Gw({"get_active_wars": gaw, "query_war_score_breakdown": bd_single,
               "query_wars": {"wars": [{"war_id": WAR, "sieges": rec["sieges"]}]},
               "query_war_stance": {}}, date="1068.09.15")


def test_1_the_recorded_numbers():
    """Live: war_stance read 21 on 1068.02.08, 04.22, 07.04 (get_active_wars sends it only
    when the game's cache is fresh); the game's own end_war check on 04.22 read the band
    0..25; on 1068.09.15 the breakdown read battle 21, occupation 100, imprisonment 25,
    total 100. The 21 was the total then; nothing on the reply said which number it was."""
    for d in ("1068_02_08", "1068_04_22", "1068_07_04"):
        (w,) = LIVE[f"war_stance_reply_{d}"]["wars"]
        assert w["war_score"] == 21 and w["occupation_score"] == 0 and "war_score_parts" not in w
    band = LIVE["end_war_check_reply_1068_04_22"]["reading"]["our_war_score"]
    assert (band["at_least"], band["below"]) == (0, 25)
    assert (BD_0915["battle"], BD_0915["occupation"], BD_0915["imprisonment"], BD_0915["total"]) == (21, 100, 25, 100)


def test_1_war_stance_carries_the_total_its_parts_and_the_ready_call():
    env = run(R.inspect_impl(_stance_gw(_single(BD_0915), _gaw(100)), subject="war_stance", character_id=ME))
    (w,) = env["result"]["wars"]
    assert w["war_score"] == 100
    assert w["war_score_parts"] == {"battle": 21, "occupation": 100, "imprisonment": 25, "ticking": 0}
    assert w["next_call"] == END_WAR_WE_WIN
    assert "the war's total" in env["result"]["note"]


def test_1_no_engine_total_is_null_never_a_part():
    """The cache not fresh: get_active_wars sends no war_score and the breakdown's total is
    the bridge's battle+ticking estimate. The row said war_score 0 (absent read as 0)."""
    env = run(R.inspect_impl(_stance_gw(_single(BD_0915, cached=False), _gaw(None)),
                             subject="war_stance", character_id=ME))
    (w,) = env["result"]["wars"]
    assert w["war_score"] is None and w["war_score_unread"] == R.WAR_SCORE_UNREAD
    assert w["war_score_parts"]["battle"] == 21 and "next_call" not in w


def test_1_query_wars_fallback_is_not_named_war_score():
    """query_wars falls back to battle+ticking under war_score (war_score_source
    computed_fallback) when the total is not cached."""
    raw = {"wars": [{"war_id": WAR, "is_attacker": True, "primary_attacker": ME, "primary_defender": ATHLONE_LORD,
                     "battle_score": 21, "ticking_score": 0, "war_score": 21,
                     "war_score_source": "computed_fallback"}]}
    env = run(R.inspect_impl(Gw({"query_wars": raw}), subject="wars", character_id=ME))
    (w,) = env["result"]["wars"]
    assert w["war_score"] is None and w["battle_and_ticking"] == 21
    raw["wars"][0].update(war_score=100, war_score_source="computed")
    (w,) = run(R.inspect_impl(Gw({"query_wars": raw}), subject="wars", character_id=ME))["result"]["wars"]
    assert w["war_score"] == 100 and w["next_call"] == END_WAR_WE_WIN


def _replies_at_0915():
    """Every reply that shows a war score, made by this code on the 1068.09.15 state."""
    out = {}
    out["war_stance"] = run(R.inspect_impl(_stance_gw(_single(BD_0915), _gaw(100)),
                                           subject="war_stance", character_id=ME))
    out["war_score"] = run(R.inspect_impl(Gw({"query_war_score_breakdown": LIVE["war_score_reply_1068_09_15"],
                                              "get_active_wars": _gaw(100)}),
                                          subject="war_score", character_id=ME))
    gw = NewsGw()
    gw.wars = _gaw(100)

    async def game_reads_we_win(_gw, me, enemy):
        # The news row stands on the game's own reading (end_war's listing); on 1068.09.15 it opened we_win
        # (end_war_check below, the same marks)
        return R.end_war_reading(["LEADS", "OPEN_WE_WIN", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD", "SCORE_GE_100"])
    keep, R.end_war_open_terms = R.end_war_open_terms, game_reads_we_win
    try:
        out["observe"] = run(R.observe_impl(gw, character_id=ME))
        out["observe_again"] = run(R.observe_impl(gw, character_id=ME))      # news: second read
        out["observe_news"] = run(R.observe_impl(gw, character_id=ME, depth="news"))
    finally:
        R.end_war_open_terms = keep
    from test_end_war import gw_with, ran, end
    out["end_war_check"] = end(gw_with(ran("end_war", read_only=True),
                                       marks=["LEADS", "OPEN_WE_WIN", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD",
                                              "SCORE_GE_100"]), enemy_id=ATHLONE_LORD)
    wgw = WarGateway(replies={"query_wars": _active(_war_row(atk=ME, dfn=ATHLONE_LORD, war_id=WAR)),
                              "query_war_score_breakdown": _single(BD_0915),
                              "query_intent_results": {"results": [
                                  {"intent_id": "si_100_1_1", "status": "rejected",
                                   "rejection_reason": "target_would_reject"}]}})
    out["make_peace"] = run(R.do_impl(wgw, intent="make_peace", character_id=ME,
                                      args={"war_id": WAR, "terms": "enforce_demands"}))
    return out


def test_1_walk_every_reply_for_a_war_score_that_is_not_the_total():
    replies = _replies_at_0915()
    seen = []
    for name, env in replies.items():
        for path, v in war_scores(env):
            seen.append(name)
            assert v in (None, 100), (name, path, v)
    assert {"war_stance", "war_score", "observe", "make_peace"} <= set(seen), seen
    # the ready call, in the one shape, where the total reaches we_win
    assert replies["war_stance"]["result"]["wars"][0]["next_call"] == END_WAR_WE_WIN
    assert replies["war_score"]["result"]["breakdowns"][0]["next_call"] == END_WAR_WE_WIN
    assert replies["observe"]["result"]["wars"]["active"][0]["next_call"] == END_WAR_WE_WIN
    assert replies["observe_again"]["result"]["news"]["wars_at_we_win"][0]["next_call"] == END_WAR_WE_WIN
    assert replies["end_war_check"]["result"]["next_call"]["args"]["args"]["terms"] == "we_win"
    mp = replies["make_peace"]
    assert mp["blocked"]["gate"] == "recipient_refuses" and mp["result"]["war_score"] == 100
    assert mp["result"]["next_call"] == END_WAR_WE_WIN


def test_1_CONTROL_below_we_win_no_ready_call():
    bd = dict(BD_0915, total=21, occupation=0, imprisonment=0)
    env = run(R.inspect_impl(_stance_gw(_single(bd), _gaw(21)), subject="war_stance", character_id=ME))
    (w,) = env["result"]["wars"]
    assert w["war_score"] == 21 and "next_call" not in w


# ── 2: the news reaches the seat each turn, without moving observe's ─────────

def test_2_news_depth_does_not_move_the_seats_observe_baseline():
    gw = NewsGw()
    first = run(R.observe_impl(gw, character_id=ME))["result"]["news"]
    assert first["first_observe"] is True
    t0 = run(R.observe_impl(gw, character_id=ME, depth="news"))
    assert t0["ok"] is True and t0["result"]["depth"] == "news" and t0["result"]["news"]["nothing_new"] is True
    gw.date = "1067.11.27"
    gw.claims = {"claims": [{"title_id": C_ATHLONE, "title_name": "Athlone", "type": "unpressed",
                             "holder_id": ATHLONE_LORD}]}
    turn = run(R.observe_impl(gw, character_id=ME, depth="news"))["result"]["news"]
    assert [c["title_id"] for c in turn["claims_gained"]] == [C_ATHLONE]
    again = run(R.observe_impl(gw, character_id=ME, depth="news"))["result"]["news"]
    # No game day passed: the same news again (it was "nothing_new", which left a
    # fresh-turn seat with nothing: the environment had read it first)
    assert [c["title_id"] for c in again["claims_gained"]] == [C_ATHLONE]
    assert again["read_again"]["date"] == "1067.11.27"
    # the seat's own observe still diffs against ITS last observe (1066.09.15)
    own = run(R.observe_impl(gw, character_id=ME))["result"]["news"]
    assert own["since"] == "1066.09.15" and [c["title_id"] for c in own["claims_gained"]] == [C_ATHLONE]


def test_2_news_depth_reads_no_records_and_no_meters():
    gw = NewsGw()
    calls = []

    async def records():
        calls.append(1)
        return []
    gw.delta_records_since_last = records
    env = run(R.observe_impl(gw, character_id=ME, depth="news"))
    assert env["ok"] is True and set(env["result"]) == {"depth", "news"}, env
    assert calls == [] and not [a for a, _ in gw.reads if a in ("query_dejure_progress", "get_ai_personality")]


def test_2_compact_sizes_on_the_recorded_turns():
    """compact() of the three news blocks the seat's observe gave (1066.09.15 first,
    1067.04.22, 1068.09.15)."""
    sizes = {d: (size(n), size(news.compact(n))) for d, n in LIVE["observe_news"].items()}
    assert sizes["1066.09.15"][1] < 60 and news.compact(LIVE["observe_news"]["1066.09.15"])["nothing_new"]
    assert all(c < full for full, c in sizes.values()), sizes
    assert sizes["1068.09.15"][1] < 1100, sizes
    c = news.compact(LIVE["observe_news"]["1068.09.15"])
    assert c["council_changed"][0]["fabrication_left"]["title_id"] == C_LEINSTER


# ── 3: council_tasks names the province, never a wrong title ─────────────────

def test_3_the_recorded_targets_were_province_ids_read_as_titles():
    """Live: the chaplain on c_athlone read "Blue Horde" (34), on c_ossory "Bosporan
    Kingdom" (43), the steward "Red Horde" (28). The script set the target as
    title:<county>.title_province; the game's map definition has 28 Dublin, 34 Athlone,
    43 Gowran (c_ossory's capital barony)."""
    rows = {s["task_name"]: s for s in LIVE["council_tasks_1069_02_08"]["seats"]}
    assert (rows["task_fabricate_claim"]["target_title_id"], rows["task_fabricate_claim"]["target_title_key"]) == \
        (43, "k_bosporan_kingdom")
    assert "title:c_ossory.title_province" in LIVE["council_task_reply_1068_11_27"]["raw"]["script"]
    early = {s["task_name"]: s for s in LIVE["council_tasks_1067_07_04"]["seats"]}
    assert early["task_fabricate_claim"]["target_title_key"] == "e_blue_horde"


def test_3_the_reply_names_the_province_and_no_wrong_title():
    out = R.council_task_rows(LIVE["council_tasks_1069_02_08"], "1069.02.08")
    blob = json.dumps(out)
    for wrong in ("Bosporan", "Blue Horde", "Red Horde", "e_blue_horde", "k_bosporan_kingdom"):
        assert wrong not in blob, wrong
    rows = {s["task_name"]: s for s in out["seats"]}
    assert rows["task_fabricate_claim"]["target_province_id"] == 43
    assert rows["task_fabricate_claim"]["target"] == "unknown"
    assert rows["task_increase_control"]["target_province_id"] == 34
    assert out["target_note"] == R.COUNCIL_TARGET_NOTE


def _fab_gw(seats_reply, date):
    gw = Gw({"query_council_tasks": seats_reply}, date=date)

    async def script_write(_gw, template, params):
        return {"raw": LIVE["council_task_reply_1068_11_27"]["raw"], "guard": "ran", "marks": []}
    return gw


def test_3_4_the_task_this_server_set_is_named_by_its_read_back_province():
    news.note_fabrication(ME, C_OSSORY, "c_ossory", "court_chaplain", "1068.11.27")
    seats = R.council_task_seats(LIVE["council_tasks_1069_02_08"])
    news.note_fabrication_province(ME, next(s for s in seats if s["task_name"] == "task_fabricate_claim")
                                   ["target_province_id"])
    out = R.council_task_rows(LIVE["council_tasks_1069_07_04"], "1069.07.04")
    fab = next(s for s in out["seats"] if s["task_name"] == "task_fabricate_claim")
    assert fab["target"] == {"title_id": C_OSSORY, "title_key": "c_ossory"}
    f = out["fabrication"]
    assert f["running"] is True and f["since"] == "1068.11.27" and f["days"] == 219
    assert f["progress"] is None and f["progress_unread"].startswith("unknown")
    assert "100 / (3 + the chaplain's learning / 5) months" in f["measured"]


# ── 4: claim progress, and the loud case: a fabrication that stopped ──────────

def test_4_a_fabrication_not_set_here_is_running_since_first_seen():
    out = R.council_task_rows(LIVE["council_tasks_1067_07_04"], "1067.07.04")
    f = out["fabrication"]
    assert f["running"] is True and f["target"] == "unknown" and f["target_province_id"] == 34
    assert f["since"] == "1067.07.04" and f["since_is"].startswith("the first date")
    later = R.council_task_rows(LIVE["council_tasks_1067_07_04"], "1067.09.15")["fabrication"]
    assert later["since"] == "1067.07.04" and later["days"] == 73


def test_4_a_fabrication_that_stopped_is_loud_with_the_call_to_set_it_again():
    """Live 1068.09.15: after the war the game's AI moved the chaplain to
    task_religious_relations; the seat lost ~10 months not knowing."""
    news.note_fabrication(ME, C_LEINSTER, "c_leinster", "court_chaplain", "1067.11.27")
    retasked = {"character_id": ME, "seats": [{"councillor_id": 56090, "position_id": 9488,
                                               "task_name": "task_religious_relations"}]}
    f = R.council_task_rows(retasked, "1068.09.15")["fabrication"]
    assert f["running"] is False and f["was"]["title_id"] == C_LEINSTER
    assert f["next_call"] == {"tool": "do", "args": {"intent": "council_task", "args": {
        "seat": "court_chaplain", "task": "task_fabricate_claim", "target_title_id": C_LEINSTER}}}


def test_4_inspect_council_carries_the_fabrication():
    news.note_fabrication(ME, C_LEINSTER, "c_leinster", "court_chaplain", "1067.11.27")
    gw = Gw({"query_council": {"character_id": ME, "council": []},
             "query_council_tasks": {"character_id": ME, "seats": [
                 {"councillor_id": 56090, "task_name": "task_religious_relations"}]}}, date="1068.09.15")
    env = run(R.inspect_impl(gw, subject="council", character_id=ME))
    assert env["result"]["fabrication"]["running"] is False


def test_4_news_turns_say_running_then_stopped():
    gw = NewsGw()
    tasks = {"r": {"character_id": ME, "seats": [{"councillor_id": 56090, "task_name": "task_fabricate_claim",
                                                  "target_title_id": 34, "target_title": "Blue Horde"}]}}
    gw.replies["query_council_tasks"] = lambda p: tasks["r"]
    run(R.observe_impl(gw, character_id=ME, depth="news"))
    news.note_fabrication(ME, C_ATHLONE, "c_athlone", "court_chaplain", "1066.09.15")
    news.note_fabrication_province(ME, 34)
    gw.date = "1066.11.27"
    n = run(R.observe_impl(gw, character_id=ME, depth="news"))["result"]["news"]
    assert n["fabrication"] == {"running": True, "target": {"title_id": C_ATHLONE, "title_key": "c_athlone"},
                                "target_province_id": 34, "since": "1066.09.15", "days": 73, "progress": None,
                                # The claim it makes, and that it dies with the ruler
                                "claim_it_makes": {"type": "unpressed", "on_your_death": news.CLAIM_DIES, "age": 41}}
    gw.date, gw.retasked = "1067.02.08", True
    tasks["r"] = {"character_id": ME, "seats": [{"councillor_id": 56090, "task_name": "task_religious_relations"}]}
    n = run(R.observe_impl(gw, character_id=ME, depth="news"))["result"]["news"]
    assert n["fabrication"]["running"] is False and n["fabrication"]["next_call"]["args"]["intent"] == "council_task"
    (chap,) = n["council_changed"]
    assert chap["fabrication_left"]["claim_held"] is False
    assert chap["next"]["args"]["args"]["target_title_id"] == C_ATHLONE


def test_4_CONTROL_a_fabrication_that_ended_with_the_claim_is_not_stopped():
    gw = NewsGw()
    run(R.observe_impl(gw, character_id=ME, depth="news"))
    news.note_fabrication(ME, C_ATHLONE, "c_athlone", "court_chaplain", "1066.09.15")
    gw.date, gw.retasked = "1067.11.27", True
    gw.claims = {"claims": [{"title_id": C_ATHLONE, "type": "unpressed", "holder_id": ATHLONE_LORD}]}
    gw.replies["query_council_tasks"] = {"character_id": ME, "seats": [{"task_name": "task_religious_relations"}]}
    n = run(R.observe_impl(gw, character_id=ME, depth="news"))["result"]["news"]
    assert "fabrication" not in n
    (chap,) = n["council_changed"]
    assert chap["fabrication_left"]["claim_held"] is True
    assert chap["next"]["args"].get("action_class") == "fabricate_targets"


def test_4_the_council_task_reply_reads_the_province_back_and_names_the_key():
    """Live 1068.09.15 the seat set the chaplain on 508 by id; the news row later read
    title_key null. The key now comes from the script the reply carries."""
    rec = LIVE["council_task_reply_1068_09_15"]
    gw = Gw({"query_council_tasks": {"character_id": ME, "seats": [
        {"task_name": "task_fabricate_claim", "target_title_id": 41}]}}, date="1068.09.15")
    env = {"verdict": "applied", "actor": {"id": ME}, "result": rec}

    async def fake_do(*_a, **_k):
        return json.loads(json.dumps(env))
    orig = R._do_impl
    R._do_impl = fake_do
    try:
        out = run(R.do_impl(gw, intent="council_task", character_id=ME, args={
            "seat": "court_chaplain", "task": "task_fabricate_claim", "target_title_id": C_LEINSTER}))
    finally:
        R._do_impl = orig
    fab = news.fabrication_of(ME)
    assert fab["title_key"] == "c_leinster" and fab["province_id"] == 41
    assert out["result"]["fabrication"]["rule"] == news.FABRICATION_MEASURED


# ── 5: prestige: the cost before the act, the change after ────────────────────

def test_5_the_recorded_drop_had_no_act_of_the_seat_behind_it():
    """600 on 1066.09.15, 405.94 on 1067.04.22: the seat's acts in between were a
    council task and set_phase; the claim war came on 1067.11.27, and its plan_war row
    priced claim_cb at 0 prestige."""
    p = LIVE["purse_by_date"]
    assert (p["1066.09.15"]["prestige"], p["1067.04.22"]["prestige"]) == (600.0, 405.94)
    (t,) = LIVE["plan_war_reply_1067_11_27"]["targets"]
    assert t["affordability"]["cost_prestige"] == 0 and t["affordability"]["estimate"] is True


def _purse_gw(date, prestige):
    gw = NewsGw()
    gw.date = date
    gw.replies["query_character"] = lambda p: {"id": ME, "name": "Murchad", "alive": True, "age": 41,
                                               "liege_id": 0, "top_liege_id": ME, "prestige": prestige,
                                               "piety": 400.0, "gold": 84.0}
    return gw


def test_5_news_says_the_change_and_that_its_cause_is_unknown():
    p = LIVE["purse_by_date"]
    run(R.observe_impl(_purse_gw("1066.09.15", p["1066.09.15"]["prestige"]), character_id=ME, depth="news"))
    n = run(R.observe_impl(_purse_gw("1067.04.22", p["1067.04.22"]["prestige"]), character_id=ME,
                           depth="news"))["result"]["news"]
    pr = n["purse"]["prestige"]
    assert (pr["was"], pr["now"], pr["change"]) == (600.0, 405.94, -194.06)
    assert pr["cause"].startswith("unknown") and "your_acts" not in pr


def test_5_an_act_with_a_known_cost_is_named_beside_the_change():
    run(R.observe_impl(_purse_gw("1067.11.27", 400.0), character_id=ME, depth="news"))
    news.note_act(ME, "1067.11.27", "declared war 46 (de_jure_cb)", cost_known=True, prestige=-100.0)
    n = run(R.observe_impl(_purse_gw("1068.02.08", 300.0), character_id=ME, depth="news"))["result"]["news"]
    pr = n["purse"]["prestige"]
    assert pr["your_acts"] == [{"what": "declared war 46 (de_jure_cb)", "prestige": -100.0}]
    assert "cause" not in pr          # the act accounts for the change


def test_5_declare_war_reply_carries_the_cost_the_cbs_read_showed():
    R.CB_COST_SEEN.clear()
    R._shape_cb_options({"cbs": [{"cb_key": "claim_cb", "valid": True, "cb_title_id": C_ATHLONE,
                                  "target_mode": 6, "cost_prestige": 0, "cost_piety": 0, "scope": "county"}]},
                        target_id=ATHLONE_LORD)
    # claim_cb's cost is the game's own table (at least 75 on a county), not the
    # bridge's 0 (its cb_cost_for says claim wars are free; 99_casus_belli_values.txt says not)
    assert R.CB_COST_SEEN[(ATHLONE_LORD, "claim_cb", C_ATHLONE)] == {"prestige": 75.0, "piety": 0}
    from test_script_actions import Gw as ScriptGw, ran, ME as SME, FOE

    R._shape_cb_options({"cbs": [{"cb_key": "de_jure_cb", "valid": True, "cb_title_id": 1141,
                                  "target_mode": 2, "cost_prestige": 100, "cost_piety": 0}]}, target_id=FOE)

    def lands(st, _p):
        st["wars"] = [{"war_id": WAR, "attacker_id": SME, "defender_id": FOE, "cb_type": "de_jure_cb",
                       "is_attacker": True, "target_titles": [1141], "name": "De jure war"}]
    gw = ScriptGw(script_reply=ran("declare_war"), on_write=lands)
    env = run(R.declare_war_impl(gw, SME, target_id=FOE, cb="de_jure_cb", cb_title_id=1141, confirm_straddle=True))
    assert env["verdict"] == "applied", env
    assert env["result"]["declare_cost"]["prestige"] == 100
    assert env["result"]["declare_cost"]["basis"] == R.DECLARE_COST_BASIS
    (act,) = news._ACTS[SME]
    assert act["what"] == f"declared war {WAR} (de_jure_cb)" and act["prestige"] == -100.0 and act["cost_known"]


# ── 6: inspect title carries the creation requirements, one source ───────────

def _title_inputs():
    """The bridge replies behind inspect(title, k_ireland) on 1068.11.27, rebuilt from the
    recorded table; query_create_title_cost for d_meath from the recorded formable reply;
    for k_ireland the 1068.09.15 lines with this date's counts and
    gold (229.67, the formable reply's) written here."""
    rec = LIVE["title_k_ireland_reply_1068_11_27"]
    counties = rec["counties"]
    prog = {"character_id": ME, "title_key": "k_ireland", "title_id": 461, "held": 3, "total": 14,
            "fraction": 0.21,
            "counties": [{"county_id": c["title_id"], "holder_id": c["holder_id"], "top_liege_id": c["realm_ruler_id"],
                          "you_control": c["you_control"]} for c in counties]}
    top = {"title": {"title_id": 461, "key": "k_ireland", "name": "Ireland", "tier": 4, "tier_name": "kingdom"},
           "de_jure_children": [{"title_id": d["title_id"], "key": d["key"], "name": d["name"], "tier": 3,
                                 "holder_id": d["holder_id"], "holder_name": d["holder_name"]}
                                for d in rec["duchies"]]}
    kids = {d["title_id"]: {"de_jure_children": [{"title_id": c["title_id"], "key": c["key"], "name": c["name"],
                                                  "tier": 2, "holder_name": c["holder_name"]}
                                                 for c in counties if c["duchy"] == d["key"]]}
            for d in rec["duchies"]}
    meath = next(r for r in LIVE["formable_reply_1068_11_27"]["available"]
                 if r["do_call"]["args"]["title_key"] == "d_meath")
    cost = {"k_ireland": {"success": True, "engine_valid": False, "engine_reasons": [
                "", "Duchy Titles Required: 2 (currently: 0)", "Kingdom Titles Required: 1 (currently: 0)",
                "Empire Titles Required: 1 (currently: 0)", "Hegemony Titles Required: 1 (currently: 0)",
                "You only have 3 of 8 required Counties", "Earl Murchad does not have gold 425"],
                          "price": {"known": True, "currency": "gold", "cost": 425.0, "have": 229.67,
                                    "affordable": False}},
            "d_meath": {"success": True, "engine_valid": True, "engine_reasons": [],
                        "price": {"known": True, "currency": "gold", "cost": meath["price"]["cost"],
                                  "have": meath["price"]["gold"], "affordable": True}}}
    # The second duchy the tier rule needs is priced too; the recording holds no
    # price for d_leinster on this date: d_meath's is written here for it
    cost["d_leinster"] = dict(cost["d_meath"], engine_valid=False)
    realms = {"realms": [{"character_id": r["ruler_id"]} for r in rec["realms"]]}
    replies = {"query_dejure_progress": prog, "query_claims": {"claims": []}, "query_realm": realms,
               "query_de_jure": lambda p: top if p.get("title_key") == "k_ireland" else kids[p["title_id"]],
               "query_create_title_cost": lambda p: cost[p["title_key"]]}
    replies.update(cbs_by_target({"cbs": []}))
    return replies


def test_6_inspect_title_carries_the_creation_summary_from_the_formable_function():
    gw = Gw(_title_inputs(), date="1068.11.27")
    env = run(R.inspect_impl(gw, subject="title", title_key="k_ireland", character_id=ME))
    c = env["result"]["creation"]
    assert c["counties"] == {"have": 3, "need": 8, "met": False}     # More than half of 14 (the game's 8)
    assert c["tier_rule"]["met"] is False and c["tier_rule"]["rule"].startswith("become a duke holding 2 duchy")
    # Was {"cost": None, "unread": "... 1 priced ..."}; both duchies are priced now
    assert c["tier_rule"]["bill"] == {"currency": "gold", "cost": 850.0, "of": ["d_meath", "d_leinster", "k_ireland"]}
    # A kingdom short of gold also carries its months of income (and the whole bill's)
    assert {k: v for k, v in c["price"].items() if k in ("met", "cost", "currency", "have")} == {
        "met": False, "cost": 425.0, "currency": "gold", "have": 229.67}
    assert set(c["price"]) - {"met", "cost", "currency", "have"} <= {"months_of_income", "income_basis",
                                                                    "bill_months_of_income"}
    n = c["nearest_unmet"]
    assert (n["requirement"], n["step"], n["counties"], n["cost"], n["you_have"], n["gate"]) == \
        ("tier_rule", "create d_meath", "2/2 held", 212.5, 229.67, None)
    assert n["next_call"] == {"tool": "do", "args": {"intent": "mint_title", "args": {"title_key": "d_meath"}}}
    # one source: formable_titles gives the same kingdom requirements
    fgw = Gw(_title_inputs(), date="1068.11.27")
    fenv = run(R.do_impl(fgw, action_class="formable_titles", character_id=ME,
                         candidates=["d_meath", "d_leinster", "k_ireland"]))
    (k,) = [r for r in fenv["result"]["unavailable"] if r["title_key"] == "k_ireland"]
    tr = next(q for q in k["requirements"] if q["requirement"] == "tier_rule")
    assert tr["rule"] == c["tier_rule"]["rule"] and tr["bill"] == c["tier_rule"]["bill"]
    # the reads it added: a create-title check for the title and each duchy priced
    assert [a for a, _ in gw.reads].count("query_create_title_cost") == 3
    assert size(env) < 9300 + 1200, size(env)


def test_6_CONTROL_no_duchy_creatable_names_the_county_bar():
    inputs = _title_inputs()
    prog = inputs["query_dejure_progress"]
    # A duchy is creatable at half its counties (the game's rule, not the bridge's 51%): with Athlone gone
    # d_meath (1 of 2) and d_leinster (1 of 2) would still be, so the realm here holds none
    for c in prog["counties"]:
        c["you_control"] = False
    prog["held"] = 0
    env = run(R.inspect_impl(Gw(inputs, date="1068.11.27"), subject="title", title_key="k_ireland",
                             character_id=ME))
    n = env["result"]["creation"]["nearest_unmet"]
    assert n["requirement"] == "de_jure_counties"


# ── the surface ───────────────────────────────────────────────────────────────

def test_surface_thirteen_tools_news_is_an_observe_depth_and_the_dump_does_not_grow():
    ms = pytest.importorskip("mcp_server")
    tools = run(ms.mcp.list_tools())
    out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in tools]
    out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]}
           for d in out]
    assert len(out) == 13
    obs = next(d for d in out if d["name"] == "observe")
    assert obs["inputSchema"]["properties"]["depth"]["enum"] == ["brief", "full", "news"]
    # +268 for the shared notes' pointer in discover (test_ireland_walk_replies has it line by line)
    # +396 compact (+408 at indent=1) for do's two intents event_rule and gold_reserve: their
    # enum entries (+32), their rows in the generated args table (+165) and one description sentence (+199)
    assert len(json.dumps(out)) <= 31_052, len(json.dumps(out))      # the recorded reply's size


def test_fixture_is_shippable():
    s = json.dumps(LIVE)
    assert LIVE["_source"].startswith("recorded replies from a live 1066 game")
    for bad in ("/home", "/mnt", "pilot", "lane"):
        assert bad not in s, bad
