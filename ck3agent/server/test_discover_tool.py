"""discover(): behaviour against a fake bridge (cleanup 2026.11 F15).

Rows:
  * every `what` resolve_discover maps reaches its bridge action, on the path its
    kind needs: those whose bridge action is a deferred-drain read
    (mcp_server.DEFERRED_DRAIN_READS, the one copy of is_deferred_drain_action)
    go out as actions with the deferred budget, the game-thread reads through
    the gateway with no clock call around them, the rest as reads;
  * every parameter of the registered discover schema reaches the bridge (or the
    shaped verdict) for a `what` that reads it, with a control call without it;
    a census row reds when the schema gains a parameter this table lacks;
  * every refusal is named: missing_required_argument, unknown_subject,
    unknown_verb, read_timeout (an exception or a deferred read the bridge never
    answered), bridge_disconnected, and a reply the bridge declined;
  * what=targets: the single and pool+annotate plans, the 40-row cap, a row the
    bridge has not named yet, an annotation that failed;
  * what=interaction_validity: the declare_war CB verdict and the trigger cascade;
  * what=decision_why_blocked: a decision it does not model, the danelaw duchy
    clause reported unmeasured when the title read fails, and its duchy count
    read by the keys the bridge writes (held_titles, key, tier,
    de_jure_parent_key): only k_england's duchies counted, an unreadable one
    unmeasured.
"""

import asyncio
import re
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402
import tools as T  # noqa: E402

DEFERRED = {w for w in T._DISCOVER_RESOLVED
            if (T.resolve_discover({"what": w}) or ("",))[0] in ms.DEFERRED_DRAIN_READS}
#: Game-thread reads that go through the gateway (lane l6: construction_check runs
#: the validity under the random scope).
GATEWAY = {w for w in T._DISCOVER_RESOLVED
           if (T.resolve_discover({"what": w}) or ("",))[0] in ms.BridgeGateway._GAME_THREAD_READS}


def test_the_game_thread_reads_are_the_ones_lane_l6_moved():
    # fabricate_targets left discover in L14 (finding 8): do(action_class=...) owns it.
    assert GATEWAY == {"construction_check"}


def run(coro):
    return asyncio.run(coro)


def _defs():
    return {t["name"]: t for t in ms.registered_tool_definitions()}


class Bridge:
    def __init__(self, state):
        self.state, self.replies, self.sent = state, {}, []
        self.clock_reads = 0

    def answer(self, path, action, params, timeout):
        if action == "get_tick_info":     # the clock reading every tool takes at entry (m-16)
            self.clock_reads += 1
            return {"paused": True, "game_speed": 1}
        self.sent.append((path, action, dict(params), timeout))
        r = self.replies.get(action, {"action": action})
        if callable(r):
            r = r(params)
        if isinstance(r, BaseException):
            raise r
        return r

    def params_of(self, action):
        return [p for _, a, p, _ in self.sent if a == action]


@pytest.fixture
def bridge(monkeypatch):
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 12, "867.1.9"
    b = Bridge(state)
    # scheme_targets reads its verdict off a success reply (the check starts nothing)
    b.replies["scheme_action"] = {"success": True, "result": {"scheme": "murder", "targets": []}}
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def qb(_s, action, params, timeout=15.0):
        return b.answer("query", action, params, timeout)

    async def ab(_s, action, params, timeout=5.0):
        return b.answer("action", action, params, timeout)

    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_action_bridge", ab)

    # A ruler is bound: the unbound answer (redesign.unbound_refusal, one read of
    # query_character) is test_no_ruler_bound.py's subject; these rows count the
    # sends of the tool's own reads.
    async def bound(_gw, _tool, _action, _params):
        return None
    monkeypatch.setattr(R, "unbound_refusal", bound)
    return b


def refused(env, gate):
    assert env["ok"] is False and env["tool"] == "discover", env
    b = env["blocked"]
    assert b["gate"] == gate and b["gate"] in R.GATE_TABLE, b
    assert b["summary"]
    return b


