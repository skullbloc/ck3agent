"""cleanup 2026.11 L1: the tool surface, its vocabularies and its refusals, checked
against the code that answers them.

THE SURFACE IS WHAT FASTMCP REGISTERS. Every row below reads
mcp_server.registered_tool_definitions() (the tools/list handler), never a list
typed beside it: a hand-kept schema is how `do` came to advertise intents it did
not accept and a server came to advertise tools it did not register.

THE ENUMS ARE THE CODE'S. Each tool parameter that names a vocabulary carries an
enum built from the constant the implementation dispatches on, and each row here
checks the constant against the dispatch itself (an AST walk, or a resolve call
per value), so a branch added without its name, or a name without its branch,
reds.

EVERY REFUSAL IS NAMED. The gate `unknown` is retired. A row drives each tool's
error paths and checks `blocked.gate` is a GATE_TABLE name; another checks every
literal gate the source passes to why_blocked is in GATE_TABLE, and that the
fallback for a name that is not reads as `unmapped_refusal`, never a neighbour.

THE SPEED RULE IS THE BRIDGE'S. SPEED_MIN/SPEED_MAX are read against the clamp in
bridge/src/event_handler.cpp; a reverse row fails if a second copy of the cap
appears in the server.
"""

import ast
import asyncio
import os
import re
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402
import tools as T  # noqa: E402

THE_13 = {"observe", "query", "discover", "inspect", "cbs", "plan_war",
          "succession_plan", "set_phase", "steer", "declare_war", "do", "session",
          "pass_turn"}


def run(coro):
    return asyncio.run(coro)


def _defs():
    return {t["name"]: t for t in ms.registered_tool_definitions()}


def _enum(tool, param):
    props = _defs()[tool]["input_schema"]["properties"]
    p = props[param]
    vals = p.get("enum") or (p.get("items") or {}).get("enum")
    assert vals, f"{tool}.{param} carries no enum: {p}"
    return [v for v in vals if v is not None]


# ── the surface ───────────────────────────────────────────────────────────────

def test_the_registered_surface_is_the_13_tools():
    names = set(_defs())
    assert names == THE_13, sorted(names ^ THE_13)


def _probe(env: dict) -> tuple[int, list, str]:
    """A FRESH INTERPRETER's registered names under `env` (registration happens
    at import; re-importing here would measure import caching)."""
    import json
    import subprocess
    code = ("import json, mcp_server as m; "
            "print(json.dumps(sorted(t['name'] for t in m.registered_tool_definitions())))")
    r = subprocess.run([sys.executable, "-c", code], cwd=str(HERE), capture_output=True,
                       text=True, timeout=180, env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                                                    **env})
    names = json.loads(r.stdout.strip().splitlines()[-1]) if r.returncode == 0 else []
    return r.returncode, names, r.stderr


CHANNEL_DECLARED = {"CK3_EPISODE_ARM": "council", "CK3_CHANNEL_TOPOLOGY": "complete",
                    "CK3_CHANNEL_SEATS": "K,D1,D2,D3,D4,D5", "CK3_SEAT_LABEL": "D2",
                    "CK3_CHANNEL_QUOTA_PER_CYCLE": "2", "CK3_CHANNEL_MAX_TEXT_BYTES": "600"}


def test_msg_send_is_registered_exactly_when_a_topology_is_declared():
    """Pre-registration v4 (v3 section 5.1 by reference): the 13 and the one. `msg_send` is on the
    wire only under a declared topology other than `none`; in E0, K1 and free
    play the NAME IS ABSENT, not present-and-refused; an arm with no topology
    key does not boot at all (channel_topology_undeclared)."""
    rc, names, _ = _probe({})
    assert rc == 0 and set(names) == THE_13                       # free play
    rc, names, _ = _probe(CHANNEL_DECLARED)
    assert rc == 0 and set(names) == THE_13 | {"msg_send"}
    rc, names, _ = _probe({"CK3_EPISODE_ARM": "k1", "CK3_CHANNEL_TOPOLOGY": "none"})
    assert rc == 0 and set(names) == THE_13                       # declared off: absent
    rc, names, err = _probe({"CK3_EPISODE_ARM": "council"})
    assert rc == 3 and names == [] and "channel_topology_undeclared" in err


NOTEPAD_DECLARED = {"CK3_NOTEPAD_PRIVATE_MAX_BYTES": "4096", "CK3_NOTEPAD_COUNCIL_MAX_BYTES": "8192",
                    "CK3_NOTEPAD_MAX_WRITE_BYTES": "1024"}


