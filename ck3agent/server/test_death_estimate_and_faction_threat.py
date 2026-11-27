"""Rows from the Karling exploratory run (867, Charles of West Francia).

Defect 1, the death estimate. fixtures/ruler_deaths.json is every seat-ruler death in the recorded
trajectories (a census of the recorded games, plus the Karling
run), with the ruler's age and health read at most once a year before it, and the rulers who outlived their
runs (censored). The calibration below runs the shipped estimate over those rows. Without the calibration
the game's health check alone put 26 of 27 deaths before its own 25 % year.
"""
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ruler_death as RD  # noqa: E402

ROWS = json.load(open(os.path.join(HERE, "fixtures", "ruler_deaths.json")))["rows"]


def _first_reads(died):
    first = {}
    for r in ROWS:
        if r["died"] is died:
            first.setdefault((r["game"], r["ruler"]), r)
    return list(first.values())


def _before(rows, q):
    """How many of these rulers died by the band's year q (None = past the horizon: not before)."""
    n = 0
    for r in rows:
        v = RD.estimate(r["age"], r["health"])["years"].get(q)
        n += v is not None and r["hi"] <= v
    return n


def test_1_census_is_the_recorded_deaths():
    dying = _first_reads(True)
    assert len(dying) == 32                       # 29 enumerated, 2 had no age/health read before the death;
    #                                               then game AH 28101, 34471 and game AI 12015;
    #                                               then games AJ 31762 and AK 31762 (and two survivors)
    assert any(r["game"] == "AG" and r["ruler"] == 12015 for r in dying)
    assert len({r["game"] for r in ROWS}) >= 25


def test_1_band_is_calibrated_on_the_recorded_deaths():
    """From each dying ruler's first read: about a quarter dead by p25, half by p50, most by p90."""
    dying = _first_reads(True)
    n = len(dying)
    assert 0.10 <= _before(dying, "p25") / n <= 0.40
    assert 0.30 <= _before(dying, "p50") / n <= 0.70
    assert _before(dying, "p75") / n >= 0.65
    assert _before(dying, "p90") / n >= 0.90       # the range now..p90 holds the recorded deaths
    every = [r for r in ROWS if r["died"]]
    assert _before(every, "p90") / len(every) >= 0.90


def test_1_band_is_not_trivially_short():
    """The survivors (censored at their run's end): a band that said 'any day now' would be calibrated
    on deaths and useless; most survivors outlived the 25 % year."""
    alive = _first_reads(False)
    out = 0
    for r in alive:
        v = RD.estimate(r["age"], r["health"])["years"]["p25"]
        out += v is not None and r["hi"] > v
    assert out / len(alive) >= 0.40


def test_1_karling_reads():
    # Turn 1 867.01.01: age 43, health 3.66; died 8.4 years later. Base: p25 16.8, p50 18.9.
    est = RD.estimate(43.5, 3.66)["years"]
    assert est["p25"] < 8.4 <= est["p90"] and est["p50"] <= 11
    eta, basis, _ = RD.eta_years(43.5, 3.66)
    assert eta <= 5 and "sooner" in basis
    # Turn 18 874.05: health 2.79; died 1.1 years later. Base: p25 5.2.
    assert RD.estimate(50.9, 2.79)["years"]["p25"] < 5.2


def test_1_says_it_is_uncertain():
    est = RD.estimate(40, 4.2)
    assert "not a date" in est["uncertain"] and len(json.dumps(est)) < 600


def test_1_CONTROL_the_game_check_alone_fails_the_calibration(monkeypatch):
    """The control that runs: with the outside rate off (the superseded model) the same rows red."""
    monkeypatch.setattr(RD, "OUTSIDE_MONTHLY_AT_40", 0.0)
    dying = _first_reads(True)
    assert _before(dying, "p25") >= 24


# ── defect 2: the faction read ────────────────────────────────────────────────
import faction_letters as FL  # noqa: E402
from test_faction_letters import fac, game, snap, threat  # noqa: E402,F401

