"""Two replies that must agree with the game's own check, each driven by recorded game values (1091.03.09).

1. discover(what="acceptance") for call_ally_interaction checked the call without its war. The game's check, given
   no war, answered "No target selected" for both allies (sendable false, retry never, beside "yes" at 16 and 40);
   the send names the war, and the same check with the war refused one ally at -984 ("Will not accept") and took the
   other, who joined. The check now carries what the send names: the war (one row per war the send could name), and
   for an interaction whose send names what the check cannot take (a title, a marriage's pair) it says so.
2. The mercenary listing and the hire's refusal. The game's afford test (the hire command's CanExecute): gold
   minus the price at or above 0, or above -24 x the monthly income. A hire within that line that comes back
   refused (CanExecute 0) is not a hire the ruler cannot afford, and a row whose debt is past the line is not one
   "the game allows a hire into debt".
"""
import asyncio
import os
import re
from pathlib import Path

import pytest

import redesign as R
from test_ireland_walk_replies import Gw

HERE = Path(__file__).resolve().parent
BRIDGE = HERE / ".." / ".." / "bridge"
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")

ME, NORWAY, LORD, BREIFNE = 28112, 36813, 32420, 34580
WAR = 167772192
WAR_ROW = {"war_id": WAR, "name": "Munsterian Claim on the Earldom of Breifne", "primary_attacker": ME,
           "primary_defender": BREIFNE, "attackers": [ME], "defenders": [BREIFNE]}
#: the game's check without a war: the recorded discover replies' rows (refused_by, the game's words, his answer)
NO_WAR = {NORWAY: {"sendable": False, "refused_by": "send_conditions_not_met", "engine_reason": "No target selected",
                   "answer": "yes", "accept_score": 16.0},
          LORD: {"sendable": False, "refused_by": "send_conditions_not_met", "engine_reason": "No target selected",
                 "answer": "yes", "accept_score": 40.0}}
#: the same check with the war: the recorded send's game_check for Norway; the lord's call was sent and he joined
WITH_WAR = {NORWAY: {"sendable": False, "refused_by": "target_would_reject",
                     "engine_reason": "ai_accept -984.00: Will not accept", "answer": "no", "accept_score": -984.0},
            LORD: {"sendable": True, "answer": "yes", "accept_score": 40.0}}


def run(c):
    return asyncio.run(c)


def bridge_check(by_war=None):
    """query_interaction_check as the bridge answers it: a war-scoped interaction without war_id has no target."""
    by_war = by_war or {WAR: WITH_WAR}

    def answer(p):
        t = p.get("target_id") or (p.get("candidates") or [0])[0]
        table = by_war.get(p["war_id"]) if "war_id" in p else NO_WAR
        return {"success": True, "result": {"results": [dict(table[t], target_id=t)]}}
    return answer


def gw(wars=(WAR_ROW,), by_war=None):
    return Gw({"query_character": lambda p: {"id": p.get("character_id") or ME, "name": "Donnchad", "alive": True},
               "query_wars": {"wars": list(wars)}, "query_interaction_check": bridge_check(by_war)},
              date="1091.03.09")


def checks(g):
    return [p for a, p in g.reads if a == "query_interaction_check"]


# ── 1. the call to arms is checked with its war ─────────────────────────────────────────────────────────────────

def test_1_CONTROL_the_check_without_the_war_gives_the_recorded_wrong_verdict():
    """Non-discriminating (it calls the stand-in bridge directly): the gateway answers as the game did."""
    row = bridge_check()({"target_id": NORWAY})["result"]["results"][0]
    assert row["engine_reason"] == "No target selected" and row["accept_score"] == 16.0


def test_1_the_refused_ally_reads_his_refusal_not_no_target():
    g = gw()
    env = run(R.acceptance_impl(g, "call_ally_interaction", NORWAY, ME))
    assert [c.get("war_id") for c in checks(g)] == [WAR]
    res = env["result"]
    assert res["war_id"] == WAR and res["sendable"] is False and res["answer"] == "no"
    assert res["accept_score"] == -984.0 and res["would_accept"] is False
    assert res["refused"]["gate"] == "recipient_refuses" and "Will not accept" in res["refused"]["summary"]
    assert "No target selected" not in str(env)