def test_the_notepad_tools_are_registered_exactly_when_the_arm_declares_a_pad():
    """v4 draft §5/§7 (lane v4note): the 13, msg_send where a topology is
    declared, and note_read/note_write where pads are declared, each a
    declared per-arm addition; absent (not present-and-refused) otherwise."""
    both = {"note_read", "note_write"}
    rc, names, _ = _probe({"CK3_EPISODE_ARM": "k1", "CK3_CHANNEL_TOPOLOGY": "none",
                           "CK3_NOTEPAD_PADS": "private", "CK3_SEAT_LABEL": "K", **NOTEPAD_DECLARED})
    assert rc == 0 and set(names) == THE_13 | both                # K1: private pad, no channel
    rc, names, _ = _probe(dict(CHANNEL_DECLARED, CK3_NOTEPAD_PADS="private,council", **NOTEPAD_DECLARED))
    assert rc == 0 and set(names) == THE_13 | {"msg_send"} | both   # COUNCIL
    rc, names, _ = _probe({"CK3_EPISODE_ARM": "k1", "CK3_CHANNEL_TOPOLOGY": "none",
                           "CK3_NOTEPAD_PADS": "none"})
    assert rc == 0 and set(names) == THE_13                       # declared off: absent
    rc, names, err = _probe({"CK3_NOTEPAD_PADS": "private"})
    assert rc == 3 and names == [] and "notepad_bounds_undeclared" in err


def test_no_retired_tool_or_handler_is_left_in_the_server():
    for gone in ("act", "game", "control", "bridge", "execute_console_command",
                 "intent", "observe_v2"):
        assert gone not in _defs()
    for attr in ("act", "game", "control", "bridge", "execute_console_command",
                 "intent", "observe_v2", "_envelope", "_error"):
        assert not hasattr(ms, attr), attr
    for attr in ("get_tool_definitions", "resolve_game", "resolve_control",
                 "resolve_intent", "_RETIRED_LEGACY_TOOLS", "OBSERVE_SECTIONS"):
        assert not hasattr(T, attr), attr
    for attr in ("TOOL_BRIDGE_ACTIONS", "FORMABLE_TITLE_CANDIDATES"):
        assert not hasattr(R, attr), attr


#: What redesign.py calls on a gateway. The fallbacks for a gateway lacking one
#: (D2) are deleted, so both gateway classes must carry every member.
GATEWAY_SURFACE = ("read", "read_many", "write", "confirmed_write",
                   "delta_records_since_last", "actor_gate", "game", "health", "clock")

#: L13 deleted these: the gateway never changes the clock, so it has no scope to
#: open, no polarity to look up and no tick to wait for.
GATEWAY_DELETED = ("read_unpaused", "read_inline", "unpaused_window", "_scoped_pause",
                   "_await_tick_advance", "sync_polarity_table", "_polarity",
                   "_classify_local", "Polarity", "_UNPAUSED_READS", "_ANY_ACTIONS")


def test_both_gateways_carry_everything_redesign_calls():
    agent = pytest.importorskip("agent")
    src = Path(R.__file__).read_text(encoding="utf-8")
    code = "\n".join(l.split("#", 1)[0] for l in src.splitlines())
    called = set(re.findall(r"\bgw\.([a-z_]+)", code))
    assert len(called) * 2 > len(GATEWAY_SURFACE), "the census is stale (fail OPEN)"
    assert called <= set(GATEWAY_SURFACE), sorted(called - set(GATEWAY_SURFACE))
    for cls in (ms.BridgeGateway, agent._AgentGateway):
        missing = [m for m in GATEWAY_SURFACE if not hasattr(cls, m)]
        assert not missing, (cls.__name__, missing)
        left = [m for m in GATEWAY_DELETED if hasattr(cls, m)]
        assert not left, (cls.__name__, left)


# ── the vocabularies ──────────────────────────────────────────────────────────

def test_do_intent_enum_is_INTENTS_and_every_intent_has_a_route():
    assert _enum("do", "intent") == list(R.INTENTS)
    dedicated = {R._CANCEL_PENDING_INTENT, R._FACTION_INTENT, R._POLICY_INTENT}
    assert set(R.INTENTS) == set(R.INTENT_TO_ACT) | dedicated
    for intent in R.INTENT_TO_ACT:
        resolved = T.resolve_act(R._intent_to_act_params(intent, {}, 1)) \
            if R.INTENT_TO_ACT[intent].get("type") != "war" else ("x", {})
        assert resolved is not None, f"intent {intent} resolves to no bridge action"
    src = Path(R.__file__).read_text(encoding="utf-8")
    fn = src[src.index("async def do_impl("):]
    for name in ("_CANCEL_PENDING_INTENT", "_FACTION_INTENT", "_POLICY_INTENT"):
        assert f"if intent == {name}:" in fn, f"dedicated intent {name} has no branch in do_impl"


