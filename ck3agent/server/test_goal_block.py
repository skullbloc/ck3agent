"""Lane v4sees: what an experiment seat's `observe` says about the goal, the
resume-prompt delivery of its messages, and observe during a stall.

Seat review of set v3-registered-2, problem 5: no score, no end date and no
cycle count reached a seat. Every row drives `mcp_server.observe` with the game
doubled (the bridge replies, keyed as the emitter writes them:
query_handlers.cpp build_char_profile `id`/`alive`, emit_played_line,
do_query_realm `character_id`/`has_realm`/`realm_total_counties`); the goal
path itself is not doubled.

Red on 8cef520a: goal_block does not exist; observe has no `goal`; a stalled
bridge's observe reads ok true; channel.deliver takes no `via`.
"""
import asyncio
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import channel as C  # noqa: E402
import cycle_cadence as cc  # noqa: E402
import goal_block as G  # noqa: E402

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402

K_ID, D_ID, HEIR = 12015, 13586, 14445
EID = "ep-20261111T000000Z-goal-0badf00d"
CELL = {cc.HORIZON_START_ENV: "867.01.03", cc.HORIZON_END_ENV: "877.01.03",
        cc.CYCLE_GAME_DAYS_ENV: "90"}
OPEN = {G.ENV_LINE_START: str(K_ID), G.ENV_R_OPEN: "89", G.ENV_OPEN_DATE: "867.01.03"}


def run(coro):
    return asyncio.run(coro)


def watch(line=K_ID, status="following", **kw):
    w = {"start_character_id": K_ID, "line_character_id": line, "previous_character_id": 0,
         "carrying_title_id": 0, "primary_title_id": 7052, "successions": 0, "why": "armed",
         "status": status, "armed_date": "867.01.03", "since_date": "867.01.03",
         "armed_this_read": False}
    w.update(kw)
    return w


@pytest.fixture
def game(monkeypatch, tmp_path):
    """A seat's server on a doubled bridge. `world` is what each read answers."""
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 9, "869.04.03"
    state._client = type("C", (), {"session_id": "s-seat"})()
    world = {
        "tick": {"paused": True, "game_speed": 1, "tick_id": 9, "game_date": "869.04.03",
                 "pipeline": {"episode_id": EID}},
        "chars": {K_ID: {"id": K_ID, "alive": True}, D_ID: {"id": D_ID, "alive": True},
                  HEIR: {"id": HEIR, "alive": True}},
        "watch": watch(),
        "realms": {K_ID: {"character_id": K_ID, "has_realm": True, "realm_total_counties": 101},
                   D_ID: {"character_id": D_ID, "has_realm": True, "realm_total_counties": 7},
                   HEIR: {"character_id": HEIR, "has_realm": True, "realm_total_counties": 44}},
        "stall": False,
        "reads": [],
    }
    roster = {"get_controlled_roster": {"primary_character_id": K_ID, "roster": [K_ID],
                                        "player_character_id": 0},
              "get_bridge_state": {"sessions": [{"session_id": "s-seat", "primary_character_id": K_ID,
                                                 "claimed_character_ids": [K_ID]}]}}
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def qb(_state, action, params, timeout=15.0):
        world["reads"].append((action, dict(params or {})))
        if action == "get_tick_info":
            return world["tick"]
        if action in roster:
            return roster[action]
        cid = (params or {}).get("character_id") or K_ID
        if world["stall"]:
            return {}
        if action == "query_character":
            out = dict(world["chars"].get(cid) or {"success": False})
            if (params or {}).get("played_line") == 1:
                out["played_line"] = world["watch"]
            return out
        if action == "query_realm":
            return world["realms"].get(cid) or {"character_id": cid, "has_realm": False}
        return {"character_id": cid, "action": action}

    async def ab(_state, action, params, timeout=5.0):
        return await qb(_state, action, params, timeout)

    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_action_bridge", ab)
    monkeypatch.setattr(ms, "_CHANNEL", None)
    monkeypatch.setenv("CK3_INSTANCE_ID", "goal-test")
    monkeypatch.setenv("CK3_INSTANCE_ROOT", str(tmp_path))
    for k in (*CELL, *OPEN, G.ENV_SEAT, "CK3_EPISODE_ID"):
        monkeypatch.delenv(k, raising=False)
    return world


def cell(monkeypatch, seat="K", open_env=True):
    for k, v in CELL.items():
        monkeypatch.setenv(k, v)
    if open_env:
        for k, v in OPEN.items():
            monkeypatch.setenv(k, v)
    monkeypatch.setenv(G.ENV_SEAT, seat)


# ── the schedule position (cycle_cadence, the one authority) ────────────────

def test_schedule_position_outside_a_cell_says_so_and_carries_no_numbers():
    assert cc.schedule_position("869.04.03", {}) == {"state": "not_an_experiment_episode"}


