"""rc2 B6: nothing bound must not read as ok=true.

Before: with no ruler bound, query_character(character_id=0) failed, and
_resolve_actor never checked. actor_echo's defaults (alive=True,
is_controlled=True) and zeroed standing came back as the ruler's state with
ok=True. whoami echoed {id: 0, is_controlled: True} the same way.

The declined reply below is the wire shape, not a guess: do_query_character
returns false with no response, dispatch_action_with_response fills
{"success": false, "action": ...}, and the IPC layer prepends request_id,
tick and game_date (dispatch_router.cpp, ipc_server.cpp).
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

redesign = pytest.importorskip("redesign")

DECLINED = {"request_id": "r1", "tick": 5, "game_date": "867.1.1",
            "success": False, "action": "query_character"}
BOUND = {"request_id": "r2", "character": {"id": 30853, "name": "Seat", "alive": True}}


class Gw:
    def __init__(self, char_reply=None, sessions=None, sid=None, write_reply=None):
        self._char = char_reply
        self._sessions = sessions
        self.reads, self.writes = [], []
        self._write_reply = write_reply if write_reply is not None else {"success": True}
        self.health = "ok"
        if sid is not None:
            self.state = type("S", (), {"_client": type("C", (), {"session_id": sid})()})()

    async def multi_actor(self):
        return False

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return self._char
        if action == "get_bridge_state" and self._sessions is not None:
            return {"controlled_primary": 0, "sessions": self._sessions}
        return None

    async def read_many(self, specs):
        return [None for _ in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self._write_reply

    def game(self):
        return {"tick": 5, "date": "867.1.1", "paused": True, "speed": 1}


def run(coro):
    return asyncio.run(coro)


# ── observe_v2 / every actor-bearing tool through _resolve_actor ────────────

def test_observe_v2_unbound_is_blocked_no_ruler_bound():
    out = run(redesign.observe_impl(Gw(DECLINED), character_id=0))
    assert out["ok"] is False
    assert out["blocked"]["gate"] == "no_ruler_bound"
    assert out["blocked"]["retryable"] is False
    assert out["result"] == {}, "no standing may be reported for an unbound seat"
    assert "actor" not in out, "no actor echo for nothing"


def test_observe_v2_with_no_reply_is_unresolved_not_unbound():
    """The verifier's probe: every read returns None. That is 'looked and could
    not see', a different defect from 'nothing bound'."""
    out = run(redesign.observe_impl(Gw(None), character_id=0))
    assert out["ok"] is False
    assert out["blocked"]["gate"] == "actor_unresolved"
    assert out["blocked"]["measured"] == {"reply": "absent"}


def test_an_error_reply_keeps_its_code():
    out = run(redesign.observe_impl(Gw({"error": "memory_access_fault"}), character_id=0))
    assert out["blocked"]["gate"] == "actor_unresolved"
    assert out["blocked"]["raw_reason"] == "memory_access_fault"


def test_set_phase_unbound_writes_nothing():
    gw = Gw(DECLINED)
    out = run(redesign.set_phase_impl(gw, phase="expansion", character_id=0))
    assert out["blocked"]["gate"] == "no_ruler_bound"
    assert gw.writes == []


def test_CONTROL_a_bound_ruler_resolves():
    """Non-discriminating control: passes before and after the fix."""
    actor, err = run(redesign._resolve_actor(Gw(BOUND), 0, "observe_v2"))
    assert err is None and actor["id"] == 30853


def test_CONTROL_an_explicit_id_is_not_judged_by_the_unbound_rule():
    actor, err = run(redesign._resolve_actor(Gw(DECLINED), 777, "observe_v2"))
    assert err is None and actor["id"] == 777


# ── whoami ───────────────────────────────────────────────────────────────────

def test_whoami_seat_holding_nothing_is_blocked():
    gw = Gw(DECLINED, sid="me",
            sessions=[{"session_id": "me", "primary_character_id": 0,
                       "claimed_character_ids": []}])
    out = run(redesign.session_impl(gw, "whoami"))
    assert out["ok"] is False
    assert out["blocked"]["gate"] == "no_ruler_bound"
    assert out["result"]["controlled"] is None
    assert out["result"]["session_id"] == "me"
    assert out["result"]["my_claims"] == []


def test_whoami_seat_unknown_and_declined_is_blocked():
    out = run(redesign.session_impl(Gw(DECLINED), "whoami"))
    assert out["blocked"]["gate"] == "no_ruler_bound"
    assert out["result"]["controlled"] is None