def test_do_action_class_enum_is_ACTION_CLASSES():
    assert _enum("do", "action_class") == list(R.ACTION_CLASSES) == list(T.ACTION_CLASSES)


def test_inspect_subject_enum_is_every_subject_inspect_answers():
    assert _enum("inspect", "subject") == list(R.INSPECT_SUBJECTS)
    assert set(R.INSPECT_SUBJECTS) == set(R._INSPECT_SUBJECTS) | set(R._INSPECT_COMPOSED)
    src = Path(R.__file__).read_text(encoding="utf-8")
    fn = src[src.index("async def inspect_impl("):]
    for composed in R._INSPECT_COMPOSED:
        assert f'subject == "{composed}"' in fn, composed


def _session_branch_actions() -> set:
    tree = ast.parse(Path(R.__file__).read_text(encoding="utf-8"))
    fn = next(n for n in ast.walk(tree) if isinstance(n, ast.AsyncFunctionDef)
              and n.name == "_session_impl_branches")
    out = set()
    for n in ast.walk(fn):
        if (isinstance(n, ast.Compare) and isinstance(n.left, ast.Name)
                and n.left.id == "action" and len(n.ops) == 1
                and isinstance(n.ops[0], ast.Eq)
                and isinstance(n.comparators[0], ast.Constant)):
            out.add(n.comparators[0].value)
    return out


def test_session_action_enum_is_its_branches_and_has_add():
    branches = _session_branch_actions()
    assert branches, "the AST walk found no branches: the census is stale (fail OPEN)"
    assert set(R.SESSION_ACTIONS) == branches, sorted(set(R.SESSION_ACTIONS) ^ branches)
    assert "add" in R.SESSION_ACTIONS
    assert _enum("session", "action") == list(R.SESSION_ACTIONS)


def test_set_phase_observe_and_query_enums_are_their_constants():
    assert _enum("set_phase", "phase") == list(R.PHASES) == list(R.PHASE_GENOME)
    assert _enum("observe", "depth") == list(R.OBSERVE_DEPTHS)
    assert _enum("query", "domain") == list(T.QUERY_DOMAINS)
    assert _enum("query", "domains") == list(T.QUERY_DOMAINS)


def test_discover_what_enum_resolves_every_value_and_nothing_else():
    assert _enum("discover", "what") == list(T.DISCOVER_WHATS)
    for what in T._DISCOVER_RESOLVED:
        assert T.resolve_discover({"what": what}) is not None, what
    for what in T._DISCOVER_COMPOSED:
        assert f'what == "{what}"' in Path(ms.__file__).read_text(encoding="utf-8"), what
    # D8 (deleted 2026-10-20): the named-trigger evaluator is gone end to end.
    assert "evaluate_trigger" not in T.DISCOVER_WHATS
    assert "evaluate_trigger" not in _enum("discover", "what")
    assert T.resolve_discover({"what": "evaluate_trigger", "character_id": 7}) is None
    assert T.resolve_discover({"what": "no_such_thing"}) is None


def test_do_args_description_carries_the_table_read_from_the_code():
    import do_intent_args
    desc = _defs()["do"]["input_schema"]["properties"]["args"]["description"]
    table = do_intent_args.table_text()
    assert table in desc
    assert "take_decision: args.{decision}" in table


# ── the gates ─────────────────────────────────────────────────────────────────

def test_the_gate_unknown_is_retired_and_every_mapped_gate_exists():
    assert "unknown" not in R.GATE_TABLE
    assert set(R.RAW_REASON_TO_GATE.values()) <= set(R.GATE_TABLE), \
        sorted(set(R.RAW_REASON_TO_GATE.values()) - set(R.GATE_TABLE))
    assert {g for _, g in R.ENGINE_PROSE_TO_GATE} <= set(R.GATE_TABLE)
    for g in ("missing_required_argument", "unknown_verb", "unknown_subject",
              "unknown_domain", "bridge_disconnected", "read_timeout",
              "write_unconfirmed", "speed_refused_at_war",
              "experimental_write_disabled", "unmapped_refusal", "invalid_argument"):
        assert g in R.GATE_TABLE, g


