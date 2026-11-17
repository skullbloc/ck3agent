"""fix10 defect 7: a write the GAME refused this game-month is answered with the
earlier refusal (`repeat_of`) and not sent again; plan_war stops offering it.

Evidence (v4 pilot 2, council-a K transcript): declare_war on William 31808 for
title 7127 (individual_duchy_de_jure_cb) refused cb_invalid_for_target at
22:25:48Z, and plan_war at 22:25:50Z offered the same declaration again; it was
sent and refused again at 22:51:07Z and 00:38:19Z. 7146 and 7808 likewise.

These drive the real declare_war / plan_war tool bodies (mcp_server) with the
redesign impls stubbed to the refusal the game gave, counting how many times
the impl (the bridge) is reached. Red on 5e506f28: refusal_memo does not exist,
and every repeat reaches the bridge.
"""
import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mcp_server  # noqa: E402
import redesign  # noqa: E402
import refusal_memo  # noqa: E402

ARGS = {"target_id": 31808, "cb": "individual_duchy_de_jure_cb", "cb_title_id": 7127, "claimant_id": None}


def _refusal(date, gate="cb_invalid_for_target"):
    return redesign.envelope("declare_war", ok=False,
                             blocked=redesign.why_blocked(gate, summary=gate),
                             game={"tick": 1, "date": date, "paused": True, "speed": 2}) | {"verdict": "rejected"}


def _run(coro):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


@pytest.fixture
def world(monkeypatch):
    clock = {"date": "1070.08.26"}
    calls = {"declare": 0}
    replies = {"declare": lambda: _refusal(clock["date"])}

    async def connected(state, tool):
        return None

    class GW:
        async def clock(self):
            return None

    async def gw_now():
        return GW()

    async def declare_impl(gw, **kw):
        calls["declare"] += 1
        return replies["declare"]()

    async def plan_impl(gw, **kw):
        return redesign.envelope("plan_war", result={"targets": [{"declare_args": dict(ARGS), "target_name": "William"},
                                                                 {"declare_args": {**ARGS, "cb_title_id": 7114}}],
                                                     "excluded": []},
                                 game={"tick": 1, "date": clock["date"], "paused": True, "speed": 2})

    monkeypatch.setattr(mcp_server, "_ensure_connected", connected)
    monkeypatch.setattr(mcp_server, "_gateway_now", gw_now)
    monkeypatch.setattr(mcp_server, "_get_state", lambda: None)
    monkeypatch.setattr(mcp_server, "_state_game", lambda st: {"date": clock["date"]})
    monkeypatch.setattr(redesign, "declare_war_impl", declare_impl)
    monkeypatch.setattr(redesign, "plan_war_impl", plan_impl)
    monkeypatch.setattr(mcp_server, "_REFUSALS", refusal_memo.Memo())
    return clock, calls, replies


def _declare(**kw):
    fn = getattr(mcp_server.declare_war, "fn", mcp_server.declare_war)
    return _run(fn(declare_args=dict(ARGS), **kw))


def _plan():
    fn = getattr(mcp_server.plan_war, "fn", mcp_server.plan_war)
    return _run(fn())


def test_an_identical_declaration_refused_this_month_is_not_sent_again(world):
    clock, calls, _ = world
    first = _declare()
    assert first["verdict"] == "rejected" and calls["declare"] == 1
    clock["date"] = "1070.08.30"                       # same game-month
    again = _declare()
    assert calls["declare"] == 1, "the repeat reached the game"
    assert again["repeat_of"]["game_date"] == "1070.08.26"
    assert again["blocked"]["gate"] == "cb_invalid_for_target"
    assert any(w.get("code") == "repeat_of_refused_write" for w in again["warnings"])


def test_a_new_game_month_sends_it_again(world):
    """Control: the game's answer may change; the memory is per game-month."""
    clock, calls, _ = world
    _declare()
    clock["date"] = "1070.09.02"
    out = _declare()
    assert calls["declare"] == 2 and "repeat_of" not in out


def test_a_different_title_is_a_different_write(world):
    clock, calls, _ = world
    _declare()
    fn = getattr(mcp_server.declare_war, "fn", mcp_server.declare_war)
    _run(fn(declare_args={**ARGS, "cb_title_id": 7146}))
    assert calls["declare"] == 2


def test_a_bridge_refusal_is_not_remembered(world):
    """Control: a refusal the bridge made (class bridge: owned_by_other) is not
    the game's answer and is sent again."""
    clock, calls, replies = world
    replies["declare"] = lambda: _refusal(clock["date"], gate="owned_by_other")
    _declare()
    _declare()
    assert calls["declare"] == 2