# ── every what ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("what", T._DISCOVER_RESOLVED)
def test_every_resolved_what_reaches_its_action_on_the_right_path(bridge, what):
    expect_action = T.resolve_discover({"what": what})[0]
    kw = {}
    if what in ("decisions_annotated", "valid_decisions"):
        # L18-B: these evaluate the decisions named (none named is a missing
        # argument) and answer an evaluation (valid_only / considered present)
        kw["decisions"] = ["found_kingdom_decision"]
        bridge.replies[expect_action] = {"valid_only": what == "valid_decisions",
                                         "considered": 1, "count": 0, "decisions": []}
    env = run(ms.discover(what=what, **kw))
    assert env["ok"] is True, env
    ((path, action, _, timeout),) = bridge.sent       # one call, no clock verb around it
    assert action == expect_action
    if what in GATEWAY or what in DEFERRED:
        assert (path, timeout) == ("action", ms.DEFERRED_READ_TIMEOUT_S), \
            "a game-thread query takes the action path"
    else:
        assert path == "query"


def test_the_deferred_whats_come_from_the_one_definition():
    """discover keeps no list of its own: the five whats that queue work onto the
    game thread are found through DEFERRED_DRAIN_READS (the control: the set is
    not empty and holds them)."""
    assert {"acceptance", "valid_cbs", "valid_cbs_strict", "trigger_result",
            "interaction_validity"} <= DEFERRED
    src = Path(ms.__file__).read_text(encoding="utf-8")
    body = src[src.index("async def discover("):]
    body = body[:body.index("\n@mcp.tool()")] if "\n@mcp.tool()" in body else body
    assert "is_deferred = what in (" not in body
    assert "action in DEFERRED_DRAIN_READS" in body


# ── every parameter ───────────────────────────────────────────────────────────

#: parameter -> (what, kwargs, the bridge action, what it must receive)
PARAMS = {
    "interaction_type": ("acceptance", {"interaction_type": "befriend_interaction"},
                         "query_interaction_acceptance",
                         {"interaction_type": "befriend_interaction"}),
    "target_id": ("acceptance", {"target_id": 4242}, "query_interaction_acceptance",
                  {"target_id": 4242}),
    "actor_id": ("valid_cbs", {"actor_id": 30853}, "query_valid_cbs", {"actor_id": 30853}),
    "interaction": ("interaction_validity", {"interaction": "befriend"},
                    "query_trigger_result", {"interaction_type": "befriend_interaction"}),
    "action": ("targets", {"action": "war", "actor_id": 3}, "query_valid_war_targets",
               {"actor_id": 3}),
    "title_id": ("targets", {"action": "claim", "title_id": 55}, "query_title_claimants",
                 {"title_id": 55}),
    "building": ("construction_check", {"building": "castle_02"}, "construction_check",
                 {"building": "castle_02"}),
    "location_id": ("construction_check", {"location_id": 991}, "construction_check",
                    {"location_id": 991}),
    "slot": ("construction_check", {"slot": 2}, "construction_check", {"slot": 2}),
    "max_results": ("scheme_targets", {"scheme": "murder", "max_results": 5,
                                       "character_id": 30853},
                    "scheme_action", {"action": "check", "max_results": 5}),
    "character_id": ("decisions", {"character_id": 812}, "list_decisions",
                     {"character_id": 812}),
    "decisions": ("decisions_annotated", {"decisions": ["found_kingdom_decision"]},
                  "list_decisions_annotated", {"decisions": ["found_kingdom_decision"]}),
    # what=scheme_targets (cleanup L3): the scheme key and an explicit candidate list
    # reach the engine's start check (scheme_action, action=check); nothing starts.
    "scheme": ("scheme_targets", {"scheme": "murder", "target_id": 4242, "character_id": 30853},
               "scheme_action", {"action": "check", "scheme": "murder", "target_id": 4242,
                                 "actor_id": 30853}),
    "candidates": ("scheme_targets", {"scheme": "murder", "candidates": [11, 12],
                                      "character_id": 30853},
                   "scheme_action", {"action": "check", "candidates": [11, 12],
                                     "actor_id": 30853}),
}

#: the two parameters that shape the verdict instead of the request
SHAPING = {"cb", "title", "decision_type"}


