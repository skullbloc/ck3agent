"""The fixes from a recorded game (one seated game of the Munster seat paid once, k_ireland never held).

  1  the partition forecast was one news row on page 2; the seat's next inspect title call dropped the page unread
     -> the goal row's at_death (the goal line every turn) and reply pages kept per call
  1b a harness's own paged reads on the seat's server reset the seat's kept pages          -> `_meta` marks them
  2  an always-pay claim-letter rule paid 108 at 38.19 gold                                  -> floored at the price
  3  titles gained "how not read" (d_ulster, c_ulster, c_oriel 1090.09.07)                   -> the title history
  4  `0302 decline` refused needs_two_options                                                -> its free option

The values are the run's own where a test says so (the game's own records); the game's files are read
where a test says so (skipped when they are not on this host). No game.
"""
from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

import news as N  # noqa: E402
import redesign as R  # noqa: E402
import reply_fit as F  # noqa: E402
import seat_events as SE  # noqa: E402
from test_ireland_walk_replies import Gw  # noqa: E402

FILES = SE.game_dir() is not None
need_files = pytest.mark.skipif(not FILES, reason="the game's files are not on this host")
MURCHAD, BRIAN, DONNCHAD = 31762, 36303, 66915
L0301, L0302 = "court_chaplain_task.0301", "court_chaplain_task.0302"


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _clean(monkeypatch):
    monkeypatch.delenv(F.PAGES_ENV, raising=False)
    monkeypatch.delenv("CK3_GOAL_KIND", raising=False)
    monkeypatch.setenv("CK3_GOAL_TITLE", "k_ireland")
    _reset()
    yield
    _reset()


def _reset():
    # tolerant of a tree without these rules (its RED tests are run there): one kept store, no floors
    if hasattr(F, "reset_pages"):
        F.reset_pages()
    else:
        F._STORE.update(tool=None, pages=[], next=0)
    getattr(SE, "_FLOORED", {}).clear()


# ── 1: reply pages kept per call ───────────────────────────────────────────────────────────────────────────────

def _env(tool, date, rows, extra=None):
    return {"ok": True, "tool": tool, "game": {"date": date}, "result": dict(extra or {}, rows=rows)}


def _news_reply(date="1076.05.20"):
    """The step-29 news as it was laid out: events_answered rows 7-18 and heir_split_ahead on page 2."""
    ev = [{"event": f"travel_danger_events.{i}", "applied": "x" * 400} for i in range(18)]
    split = {"takers": [BRIAN, DONNCHAD], "leaves_realm": [DONNCHAD], "age": 49, "health": 3.9}
    return {"ok": True, "tool": "observe", "game": {"date": date},
            "result": {"news": {"events_answered": ev, "heir_split_ahead": split}}}


def _title_reply(date="1076.05.20"):
    return {"ok": True, "tool": "inspect", "game": {"date": date},
            "result": {"summary": {"held": 9}, "counties": [{"key": f"c_{i}", "pad": "y" * 500} for i in range(30)]}}


def _all_more():
    out = []
    while True:
        p = F.next_page()
        if p.get("ok") is False:
            return out
        out.append(p)


def test_1_RED_the_turns_news_pages_outlive_the_seats_next_inspect_title_call():
    """Turn 30 (1076.05.20): the opening news named `news.heir_split_ahead: whole` on page 2; the seat's fourth call,
    inspect title k_ireland, dropped those pages before any `more`. Now both replies' pages are kept: the newest
    first, then the news's, the heir_split_ahead row among them."""
    first = F.paginate(_news_reply(), "observe", {"depth": "news"}, cap=F.CAP)
    # The paged news lays the goal's rows first (reply_fit.NEWS_FIRST), so heir_split_ahead is on page one now;
    # the later pages kept are the events_answered rows that did not fit
    assert "heir_split_ahead" in first["result"]["news"]
    assert any("events_answered" in w for ws in first["result"]["more"]["on_later_pages"].values() for w in ws)
    F.paginate(_title_reply(), "inspect:title", {"subject": "title"}, cap=F.CAP)
    pages = _all_more()
    assert pages[0]["result"]["more"]["of"] == "inspect:title"
    assert any("travel_danger_events" in json.dumps(p["result"]) for p in pages), [p["result"]["more"] for p in pages]
    assert pages[-1]["result"]["more"]["of"] == "observe"