def test_plan_war_withholds_a_declaration_refused_this_month(world):
    clock, calls, _ = world
    _declare()
    out = _plan()
    res = out["result"]
    assert [r["declare_args"]["cb_title_id"] for r in res["targets"]] == [7114]
    ex = [e for e in res["excluded"] if e["blocked"]["gate"] == "refused_this_month"]
    assert len(ex) == 1 and ex[0]["declare_args"]["cb_title_id"] == 7127
    assert ex[0]["repeat_of"] == {"game_date": "1070.08.26", "gate": "cb_invalid_for_target"}
    clock["date"] = "1070.09.01"
    assert len(_plan()["result"]["targets"]) == 2      # offered again next month


def test_game_month_reads_game_dates_only():
    assert refusal_memo.game_month("1070.8.26") == "1070.08"
    assert refusal_memo.game_month("unknown") is None and refusal_memo.game_month(None) is None


def test_a_refused_offer_through_do_is_not_sent_again(world, monkeypatch):
    """K1: make_peace white_peace re-sent after `target_would_reject`
    (gate recipient_refuses). do keys on actor, intent and its args."""
    clock, _calls, _ = world
    n = {"do": 0}

    async def do_impl(gw, **kw):
        n["do"] += 1
        return redesign.envelope("do", ok=False, blocked=redesign.why_blocked("recipient_refuses", summary="x"),
                                 game={"tick": 1, "date": clock["date"], "paused": True, "speed": 2}) | {"verdict": "rejected"}
    monkeypatch.setattr(redesign, "do_impl", do_impl)
    fn = getattr(mcp_server.do, "fn", mcp_server.do)
    args = {"war_id": 5, "terms": "white_peace"}
    _run(fn(intent="make_peace", args=dict(args)))
    out = _run(fn(intent="make_peace", args=dict(args)))
    assert n["do"] == 1 and out["repeat_of"]["game_date"] == "1070.08.26"
    _run(fn(intent="make_peace", args={**args, "terms": "surrender"}))
    assert n["do"] == 2


def test_a_repeat_says_nothing_was_sent_by_it(world, monkeypatch):
    """Lane QTRIAGE, live qf1 (Q3s, 2026-11-15): the third answer to a void offer
    came back from the memo as the SECOND call's envelope, `result.sent: true`
    and its token SACT_45, in 0.001 s, beside a warning saying "not sent". A
    reply that disagrees with itself: the replay must say that THIS call sent
    nothing (`result.sent` false) and keep what the earlier call did under
    `repeat_of` (its token and its sent)."""
    clock, _calls, _ = world
    n = {"do": 0}

    async def do_impl(gw, **kw):
        n["do"] += 1
        return redesign.envelope("do", ok=False,
                                 blocked=redesign.why_blocked("interaction_invalid", summary="void"),
                                 result={"template": "proposal_answer", "token": "SACT_45", "sent": True,
                                         "void": {"reason": "invalid_now"}},
                                 game={"tick": 1, "date": clock["date"], "paused": True, "speed": 2}
                                 ) | {"verdict": "rejected"}
    monkeypatch.setattr(redesign, "do_impl", do_impl)
    fn = getattr(mcp_server.do, "fn", mcp_server.do)
    args = {"proposal_id": 5, "answer": "accept"}
    first = _run(fn(intent="answer_proposal", args=dict(args)))
    again = _run(fn(intent="answer_proposal", args=dict(args)))
    assert n["do"] == 1 and first["result"]["sent"] is True
    assert again["result"]["sent"] is False, again["result"]
    assert again["result"]["token"] is None
    assert again["repeat_of"]["earlier"] == {"token": "SACT_45", "sent": True}
    assert again["result"]["void"] == {"reason": "invalid_now"}      # the earlier answer's reading is kept


def test_a_repeat_of_a_reply_without_a_sent_field_adds_none(world, monkeypatch):
    """A result that never said whether it was sent gains no `sent` key (absence
    stays absence); `repeat_of.earlier` says so."""
    clock, _calls, _ = world
    fn = getattr(mcp_server.declare_war, "fn", mcp_server.declare_war)
    _run(fn(**ARGS))
    again = _run(fn(**ARGS))
    assert "sent" not in (again.get("result") or {})
    assert again["repeat_of"]["earlier"] == {"token": None, "sent": "not_stated"}
