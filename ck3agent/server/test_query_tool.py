"""query(): behaviour against a fake bridge (cleanup 2026.11 F15).

The fake answers each bridge action from a table and records every send, so a
row can say which bridge action a call reached and with what parameters. Rows:

  * the envelope: its keys, the actor echo, the game block, a parsed result;
  * every domain in tools.QUERY_DOMAIN_MAP reaches its bridge action;
  * every parameter of the registered query schema reaches the bridge for a
    domain that reads it, and the same call WITHOUT it does not send that value
    (the control that makes each row able to fail). A census row reds when the
    schema gains a parameter this table does not test;
  * every refusal is named: missing_required_argument, unknown_domain,
    invalid_argument (oversized batch; domain and domains together),
    read_timeout, bridge_disconnected, and a reply the bridge declined;
  * the batch: per-domain keys, a failed domain is an errors[] row, and the two
    composed domains (event_log, relative_power) answer in a batch as they do alone;
  * event_log and relative_power: what they compute from their reads;
  * every key a domain builder reads is a parameter query() can set (a key no
    caller can set is a dead branch).
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

ENVELOPE_KEYS = {"ok", "tool", "result", "blocked", "warnings", "errors", "bridge_health"}


def run(coro):
    return asyncio.run(coro)


def _defs():
    return {t["name"]: t for t in ms.registered_tool_definitions()}


class Bridge:
    """Replies by bridge action; records (path, action, params, timeout)."""

    def __init__(self, state):
        self.state = state
        self.replies = {}
        self.sent = []
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

    def actions(self):
        return [a for _, a, _, _ in self.sent]

    def params_of(self, action):
        return [p for _, a, p, _ in self.sent if a == action]


@pytest.fixture
def bridge(monkeypatch):
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 41, "867.3.1"
    b = Bridge(state)
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
    assert env["ok"] is False, env
    assert ENVELOPE_KEYS <= set(env), env
    b = env["blocked"]
    assert b["gate"] == gate, b
    assert b["gate"] in R.GATE_TABLE
    for k in ("class", "severity", "retryable", "summary"):
        assert k in b, (k, b)
    assert b["summary"], b
    return b


# ── the envelope ──────────────────────────────────────────────────────────────

def test_a_single_domain_answers_in_the_one_envelope(bridge):
    bridge.replies["query_realm"] = {"character_id": 30853, "held_title_count": 4}
    env = run(ms.query(domain="realm"))
    assert set(env) == ENVELOPE_KEYS | {"actor", "game"}, sorted(env)
    assert env["ok"] is True and env["tool"] == "query" and env["blocked"] is None
    assert env["result"] == {"character_id": 30853, "held_title_count": 4}
    assert env["actor"] == {"character_id": 30853}
    assert env["game"] == {"tick": 41, "date": "867.3.1", "paused": True, "speed": 1}
    assert env["warnings"] == [] and env["errors"] == [] and env["bridge_health"] == "ok"
    assert bridge.sent == [("query", "query_realm",
                            T.resolve_query("realm", {"character_id": 0})[1], 15.0)]


def test_a_reply_naming_no_character_carries_no_actor(bridge):
    bridge.replies["query_law_groups"] = {"groups": []}
    env = run(ms.query(domain="law_groups"))
    assert env["ok"] is True and "actor" not in env


def test_a_json_text_reply_is_parsed_and_a_scalar_is_carried_as_value(bridge):
    bridge.replies["query_realm"] = '{"character_id": 5, "gold": 1}'
    assert run(ms.query(domain="realm"))["result"] == {"character_id": 5, "gold": 1}
    bridge.replies["query_realm"] = 7
    assert run(ms.query(domain="realm"))["result"] == {"value": 7}


@pytest.mark.parametrize("domain", sorted(T.QUERY_DOMAIN_MAP))
def test_every_mapped_domain_reaches_its_bridge_action(bridge, domain):
    env = run(ms.query(domain=domain))
    assert env["ok"] is True, env
    assert bridge.actions() == [T.QUERY_DOMAIN_MAP[domain][0]]
    assert bridge.sent[0][0] == "query", "a query domain is a read, not an action"


# ── every parameter ───────────────────────────────────────────────────────────

#: parameter -> (domain, the call's kwargs, what the bridge must receive)
PARAMS = {
    "character_id": ("realm", {"character_id": 77}, {"character_id": 77}),
    "target_id": ("opinion", {"character_id": 5, "target_id": 88},
                  {"actor_id": 5, "target_id": 88}),
    "title_id": ("titles", {"title_id": 123}, {"title_id": 123}),
    "trait_id": ("trait_compatibility", {"trait_id": 9}, {"trait_id": 9}),
    "include_vassals": ("claims", {"include_vassals": True}, {"include_vassals": True}),
    "war_id": ("war_score", {"war_id": 4}, {"war_id": 4}),
    "culture_id": ("innovation_progress", {"culture_id": 31}, {"culture_id": 31}),
    "dynasty_id": ("dynasty", {"dynasty_id": 600}, {"dynasty_id": 600}),
    "filter": ("characters", {"filter": "ruler"}, {"filter": "ruler"}),
    "sort": ("characters", {"sort": "gold"}, {"sort": "gold"}),
    "sort_order": ("characters", {"sort_order": "asc"}, {"sort_order": "asc"}),
    "max_results": ("characters", {"max_results": 7}, {"max_results": 7}),
    "court_of": ("characters", {"court_of": 163110}, {"court_of": 163110}),
    "female": ("characters", {"female": False}, {"female": False}),
    "since_tick": ("war_declarations", {"since_tick": 300}, {"since_tick": 300}),
    "limit": ("war_declarations", {"limit": 12}, {"limit": 12}),
    "require_in_range": ("marriage_candidates", {"require_in_range": False},
                         {"require_in_range": 0}),
    "verbose": ("realm", {"verbose": True}, {"verbose": True}),
    "scope": ("map_graph", {"scope": "target"}, {"scope": "target"}),
    "hops": ("map_graph", {"hops": 4}, {"hops": 4}),
    "max_nodes": ("map_graph", {"max_nodes": 25}, {"max_nodes": 25}),
    "target_county": ("map_graph", {"target_county": 4411}, {"target_county": 4411}),
    "target_county_key": ("map_graph", {"target_county_key": "c_york"},
                          {"target_county_key": "c_york"}),
    "counties": ("map_graph", {"counties": "c_york,c_lincoln"},
                 {"counties": "c_york,c_lincoln"}),
    "detail": ("map_graph", {"detail": "brief"}, {"detail": "brief"}),
}


def test_every_query_parameter_has_a_row():
    props = set(_defs()["query"]["input_schema"]["properties"])
    assert len(props) > 10, "the schema read found too few parameters (fail OPEN)"
    assert props == set(PARAMS) | {"domain", "domains"}, sorted(props ^ (set(PARAMS) | {"domain", "domains"}))


@pytest.mark.parametrize("name", sorted(PARAMS))
def test_each_parameter_reaches_the_bridge(bridge, name):
    domain, kwargs, expect = PARAMS[name]
    env = run(ms.query(domain=domain, **kwargs))
    assert env["ok"] is True, env
    (sent,) = bridge.params_of(T.QUERY_DOMAIN_MAP[domain][0])
    assert expect.items() <= sent.items(), (name, sent)


@pytest.mark.parametrize("name", sorted(PARAMS))
def test_control_without_the_parameter_the_value_is_not_sent(bridge, name):
    domain, kwargs, expect = PARAMS[name]
    others = {k: v for k, v in kwargs.items() if k != name}
    run(ms.query(domain=domain, **others))
    (sent,) = bridge.params_of(T.QUERY_DOMAIN_MAP[domain][0])
    assert not expect.items() <= sent.items(), (name, sent)


def test_the_defaults_the_description_states(bridge):
    run(ms.query(domain="characters"))
    run(ms.query(domain="marriage_candidates"))
    run(ms.query(domain="realm"))
    (chars,) = bridge.params_of("search_characters")
    assert chars == {"filter": "alive", "sort": "military", "sort_order": "desc",
                     "max_results": 50}
    (mc,) = bridge.params_of("query_marriage_candidates")
    assert mc["max_results"] == 30 and mc["sort"] == "military" and mc["require_in_range"] == 1
    (realm,) = bridge.params_of("query_realm")
    assert "verbose" not in realm, "lean is the default: verbose is sent only when asked"
    desc = _defs()["query"]["description"]
    assert "default 50" in desc and "default 30" in desc


def test_female_a_non_boolean_is_refused_at_the_mcp_boundary_and_nothing_is_sent(bridge):
    """L14 m-10 (server half): the MCP boundary refuses female="yes" instead of
    turning it into True; at a1fb8c6b pydantic's lax bool plus bool(female) sent
    female=True for "yes". Control: a real boolean reaches the bridge as given."""
    for bad in ("yes", "no", 1):
        with pytest.raises(Exception) as e:
            run(ms.mcp.call_tool("query", {"domain": "characters", "female": bad}))
        assert "female" in str(e.value), (bad, e.value)
    assert bridge.params_of("search_characters") == [], bridge.sent
    run(ms.mcp.call_tool("query", {"domain": "characters", "female": False}))
    (sent,) = bridge.params_of("search_characters")
    assert sent["female"] is False, sent


def test_female_is_passed_as_given_not_coerced(bridge):
    """A value that reaches query() past the MCP boundary is not bool()-ed: the
    bridge's own female_must_be_boolean refusal sees what the caller gave."""
    run(ms.query(domain="characters", female="yes"))
    (sent,) = bridge.params_of("search_characters")
    assert sent["female"] == "yes", sent


