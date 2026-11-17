"""Lane peace: do(intent='end_war'), the script layer's peace, and the three
fixes beside it (the war-declaration arguments, the council task's character
target).

end_war ENDS a war the seat's character leads against a named enemy leader,
through the bridge's end_war template (bridge/include/script_templates.h): in
the war scope, `end_war = attacker | defender | white_peace`, the side resolved
in script. Its guards are the game's and are logged beside the write as the
token's marks (LEADS, OPEN_<TERMS>, SCORE_GE_<n>); the server turns them into
the reading. The terms are named from the ruler's side; the words that read
from either side are refused by name.

The fakes answer in the EMITTER's keys: script_action / query_script_log
(action_handlers.cpp do_script_action: guard, marks, read_only; do_query_script_log:
guard, marks), query_wars (query_handlers.cpp:4657 primary_attacker,
primary_defender), query_cb_validity_strict (query_handlers.cpp do_query_cb_validity_strict:
cb_name, target_mode "none"|"claim"|"title", declarable).
"""
import asyncio
import re
from pathlib import Path

import pytest

import redesign as R
from test_script_actions import Gw, ran, scripts_sent, ME, FOE, COURTIER, HEADER

WAR = 41


@pytest.fixture(autouse=True)
def _instant(monkeypatch):
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)


def run(c):
    return asyncio.run(c)


def _war(wid=WAR, atk=ME, dfn=FOE):
    return {"war_id": wid, "name": "Claim on Meath", "primary_attacker": atk, "primary_defender": dfn,
            "attackers": [atk], "defenders": [dfn]}


class WarGw(Gw):
    """Gw plus the war list (query_wars all/verbose) and the war-end ledger."""

    async def read(self, action, params=None):
        if action == "query_wars":
            self.reads.append((action, dict(params or {})))
            if self.state.get("wars_unread"):
                return {"success": False, "reason": "read_timeout"}
            return {"success": True, "wars": [dict(w) for w in self.state["war_list"]]}
        if action == "query_war_history":
            self.reads.append((action, dict(params or {})))
            return {"wars": [w for w in self.state.get("ended", []) if w["war_id"] == params.get("war_id")]}
        if action == "query_script_log" and "token" in (params or {}):
            r = await super().read(action, params)
            r["marks"] = self.state.get("late_marks", [])
            return r
        return await super().read(action, params)


def gw_with(reply, marks=(), wars=None, on_write=None, **state):
    st = {"war_list": [_war()] if wars is None else wars}
    st.update(state)
    if isinstance(reply, dict):
        reply = dict(reply, marks=list(marks))
    return WarGw(reply, state=st, on_write=on_write)


def end(gw, **args):
    return run(R.do_impl(gw, intent="end_war", character_id=ME, args=args))


# ── the terms ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("word", ["surrender", "victory", "enforce_demands", "Surrender"])
def test_a_word_that_reads_from_either_side_is_refused_by_name_and_nothing_is_sent(word):
    gw = gw_with(ran("end_war"))
    env = end(gw, enemy_id=FOE, terms=word)
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "invalid_argument", env
    assert env["blocked"]["raw_reason"] == "ambiguous_peace_terms"
    for t in ("we_win", "white_peace", "we_yield"):
        assert t in env["blocked"]["summary"], env["blocked"]["summary"]
    assert env["blocked"]["fix"]["accepted"] == ["we_win", "white_peace", "we_yield"]
    assert scripts_sent(gw) == []


@pytest.mark.parametrize("word", ["peace", "attacker", 3, ["we_win"]])
def test_any_other_word_is_refused_unknown_naming_the_three(word):
    gw = gw_with(ran("end_war"))
    env = end(gw, enemy_id=FOE, terms=word)
    assert env["blocked"]["raw_reason"] == "unknown_peace_terms", env
    assert "we_yield" in env["blocked"]["summary"] and scripts_sent(gw) == []