def test_every_literal_gate_in_the_source_is_in_the_table():
    src = (Path(R.__file__).read_text(encoding="utf-8")
           + Path(ms.__file__).read_text(encoding="utf-8")
           + (Path(R.__file__).parent / "factions.py").read_text(encoding="utf-8"))
    literal = set(re.findall(r'why_blocked\(\s*"([a-z_]+)"', src))
    literal |= set(re.findall(r'_blocked\("[a-z_]+", state, "([a-z_]+)"', src))
    assert len(literal) * 4 > len(R.GATE_TABLE), \
        "the census found too few gates: the regex is stale (fail OPEN)"
    assert literal - set(R.GATE_TABLE) == set(), sorted(literal - set(R.GATE_TABLE))


def test_a_raw_reason_that_is_a_gate_name_maps_to_itself():
    assert R.map_raw_reason("target_truced")["gate"] == "target_truced"
    assert R.map_raw_reason("target_in_truce")["gate"] == "target_truced"


def test_an_unnamed_gate_reads_as_unmapped_never_a_neighbour():
    b = R.why_blocked("no_such_gate_name", summary="x")
    assert b["gate"] == "unmapped_refusal" and b["raw_reason"] == "no_such_gate_name"
    assert R.map_raw_reason("never_seen_before")["gate"] == "unmapped_refusal"
    assert R.map_raw_reason("")["gate"] == "unmapped_refusal"
    # a mapped code still resolves late, at the choke point
    assert R.why_blocked("unmapped_refusal", raw_reason="target_required")["gate"] != "unmapped_refusal"


def test_one_pending_spelling_and_code_detail_rows():
    with pytest.raises(ValueError):
        R.envelope("do", verdict="dispatched_pending")
    env = R.envelope("do", verdict="pending", warnings=["plain text"],
                     errors=[{"code": "read_timeout"}])
    assert env["warnings"] == [{"code": "note", "detail": "plain text"}]
    assert env["errors"][0]["code"] == "read_timeout" and env["errors"][0]["detail"]
    assert env["ok"] is False
    src = Path(R.__file__).read_text(encoding="utf-8")
    # The bridge's own status string is still READ as pending; no reply SAYS it.
    assert not re.search(r'verdict\s*=\s*[^,\n]*"dispatched_pending"', src)


# ── the speed rule ────────────────────────────────────────────────────────────

def test_speed_constants_mirror_the_bridge_clamp():
    cpp = (ROOT / "bridge" / "src" / "event_handler.cpp").read_text(encoding="utf-8")
    body = cpp[cpp.index("static void speed_primitive(int speed)"):]
    body = body[:body.index("\n}\n")]
    lo = re.search(r"if \(speed < (\d+)\) speed = (\d+);", body)
    hi = re.search(r"if \(speed > (\d+)\) speed = (\d+);", body)
    assert lo and hi, "speed_primitive's clamp moved: re-read it (fail OPEN)"
    assert int(lo.group(2)) == R.SPEED_MIN and int(hi.group(2)) == R.SPEED_MAX


def test_no_second_copy_of_the_speed_cap_in_the_server():
    for f in ("redesign.py", "mcp_server.py", "tools.py"):
        code = "\n".join(l.split("#", 1)[0] for l in (HERE / f).read_text(encoding="utf-8").splitlines())
        assert not re.search(r"speed\s*>=\s*4|min\(\s*speed\s*,\s*3\s*\)|speed\s*>\s*3\b", code), f


# ── driving the error paths ───────────────────────────────────────────────────

class Gw:
    """A scripted gateway: replies keyed by bridge action, writes recorded."""

    def __init__(self, replies=None, write_reply=None, char=None):
        self.replies = dict(replies or {})
        self.char = char if char is not None else {"id": 30853, "alive": True}
        self.reads, self.writes = [], []
        self.write_reply = write_reply if write_reply is not None else {"success": True}
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return self.char
        r = self.replies.get(action)
        return r(params) if callable(r) else r


    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        r = self.write_reply
        return r(action, params) if callable(r) else r

    async def delta_records_since_last(self):
        return []

    async def confirmed_write(self, action, params=None, *, readback, deadline_s=12.0,
                              max_polls=8):
        """The real two-wait rule (redesign.confirm_write) over this fake."""
        return await R.confirm_write(self.clock, self.read,
                                     lambda: self.write(action, params), readback,
                                     deadline_s=deadline_s, max_polls=max_polls)

    async def clock(self):
        return self.game()

    def game(self):
        return {"tick": 5, "date": "867.1.1", "paused": True, "speed": 1}


