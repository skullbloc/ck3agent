"""ck-9rto — no branch of session_impl may say "applied" over a reply that
does not support it. The CLASS, asserted once, for every verb.

Three of us fixed instances of one pattern and stopped. ck-zgh4 found `control`;
I found `release`; four more were in the same 218 lines the whole time — `speed`
and `auto_events` carrying `raw: res` into a result whose verdict contradicts
it, `unpause` re-reading gw.game() so only its verdict was wrong, and `pause`
assigning the reply to `res`, never mentioning `res` again, and hard-coding
`{"paused": True}`.

IT IS REACHABLE, NOT THEORETICAL. All six verbs are in `_ANY_ACTIONS`, and for
those `gw.write` answers a timeout with `{"status": "sent_no_confirmation"}` —
the bridge's explicit "I do not know whether this landed". `pause` turned that
into `verdict="applied", result={"paused": True}`: the one shape the bridge
takes trouble to report honestly was the one the caller overwrote with a
literal.

This test would have failed on six branches before the control fix and on four
before the guard. Four more `if not ok` blocks would have closed four holes and
left the seventh branch to whoever adds it.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")

#: Every action of session_impl that WRITES, with the kwargs it needs. If a
#: seventh write verb is added and not listed here, `test_the_write_verbs_are_ALL
#: _listed` reds — the list is checked against the source, not maintained on
#: trust.
WRITE_ACTIONS = {
    "control":     {"character_id": 30853},
    "release":     {"character_id": 30853},
    "speed":       {"speed": 3},
    "pause":       {},
    "unpause":     {},
    "auto_events": {"enabled": True},
}


class _RefusingGw:
    """Answers every write with the reply under test and every read with
    something harmless, so only the write's treatment is being measured."""

    def __init__(self, reply):
        self._reply = reply
        self.health = "ok"

    async def write(self, action, params=None):
        return dict(self._reply)

    async def read(self, action, params=None):
        if action == "query_character":
            return {"character_id": (params or {}).get("character_id"), "name": "X"}
        if action == "get_active_wars":
            return {"wars": []}
        return {}

    async def _await_tick_advance(self, timeout_s=3.0):
        """The real gateway has this and `unpause` calls it. A double that
        omits it does not model the shipped surface, and the AttributeError it
        raises would read as a failure of the thing under test."""
        return True

    def game(self):
        return {"tick": 1, "date": "1066.09.15", "paused": True, "speed": 3}


def _run(action, reply):
    gw = _RefusingGw(reply)
    return asyncio.run(redesign.session_impl(gw, action, **WRITE_ACTIONS[action]))


@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_a_REFUSED_write_never_reports_applied(action):
    out = _run(action, {"success": False, "reason": "owned_by_other"})
    assert out.get("verdict") != "applied", (action, out)
    assert out.get("ok") is False, (action, out)
    assert out.get("blocked"), (action, out)


@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_an_UNCONFIRMED_write_never_reports_applied(action):
    """`sent_no_confirmation` is what gw.write returns when an _ANY_ACTIONS
    write times out. It is neither a refusal nor an application, and the
    vocabulary already has the word: confirmation_pending — VERIFY, do not
    re-declare."""
    out = _run(action, {"status": "sent_no_confirmation", "action": action})
    assert out.get("verdict") != "applied", (action, out)
    assert out["blocked"]["gate"] == "confirmation_pending", (action, out["blocked"])
    assert out["blocked"]["retryable"] is False, (
        "re-declaring an unconfirmed write is how a double-declare happens; the "
        "agent must VERIFY instead")


@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_a_WITNESSED_write_is_untouched_by_the_guard(action):
    """The flip that makes the two above non-vacuous: the same verbs, a reply
    that DOES support the claim, and the guard changes nothing."""
    out = _run(action, {"success": True})
    assert out.get("ok", True) is True, (action, out)
    assert out.get("verdict") == "applied", (action, out)
    assert not out.get("blocked"), (action, out)