def test_whoami_seat_unknown_but_resolved_is_labelled_not_claimed():
    out = run(redesign.session_impl(Gw(BOUND), "whoami"))
    assert out["ok"] is True
    assert out["result"]["controlled"]["is_controlled"] is None
    assert any(w.startswith("seat_unknown") for w in out["warnings"])


def test_CONTROL_whoami_bound_seat():
    gw = Gw(BOUND, sid="me",
            sessions=[{"session_id": "me", "primary_character_id": 30853,
                       "claimed_character_ids": [30853]}])
    out = run(redesign.session_impl(gw, "whoami"))
    assert out["ok"] is True and out["blocked"] is None
    assert out["result"]["controlled"]["id"] == 30853
    assert out["result"]["controlled"]["is_controlled"] is True


# ── control with no id used to unbind silently ──────────────────────────────

def test_control_without_an_id_is_refused_and_sends_nothing():
    gw = Gw(BOUND, write_reply={"success": True, "controlled_primary": 0,
                                "note": "reverted to engine player character"})
    out = run(redesign.session_impl(gw, "control", character_id=0))
    assert out["ok"] is False
    assert out["blocked"]["gate"] == "missing_character_id"
    assert gw.writes == [], "the unbind must not reach the bridge"


# ── set_phase: the veto is process-wide, and says so ────────────────────────

def test_set_phase_expansion_discloses_veto_scope_and_records_the_write():
    redesign._FLAG_WRITES.clear()
    out = run(redesign.set_phase_impl(Gw(BOUND), phase="expansion", character_id=0))
    res = out["result"]
    assert res["offgoal_veto_armed"] is True
    assert "process-wide" in res["offgoal_veto_scope"]
    flags = run(redesign.session_impl(Gw(BOUND), "experimental_flags"))["result"]["flags"]
    veto = next(f for f in flags if f["name"] == "off_goal_veto")
    assert veto["live"] is None
    assert veto["last_written_here"] == {"enabled": True, "by": "set_phase('expansion')"}


def test_veto_refusal_warning_names_no_removed_pulse():
    out = run(redesign.set_phase_impl(
        Gw(BOUND, write_reply={"error": "unknown_action"}), phase="expansion", character_id=0))
    w = next(w for w in out["warnings"] if w["code"] == "offgoal_veto_unavailable")
    assert "pulse" not in w["detail"]
    assert out["result"]["offgoal_veto_armed"] is False


# ── round 4: set_phase counts the veto armed only on the bridge's word ──────

@pytest.mark.parametrize("reply", [
    {"success": False, "action": "set_offgoal_veto", "reason": "not_owned_by_session",
     "actor_id": 30853, "session_id": "s1"},          # dispatch_router.cpp ownership refusal
    {"reason": "bridge_disconnected"},
    {"status": "sent_no_confirmation", "action": "set_offgoal_veto"},
    {"success": True, "action": "set_offgoal_veto", "enabled": False},   # echo disagrees
], ids=["not_owned", "disconnected", "no_confirmation", "echo_false"])
def test_unconfirmed_veto_is_not_armed_and_not_recorded(reply):
    redesign._FLAG_WRITES.clear()
    out = run(redesign.set_phase_impl(Gw(BOUND, write_reply=reply), phase="expansion",
                                      character_id=0))
    assert out["result"]["offgoal_veto_armed"] is False
    assert "off_goal_veto" not in redesign._FLAG_WRITES
    assert any(w["code"] == "offgoal_veto_unavailable" for w in out["warnings"])
    flags = run(redesign.session_impl(Gw(BOUND), "experimental_flags"))["result"]["flags"]
    assert next(f for f in flags if f["name"] == "off_goal_veto")["last_written_here"] is None


def test_confirmed_veto_is_armed_and_flagged_experimental():
    redesign._FLAG_WRITES.clear()
    out = run(redesign.set_phase_impl(
        Gw(BOUND, write_reply={"success": True, "action": "set_offgoal_veto", "enabled": True}),
        phase="expansion", character_id=0))
    assert out["result"]["offgoal_veto_armed"] is True
    assert any(w["code"] == "offgoal_veto_experimental" for w in out["warnings"])
    assert redesign._FLAG_WRITES["off_goal_veto"]["enabled"] is True


