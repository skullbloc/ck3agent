"""What the fifth seated Ireland run exposed,
each row replayed from its recorded reply.

The fixture (fixtures/county_task_replies.json.gz, built by a
research-tree script) holds whole tool replies and step news recorded in that run and its
pre-flight; `line` is the record's line in the evidence's calls file. A value the recording
does not hold (a recruit price, a stack size) is the game's file, read from the install when one
is present (CK3_GAME_DIR, or the default Steam path) and skipped otherwise.
"""
import asyncio
import gzip
import json
import os
import re
from pathlib import Path

import pytest

import news
import redesign as R
import tool_prefix as TP
from test_ireland_walk_replies import Gw, _title_gw, C_ATHLONE, C_LEINSTER, C_ULSTER

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "county_task_replies.json.gz", "rt").read())
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
ME = 31506
C_OSSORY, C_ORIEL = 513, 479


def run(c):
    return asyncio.run(c)


def env_of(name):
    r = LIVE[name]["reply"]
    return r["result"] if isinstance(r, dict) and "tool_name" in r else r


def news_of(name):
    return LIVE[name]["news"]


def game_file(rel: str) -> str:
    p = GAME / rel
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    return p.read_text(encoding="utf-8-sig", errors="replace")


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    news._FABRICATING.clear()
    news._FAB_CHAPLAIN.clear()
    getattr(R, "_MERC_LAST_ROWS", {}).clear()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    monkeypatch.delenv(TP.ENV, raising=False)
    yield
    news.reset()


FATHER = 27052


def kin_chars(p):
    """query_character: Murchad's father is Leinster's holder (27052); others unrelated."""
    cid = p.get("character_id")
    if cid == ME:
        return {"id": ME, "name": "Murchad", "father_id": FATHER, "mother_id": 70, "children": [36124]}
    return {"id": cid, "father_id": 9000 + (cid or 0), "mother_id": 9100 + (cid or 0), "children": []}


# ── D1: a fabricate target the game's own check refuses is not offered or applied ──────

def test_d1_the_recorded_listing_offered_leinster_and_the_game_reset_it_monthly():
    res = env_of("fabricate_targets_1070_11_28")["result"]
    (lein,) = [r for r in res["available"] if r["title_id"] == C_LEINSTER]
    assert lein["blocked"] is None and lein["holder_id"] == 27052
    resets = LIVE["leinster_resets"]
    # 36 re-sets of the same call read back (the run's 39 council_task calls less the first set,
    # one sent by a bare name, and the move to c_ossory)
    assert len(resets) == 36, len(resets)
    # each re-set found the chaplain on the seat's default task, not the fabrication
    assert {r["previous_task"] for r in resets} == {"task_religious_relations"}
    assert env_of("council_task_1070_11_28")["result"]["raw"]["ai_pin"] == "pinned"


def test_d1_the_listing_blocks_a_county_the_rulers_close_family_holds():
    gw = _title_gw()
    gw.replies["query_character"] = kin_chars
    env = run(R.do_impl(gw, action_class="fabricate_targets", character_id=ME, args={"within": "k_ireland"}))
    res = env["result"]
    (lein,) = [r for r in res["unavailable"] if r["title_id"] == C_LEINSTER]
    assert lein["blocked"]["gate"] == "target_ineligible" and "do_call" not in lein
    # Live: the rule the game holds (a living parent's county; its verdict unread in this stub)
    assert "your parent" in lein["blocked"]["summary"] and "claim" in lein["blocked"]["summary"]
    # the rest: validity unknown (the game's verdict did not answer), never `available`, each with its call
    assert res["available"] == []
    (ath,) = [r for r in res["unchecked"] if r["title_id"] == C_ATHLONE]
    assert ath["validity"] == "unknown" and ath["checked"] == news.FAB_KIN_CHECKED and ath["do_call"]
    # Live: the game's own verdict is asked for each county (here it does not answer)
    assert ("query_fabricate_targets", {"actor_id": ME, "title_id": C_ATHLONE}) in gw.reads