def test_every_discover_parameter_has_a_row():
    props = set(_defs()["discover"]["input_schema"]["properties"])
    assert len(props) > 10, "the schema read found too few parameters (fail OPEN)"
    want = set(PARAMS) | SHAPING | {"what"}
    assert props == want, sorted(props ^ want)


@pytest.mark.parametrize("name", sorted(PARAMS))
def test_each_parameter_reaches_the_bridge(bridge, name):
    what, kwargs, action, expect = PARAMS[name]
    if action == "list_decisions_annotated":
        # L18-B: an annotated reply is an evaluation (enumerate_decisions_annotated
        # writes valid_only and considered); anything else is refused not_evaluated.
        bridge.replies[action] = {"action": "list_decisions", "character_id": 812,
                                  "valid_only": False, "considered": 1, "count": 1,
                                  "decisions": [{"name": "found_kingdom_decision",
                                                 "hash": 1, "available": False}]}
    env = run(ms.discover(what=what, **kwargs))
    assert env["ok"] is True, env
    (sent,) = bridge.params_of(action)
    assert expect.items() <= sent.items(), (name, sent)


@pytest.mark.parametrize("name", sorted(set(PARAMS) - {"action", "title_id"}))
def test_control_without_the_parameter_the_value_is_not_sent(bridge, name):
    what, kwargs, action, expect = PARAMS[name]
    run(ms.discover(what=what, **{k: v for k, v in kwargs.items() if k != name}))
    sent = bridge.params_of(action)
    assert not sent or not expect.items() <= sent[0].items(), (name, sent)


def test_control_targets_without_action_or_title_reaches_nothing(bridge):
    refused(run(ms.discover(what="targets")), "missing_required_argument")
    refused(run(ms.discover(what="targets", action="claim")), "missing_required_argument")
    assert bridge.sent == []


def _strict(bridge):
    bridge.replies["query_cb_validity_strict"] = {
        "results": [
            {"cb_name": "claim_cb", "cb_index": 11, "outer_status": "pass",
             "inner_status": "pass"},
            {"cb_name": "de_jure_cb", "cb_index": 3, "outer_status": "fail",
             "outer_reason": "target_in_truce", "inner_status": "pass"}],
        "summary": {"inner_valid": 1}}


def test_cb_scopes_the_declare_war_verdict_and_title_is_echoed(bridge):
    _strict(bridge)
    env = run(ms.discover(what="interaction_validity", interaction="declare_war",
                          cb="de_jure_cb", title="d_york", target_id=7))
    r = env["result"]
    assert r["cb"] == "de_jure_cb" and r["title"] == "d_york"
    assert r["valid"] is False and r["reason"] == "outer:target_in_truce"
    env = run(ms.discover(what="interaction_validity", interaction="declare_war",
                          cb="claim_cb", target_id=7))
    assert env["result"]["valid"] is True and env["result"]["cb_index"] == 11
    env = run(ms.discover(what="interaction_validity", interaction="declare_war",
                          cb="no_such_cb", target_id=7))
    assert env["result"]["reason"] == "cb_not_in_db"


def test_control_without_cb_the_verdict_is_the_aggregate(bridge):
    _strict(bridge)
    r = run(ms.discover(what="interaction_validity", interaction="declare_war",
                        target_id=7))["result"]
    assert "cb" not in r and r["valid"] is True and r["valid_cbs"] == ["claim_cb"]
    assert r["title"] is None


def test_the_trigger_cascade_fails_on_a_hard_gate_slot_only(bridge):
    bridge.replies["query_trigger_result"] = {"results": [
        {"slot": 0, "name": "is_valid", "present": True, "result": True},
        {"slot": 4, "name": "can_send", "present": True, "result": False},
        {"slot": 5, "name": "auto_accept", "present": True, "result": False}]}
    r = run(ms.discover(what="interaction_validity", interaction="befriend",
                        target_id=7))["result"]
    assert r["valid"] is False and r["failing_slots"] == ["can_send"]
    assert r["interaction"] == "befriend"