def test_schedule_position_reads_the_cells_own_schedule():
    sched, _ = cc.cell_schedule(CELL)
    at_open = cc.schedule_position("867.01.03", CELL)
    assert (at_open["cycle"], at_open["cycles_total"], at_open["cycles_left"]) == (0, 41, 41)
    assert at_open["days_left"] == 3650 and at_open["horizon_end"] == "877.01.03"
    mid = cc.schedule_position("869.04.03", CELL)
    o = cc.ordinal("869.04.03")
    assert mid["cycle"] == sched.cycle_index(o) and mid["cycles_left"] == sched.n_boundaries - mid["cycle"]
    assert mid["days_left"] == sched.end - o and mid["horizon_reached"] is False
    end = cc.schedule_position("877.01.03", CELL)
    assert end["cycles_left"] == 0 and end["days_left"] == 0 and end["horizon_reached"] is True
    assert cc.schedule_position("soon", CELL)["state"] == cc.POS_DATE_UNREAD
    bad = cc.schedule_position("869.04.03", {cc.HORIZON_START_ENV: "867.01.03"})
    assert bad["state"] == cc.POS_SCHEDULE_UNREADABLE and "cycle" not in bad


# ── observe's goal block ────────────────────────────────────────────────────

def test_outside_an_experiment_the_goal_is_stated_absent_not_zeros(game):
    env = run(ms.observe(character_id=K_ID))
    assert list(env["result"])[0] == "goal"
    assert env["result"]["goal"] == {"state": "not_an_experiment_episode"}
    # no goal read was made: the watch is never armed outside a cell
    assert not any(p.get("played_line") for _a, p in game["reads"])


def test_the_kings_goal_block_carries_R_now_and_at_open_the_end_date_and_the_cycles(game, monkeypatch):
    cell(monkeypatch, "K")
    env = run(ms.observe(character_id=K_ID))
    g = env["result"]["goal"]
    assert list(env["result"])[0] == "goal" and g["state"] == "present" and g["seat"] == "K"
    assert g["R"]["now"] == 101 and g["R"]["at_open"] == 89 and g["R"]["change"] == 12
    assert g["R"]["read"] == "measured" and g["R"]["line_character_id"] == K_ID
    assert g["R"]["open_date"] == "867.01.03" and "de-jure" in g["R"]["definition"]
    pos = cc.schedule_position("869.04.03", CELL)
    assert g["horizon"] == {"end_date": "877.01.03", "days_left": pos["days_left"], "reached": False}
    assert g["cycle"] == {"game_days": 90, "index": pos["cycle"], "total": 41, "left": pos["cycles_left"]}
    assert "own_counties" not in g
    # R is the watch's read, then the line's realm: the meter's two reads
    assert ("query_character", {"character_id": K_ID, "played_line": 1}) in game["reads"]


def test_after_a_succession_R_is_the_heirs_realm_by_the_bridges_decision(game, monkeypatch):
    cell(monkeypatch, "K")
    game["watch"] = watch(line=HEIR, successions=1, previous_character_id=K_ID)
    g = run(ms.observe(character_id=HEIR))["result"]["goal"]
    assert g["R"]["now"] == 44 and g["R"]["line_character_id"] == HEIR and g["R"]["change"] == -45


def test_a_living_landless_line_character_reads_zero_measured(game, monkeypatch):
    cell(monkeypatch, "K")
    game["realms"][K_ID] = {"character_id": K_ID, "has_realm": False}
    g = run(ms.observe(character_id=K_ID))["result"]["goal"]
    assert g["R"]["now"] == 0 and g["R"]["read"] == "measured" and g["R"]["change"] == -89


def test_a_pending_line_is_unread_with_its_reason_never_a_number(game, monkeypatch):
    cell(monkeypatch, "K")
    game["watch"] = watch(status="successor_pending", pending_reason="primary_title_unheld")
    g = run(ms.observe(character_id=K_ID))["result"]["goal"]
    assert g["R"]["now"] is None and g["R"]["change"] is None
    assert g["R"]["read"] == "unread" and g["R"]["why"] == G.WHY_LINE_PENDING
    assert g["R"]["at_open"] == 89


def test_a_bridge_without_the_watch_is_unread_not_the_seats_own_realm(game, monkeypatch):
    cell(monkeypatch, "K")
    game["watch"] = None
    g = run(ms.observe(character_id=K_ID))["result"]["goal"]
    assert g["R"]["now"] is None and g["R"]["why"] == "line_watch_unreadable"


def test_a_cell_that_passed_no_open_sample_is_unread_by_name(game, monkeypatch):
    cell(monkeypatch, "K", open_env=False)
    g = run(ms.observe(character_id=K_ID))["result"]["goal"]
    assert g["R"]["now"] is None and g["R"]["why"] == G.WHY_NO_LINE_START
    assert g["R"]["at_open"] is None and G.ENV_R_OPEN in g["R"]["at_open_why"]