def test_d1_kinship_reads():
    gw = Gw({"query_character": kin_chars})
    assert run(news.holder_kin(gw, ME, FATHER)) == "parent"
    assert run(news.holder_kin(gw, ME, 36124)) == "child"
    assert run(news.holder_kin(gw, ME, 29585)) == ""
    sib = Gw({"query_character": lambda p: dict(kin_chars(p), father_id=FATHER) if p["character_id"] == 5 else kin_chars(p)})
    assert run(news.holder_kin(sib, ME, 5)) == "sibling"
    assert run(news.holder_kin(Gw({"query_character": {}}), ME, FATHER)) is None
    chk = run(news.fabricate_check(Gw({"query_character": {}}), ME, FATHER))
    assert chk["game_check"] == "unknown" and "did not answer" in chk["why"]


def test_d1_the_task_is_not_set_on_the_fathers_county():
    gw = Gw({"query_character": kin_chars, "query_council_tasks": {"character_id": ME, "seats": []},
             "query_title_hierarchy": lambda p: {"title": {"title_id": p["title_id"],
                                                           "holder_id": FATHER if p["title_id"] == C_LEINSTER
                                                           else 29585}}})
    env = run(R.do_impl(gw, intent="council_task", character_id=ME, args={
        "seat": "court_chaplain", "task": "task_fabricate_claim", "target_title_id": C_LEINSTER}))
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "target_ineligible", env
    assert env["blocked"]["raw_reason"] == "game_check_refuses" and "your parent" in env["blocked"]["summary"]
    assert gw.writes == []
    refused, chk = run(R._fabrication_game_check(gw, ME, {"target_title_id": C_ATHLONE}))
    assert refused is None and chk["game_check"] == "unknown"


# ── D2: a task the game resets is not offered a third time; a declined letter says so ──

def test_d2_the_recorded_news_offered_the_same_call_every_step():
    for name in ("news_1071_01_14", "news_1071_02_13"):
        f = news_of(name)["fabrication"]
        assert f["running"] is False and f["next_call"]["args"]["args"]["target_title_id"] == C_LEINSTER


def _seat(task):
    return [{"position_key": "councillor_court_chaplain", "councillor_id": 56090, "task_name": task}]


def _snap(date, task_rows, **kw):
    return dict({"actor_id": ME, "date": date, "council_tasks": task_rows}, **kw)


def test_d2_after_two_resets_the_same_call_is_not_offered():
    gw = Gw({})
    run(news.block(gw, 0, _snap("1070.11.28", _seat("task_religious_relations")), offers_served=False,
                   baseline="turn", fabricate_within="k_ireland"))
    dates = ["1070.12.13", "1071.01.14", "1071.02.13"]
    seen = []
    for set_on, read_on in zip(["1070.11.28"] + dates, dates):
        news.note_fabrication(ME, C_LEINSTER, "c_leinster", "court_chaplain", set_on)
        n = run(news.block(gw, 0, _snap(read_on, _seat("task_religious_relations")), offers_served=False,
                           baseline="turn", fabricate_within="k_ireland"))
        seen.append(n["fabrication"])
    one, two, three = seen
    assert one["reverted"]["times"] == 1 and one["next_call"]["args"]["args"]["target_title_id"] == C_LEINSTER
    assert one["reverted"]["seat_task_now"] == "task_religious_relations"
    for f in (two, three):
        assert f["reverted"]["times"] >= 2 and "not_offered_again" in f
        assert f["next_call"] == {"tool": "do", "args": {"action_class": "fabricate_targets",
                                                         "args": {"within": "k_ireland"}}}
    # an observe of the other baseline in the same step does not count it again
    run(news.block(gw, 0, _snap("1071.02.13", _seat("task_religious_relations")), offers_served=False))
    assert news.reverts_of(ME, C_LEINSTER) == 3