GAME = os.environ.get("CK3_GAME_DIR") or os.path.expanduser(
    "~/.local/share/Steam/steamapps/common/Crusader Kings III/game")      # the game's `game/` folder, as seat_events.game_dir


def _karling_faction(**kw):
    # a recorded game step 50, 886.08: claimant faction 171.4 % of the ruler's power, discontent 38, bridge "medium"
    return fac(fid=60, level="medium", discontent=38.0, power=171.4, type="claimant_faction", **kw)


def test_2_a_faction_stronger_than_the_ruler_is_high_with_its_clock(game):
    (row,) = FL.news_part(snap(threat([])), snap(threat([_karling_faction()])))["factions_against_you"]
    assert row["threat_level"] == "high" and row["power_pct"] == 171.4
    assert row["over_demand_threshold"] is True
    # the game's rule: +3 base, +min(10, round(3 x 4 x 91.4 / 80)) = +13 a month: (100 - 38) / 13 -> 5 months
    assert row["demand_in"].startswith("about 5 months") and "90 days" in row["demand_in"]
    assert "AS A LOSS" in row["if_war_lost"] and "vassal" in row["if_war_lost"]
    assert any("imprison" in x for x in row["levers"]) and any("gift" in x for x in row["levers"])
    # refusing a faction stronger than the ruler is not the suggested call any more; it is still offered
    assert row["next"] == {"tool": "inspect", "args": {"subject": "factions"}}
    assert row["refuse_call"]["args"]["intent"] in ("event_rule", "faction")


def test_2_a_weak_faction_keeps_the_refuse_call_and_reads_no_demand(game):
    (row,) = FL.news_part(snap(threat([])), snap(threat([fac(power=60.0)])))["factions_against_you"]
    assert row["over_demand_threshold"] is False and row["demand_in"].startswith("not while")
    assert row["next"]["args"]["intent"] == "event_rule" and "refuse_call" not in row


def test_2_crossing_the_threshold_is_news(game):
    s1 = snap(threat([fac(power=70.0, level="low", discontent=10.0)]))
    s2 = snap(threat([fac(power=95.0, level="low", discontent=10.0)]))
    (row,) = FL.news_part(s1, s2)["factions_against_you"]
    assert row["shown_because"].startswith("crossed the demand power threshold")


def test_2_the_bridge_grade_is_never_lowered():
    assert FL.graded_threat({"threat_level": "critical", "power_ratio": 10, "discontent": 0}) == "critical"
    assert FL.graded_threat({"threat_level": "low", "power_ratio": 85, "discontent": 0}) == "medium"
    # 85 %: +3 +round(12 x 5 / 80) = +4 a month; from 70 that is 8 months (medium), from 80 it is 5 (high)
    assert FL.graded_threat({"threat_level": "low", "power_ratio": 85, "discontent": 70}) == "medium"
    assert FL.graded_threat({"threat_level": "low", "power_ratio": 85, "discontent": 80}) == "high"


def test_2_the_clock_constants_are_the_game_files():
    d = os.path.join(GAME, "common", "defines", "00_defines.txt")
    if not os.path.exists(d):
        pytest.skip("the game's files are not installed here")
    txt = open(d, encoding="utf-8-sig").read()

    def val(text, name):
        line = next(x for x in text.splitlines() if x.strip().startswith(name + " "))
        return float(line.split("=")[1].split("#")[0])
    assert val(txt, "DISCONTENT_THRESHOLD") == FL.DISCONTENT_THRESHOLD
    assert val(txt, "DEFAULT_POWER_THRESHOLD") == FL.POWER_THRESHOLD
    assert val(txt, "MAX_DEMAND_DELAY_DAYS") == FL.MAX_DEMAND_DELAY_DAYS
    sv = open(os.path.join(GAME, "common", "script_values", "00_faction_values.txt"), encoding="utf-8-sig").read()
    assert val(sv, "base_discontent_progress") == FL.BASE_PROGRESS
    block = sv.split("faction_discontent_for_extra_power")[1].split("\n}")[0]
    assert f"multiply = {FL.EXTRA_PROGRESS_FACTOR}" in block and f"max = {FL.EXTRA_PROGRESS_MAX}" in block
    fx = open(os.path.join(GAME, "common", "factions", "00_factions.txt"), encoding="utf-8-sig").read()
    claimant = fx.split("claimant_faction = {")[1].split("power_threshold")[1][:40]
    assert "base = 80" in claimant