def test_a_duke_sees_the_kings_R_and_his_own_county_count(game, monkeypatch):
    cell(monkeypatch, "D1")
    g = run(ms.observe(character_id=D_ID))["result"]["goal"]
    assert g["seat"] == "D1" and g["R"]["now"] == 101
    assert g["own_counties"] == {"character_id": D_ID, "read": "measured", "count": 7}


def test_an_unreadable_schedule_is_named_and_carries_no_numbers(game, monkeypatch):
    monkeypatch.setenv(cc.HORIZON_START_ENV, "867.01.03")
    g = run(ms.observe(character_id=K_ID))["result"]["goal"]
    assert g["state"] == cc.POS_SCHEDULE_UNREADABLE and "R" not in g


# ── observe during a stall (carried defect, SMOKE6: ok=True over a 93 s stall) ──

def test_a_bridge_that_answers_none_of_observes_reads_is_not_ok(game):
    game["stall"] = True
    env = run(ms.observe(character_id=K_ID))
    assert env["ok"] is False, env
    assert any(e["code"] == R.OBSERVE_BRIDGE_UNANSWERED for e in env["errors"])


def test_some_unanswered_reads_are_a_warning_naming_them(game, monkeypatch):
    real = ms._query_bridge

    async def qb(state, action, params, timeout=15.0):
        if action == "query_faction_threat":
            return {}
        if action == "get_active_wars":
            return {"error": "game_thread_timeout"}
        return await real(state, action, params, timeout)
    monkeypatch.setattr(ms, "_query_bridge", qb)
    env = run(ms.observe(character_id=K_ID))
    assert env["ok"] is True
    w = [x for x in env["warnings"] if x["code"] == R.OBSERVE_READS_UNANSWERED]
    assert len(w) == 1 and "query_faction_threat=empty_reply" in w[0]["detail"]
    assert "get_active_wars=game_thread_timeout" in w[0]["detail"]


def test_CONTROL_an_answered_observe_carries_neither_code(game):
    env = run(ms.observe(character_id=K_ID))
    codes = {x["code"] for x in env["warnings"] + env["errors"]}
    assert not codes & {R.OBSERVE_BRIDGE_UNANSWERED, R.OBSERVE_READS_UNANSWERED}


# ── resume-prompt delivery: one inbox, delivered once ──────────────────────

def _cfg(seat):
    return C.ChannelConfig(arm="council", topology="complete", seats=("K", "D1"), seat=seat,
                           quota_per_cycle=2, max_text_bytes=600)


def test_a_message_delivered_in_the_resume_prompt_is_never_shown_again_by_observe(game, monkeypatch, tmp_path):
    ledger = C.Ledger(C.ledger_path(EID), episode_id=EID)
    C.attempt(_cfg("K"), ledger, to="D1", text="join my war on Ludwig", game_date="869.03.01",
              tick=5, cycle=7)
    got = C.deliver(_cfg("D1"), ledger, game_date="869.04.03", tick=9, cycle=8,
                    via=C.VIA_RESUME_PROMPT, turn=3)
    assert [m["text"] for m in got] == ["join my war on Ludwig"]
    block = C.prompt_block(got)
    assert block.startswith(C.PROMPT_HEADING) and "from K (sent 869.03.01): join my war on Ludwig" in block
    row = C.materialise(ledger.records())[0]
    assert (row["read"], row["read_via"], row["read_turn"], row["read_cycle"]) == ("read", "resume_prompt", 3, 8)
    # the same seat's observe: nothing again, no second receipt
    monkeypatch.setattr(ms, "_CHANNEL", _cfg("D1"))
    monkeypatch.setattr(ms, "_SEAT", C.SeatState())
    assert run(ms.observe(character_id=K_ID))["result"]["messages"] == []
    assert sum(r.get("kind") == C.KIND_RECEIPT for r in ledger.records()) == 1


def test_an_observe_receipt_says_observe_and_an_old_receipt_reads_as_observe(tmp_path):
    ledger = C.Ledger(tmp_path / "l.jsonl", episode_id="e")
    C.attempt(_cfg("K"), ledger, to="D1", text="a", game_date="867.1.5", tick=1, cycle=0)
    C.deliver(_cfg("D1"), ledger, game_date="867.1.6", tick=2, cycle=0)
    assert C.materialise(ledger.records())[0]["read_via"] == C.VIA_OBSERVE
    recs = [dict(r) for r in ledger.records()]
    recs[-1].pop("via")
    assert C.materialise(recs)[0]["read_via"] == C.VIA_OBSERVE
    with pytest.raises(ValueError):
        C.deliver(_cfg("D1"), ledger, game_date="x", tick=0, cycle=0, via="carrier_pigeon")
    assert C.prompt_block([]) == ""


def test_the_goal_envs_seat_label_is_the_channels():
    assert G.ENV_SEAT == C.ENV_SEAT and G.KING == C.KING