def test_1_the_newer_reply_names_the_pages_kept_beside_it():
    F.paginate(_news_reply(), "observe", {"depth": "news"}, cap=F.CAP)
    t = F.paginate(_title_reply(), "inspect:title", {"subject": "title"}, cap=F.CAP)
    assert "observe:news" in t["result"]["more"]["also_kept"]
    last_title = [p for p in _all_more() if p["result"]["more"]["of"] == "inspect:title"][-1]
    assert "observe:news" in last_title["result"]["more"]["then"] and "next" in last_title["result"]["more"]


def test_1_a_whole_reply_of_another_call_keeps_the_pages_and_the_same_call_replaces_them_saying_so():
    F.paginate(_news_reply(), "observe", {"depth": "news"}, cap=F.CAP)
    small = {"ok": True, "tool": "inspect", "game": {"date": "1076.05.20"}, "result": {"summary": {"held": 9}}}
    assert F.paginate(small, "inspect:title", {"subject": "title"}, cap=F.CAP) is small
    n = F.unread_pages()["observe:news"]
    assert n >= 1
    again = F.paginate({"ok": True, "tool": "observe", "game": {"date": "1076.05.20"}, "result": {"news": {}}},
                       "observe", {"depth": "news"}, cap=F.CAP)
    assert again["result"]["pages_dropped"] == {"call": "observe:news", "unread_pages": n,
                                                "why": "this reply of the same call is newer"}
    assert F.unread_pages() == {}


def test_1_the_seats_observe_and_the_turns_news_are_two_calls():
    F.paginate(_news_reply(), "observe", {"depth": "news"}, cap=F.CAP)
    F.paginate(_news_reply(), "observe", {}, cap=F.CAP)
    assert set(F.unread_pages()) == {"observe:news", "observe"}


def test_1_pages_of_a_game_date_gone_by_are_dropped():
    F.paginate(_news_reply("1076.05.20"), "observe", {"depth": "news"}, cap=F.CAP)
    F.paginate(_title_reply("1077.05.20"), "inspect:title", {"subject": "title"}, cap=F.CAP)
    assert set(F.unread_pages()) == {"inspect:title"}


def test_1b_RED_a_bench_read_leaves_the_seats_pages():
    """a recorded game's suspicion, proven from the code: a harness's step-end metric (inspect title, paged) and the outcome
    witness (cbs, paged, during the seat's turn) ran through the same paginate as the seat's calls and reset its
    pages. Marked as a harness's own (store=False), it keeps them and keeps nothing of its own."""
    F.paginate(_news_reply(), "observe", {"depth": "news"}, cap=F.CAP)
    before = F.unread_pages()
    first = F.delivered(_title_reply(), "inspect", {"subject": "title"}, store=False)
    assert first["result"]["summary"] == {"held": 9} and "more" in first["result"]
    assert F.unread_pages() == before == {"observe:news": before["observe:news"]}
    F.delivered(_title_reply(), "inspect", {"subject": "title"})            # the CONTROL: a seat read keeps its own
    assert set(F.unread_pages()) == {"observe:news", "inspect:title"}


# ── 1: the goal row says what the death takes; the goal line says it every turn ─────────────────────────────────

D_ULSTER = {"title_id": 473, "key": "d_ulster", "tier": 3}