def test_1_the_ally_who_joined_reads_sendable_with_the_send_that_names_the_war():
    g = gw()
    env = run(R.acceptance_impl(g, "call_ally_interaction", LORD, ME))
    res = env["result"]
    assert res["sendable"] is True and res["answer"] == "yes" and "refused" not in res
    assert res["do_call"] == {"tool": "do", "args": {"intent": "call_ally", "args": {"war_id": WAR, "ally_id": LORD}}}
    assert res["your_side"] == "attacker"


def test_1_several_wars_give_one_row_each_checked_with_its_own_war():
    other = dict(WAR_ROW, war_id=33, name="Second war", primary_attacker=999, primary_defender=ME,
                 attackers=[999], defenders=[ME])
    g = gw(wars=(WAR_ROW, other), by_war={WAR: WITH_WAR, 33: {NORWAY: dict(WITH_WAR[LORD])}})
    env = run(R.acceptance_impl(g, "call_ally_interaction", NORWAY, ME))
    res = env["result"]
    assert sorted(c["war_id"] for c in checks(g)) == [33, WAR]
    rows = {r["war_id"]: r for r in res["by_war"]}
    assert rows[WAR]["sendable"] is False and rows[WAR]["accept_score"] == -984.0
    assert rows[33]["sendable"] is True and rows[33]["your_side"] == "defender"
    assert rows[33]["do_call"]["args"]["args"] == {"war_id": 33, "ally_id": NORWAY}
    assert res["sendable"] is True and res["wars_checked"] == 2


def test_1_no_war_led_is_refused_and_nothing_is_checked():
    g = gw(wars=())
    env = run(R.acceptance_impl(g, "call_ally_interaction", LORD, ME))
    assert env["ok"] is False and env["blocked"]["gate"] == "war_not_found" and checks(g) == []
    led_by_other = dict(WAR_ROW, primary_attacker=999, attackers=[999, ME])
    g = gw(wars=(led_by_other,))
    env = run(R.acceptance_impl(g, "call_ally_interaction", LORD, ME))
    assert env["blocked"]["gate"] == "not_war_leader" and checks(g) == []


def test_1_join_war_is_checked_with_the_wars_its_recipient_leads():
    theirs = dict(WAR_ROW, war_id=77, primary_attacker=LORD, attackers=[LORD])
    g = gw(wars=(WAR_ROW, theirs), by_war={77: {LORD: dict(WITH_WAR[LORD])}})
    env = run(R.acceptance_impl(g, "join_war_interaction", LORD, ME))
    assert [c["war_id"] for c in checks(g)] == [77]
    assert env["result"]["do_call"]["args"] == {"intent": "join_war", "args": {"war_id": 77, "leader_id": LORD}}


def test_1_every_interaction_a_war_intent_sends_with_its_war_is_checked_with_it():
    """The census from the send path itself: each war intent driven to its send; every interaction sent with a
    war_id must be one acceptance checks with its war."""
    class Sent(Exception):
        pass

    class G(Gw):
        async def confirmed_write(self, action, params, **kw):
            self.writes.append((action, dict(params)))
            raise Sent

    sent = {}
    for intent, p, me in (("call_ally", {"war_id": WAR, "target_id": LORD}, ME),
                          ("join_war", {"war_id": WAR, "target_id": ME}, LORD),
                          ("make_peace", {"war_id": WAR, "terms": "white_peace"}, ME)):
        g = G({"query_wars": {"wars": [WAR_ROW]}, "query_character": {"id": me, "alive": True}})
        with pytest.raises(Sent):
            run(R._do_war_intent(g, intent, p, me, {"id": me}))
        (_, params), = g.writes
        sent[params["interaction_type"]] = "war_id" in params
    with_war = {k for k, v in sent.items() if v}
    assert with_war and with_war == set(R.WAR_SCOPED_INTERACTIONS)


