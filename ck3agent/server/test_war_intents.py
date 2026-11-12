"""Lane l5 (F8): war completion from the player's seat.

Driven through the real redesign/tools functions with a scripted gateway, so a
row reds on what the function SENDS and RETURNS. Where the contract is with the
bridge (an interaction routed to the direct path, a war-end outcome spelling, a
param a handler reads), the row reads the bridge's own source: a literal written
only here could agree with itself.

  make_peace   the three terms from both seats map to the right end-war
               interaction and the right recorded outcome; the recipient is the
               enemy war leader; applied only on a war-end ledger row.
  call_ally /  the war rides as war_id; applied only when the joiner shows in
  join_war     the war's participant list.
  cbs          every row carries the bridge's family; by_family groups them.
  plan_war     `toward` reaches query_conquest_targets; strength and score come
               from reads; straddling is the declare_war age rule.
  dejure       query_dejure_progress has no default title.
"""
import asyncio
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

R = pytest.importorskip("redesign")
T = pytest.importorskip("tools")

BRIDGE = os.path.join(os.path.dirname(__file__), "..", "..", "bridge")


def _src(*parts):
    return open(os.path.join(BRIDGE, *parts), encoding="utf-8", errors="replace").read()


ME, ENEMY, ALLY, WAR = 100, 200, 300, 16777282


@pytest.fixture(autouse=True)
def _instant_waits(monkeypatch):
    """confirm_write's waits (the drain schedule, the world poll) take no time."""
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)


class WarGateway:
    """A scripted gateway: reads answer from `replies` (value or callable);
    confirmed_write records the dispatch and resolves it by the REAL
    redesign.confirm_write (the two waits) over this gateway's reads and clock.
    query_dispatch_result answers from the scripted query_intent_results row of
    the intent ("pending" while there is none), unless scripted itself."""

    def __init__(self, replies=None, send_reply=None):
        self.replies = dict(replies or {})
        self.send_reply = send_reply or {"success": True, "status": "queued",
                                         "intent_request_id": "si_100_1_1"}
        self.reads, self.writes = [], []
        self.health = "ok"
        self.paused = True

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character" and action not in self.replies:
            return {"id": (params or {}).get("character_id"), "alive": True,
                                  "gold": 50, "prestige": 300, "age": 30}
        if action == "query_dispatch_result" and action not in self.replies:
            rid = (params or {}).get("dispatch_id")
            done = self.replies.get("query_intent_results", {})
            done = done(params) if callable(done) else done
            row = next((r for r in (done or {}).get("results") or []
                        if r.get("intent_id") == rid), None)
            return dict(row, dispatch_id=rid) if row else {"dispatch_id": rid, "status": "pending"}
        if action == "get_tick_info" and action not in self.replies:
            return {"paused": self.paused, "game_date": "867.01.05", "tick_id": 1}
        r = self.replies.get(action, {})
        return r(params) if callable(r) else r

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        # Lane v4act: this write goes through the script layer first; these rows
        # grade the path it falls back to (a character with no title to name him by).
        if action == "script_action":
            return {"success": False, "action": "script_action", "reason": "target_not_script_addressable", "detail": "target_id"}
        return {"success": True}

    async def clock(self):
        return R.clock_of(await self.read("get_tick_info", {}))

    async def confirmed_write(self, action, params, *, readback, deadline_s=12.0, max_polls=8):
        async def send():
            self.writes.append((action, dict(params or {})))
            return self.send_reply
        return await R.confirm_write(self.clock, self.read, send, readback,
                                     deadline_s=deadline_s, max_polls=max_polls)

    def game(self):
        return {"tick": 1, "date": "867.01.05", "paused": self.paused}


def _run(coro):
    return asyncio.run(coro)


def _war_row(atk=ME, dfn=ENEMY, war_id=WAR, attackers=None, defenders=None):
    """One war as query_wars(all=true, verbose=true) emits it."""
    return {"war_id": war_id, "name": "War", "primary_attacker": atk, "primary_defender": dfn,
            "attackers": attackers or [atk], "defenders": defenders or [dfn]}


def _active(*rows):
    return {"scope": "all_active", "wars": list(rows)}


# ── make_peace ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("terms,side,interaction,outcome", [
    ("white_peace", "attacker", "end_war_attacker_white_peace_interaction", "white_peace"),
    ("white_peace", "defender", "end_war_attacker_white_peace_interaction", "white_peace"),
    ("surrender", "attacker", "end_war_attacker_defeat_interaction", "defender_victory"),
    ("surrender", "defender", "end_war_attacker_victory_interaction", "attacker_victory"),
    ("enforce_demands", "attacker", "end_war_attacker_victory_interaction", "attacker_victory"),
    ("enforce_demands", "defender", "end_war_attacker_defeat_interaction", "defender_victory"),
])
def test_make_peace_sends_the_terms_interaction_to_the_enemy_leader(terms, side, interaction, outcome):
    row = _war_row() if side == "attacker" else _war_row(atk=ENEMY, dfn=ME)
    ledger = {"success": True, "wars": [{"war_id": WAR, "outcome": outcome,
                                         "war_name": "War", "end_date": "867.1.6"}]}
    gw = WarGateway(replies={"query_wars": _active(row),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "success"}]},
                             "query_war_history": ledger})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": terms}))
    sent = [p for a, p in gw.writes if a == "send_interaction"]
    assert sent == [{"interaction_type": interaction, "actor_id": ME, "target_id": ENEMY}], sent
    assert env["verdict"] == "applied", env
    assert env["result"]["war_ended"]["outcome"] == outcome
    assert env["result"]["expected_outcome"] == outcome
    assert env["result"]["side"] == side
    assert "gold" in env["result"]["before"] and "gold" in env["result"]["after"]
    assert not [w for w in env["warnings"] if w.get("code") == "war_ended_otherwise"]


@pytest.mark.parametrize("terms", ["surrender", "white_peace", "enforce_demands"])
def test_only_a_surrender_says_it_is_your_defeat(terms):
    """L18-C: six budget-pilot seats surrendered believing it free (Eung-ryeom e2/s4
    0869.05.30, "surrendering costs me nothing"; the reply's before/after then read
    prestige 456.3 -> 306.3). The surrender reply says it is a defeat with a cost."""
    outcome = {"surrender": "defender_victory", "white_peace": "white_peace",
               "enforce_demands": "attacker_victory"}[terms]
    ledger = {"success": True, "wars": [{"war_id": WAR, "outcome": outcome,
                                         "war_name": "War", "end_date": "867.1.6"}]}
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "success"}]},
                             "query_war_history": ledger})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": terms}))
    codes = [w.get("code") for w in env["warnings"]]
    assert ("surrender_is_your_defeat" in codes) is (terms == "surrender"), codes


def test_make_peace_is_not_applied_without_a_ledger_row():
    """A dispatched interaction is not a peace: no war-end row, war still listed ⇒ pending."""
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "success"}]},
                             "query_war_history": {"success": True, "wars": []}})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "surrender"}))
    assert env["verdict"] == "pending", env
    assert env["pending"]["intent_id"] == "si_100_1_1" and "poll_token" not in env["pending"]
    assert "timeout_s" not in env["pending"]