def _gate(env):
    assert env["ok"] is False, env
    g = env["blocked"]["gate"]
    assert g in R.GATE_TABLE and g != "unknown", g
    for row in env["warnings"] + env["errors"]:
        assert isinstance(row.get("code"), str) and isinstance(row.get("detail"), str), row
    return g


@pytest.mark.parametrize("call,gate", [
    (lambda gw: R.do_impl(gw), "missing_required_argument"),
    (lambda gw: R.do_impl(gw, intent="no_such_intent"), "unknown_verb"),
    (lambda gw: R.do_impl(gw, intent="cancel_pending"), "missing_required_argument"),
    (lambda gw: R.inspect_impl(gw, subject=""), "missing_required_argument"),
    (lambda gw: R.inspect_impl(gw, subject="nonsense", character_id=1), "unknown_subject"),
    (lambda gw: R.session_impl(gw, action=""), "missing_required_argument"),
    (lambda gw: R.session_impl(gw, action="frobnicate"), "unknown_verb"),
    (lambda gw: R.session_impl(gw, action="speed", speed=0), "invalid_argument"),
    (lambda gw: R.session_impl(gw, action="set_experimental"), "missing_required_argument"),
    (lambda gw: R.session_impl(gw, action="set_experimental", flag="nope"), "invalid_argument"),
    (lambda gw: R.set_phase_impl(gw, phase="berserk", character_id=30853), "invalid_argument"),
    (lambda gw: R.cbs_impl(gw, target_id=0), "missing_required_argument"),
    (lambda gw: R.declare_war_impl(gw), "missing_required_argument"),
])
def test_redesign_error_paths_return_named_gates(call, gate):
    assert _gate(run(call(Gw()))) == gate


def test_speed_above_the_cap_at_war_is_refused_by_name_and_clamped_at_peace():
    at_war = Gw({"get_active_wars": {"wars": [{"war_id": 1}]}})
    assert _gate(run(R.session_impl(at_war, action="speed", speed=5))) == "speed_refused_at_war"
    assert at_war.writes == []
    peace = Gw({"get_active_wars": {"wars": []}})
    env = run(R.session_impl(peace, action="speed", speed=5))
    assert env["ok"] is True and peace.writes == [("set_speed", {"speed": R.SPEED_MAX})]


def test_experimental_write_disabled_carries_the_operator_file():
    gw = Gw(write_reply={"success": False, "reason": "experimental_write_disabled",
                         "enabled_writes_file": "/state/ck3agent/ck3-enabled-writes"})
    env = run(R.do_impl(gw, intent="scheme", character_id=5, args={"scheme": "murder", "target_id": 3}))
    assert _gate(env) == "experimental_write_disabled"
    assert env["blocked"]["fix"]["enabled_writes_file"].endswith("ck3-enabled-writes")
    assert env["blocked"]["fix"]["who"] == "operator"


def test_cancel_pending_and_inspect_pending_replace_the_intent_tool():
    gw = Gw(write_reply={"success": True, "action": "cancel_intent", "intent_request_id": "r9"})
    env = run(R.do_impl(gw, intent="cancel_pending", args={"request_id": "r9"}))
    assert env["verdict"] == "applied" and gw.writes == [("cancel_intent", {"request_id": "r9"})]
    gw2 = Gw(write_reply={"success": False, "reason": "not_found_or_already_consumed"})
    assert run(R.do_impl(gw2, intent="cancel_pending", args={"request_id": "r9"}))["verdict"] == "rejected"
    gw3 = Gw({"query_intents": {"pending": [{"request_id": "r1"}], "pending_count": 1},
              "query_intent_results": {"results": [{"intent_id": "r0", "status": "success"}]}})
    env3 = run(R.inspect_impl(gw3, subject="pending"))
    assert env3["ok"] is True
    assert env3["result"]["queued"] == [{"request_id": "r1"}]
    assert env3["result"]["completed"][0]["status"] == "success"
    assert ("query_character", {"character_id": 0}) not in gw3.reads, "pending is not actor-scoped"


def test_F1_declare_war_with_character_id_0_matches_wars_against_the_resolved_actor():
    """character_id=0 used to compare each war's attacker_id against 0, so the
    idempotency check never fired; the resolved actor must be the one matched."""
    gw = Gw({"get_active_wars": {"wars": [{"war_id": 77, "attacker_id": 30853,
                                           "defender_id": 555, "cb_type": "claim_cb"}]}})
    env = run(R.declare_war_impl(gw, character_id=0, target_id=555, cb="claim_cb"))
    assert env["verdict"] == "applied" and env["result"]["war_id"] == 77, env
    assert env["warnings"][0]["code"] == "already_at_war"
    assert gw.writes == [], "nothing re-declared"
    assert ("get_active_wars", {"character_id": 30853}) in gw.reads