def test_every_parameter_is_described():
    desc = _defs()["query"]["description"]
    args = desc[desc.index("Args:"):]
    for name in list(PARAMS) + ["domain", "domains"]:
        assert re.search(rf"\b{name}\b", args), f"query's description does not explain {name}"


# ── the refusals ──────────────────────────────────────────────────────────────

def test_no_domain_is_missing_required_argument_and_lists_the_domains(bridge):
    b = refused(run(ms.query()), "missing_required_argument")
    assert "realm" in b["summary"] and "event_log" in b["summary"]
    assert bridge.sent == []


def test_an_unknown_domain_is_named(bridge):
    b = refused(run(ms.query(domain="no_such_domain")), "unknown_domain")
    assert "no_such_domain" in b["summary"]
    assert bridge.sent == []


#: Domains deleted in cleanup 2026.11 L14 (lane S2), each with why. A domain added
#: back under one of these names reds here.
DELETED_DOMAINS = {
    "event_probabilities": "finding 7: answered events: [] in both clock modes; no tool answers an event",
    "active_wars": "finding 8: the same war ids as query(wars), inspect(wars) and observe",
    "dejure": "finding 8: inspect(subject='title') reads the same meter; de_jure is the one the docs cite (2 to 0)",
    "goal": "finding 8: the same read as dejure",
    "partition_preview": "finding 8: reads the current holder, not the split after the death",
    "succession_preview": "finding 8: succession_plan carries the per-title map and the verdict",
    "succession_simulation": "finding 8: succession_plan reads it and states its verdict",
    "debug_flags": "finding 8: bridge switches (instabuild, yesmen), not a player read",
}