def test_make_peace_refusal_is_the_recipients_and_is_named():
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "rejected",
                                  "rejection_reason": "target_would_reject"}]}})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "white_peace"}))
    assert env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "recipient_refuses", env["blocked"]
    assert env["blocked"]["retryable"] is True


def test_make_peace_invalid_interaction_is_named():
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "rejected",
                                  "rejection_reason": "interaction_not_valid_for_target"}]}})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "enforce_demands"}))
    assert env["blocked"]["gate"] == "interaction_invalid", env["blocked"]


def test_make_peace_refused_on_a_war_the_game_already_ended_says_so():
    """m-l12-gl5-r1: the war ended in the tick the enforce was queued; the send was
    refused (no casus belli left). The player is told the war had ended."""
    ended = {"success": True, "wars": [{"war_id": WAR, "outcome": "defender_victory",
                                        "end_date": "0868.09.27", "is_ended": True}]}
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "rejected",
                                  "rejection_reason": "interaction_not_valid_for_target"}]},
                             "query_war_history": ended})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "enforce_demands"}))
    assert env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "war_not_found", env["blocked"]
    assert "defender_victory" in env["blocked"]["summary"], env["blocked"]


def test_make_peace_needs_the_war_leader():
    gw = WarGateway(replies={"query_wars": _active(_war_row(atk=ALLY, dfn=ENEMY,
                                                             attackers=[ALLY, ME]))})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "white_peace"}))
    assert env["blocked"]["gate"] == "not_war_leader"
    assert env["blocked"]["measured"] == {"attacker_id": ALLY, "defender_id": ENEMY}
    assert not [a for a, _ in gw.writes]


def test_make_peace_unknown_war_and_bad_terms():
    gw = WarGateway(replies={"query_wars": _active(_war_row(war_id=5))})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "white_peace"}))
    assert env["blocked"]["gate"] == "war_not_found"
    gw = WarGateway(replies={"query_wars": _active(_war_row(atk=ALLY, dfn=ENEMY))})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "white_peace"}))
    assert env["blocked"]["gate"] == "war_not_found"   # a war ME is not in
    gw = WarGateway(replies={"query_wars": _active(_war_row())})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "truce"}))
    assert env["blocked"]["gate"] == "invalid_argument"
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME, args={"terms": "surrender"}))
    assert env["blocked"]["gate"] == "missing_required_argument"


def test_make_peace_warns_when_two_wars_share_the_pair():
    gw = WarGateway(replies={"query_wars": _active(_war_row(), _war_row(war_id=7, atk=ENEMY, dfn=ME)),
                             "query_intent_results": {"results": []},
                             "query_war_history": {"success": True, "wars": [
                                 {"war_id": WAR, "outcome": "white_peace"}]}})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "white_peace"}))
    assert [w for w in env["warnings"] if w.get("code") == "several_wars_with_this_enemy"]


def test_peace_interactions_take_the_bridge_direct_path():
    """The bridge sends them with the explicit recipient (is_direct_dispatch_interaction)."""
    seg = _src("src", "ai_eval_hook.cpp")
    m = re.search(r"static bool is_direct_dispatch_interaction\(.*?nullptr\s*\};", seg, re.S)
    assert m, "the direct-dispatch list moved"
    listed = set(re.findall(r'"([a-z_]+)"', m.group(0)))
    for inter in set(R.PEACE_OUTCOME) | {"call_ally_interaction", "join_war_interaction"}:
        assert inter in listed, inter


def test_peace_outcome_spellings_are_the_war_history_hooks():
    names = set(re.findall(r'return "([a-z_]+)";', _src("src", "war_history.cpp")))
    assert set(R.PEACE_OUTCOME.values()) <= names, (R.PEACE_OUTCOME, names)
    assert set(R.PEACE_TERMS) == {"white_peace", "surrender", "enforce_demands"}
    for by_side in R.PEACE_TERMS.values():
        assert set(by_side) == {"attacker", "defender"}
        assert set(by_side.values()) <= set(R.PEACE_OUTCOME)


# ── call_ally / join_war ──────────────────────────────────────────────────────

def test_call_ally_carries_the_war_and_is_applied_on_the_participant_list():
    gw = WarGateway(replies={"query_wars": _active(_war_row(attackers=[ME, ALLY])),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "success"}]}})
    env = _run(R.do_impl(gw, intent="call_ally", character_id=ME,
                         args={"war_id": WAR, "ally_id": ALLY}))
    sent = [p for a, p in gw.writes if a == "send_interaction"]
    assert sent == [{"interaction_type": "call_ally_interaction", "actor_id": ME,
                     "target_id": ALLY, "war_id": WAR}], sent
    assert env["verdict"] == "applied", env
    assert env["result"]["joined_id"] == ALLY
    assert ALLY in env["result"]["participants"]["attackers"]


def test_call_ally_not_in_the_list_after_the_drain_is_DECLINED_at_once():
    """call_ally's answer shows on the dispatch pass (LEVERS
    "on_execute"): the drain ran it and the ally is not on the war after that
    pass, so the ally said no. Rejected recipient_refuses, with no day waited
    and nothing touching the clock."""
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "success"}]}})
    gw.paused = False                       # running: still no world poll for it
    env = _run(R.do_impl(gw, intent="call_ally", character_id=ME,
                         args={"war_id": WAR, "ally_id": ALLY}))
    assert env["verdict"] == "rejected", env
    assert env["blocked"]["gate"] == "recipient_refuses", env["blocked"]
    assert "no reason text" in env["blocked"]["summary"], env["blocked"]
    after_send = [a for a, _ in gw.reads][[a for a, _ in gw.reads].index("query_intent_results"):]
    assert after_send.count("query_wars") == 1 + len(R.DRAIN_SCHEDULE_S), gw.reads
    assert not {a for a, _ in gw.writes} & {"pause_game", "unpause_game", "set_speed"}


@pytest.mark.parametrize("paused", [True, False])
def test_white_peace_is_answered_over_DAYS_reread_only_while_the_clock_runs(paused):
    """white_peace is a "days" lever (l13ff2a-p: all 31 sent offers ended
    white_peace 3-10 days later). Once the drain sent it, the ledger is re-read
    for game days only while a FRESH clock reading says the game runs (bounded);
    paused it is pending until the clock advances, never a refusal."""
    stage1 = 1 + len(R.DRAIN_SCHEDULE_S)
    seen = {"n": 0}

    def ledger(_p):
        seen["n"] += 1
        ended = seen["n"] > stage1           # the answer comes after stage 1: days
        return {"success": True, "wars": [{"war_id": WAR, "outcome": "white_peace",
                                           "war_name": "War", "end_date": "867.1.12"}]
                if ended else []}
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "success"}]},
                             "query_war_history": ledger})
    gw.paused = paused
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "white_peace"}))
    if paused:
        assert env["verdict"] == "pending", env
        assert env["pending"]["drains_when"] == "clock_advances", env["pending"]
    else:
        assert env["verdict"] == "applied", env
        assert seen["n"] > stage1
    assert not {a for a, _ in gw.writes} & {"pause_game", "unpause_game", "set_speed"}


