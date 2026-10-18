"""ck-zgh4 deploy-51 item (4) — tell each seat what it controls, truthfully.

THE EVIDENCE. d49's king wrote "I never had hands on the army" in its own
retrospective. d50 read `native_ai_disabled: true` 210 times — and that field is
`bool(disable_native_ai)`, THE PARAMETER THE CALLER PASSED, echoed back as
though it were a measured state. Seats then spent cycles interrogating it: 188
`control` and 183 `whoami` calls in arm A, 4-9% of all calls, asking a question
the instrument answered with their own input.

THE TRAP THESE TESTS EXIST FOR. There are TWO fields called `ai_enabled` and
they are not the same fact:

  query_war_stance    resolves g_get_ai_ctrl(character) — the SAME per-character
                      controller set_character_ai writes through
                      (action_handlers.cpp:3477) — and reads AI_ENABLED off it.
                      PER-CHARACTER. This is the one.
  get_ai_evaluations  reads `core_data + AI_CONTROLLER`, a SINGLE GLOBAL
                      controller (query_handlers.cpp:7493). Reporting this
                      per-character would repeat the very defect being fixed,
                      with a different field name.

So the control below is not decoration: it drives a gateway where the two
disagree and asserts the block follows the per-character one.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")


class Gateway:
    def __init__(self, war_stance=None, raise_on_stance=False):
        self.reads: list[tuple[str, dict]] = []
        self._ws = war_stance
        self._raise = raise_on_stance
        self.health = {"ok": True}

    async def multi_actor(self):
        return False

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_war_stance":
            if self._raise:
                raise RuntimeError("bridge said no")
            return self._ws
        if action == "get_ai_evaluations":
            # the GLOBAL controller, deliberately disagreeing with the per-char one
            return {"ai_controller": {"ai_enabled": True}}
        if action == "query_character":
            # a BOUND ruler: since rc2 B6 whoami blocks no_ruler_bound when
            # character_id=0 resolves to nothing, and carries no agency then.
            return {"character": {"id": 30853, "alive": True}}
        return {}

    async def write(self, action, params=None):
        return {"ok": True}

    def game(self):
        return {"tick": 1, "game_date": "1066.09.15"}


def _block(**kw):
    gw = Gateway(**kw)
    return gw, asyncio.run(redesign.agency_block(gw, 30853))


def test_the_block_states_the_three_STATIC_facts():
    _, b = _block(war_stance={"ai_enabled": True, "ai_suspended": False})
    assert "the game's own AI" in b["armies_are_moved_by"]
    assert "posture" in b["armies_are_moved_by"]
    assert "do" in b["you_act_directly_through"]
    assert "declare_war" in b["you_act_directly_through"]
    assert "session(action='control')" in b["at_your_death"]


def test_ai_tick_is_MEASURED_from_the_PER_CHARACTER_read():
    gw, b = _block(war_stance={"ai_enabled": True, "ai_suspended": False})
    assert b["ai_tick"]["measured"] is True
    assert b["ai_tick"]["enabled"] is True
    assert b["ai_tick"]["suspended"] is False
    assert "query_war_stance" in b["ai_tick"]["source"]
    assert ("query_war_stance", {"character_id": 30853}) in gw.reads


def test_CONTROL_the_GLOBAL_controller_is_NOT_used_even_when_it_DISAGREES():
    """THE ROW THAT MATTERS. The gateway reports the global controller as
    enabled and the per-character one as DISABLED. A block that read the global
    field — which looks like the same fact — would say True and be wrong in the
    exact way `native_ai_disabled` was wrong."""
    gw, b = _block(war_stance={"ai_enabled": False, "ai_suspended": False})
    assert b["ai_tick"]["enabled"] is False, (
        "the block followed the GLOBAL ai_controller instead of the "
        "per-character ai_ctrl")
    assert "get_ai_evaluations" not in [a for a, _ in gw.reads], (
        "the block read the global-controller action at all; it must not, "
        "because that field is not per-character")


def test_UNREADABLE_is_a_STATE_and_says_WHY_not_a_guess():
    for kw in ({"war_stance": {}}, {"war_stance": None}, {"raise_on_stance": True}):
        _, b = _block(**kw)
        t = b["ai_tick"]
        assert t["measured"] is False
        assert t["enabled"] is None and t["suspended"] is None
        assert t["why_not"], f"no reason given for {kw}"


def test_whoami_CARRIES_the_block():
    """Driven through the real session impl, not asserted of the helper alone."""
    gw = Gateway(war_stance={"ai_enabled": True, "ai_suspended": False})
    env = asyncio.run(redesign._session_impl_branches(
        gw, action="whoami", character_id=0, disable_native_ai=False)) \
        if hasattr(redesign, "_session_impl_branches") else None
    if env is None:
        pytest.skip("_session_impl_branches not exposed under this name")
    res = (env or {}).get("result") or {}
    assert "agency" in res, "session(whoami) must carry the agency block"
    assert res["agency"]["ai_tick"]["measured"] is True


def test_the_PARAMETER_ECHO_is_named_as_a_REQUEST_not_a_state():
    """`native_ai_disabled` was the caller's own argument wearing a state's
    name. It is renamed; the old name must not come back."""
    import inspect
    src = inspect.getsource(redesign)
    assert '"native_ai_disabled"' not in src, (
        "the parameter echo is still named as though it were measured state")
    assert '"native_ai_disable_requested"' in src