@pytest.mark.parametrize("domain", sorted(DELETED_DOMAINS))
def test_a_deleted_domain_is_unknown_and_reads_nothing(bridge, domain):
    assert domain not in T.QUERY_DOMAINS
    refused(run(ms.query(domain=domain)), "unknown_domain")
    assert bridge.sent == []


def test_domain_and_domains_together_are_refused_and_nothing_is_read(bridge):
    refused(run(ms.query(domain="realm", domains=["wars"])), "invalid_argument")
    assert bridge.sent == []


def test_a_batch_over_the_cap_is_refused_and_nothing_is_read(bridge):
    six = ["realm", "wars", "council", "laws", "claims", "armies"]
    assert len(six) > ms.QUERY_BATCH_MAX
    b = refused(run(ms.query(domains=six)), "invalid_argument")
    assert str(ms.QUERY_BATCH_MAX) in b["summary"]
    assert bridge.sent == []


@pytest.mark.parametrize("exc,gate", [(TimeoutError("late"), "read_timeout"),
                                      (asyncio.TimeoutError(), "read_timeout"),
                                      (ConnectionError("gone"), "bridge_disconnected"),
                                      (OSError("pipe"), "bridge_disconnected")])
def test_a_read_that_did_not_come_back_is_named(bridge, exc, gate):
    bridge.replies["query_realm"] = exc
    refused(run(ms.query(domain="realm")), gate)


def test_no_bridge_is_bridge_disconnected_and_nothing_is_sent(bridge, monkeypatch):
    bridge.state.connected = False

    async def no(_s):
        return False
    monkeypatch.setattr(ms, "_try_reconnect", no)
    refused(run(ms.query(domain="realm")), "bridge_disconnected")
    assert bridge.sent == []