@pytest.mark.parametrize("reply", [
    {"reason": "bridge_disconnected"},
    {"status": "sent_no_confirmation"},
    {"success": False, "reason": "not_owned_by_session"},
], ids=["disconnected", "no_confirmation", "not_owned"])
def test_set_experimental_disarm_unconfirmed_is_not_applied(reply):
    """The old check read a missing `enabled` as 0 == False, so any reply to a
    disarm counted as applied."""
    redesign._FLAG_WRITES.clear()
    out = run(redesign.session_impl(Gw(BOUND, write_reply=reply), "set_experimental",
                                    flag="off_goal_veto", enabled=False))
    assert out.get("verdict") != "applied"
    assert "off_goal_veto" not in redesign._FLAG_WRITES
    if reply.get("success") is False:
        assert out["ok"] is False


def test_flag_scope_is_stated_beside_last_written_here():
    flags = run(redesign.session_impl(Gw(BOUND), "experimental_flags"))["result"]["flags"]
    assert all("null does not mean off" in f["last_written_here_scope"] for f in flags)


# ── round 4: whoami says WHY the seat is unbound ─────────────────────────────

def _seat(**row):
    base = {"session_id": "me", "primary_character_id": 0, "claimed_character_ids": []}
    base.update(row)
    return Gw(DECLINED, sid="me", sessions=[base])


def test_whoami_surfaces_heir_is_player():
    out = run(redesign.session_impl(
        _seat(unbound_reason="heir_is_player", unbound_heir_id=6001), "whoami"))
    assert out["blocked"]["gate"] == "no_ruler_bound"
    m = out["blocked"]["measured"]
    assert m["unbound_reason"] == "heir_is_player" and m["unbound_heir_id"] == 6001
    assert "human" in out["blocked"]["summary"] and "6001" in out["blocked"]["summary"]


def test_whoami_unbound_with_no_reason_does_not_claim_the_human_inherited():
    out = run(redesign.session_impl(_seat(unbound_reason="", unbound_heir_id=0), "whoami"))
    m = out["blocked"]["measured"]
    assert m["unbound_reason"] == ""
    assert "human" not in out["blocked"]["summary"]
    assert "reconnected" in out["blocked"]["summary"]


def test_whoami_row_without_the_key_is_unreported_not_empty():
    out = run(redesign.session_impl(_seat(), "whoami"))
    m = out["blocked"]["measured"]
    assert m["unbound_reason"] is None and m["unbound_heir_id"] is None


class _PlayerGw(Gw):
    """get_bridge_state reports player_character_id; this seat's primary may equal it."""

    def __init__(self, *a, player=30853, **k):
        super().__init__(*a, **k)
        self._player = player

    async def read(self, action, params=None):
        if action == "get_bridge_state":
            self.reads.append((action, dict(params or {})))
            return {"controlled_primary": 30853, "player_character_id": self._player,
                    "sessions": self._sessions}
        return await super().read(action, params)


_HOLDS_30853 = [{"session_id": "me", "primary_character_id": 30853,
                 "claimed_character_ids": [30853]}]


def test_whoami_ruler_became_the_players_is_blocked():
    """The reverse succession: the human inherited this seat's ruler."""
    out = run(redesign.session_impl(_PlayerGw(BOUND, sid="me", sessions=_HOLDS_30853), "whoami"))
    assert out["ok"] is False
    assert out["blocked"]["gate"] == "cannot_act_as_player"
    assert out["result"]["controlled"] is None


def test_CONTROL_whoami_observer_sentinel_is_not_a_match():
    gw = _PlayerGw(BOUND, sid="me", sessions=_HOLDS_30853, player=0xFFFFFF)
    out = run(redesign.session_impl(gw, "whoami"))
    assert out["ok"] is True and out["result"]["controlled"]["id"] == 30853


# ── round 4: inspect's actor-scoped subjects block while unbound ────────────

class _DeclineAll(Gw):
    """Every read declines, as the bridge does for character_id=0 with nothing bound."""

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        return dict(DECLINED, action=action)


@pytest.mark.parametrize("subject", ["war_score", "war_stance", "succession_preview",
                                     "title", "realm", "character"])
def test_inspect_unbound_is_blocked(subject):
    gw = _DeclineAll()
    out = run(redesign.inspect_impl(gw, subject=subject, character_id=0))
    assert out["ok"] is False
    assert out["blocked"]["gate"] == "no_ruler_bound"
    assert [a for a, _ in gw.reads] == ["query_character"], "no subject read for nothing"