def test_decision_type_picks_the_shield_decision(bridge):
    bridge.replies["query_realm"] = {"title_tier": 2, "war_count": 0}
    bridge.replies["query_character"] = {"id": 30853, "top_liege_id": 30853, "prestige": 2500.0}
    env = run(ms.discover(what="decision_why_blocked", decision_type="found_kingdom"))
    assert env["ok"] is True and env["result"]["decision"] == "found_kingdom_decision"


def test_every_parameter_is_described():
    desc = _defs()["discover"]["description"]
    args = desc[desc.index("Args:"):]
    for name in set(PARAMS) | SHAPING | {"what"}:
        assert re.search(rf"\b{name}\b", args), f"discover's description does not explain {name}"


# ── the refusals ──────────────────────────────────────────────────────────────

def test_no_what_is_missing_required_argument_and_lists_the_values(bridge):
    b = refused(run(ms.discover()), "missing_required_argument")
    assert "valid_cbs_strict" in b["summary"] and "targets" in b["summary"]
    assert bridge.sent == []


def test_an_unknown_what_is_unknown_subject(bridge):
    b = refused(run(ms.discover(what="no_such_thing")), "unknown_subject")
    assert "no_such_thing" in b["summary"]
    assert bridge.sent == []


@pytest.mark.parametrize("action", ["marry", "befriend", "seduce"])
def test_a_targets_action_it_cannot_enumerate_is_unknown_verb(bridge, action):
    b = refused(run(ms.discover(what="targets", action=action)), "unknown_verb")
    assert "war" in b["summary"] and "claim" in b["summary"]
    assert bridge.sent == []


def test_an_unmodelled_decision_is_unknown_subject(bridge):
    refused(run(ms.discover(what="decision_why_blocked", decision_type="become_pope")),
            "unknown_subject")


@pytest.mark.parametrize("what", ["war_targets", "valid_cbs_strict"])
@pytest.mark.parametrize("exc,gate", [(TimeoutError("late"), "read_timeout"),
                                      (ConnectionError("gone"), "bridge_disconnected")])
def test_a_read_that_did_not_come_back_is_named(bridge, what, exc, gate):
    bridge.replies[T.resolve_discover({"what": what})[0]] = exc
    refused(run(ms.discover(what=what)), gate)


def test_a_deferred_read_the_bridge_never_answered_is_read_timeout(bridge):
    bridge.replies["query_cb_validity_strict"] = {
        "status": "sent_no_confirmation", "reason_code": "no_ack", "waited_s": 13.5}
    b = refused(run(ms.discover(what="valid_cbs_strict", target_id=9)), "read_timeout")
    assert "13.5" in b["summary"] and b["raw_reason"] == "no_ack"


@pytest.mark.parametrize("what,reply,gate", [
    ("acceptance", {"success": False, "reason": "interaction_not_found"}, "name_not_found"),
    ("acceptance", {"success": False, "reason": "missing interaction_type"},
     "missing_required_argument"),
    ("imprisonment", {"success": False}, "unmapped_refusal"),
])
def test_a_reply_the_bridge_declined_is_a_refusal(bridge, what, reply, gate):
    bridge.replies[T.resolve_discover({"what": what})[0]] = reply
    refused(run(ms.discover(what=what)), gate)


def test_a_declined_targets_read_is_a_refusal(bridge):
    bridge.replies["query_realm"] = {"success": False, "reason": "character_not_found"}
    refused(run(ms.discover(what="targets", action="execute", actor_id=30853)), "character_not_found")
    refused(run(ms.discover(what="targets", action="imprison", actor_id=30853)), "character_not_found")
    # nobody looked is not "no vassal": an unread realm is read_timeout, never an empty list
    bridge.replies["query_realm"] = TimeoutError("late")
    env = run(ms.discover(what="targets", action="imprison", actor_id=30853))
    assert env["ok"] is False and env["blocked"]["gate"] in ("read_timeout", "bridge_disconnected"), env