@pytest.mark.parametrize("reply,gate", [
    ({"success": False, "action": "query_realm", "reason": "character_not_found"},
     "character_not_found"),
    ({"success": False, "action": "query_realm"}, "unmapped_refusal"),
    ({"success": False, "reason": "no_controlled_character"}, "no_ruler_bound"),
])
def test_a_reply_the_bridge_declined_is_a_refusal_not_a_reading(bridge, reply, gate):
    bridge.replies["query_realm"] = reply
    b = refused(run(ms.query(domain="realm")), gate)
    assert "query_realm" in b["summary"]


#: The replies the bridge really sends for a READ it did not answer (L14 FF3 N3-1):
#:   pipeline_timeout  pipeline.cpp (serve, `run_on_game_thread` false):
#:                     {"success":false,"reason":"game_thread_timeout","phase":"serve"};
#:   inline_timeout    ipc_server.cpp serve_on_game_thread (`!ran`), no `success`:
#:                     {"error":"game_thread_timeout","action":...,"waited_ms":...,"hint":...};
#:   inline_fault      MEMORY_ACCESS_FAULT_REPLY (bridge/include/bridge.h), from the
#:                     SIGSEGV recovery in serve_on_game_thread and every
#:                     answer_memory_access_fault: {"error":"memory_access_fault"};
#:   in_flight_drop    mcp_server.py _listen `except ConnectionError`:
#:                     {"error":"bridge_disconnected"}.
#: A read that did not run in time changed nothing: read_timeout, retryable. A
#: fault is bridge_fault. A read in flight at a disconnect stays bridge_disconnected.
READ_FAULTS = {
    "pipeline_timeout": ({"success": False, "reason": "game_thread_timeout", "phase": "serve"},
                         "read_timeout", True),
    "inline_timeout": ({"error": "game_thread_timeout", "action": "query_realm", "waited_ms": 2000,
                        "hint": "The game-thread drain did not run this read in time (a load "
                                "in progress, or no session-pass hook and the clock stopped). "
                                "Nothing was read."},
                       "read_timeout", True),
    "inline_fault": ({"error": "memory_access_fault"}, "bridge_fault", False),
    "in_flight_drop": ({"error": "bridge_disconnected"}, "bridge_disconnected", True),
}


@pytest.mark.parametrize("shape", sorted(READ_FAULTS))
def test_a_read_the_bridge_did_not_answer_is_a_typed_refusal(bridge, shape):
    """RED on 293c0e00: the pipeline timeout was bridge_fault, retryable False
    ("read back before sending it again" for a read that changed nothing), and
    the three replies with no `success` came back ok:true as the reading."""
    reply, gate, retryable = READ_FAULTS[shape]
    bridge.replies["query_realm"] = reply
    b = refused(run(ms.query(domain="realm")), gate)
    assert b["retryable"] is retryable, b
    # and in a batch, an errors[] row, never the domain's data
    bridge.replies["query_wars"] = {"wars": []}
    env = run(ms.query(domains=["realm", "wars"]))
    assert "realm" not in env["result"] and env["result"]["wars"] == {"wars": []}, env
    assert [e["code"] for e in env["errors"]] == [gate], env["errors"]


def test_control_a_read_error_beside_the_reading_is_still_the_reading(bridge):
    """The flip: a read's `error` that is not one of the bridge's fault codes can
    sit beside data (query_handlers.cpp do_query_title_hierarchy: an unknown
    title_id is {"error": "title_not_found", "title_id": N}), so it stays the
    reading."""
    reply = {"error": "title_not_found", "title_id": 999}
    bridge.replies["query_title_hierarchy"] = reply
    env = run(ms.query(domain="titles", title_id=999))
    assert env["ok"] is True and env["result"] == reply, env


def test_control_a_reply_with_success_true_or_absent_is_a_reading(bridge):
    for reply in ({"success": True, "held_title_count": 3}, {"held_title_count": 3}):
        bridge.replies["query_realm"] = reply
        assert run(ms.query(domain="realm"))["ok"] is True


# ── the batch ─────────────────────────────────────────────────────────────────

def test_a_clean_batch_keys_each_reply_by_domain(bridge):
    bridge.replies.update({"query_realm": {"held_title_count": 1}, "query_wars": {"wars": []}})
    env = run(ms.query(domains=["realm", "wars"]))
    assert env["ok"] is True and env["errors"] == []
    assert env["result"] == {"realm": {"held_title_count": 1}, "wars": {"wars": []}}
    assert bridge.actions() == ["query_realm", "query_wars"]