def test_inspect_composed_subjects_still_block_through_resolve_actor():
    for subject in ("heir_plan", "guardian_candidates"):
        out = run(redesign.inspect_impl(_DeclineAll(), subject=subject, character_id=0))
        assert out["blocked"]["gate"] == "no_ruler_bound", subject


def test_CONTROL_inspect_explicit_id_is_not_judged_by_the_unbound_rule():
    gw = _DeclineAll()
    out = run(redesign.inspect_impl(gw, subject="realm", character_id=777))
    assert out["ok"] is True
    assert gw.reads == [("query_realm", {"character_id": 777})]


def test_CONTROL_inspect_unknown_subject_is_unknown_not_unbound():
    out = run(redesign.inspect_impl(_DeclineAll(), subject="nonsense", character_id=0))
    assert out["blocked"]["gate"] == "unknown"


# ── round 4: no shipped text cites a console deny-list ──────────────────────

def test_scripted_effect_refusal_cites_no_deny_list():
    out = run(redesign._do_scripted_effect(Gw(BOUND), {"effect": "add_gold", "execute": True},
                                           30853, {"id": 30853}))
    assert out["ok"] is False
    assert "deny-list" not in out["blocked"]["summary"]
    src = open(redesign.__file__, encoding="utf-8").read()
    assert "console deny-list" not in src and "execute_console_command deny-list" not in src


# ── round 4: the docs name exactly the tools that block while unbound ───────
#
# One list, two readers: BLOCKS is checked against the code by DRIVING each
# tool with nothing bound, and against each doc by reading the paragraph that
# names them. A tool added to the MCP surface with a character_id must be
# classified here, or the census reds.

BLOCKS = {
    "observe_v2": lambda gw: redesign.observe_impl(gw, character_id=0),
    "inspect": lambda gw: redesign.inspect_impl(gw, subject="war_score", character_id=0),
    "plan_war": lambda gw: redesign.plan_war_impl(gw, character_id=0),
    "declare_war": lambda gw: redesign.declare_war_impl(gw, character_id=0, target_id=1,
                                                        cb="claim_cb"),
    "cbs": lambda gw: redesign.cbs_impl(gw, target_id=1, character_id=0),
    "succession_plan": lambda gw: redesign.succession_plan_impl(gw, character_id=0),
    "set_phase": lambda gw: redesign.set_phase_impl(gw, phase="expansion", character_id=0),
    "do": lambda gw: redesign.do_impl(gw, intent="pass", character_id=0),
}
UNCHECKED = {"observe", "query", "discover", "steer", "intent"}
# character_id there names the ruler to control or release; whoami is driven above.
NOT_ACTOR_SCOPED = {"session"}

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# The per-tool list moved from getting-started to troubleshooting in the
# pre-release docs review (getting-started keeps one plain sentence).
DOCS = [("release/overlay/docs/troubleshooting.md", "docs/troubleshooting.md"),
        ("release/overlay/docs/reference/bridge-contract.md", "docs/reference/bridge-contract.md"),
        ("release/overlay/ck3agent/protocol/CLAUDE.md", "ck3agent/protocol/CLAUDE.md"),
        ("release/overlay/ck3agent/protocol/CLAUDE-desktop.md", "ck3agent/protocol/CLAUDE-desktop.md")]


def _mcp_tools_with_an_actor_param() -> set:
    import re
    src = open(os.path.join(os.path.dirname(__file__), "mcp_server.py"), encoding="utf-8").read()
    out = set()
    for m in re.finditer(r"^@mcp\.tool\(\)\s*\n(?:async )?def (\w+)\((.*?)\)\s*->", src, re.M | re.S):
        if "character_id" in m.group(2) or "actor_id" in m.group(2):
            out.add(m.group(1))
    return out


@pytest.mark.parametrize("tool", sorted(BLOCKS))
def test_each_listed_tool_blocks_unbound(tool):
    gw = Gw(DECLINED)
    out = run(BLOCKS[tool](gw))
    assert out["ok"] is False and out["blocked"]["gate"] == "no_ruler_bound", tool
    assert gw.writes == [], tool


def test_every_tool_with_an_actor_param_is_classified():
    tools = _mcp_tools_with_an_actor_param()
    assert tools, "the census found no tools: the decorator regex is stale (fail OPEN)"
    assert tools - set(BLOCKS) - UNCHECKED - NOT_ACTOR_SCOPED == set(), \
        "classify it in BLOCKS or UNCHECKED"
    assert (set(BLOCKS) | UNCHECKED | NOT_ACTOR_SCOPED) - tools == set(), "a listed tool is gone"