# ── defect 3: a liege change in the step a lost war ended ─────────────────────
import news as N  # noqa: E402

#: A recorded game, step 61 (889.02.03), the news rows as recorded (ids and keys only)
KARLING_61 = {"wars_ended": [{"attacker_id": 11403, "defender_id": 14577, "end_date": "0889.01.28",
                              "my_side": "defender", "name": "War for Prince Louis' Claim on the Kingdom of West Francia",
                              "outcome": "attacker_victory", "war_id": 150995015}],
              "titles_lost": [{"how": "war 150995015 ended attacker_victory", "key": "k_france", "title_id": 7052}]}


def test_3_the_vassalisation_is_attributed_to_the_war_lost_in_the_same_step():
    out = N.liege_cause({"from": 0, "to": 16710}, dict(KARLING_61), {}, {"date": "0889.02.03"})
    assert out["cause"] == "war" and "150995015" in out["how"] and "k_france" in out["how"]
    assert "closes_it" not in out and "cause_unread" not in out


def test_3_a_won_war_is_not_the_cause():
    won = {"wars_ended": [dict(KARLING_61["wars_ended"][0], outcome="defender_victory")], "titles_lost": []}
    out = N.liege_cause({"from": 0, "to": 16710}, won, {}, {"date": "0889.02.03"})
    assert "cause_unread" in out


# ── defect 4: titles gained and lost, direction and cause ─────────────────────
import test_ai_veto as TV  # noqa: E402
from test_ireland_walk_replies import run  # noqa: E402

DUB = {"title_id": 463, "key": "c_dublin"}
ATH = {"title_id": 469, "key": "c_athlone"}


def _war(wid=16777240):
    return {"war_id": wid, "name": "Claim on Athlone", "attacker_id": TV.ME, "defender_id": 29585,
            "is_attacker": True, "cb_type": "claim_cb"}


def test_4_a_county_won_by_war_and_then_granted_is_not_lost_by_that_war():
    """A recorded game, step 6: c_nantes listed lost 'how: war 16777240 ended attacker_victory', the war that won it."""
    N.reset()
    hist = {"wars": [{"war_id": 16777240, "outcome": "attacker_victory", "end_date": "0869.08.20",
                      "target_titles": [469]}]}
    gw = TV.Gw({"query_war_history": hist})
    s1 = TV.snap("0869.06.01", [], wars={"wars": [_war()]}, held={"held_titles": [DUB]})
    s2 = TV.snap("0869.08.24", [], wars={"wars": []}, held={"held_titles": [DUB, ATH]})
    s3 = TV.snap("0869.12.22", [], wars={"wars": []}, held={"held_titles": [DUB]})
    run(N.block(gw, 0, s1, offers_served=False))
    out2 = run(N.block(gw, 0, s2, offers_served=False))
    assert out2["titles_gained"][0]["how"] == "war 16777240 ended attacker_victory"
    out3 = run(N.block(gw, 0, s3, offers_served=False))
    (lost,) = out3["titles_lost"]
    assert "16777240" not in str(lost["how"]) and lost["how"] is not None
    if lost["how"] == "unknown":
        assert lost["how_reason"] == "no_record_in_step"
    N.reset()


def test_4_a_title_whose_war_was_declared_and_ended_between_reads_names_it():
    """A recorded game, step 20: c_rennes gained how null; its claim war was declared and won inside the step."""
    N.reset()
    s1 = TV.snap("0874.08.24", [], held={"held_titles": [DUB]})
    N.note_act(TV.ME, "0874.08.24", "declared war 83886112 (claim_cb)", cost_known=True, prestige=-100.0,
               war_id=83886112, war_title_id=469)
    s2 = TV.snap("0875.01.21", [], held={"held_titles": [DUB, ATH]})
    run(N.block(TV.Gw({}), 0, s1, offers_served=False))
    out = run(N.block(TV.Gw({}), 0, s2, offers_served=False))
    (g,) = out["titles_gained"]
    assert "83886112" in g["how"] and g.get("by") == "you"
    N.reset()