def test_F1_the_dispatched_war_carries_the_resolved_actor_not_0():
    """The other half of F1: the write itself must name the resolved ruler. With
    actor_id=0 on the wire the bridge refused (missing_required_argument /
    actor_id_required), which is what the live control run showed."""
    wars = {"wars": []}

    def landed(action, params):
        assert params.get("actor_id") == 30853, params
        wars["wars"].append({"war_id": 9, "attacker_id": 30853, "defender_id": 555,
                             "cb_type": "claim_cb"})
        return {"success": True}

    gw = Gw({"get_active_wars": lambda p: {"wars": list(wars["wars"])},
             "query_cb_validity_strict": {"results": [{"cb_name": "claim_cb",
                                                       "declarable": True}]}},
            write_reply=landed)
    env = run(R.declare_war_impl(gw, character_id=0, target_id=555, cb="claim_cb"))
    assert env["verdict"] == "applied" and env["result"]["war_id"] == 9, env
    assert len(gw.writes) == 1 and gw.writes[0][1]["actor_id"] == 30853, gw.writes


def test_F1_CONTROL_another_attackers_war_is_not_taken_for_ours():
    """The same war with a different attacker must NOT read as already declared:
    the call goes on to the strict pre-check, which here refuses it by name."""
    gw = Gw({"get_active_wars": {"wars": [{"war_id": 77, "attacker_id": 999,
                                           "defender_id": 555, "cb_type": "claim_cb"}]},
             "query_cb_validity_strict": {"results": [{"cb_name": "claim_cb",
                                                       "declarable": False,
                                                       "cull_reason": "target_in_truce"}]}})
    env = run(R.declare_war_impl(gw, character_id=0, target_id=555, cb="claim_cb"))
    assert env["verdict"] == "rejected" and _gate(env) == "target_truced", env
    assert gw.writes == []


# ── the mcp_server tools (query, discover, steer, pass_turn and the gate in front)

class Wired:
    def __init__(self, state):
        self.state, self.replies, self.calls = state, {}, []


@pytest.fixture
def wired(monkeypatch):
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 9, "867.2.1"
    # This seat holds one ruler, 30853, so an id-less call resolves to it
    # (redesign.actor_gate_of reads both: do_get_controlled_roster's `roster`,
    # Session::to_json's row under get_bridge_state.sessions).
    state._client = type("C", (), {"session_id": "s-wired"})()
    seat = {"get_controlled_roster": {"action": "get_controlled_roster",
                                      "primary_character_id": 30853, "roster": [30853],
                                      "player_character_id": 0},
            "get_bridge_state": {"action": "get_bridge_state", "sessions": [
                {"session_id": "s-wired", "primary_character_id": 30853,
                 "claimed_character_ids": [30853]}]}}
    w = Wired(state)
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def qb(_state, action, params, timeout=15.0):
        if action == "get_tick_info":     # the clock reading every tool takes at entry (m-16)
            return {"paused": True, "game_speed": 1}
        if action in seat and action not in w.replies:
            return seat[action]
        w.calls.append(action)
        r = w.replies.get(action)
        if isinstance(r, BaseException):
            raise r
        if r is None and action == "query_character":
            return {"id": 30853, "alive": True}     # build_char_profile's shape
        return r if r is not None else {"character_id": 30853, "action": action}

    async def ab(_state, action, params, timeout=5.0):
        if action == "get_tick_info":     # the clock reading every tool takes at entry (m-16)
            return {"paused": True, "game_speed": 1}
        w.calls.append(action)
        r = w.replies.get(action)
        return r if r is not None else {"success": True}

    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_action_bridge", ab)
    return w


def test_query_returns_the_one_envelope(wired):
    env = run(ms.query(domain="realm"))
    assert env["ok"] is True and env["tool"] == "query"
    assert env["result"]["action"] == "query_realm"
    assert env["actor"] == {"character_id": 30853}
    assert env["game"] == {"tick": 9, "date": "867.2.1", "paused": True, "speed": 1}
    assert "data" not in env and "error" not in env


@pytest.mark.parametrize("kw,gate", [
    ({}, "missing_required_argument"),
    ({"domain": "no_such_domain"}, "unknown_domain"),
])
def test_query_error_paths(wired, kw, gate):
    assert _gate(run(ms.query(**kw))) == gate