def _unbound_paragraph(text: str) -> str:
    blocks = [b for b in text.split("\n\n") if "`observe_v2`" in b and "ok: true" in b]
    assert len(blocks) == 1, f"expected one paragraph naming the unbound behaviour, got {len(blocks)}"
    return blocks[0]


@pytest.mark.parametrize("pair", DOCS, ids=[p[0].rsplit("/", 1)[-1] for p in DOCS])
def test_docs_name_exactly_the_blocking_and_unchecked_tools(pair):
    import re
    path = next((os.path.join(ROOT, p) for p in pair if os.path.isfile(os.path.join(ROOT, p))), None)
    assert path, f"not found in the source or export layout: {pair}"
    para = _unbound_paragraph(open(path, encoding="utf-8").read())
    known = set(BLOCKS) | UNCHECKED
    named = set(re.findall(r"`([a-z_0-9]+)`", para)) & known
    sentence = next(s for s in re.split(r"(?<=\.)\s", para) if "ok: true" in s)
    unchecked = set(re.findall(r"`([a-z_0-9]+)`", sentence)) & known
    assert unchecked == UNCHECKED, (path, sorted(unchecked ^ UNCHECKED))
    assert named - unchecked == set(BLOCKS), (path, sorted((named - unchecked) ^ set(BLOCKS)))
    # The blocking sentence ALONE must equal BLOCKS: a tool listed as both
    # blocking and ok:true survives the set difference above.
    blocking = [s for s in re.split(r"(?<=\.)\s", para)
                if "`observe_v2`" in s and "ok: true" not in s]
    assert len(blocking) == 1, (path, "expected one sentence naming the blocking tools", blocking)
    listed = set(re.findall(r"`([a-z_0-9]+)`", blocking[0])) & known
    assert listed == set(BLOCKS), (path, sorted(listed ^ set(BLOCKS)))


# ── an empty unbound_reason is not evidence that no handoff happened ────────
#
# unbound_reason lives on the session row only (session.cpp
# unbind_for_player_heir); in free play a disconnect DESTROYS the row
# (ipc_server.cpp, episode_pins_claims() false), so heir_is_player followed by
# an MCP restart reads "" on the new seat. The protocol must not tell Claude to
# rebind its old ruler on "" without checking that ruler is alive.

PROTOCOL = [("release/overlay/ck3agent/protocol/CLAUDE.md", "ck3agent/protocol/CLAUDE.md"),
            ("release/overlay/ck3agent/protocol/CLAUDE-desktop.md",
             "ck3agent/protocol/CLAUDE-desktop.md")]


def _flat(pair) -> str:
    path = next((os.path.join(ROOT, p) for p in pair if os.path.isfile(os.path.join(ROOT, p))), None)
    assert path, f"not found in the source or export layout: {pair}"
    return " ".join(open(path, encoding="utf-8").read().split())


@pytest.mark.parametrize("pair", PROTOCOL, ids=[p[0].rsplit("/", 1)[-1] for p in PROTOCOL])
def test_empty_reason_is_not_called_no_handoff(pair):
    text = _flat(pair).lower()
    assert "no handoff happened" not in text, pair[0]
    assert "rebind the ruler you had" not in text, pair[0]
    assert "rebind your ruler" not in text, pair[0]


@pytest.mark.parametrize("pair", PROTOCOL, ids=[p[0].rsplit("/", 1)[-1] for p in PROTOCOL])
def test_empty_reason_rebind_is_gated_on_a_liveness_check(pair):
    import re
    text = _flat(pair)
    # The paragraph that handles an empty reason.
    m = re.search(r"[Ee]mpty[^.]*?(?:reason|:)(.*?)(?=If `whoami` returns|- `cannot_act_as_player`|$)",
                  text)
    assert m, (pair[0], "no instruction for an empty unbound_reason")
    rule = m.group(0)
    assert "handoff before an MCP restart" in rule, (pair[0], "must name the restart case", rule)
    assert 'query(domain="character", character_id=<id>)' in rule, (pair[0], "no liveness read", rule)
    assert "alive" in rule and "rebind it only if so" in rule, (pair[0], rule)
    assert "ask the human" in rule, (pair[0], "no fallback when the ruler is dead", rule)
    # The check comes BEFORE the rebind.
    assert rule.index("query(domain=") < rule.index("rebind"), (pair[0], rule)