def _munster_gw(age=47, health=4.02, ulster_held=3, laws=("confederate_partition_succession_law",
                                                           "male_preference_law")):
    """1074.09.23 as recorded: Murchad 31762 (age 47, health 4.02: inspect character turn 27), Duke of Munster (d_munster
    518, k_ireland 450); sons Brian 36303 (primary heir) and Donnchad 66915; d_ulster 3/3 de jure counties in the realm
    and not held (formable_titles turn 27: d_ulster 3/3); confederate partition (the series' realm law)."""
    people = {MURCHAD: {"id": MURCHAD, "name": "Murchad", "alive": True, "age": age, "health": health,
                        "ruler_tier": 3, "children": [BRIAN, DONNCHAD], "dynasty_id": 77, "gold": 46.51,
                        "prestige": 400.0, "piety": 90.0, "faith_id": 5, "liege_id": 0, "top_liege_id": MURCHAD},
              BRIAN: {"id": BRIAN, "name": "Brian", "alive": True, "is_female": False, "age": 20, "traits": [],
                      "faith_id": 5, "dynasty_id": 77},
              DONNCHAD: {"id": DONNCHAD, "name": "Donnchad", "alive": True, "is_female": False, "age": 14,
                         "traits": [], "faith_id": 5, "dynasty_id": 77}}

    def hier(p):
        if p.get("title_id") == 518:
            return {"title": {"key": "d_munster", "tier": 3, "de_jure_parent_id": 450}}
        if p.get("title_id") == 450:
            return {"title": {"key": "k_ireland", "tier": 4, "sub_titles": [
                {"title_id": 518, "key": "d_munster", "tier": 3, "holder_id": MURCHAD},
                {"title_id": 473, "key": "d_ulster", "tier": 3, "holder_id": 0},
                {"title_id": 490, "key": "d_meath", "tier": 3, "holder_id": 0}]}}
        return {}

    def dejure(p):
        if p.get("title_key") == "k_ireland":
            return {"de_jure_children": [{"key": k, "tier": 3, "title_id": i} for k, i in
                                         (("d_munster", 518), ("d_ulster", 473), ("d_meath", 490),
                                          ("d_connacht", 495), ("d_leinster", 505))]}
        if p.get("title_key") == "d_ulster":
            return {"de_jure_children": [{"key": k, "tier": 2} for k in ("c_ulster", "c_oriel", "c_ailech")]}
        return {}
    sim = {"primary_title_id": 518, "heirs": [
        {"character_id": BRIAN, "titles": [{"title_id": 518, "tier": 3, "key": "d_munster"},
                                           {"title_id": 519, "tier": 2, "key": "c_thomond"},
                                           {"title_id": 531, "tier": 2, "key": "c_desmond"}]}]}
    return Gw({"query_character": lambda p: people.get(p.get("character_id"), {}),
               "query_realm_laws": {"active_laws": [{"name": n} for n in laws]},
               "query_succession_simulation": sim,
               "query_succession": {"primary_heir_id": BRIAN},
               "query_title_succession_map": {},
               "query_title_hierarchy": hier,
               "query_dejure_progress": lambda p: ({"held": ulster_held, "total": 3} if p.get("title_id") == 473
                                                   else {"held": 0, "total": 3}),
               "query_de_jure": dejure,
               "query_dynasty": {"head_character_id": 999, "renown": 50}})


def _goal_row():
    return {"title_key": "k_ireland",
            "price_basis": {"at_succession": {"heir_id": BRIAN, "price": 425.0, "rises": 100.0,
                                              "why": "your own ducal_conquest_perk discount dies with you; create "
                                                     "k_ireland before your death to pay 325.0"}}}


def test_1_the_goal_row_names_the_chain_duchy_the_law_creates_for_a_son_who_leaves():
    ad = run(R.goal_death_forecast(_munster_gw(), MURCHAD, _goal_row()))
    (leave,) = ad["leaves_realm"]
    assert leave == {"to": DONNCHAD, "name": "Donnchad", "takes": "d_ulster", "created_by_the_law": True,
                     "counties": {"in_your_realm": 3, "of": 3}, "county_keys": ["c_ulster", "c_oriel", "c_ailech"]}
    assert ad["price_after"]["price"] == 425.0 and ad["ruler"]["age"] == 47 and ad["ruler"]["act_now"] is False
    levers = {x["lever"] for x in ad["levers"]}
    assert {"disinherit", "vows"} <= levers
    dis = next(x for x in ad["levers"] if x["lever"] == "disinherit")
    assert dis["allowed_now"] is False and "dynasty's head" in dis["why_not"][0]
    assert dis["call"]["args"]["args"] == {"interaction": "disinherit_interaction", "target_id": DONNCHAD}


def test_1_nothing_when_no_chain_title_leaves_and_the_price_holds():
    row = {"title_key": "k_ireland"}
    assert run(R.goal_death_forecast(_munster_gw(ulster_held=1), MURCHAD, row)) is None