def test_a_failed_domain_is_an_errors_row_and_the_rest_still_answer(bridge):
    bridge.replies.update({
        "query_wars": TimeoutError("late"),
        "query_council": {"success": False, "reason": "character_not_found"},
    })
    env = run(ms.query(domains=["realm", "wars", "council", "nope"]))
    assert env["ok"] is False and env["blocked"] is None
    rows = {e["detail"].split(":", 1)[0]: e["code"] for e in env["errors"]}
    assert rows == {"wars": "read_timeout", "council": "character_not_found",
                    "nope": "unknown_domain"}, env["errors"]
    assert set(env["result"]) == {"realm"}


def test_the_composed_domains_answer_in_a_batch(bridge):
    bridge.replies.update({
        "query_war_declarations": {"current_tick": 50, "declarations": [{"tick": 49}]},
        "query_alliances": {"own_military_strength": 100},
    })
    env = run(ms.query(domains=["event_log", "relative_power"]))
    assert env["ok"] is True, env["errors"]
    assert env["result"]["event_log"]["event_count"] == 1
    assert env["result"]["relative_power"]["my_strength"] == 100.0


# ── event_log ─────────────────────────────────────────────────────────────────

def _feeds(bridge):
    bridge.replies.update({
        "query_war_declarations": {"current_tick": 90, "declarations": [
            {"tick": 80, "attacker_id": 1}, {"tick": 20, "attacker_id": 2}]},
        "query_scheme_inits": {"current_tick": 95, "scheme_inits": [{"tick": 85}]},
        "query_council_tasks": {"current_tick": 70, "assignments": [{"tick": 60}]},
    })


def test_event_log_merges_the_three_feeds_newest_first(bridge):
    _feeds(bridge)
    env = run(ms.query(domain="event_log", since_tick=10))
    r = env["result"]
    assert env["ok"] is True
    assert [(e["tick"], e["kind"]) for e in r["events"]] == [
        (85, "scheme"), (80, "war"), (60, "council_task"), (20, "war")]
    assert r["current_tick"] == 95 and r["since_tick"] == 10 and r["event_count"] == 4
    assert r["truncated"] is False
    for action in ("query_war_declarations", "query_scheme_inits", "query_council_tasks"):
        assert bridge.params_of(action) == [{"since_tick": 10, "limit": 100}]


def test_event_log_limit_truncates_and_filter_picks_the_feeds(bridge):
    _feeds(bridge)
    r = run(ms.query(domain="event_log", limit=2))["result"]
    assert r["event_count"] == 2 and r["truncated"] is True
    assert [e["tick"] for e in r["events"]] == [85, 80]
    bridge.sent.clear()
    r = run(ms.query(domain="event_log", filter="war"))["result"]
    assert bridge.actions() == ["query_war_declarations"]
    assert {e["kind"] for e in r["events"]} == {"war"}


def test_event_log_a_feed_that_fails_is_a_row_and_the_others_report(bridge):
    _feeds(bridge)
    bridge.replies["query_scheme_inits"] = TimeoutError("late")
    bridge.replies["query_council_tasks"] = {"success": False, "reason": "character_not_found"}
    env = run(ms.query(domain="event_log"))
    assert env["ok"] is False and env["blocked"] is None
    assert sorted(e["code"] for e in env["errors"]) == ["character_not_found", "read_timeout"]
    assert {e["kind"] for e in env["result"]["events"]} == {"war"}


# ── relative_power ────────────────────────────────────────────────────────────

@pytest.mark.parametrize("mine,theirs,verdict", [
    (300, 200, "winnable_on_tempo"), (200, 200, "parity_defense_only"),
    (100, 200, "unfavorable")])
def test_relative_power_verdict_follows_the_callable_ratio(bridge, mine, theirs, verdict):
    bridge.replies["query_alliances"] = lambda p: (
        {"own_military_strength": mine / 2, "total_callable_strength": mine}
        if p["character_id"] == 0 else {"own_military_strength": theirs})
    bridge.replies["query_faction_threat"] = {"factions": [{"power_pct_of_liege": 50}]}
    r = run(ms.query(domain="relative_power", target_id=555))["result"]
    assert r["offensive_verdict"] == verdict
    assert r["offensive_ratio"] == round(mine / theirs, 2)
    assert r["target_id"] == 555 and r["target_strength"] == float(theirs)
    assert r["internal_ratio"] == 2.0 and r["internal_safe"] is True