def test_every_war_lever_names_when_its_answer_shows():
    """One table: every (intent, terms) _do_war_intent can send has a row."""
    keys = {("make_peace", t) for t in R.PEACE_TERMS} | {("call_ally", ""), ("join_war", "")}
    for k in keys:
        assert k in R.LEVERS, k
        assert R.lever_of(k[0], sub=k[1])[0] == k
    assert "PROVEN FOR ACCEPT ONLY" in R.LEVERS[("call_ally", "")]["evidence"]


def test_call_ally_is_applied_PAUSED_when_the_ally_shows_after_the_drain():
    """The gl5-p reply shape ("status": "queued", "pending_count": 1). The verdict
    appears on the 2nd drain read with the game paused, and here the ally shows on
    the war after it: the outcome is visible, so it is applied paused (stage 1).
    The pre-fix server read once at once and answered pending.

    This row grades the readback, not the game: per the bridge, a send's
    "success" means the interaction was dispatched, and an AI's acceptance of a
    call to arms normally comes days later. That case is stage 2 (pending
    drains_when "clock_advances" paused, re-read only while the clock runs):
    test_call_ally_rereads_the_war_for_days_only_while_the_clock_runs. A peace
    ends the war when it executes, so it resolves in stage 1 in both clock
    states: test_surrender_applies_once_the_drain_ran_it_in_EITHER_clock_state."""
    drained = {"n": 0}
    rid = "si_12627_31_39007"

    def dispatch(p):
        drained["n"] += 1
        return {"dispatch_id": rid, "status": "success" if drained["n"] >= 2 else "pending"}

    def intents(_p):
        return {"results": [{"intent_id": rid, "status": "success"}] if drained["n"] >= 2 else []}

    def wars(_p):
        return _active(_war_row(attackers=[ME, ALLY] if drained["n"] >= 2 else [ME]))
    gw = WarGateway(replies={"query_wars": wars, "query_intent_results": intents,
                             "query_dispatch_result": dispatch},
                    send_reply={"success": True, "status": "queued", "action": "send_interaction",
                                "method": "intent_dispatch", "intent_request_id": rid,
                                "pending_count": 1})
    gw.paused = True
    env = _run(R.do_impl(gw, intent="call_ally", character_id=ME,
                         args={"war_id": WAR, "ally_id": ALLY}))
    assert env["verdict"] == "applied", env
    assert ALLY in env["result"]["participants"]["attackers"]
    assert not {a for a, _ in gw.writes} & {"pause_game", "unpause_game", "set_speed"}


@pytest.mark.parametrize("paused", [True, False])
def test_surrender_applies_once_the_drain_ran_it_in_EITHER_clock_state(paused):
    """l5_war_defender, both launches of gl5 (p and r): surrender on the defender
    seat answered pending in 0.256 s with drains_when "clock_advances" while the
    game ran at speed 1 (the frozen clock cache, W1) and while it was paused (D5
    as written). The drain runs the queued interaction on the next pass and the
    war-end ledger has the war: applied in both."""
    drained = {"n": 0}

    def dispatch(p):
        drained["n"] += 1
        return {"dispatch_id": "si_100_1_1",
                "status": "success" if drained["n"] >= 2 else "pending"}

    def ledger(_p):
        return {"success": True, "wars": [{"war_id": WAR, "outcome": "attacker_victory",
                                           "war_name": "War", "end_date": "867.1.5"}]
                if drained["n"] >= 2 else []}
    gw = WarGateway(replies={"query_wars": _active(_war_row(atk=ENEMY, dfn=ME)),
                             "query_intent_results": {"results": []},
                             "query_dispatch_result": dispatch,
                             "query_war_history": ledger},
                    send_reply={"status": "queued", "method": "intent_dispatch",
                                "intent_request_id": "si_100_1_1", "pending_count": 1})
    gw.paused = paused
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": WAR, "terms": "surrender"}))
    assert env["verdict"] == "applied", env
    assert env["result"]["war_ended"]["outcome"] == "attacker_victory"


def test_a_verdict_never_read_is_pending_on_the_NEXT_PASS_not_the_clock():
    """Stage 1 ran out with the intent still queued: that is a different wait from
    game days, and the pending says so (drains_when "next_pass", the intent id)."""
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": []}})
    env = _run(R.do_impl(gw, intent="call_ally", character_id=ME,
                         args={"war_id": WAR, "ally_id": ALLY}))
    assert env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "next_pass", env["pending"]
    assert env["pending"]["intent_id"] == "si_100_1_1"
    reads = [a for a, _ in gw.reads]
    assert reads.count("query_dispatch_result") == 1 + len(R.DRAIN_SCHEDULE_S), reads


def test_join_war_finds_the_leader_among_allies_and_joins_his_side():
    state = {"n": 0}

    def wars(p):
        state["n"] += 1
        joined = state["n"] > 1           # after the send, ME is on the defenders' side
        return _active(_war_row(atk=ENEMY, dfn=ALLY,
                                defenders=[ALLY, ME] if joined else [ALLY]))
    gw = WarGateway(replies={"query_wars": wars,
                             "query_alliances": {"alliances": [{"ally_id": ALLY}]},
                             "query_intent_results": {"results": [
                                 {"intent_id": "si_100_1_1", "status": "success"}]}})
    env = _run(R.do_impl(gw, intent="join_war", character_id=ME, args={"war_id": WAR}))
    sent = [p for a, p in gw.writes if a == "send_interaction"]
    assert sent == [{"interaction_type": "join_war_interaction", "actor_id": ME,
                     "target_id": ALLY, "war_id": WAR}], sent
    assert env["verdict"] == "applied" and env["result"]["side"] == "defender", env


def test_wars_are_found_in_the_global_list_with_all_true():
    gw = WarGateway(replies={"query_wars": _active(_war_row()),
                             "query_intent_results": {"results": []},
                             "query_war_history": {"success": True, "wars": [
                                 {"war_id": WAR, "outcome": "white_peace"}]}})
    _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                   args={"war_id": WAR, "terms": "white_peace"}))
    q = [p for a, p in gw.reads if a == "query_wars"]
    assert q and q[0] == {"all": True, "verbose": True}, q
    src = _src("src", "query_handlers.cpp")
    seg = src[src.index("bool do_query_wars("):]
    seg = seg[:seg.index("\nbool ", 10)]
    assert 'json_get_bool(params_json, "all", false)' in seg


def test_war_id_reaches_send_interaction_under_the_key_the_bridge_reads():
    act = R._intent_to_act_params("interact", {"interaction": "call_ally_interaction",
                                               "target_id": ALLY, "war_id": WAR}, ME)
    action, params = T.resolve_act(act)
    assert action == "send_interaction" and params["war_id"] == WAR
    src = _src("src", "action_handlers.cpp")
    seg = src[src.index("bool do_send_interaction("):]
    seg = seg[:seg.index("\nbool ", 10)]
    assert 'json_get_int(params_json, "war_id")' in seg
    assert "target_war_id" in seg


