"""The tool defects of the second seated Francia 867 run, replayed over its recorded replies
(fixtures/francia867_crown_loss_replies.json: tool replies as the seat received them and the env's news per step; no seat
text). Each defect has its RED: the recorded number or the code without the rule reproduces the hazard.

  1  observe cut the crown-loss news (turn 97, 4,130 of 8,192 bytes)       reply_fit keep paths, smallest omission, regrow
  2  faction power 107 (query) vs 52 "medium" (inspect), turn 53           faction_letters.faction_power, every reader
  3  liege change read "no war, no offer" and offered a set veto (turn 98) news.liege_cause
  4  prestige -499.86 unexplained (step 5)                              news.price_match (the court type's 500)
  5  gold moves unexplained; no months to the 850 at the net income     ai_pick_gold windows; months_to_target
  6  a refused disinherit order said only its code; headship moved      orders.refusal_of; news.dynasty_head_changed
  7  a surrender refused "Does not have a valid Casus Belli" (turn 104)      make_peace surrender -> end_war we_yield
"""
import asyncio
import copy
import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT))

import faction_letters as FL  # noqa: E402
import news as N  # noqa: E402
import reply_fit as F  # noqa: E402

FX = json.loads((HERE / "fixtures" / "francia867_crown_loss_replies.json").read_text())
#: the base commit of this code, and reply_fit.py's blob there
BASE_SHA = "55dc8ec687d36fee03d32412cd921ccc92699247"