def test_4_a_title_gained_with_no_record_is_unknown_with_a_reason_never_null():
    N.reset()
    out = TV.two(TV.snap("0887.03.15", [], held={"held_titles": [DUB]}),
                 TV.snap("0887.06.13", [], held={"held_titles": [DUB, ATH]}))
    (g,) = out["titles_gained"]
    assert g["how"] == "unknown" and g["how_reason"] == "no_record_in_step"
    assert N.compact(out)["titles_gained"][0]["how_reason"] == "no_record_in_step"
    N.reset()


def test_4_titles_a_new_ruler_brings_are_the_succession():
    N.reset()
    s1 = TV.snap("0875.01.21", [], held={"held_titles": [DUB]})
    s2 = TV.snap("0875.06.09", [], held={"held_titles": [DUB, ATH]}, actor_id=TV.ME + 1)
    out = TV.two(s1, s2)
    (g,) = out["titles_gained"]
    assert g["how"].startswith("at the succession")
    N.reset()


# ── defect 6: the cap on the bytes the seat receives ──────────────────────────
import gzip  # noqa: E402

import reply_fit as RF  # noqa: E402

OVER = json.load(gzip.open(os.path.join(HERE, "fixtures", "francia867_over_cap.json.gz")))["rows"]


def _delivered_size(env, tool):
    return RF.size({"tool_name": tool, "ok": True, "result": env, "error": None})


def test_6_the_recorded_over_cap_replies_are_the_ones_the_seat_got():
    assert len(OVER) == 9 and max(r["delivered_bytes"] for r in OVER) == 8630
    for r in OVER:
        assert _delivered_size(r["env"], r["tool"]) == r["delivered_bytes"]
    # the replies carry the public tool prefix, shorter than the recording's: two rows fell
    # just under the cap, so the fit test below is a real cut on these seven, stated here
    assert sum(r["delivered_bytes"] > RF.CAP for r in OVER) == 7


def test_6_replayed_through_the_delivered_cap_each_fits_and_keeps_every_list():
    for r in OVER:
        out = RF.delivered(r["env"], r["tool"], r["args"])
        assert _delivered_size(out, r["tool"]) <= RF.CAP, (r["tool"], _delivered_size(out, r["tool"]))
        res = out.get("result") if isinstance(out.get("result"), dict) else out
        r_in = r["env"].get("result") if isinstance(r["env"].get("result"), dict) else r["env"]
        # a cut the recorded (older) tree already made to 0 rows stays said; this pass cuts none to 0
        old0 = {x["list"] for x in (r_in.get("truncated") or {}).get("lists") or [] if x.get("shown") == 0}
        for row in (res.get("truncated") or {}).get("lists") or []:
            assert row["list"] in old0 or row["shown"] >= min(RF.MIN_ROWS, row["of"]), row


def test_6_a_second_pass_keeps_the_first_pass_cuts_said():
    r = next(x for x in OVER if x["tool"] == "plan_war")
    first = (r["env"]["result"]["truncated"])
    out = RF.delivered(r["env"], r["tool"], r["args"])
    t = out["result"]["truncated"]
    assert t["why"].startswith(first["why"]) and len(t.get("dropped") or []) >= len(first.get("dropped") or [])


def test_6_writes_are_still_sent_whole():
    big = {"ok": True, "result": {"x": "y" * 9000}}
    assert RF.delivered(big, "do", {"intent": "interact"}) is big