def test_d2_the_recorded_declined_letter_read_running_then_overdue():
    n = news_of("pf_news_1068_09_15")
    (pick,) = [e for e in n["events_answered"] if e["event"] == "court_chaplain_task.0301"]
    assert pick["applied"] == "court_chaplain_task.0301.b" and pick["ai_would_pick"].endswith(".a")
    assert n["fabrication"]["running"] is True and n["fabrication"]["overdue"] is True
    assert "claim_declined" not in n["fabrication"]


def test_d2_a_declined_letter_is_named_and_the_count_restarts():
    row = news_of("pf_news_1068_09_15")["events_answered"][0]
    # the bridge's own pick record (query_event_rules picks), as the news row was made from it
    pick = {"seq": 1, "character_id": ME, "date": row["date"], "event": row["event"], "options": row["options"],
            "ai_pick": {"index": 0, "key": row["ai_would_pick"]}, "applied": {"index": 1, "key": row["applied"]},
            "rule": row["rule"], "why": row["why"], "covered": row["covered"], "gold": row["gold_at_pick"]}
    rows = [{"position_key": "councillor_court_chaplain", "task_name": "task_fabricate_claim",
             "target_province_id": 34}]
    # Athlone's holder in another realm (liege and top liege 999), read: overdue is judged
    gw = Gw({"query_county_neighbors": {"neighbors": [{"realm_ruler_id": ME}]},
             "query_title_hierarchy": {"title": {"holder_id": 700}},
             "query_character": lambda p: ({"id": 700, "faith_id": 1, "liege_id": 999, "top_liege_id": 999}
                                           if p.get("character_id") == 700 else
                                           {"id": p.get("character_id"), "faith_id": 1, "liege_id": 0,
                                            "top_liege_id": ME})})
    council = {"councillor_court_chaplain": {"holder_id": 56090, "task": "task_fabricate_claim"}}
    news.note_fabrication(ME, C_ATHLONE, "c_athlone", "court_chaplain", "1066.09.15")
    news.note_fabrication_province(ME, 34)
    base = dict(council=council, chaplain_learning=8, event_watched=True, event_picks=[])
    run(news.block(gw, 0, _snap("1067.09.15", rows, **base), offers_served=False, baseline="turn"))
    n = run(news.block(gw, 0, _snap("1068.09.15", rows, **dict(base, event_picks=[pick])),
                       offers_served=False, baseline="turn"))
    f = n["fabrication"]
    assert f["claim_declined"]["date"] == "1067.11.24" and "declined" in f["claim_declined"]["detail"]
    assert f["expected"]["counted_from"] == "1067.11.24" and f["overdue"] is False
    assert "fabrication_overdue" not in (n.get("idle") or {})
    # the per-turn form keeps it (a live check 1067.11.24: the count restarted, the name was cut)
    assert news.compact(n)["fabrication"]["claim_declined"]["date"] == "1067.11.24"


# ── D3: the price of losing names what it is computed from ─────────────────────────────

def test_d3_the_recorded_price_and_charge():
    (opt,) = [o for o in env_of("cbs_1073_10_29")["result"]["valid_options"] if o["cb"] == "claim_cb"]
    assert opt["if_you_lose"]["gold"] == -119.9 and "gold_basis" not in opt["if_you_lose"]
    (lost,) = news_of("news_1074_10_16")["purse"]["gold"]["in_this_step"]
    assert lost["expected"] == -202.32 and news_of("news_1074_10_16")["purse"]["gold"]["income"]["monthly"] == 5.62


def test_d3_the_formula_gives_both_recorded_numbers_and_says_it_moves():
    at_declare, at_loss = R.loss_price("claim_cb", 3.33), R.loss_price("claim_cb", 5.62)
    assert at_declare["gold"] == -119.9 and at_loss["gold"] == -202.3      # charged -202.32
    assert "each +1 a month adds 36" in at_declare["gold_basis"] and "on the day you lose" in at_declare["gold_basis"]