# ── 1b. what the check cannot take is said beside its verdict ───────────────────────────────────────────────────

def _no_title(p):
    return {"result": {"results": [{"target_id": p["candidates"][0], "sendable": False,
                                    "refused_by": "interaction_not_valid_for_target",
                                    "engine_reason": "No target selected"}]}}


#: the four whose file declares a title target (the census below checks the toolkit's list against the files)
TITLED = ("buy_claim_interaction", "grant_titles_interaction", "hof_ask_for_claim_interaction",
          "revoke_title_interaction")


@pytest.mark.parametrize("name", TITLED)
def test_1b_a_title_interaction_says_its_check_ran_without_the_title(name):
    g = Gw({"query_interaction_check": _no_title, "query_character": {"id": ME, "alive": True}})
    res = run(R.acceptance_impl(g, name, LORD, ME))["result"]
    assert res["not_checked"] == R.TITLE_NOT_CHECKED
    assert res["refused"]["retry"] != "never" and res["refused"]["not_checked"] == R.TITLE_NOT_CHECKED


def test_1b_a_marriage_says_its_check_ran_without_the_pair():
    g = Gw({"query_interaction_check": _no_title, "query_character": {"id": ME, "alive": True}})
    res = run(R.acceptance_impl(g, R.MARRIAGE_INTERACTION, LORD, ME))["result"]
    assert res["not_checked"] == R.PAIR_NOT_CHECKED and res["refused"]["retry"] != "never"


def test_1b_CONTROL_an_interaction_that_names_nothing_more_keeps_the_games_verdict():
    g = Gw({"query_interaction_check": _no_title, "query_character": {"id": ME, "alive": True}})
    res = run(R.acceptance_impl(g, "sway_interaction", LORD, ME))["result"]
    assert "not_checked" not in res and res["refused"]["retry"] == "never"


def test_1b_the_title_list_is_every_checkable_interaction_whose_target_is_a_title():
    """The bridge's check sets no title (its handler fixes title_id at -1), and the send pipeline checks only the
    interactions it sends itself; of those, each whose file declares `target_type = title` is in the list."""
    src = (BRIDGE / "src" / "action_handlers.cpp").read_text(errors="replace")
    body = src[src.index("bool do_query_interaction_check("):]
    body = body[:body.index("\n}\n")]
    assert "intent.title_id = -1;" in body and '"title_id"' not in body
    d = GAME / "common" / "character_interactions"
    if not d.is_dir():
        pytest.skip(f"no game install at {GAME}")
    text = "\n".join(p.read_text(encoding="utf-8-sig", errors="replace") for p in sorted(d.glob("*.txt")))
    titled = set()
    for name in R.DIRECT_DISPATCH_INTERACTIONS:
        m = re.search(rf"(?m)^{re.escape(name)} = \{{(.*?)^\}}", text, re.S)
        if m and re.search(r"(?m)^\ttarget_type = title\b", m.group(1)):
            titled.add(name)
    assert titled == set(R.TITLE_SCOPED_INTERACTIONS) == set(TITLED)


# ── 2. the mercenary hire: the game's rule, and a refusal that is not the price ─────────────────────────────────

def test_2_the_afford_test_is_strictly_above_the_line_and_none_without_income():
    assert R.merc_debt_line(4.39) == -105.36
    assert R.merc_affordable(100.0, 0.0, 1.0 / 0.97) is False     # exactly at the line: the game refuses
    assert R.merc_affordable(10.0, 5.0, 0.0) is False              # no income: no debt allowed
    assert R.merc_affordable(10.0, 5.0, -3.0) is False
    assert R.merc_affordable(5.0, 10.0, -3.0) is True              # no debt: allowed whatever the income