def test_query_timeout_and_disconnect_are_named(wired):
    wired.replies["query_realm"] = TimeoutError("late")
    assert _gate(run(ms.query(domain="realm"))) == "read_timeout"
    wired.replies["query_realm"] = ConnectionError("gone")
    assert _gate(run(ms.query(domain="realm"))) == "bridge_disconnected"


def test_a_batch_with_a_failed_domain_is_not_a_success(wired):
    wired.replies["query_wars"] = TimeoutError("late")
    env = run(ms.query(domains=["realm", "wars", "no_such_domain"]))
    assert env["ok"] is False and env["blocked"] is None
    codes = sorted(e["code"] for e in env["errors"])
    assert codes == ["read_timeout", "unknown_domain"], env["errors"]
    assert "realm" in env["result"]


@pytest.mark.parametrize("kw,gate", [
    ({}, "missing_required_argument"),
    ({"what": "no_such_subject"}, "unknown_subject"),
    ({"what": "targets", "action": "marry"}, "unknown_verb"),
    ({"what": "targets", "action": "claim"}, "missing_required_argument"),
])
def test_discover_error_paths(wired, kw, gate):
    assert _gate(run(ms.discover(**kw))) == gate


def test_steer_write_the_bridge_did_not_confirm_is_named(wired):
    wired.replies["set_ai_personality"] = {"status": "sent_no_confirmation"}
    env = run(ms.steer(personality={"boldness": 10}))
    # L14 finding 5: the write envelope; not confirmed and not read back is pending
    assert env.get("verdict") == "pending" and env["pending"]["drains_when"] == "next_pass", env


def test_pass_turn_returns_the_one_envelope(wired):
    env = run(ms.pass_turn(reason="holding"))
    assert env["ok"] is True and env["tool"] == "pass_turn"
    assert env["result"] == {"action": "pass_turn", "reason": "holding"}


def test_every_tool_names_a_lost_bridge(monkeypatch):
    state = ms.BridgeState()
    state.connected, state.mock_mode = False, False
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def no(_s):
        return False
    monkeypatch.setattr(ms, "_try_reconnect", no)
    for name in sorted(THE_13 - {"pass_turn"}):
        env = run(getattr(ms, name)())
        assert _gate(env) == "bridge_disconnected", name
        assert env["tool"] == name


# ── NIT-3 (L14 witness r4): the idempotency CB match is real ─────────────────
# get_active_wars rows carry cb_type (query_handlers.cpp do_get_active_wars,
# war_cb_type_name; test_observe_war_truth pins the emitted key).

def _war(cb=None, war_id=77):
    w = {"war_id": war_id, "attacker_id": 30853, "defender_id": 555, "is_attacker": True}
    if cb is not None:
        w["cb_type"] = cb
    return w


def test_NIT3_a_war_under_the_same_cb_answers_applied_without_a_write():
    gw = Gw({"get_active_wars": {"wars": [_war("claim_cb")]}})
    env = run(R.declare_war_impl(gw, character_id=30853, target_id=555, cb="claim_cb"))
    assert env["verdict"] == "applied" and env["warnings"][0]["code"] == "already_at_war"
    assert gw.writes == []


@pytest.mark.parametrize("row_cb", ["county_conquest_cb", None])
def test_NIT3_a_war_under_another_or_an_unread_cb_is_not_already_at_war(row_cb):
    """A war vs the target under ANOTHER CB, or one whose CB was not read, is
    not this declaration: it is dispatched, and the readback does not hand the
    old war back as the one declared (before: `applied` on any attacker war)."""
    gw = Gw({"get_active_wars": {"wars": [_war(row_cb)]}},
            write_reply={"success": False, "reason": "invalid_cb"})
    env = run(R.declare_war_impl(gw, character_id=30853, target_id=555, cb="claim_cb",
                                 confirm_straddle=True))
    assert not any(w.get("code") == "already_at_war" for w in env["warnings"]), env
    assert env.get("verdict") != "applied", env
    assert [a for a, _ in gw.writes], "the declaration was dispatched"


def test_NIT3_the_readback_does_not_take_a_pre_existing_war_under_another_cb():
    gw = Gw({"get_active_wars": {"wars": [_war("county_conquest_cb")]}})
    env = run(R.declare_war_impl(gw, character_id=30853, target_id=555, cb="claim_cb",
                                 confirm_straddle=True))
    assert env.get("verdict") != "applied" or env["result"]["war_id"] != 77, env