def test_d3_the_news_says_when_a_running_wars_loss_price_moved():
    war = {7: {"war_id": 7, "name": "Claim on Athlone", "attacker_id": ME, "is_attacker": True,
               "cb_type": "claim_cb"}}
    gw = Gw({})
    run(news.block(gw, 0, _snap("1073.10.01", None, wars={}, monthly_income=3.33), offers_served=False))
    run(news.block(gw, 0, _snap("1073.10.29", None, wars=war, monthly_income=3.33), offers_served=False))
    n = run(news.block(gw, 0, _snap("1074.06.26", None, wars=war, monthly_income=3.4), offers_served=False))
    assert "loss_price_moved" not in n
    n = run(news.block(gw, 0, _snap("1074.09.24", None, wars=war, monthly_income=5.62), offers_served=False))
    (m,) = n["loss_price_moved"]
    assert m["if_you_lose_now"] == -202.3 and m["first_read"] == {"gold": -119.9, "date": "1073.10.29"}


# ── D4: the mercenary price is the game's, per sub-regiment, from the realm's counties ──

def _company(cid):
    (row,) = [r for r in env_of("mercenaries_1074_06_26")["result"]["available"] if r["company_id"] == cid]
    return row


def test_d4_the_recorded_estimate_was_half_the_charge():
    assert _company(157)["price_estimate"] == 112.53 and _company(360)["affordable"] is True
    assert env_of("hire_157")["result"]["purse_moved"]["gold"] == -223.0
    assert env_of("hire_360")["blocked"]["gate"] == "cannot_afford"


def test_d4_the_estimate_matches_the_charge_and_the_refusal():
    lc = 85.0                                   # @light_cavalry_recruitment_cost
    hus = 90.0 * 1.28                           # huscarls_recruitment_cost = heavy_infantry x 1.28
    est = R.merc_price(_company(157)["regiments"], 2, {"light_horsemen": lc})
    assert abs(est - 223.0) / 223.0 < 0.01, est
    p360 = R.merc_price(_company(360)["regiments"], 2, {"huscarl": hus})
    # gold 163.9 at the hire, income 3.11 a month (the step's read): the game refused it
    assert R.merc_affordable(p360, 163.9, 3.11) is False, p360
    assert R.merc_affordable(est, 163.9, 3.11) is True
    assert R.merc_price(_company(157)["regiments"], None, {"light_horsemen": lc}) is None


def test_d4_the_prices_and_stacks_are_the_game_files():
    v = game_file("common/script_values/00_men_at_arms_values.txt")
    assert re.search(r"@light_cavalry_recruitment_cost = 85\b", v)
    assert "huscarls_recruitment_cost = @[heavy_infantry_recruitment_cost * 1.28]" in v
    d = game_file("common/defines/00_defines.txt")
    assert re.search(r"MAA_COST_RATIO = 0\.5\b", d) and re.search(r"REALM_SIZE_MULT = 0\.05\b", d)
    stacks = {}
    for f in sorted((GAME / "common/men_at_arms_types").glob("*.txt")):
        name = None
        for line in f.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            m = re.match(r"^([a-z_]+) = \{", line)
            if m:
                name = m.group(1)
            m = re.match(r"^\tstack = (\d+)", line)
            if m and name:
                stacks[name] = int(m.group(1))
    assert {k: v for k, v in stacks.items() if v != 100} == R.MAA_STACK


def test_d4_the_listing_reads_the_realms_counties_and_the_hire_shows_both():
    gw = Gw({"query_hireable_mercenaries": {"companies": [dict(_company(157), in_range=True)]},
             "query_character": lambda p: {"id": ME, "gold": 163.9, "realm_size": 7,
                                           "income_breakdown": {"total_monthly": 3.11}},
             "query_realm": {"realm_total_counties": 2},
             "query_maa_types": {"types": [{"maa_type": "light_horsemen", "recruit_cost": 85.0}]}})
    rows, _far, _note = run(R.mercenary_rows(gw, ME))
    assert abs(rows[0]["price_estimate"] - 223.0) < 2.3


# ── D5: every gold move above the threshold names a cause or what was ruled out ────────