def test_2_the_debt_note_says_whether_the_game_allows_this_hire():
    """The listing's rows (gold 236.67, income 4.39): company 157 at an upper price of 285.66 is inside the line;
    company 360 at 365.28 is past it, and its row said "the game allows a hire into debt"."""
    inside = R.merc_debt_after(285.66, 236.67, 4.39)
    assert inside["gold_after"] == -48.99 and inside["debt_line"] == -105.36
    assert inside["note"].startswith("the game allows this hire into debt")
    past = R.merc_debt_after(365.28, 236.67, 4.39)
    assert past["gold_after"] == -128.61 and past["note"].startswith("the game refuses this hire")
    assert "allows" not in past["note"].split(":")[0]
    unread = R.merc_debt_after(365.28, 236.67, None)
    assert "not known" in unread["note"] and unread["debt_line"] is None
    assert R.merc_affordable(285.66, 236.67, 4.39) is True and R.merc_affordable(365.28, 236.67, 4.39) is False


#: company 157 as the listing read it; light horsemen at 85 a sub-regiment and 4 counties give the recorded
#: estimate (251.38) and upper bound (285.66) for a duke
C157 = {"company_id": 157, "label": "Mercenary company from Ailech, 829 men", "troops": 829, "in_range": True,
        "same_culture": True, "distance_px": 181,
        "regiments": [{"maa_type": "levies", "men": 474}, {"maa_type": "light_horsemen", "men": 355}]}


def _hire_gw():
    return Gw({"query_hireable_mercenaries": {"companies": [C157]},
               "query_character": {"id": ME, "alive": True, "gold": 236.67, "ruler_tier": 3,
                                   "income_breakdown": {"total_monthly": 4.39}},
               "query_maa_types": {"types": [{"maa_type": "light_horsemen", "recruit_cost": 85.0}]},
               "query_realm": {"realm_total_counties": 4}}, date="1091.03.09")


def test_2_the_listing_row_is_the_recorded_one():
    rows, _far, _note = run(R.mercenary_rows(_hire_gw(), ME))
    (row,) = rows
    assert (row["price_estimate"], row["price_upper"], row["affordable"]) == (251.38, 285.66, True)
    assert row["debt_after_hire"]["gold_after"] == -48.99


def test_2_a_hire_refused_inside_the_rule_is_not_called_unaffordable():
    """The recorded reply of the hire of 157 with confirm_debt: gate cannot_afford, "cannot pay it now"."""
    env = {"verdict": "rejected", "ok": False, "blocked": R.map_raw_reason("rejected_or_unaffordable")}
    assert env["blocked"]["gate"] == "cannot_afford"
    run(R._explain_hire_refusal(_hire_gw(), env, 157, ME))
    b = env["blocked"]
    assert b["gate"] == "engine_refused" and b["summary"] == R.HIRE_REFUSED_UNREAD
    assert "cannot pay" not in str(b) and b["raw_reason"] == "rejected_or_unaffordable"
    assert b["gate_named"]["ruled_out"] == R.HIRE_RULED_OUT
    # the bridge's hire passes the whole ids, so the reply no longer names the id lookup as the open question
    assert "whole id" not in b["gate_named"]["not_read"] and "available" in b["gate_named"]["not_read"]
    assert b["measured"]["gold_after"] == -48.99 and b["measured"]["debt_line"] == -105.36


def test_2_CONTROL_a_hire_past_the_line_keeps_cannot_afford():
    g = _hire_gw()
    g.replies["query_character"] = {"id": ME, "alive": True, "gold": 150.0, "ruler_tier": 3,
                                    "income_breakdown": {"total_monthly": 4.39}}
    env = {"verdict": "rejected", "ok": False, "blocked": R.map_raw_reason("rejected_or_unaffordable")}
    run(R._explain_hire_refusal(g, env, 157, ME))
    assert env["blocked"]["gate"] == "cannot_afford"
    assert env["blocked"]["gate_named"]["gate"] == "HIRED_TROOPS_CANNOT_AFFORD"