def test_1_a_share_outside_the_goal_land_is_not_named_and_an_unread_goal_land_lists_all():
    gw = _munster_gw()
    dj = gw.replies["query_de_jure"]
    gw.replies["query_de_jure"] = lambda p: ({"de_jure_children": [{"key": "d_munster", "tier": 3}]}
                                             if p.get("title_key") == "k_ireland" else dj(p))
    ad = run(R.goal_death_forecast(gw, MURCHAD, _goal_row()))
    assert "leaves_realm" not in ad and ad["price_after"]["price"] == 425.0     # d_ulster not of this goal
    gw.replies["query_de_jure"] = lambda p: {} if p.get("title_key") == "k_ireland" else dj(p)
    ad = run(R.goal_death_forecast(gw, MURCHAD, _goal_row()))
    assert ad["leaves_realm"][0]["takes"] == "d_ulster" and "did not answer" in ad["goal_land_unread"]


def test_1_only_the_goal_title_and_never_under_a_peaceful_goal(monkeypatch):
    assert run(R.goal_death_forecast(_munster_gw(), MURCHAD, dict(_goal_row(), title_key="d_ulster"))) is None
    monkeypatch.setenv("CK3_GOAL_KIND", "line_kept_peacefully")
    assert run(R.goal_death_forecast(_munster_gw(), MURCHAD, _goal_row())) is None
    monkeypatch.delenv("CK3_GOAL_TITLE")
    monkeypatch.delenv("CK3_GOAL_KIND")
    assert run(R.goal_death_forecast(_munster_gw(), MURCHAD, _goal_row())) is None


#: the step-28 goal line as the seat read it (1075.05.20, the observation's world.goal)
LINE_28 = ("k_ireland needs 1 more duchy title first: d_ulster waits only for gold (it costs 162.5, you have 48.12). "
           "Then k_ireland waits only for gold (325): 439.38 gold still needed in all (487.5); 99 months at the last "
           "step's net +4.46 gold a month (one-off -10.09 gold left out) (1429 men raised cost 4.29 a month; stood "
           "down, 51 months at +8.75). At your death the price rises to 425 (your own ducal_conquest_perk discount "
           "dies with you).")


def test_1_the_goal_rows_reply_over_the_cap_keeps_the_goal_row_whole_and_first():
    """a recorded game's other suspicion: a harness's second goal read (the target and its chain duchies) over the cap. The
    reply is fit (not paged); the goal row is ranked first and only rows after it are cut."""
    duchy = {"title_key": "d_meath", "blocked": {"gate": "cannot_afford"}, "pad": "z" * 2200}
    goal = dict(_goal_row(), blocked={"gate": "cannot_afford"}, requirements=[{"requirement": "tier_rule",
                "met": False, "chain": [{"title_key": "d_ulster", "state": "gold_only", "cost": 162.5}]}],
                at_death={"leaves_realm": [{"to": DONNCHAD, "takes": "d_ulster"}]}, pad="g" * 2500)
    rows = [dict(duchy, title_key=k) for k in ("d_meath", "d_ulster")] + [goal] + \
        [dict(duchy, title_key=k) for k in ("d_connacht", "d_leinster")]
    env = {"ok": True, "tool": "do", "result": {"action_class": "formable_titles", "available": [],
                                                "unavailable": rows}}
    out = F.delivered(json.loads(json.dumps(env)), "do", {"action_class": "formable_titles",
                                                          "candidates": ["k_ireland"]})
    assert F.size(out) <= F.CAP - F.wrapper_bytes("do")
    assert out["result"]["unavailable"][0] == goal
    assert out["result"]["truncated"]["lists"][0]["list"] == "unavailable"


# ── 2: an always-pay letter rule never pays into debt by default ─────────────────────────────────────────────────