def test_the_write_verbs_are_ALL_listed():
    """The list above is checked against the SOURCE rather than maintained on
    trust: every `gw.write` inside the branch function must belong to a branch
    this test drives. A seventh write verb reds here."""
    import ast
    import inspect
    src = inspect.getsource(redesign._session_impl_branches)
    tree = ast.parse(src.lstrip())
    written = set()
    for node in ast.walk(tree):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and node.func.attr == "write" and node.args
                and isinstance(node.args[0], ast.Constant)):
            written.add(node.args[0].value)
    # the bridge verbs each listed action sends
    expected = {"set_controlled_character", "release_character", "set_speed",
                "pause_game", "unpause_game", "set_auto_respond_events"}
    assert written == expected, (
        "the set of bridge verbs session_impl writes has changed — add the new "
        f"action to WRITE_ACTIONS so the guard is proved over it too: {written ^ expected}")


def test_the_guard_is_applied_ONCE_and_not_per_branch():
    """The shape of the fix is the point. `session_impl` is a thin wrapper that
    runs the branches and applies one rule to whatever they return, so a branch
    added tomorrow is covered without its author knowing this rule exists."""
    import ast
    import inspect

    def calls_to(fn, name):
        tree = ast.parse(inspect.getsource(fn).lstrip())
        return sum(1 for n in ast.walk(tree)
                   if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
                   and n.func.id == name)

    # CALLS, not mentions — the wrapper's docstring names the guard as well, and
    # counting the string said 2 where the behaviour is 1.
    assert calls_to(redesign.session_impl, "unwitnessed_applied") == 1
    assert "_session_impl_branches" in inspect.getsource(redesign.session_impl)
    assert calls_to(redesign._session_impl_branches, "unwitnessed_applied") == 0, (
        "the guard has leaked into the branches — that is the per-instance shape "
        "this replaced")


def test_EVERY_write_is_considered_not_only_the_last():
    """ck-zgh4's structural note, closed rather than documented.

    Every branch performs exactly one write today, so a last-write-wins slot
    would be CORRECT — and "one guard, one recorded reply" would be a
    load-bearing assumption nothing states. A branch that ever wrote twice would
    leave the FIRST reply unwitnessed, and the source check cannot catch that:
    it checks the write VERBS against the branches it drives, so `pause_game`
    then `set_speed` uses two verbs already listed and reds nothing.

    The guard's contract is tested directly, because the branch that would
    exercise it does not exist yet — which is the point: this row exists so the
    branch can be written safely, not because something is broken."""
    applied = {"ok": True, "verdict": "applied", "result": {"done": True}}

    # the FIRST write refused, the second succeeded: a last-write-wins slot sees
    # only the success and lets `applied` stand
    out = redesign.unwitnessed_applied(applied, [
        ("pause_game", {"success": False, "reason": "no_session"}),
        ("set_speed", {"success": True}),
    ])
    assert out.get("verdict") != "applied", out
    assert out["blocked"]["raw_reason"] == "no_session", out["blocked"]

    # an unconfirmed FIRST write, likewise
    out = redesign.unwitnessed_applied(applied, [
        ("pause_game", {"status": "sent_no_confirmation", "action": "pause_game"}),
        ("set_speed", {"success": True}),
    ])
    assert out["blocked"]["gate"] == "confirmation_pending", out

    # and the flip: every write fine, guard changes nothing
    out = redesign.unwitnessed_applied(applied, [
        ("pause_game", {"success": True}), ("set_speed", {"success": True})])
    assert out == applied


def test_the_FIRST_failing_reply_is_the_one_reported():
    """Two failures in one branch: the earliest wins, because everything after
    it ran on a premise that had already broken."""
    out = redesign.unwitnessed_applied(
        {"ok": True, "verdict": "applied"},
        [("pause_game", {"success": False, "reason": "no_session"}),
         ("set_speed", {"success": False, "reason": "observe_mode"})])
    assert out["blocked"]["raw_reason"] == "no_session", out["blocked"]