def test_imprison_targets_are_one_judge_in_discover_and_do(bridge):
    """MAJOR 4 (witness #5): discover(what='targets', action='imprison') annotated
    vassals with grounds alone (query_imprisonment_reasons), a second imprison judge
    beside do(action_class='imprison_targets'). It is now the same rows: the game's
    own send check per vassal, with the target's answer and score."""
    bridge.replies["query_realm"] = {"vassals": [{"id": 5, "name": "Ulf"}, {"id": 6, "name": "Bo"},
                                                 {"id": 7, "name": "Ari"}]}
    bridge.replies["query_imprisonment_reasons"] = {"has_imprisonment_reason": False, "reasons": []}
    bridge.replies["query_interaction_check"] = {"success": True, "result": {"results": [
        {"target_id": 5, "sendable": True, "answer": "yes", "accept_score": 153.5, "answer_text": "Will succeed"},
        {"target_id": 6, "sendable": True, "answer": "maybe", "accept_score": 50.0,
         "answer_text": "Might succeed"},
        {"target_id": 7, "sendable": False, "refused_by": "send_conditions_not_met",
         "engine_reason": "You have no reason to imprison them"}]}}
    d = run(ms.discover(what="targets", action="imprison", actor_id=30853))
    assert d["ok"] is True, d
    r = d["result"]
    do = run(ms.do(action_class="imprison_targets", character_id=30853))["result"]
    assert r["available"] == do["available"] and r["unavailable"] == do["unavailable"]
    assert [(x["do_call"]["args"]["target_id"], x["answer"], x["accept_score"]) for x in r["available"]] == \
        [(5, "yes", 153.5), (6, "maybe", 50.0)]
    assert "chance_pct" not in r["available"][0] and r["available"][1]["chance_pct"] == 50.0
    assert r["unavailable"][0]["blocked"]["gate"] == "interaction_invalid"
    # the grounds read is a record on each row, never the verdict
    assert all("grounds" in x for x in r["available"] + r["unavailable"])
    checks = bridge.params_of("query_interaction_check")
    assert checks and all(c["interaction_type"] == "imprison_interaction" and c["actor_id"] == 30853
                          for c in checks)


def test_no_bridge_is_bridge_disconnected(bridge, monkeypatch):
    bridge.state.connected = False

    async def no(_s):
        return False
    monkeypatch.setattr(ms, "_try_reconnect", no)
    refused(run(ms.discover(what="interactions")), "bridge_disconnected")
    assert bridge.sent == []


# ── targets ───────────────────────────────────────────────────────────────────

def test_targets_single_plan_carries_the_reply_and_the_action(bridge):
    bridge.replies["query_valid_war_targets"] = {"targets": [{"target_id": 5}]}
    env = run(ms.discover(what="targets", action="declare_war"))
    assert env["ok"] is True
    assert env["result"] == {"action": "declare_war", "targets": [{"target_id": 5}]}


def test_targets_pool_is_capped_and_annotated(bridge):
    vassals = [{"id": i, "name": f"V{i}"} for i in range(1, 46)]
    vassals[0] = {"id": 1, "name_pending": True, "name_key": "E_thelred"}
    bridge.replies["query_realm"] = {"vassals": vassals}

    def ann(p):
        if p["target_id"] == 2:
            raise TimeoutError("late")
        return {"has_imprisonment_reason": p["target_id"] % 2 == 1, "reason_count": 1,
                "reasons": ["crime"], "actor_is_liege": True, "noise": 1}
    bridge.replies["query_imprisonment_reasons"] = ann
    r = run(ms.discover(what="targets", action="execute", actor_id=30853))["result"]
    assert r["label"] == "grounds" and r["target_count"] == 40 and r["truncated"] is True
    assert len(bridge.params_of("query_imprisonment_reasons")) == 40
    assert bridge.params_of("query_realm") == [{"character_id": 30853}]
    first, second, third = r["targets"][:3]
    assert first["name"] is None and first["name_pending"] is True
    assert first["name_key"] == "E_thelred"
    assert second == {"target_id": 2, "name": "V2", "annotate_error": True}
    assert third["has_imprisonment_reason"] is True and "noise" not in third


def test_targets_revoke_title_is_the_bare_vassal_roster(bridge):
    bridge.replies["query_realm"] = {"vassals": [{"id": 9, "name": "Ulf"}]}
    r = run(ms.discover(what="targets", action="revoke_title"))["result"]
    assert r["targets"] == [{"target_id": 9, "name": "Ulf"}]
    assert bridge.params_of("query_imprisonment_reasons") == []