def test_the_war_target_scope_is_written_from_the_header():
    hook = _src("src", "ai_eval_hook.cpp")
    assert "interaction_ctx::TARGET_SCOPE_TYPE" in hook
    assert "interaction_ctx::SCOPE_TYPE_WAR" in hook
    hdr = open(os.path.join(BRIDGE, "..", "re", "offsets", "ck3_offsets.h")).read()
    assert re.search(r"SCOPE_TYPE_WAR\s*=\s*0x10;", hdr)
    assert re.search(r"TARGET_SCOPE_TYPE\s*=\s*0x2F0;", hdr)
    # one name for the offset: the refuted "bool array" name is not a constant
    assert not re.search(r"constexpr\s+\w+\s+EVAL_FLAGS_PTR\b", hdr)


def test_the_windows_build_measures_the_target_scope_under_its_one_name(tmp_path):
    """The Windows ck3_offsets.h grades TARGET_SCOPE_TYPE "measured" from the
    Windows table's own entry, and carries no second name for 0x2F0. A public
    export ships that header (ck3_offsets.generated.h); a research tree generates
    it as bridge/CMakeLists.txt does."""
    import subprocess
    root = os.path.join(BRIDGE, "..")
    win_dir = os.path.join(root, "re", "offsets", "windows")
    shipped = os.path.join(win_dir, "ck3_offsets.generated.h")
    if os.path.exists(shipped):
        gen = open(shipped).read()
    else:
        probes = os.path.join(root, "re", "findings", "windows")
        out = tmp_path / "ck3_offsets.h"
        subprocess.run([sys.executable, "-I", os.path.join(root, "re", "scripts", "windows", "gen_win_offsets.py"),
                        "--linux", os.path.join(root, "re", "offsets", "ck3_offsets.h"),
                        "--windows", os.path.join(win_dir, "ck3_offsets_win.h"),
                        "--probe", os.path.join(probes, "p3_probe_867_pair.json"),
                        "--probe", os.path.join(probes, "p3_probe_867_07_09_pair.json"),
                        "--recipes", os.path.join(BRIDGE, "src", "os", "windows", "recipes.inc"),
                        "--out", str(out), "--report", str(tmp_path / "report.json")],
                       check=True, capture_output=True)
        gen = out.read_text()
    ns = gen[gen.index("namespace interaction_ctx {"):]
    ns = ns[:ns.index("\n}")]
    row = re.search(r"constexpr size_t TARGET_SCOPE_TYPE\s*=\s*0x2F0;.*", ns)
    assert row and "win:measured struct_layout::INTERACTION_CTX_TARGET_SCOPE_TYPE" in row.group(0)
    assert not re.search(r"constexpr\s+\w+\s+EVAL_FLAGS_PTR\b", ns)
    assert "EVAL_FLAGS_PTR" not in open(os.path.join(win_dir, "ck3_offsets_win.h")).read()


# ── cbs ────────────────────────────────────────────────────────────────────────

def test_cbs_carries_family_and_groups_valid_options():
    raw = {"status": "ok", "target_state": {}, "cbs": [
        {"cb_key": "invasion_war", "family": "invasion", "valid": True, "target_mode": 1},
        {"cb_key": "county_conquest_cb", "family": "conquest", "valid": False, "target_mode": 1},
        {"cb_key": "claim_cb", "family": "claim", "valid": True, "target_mode": 6},
    ]}
    gw = WarGateway(replies={"query_cbs_for_target": raw})
    env = _run(R.cbs_impl(gw, target_id=ENEMY, character_id=ME))
    res = env["result"]
    assert {o["cb"]: o["family"] for o in res["options"]} == {
        "invasion_war": "invasion", "county_conquest_cb": "conquest", "claim_cb": "claim"}
    assert res["by_family"] == {"claim": ["claim_cb"], "conquest": [], "invasion": ["invasion_war"]}


def test_bridge_cbs_view_names_the_invasion_family():
    src = _src("src", "query_handlers.cpp")
    fn = src[src.index("static const char* cb_family(const char* n) {"):]
    fn = fn[:fn.index("\n}\n")]
    for needle in ('"invasion"', '"adventur"', '"holy_war"', '"claim"', '"conquest"', '"raid"'):
        assert needle in fn, needle
    assert "cb_is_winline_family" not in src


def test_query_wars_places_a_siege_by_the_one_attribution_rule():
    # witness #4 (blocker 2): a siege was listed under every war whose attackers
    # included the besieger, with that war's primary defender written in. The rule
    # is siege_attribution.h (host test bridge/test/test_siege_attribution.cpp).
    src = _src("src", "query_handlers.cpp")
    fn = src[src.index("static void emit_war_sieges("):]
    fn = fn[:fn.index("\n}\n")]
    assert "siege_attr::war_side_of_siege(" in fn
    assert "PRIMARY_DEF_REF" not in fn, "the war's primary defender is not the siege's defender"
    assert '"holding_title_id"' in fn and '"besieger_side"' in fn
    assert "province_siege_link::SIEGE_REF_0x368" in src
    cm = open(os.path.join(BRIDGE, "CMakeLists.txt")).read()
    assert "test_siege_attribution" in cm.split("enable_testing()")[1]


def test_query_wars_names_the_cb_family_by_the_one_definition():
    src = _src("src", "query_handlers.cpp")
    entry = src[src.index("static void emit_war_entry("):]
    entry = entry[:entry.index("\n}\n")]
    assert 'r.key("cb_family"); r.value_string(cb_family(' in entry


def test_war_score_is_the_engines_sum_in_points():
    """ComputeTotalWarScore (re_war.md §8): points, defender terms subtracted, clamp
    +-100. The old sum added them, read fp/1000 and divided: every total was 0."""
    hdr = _src("include", "bridge.h")
    assert "return b.imprisonment_atk - b.imprisonment_def;" in hdr
    assert "return b.occupation_atk - b.occupation_def;" in hdr
    hook = _src("src", "war_score_hook.cpp")
    assert "bd.total = std::max(-100, std::min(100, raw_sum));" in hook
    assert "clamped / 1000" not in hook and "0x1738" not in hook.split("ComputeTotalWarScore")[-1]
    for f in ("query_handlers.cpp", "observation.cpp"):
        src = _src("src", f)
        assert not re.search(r"_atk \+ \w+\.\w+_def\) / 1000", src), f


def _fn_body(src, signature):
    fn = src[src.index(signature):]
    return fn[:fn.index("\n}\n")]


def test_cbs_bridge_query_is_a_game_thread_read_that_services_its_own_slot():
    """L13 FF1-B (witness W6): query_cbs_for_target(s) and query_cb_validity_strict
    read the pair before they submit, so they are game-state reads, run by the drain
    on the game thread, the one thread that services g_cb_query. The failure
    live867 l5-r1 recorded (queued to the game thread, a cbs poll waited on the slot
    it could only service itself; every cbs answered cb_query_busy) stays excluded
    because every submit services the slot ITSELF on that thread before it polls,
    and the poll does not sleep there. A false fix (moving them back to the IPC
    thread, or dropping the self-service) reds one of the rows below."""
    snap = _src("src", "snapshot.cpp")
    internal = snap[snap.index("bool is_bridge_internal_query("):snap.index("bool is_game_state_query(")]
    state = _fn_body(snap, "bool is_game_state_query(")
    for action in ("query_cbs_for_target", "query_cbs_for_targets", "query_cb_validity_strict"):
        assert f'"{action}",' not in internal, action          # not on the IPC thread
        assert f'"{action}"' in state, action                  # a game-thread read
    hook = _src("src", "ai_eval_hook.cpp")
    for sig in ("int cb_query_count_valid_sync(", "int cb_query_count_valid_strict(",
                "int cb_query_strict_full_sync("):
        fn = _fn_body(hook, sig)
        pub = fn.index("g_cb_query.state.store(1, std::memory_order_release);")
        poll = fn.index("while (waited < timeout_ms)")
        assert pub < fn.index("service_slots_if_game_thread();", pub) < poll, sig
        assert "slot_poll_wait();" in fn[poll:] and "sleep_for" not in fn[poll:], sig
    wait = _fn_body(hook, "void slot_poll_wait() {")
    assert wait.index("if (bridge::on_game_thread()) return;") < wait.index("sleep_for")