def test_d5_the_recorded_moves():
    g = news_of("news_1095_01_06")["purse"]["gold"]
    assert g["unexplained"] == -196.98 and "mercenaries_hired_for_you" not in g
    up = news_of("news_1094_09_08")["purse"]["gold"]
    assert up["unexplained"] == 298.44 and "not_read" not in up and "ruled_out" not in up
    pr = news_of("news_1091_09_15")["purse"]["prestige"]
    assert pr["unexplained"] == -348.97 and pr["ruled_out"] and pr["not_read"]     # named, not silent


def _gold_gw(mercs):
    return Gw({"query_county_details": {"counties": []},
               "query_hireable_mercenaries": {"companies": mercs},
               "query_realm": {"realm_total_counties": 2},
               "query_maa_types": {"types": [{"maa_type": "light_horsemen", "recruit_cost": 85.0}]}})


def test_d5_a_fall_names_mercenaries_the_ai_hired_with_the_lever():
    mine = dict(_company(157), hired_by=ME)
    gw = _gold_gw([mine, dict(_company(360), hired_by=999)])
    run(news.block(gw, 0, _snap("1094.09.08", None, gold=236.08, monthly_income=1.71), offers_served=False))
    n = run(news.block(gw, 0, _snap("1095.01.06", None, gold=45.84, monthly_income=1.71), offers_served=False))
    g = n["purse"]["gold"]
    (m,) = g["mercenaries_hired_for_you"]
    assert m["company_id"] == 157 and g["mercenaries_expected"] == -m["price_estimate"]
    # The veto is the lever (a hire under a reserve of 500)
    assert g["lever"]["args"] == {"intent": "ai_veto", "args": {"op": "set", "what": "mercenary"}}
    assert not any("mercenaries" in x for x in g["not_read"])
    gw = _gold_gw([])
    news.reset()
    run(news.block(gw, 0, _snap("1094.09.08", None, gold=236.08, monthly_income=1.71), offers_served=False))
    g = run(news.block(gw, 0, _snap("1095.01.06", None, gold=45.84, monthly_income=1.71),
                       offers_served=False))["purse"]["gold"]
    assert "no mercenary company is under contract to you" in g["ruled_out"]


def test_d5_a_rise_names_what_was_ruled_out():
    gw = Gw({})
    run(news.block(gw, 0, _snap("1094.07.18", None, gold=-65.28, piety=269.6, monthly_income=1.71),
                   offers_served=False))
    g = run(news.block(gw, 0, _snap("1094.09.08", None, gold=236.08, piety=223.8, monthly_income=1.71),
                       offers_served=False))["purse"]["gold"]
    assert g["unexplained"] > 50 and g["ruled_out"] and any("inherited" in x for x in g["not_read"])


# ── D6: hints name the tools as the client calls them ─────────────────────────────────

def test_d6_the_recorded_bare_calls():
    bare = LIVE["bare_name_refusals"]
    assert len(bare) == 15 and {b["tool"] for b in bare} >= {"observe", "do", "cbs", "inspect"}
    assert news_of("news_1071_01_14")["fabrication"]["next_call"]["tool"] == "do"


def test_d6_every_hint_carries_the_prefix_and_nothing_else_changes():
    names = ["do", "inspect", "observe", "cbs", "plan_war"]
    env = {"ok": True, "tool": "observe", "result": {"news": news_of("news_1071_01_14"),
           "note": "re-verify via inspect(subject='council'); do(action_class='x'); undo(1)",
           "raw": {"script": "do(x)", "tool": "do"}}}
    assert TP.apply(env, names, "") is env
    out = TP.apply(env, names, "mcp__ck3__")
    assert out["tool"] == "observe"
    f = out["result"]["news"]["fabrication"]
    assert f["next_call"]["tool"] == "mcp__ck3__do"
    assert out["result"]["news"]["idle"]["claims_no_war"]["next"]["tool"] == "mcp__ck3__cbs"
    assert out["result"]["note"] == ("re-verify via mcp__ck3__inspect(subject='council'); "
                                     "mcp__ck3__do(action_class='x'); undo(1)")
    assert out["result"]["raw"] == env["result"]["raw"]
    assert TP.apply(out, names, "mcp__ck3__") == out          # applied once, not twice