# ── decision_why_blocked ──────────────────────────────────────────────────────

def test_danelaw_with_a_failed_title_read_reports_the_duchy_clause_unmeasured(bridge):
    bridge.replies["query_realm"] = {"title_tier": 3, "war_count": 0}
    bridge.replies["query_character"] = {"id": 30853, "top_liege_id": 30853}
    bridge.replies["query_title_hierarchy"] = TimeoutError("late")
    env = run(ms.discover(what="decision_why_blocked", decision_type="negotiate_the_danelaw"))
    assert env["ok"] is True
    r = env["result"]
    assert r["decision"] == "negotiate_the_danelaw_decision"
    (duchy,) = [c for c in r["clauses"] if c["clause"] == "controlled_duchies"]
    assert "UNMEASURED" in duchy["label"] and "note" in duchy["measured"], duchy
    assert "complete" not in duchy["measured"], "a failed read is not a count of 0"
    assert duchy["pass"] is None and "controlled_duchies" in r["unmeasured"], duchy
    assert r["all_structural_pass"] is False, "an unmeasured clause is not a pass"
    assert bridge.params_of("query_dejure_progress") == []


def test_danelaw_with_a_clamped_title_list_reports_the_duchy_clause_unmeasured(bridge):
    """L15 witness n6: an empty held_titles while query_realm counts titles was the
    emitter's >500 clamp, not "holds no duchy". RED on 16dd7f14 (counted 0). The
    emitter answers an error since L16 R9; this is the second line of defence."""
    bridge.replies["query_realm"] = {"title_tier": 3, "war_count": 0, "held_title_count": 612}
    bridge.replies["query_character"] = {"id": 30853, "top_liege_id": 30853}
    bridge.replies["query_title_hierarchy"] = {"character_id": 30853, "held_titles": [],
                                               "held_title_count": 0}
    env = run(ms.discover(what="decision_why_blocked", decision_type="negotiate_the_danelaw"))
    (duchy,) = [c for c in env["result"]["clauses"] if c["clause"] == "controlled_duchies"]
    assert "UNMEASURED" in duchy["label"] and duchy["pass"] is None, duchy
    assert "complete" not in duchy["measured"], duchy


@pytest.mark.parametrize("hier", [
    {"character_id": 30853, "error": "title_count_clamped", "count_field": "held_title_count",
     "raw_count": 612, "max_count": 500},
    {"character_id": 30853, "error": "title_lookup_failed", "title_id": 4242, "index": 1,
     "raw_count": 3},
], ids=["clamped", "lookup_failed"])
def test_danelaw_with_the_emitted_title_error_reports_the_duchy_clause_unmeasured(bridge, hier):
    """L16 R9: the emitter's own error shape (query_handlers.cpp title_count_clamped,
    do_query_title_hierarchy's lookup failure) is unmeasured, never a count."""
    bridge.replies["query_realm"] = {"title_tier": 3, "war_count": 0,
                                     "held_title_count": hier["raw_count"]}
    bridge.replies["query_character"] = {"id": 30853, "top_liege_id": 30853}
    bridge.replies["query_title_hierarchy"] = dict(hier)
    env = run(ms.discover(what="decision_why_blocked", decision_type="negotiate_the_danelaw"))
    (duchy,) = [c for c in env["result"]["clauses"] if c["clause"] == "controlled_duchies"]
    assert "UNMEASURED" in duchy["label"] and duchy["pass"] is None, duchy
    assert "complete" not in duchy["measured"], duchy


def _held(key, tid, parent_key, tier=3):
    """One held_titles row as emit_title_info writes it (query_handlers.cpp:
    title_id :2893, key :2910, tier/tier_name :2935, holder_id :2943,
    de_jure_parent_id/_key :2957/:2964; sub_titles/sub_title_count from the
    held-title loop :3062/:3075). A parent_key of None leaves the key out, as the
    emitter does when the parent's type did not read."""
    row = {"title_id": tid, "name": key[2:].title(), "key": key, "tier": tier,
           "tier_name": {2: "county", 3: "duchy"}[tier], "holder_id": 30853,
           "holder_name": "Halfdan", "de_jure_parent_id": tid + 1000,
           "sub_titles": [], "sub_title_count": 0}
    if parent_key is not None:
        row["de_jure_parent_key"] = parent_key
    return row


