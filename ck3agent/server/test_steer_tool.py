"""steer() and pass_turn(): behaviour against a fake bridge (cleanup 2026.11 F15).

steer rows:
  * read mode (no personality): one read, with character_id; no verdict;
  * write mode answers the write envelope (L14 finding 5): exactly the axes
    passed are written, applied when they read back as written, rejected
    write_unconfirmed when they do not, pending next_pass when the bridge did not
    confirm it and a read does not show it, rejected with the gate its reason
    names when the bridge refused;
  * refusals before anything is written: a key that is not an axis (dominance
    among them: no engine byte, no getter), a value that is not an integer in
    range (invalid_argument, verdict rejected, nothing sent); a read that failed
    is a named refusal; no bridge is bridge_disconnected;
  * the one Python axis table (personality_axes.AXES, with its range) is the
    bridge's (personality_locks.h), checked, and redesign serves that same table;
  * the tool takes character_id and personality and nothing else: the subsystem
    toggles, a record the game's AI never read, are deleted end to end.

pass_turn rows: the envelope, the reason in the action log, the default reason,
and that it sends nothing to the bridge but the clock reading every tool takes at
entry (m-16), so it answers with no bridge at all.
"""

import asyncio
import re
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402
import personality_axes as PA  # noqa: E402


def run(coro):
    return asyncio.run(coro)


class Bridge:
    def __init__(self, state):
        self.state, self.replies, self.sent = state, {}, []
        self.clock_reads = 0

    def answer(self, path, action, params, timeout):
        if action == "get_tick_info":     # the clock reading every tool takes at entry (m-16)
            self.clock_reads += 1
            return {"paused": True, "game_speed": 1}
        self.sent.append((path, action, dict(params)))
        r = self.replies.get(action, {"success": True, "action": action})
        if callable(r):
            r = r(params)
        if isinstance(r, BaseException):
            raise r
        return r


@pytest.fixture
def bridge(monkeypatch):
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 7, "867.1.5"
    b = Bridge(state)
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def qb(_s, action, params, timeout=15.0):
        return b.answer("query", action, params, timeout)

    async def ab(_s, action, params, timeout=5.0):
        return b.answer("action", action, params, timeout)

    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_action_bridge", ab)
    return b


def refused(env, gate):
    assert env["ok"] is False and env["tool"] == "steer", env
    assert env["blocked"]["gate"] == gate, env["blocked"]
    return env["blocked"]


# ── read mode ─────────────────────────────────────────────────────────────────

ME = 812


def _seat(bridge, cid=ME):
    bridge.replies["query_character"] = {"id": cid, "name": "seat", "alive": True}


@pytest.mark.parametrize("kw", [{}, {"personality": {}}, {"personality": None}])
def test_no_values_reads_the_personality(bridge, kw):
    _seat(bridge)
    bridge.replies["get_ai_personality"] = {"character_id": ME, "boldness": 40}
    env = run(ms.steer(character_id=ME, **kw))
    assert env["ok"] is True and env["tool"] == "steer" and env["blocked"] is None
    assert env["result"] == {"personality": {"character_id": ME, "boldness": 40}}
    assert [a for p, a, q in bridge.sent if a != "query_character"
            and a != "get_controlled_roster"] == ["get_ai_personality"]
    assert ("query", "get_ai_personality", {"character_id": ME}) in bridge.sent
    assert env["actor"]["id"] == ME
    assert "verdict" not in env, "a read has no verdict"


@pytest.mark.parametrize("reply,gate", [
    (TimeoutError("late"), "read_timeout"),
    ({"success": False, "reason": "character_not_found"}, "character_not_found"),
])
def test_a_read_that_failed_is_a_named_refusal(bridge, reply, gate):
    _seat(bridge)
    bridge.replies["get_ai_personality"] = reply
    env = run(ms.steer(character_id=ME))
    assert env["ok"] is False and env["blocked"]["gate"] == gate, env
    assert "personality" not in env["result"], "a refusal is not shown as the reading"