def test_d6_the_server_applies_it_to_every_reply(monkeypatch):
    import mcp_server as M
    srv = M._StrictArgumentsFastMCP("t")

    @srv.tool()
    def do(x: int = 0) -> dict:
        return {"ok": True, "tool": "do", "result": {"next": {"tool": "do", "args": {}}}}

    def text(r):
        blocks = r[0] if isinstance(r, tuple) else r
        return json.loads(blocks[0].text)
    plain = text(run(srv.call_tool("do", {})))
    assert plain["result"]["next"]["tool"] == "do"
    monkeypatch.setenv(TP.ENV, "mcp__ck3__")
    got = text(run(srv.call_tool("do", {})))
    assert got["result"]["next"]["tool"] == "mcp__ck3__do" and got["tool"] == "do"


# ── D7: the news of the step is readable by the turn's first call ─────────────────────

def test_d7_the_recorded_turn_start_read_was_empty():
    r = json.loads(LIVE["pf_turn2_observe_news"]["reply"])["result"]["result"]["news"]
    assert r["since"] == "1067.09.15" and r["purse"]["gold"]["change"] == 0
    assert "council_changed" not in r


def test_d7_the_same_day_read_gives_the_step_news_again():
    gw = Gw({})
    council = {"councillor_steward": {"holder_id": 1, "task": "task_collect_taxes"}}
    run(news.block(gw, 0, _snap("1066.09.15", None, council=council, gold=84.0), offers_served=False,
                   baseline="turn"))
    moved = {"councillor_steward": {"holder_id": 1, "task": "task_develop_county"}}
    env_read = run(news.block(gw, 0, _snap("1067.09.15", None, council=moved, gold=111.86),
                              offers_served=False, baseline="turn"))
    seat_read = run(news.block(gw, 0, _snap("1067.09.15", None, council=moved, gold=111.86),
                               offers_served=False, baseline="turn"))
    assert seat_read["council_changed"] == env_read["council_changed"] and seat_read["council_changed"]
    assert seat_read["read_again"]["date"] == "1067.09.15"
    later = run(news.block(gw, 0, _snap("1067.11.28", None, council=moved, gold=34.7),
                           offers_served=False, baseline="turn"))
    assert later["since"] == "1067.09.15" and "read_again" not in later


# ── D8: what the AI paid names the lever that decides it ──────────────────────────────

def test_d8_the_recorded_letter_named_no_lever():
    (row,) = news_of("news_1067_11_28")["purse"]["gold"]["in_this_step"]
    assert row["what"].startswith("claim on") and "lever" not in row


def _letter(rules):
    gw = Gw({})
    base = dict(chaplain_learning=8, event_rule_events=rules)
    run(news.block(gw, 0, _snap("1067.09.15", None, claims={}, gold=113.9, **base), offers_served=False))
    n = run(news.block(gw, 0, _snap("1067.11.28", None, claims={469: {"title_id": 469, "title_name": "Athlone",
                                                                     "type": "unpressed", "holder_id": 29585}},
                                    gold=34.7, **base), offers_served=False))
    return [e for e in n["purse"]["gold"]["in_this_step"] if e["what"].startswith("claim on")][0]


def test_d8_a_letter_the_ai_paid_names_the_event_rule_call():
    row = _letter([])
    assert row["decided_by"].startswith("the game's AI") and row["expected"] == -96.0
    assert row["lever"]["args"]["args"] == {"op": "set", "event": "court_chaplain_task.0301", "answer": "pay_if",
                                            "option": "court_chaplain_task.0301.a", "currency": "gold",
                                            "amount": 96}
    assert _letter(["court_chaplain_task.0301"])["decided_by"].startswith("your event rule")
    assert "lever" not in _letter(None)