def _letter_gw(gold, learning):
    """The chaplain's learning, the purse, and an event-rule table that holds what was written (the readback)."""
    rules: list = []

    def table(_p):
        return {"success": True, "seats": [{"character_id": MURCHAD, "rules": list(rules)}], "picks": []}
    gw = Gw({"query_council": {"council": [{"position_key": "councillor_court_chaplain", "holder_id": 9,
                                              "skills": {"learning": learning}}]},
               "query_character": {"id": MURCHAD, "gold": gold, "income_breakdown": {"total_monthly": 2.6}},
               "query_event_rules": table,
               "query_armies": {"armies": []}})
    write = gw.write

    async def recording(action, params=None):
        if action == "set_event_rule":
            rules[:] = [r for r in rules if r.get("event") != params.get("event")] + [
                {k: v for k, v in params.items() if k != "character_id"}]
        return await write(action, params)
    gw.write = recording
    return gw


@need_files
def test_2_RED_the_recorded_always_pay_rule_is_set_floored_at_the_letters_price():
    """Turn 47 1086.05: event_rule set 0301 answer=option option=0301.a at 221.56 gold (letter 108: learning 4); it paid
    108 at 38.19 on 1090.11.06. Now it is set as pay_if gold>=108 with the free option as else, on both variants."""
    gw = _letter_gw(221.56, 4)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": L0301, "answer": "option",
                                    "option": L0301 + ".a"}, MURCHAD, {"id": MURCHAD}))
    assert env["verdict"] == "applied", env
    sent = [p for w, p in gw.writes if w == "set_event_rule"]
    assert sent[0]["answer"] == "pay_if" and sent[0]["amount"] == 108 and sent[0]["else"] == L0301 + ".b"
    assert sent[1]["event"] == L0302 and sent[1]["answer"] == "pay_if" and sent[1]["amount"] == 108
    fl = env["result"]["event_check"]["always_pay_floor"]
    assert fl["set_as"] == f"pay_if gold>=108 else={L0301}.b" and "args.debt=true" in fl["why"]
    # at fire time the bridge declines (38.19 < 108), and the news says why
    row = SE.pick_row({"character_id": MURCHAD, "event": L0301, "rule": f"pay_if gold>=108 option={L0301}.a "
                       f"else={L0301}.b", "covered": False, "gold": 38.19, "applied": L0301 + ".b",
                       "answered_by": "event_rule_else", "wanted": L0301 + ".a", "why": "else"})
    assert "did not pay: the gold (38.19) was below the letter's 108" in row["payment_refused"]


@need_files
def test_2_debt_true_sets_the_always_pay_rule_as_asked():
    gw = _letter_gw(221.56, 4)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": L0301, "answer": "option", "option": L0301 + ".a",
                                    "debt": True}, MURCHAD, {"id": MURCHAD}))
    sent = [p for w, p in gw.writes if w == "set_event_rule"]
    assert sent[0]["answer"] == "option" and "amount" not in sent[0], sent
    assert env["result"]["event_check"]["debt"].startswith("set: this rule pays at any gold")
    assert SE._FLOORED == {}


@need_files
def test_2_a_decline_rule_is_not_floored_and_clears_the_floor():
    gw = _letter_gw(221.56, 4)
    run(SE.do_event_rule(gw, {"op": "set", "event": L0301, "answer": "option", "option": L0301 + ".a"},
                         MURCHAD, {"id": MURCHAD}))
    assert SE._FLOORED[(MURCHAD, L0302)] == 108
    run(SE.do_event_rule(gw, {"op": "set", "event": L0301, "answer": "option", "option": L0301 + ".b"},
                         MURCHAD, {"id": MURCHAD}))
    assert SE._FLOORED == {}


# ── 4: decline on the three-option letter ─────────────────────────────────────────────────────────────────────────

@need_files
def test_4_RED_decline_on_0302_takes_its_free_option():
    """Turn 65: `0302 decline` refused needs_two_options right after `0301 decline` had set 0302 on 0301.b."""
    gw = _letter_gw(9.0, 4)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": L0302, "answer": "decline"}, MURCHAD, {"id": MURCHAD}))
    assert env["verdict"] == "applied", env
    sent = [p for w, p in gw.writes if w == "set_event_rule"]
    assert sent[0] == {"character_id": MURCHAD, "op": "set", "event": L0302, "answer": "option",
                       "option": L0301 + ".b"}
    assert env["result"]["event_check"]["decline_as"]["option"] == L0301 + ".b"
    assert sent[1]["event"] == L0301 and sent[1]["option"] == L0301 + ".b"