# ── write mode: the write envelope (L14 finding 5) ────────────────────────────

def _back(**axes):
    return {"success": True, "action": "set_ai_personality", "character_id": 30853,
            "axes_written": len(axes), "personality": dict(axes)}


def test_a_write_sends_exactly_the_axes_passed_and_is_applied_on_its_readback(bridge):
    _seat(bridge, 30853)
    bridge.replies["set_ai_personality"] = _back(boldness=60, greed=-20, honor=0)
    env = run(ms.steer(character_id=30853, personality={"boldness": 60, "greed": -20}))
    assert env["ok"] is True and env["verdict"] == "applied", env
    assert ("action", "set_ai_personality",
            {"character_id": 30853, "boldness": 60, "greed": -20}) in bridge.sent
    assert [a for _, a, _ in bridge.sent].count("set_ai_personality") == 1
    assert env["result"]["readback"] == {"axes": {"boldness": 60, "greed": -20}}
    assert env["result"]["personality"]["axes_written"] == 3


def test_a_write_whose_axes_do_not_read_back_is_rejected_unconfirmed(bridge):
    _seat(bridge, 30853)
    bridge.replies["set_ai_personality"] = _back(boldness=12)
    bridge.replies["get_ai_personality"] = {"personality": {"boldness": 12}}
    env = run(ms.steer(character_id=30853, personality={"boldness": 60}))
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "write_unconfirmed", env


def test_a_write_the_bridge_did_not_confirm_is_pending_with_its_read(bridge):
    _seat(bridge)
    bridge.replies["set_ai_personality"] = {"status": "sent_no_confirmation"}
    bridge.replies["get_ai_personality"] = {"personality": {"boldness": 3}}
    env = run(ms.steer(character_id=ME, personality={"boldness": 10}))
    assert env["ok"] is True and env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "next_pass"
    assert env["pending"]["verify_with"] == "steer()"
    # and when the read shows it, it is applied
    bridge.replies["get_ai_personality"] = {"personality": {"boldness": 10}}
    assert run(ms.steer(character_id=ME, personality={"boldness": 10}))["verdict"] == "applied"


@pytest.mark.parametrize("reply,gate", [
    ({"success": False, "reason": "axis_out_of_range", "axis": "boldness"}, "invalid_argument"),
    ({"success": False, "reason": "character_not_found"}, "character_not_found"),
    ({"success": False, "reason": "cannot_act_as_player"}, "cannot_act_as_player"),
    ({"success": False, "action": "set_ai_personality"}, "unmapped_refusal"),
])
def test_a_write_the_bridge_refused_is_rejected_with_its_gate(bridge, reply, gate):
    _seat(bridge)
    bridge.replies["set_ai_personality"] = reply
    env = run(ms.steer(character_id=ME, personality={"boldness": 10}))
    assert env["ok"] is False and env["verdict"] == "rejected", env
    assert env["blocked"]["gate"] == gate, env["blocked"]
    assert gate in R.GATE_TABLE


# ── refused before anything is written ────────────────────────────────────────

@pytest.mark.parametrize("personality,needle", [
    ({"bravery": 50}, "bravery"),
    ({"dominance": 10}, "dominance"),
    ({"boldness": 50, "Boldness": 10}, "Boldness"),
    ({"boldness": 101}, "boldness=101"),
    ({"greed": -101}, "greed=-101"),
    ({"zeal": "high"}, "zeal='high'"),
    ({"zeal": 5.5}, "zeal=5.5"),
    ({"honor": True}, "honor=True"),
])
def test_a_bad_axis_is_refused_and_nothing_is_written(bridge, personality, needle):
    _seat(bridge)
    env = run(ms.steer(character_id=ME, personality=personality))
    b = refused(env, "invalid_argument")
    assert env["verdict"] == "rejected"
    assert needle in b["summary"] and "Nothing was written" in b["summary"]
    assert "set_ai_personality" not in [a for _, a, _ in bridge.sent]