def _danelaw(bridge, held, progress=None):
    """discover(decision_why_blocked) for the danelaw with these held titles; every
    query_dejure_progress reply is 3/3 unless `progress` names the key's reply."""
    bridge.replies["query_realm"] = {"title_tier": 3, "war_count": 0}
    bridge.replies["query_character"] = {"id": 30853, "top_liege_id": 30853}
    bridge.replies["query_title_hierarchy"] = {"character_id": 30853, "held_titles": held,
                                               "held_title_count": len(held)}
    progress = progress or {}
    bridge.replies["query_dejure_progress"] = lambda p: progress.get(
        p["title_key"], {"character_id": 30853, "title_key": p["title_key"], "title_id": 1,
                         "held": 3, "total": 3, "fraction": 1.0})
    env = run(ms.discover(what="decision_why_blocked", decision_type="negotiate_the_danelaw"))
    assert env["ok"] is True, env
    (duchy,) = [c for c in env["result"]["clauses"] if c["clause"] == "controlled_duchies"]
    return duchy


def test_danelaw_counts_only_the_duchies_whose_de_jure_parent_is_k_england(bridge):
    """L15 A2: the reader filtered on "de_jure_kingdom"/"de_jure_liege_key", which
    the bridge never writes (it writes de_jure_parent_key, query_handlers.cpp:2964),
    so the k_england filter never ran and a Scottish duchy counted. RED on
    0f5ed6b7: complete 2, pass True."""
    duchy = _danelaw(bridge, [_held("d_lothian", 70, "k_scotland"),
                              _held("d_essex", 80, "k_england"),
                              _held("c_colchester", 81, "d_essex", tier=2)])
    assert duchy["measured"]["complete"] == 1, duchy
    assert [d["key"] for d in duchy["measured"]["complete_duchies"]] == ["d_essex"]
    assert duchy["pass"] is False, duchy
    assert [p["title_key"] for p in bridge.params_of("query_dejure_progress")] == ["d_essex"]


def test_danelaw_a_duchy_whose_kingdom_did_not_read_is_unmeasured(bridge):
    """A held duchy with no de_jure_parent_key may be an English one: short of the
    count, the clause is UNMEASURED, not a fail, and the duchy is not counted.
    RED on 0f5ed6b7 (counted: complete 2, pass True)."""
    duchy = _danelaw(bridge, [_held("d_essex", 80, "k_england"),
                              _held("d_kent", 90, None)])
    assert duchy["measured"]["complete"] == 1
    assert [d["key"] for d in duchy["measured"]["unmeasured_duchies"]] == ["d_kent"]
    assert duchy["pass"] is None and duchy["unmeasured"], duchy


def test_danelaw_a_failed_meter_read_is_unmeasured_not_incomplete(bridge):
    duchy = _danelaw(bridge, [_held("d_essex", 80, "k_england"),
                              _held("d_kent", 90, "k_england")],
                     progress={"d_kent": {"character_id": 30853, "title_key": "d_kent",
                                          "title_id": -1, "held": 0, "total": 0,
                                          "fraction": 0.0, "error": "character_unresolved"}})
    assert [d["key"] for d in duchy["measured"]["unmeasured_duchies"]] == ["d_kent"]
    assert duchy["measured"]["incomplete_candidates"] == []
    assert duchy["pass"] is None, duchy


def test_danelaw_two_complete_english_duchies_pass(bridge):
    """The non-discriminating control (green on 0f5ed6b7 too)."""
    duchy = _danelaw(bridge, [_held("d_essex", 80, "k_england"),
                              _held("d_kent", 90, "k_england")])
    assert duchy["pass"] is True and duchy["measured"]["complete"] == 2, duchy


def _clauses(realm, character, decision="found_kingdom_decision"):
    r = T.shield_decision_why_blocked(decision, realm=realm, character=character)
    return {c["clause"]: c for c in r["clauses"]}, r