# ── m7 (L15 witness): the idempotency keys on the CB title; a failed pre-read refuses ──
# get_active_wars rows carry target_titles (query_handlers.cpp do_get_active_wars,
# read_war_target_titles over war_obj::CB_TITLE_ARRAY, the read query_wars makes).

def _titled(titles, cb="county_conquest_cb", war_id=77):
    w = _war(cb, war_id)
    if titles is not None:
        w["target_titles"] = list(titles)
    return w


def test_m7_a_war_for_the_same_cb_title_answers_applied_without_a_write():
    gw = Gw({"get_active_wars": {"wars": [_titled([1201])]}})
    env = run(R.declare_war_impl(gw, character_id=30853, target_id=555,
                                 cb="county_conquest_cb", cb_title_id=1201))
    assert env["verdict"] == "applied" and env["warnings"][0]["code"] == "already_at_war", env
    assert gw.writes == []


@pytest.mark.parametrize("titles", [[1300], [], None], ids=["another", "empty", "unread"])
def test_m7_a_war_for_another_or_an_unread_title_is_not_already_at_war(titles):
    """RED on 03f19352: a declaration for another title against a target already
    at war under the same CB answered `applied` with the old war."""
    gw = Gw({"get_active_wars": {"wars": [_titled(titles)]}},
            write_reply={"success": False, "reason": "invalid_cb"})
    env = run(R.declare_war_impl(gw, character_id=30853, target_id=555,
                                 cb="county_conquest_cb", cb_title_id=1201, confirm_straddle=True))
    assert not any(w.get("code") == "already_at_war" for w in env["warnings"]), env
    assert env.get("verdict") != "applied", env
    assert [a for a, _ in gw.writes], "the declaration was dispatched"


@pytest.mark.parametrize("cb", ["tribal_subjugation_cb", "vassalization_cb", "make_tributary_cb"])
def test_m7_a_mode0_cb_names_no_title_and_matches_on_the_cb(cb):
    """L15 witness n5: make_tributary_cb is mode 0 too (target_mode 0, live rows
    target_titles []); RED on 16dd7f14 for it: a nonzero cb_title_id could never
    match its war, and the declaration was dispatched again."""
    gw = Gw({"get_active_wars": {"wars": [_titled([], cb=cb)]}})
    env = run(R.declare_war_impl(gw, character_id=30853, target_id=555,
                                 cb=cb, cb_title_id=1201))
    assert env["verdict"] == "applied" and gw.writes == [], env


def test_n5_a_mode0_cb_is_dispatched_with_no_title():
    """make_tributary_cb is marshalled as mode 0: cb_title_id 0. (The claimant is
    not asserted: tools.resolve_act's steered war fills claimant_id 0 with the
    actor for every CB, before and after this change.)"""
    gw = Gw({"get_active_wars": {"wars": []}}, write_reply={"success": False, "reason": "invalid_cb"})
    run(R.declare_war_impl(gw, character_id=30853, target_id=555, cb="make_tributary_cb",
                           cb_title_id=1201, confirm_straddle=True))
    assert gw.writes, "the declaration was dispatched"
    assert gw.writes[0][1].get("cb_title_id") == 0, gw.writes[0][1]


@pytest.mark.parametrize("reply, gate", [
    (None, "read_timeout"),
    ({}, "read_timeout"),
    ({"error": "game_thread_timeout", "action": "get_active_wars"}, "read_timeout"),
    ({"success": False, "action": "get_active_wars"}, "unmapped_refusal"),
    ({"action": "get_active_wars", "war_count": 0}, "unmapped_refusal"),
], ids=["none", "empty", "timeout", "declined", "no_list"])
def test_m7_a_failed_pre_read_refuses_and_sends_nothing(reply, gate):
    """RED on 03f19352: a failed get_active_wars read was "no wars": the war cap
    and the idempotency check were skipped and the war was dispatched."""
    gw = Gw({"get_active_wars": reply})
    env = run(R.declare_war_impl(gw, character_id=30853, target_id=555, cb="claim_cb",
                                 confirm_straddle=True))
    assert env["ok"] is False and env["blocked"]["gate"] == gate, env
    assert gw.writes == [], "nothing was declared"
    # the control: the same call over a read that came back is dispatched
    ok = Gw({"get_active_wars": {"wars": [], "war_count": 0}})
    run(R.declare_war_impl(ok, character_id=30853, target_id=555, cb="claim_cb",
                           confirm_straddle=True))
    assert ok.writes, "a read that came back lets the declaration through"