def test_cb_slot_is_serviced_inline_from_the_game_thread_drain():
    """query_conquest_targets is a game-state read, run by the drain on the game
    thread (the session thread since L13), the one thread that services the
    g_cb_query slot; before that its engine checks waited out the timeout on
    themselves (live867 l5-r3: engine_checks_done 2, no holder checked). Since L13
    FF1-B every slot submit calls service_slots_if_game_thread(): on the drain
    thread it runs the slot processing (service_query_slots, process_cb_query among
    it) under the caller's saved fault recovery point. The predicate is the drain
    thread's own identity, not a flag a caller sets around the dispatch."""
    hook = _src("src", "ai_eval_hook.cpp")
    fn = _fn_body(hook, "int cb_query_strict_full_sync(")
    pub = fn.index("g_cb_query.state.store(1, std::memory_order_release);")
    inline = fn.index("service_slots_if_game_thread();", pub)
    assert pub < inline < fn.index("while (waited < timeout_ms)")
    svc = _fn_body(hook, "void service_slots_if_game_thread() {")
    assert svc.index("if (!bridge::on_game_thread()) return;") \
        < svc.index("memcpy(&saved_point, &g_recovery_point") \
        < svc.index("service_query_slots();") \
        < svc.index("memcpy(&g_recovery_point, &saved_point")
    assert "process_cb_query();" in _fn_body(hook, "void service_query_slots() {")
    disp = _src("src", "intent_dispatch.cpp")
    assert ("bool on_game_thread() { return s_game_thread.load(std::memory_order_relaxed)"
            " == std::this_thread::get_id(); }") in disp
    for f in ("bridge_init.cpp", "ai_eval_hook.cpp"):
        assert "g_cb_slot_service_inline" not in _src("src", f), f


# ── plan_war ──────────────────────────────────────────────────────────────────

class PlanGateway(WarGateway):
    pass


def _plan_gw(age=30, cbs_valid=True):
    def character(p):
        return {"id": p.get("character_id"), "alive": True, "age": age,
                              "stress": 0, "prestige": 10}
    conquest = {"duchies": [{"duchy_key": "d_x", "remaining_counties": [
        {"county_id": 11, "county_key": "c_x", "holder_id": ENEMY, "holder_top_liege_id": ENEMY,
         "holder_name": "E", "adjacent": True, "conquerable_now": True,
         "declare": {"casus_belli": "county_conquest_cb", "target_id": ENEMY, "cb_title_id": 11}}]}]}
    strict = {"status": "ok", "results": [{"cb_name": "county_conquest_cb", "declarable": True,
                                           "outer_status": "pass", "inner_status": "pass"}]}
    cbs = {"status": "ok", "target_state": {}, "cbs": [
        {"cb_key": "county_conquest_cb", "family": "conquest", "valid": cbs_valid, "target_mode": 1,
         "scope": "county", "cost_prestige": 100, "cost_piety": 0, "affordable": False}]}
    alliances = {ME: {"own_military_strength": 3000, "total_callable_strength": 4000},
                 ENEMY: {"own_military_strength": 1000, "total_callable_strength": 2000}}
    return PlanGateway(replies={
        "query_character": character,
        "query_valid_war_targets": {"targets": []},
        "query_conquest_targets": conquest,
        "query_cb_validity_strict": strict,
        "query_cbs_for_target": cbs,
        "query_cbs_for_targets": {"error": "unknown_action"},
        "query_wars": _active(),
        "query_war_stance": {"error": "unknown_action"},
        "query_alliances": lambda p: alliances.get(p.get("character_id"), {}),
    })


def test_plan_war_toward_reaches_conquest_targets_and_rows_come_from_reads():
    gw = _plan_gw()
    env = _run(R.plan_war_impl(gw, character_id=ME, toward="k_france"))
    ct = [p for a, p in gw.reads if a == "query_conquest_targets"]
    assert ct and ct[0]["toward"] == "k_france" and ct[0]["include_expansion"] is False, ct
    row = env["result"]["targets"][0]
    assert "win_prob" not in row
    assert row["strength"] == {"mine": 3000, "mine_with_allies": 4000, "theirs": 1000,
                               "theirs_with_allies": 2000, "ratio": 2.0}
    assert row["score"] == 2.0
    assert row["affordability"]["estimate"] is True
    assert row["affordability"]["cost_prestige"] == 100
    assert row["ready_to_declare"] is True    # cost is an estimate; it does not gate the row


def _unresolved_county(**extra):
    return {"duchies": [{"duchy_key": "d_x", "remaining_counties": [dict(
        {"county_id": 12, "county_key": "c_y", "holder_id": 777, "holder_name": "V",
         "adjacent": True, "conquerable_now": False,
         "block_reason": "holder_top_liege_unresolved"}, **extra)]}]}


def test_plan_war_an_unresolved_top_liege_is_no_target():
    """L15 A3. The bridge writes holder_top_liege_unresolved and no
    holder_top_liege_id when the holder's liege walk cannot finish
    (query_handlers.cpp do_query_conquest_targets, liege_walk.h). RED on
    55a66b0c: plan_war fell back to holder_id and probed the vassal 777 as the
    war's target. Now the county is excluded, unmeasured, and nobody is probed."""
    gw = _plan_gw()
    gw.replies["query_conquest_targets"] = _unresolved_county(holder_top_liege_unresolved=True)
    res = _run(R.plan_war_impl(gw, character_id=ME))["result"]
    assert all(_as_int_or_none(t.get("target_id")) != 777 for t in res["targets"])
    probed = [p for a, p in gw.reads if a in ("query_cb_validity_strict", "query_cbs_for_target")]
    assert not any(777 in p.values() for p in probed), probed
    ex = [e for e in res["excluded"]
          if (e.get("blocked") or {}).get("raw_reason") == "holder_top_liege_unresolved"]
    assert len(ex) == 1 and ex[0]["county_id"] == 12 and ex[0]["holder_id"] == 777
    assert ex[0]["blocked"]["gate"] == "gate_introspection_unavailable"