@pytest.mark.parametrize("value", [R.AXIS_MIN, R.AXIS_MAX, 0])
def test_control_the_range_ends_are_written(bridge, value):
    _seat(bridge)
    bridge.replies["set_ai_personality"] = {"success": True, "personality": {"energy": value}}
    env = run(ms.steer(character_id=ME, personality={"energy": value}))
    assert env["ok"] is True and env["verdict"] == "applied", env


def test_no_bridge_is_bridge_disconnected(bridge, monkeypatch):
    bridge.state.connected = False

    async def no(_s):
        return False
    monkeypatch.setattr(ms, "_try_reconnect", no)
    refused(run(ms.steer(personality={"boldness": 1})), "bridge_disconnected")
    assert bridge.sent == []


# ── the axes are the bridge's ─────────────────────────────────────────────────

def _bridge_src(rel):
    return (ROOT / "bridge" / rel).read_text(encoding="utf-8")


def test_the_axes_mirror_the_bridge_table():
    src = _bridge_src("include/personality_locks.h")
    table = src[src.index("PERSONALITY_AXIS_NAMES[PERSONALITY_AXIS_COUNT] = {"):]
    table = table[:table.index("};")]
    names = tuple(re.findall(r'"([a-z]+)"', table))
    assert len(names) == 10, f"the axis table moved: re-read it (fail OPEN): {names}"
    engine = re.search(r"PERSONALITY_ENGINE_AXES = (\d+);", src)
    assert engine, "PERSONALITY_ENGINE_AXES moved: re-read it (fail OPEN)"
    # the axes are the engine-backed names; the lock-only tail is no axis here
    assert names[:int(engine.group(1))] == PA.AXES
    assert "dominance" in names[int(engine.group(1)):] and "dominance" not in PA.AXES
    # One table: steer reads it through redesign (personality_nudge's own
    # import is checked in test_personality_nudge.py, which does not ship).
    assert R.AXES is PA.AXES