def test_relative_power_without_a_target_compares_only_against_factions(bridge):
    bridge.replies["query_alliances"] = {"own_military_strength": 80}
    bridge.replies["query_faction_threat"] = {"factions": [{"power_pct_of_liege": 90}]}
    r = run(ms.query(domain="relative_power"))["result"]
    assert "offensive_verdict" not in r and "target_id" not in r
    assert r["internal_safe"] is False and r["largest_faction_power_pct"] == 90.0


def test_relative_power_without_my_own_strength_is_refused(bridge):
    bridge.replies["query_alliances"] = TimeoutError("late")
    refused(run(ms.query(domain="relative_power")), "read_timeout")
    bridge.replies["query_alliances"] = {"success": False, "reason": "character_not_found"}
    refused(run(ms.query(domain="relative_power")), "character_not_found")


def test_relative_power_a_failed_side_read_is_a_row(bridge):
    bridge.replies["query_alliances"] = {"own_military_strength": 80}
    bridge.replies["query_faction_threat"] = ConnectionError("gone")
    env = run(ms.query(domain="relative_power"))
    assert env["ok"] is False and [e["code"] for e in env["errors"]] == ["bridge_disconnected"]
    assert env["result"]["my_strength"] == 80.0


# ── what a domain builder may read ────────────────────────────────────────────

class _Reads(dict):
    """A params dict that records every key a builder reads, however it reads it."""

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.read = set()

    def get(self, key, default=None):
        self.read.add(key)
        return super().get(key, default)

    def __getitem__(self, key):
        self.read.add(key)
        return super().__getitem__(key)

    def __contains__(self, key):
        self.read.add(key)
        return super().__contains__(key)


def _query_settable() -> set:
    """The params keys query() can put in front of a builder: its own signature,
    less the two that pick the domain (mcp_server.query passes each parameter
    under its own name)."""
    import inspect
    return set(inspect.signature(ms.query).parameters) - {"domain", "domains"}


@pytest.mark.parametrize("domain", sorted(T.QUERY_DOMAIN_MAP))
def test_every_key_a_builder_reads_is_one_query_can_set(domain):
    settable = _query_settable()
    for present in (False, True):
        # Both shapes: empty, so the `"k" in p` / p.get() branches are read, and
        # every settable key present and truthy, so the p[k] branches are too.
        p = _Reads({k: 1 for k in settable} if present else {})
        T.QUERY_DOMAIN_MAP[domain][1](p)
        assert p.read <= settable, (domain, sorted(p.read - settable))


def test_control_the_reads_recorder_sees_a_key_query_cannot_set():
    p = _Reads({})
    (lambda q: {**({"no_such_key": q["no_such_key"]} if q.get("no_such_key") else {})})(p)
    assert "no_such_key" in p.read and "no_such_key" not in _query_settable()


def test_court_of_reaches_search_characters_and_is_absent_by_default(bridge):
    run(ms.query(domain="characters", court_of=163110, filter="alive"))
    run(ms.query(domain="characters"))
    first, second = bridge.params_of("search_characters")
    assert first["court_of"] == 163110 and first["filter"] == "alive"
    assert "court_of" not in second, "court_of=0 must not filter the whole DB"



def test_female_reaches_search_characters_both_ways_and_is_absent_by_default(bridge):
    """L14 S4 (surface-eval minor 10): female=False is a filter (men), not unset."""
    run(ms.query(domain="characters", court_of=163110, female=True))
    run(ms.query(domain="characters", court_of=163110, female=False))
    run(ms.query(domain="characters", court_of=163110))
    yes, no, unset = bridge.params_of("search_characters")
    assert yes["female"] is True and no["female"] is False
    assert "female" not in unset, "female unset must not filter by sex"


def test_the_court_row_carries_sex_and_dynasty(bridge):
    """The row a player reads: the bridge's is_female and dynasty_name reach the
    result unchanged, beside the filter echo."""
    bridge.replies["search_characters"] = {
        "court_of": 163110, "female": True, "returned": 1,
        "characters": [{"id": 6878, "name": "Gytha", "is_dead": False, "is_female": True,
                        "dynasty_name": "dynn_Godwin", "is_ruler": False}]}
    env = run(ms.query(domain="characters", court_of=163110, female=True))
    assert env["ok"] is True, env
    res = env["result"]
    assert res["female"] is True and res["court_of"] == 163110
    (row,) = res["characters"]
    assert row["is_female"] is True and row["dynasty_name"] == "dynn_Godwin"