def test_plan_war_CONTROL_a_resolved_top_liege_is_the_target():
    """The same county with its top liege measured (ENEMY): the war's target is
    ENEMY, never the holder; nothing is excluded as unresolved."""
    gw = _plan_gw()
    gw.replies["query_conquest_targets"] = _unresolved_county(
        holder_top_liege_id=ENEMY, holder_top_liege_independent=True, block_reason="")
    res = _run(R.plan_war_impl(gw, character_id=ME))["result"]
    probed = [p for a, p in gw.reads if a in ("query_cb_validity_strict", "query_cbs_for_target")]
    assert probed and not any(777 in p.values() for p in probed), probed
    assert any(ENEMY in p.values() for p in probed), probed
    assert not [e for e in res["excluded"]
                if (e.get("blocked") or {}).get("raw_reason") == "holder_top_liege_unresolved"]


def _as_int_or_none(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def test_plan_war_goal_scoped_false_asks_for_expansion():
    gw = _plan_gw()
    _run(R.plan_war_impl(gw, character_id=ME, goal_scoped=False))
    ct = [p for a, p in gw.reads if a == "query_conquest_targets"]
    assert ct[0]["include_expansion"] is True and "toward" not in ct[0]


def test_plan_war_straddle_is_the_declare_war_age_rule():
    gw = _plan_gw(age=R.STRADDLE_AGE)
    env = _run(R.plan_war_impl(gw, character_id=ME))
    assert env["result"]["targets"] == []
    assert env["result"]["excluded"][0]["blocked"]["gate"] == "succession_straddle"
    env = _run(R.plan_war_impl(_plan_gw(age=R.STRADDLE_AGE), character_id=ME,
                               exclude_straddling=False))
    assert env["result"]["targets"][0]["straddles_succession"] is True
    assert "death_eta_years" not in env["result"]["lifecycle"]["succession"]
    src = open(R.__file__).read()
    assert "age >= STRADDLE_AGE" in src[src.index("async def declare_war_impl("):]


def test_conquest_targets_domain_maps_title_to_toward():
    action, params = T.resolve_query("conquest_targets", {"character_id": ME, "title_id": 42})
    assert action == "query_conquest_targets" and params["toward_title_id"] == 42
    src = _src("src", "query_handlers.cpp")
    seg = src[src.index("bool do_query_conquest_targets("):]
    seg = seg[:seg.index("\nbool ", 10)]
    assert 'json_get_int(params_json, "toward_title_id")' in seg
    assert 'json_get_string(params_json, "toward")' in seg
    for gone in ("k_england", "k_scotland", "e_britannia", "british"):
        assert gone not in seg.lower(), gone


def test_dejure_progress_has_no_default_title():
    src = _src("src", "query_handlers.cpp")
    seg = src[src.index("bool do_query_dejure_progress("):]
    seg = seg[:seg.index("\nbool ", 10)]
    assert "e_britannia" not in seg
    assert '"title_required"' in seg


# ── war 0 is a war ────────────────────────────────────────────────────────────

def test_war_zero_is_a_war_not_an_absent_argument():
    """The engine numbers wars from 0 (the 867 save's war 0 is the Aghlabid
    Conquest of Sicily); 0 must reach the bridge as a war id."""
    gw = WarGateway(replies={"query_wars": _active(_war_row(war_id=0, atk=ENEMY, dfn=ME)),
                             "query_intent_results": {"results": []},
                             "query_war_history": {"success": True, "wars": [
                                 {"war_id": 0, "outcome": "white_peace"}]}})
    env = _run(R.do_impl(gw, intent="make_peace", character_id=ME,
                         args={"war_id": 0, "terms": "white_peace"}))
    assert env["verdict"] == "applied", env
    act = R._intent_to_act_params("interact", {"interaction": "call_ally_interaction",
                                               "target_id": ALLY, "war_id": 0}, ME)
    assert T.resolve_act(act)[1]["war_id"] == 0
    act = R._intent_to_act_params("interact", {"interaction": "gift_interaction",
                                               "target_id": ALLY}, ME)
    assert "war_id" not in T.resolve_act(act)[1]


def test_the_bridge_reads_war_ref_zero():
    """A war-ref array entry 0 is war 0; no per-character war read may skip it."""
    for f in ("query_handlers.cpp", "action_handlers.cpp"):
        assert "war_ref == 0 ||" not in _src("src", f), f
    seg = _src("src", "action_handlers.cpp")
    seg = seg[seg.index("bool do_send_interaction("):]
    assert 'json_has_key(params_json, "war_id")' in seg[:seg.index("\nbool ", 10)]


def test_plan_war_never_offers_a_claim_war_the_strict_row_calls_undeclarable():
    """The strict row's `declarable` decides: claim_cb passes the engine's inner check
    with no pressed claim, and the war is refused claim_not_pressed (live867 l5-r10:
    13495 held no pressed claim and plan_war offered two claim_cb wars)."""
    gw = _plan_gw()
    gw.replies["query_conquest_targets"] = {"duchies": []}
    gw.replies["query_valid_war_targets"] = {"targets": [
        {"target_id": ENEMY, "target_name": "E", "cb_type": "claim_cb", "title_id": 1113,
         "claimant_id": ME, "pressed": False}]}
    gw.replies["query_cb_validity_strict"] = {"status": "ok", "results": [
        {"cb_name": "claim_cb", "declarable": False, "cull_reason": "claim_not_pressed",
         "outer_status": "pass", "inner_status": "pass", "target_mode": 6}]}
    gw.replies["query_cbs_for_target"] = {"status": "ok", "target_state": {}, "cbs": [
        {"cb_key": "claim_cb", "family": "claim", "valid": False, "target_mode": 6,
         "cull_reason": "claim_not_pressed", "scope": "county"}]}
    env = _run(R.plan_war_impl(gw, character_id=ME))
    cbs = [(t.get("declare_args") or {}).get("cb") for t in env["result"].get("targets") or []]
    assert "claim_cb" not in cbs, env["result"]


# ── muster is gone (owner ruling 2026-10-21) ──────────────────────────────────

def test_muster_and_the_raise_actions_are_gone():
    # The raise primitive raised nothing live (re_raise_armies.md, "Refuted"); the
    # intent, its readback and both bridge actions were deleted.
    assert "muster" not in R.INTENTS
    gw = WarGateway()
    env = _run(R.do_impl(gw, intent="muster", character_id=ME))
    assert env["ok"] is False and (env.get("blocked") or {}).get("gate") == "unknown_verb", env
    assert not any(a in ("raise_armies", "raise_raiders") for a, _ in gw.writes)
    reg = _src("src", "action_registry.inc")
    assert 'BRIDGE_ACTION("raise_armies"' not in reg and 'BRIDGE_ACTION("raise_raiders"' not in reg
    assert "RAISE_ARMIES" not in _src("include", "intent_queue.h").split("// 7 and 8 were")[0]


def test_every_name_the_signature_generator_lists_is_an_offset_header_name():
    # witness #4 (blocker 5): AutoGenerateSignatures' built-in table still named
    # ARMY_RAISE_ALL and ARMY_RAISE_ALL_WRAPPER after the header deleted them, and
    # INTERACTION_COMPUTE_ACCEPT, a name the header never had (its 0x06b441c0 is
    # FUN_06b441c0, the header's GET_AI_WILL_DO_SCORE).
    root = os.path.join(BRIDGE, "..")
    java = open(os.path.join(root, "re", "scripts", "utility", "AutoGenerateSignatures.java")).read()
    table = java[java.index("ALL_FUNCS = {"):]
    table = table[:table.index("};")]
    names = re.findall(r'\{0x[0-9a-fA-F]+L,\s*"(\w+)"\}', table)
    assert len(names) > 300, len(names)
    hdr = open(os.path.join(root, "re", "offsets", "ck3_offsets.h")).read()
    declared = set(re.findall(r"constexpr\s+\w+\s+(\w+)\s*=", hdr))
    missing = [n for n in names if n not in declared]
    assert not missing, missing
    assert len(names) == len(set(names)), "a name is listed twice"


# ── force_offensive_stance is gone (owner ruling 2026-10-22) ──────────────────

def test_force_offensive_stance_is_gone_end_to_end():
    # The engine's own coordinator already held attacker_offensive when it ran
    # (live867 l5fx6-p2-live); the game's AI prosecutes a declared war on its own.
    assert "force_offensive_stance" not in R.INTENTS
    assert "force_offensive_stance" not in R._EXPERIMENTAL_FLAGS
    gw = WarGateway()
    env = _run(R.do_impl(gw, intent="force_offensive_stance", character_id=ME,
                         args={"war_id": WAR, "attacker_id": ME}))
    assert env["ok"] is False and (env.get("blocked") or {}).get("gate") == "unknown_verb", env
    assert gw.writes == []
    reg = _src("src", "action_registry.inc")
    assert "force_offensive_stance" not in reg
    for rel in (("src", "intent_dispatch.cpp"), ("src", "ai_eval_hook.cpp"), ("src", "snapshot.cpp"),
                ("src", "action_handlers.cpp"), ("src", "dispatch_router.cpp"),
                ("include", "action_dispatch_internal.h")):
        assert "force_offensive_stance" not in _src(*rel), rel
    assert "WAR_FORCE_OFFENSIVE_STANCE =" not in _src("include", "intent_queue.h")
    hdr = open(os.path.join(BRIDGE, "..", "re", "offsets", "ck3_offsets.h")).read()
    for name in ("COORD_STANCE_TICK", "COORD_OBJ_BOOTSTRAP", "REC_STRENGTH_ENEMY",
                 "REC_STRENGTH_OWN", "OFFENSIVE_RATIO_HI", "REC_OBJ_COUNT",
                 "REC_REEVAL_CD_1", "REC_REEVAL_CD_2"):
        assert not re.search(rf"constexpr\s+\w+\s+{name}\b", hdr), name


# ── set_phase: the read names the phase the engine holds ───────────────────────

def _pers(genome):
    return {"success": True, "personality": dict(genome, dominance=0), "engine": dict(genome)}


def test_set_phase_read_names_the_phase_the_engine_holds():
    gw = WarGateway(replies={"get_ai_personality": _pers(R.PHASE_GENOME["expansion"])})
    env = _run(R.set_phase_impl(gw, phase=None, character_id=ME))
    assert env["result"]["phase"] == "expansion"
    assert env["result"]["phase_read_from"] == "engine"


def test_set_phase_read_is_none_when_no_phase_matches():
    g = dict(R.PHASE_GENOME["consolidate"], honor=100)       # a phase with an override
    gw = WarGateway(replies={"get_ai_personality": _pers(g)})
    assert _run(R.set_phase_impl(gw, phase=None, character_id=ME))["result"]["phase"] is None
    # the lock map alone is not the engine: an empty engine read names no phase
    gw = WarGateway(replies={"get_ai_personality": {"personality": dict(R.PHASE_GENOME["expansion"]),
                                                    "engine": {}}})
    assert _run(R.set_phase_impl(gw, phase=None, character_id=ME))["result"]["phase"] is None


def test_every_phase_genome_is_distinct_so_the_read_can_name_it():
    for name, genome in R.PHASE_GENOME.items():
        assert R.phase_of_genome(_pers(genome)) == name


def test_the_phase_is_named_from_engine_axes_only():
    # witness #4 (minor 7): the lock map (the write's own record) does not
    # decide the name; the bridge's lock-only dominance is in no genome (L14).
    g = R.PHASE_GENOME["expansion"]
    assert "dominance" not in {k for gen in R.PHASE_GENOME.values() for k in gen}
    pers = _pers(g)
    pers["personality"]["boldness"] = -99            # the write's record disagrees
    assert R.phase_of_genome(pers) == "expansion"
    gw = WarGateway(replies={"get_ai_personality": pers})
    res = _run(R.set_phase_impl(gw, phase=None, character_id=ME))["result"]
    assert res["phase"] == "expansion" and "axes_not_in_engine" not in res
    # an engine axis that disagrees still un-names the phase
    pers = _pers(dict(g, boldness=g["boldness"] - 1))
    assert R.phase_of_genome(pers) is None


# ── a game-thread handler never waits for its own intent ───────────────────────

def _game_thread_handlers():
    """(action, handler) for every is_game_thread_action the registry maps."""
    snap = _src("src", "snapshot.cpp")
    body = snap[snap.index("bool is_game_thread_action("):]
    body = body[:body.index("\n}\n")]
    acts = re.findall(r'^\s*"(\w+)",', body, re.M)
    reg = dict(re.findall(r'BRIDGE_ACTION\("(\w+)",\s*(\w+)\)', _src("src", "action_registry.inc")))
    return [(a, reg[a]) for a in acts if a in reg]


def test_game_thread_handlers_do_not_wait_for_their_own_intent():
    src = _src("src", "action_handlers.cpp")
    pairs = _game_thread_handlers()
    assert len(pairs) >= 15
    polled = []
    for action, fn in pairs:
        start = src.find(f"\nbool {fn}(")
        if start < 0:
            continue
        end = src.find("\nbool ", start + 10)
        body = src[start:end if end > 0 else len(src)]
        if "completed_snapshot" not in body:
            continue
        polled.append(action)
        loops = re.findall(r"for \(int poll = 0;[^)]*\)", body)
        assert loops and all("on_game_thread()" in l for l in loops), (action, loops)
    # create_title no longer polls at all (L14 FB m-5: nothing reaches it off
    # the game thread, so its poll and its 12 s reply were deleted).
    assert "call_war_create_core" in polled and "create_title" not in polled, polled
    assert "s_game_thread" in _src("src", "intent_dispatch.cpp")
    assert "bridge::note_game_thread();" in _src("src", "bridge_init.cpp")


def test_the_option_selection_has_one_name():
    hdr = open(os.path.join(BRIDGE, "..", "re", "offsets", "ck3_offsets.h")).read()
    for alias in ("EVAL_FLAGS_2", "EVAL_SLOT_COUNT"):
        assert not re.search(rf"constexpr\s+\w+\s+{alias}\b", hdr), alias
    q = _src("src", "query_handlers.cpp")
    assert "interaction_ctx::OPTION_SELECTION_PTR" in q and "interaction_ctx::OPTION_SELECTION_COUNT" in q


# ── the drain's verdict is read by the HANDLER's intent id, never the client's ─
#
# create_title and call_war_create_core run on the game thread and queue their
# own intent (crtitle_..., the war's id), replying at once with success:false
# and that id under intent_request_id / dispatch_id. The drain also records the
# handler's immediate reply under the CLIENT's request_id, where it reads
# "rejected / rejected_by_handler" although the intent then executes. A server
# that read that record would call a minted title or a declared war refused.

CLIENT_RID = "8f0c2a4e-client-uuid"


def _dispatch_by_id(own_id, seen):
    """query_dispatch_result: the misleading client-id record, and the handler's
    own intent, which the drain ran."""
    def answer(p):
        rid = (p or {}).get("dispatch_id")
        seen.append(rid)
        if rid == CLIENT_RID:
            return {"dispatch_id": rid, "status": "rejected",
                    "rejection_reason": "rejected_by_handler"}
        if rid == own_id:
            return {"dispatch_id": rid, "status": "success", "action_name": "create_title"}
        return {"dispatch_id": rid, "status": "not_found"}
    return answer


def test_mint_title_reads_the_crtitle_intent_not_the_client_request_id():
    own = "crtitle_12627_31_4242"
    seen: list = []
    held = lambda: own in seen                                  # noqa: E731
    gw = WarGateway(replies={
        "query_dispatch_result": _dispatch_by_id(own, seen),
        # the title's own row (query_handlers.cpp do_query_title_succession, 12126):
        # current_holder_id only once held; the reply names the title it queued
        "query_title_succession": lambda p: ({"action": "query_title_succession", "title_id": p["title_id"],
                                              "title_key": "d_york", "tier": 3, "current_holder_id": ME}
                                             if held() else
                                             {"action": "query_title_succession", "title_id": p["title_id"],
                                              "title_key": "d_york", "tier": 3}),
        "query_dejure_progress": {"held": 0, "total": 0}},
        send_reply={"request_id": CLIENT_RID, "success": False, "action": "create_title",
                    "actor_id": ME, "title_id": 4242, "title_key": "d_york",
                    "status": "queued", "intent_request_id": own,
                    "dispatch_id": own,
                    "reason": "no_poll_on_game_thread", "drains_when": "next_pass"})
    env = _run(R._confirm_intent_write(gw, "mint_title", "create_title",
                                       {"actor_id": ME, "title_key": "d_york"},
                                       {"title_key": "d_york"}, ME, {"id": ME}, []))
    assert env["verdict"] == "applied", env
    assert CLIENT_RID not in seen and own in seen, seen


def test_the_steered_war_reads_its_own_intent_not_the_client_request_id():
    own = "warcore_12627_31_77"
    seen: list = []
    war = {"war_id": WAR, "attacker_id": ME, "defender_id": ENEMY}
    gw = WarGateway(replies={"query_dispatch_result": _dispatch_by_id(own, seen)},
                    send_reply={"request_id": CLIENT_RID, "success": False,
                                "action": "call_war_create_core", "status": "timeout",
                                "reason": "intent_not_completed_within_5s",
                                "intent_request_id": own, "dispatch_id": own})

    async def landed(_raw):
        return war if own in seen else None
    out = _run(gw.confirmed_write("call_war_create_core", {"actor_id": ME}, readback=landed))
    assert out["resolved"] == war and out["refused"] is None, out
    assert out["intent_id"] == own and CLIENT_RID not in seen, (out, seen)


def test_declare_war_pending_is_dos_spelling_and_asserts_no_reason_it_was_not_given():
    """L14 FF1 m-4: declare_war's pending is ok:true, verdict pending, no blocked
    (do()'s spelling), and a reply that named no reason carries none (no
    "no_poll_on_game_thread" default)."""
    gw = WarGateway(replies={"query_wars": _active(),
                             "get_active_wars": {"wars": []},
                             "query_valid_cbs": {"cbs": [{"name": "claim_cb", "valid": True}]}},
                    send_reply={"status": "queued", "intent_request_id": "warcore_1_1_1"})
    env = _run(R.declare_war_impl(gw, ME, target_id=ENEMY, cb="claim_cb", confirm_straddle=True))
    assert env.get("verdict") == "pending", env
    assert env["ok"] is True and not env.get("blocked"), env
    assert "raw_reason" not in env["pending"] or env["pending"]["raw_reason"] == "queued", env
    assert "no_poll_on_game_thread" not in str(env), env


@pytest.mark.parametrize("occupation,native,expected", [
    (0, None, True),                                      # inferred non-sieging attacker
    (40, None, False),                                    # inferred sieging attacker
    (0, {"stance": "defensive", "will_siege": False}, False),  # the engine's stance read
])
def test_an_inferred_stalled_attack_says_no_lever_moves_it(occupation, native, expected):
    """L18-A: the engine's offensive stance is a strength gate (1.333x, constants;
    personality does not enter), so a war row inferred `attacker_not_sieging` carries
    the typed `war_stalled_no_lever` limitation naming the one lever there is (allies).
    Only the inferred row: an engine_stance_db row is the engine's own word."""
    class GW:
        async def read(self, verb, params):
            if verb == "get_active_wars":
                return {"wars": [{"war_id": WAR, "name": "W", "is_attacker": True,
                                  "attacker_id": ME, "defender_id": ENEMY, "war_score": 0}]}
            if verb == "query_war_score_breakdown":
                return {"occupation_atk": occupation}
            if verb == "query_war_stance":
                return {"wars": [dict(native, war_id=WAR)]} if native else {}
            return {}

        def game(self):
            return {"tick": 1}

    R._WAR_STANCE_UNSERVED.clear()
    row = asyncio.run(R._war_stance_view(GW(), ME))["wars"][0]
    codes = [lim["code"] for lim in row["limitations"]]
    assert ("war_stalled_no_lever" in codes) is expected, (row["stance"], codes)
    if expected:
        assert row["stance_source"] == "inferred_from_occupation"
        assert "1.33" in row["limitations"][0]["detail"] and "call_ally" in row["limitations"][0]["detail"]


def test_a_stance_read_for_the_other_side_is_not_shown_as_this_sides():
    """L22 (smoke19 A3): at 0868.01.09 Charles, war 13's primary attacker with
    occupation 8, got `side attacker, stance defender_defensive` from
    inspect(war_stance): the bridge's row was the defender's coordinator record.
    A native row whose is_attacker disagrees with the war row is not used."""
    class GW:
        async def read(self, verb, params):
            if verb == "get_active_wars":
                return {"wars": [{"war_id": WAR, "name": "W", "is_attacker": True,
                                  "attacker_id": ME, "defender_id": ENEMY, "war_score": 10}]}
            if verb == "query_war_score_breakdown":
                return {"occupation_atk": 8}
            if verb == "query_war_stance":
                return {"wars": [{"war_id": WAR, "is_attacker": False,
                                  "stance": "defender_defensive", "will_siege": False}]}
            return {}

        def game(self):
            return {"tick": 1}

    R._WAR_STANCE_UNSERVED.clear()
    row = asyncio.run(R._war_stance_view(GW(), ME))["wars"][0]
    assert row["side"] == "attacker", row
    assert row["stance"] == "offensive_sieging", row
    assert row["stance_source"] == "inferred_from_occupation", row
    assert "stance_side_mismatch" in [lim["code"] for lim in row["limitations"]], row