def test_6_plan_war_rows_say_the_precheck_rule_once():
    row = {"target_name": "a", "cb_options": [1], "precheck": {"passed": True, "as_of": "0867.01.01",
                                                               "limit": "L", "not_checked": ["g"]}}
    res: dict = {}
    out = RF._plan_row(row, res)
    assert out["precheck"] == {"passed": True, "as_of": "0867.01.01"} and "cb_options" not in out
    assert res["precheck_rule"] == {"limit": "L", "not_checked": ["g"]}


# ── defect 7: cancel_pending on an id a reply gave ────────────────────────────
import redesign as R  # noqa: E402


def test_7_a_write_the_game_already_ran_is_refused_in_plain_words():
    """A recorded game, turn 11 (0871.08.29): call_ally answered pending (drains_when clock_advances, intent_id
    si_12015_1700_22999); cancel_pending on that id read 'the name given does not exist in the game's database'."""
    from test_surface_contract import Gw as SGw  # noqa: PLC0415
    gw = SGw(write_reply={"success": False, "action": "cancel_intent", "reason": "not_found_or_already_consumed",
                          "intent_request_id": "si_12015_1700_22999"})
    env = run(R.do_impl(gw, intent="cancel_pending", args={"request_id": "si_12015_1700_22999"}))
    b = env["blocked"]
    assert env["verdict"] == "rejected" and "already ran" in b["why"] and "database" not in b["why"]
    assert b["next"] == {"tool": "inspect", "args": {"subject": "pending"}}


# ── defect 5: claim_gained in the wake and claims_gained in the news ──────────
#: a recorded game's control-session query_claims replies for the ruler 14577 (ids and types only), 889.01.04 (the
#: step's start) and 889.02.03 (its end): k_france 7052 arrived pressed when the claimant war took the title
CLAIMS_0104 = [(998, "unpressed"), (2189, "pressed"), (2256, "unpressed"), (2268, "unpressed"), (7096, "pressed"),
               (7179, "unpressed"), (7259, "pressed"), (7472, "unpressed"), (7480, "unpressed")]
CLAIMS_0203 = sorted(CLAIMS_0104 + [(7052, "pressed")])


def _claims(rows):
    return {"claims": [{"title_id": t, "type": ty, "title_name": str(t), "holder_id": 16710} for t, ty in rows]}


def test_5_the_cap_never_empties_the_claims_row_the_wake_fired_on():
    """The recorded disagreement: the step's news was over 8 KB and a cap with no MIN_ROWS could cut
    any list to 0 rows, its own `truncated` list too (the record shows `truncated.dropped` 0 of 5)."""
    big = {"ok": True, "result": {"news": {"claims_gained": [{"title_id": 7052, "pad": "x" * 300}],
                                           "purse": {"x": "y" * 9000}}, "pad": ["z" * 400] * 4}}
    out = RF.delivered(big, "observe", {})
    assert [c["title_id"] for c in out["result"]["news"]["claims_gained"]] == [7052]


def test_6_the_server_caps_what_leaves_it_after_the_prefix(monkeypatch):
    """A read the tool body fit to 8,192 B that the prefix takes over: the server's call_tool fits it again. RED on
    a call_tool that applies the prefix after the body's fit and fits nothing after it."""
    import asyncio  # noqa: PLC0415

    import mcp_server as M  # noqa: PLC0415
    import tool_prefix as TP  # noqa: PLC0415
    srv = M._StrictArgumentsFastMCP("t")
    rows = [{"target_name": f"t{i}", "next": {"tool": "plan_war", "args": {}}, "pad": "p" * 120} for i in range(80)]

    @srv.tool()
    def plan_war(goal_scoped: bool = False) -> dict:
        return RF.sent({"ok": True, "tool": "plan_war", "result": {"targets": rows}}, "plan_war")

    def payload(r):
        blocks = r[0] if isinstance(r, tuple) else r
        return json.loads(blocks[0].text)
    monkeypatch.setenv(TP.ENV, "mcp__ck3__")
    got = payload(asyncio.run(srv.call_tool("plan_war", {})))
    assert got["result"]["targets"][0]["next"]["tool"] == "mcp__ck3__plan_war"
    assert _delivered_size(got, "plan_war") <= RF.CAP, _delivered_size(got, "plan_war")