def test_the_terms_are_the_bridges():
    src = HEADER.read_text()
    m = re.search(r"kPeaceTerms\[\] = \{([^}]*)\}", src)
    assert m and re.findall(r'"([a-z_]+)"', m.group(1)) == list(R.END_WAR_TERMS)
    assert re.search(r'\{"end_war", Template::kEndWar\}', src)
    assert not set(R.END_WAR_AMBIGUOUS) & set(R.END_WAR_TERMS)


def test_an_enemy_is_required():
    gw = gw_with(ran("end_war"))
    env = end(gw, terms="we_yield")
    assert env["blocked"]["gate"] == "missing_required_argument" and scripts_sent(gw) == []
    assert env["blocked"]["fix"]["rejected_fields"] == ["enemy_id"]


# ── the write ────────────────────────────────────────────────────────────────

def test_we_win_goes_as_the_template_with_ids_and_terms_and_applies_when_the_war_is_gone():
    def ends(st, _p):
        st["war_list"] = []
        st["ended"] = [{"war_id": WAR, "outcome": "attacker_victory", "end_date": "867.01.05"}]
    gw = gw_with(ran("end_war", terms="we_win", read_only=False),
                 marks=["LEADS", "OPEN_WE_WIN", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD", "SCORE_GE_100"],
                 on_write=ends)
    env = end(gw, enemy_id=FOE, terms="we_win", script="add_gold = 1")
    assert scripts_sent(gw) == [{"actor_id": ME, "target_id": FOE, "terms": "we_win", "template": "end_war"}]
    assert env["verdict"] == "applied", env
    r = env["result"]
    assert r["readback"] == {"war_id": WAR, "gone_from_war_list": True, "outcome": "attacker_victory",
                             "end_date": "867.01.05"}
    assert r["path"] == "script" and r["terms"] == "we_win" and "YOUR SIDE WINS" in r["outcome_for_you"]
    assert r["reading"]["our_war_score"]["at_least"] == 100 and "not a negotiated offer" in r["what"]
    assert [a for a, _ in gw.writes] == ["script_action"], "no native send beside the script"


def test_a_closed_term_is_refused_with_the_reading_beside_it():
    gw = gw_with(ran("end_war", guard="refused", terms="we_win", read_only=False),
                 marks=["LEADS", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD", "SCORE_GE_25", "SCORE_GE_0",
                        "SCORE_GE_M25", "SCORE_GE_M50", "SCORE_GE_M100"])
    env = end(gw, enemy_id=FOE, terms="we_win")
    assert env["verdict"] == "rejected", env
    b = env["blocked"]
    assert b["gate"] == "peace_terms_not_open" and b["retryable"] is True
    assert b["class"] == "timing", "war score moves within a month: the refusal memo must not keep it"
    assert b["measured"]["open_terms"] == ["white_peace", "we_yield"]
    assert b["measured"]["our_war_score"]["at_least"] == 25 and b["measured"]["our_war_score"]["below"] == 50
    assert env["result"]["reading"] == b["measured"]


def test_no_war_led_against_that_enemy_is_war_not_found():
    gw = gw_with(ran("end_war", guard="refused", terms="we_yield", read_only=False), marks=["NO_WAR"], wars=[])
    env = end(gw, enemy_id=COURTIER, terms="we_yield")
    assert env["blocked"]["gate"] == "war_not_found"
    assert env["blocked"]["raw_reason"] == "no_war_led_against_enemy"
    assert env["blocked"]["measured"]["leads_war_against_enemy"] is False


def test_a_war_still_listed_after_the_guard_ran_is_pending_never_applied():
    gw = gw_with(ran("end_war", terms="we_yield", read_only=False), marks=["LEADS", "OPEN_WE_YIELD"])
    env = end(gw, enemy_id=FOE, terms="we_yield")
    assert env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "readback"


def test_a_war_the_server_could_not_read_before_is_pending_not_applied():
    def ends(st, _p):
        st["war_list"] = []
    gw = gw_with(ran("end_war", terms="white_peace", read_only=False), marks=["LEADS", "OPEN_WHITE_PEACE"],
                 on_write=ends, wars_unread=True)
    env = end(gw, enemy_id=FOE, terms="white_peace")
    assert env["verdict"] == "pending", env


def test_marks_the_logger_wrote_late_are_read_back():
    gw = gw_with(ran("end_war", guard="unseen", terms="we_win", read_only=False),
                 late_guard="refused", late_marks=["LEADS", "OPEN_WE_YIELD", "SCORE_GE_0"])
    env = end(gw, enemy_id=FOE, terms="we_win")
    assert any(a == "query_script_log" and p.get("token") == "SACT_9" for a, p in gw.reads)
    assert env["blocked"]["gate"] == "peace_terms_not_open", env
    assert env["blocked"]["measured"]["open_terms"] == ["we_yield"]
    assert env["result"]["marker_read_late"] is True


def test_an_unaddressable_leader_is_refused_and_never_sent_natively():
    gw = gw_with({"success": False, "action": "script_action", "reason": "target_not_script_addressable",
                  "detail": "target_id"})
    env = end(gw, enemy_id=COURTIER, terms="we_yield")
    assert env["blocked"]["gate"] == "target_ineligible", env
    assert [a for a, _ in gw.writes] == ["script_action"]


# ── the read-only form ───────────────────────────────────────────────────────

def test_no_terms_reads_the_war_the_score_and_the_open_terms():
    gw = gw_with(ran("end_war", read_only=True),
                 marks=["LEADS", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD", "SCORE_GE_50", "SCORE_GE_25", "SCORE_GE_0",
                        "SCORE_GE_M25", "SCORE_GE_M50", "SCORE_GE_M100"])
    env = end(gw, enemy_id=FOE)
    assert scripts_sent(gw) == [{"actor_id": ME, "target_id": FOE, "template": "end_war"}], "no terms sent"
    assert env["ok"] is True and env.get("verdict") not in ("applied", "rejected"), env
    r = env["result"]
    assert r["check"] is True and r["reading"]["leads_war_against_enemy"] is True
    assert r["reading"]["our_war_score"]["at_least"] == 50 and r["reading"]["our_war_score"]["below"] == 100
    assert set(r["ends_with"]) == {"white_peace", "we_yield"}
    assert r["wars_between_you"][0]["war_id"] == WAR


def test_check_true_with_terms_is_still_a_read():
    gw = gw_with(ran("end_war", read_only=True), marks=["LEADS", "OPEN_WE_YIELD"])
    env = end(gw, enemy_id=FOE, terms="we_win", check=True)
    assert "terms" not in scripts_sent(gw)[0] and env["result"]["check"] is True


def test_a_read_whose_marks_were_never_seen_is_a_read_failure():
    gw = gw_with(ran("end_war", guard="unseen", read_only=True))
    env = end(gw, enemy_id=FOE)
    assert env["ok"] is False and env["blocked"]["gate"] == "read_timeout", env


def test_end_war_is_an_intent_with_its_args_documented():
    import do_intent_args
    assert "end_war" in R.INTENTS
    assert set(do_intent_args.extract_dedicated()["end_war"]) >= {"enemy_id", "terms", "check"}
    assert "end_war" in R.SCRIPT_NOT_MOVED["make_peace"], "make_peace (the offer) names the script route"


# ── the native council task cannot carry a character target ─────────────────

def test_a_council_task_on_an_untitled_character_is_refused_not_sent_without_its_target():
    """lane peace: the script path cannot name a character without a title, and
    the native set_council_task drops target_character_id (tools.resolve_act)
    and the bridge never uses it. Red on 29c8e303: the native set_council_task
    was sent with no character target."""
    gw = Gw({"success": False, "action": "script_action", "reason": "target_not_script_addressable",
             "detail": "target_character_id"})
    env = run(R.do_impl(gw, intent="council_task", character_id=ME,
                        args={"task": "task_find_secrets", "target_character_id": COURTIER}))
    assert [a for a, _ in gw.writes] == ["script_action"], gw.writes
    assert env["verdict"] == "rejected" and env["blocked"]["raw_reason"] == "council_target_not_addressable"


def test_CONTROL_a_council_task_with_no_character_target_still_goes_native():
    gw = Gw({"success": False, "action": "script_action", "reason": "target_not_script_addressable",
             "detail": "actor"})
    run(R.do_impl(gw, intent="council_task", character_id=ME, args={"task": "task_domestic_affairs"}))
    assert [a for a, _ in gw.writes] == ["script_action", "set_council_task"]


# ── the war-declaration arguments ────────────────────────────────────────────

def _strict(*rows):
    return {"results": [dict(r) for r in rows]}


class StrictGw(Gw):
    def __init__(self, strict, **kw):
        super().__init__(**kw)
        self.strict = strict

    async def read(self, action, params=None):
        if action == "query_cb_validity_strict":
            self.reads.append((action, dict(params or {})))
            return self.strict
        return await super().read(action, params)


def test_a_casus_belli_group_name_is_refused_before_any_guard():
    """discover(what='war_targets') prints cb_type "claim" on its claim rows; a
    group name satisfies has_cb_on but never can_declare_war. Red on 29c8e303:
    the script was sent (and its guard could only say no)."""
    gw = StrictGw(_strict({"cb_name": "claim_cb", "target_mode": "claim", "declarable": True},
                          {"cb_name": "county_conquest_cb", "target_mode": "title", "declarable": False}),
                  script_reply=ran("declare_war"))
    env = run(R.declare_war_impl(gw, ME, target_id=FOE, cb="claim", cb_title_id=1141, confirm_straddle=True))
    assert scripts_sent(gw) == [], gw.writes
    assert env["blocked"]["gate"] == "invalid_argument"
    assert env["blocked"]["raw_reason"] == "cb_not_a_casus_belli_type"


def test_CONTROL_an_empty_strict_read_still_declares():
    """No rows read is not a reading that the name is unknown (refuse only on a
    read that lists the types)."""
    gw = StrictGw({"results": []}, script_reply=ran("declare_war"))
    run(R.declare_war_impl(gw, ME, target_id=FOE, cb="claim_cb", cb_title_id=1141, confirm_straddle=True))
    assert len(scripts_sent(gw)) == 1


def test_a_claim_targeted_cb_named_otherwise_gets_its_claimant():
    """norman_conquest_cb and norwegian_invasion_cb are claim-targeted
    (game/common/casus_belli_types/00_event_war.txt: target_titles = claim, 12
    scope:claimant reads each); the strict read says target_mode "claim". Red on
    29c8e303: claimant_id 0 (the name test is_claim_cb_name says no)."""
    gw = StrictGw(_strict({"cb_name": "norman_conquest_cb", "target_mode": "claim", "declarable": True}),
                  script_reply=ran("declare_war"))
    run(R.declare_war_impl(gw, ME, target_id=FOE, cb="norman_conquest_cb", cb_title_id=1141,
                           confirm_straddle=True))
    (p,) = scripts_sent(gw)
    assert p["claimant_id"] == ME, p


def test_CONTROL_a_title_cb_still_takes_no_claimant():
    gw = StrictGw(_strict({"cb_name": "duchy_conquest_cb", "target_mode": "title", "declarable": True}),
                  script_reply=ran("declare_war"))
    run(R.declare_war_impl(gw, ME, target_id=FOE, cb="duchy_conquest_cb", cb_title_id=1141,
                           claimant_id=ME, confirm_straddle=True))
    (p,) = scripts_sent(gw)
    assert p["claimant_id"] == 0