def test_a_vassal_duke_is_graded_from_the_fields_the_bridge_writes():
    """L15 (witness r4 N4-1): the tier came from primary_title_tier/tier and
    independence from is_independent/liege_id, none of which query_realm writes:
    the tier read 0 ("<= duke" FAILED for every ruler) and independence
    defaulted to True (PASSED for every vassal). Now: query_realm title_tier
    (do_query_realm), query_character id/top_liege_id (build_char_profile),
    query_realm war_count. RED on 627af763 (tier fails, independent passes)."""
    c, r = _clauses({"title_tier": 3, "war_count": 0},
                    {"id": 501, "top_liege_id": 12618, "is_imprisoned": False, "prestige": 900.0})
    assert c["highest_title_le_duke"]["pass"] is True, c["highest_title_le_duke"]
    assert c["independent"]["pass"] is False, c["independent"]
    assert c["at_peace"]["pass"] is True and c["not_imprisoned"]["pass"] is True
    assert "independent" in r["blocking"] and r["all_structural_pass"] is False


def test_an_independent_king_fails_the_tier_and_passes_independence():
    c, _ = _clauses({"title_tier": 4, "war_count": 2}, {"id": 12618, "top_liege_id": 12618})
    assert c["highest_title_le_duke"]["pass"] is False
    assert c["independent"]["pass"] is True
    assert c["at_peace"]["pass"] is False and c["at_peace"]["measured"] == {"war_count": 2}


def test_the_tier_falls_back_to_query_characters_ruler_tier_the_same_read():
    c, _ = _clauses({}, {"id": 7, "top_liege_id": 7, "ruler_tier": 2})
    assert c["highest_title_le_duke"]["pass"] is True
    assert c["highest_title_le_duke"]["measured"] == {"primary_title_tier": 2}


def test_what_no_read_carries_is_unmeasured_never_a_pass():
    """Nobody looked is not a pass: an empty reply leaves the tier, independence,
    the peace and the prison unmeasured; prestige_level and the tribal era are
    in no reply at all."""
    c, r = _clauses({}, {}, decision="negotiate_the_danelaw_decision")
    for k in ("highest_title_le_duke", "independent", "at_peace", "not_imprisoned",
              "prestige_level", "tribal_era", "controlled_duchies"):
        assert c[k]["pass"] is None and c[k]["unmeasured"], c[k]
    assert r["blocking"] == [] and r["all_structural_pass"] is False
    assert sorted(r["unmeasured"]) == sorted(c)
    # a top_liege_id of 0 is a failed liege walk, not a liege
    c, _ = _clauses({}, {"id": 7, "top_liege_id": 0})
    assert c["independent"]["pass"] is None


def test_a_shield_fix_names_no_lever_that_does_nothing():
    """A fix that sends the player to steer would name a lever that does not
    reach a decision's gates; the clauses name what does."""
    clauses = T.shield_decision_why_blocked(
        "found_kingdom_decision", realm={"title_tier": 4, "war_count": 1},
        character={"id": 1, "top_liege_id": 2, "is_imprisoned": True})["clauses"]
    for c in clauses:
        assert "steer" not in str(c.get("fix") or ""), c


#: discover enumerations deleted in L14 (finding 8) -> the do door that owns each.
MOVED = {"hireable_mercenaries": "mercenaries", "fabricate_targets": "fabricate_targets",
         "joinable_factions": "factions_to_join"}


@pytest.mark.parametrize("what", sorted(MOVED))
def test_a_moved_enumeration_names_its_do_door_and_reads_nothing(bridge, what):
    env = run(ms.discover(what=what, character_id=30853))
    assert env["ok"] is False and env["blocked"]["gate"] == "unknown_subject", env
    assert env["blocked"]["next"] == {"tool": "do", "args": {"action_class": MOVED[what]}}
    assert MOVED[what] in R.ACTION_CLASSES
    assert what not in T.DISCOVER_WHATS
    assert bridge.sent == [], bridge.sent
    assert T.DISCOVER_MOVED_TO_DO == MOVED