def _base(module: str):
    r = subprocess.run(["git", "-C", str(ROOT), "show", f"{BASE_SHA}:ck3agent/server/{module}.py"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip("the base commit is not in this clone")
    p = Path(os.environ.get("TMPDIR", "/tmp")) / f"{module}_tk41base_{os.getpid()}.py"
    p.write_text(r.stdout)
    spec = importlib.util.spec_from_file_location(f"{module}_tk41base", p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    p.unlink()
    return m


# ── 1. the cut keeps the seat's own losses and uses its room ─────────────────────────────────────────────────────

def t97_env() -> dict:
    """A crown-loss observe shaped as turn 97's (27,258 bytes recorded; this one 17,510): the recorded turn 98 reply with
    the turn-97 cut's list counts restored (events_answered 14, holders_changed 44, gold in_this_step 14,
    council_changed 6, ai_acted 3, prestige in_this_step 2, a demand's options 3), the faction war lost, the liege
    change and k_france lost, and the blocks a cut drops."""
    env = copy.deepcopy(FX["t98_observe"]["reply"])
    res = env["result"]
    res.pop("truncated", None)
    n = res["news"]
    n["wars_ended"] = [{"war_id": 201326645, "name": "Claimant Faction War", "attacker_id": 47350,
                        "defender_id": 16710, "my_side": "defender", "outcome": "attacker_victory",
                        "end_date": "0902.01.20"}]
    # the turn-97 cut listed no titles_lost row count: one row (k_france; the counties were his vassals')
    n["titles_lost"] = [{"title_id": 7052, "key": "k_france", "how": "war 201326645 ended attacker_victory",
                         "holder_id": 15236}]
    n["faction_demands_answered"] = [{"faction_id": 75, "event": "faction_demand.1001", "answered": "refuse",
                                      "options": [{"index": i, "key": f"opt{i}", "text": "x" * 120} for i in range(3)],
                                      "then": "y" * 200}]
    n["purse"]["prestige"]["in_this_step"] = [{"what": "x" * 150, "expected": -5}, {"what": "y" * 150, "expected": -5}]
    n["liege_changed"] = {"from": 0, "to": 15236}
    ev = {"event": "faction_demand.1001", "options": ["a", "b"], "ai_would_pick": "b", "applied": "b",
          "rule": None, "gold_at_pick": 12.5}
    n["events_answered"] = [dict(ev, date=f"0901.{i % 12 + 1:02d}.01") for i in range(14)]
    n["map_changes"]["holders_changed"] = [{"county": 7000 + i, "holder": "16710 -> 15236"} for i in range(44)]
    n["council_changed"] = n["council_changed"] * 6
    n["ai_acted"] = [{"date": "0901.12.01", "what": "perk", "key": "x_perk"}] * 3
    n["purse"]["gold"]["in_this_step"] = [{"what": f"event e{i} answered", "expected": -1.5} for i in range(14)]
    res["military"] = {"armies": [{"army_id": i, "men": 1000, "location": "somewhere"} for i in range(30)],
                       "unit": "men", "who_commands": "x" * 200}
    res["council"] = {"seats": [{"seat": f"s{i}", "holder": i, "task": "task_x"} for i in range(8)]}
    res["posture"] = {"phase": "x" * 300}
    res["standing_policies"] = {"policies": {"a": 1}, "holds": "x" * 400, "not_offered": "y" * 300}
    return env


def _deliver(mod, env):
    return mod.delivered(mod.sent(copy.deepcopy(env), "observe", {}), "observe", {})


def test_1_the_record_cut_the_crown_loss_news(monkeypatch):
    """The recorded turn 97 reply: 4,130 bytes delivered, news omitted whole, no wars_ended/liege_changed/titles_lost."""
    rec = FX["t97_observe_cut"]
    assert rec["delivered_bytes"] == 4130
    res = rec["reply"]["result"]
    assert res["news"].startswith("omitted: ")
    assert "27258 bytes" in res["truncated"]["why"]


def test_1_RED_the_base_drops_the_loss_news_and_leaves_room(monkeypatch):
    monkeypatch.setenv(F.PAGES_ENV, "off")
    B = _base("reply_fit")
    out = _deliver(B, t97_env())
    n = out["result"]["news"]
    assert isinstance(n, str) and n.startswith("omitted: "), type(n).__name__      # the losses went with it
    assert B.size(out) + B.wrapper_bytes("observe") < 6000                          # and the room was left


def test_1_the_cut_keeps_the_losses_and_uses_the_room(monkeypatch):
    monkeypatch.setenv(F.PAGES_ENV, "off")
    out = _deliver(F, t97_env())
    size = F.size(out) + F.wrapper_bytes("observe")
    assert 7800 <= size <= F.CAP, size
    n = out["result"]["news"]
    assert n["wars_ended"] and n["liege_changed"]["to"] == 15236
    assert n["titles_lost"][0]["key"] == "k_france"
    assert "news" not in {d["what"] for d in out["result"]["truncated"].get("dropped") or []}


def test_1_many_lost_titles_rank_the_highest_tier_first(monkeypatch):
    monkeypatch.setenv(F.PAGES_ENV, "off")
    env = t97_env()
    env["result"]["news"]["titles_lost"] = [{"title_id": 7100 + i, "key": f"c_x{i}", "how": "war 1 ended",
                                             "holder_id": 15236} for i in range(40)] + env["result"]["news"]["titles_lost"]
    out = _deliver(F, env)
    assert out["result"]["news"]["titles_lost"][0]["key"] == "k_france"
    assert F.size(out) + F.wrapper_bytes("observe") <= F.CAP


def test_1_with_pages_the_losses_are_on_the_first_page(monkeypatch):
    monkeypatch.setenv(F.PAGES_ENV, "on")
    out = _deliver(F, t97_env())
    assert F.size(out) + F.wrapper_bytes("observe") <= F.CAP
    n = out["result"]["news"]
    assert n["wars_ended"] and n["liege_changed"] and n["titles_lost"][0]["key"] == "k_france"
    assert out["result"]["more"]["page"].startswith("1 of ")


def _small(extra_news: int):
    return {"ok": True, "tool": "observe", "result": {
        "goal": {"g": "x" * 900}, "standing": {"s": "y" * 900},
        "news": {"liege_changed": {"from": 1, "to": 2, "how": "w" * 160},
                 "council_changed": {"c": "z" * extra_news}}}}


def test_1_the_smallest_omission_never_takes_a_keep_block(monkeypatch):
    """A cap at which the smallest block that frees the room is news.liege_changed when `keep` is not read (found by
    the probe below, the spec without its keep): with keep, liege_changed stays and another block goes."""
    env = _small(400)
    no_keep = {k: v for k, v in F.SPECS["observe"].items() if k != "keep"}
    cap = None
    for c in range(F.size(env) - 50, 600, -10):
        monkeypatch.setitem(F.SPECS, "observe", no_keep)
        probe = F.fit(copy.deepcopy(env), "observe", {}, cap=c)
        monkeypatch.undo()
        pn = probe["result"]["news"]
        if str(pn if isinstance(pn, str) else pn.get("liege_changed")).startswith("omitted: "):
            cap = c
            break
    assert cap is not None, "no cap makes liege_changed the smallest sufficient block"
    out = F.fit(copy.deepcopy(env), "observe", {}, cap=cap)
    n = out["result"]["news"]
    assert isinstance(n["liege_changed"], dict) and n["liege_changed"]["to"] == 2, (cap, n)
    assert F.size(out) <= cap


def test_1_with_pages_a_keep_block_is_laid_out_before_the_other_blocks():
    env = _small(400)
    out = F.paginate(copy.deepcopy(env), "observe", {}, cap=1500)
    assert out["result"]["news"]["liege_changed"]["to"] == 2, out["result"]
    assert "goal" not in out["result"] or "standing" not in out["result"]          # the others moved on


def test_1_CONTROL_a_reply_under_the_cap_is_the_same_object(monkeypatch):
    monkeypatch.setenv(F.PAGES_ENV, "off")
    small = {"ok": True, "result": {"news": {"wars_ended": [], "titles_lost": []}}}
    assert F.fit(small, "observe", {}) is small


# ── 2. one faction power ─────────────────────────────────────────────────────────────────────────────────────────

def _f75(rows):
    return next(f for f in rows if f.get("faction_id") == 75)


def test_2_the_record_two_numbers_for_one_faction():
    q = _f75(FX["t53_query_factions"]["factions"])
    i = _f75(FX["t53_inspect_factions"]["factions"])
    assert q["power_ratio"] == 107.38 and i["power_pct_of_liege"] == 51.78 and i["threat_level"] == "medium"


def test_2_RED_the_old_share_formula_is_the_recorded_inspect_number():
    r = 107.38
    assert round(r / (r + 100.0) * 100.0, 2) == 51.78


def test_2_the_one_power_is_the_games_and_grades_the_threat():
    q = _f75(FX["t53_query_factions"]["factions"])
    assert FL.faction_power(q) == 107.38
    row = {"power_ratio": q["power_ratio"], "discontent": q["discontent_pct"], "threat_level": "medium"}
    assert FL.graded_threat(row) == "high"                           # over 100: as strong as the ruler
    # Pepin's faction at turn 90: share 53.67 read "medium" at discontent 74; its power is 115.9, over the threshold
    p90 = 53.67 / (100 - 53.67) * 100
    assert FL.graded_threat({"power_ratio": p90, "discontent": 74.0, "threat_level": "medium"}) == "high"
    assert FL.faction_power({"power_pct_of_liege": 90.0}) == 90.0     # a reply without power_ratio (new bridge)
    assert FL.faction_power({}) is None


def test_2_every_reader_calls_the_one_power():
    """No server reader takes power_pct_of_liege itself; the bridge sends power_ratio under that name too."""
    for mod in ("factions.py", "mcp_server.py", "redesign.py"):
        src = (HERE / mod).read_text()
        assert not re.search(r'get\("power_pct_of_liege"\)', src), mod
        assert "faction_power(" in src, mod
    cpp = (ROOT / "bridge" / "src" / "query_handlers.cpp").read_text()
    assert "power_ratio / (power_ratio + 100.0)" not in cpp
    assert "double power_pct_of_liege = (power_ratio >= 0) ? power_ratio : 0.0;" in cpp


# ── 3. the liege change's cause ──────────────────────────────────────────────────────────────────────────────────

def _t98(**snap):
    row = copy.deepcopy(FX["t98_observe"]["reply"]["result"]["news"]["liege_changed"])
    base = {"actor_id": 16710, "date": "0902.05.21", "ai_vetoes": ["create_title", "law", "vassal_offer", "ally_call"]}
    return {"from": row["from"], "to": row["to"]}, dict(base, **snap)


def test_3_the_record_read_no_cause_and_offered_a_set_veto():
    row = FX["t98_observe"]["reply"]["result"]["news"]["liege_changed"]
    assert row["from"] == 15236 and row["to"] == 28873
    assert "no war with him ended" in row["cause_unread"] and row["closes_it"]["args"]["args"]["what"] == "vassal_offer"


def test_3_RED_the_base_offers_the_veto_that_is_set():
    B = _base("news")
    row, snap = _t98(liege_of_liege=15236)
    got = B.liege_cause(row, {}, {"liege_id": 15236}, snap)
    assert "closes_it" in got and "cause" not in got


def test_3_a_new_liege_who_is_the_old_lieges_vassal_was_put_above_you():
    row, snap = _t98(liege_of_liege=15236)
    got = N.liege_cause(row, {}, {"liege_id": 15236}, snap)
    assert got["cause"] == "placed_under_his_vassal" and "closes_it" not in got
    assert "28873 is a vassal of 15236" in got["how"]


def test_3_a_set_veto_is_said_set_never_offered():
    row, snap = _t98(liege_of_liege=99)
    got = N.liege_cause(row, {}, {"liege_id": 15236}, snap)
    assert "cause_unread" in got and "closes_it" not in got and "vassal_offer is set" in got["veto_already_set"]
    got = N.liege_cause(row, {}, {"liege_id": 15236}, dict(snap, ai_vetoes=["law"]))
    assert got["closes_it"] == N.VASSAL_OFFER_VETO                   # CONTROL: unset, it is offered


def test_3_a_war_lost_a_read_earlier_is_the_cause():
    N._LOST_WARS.clear()
    out1 = {"wars_ended": [{"war_id": 7, "name": "Claimant War", "my_side": "defender", "outcome": "attacker_victory",
                            "end_date": "0902.01.20"}],
            "titles_lost": [{"key": "k_france", "how": "war 7 ended attacker_victory", "holder_id": 15236}]}
    N.note_lost_wars(16710, out1)
    got = N.liege_cause({"from": 0, "to": 15236}, {}, {"liege_id": 0},
                        {"actor_id": 16710, "date": "0902.03.01", "ai_vetoes": ["vassal_offer"]})
    assert got["cause"] == "war" and "k_france" in got["how"] and "lost it" in got["how"]
    N._LOST_WARS.clear()


# ── 4. the step-5 prestige ───────────────────────────────────────────────────────────────────────────────────────

def test_4_the_record_500_unexplained_beside_a_court_type_post():
    n = FX["step5_news"]["news"]
    assert n["purse"]["prestige"]["unexplained"] == -499.86
    ct = next(r for r in n["ai_vetoed"] if r["what"] == "court_type")
    assert ct["price"] == {"prestige": 500.0} and ct["last"] == "0869.10.06"


def test_4_the_fall_is_named_by_the_post_whose_price_it_matches():
    n = FX["step5_news"]["news"]
    lo, hi = N._ordinal_of("0869.08.20"), N._ordinal_of("0869.11.18")
    m = N.price_match("prestige", -499.86, {"ai_vetoed": n["ai_vetoed"]}, lo, hi, 14.9)
    assert m["prestige"] == -500.0 and m["what"] == "court_type (court_scholarly)" and m["date"] == "0869.10.06"
    assert "veto dropped that post" in m["words"]
    # CONTROL: a fall no recorded price matches names nothing
    assert N.price_match("prestige", -300.0, {"ai_vetoed": n["ai_vetoed"]}, lo, hi, 14.9) is None


# ── 5. gold ──────────────────────────────────────────────────────────────────────────────────────────────────────

def test_5_RED_the_record_counted_the_lost_wars_gold_twice():
    g = FX["step105_gold"]["gold"]
    rows = {r["what"].split(":")[0]: r.get("expected") for r in g["in_this_step"]}
    assert g["unexplained"] == 357.28 and rows["war lost as its attacker"] == -207.0
    assert any(r.get("expected") == -243.1 and "coronation_events.0311" in r["what"] for r in g["in_this_step"])


def test_5_a_priced_row_inside_a_picks_window_comes_out_of_it(monkeypatch):
    """The step-105 pick coronation_events.0311 (0904.12.25) to the read (0905.02.22, gold -141.35, income 5.75):
    its gold at the pick is the one that reproduces the recorded -243.1 (the env's news kept 5 of the picks); the
    war's end 0905.02.17 (-207.0 priced) falls inside the window. The window arithmetic only; whether the
    option moves gold is not read here (coronation_events.0311.a moves none: test_rules_carried_to_heir gives it no fall)."""
    import event_prices as _ep
    monkeypatch.setattr(_ep, "option_gold", lambda *a: None)
    s = FX["step105_gold"]
    days = N._ordinal_of(s["date"]) - N._ordinal_of("0904.12.25")
    g0 = round(-141.35 + 243.1 - 5.75 * days / 30.44, 2)
    ans = [{"event": "coronation_events.0311", "applied": "coronation_events.0311.a", "date": "0904.12.25",
            "gold_at_pick": g0}]
    prev, snap = {"monthly_income": 5.75}, {"gold": -141.35, "date": s["date"]}
    (old,) = N.ai_pick_gold(prev, snap, ans)
    assert abs(old["expected"] - -243.1) <= 0.01                     # RED: the recorded double count
    end = s["wars_ended"][0]["end_date"]
    new = N.ai_pick_gold(prev, snap, ans, [(end, -207.0)])
    assert new and abs(new[0]["expected"] - (-243.1 + 207.0)) <= 0.01 and new[0]["less_priced_in_window"] == -207.0
    # CONTROL: a priced row dated outside the window is not taken out
    (out,) = N.ai_pick_gold(prev, snap, ans, [("0904.12.01", -207.0)])
    assert abs(out["expected"] - -243.1) <= 0.01


def test_5_months_to_the_price_at_the_net_income():
    R = pytest.importorskip("redesign")
    assert R.months_to_target(768.9, {"monthly": -11.66, "from": "a", "to": "b"})["months"] is None
    assert "never" in R.months_to_target(768.9, {"monthly": -11.66})["why"]
    assert R.months_to_target(768.9, {"monthly": 12.0})["months"] == 65
    assert R.months_to_target(10, None)["why"].startswith("no step read yet")
    # live 0905.01.08: a 7-day step holding a surrender's gold read -1036.92 a month; a short step gives no month
    short = R.months_to_target(996.6, {"monthly": -1036.92, "from": "0905.01.01", "to": "0905.01.08"})
    assert short["months"] is None and "7 days" in short["why"]
    assert R.months_to_target(631.53, {"monthly": 9.26, "from": "0889.01.01", "to": "0889.03.15"})["months"] == 69


def test_5_an_activity_names_a_gold_fall():
    got = N.activity_ran("gold", -273.29, {"events_answered": [{"event": "ach_coronation.0001"},
                                                                  {"event": "stress_threshold.1001"}]})
    assert got["events"] == ["ach_coronation.0001"]
    assert N.activity_ran("gold", -273.29, {"events_answered": [{"event": "stress_threshold.1001"}]}) is None


# ── 6. the order's refusal sentence; the dynasty headship ────────────────────────────────────────────────────────


def test_6_RED_the_record_said_only_the_code():
    rec = {"gate": "interaction_invalid", "reason_code": "interaction_not_valid_for_target",
           "why": "interaction_not_valid_for_target"}                  # a recorded game step 65, 0892.02.08
    assert " " not in rec["why"]


def test_6_the_dynasty_head_moving_is_news():
    prev = {"actor_id": 14577, "dynasty_id": 2032, "dynasty_head": 14577}
    snap = {"actor_id": 14577, "dynasty_id": 2032, "dynasty_head": 13495}
    row = N.dynasty_head_changed(prev, snap)
    assert row["from"] == 14577 and row["to"] == 13495 and not row["you_are_head"] and "disinherit" in row["lost"]
    back = N.dynasty_head_changed(snap, prev)
    assert back["you_are_head"] and "gained" in back
    assert N.dynasty_head_changed(prev, dict(snap, dynasty_head=None)) is None      # an unread head is no news
    assert N.dynasty_head_changed(prev, prev) is None


# ── 7. a surrender the game refused by its interaction ───────────────────────────────────────────────────────────

def test_7_the_record_refused_the_surrender():
    r = FX["t104_surrender"]["reply"]
    assert r["blocked"]["gate"] == "interaction_invalid" and r["blocked"]["detail"] == "Does not have a valid Casus Belli"
    assert r["result"]["interaction"] == "end_war_attacker_defeat_interaction"


def test_7_a_refused_surrender_is_sent_as_end_war_we_yield(monkeypatch):
    R = pytest.importorskip("redesign")
    import test_war_intents as W

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    gw = W.WarGateway(replies={"query_wars": W._active(W._war_row()),
                               "query_intent_results": {"results": [
                                   {"intent_id": "si_100_1_1", "status": "rejected",
                                    "rejection_reason": "interaction_not_valid_for_target"}]}})
    env = asyncio.run(R.do_impl(gw, intent="make_peace", character_id=W.ME,
                                args={"war_id": W.WAR, "terms": "surrender"}))
    sent = [p for a, p in gw.writes if a == "script_action"]
    assert any(p.get("template") == "end_war" and p.get("terms") == "we_yield" and p.get("target_id") == W.ENEMY
               for p in sent), gw.writes
    assert env["result"]["routed"]["sent_instead"]["args"] == {"enemy_id": W.ENEMY, "terms": "we_yield"}
    # CONTROL: a refused white peace is not routed (it needs his answer)
    gw2 = W.WarGateway(replies={"query_wars": W._active(W._war_row()),
                                "query_intent_results": {"results": [
                                    {"intent_id": "si_100_1_1", "status": "rejected",
                                     "rejection_reason": "interaction_not_valid_for_target"}]}})
    env2 = asyncio.run(R.do_impl(gw2, intent="make_peace", character_id=W.ME,
                                 args={"war_id": W.WAR, "terms": "white_peace"}))
    assert not [p for a, p in gw2.writes if a == "script_action"] and "routed" not in (env2.get("result") or {})