# ── 3: titles gained with no record in the step: the game's title history ────────────────────────────────────────

def test_3_RED_a_title_gained_with_no_record_reads_the_title_history():
    """step 57 1090.09.07: d_ulster (473), c_ulster (474) and their baronies came to Brian 36303 'how unknown'."""
    import test_ai_veto as TV
    N.reset()
    hist = {473: [{"date": "1090.08.30", "event_type_raw": 4, "char_id": TV.ME},
                  {"date": "1079.12.16", "event_type_raw": 7, "char_id": DONNCHAD}],
            474: [{"date": "1090.08.30", "event_type_raw": 4, "char_id": TV.ME},
                  {"date": "1079.12.16", "event_type_raw": 4, "char_id": DONNCHAD}]}
    gw = TV.Gw({"query_title_history": lambda p: {"entries": hist.get(p.get("title_id"), [])}})
    s1 = TV.snap("1090.05.20", [], held={"held_titles": [{"title_id": 463, "key": "c_dublin"}]})
    s2 = TV.snap("1090.09.07", [], held={"held_titles": [{"title_id": 463, "key": "c_dublin"},
                                                         {"title_id": 473, "key": "d_ulster"},
                                                         {"title_id": 474, "key": "c_ulster"}]})
    run(N.block(gw, 0, s1, offers_served=False))
    out = run(N.block(gw, 0, s2, offers_served=False))
    by = {g["key"]: g for g in out["titles_gained"]}
    assert by["d_ulster"]["how"] == f"inheritance: to you on 1090.08.30, from {DONNCHAD} (the game's title history)"
    assert by["d_ulster"]["from"] == DONNCHAD and by["c_ulster"]["history_type"] == 4
    N.reset()


def test_3_CONTROL_a_history_that_does_not_name_the_ruler_leaves_the_reason():
    import test_ai_veto as TV
    N.reset()
    gw = TV.Gw({"query_title_history": {"entries": [{"date": "1090.08.30", "event_type_raw": 4, "char_id": 5}]}})
    s1 = TV.snap("1090.05.20", [], held={"held_titles": [{"title_id": 463, "key": "c_dublin"}]})
    s2 = TV.snap("1090.09.07", [], held={"held_titles": [{"title_id": 463, "key": "c_dublin"},
                                                         {"title_id": 473, "key": "d_ulster"}]})
    run(N.block(gw, 0, s1, offers_served=False))
    (g,) = run(N.block(gw, 0, s2, offers_served=False))["titles_gained"]
    assert g["how"] == "unknown" and g["how_reason"] == "no_record_in_step"
    N.reset()


def test_1_formable_titles_puts_at_death_on_the_goal_row_only(monkeypatch):
    import test_mint_price as MP
    seen = []

    async def fake(gw, me, row):
        seen.append((me, row.get("title_key")))
        return {"leaves_realm": [{"to": DONNCHAD, "takes": "d_ulster"}]} if row.get("title_key") == "k_ireland" \
            else None
    monkeypatch.setattr(R, "goal_death_forecast", fake)
    env = MP._formable(MP.Gateway(MP.price_reply()))
    rows = env["result"]["available"] + env["result"]["unavailable"]
    assert rows[0]["at_death"]["leaves_realm"][0]["takes"] == "d_ulster" and seen == [(MURCHAD, "k_ireland")]


def test_1_a_forecast_that_raises_is_said_unread_on_the_goal_row(monkeypatch):
    import test_mint_price as MP

    async def boom(gw, me, row):
        raise KeyError("x")
    monkeypatch.setattr(R, "goal_death_forecast", boom)
    env = MP._formable(MP.Gateway(MP.price_reply()))
    rows = env["result"]["available"] + env["result"]["unavailable"]
    assert rows[0]["at_death"] == {"unread": "the death forecast raised KeyError"}


def test_1_the_real_forecast_runs_on_a_gateway_that_answers_little():
    import test_mint_price as MP
    env = MP._formable(MP.Gateway(MP.price_reply()))
    rows = env["result"]["available"] + env["result"]["unavailable"]
    assert rows and "at_death" not in rows[0]          # nothing read leaves the realm, the price does not rise