def test_no_bridge_source_keeps_its_own_axis_table():
    # Every bridge reader and writer names the axes from personality_locks.h.
    for path in sorted((ROOT / "bridge" / "src").glob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        assert '"vengefulness"' not in text, f"{path.name} spells the axes again"


def _python_axis_tables(roots):
    """Every list/tuple/set literal, in the Python files under roots, whose string
    elements name all ten axes, as (path, line). Parses each file (a comment or a
    docstring cannot satisfy it); a file that does not parse is reported, never
    skipped (fail OPEN)."""
    import ast
    axes = set(PA.AXES)
    found = []
    for root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*.py")):
            if "__pycache__" in path.parts:
                continue
            try:
                tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"))
            except SyntaxError as e:
                found.append((str(path), f"unparsable: {e}"))
                continue
            for node in ast.walk(tree):
                if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
                    names = {e.value for e in node.elts
                             if isinstance(e, ast.Constant) and isinstance(e.value, str)}
                    if axes <= names:
                        found.append((str(path.relative_to(ROOT)), node.lineno))
    return found


def test_no_python_source_keeps_its_own_axis_table():
    """personality_axes.AXES is the one Python table: no other list, tuple or set
    of the axis names in ck3agent/, scripts/ or re/coredump/."""
    roots = [ROOT / "ck3agent", ROOT / "scripts", ROOT / "re" / "coredump"]
    found = [f for f in _python_axis_tables(roots)
             if f[0] != "ck3agent/server/personality_axes.py"]
    assert not found, found


def test_control_the_axis_table_scan_finds_the_definition_and_a_planted_copy():
    defs = _python_axis_tables([HERE])
    assert any(p == "ck3agent/server/personality_axes.py" for p, _ in defs), defs
    planted = HERE / "_axis_copy_probe.py"
    planted.write_text("X = " + repr(list(reversed(PA.AXES))) + "\n", encoding="utf-8")
    try:
        assert any(p == "ck3agent/server/_axis_copy_probe.py" for p, _ in
                   _python_axis_tables([HERE]))
    finally:
        planted.unlink()


def test_the_range_mirrors_the_bridge_rejection():
    hdr = _bridge_src("include/personality_locks.h")
    lo = re.search(r"PERSONALITY_AXIS_MIN = (-?\d+);", hdr)
    hi = re.search(r"PERSONALITY_AXIS_MAX = (-?\d+);", hdr)
    assert lo and hi, "the range constants moved: re-read them (fail OPEN)"
    assert (int(lo.group(1)), int(hi.group(1))) == (PA.AXIS_MIN, PA.AXIS_MAX)
    assert (R.AXIS_MIN, R.AXIS_MAX) == (PA.AXIS_MIN, PA.AXIS_MAX)
    src = _bridge_src("src/personality.cpp")
    fn = src[src.index("bool do_set_ai_personality("):]
    assert "if (val < PERSONALITY_AXIS_MIN || val > PERSONALITY_AXIS_MAX)" in fn, \
        "the out-of-range rejection no longer reads the header's range: re-read it"


def test_every_phase_genome_uses_only_the_axes():
    for phase, genome in R.PHASE_GENOME.items():
        assert set(genome) <= set(R.AXES), phase


def test_the_description_names_every_axis_and_no_toggle():
    tool = {t["name"]: t for t in ms.registered_tool_definitions()}["steer"]
    for axis in R.AXES:
        assert axis in tool["description"], axis
    assert "toggle" not in tool["description"].lower()
    assert "subsystem" not in tool["description"].lower()
    assert set(tool["input_schema"]["properties"]) == {"character_id", "personality"}


def test_the_toggle_lever_is_gone_from_the_bridge_and_the_server():
    """The per-character subsystem mask was a record the game's AI never read
    (its replies said engine_enforced: false). It is deleted end to end: no
    bridge action, no handler source, and no server call to either verb."""
    registry = (ROOT / "bridge" / "src" / "action_registry.inc").read_text(encoding="utf-8")
    assert "character_subsystems" not in registry
    assert not (ROOT / "bridge" / "src" / "ai_subsystem_hooks.cpp").exists()
    # agent.py (the retired loop) is in the research tree only; the public
    # export leaves it out, so it is checked where it exists.
    names = ["mcp_server.py", "redesign.py", "tools.py"]
    names += [n for n in ("agent.py",) if (HERE / n).exists()]
    for name in names:
        assert "character_subsystems" not in (HERE / name).read_text(encoding="utf-8"), name


# ── pass_turn ─────────────────────────────────────────────────────────────────

def test_pass_turn_records_the_reason_and_sends_nothing(bridge):
    before = len(bridge.state.action_log)
    env = run(ms.pass_turn(reason="holding the truce"))
    assert env["ok"] is True and env["tool"] == "pass_turn" and env["blocked"] is None
    assert env["result"] == {"action": "pass_turn", "reason": "holding the truce"}
    assert env["game"] == {"tick": 7, "date": "867.1.5", "paused": True, "speed": 1}
    assert bridge.state.action_log[before:] == [
        {"tick": 7, "action": {"type": "pass_turn", "reason": "holding the truce"}}]
    assert bridge.sent == [] and bridge.clock_reads == 1, (bridge.sent, bridge.clock_reads)


def test_pass_turn_without_a_reason_says_so(bridge):
    env = run(ms.pass_turn())
    assert env["result"]["reason"] == "maintaining stability"
    assert bridge.state.action_log[-1]["action"]["reason"] == ""


def test_pass_turn_answers_with_no_bridge(monkeypatch):
    state = ms.BridgeState()
    state.connected, state.mock_mode = False, False
    monkeypatch.setattr(ms, "_get_state", lambda: state)
    env = run(ms.pass_turn(reason="x"))
    assert env["ok"] is True and env["bridge_health"] == "hung"
