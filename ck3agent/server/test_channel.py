"""The inter-seat channel (pre-registration v3 section 5.2, 7.6; Study 0 lane
s0-channel): the topology rule, the boot refusals, the cycle rule, the ledger,
the verdict order, and delivery through `observe` — never through anything else.

THE FIXTURE CANNOT FORGE A DELIVERY. Every row below that claims a message was
read has called `mcp_server.observe`; the game is doubled (the bridge replies),
the delivery path is not. A static row pins that `channel.deliver` has exactly
one caller in the server and that caller is `observe`'s hook.
"""

import ast
import asyncio
import json
import re
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import channel as C  # noqa: E402

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402


def run(coro):
    return asyncio.run(coro)


SEATS = ("K", "D1", "D2", "D3")


def cfg(seat, topology="complete", quota=2, max_bytes=600):
    return C.ChannelConfig(arm="flat_hom", topology=topology, seats=SEATS, seat=seat,
                           quota_per_cycle=quota, max_text_bytes=max_bytes)


# ── the topology table ──────────────────────────────────────────────────────

@pytest.mark.parametrize("sender,recipient,want", [
    ("K", "D1", True), ("D1", "K", True), ("D1", "D2", True), ("D3", "D1", True),
    ("K", "K", False), ("D1", "D1", False),          # nobody writes to themselves
    ("K", "D9", False), ("D1", "Q", False),           # not a seat of this game
    ("X", "K", False),                                # a sender outside the table
])
def test_complete_is_any_seat_to_any_other_seat(sender, recipient, want):
    assert C.allowed("complete", sender, recipient, SEATS) is want


@pytest.mark.parametrize("sender,recipient,want", [
    ("K", "D1", True), ("K", "D3", True), ("D1", "K", True), ("D3", "K", True),
    ("D1", "D2", False), ("D2", "D1", False),         # no lateral edge in a star
    ("K", "K", False), ("K", "D9", False),
])
def test_star_is_king_to_each_duke_and_back_never_duke_to_duke(sender, recipient, want):
    assert C.allowed("star", sender, recipient, SEATS) is want


def test_none_allows_nothing():
    assert all(not C.allowed("none", s, r, SEATS) for s in SEATS for r in SEATS)


def test_seat_labels_are_K_and_D1_to_Dn():
    assert C.seat_labels(5) == ["K", "D1", "D2", "D3", "D4", "D5"]
    with pytest.raises(ValueError):
        C.seat_labels(0)


# ── the declaration at boot ─────────────────────────────────────────────────

FULL = {"CK3_EPISODE_ARM": "flat_hom", "CK3_CHANNEL_TOPOLOGY": "complete",
        "CK3_CHANNEL_SEATS": "K,D1,D2", "CK3_SEAT_LABEL": "D1",
        "CK3_CHANNEL_QUOTA_PER_CYCLE": "2", "CK3_CHANNEL_MAX_TEXT_BYTES": "600"}


def test_free_play_declares_no_channel():
    assert C.boot({}) is None


def test_an_arm_without_a_topology_is_refused_never_defaulted():
    with pytest.raises(C.ChannelBootRefusal) as ei:
        C.boot({"CK3_EPISODE_ARM": "flat_hom"})
    assert ei.value.reason == C.R_TOPOLOGY_UNDECLARED
    assert "channel.topology.flat_hom" in ei.value.detail


def test_an_unknown_topology_is_refused_by_name():
    with pytest.raises(C.ChannelBootRefusal) as ei:
        C.boot({"CK3_EPISODE_ARM": "flat_hom", "CK3_CHANNEL_TOPOLOGY": "ring"})
    assert ei.value.reason == C.R_TOPOLOGY_INVALID


def test_none_boots_disabled_and_needs_no_seat():
    got = C.boot({"CK3_EPISODE_ARM": "k1", "CK3_CHANNEL_TOPOLOGY": "none"})
    assert got is not None and not got.enabled and got.seat == ""


@pytest.mark.parametrize("drop,reason", [
    ("CK3_CHANNEL_SEATS", C.R_SEATS_UNDECLARED),
    ("CK3_CHANNEL_QUOTA_PER_CYCLE", C.R_BOUNDS_UNDECLARED),
    ("CK3_CHANNEL_MAX_TEXT_BYTES", C.R_BOUNDS_UNDECLARED),
])
def test_each_missing_leg_is_its_own_refusal(drop, reason):
    env = {k: v for k, v in FULL.items() if k != drop}
    with pytest.raises(C.ChannelBootRefusal) as ei:
        C.boot(env)
    assert ei.value.reason == reason


def test_a_seat_outside_the_declared_set_is_refused():
    with pytest.raises(C.ChannelBootRefusal) as ei:
        C.boot(dict(FULL, CK3_SEAT_LABEL="D7"))
    assert ei.value.reason == C.R_SEAT_UNKNOWN


def test_no_seat_label_boots_the_arms_surface_but_names_the_process_as_nobodys_seat():
    """The surface is an ARM property (the launcher's probe has no session);
    the label is a SESSION property. A labelless process registers the tool
    and refuses to act as a seat, by name."""
    got = C.boot({k: v for k, v in FULL.items() if k != "CK3_SEAT_LABEL"})
    assert got.enabled and got.seat == ""
    assert got.seat_refusal() == C.R_SEAT_UNDECLARED
    assert C.boot(FULL).seat_refusal() is None


def test_the_full_declaration_boots_enabled():
    got = C.boot(FULL)
    assert got.enabled and got.seat == "D1" and got.seats == ("K", "D1", "D2")
    assert (got.quota_per_cycle, got.max_text_bytes) == (2, 600)


def test_the_transcription_round_trips_through_boot():
    env = C.transcription_for("star", quota_per_cycle="2", max_text_bytes="600", n_dukes="5")
    assert env["CK3_CHANNEL_SEATS"] == "K,D1,D2,D3,D4,D5"
    got = C.boot(dict(env, CK3_EPISODE_ARM="orders_hom", CK3_SEAT_LABEL="K"))
    assert got.topology == "star" and got.seats == tuple(C.seat_labels(5))
    assert C.transcription_for("none") == {"CK3_CHANNEL_TOPOLOGY": "none"}


def test_declared_block_names_absence_as_a_state_never_none():
    assert C.declared_block({}) == {"declared": False, "state": "not_declared", "topology": None}
    blk = C.declared_block(FULL)
    assert blk["declared"] and blk["topology"] == "complete" and blk["seats"] == ["K", "D1", "D2"]
    assert C.ledger_required({"channel": blk})
    assert not C.ledger_required({"channel": C.declared_block({"CK3_CHANNEL_TOPOLOGY": "none"})})
    assert not C.ledger_required({})            # predates the lane: coverage, not a requirement


# ── the cycle rule ──────────────────────────────────────────────────────────

def test_a_wait_that_waited_closes_a_cycle_and_nothing_else_does():
    cc = C.CycleCounter()
    assert cc.note("wait", {"ok": True, "verdict": "applied", "result": {"waited_s": 60}}) == 1
    assert cc.note("wait", {"ok": True, "verdict": "pending", "result": {"waited_s": 10}}) == 2
    assert cc.note("wait", {"ok": False, "verdict": "rejected", "result": {"waited_s": 10}}) == 3
    # refused BEFORE anything was waited: not a boundary (quota would reset for free)
    assert cc.note("wait", {"ok": False, "blocked": {"gate": "game_did_not_unpause"},
                            "result": {}}) == 3
    assert cc.note("pause", {"ok": True, "verdict": "applied", "result": {"waited_s": 1}}) == 3
    assert cc.note("whoami", {"ok": True, "result": {}}) == 3


# ── the ledger ──────────────────────────────────────────────────────────────

@pytest.fixture
def ledger(tmp_path):
    return C.Ledger(tmp_path / "ch" / "ep-x" / C.LEDGER_NAME, episode_id="ep-x", instance_id="i1")


def test_two_processes_see_one_ledger(ledger, tmp_path):
    other = C.Ledger(ledger.path, episode_id="ep-x", instance_id="i1")
    C.attempt(cfg("D1"), ledger, to="K", text="one", game_date="867.1.5", tick=3, cycle=0)
    C.attempt(cfg("D2"), other, to="K", text="two", game_date="867.1.5", tick=4, cycle=0)
    rows = C.materialise(ledger.records())
    assert [r["sender"] for r in rows] == ["D1", "D2"]
    assert all(r["episode_id"] == "ep-x" and r["read"] == C.UNREAD for r in rows)
    assert ledger.lock_path.exists()


def test_a_send_row_carries_every_declared_field(ledger):
    row = C.attempt(cfg("D1"), ledger, to="K", text="héllo", game_date="867.1.5", tick=3, cycle=1)
    assert set(row) >= {"episode_id", "kind", "msg_id", "instance_id", "game_date", "tick",
                        "sender", "recipient", "cycle", "bytes", "text_sha256", "text",
                        "verdict", "sent_utc"}
    assert row["bytes"] == 6 and row["text_sha256"] == C.text_sha256("héllo")
    assert row["verdict"] == C.V_DELIVERED and row["cycle"] == 1


def test_verdict_order_is_topology_then_length_then_quota(ledger):
    long = "x" * 601
    # a denied recipient is refused before the text is looked at
    r = C.attempt(cfg("D1", "star"), ledger, to="D2", text=long, game_date="d", tick=1, cycle=0)
    assert r["verdict"] == C.V_TOPOLOGY_DENIED
    # an over-long text to an allowed recipient is refused before the quota is counted
    r = C.attempt(cfg("D1", "star"), ledger, to="K", text=long, game_date="d", tick=1, cycle=0)
    assert r["verdict"] == C.V_TEXT_TOO_LONG
    # refusals spent no quota: two deliveries still fit, the third is the quota
    for _ in range(2):
        assert C.attempt(cfg("D1", "star"), ledger, to="K", text="ok", game_date="d", tick=1,
                         cycle=0)["verdict"] == C.V_DELIVERED
    r = C.attempt(cfg("D1", "star"), ledger, to="K", text="ok", game_date="d", tick=1, cycle=0)
    assert r["verdict"] == C.V_QUOTA_EXCEEDED
    # the next cycle starts a fresh count; another seat's sends never count against this one
    assert C.attempt(cfg("D1", "star"), ledger, to="K", text="ok", game_date="d", tick=2,
                     cycle=1)["verdict"] == C.V_DELIVERED
    assert C.attempt(cfg("D2", "star"), ledger, to="K", text="ok", game_date="d", tick=2,
                     cycle=0)["verdict"] == C.V_DELIVERED
    verdicts = [r["verdict"] for r in C.materialise(ledger.records())]
    assert verdicts == [C.V_TOPOLOGY_DENIED, C.V_TEXT_TOO_LONG, C.V_DELIVERED, C.V_DELIVERED,
                        C.V_QUOTA_EXCEEDED, C.V_DELIVERED, C.V_DELIVERED]
    assert set(verdicts) <= set(C.VERDICTS)


def test_a_receipt_for_an_unknown_send_is_a_defect(ledger):
    ledger.append({"kind": C.KIND_RECEIPT, "msg_id": "nope", "reader": "K"})
    with pytest.raises(ValueError):
        C.materialise(ledger.records())


def test_the_ledger_path_is_the_instances_and_the_episodes(tmp_path):
    env = {"CK3_INSTANCE_ID": "ep-host-a", "CK3_INSTANCE_ROOT": str(tmp_path)}
    p = C.ledger_path("ep-20261104T000000Z-x-deadbeef", env)
    assert p.name == C.LEDGER_NAME and p.parent.name == "ep-20261104T000000Z-x-deadbeef"
    assert str(tmp_path / "ep-host-a") in str(p)
    with pytest.raises(ValueError):
        C.ledger_path("../escape", env)


def test_the_episode_id_comes_from_the_environment_else_the_bridge():
    s = C.SeatState()
    assert s.resolve_episode_id({}, {"pipeline": {"episode_id": "ep-b"}}) == "ep-b"
    assert s.episode_id_source == "bridge"
    assert s.resolve_episode_id({"CK3_EPISODE_ID": "ep-e"}, {"pipeline": {"episode_id": "ep-b"}}) == "ep-e"
    assert s.episode_id_source == "environment"
    assert C.SeatState().resolve_episode_id({}, {"tick_id": 1}) is None


# ── delivery, through observe and nothing else ──────────────────────────────

EID = "ep-20261104T000000Z-chan-0badf00d"


@pytest.fixture
def seat(monkeypatch, tmp_path):
    """A seat's server with the game doubled: the bridge answers, the channel
    does not. `as_seat(label)` re-declares which seat this process is."""
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 9, "867.2.1"
    state._client = type("C", (), {"session_id": "s-seat"})()
    roster = {"get_controlled_roster": {"action": "get_controlled_roster",
                                        "primary_character_id": 30853, "roster": [30853],
                                        "player_character_id": 0},
              "get_bridge_state": {"action": "get_bridge_state", "sessions": [
                  {"session_id": "s-seat", "primary_character_id": 30853,
                   "claimed_character_ids": [30853]}]},
              "get_tick_info": {"paused": True, "game_speed": 1, "tick_id": 9,
                                "game_date": "867.2.1", "pipeline": {"episode_id": EID}}}
    monkeypatch.setattr(ms, "_get_state", lambda: state)
    clock = {"paused": True}

    async def tick():
        return dict(roster["get_tick_info"], paused=clock["paused"])

    async def qb(_state, action, params, timeout=15.0):
        if action == "get_tick_info":
            return await tick()
        if action in roster:
            return roster[action]
        if action == "query_character":
            return {"id": 30853, "alive": True}
        return {"character_id": 30853, "action": action}

    async def ab(_state, action, params, timeout=5.0):
        if action == "get_tick_info":
            return await tick()
        if action in ("pause_game", "unpause_game"):      # the clock the wait branch drives
            clock["paused"] = action == "pause_game"
        if action in roster:
            return roster[action]
        return {"success": True}

    advance = {"days": 0}                              # what a sleep moves the clock by

    async def no_sleep(_s):
        if advance["days"] and not clock["paused"]:
            roster["get_tick_info"]["game_date"] = _date_plus(roster["get_tick_info"]["game_date"],
                                                              advance["days"])
        return None
    monkeypatch.setattr(R, "_SLEEP", no_sleep)

    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_action_bridge", ab)
    monkeypatch.setenv("CK3_INSTANCE_ID", "chan-test")
    monkeypatch.setenv("CK3_INSTANCE_ROOT", str(tmp_path))
    monkeypatch.delenv("CK3_EPISODE_ID", raising=False)

    class Seat:
        def as_seat(self, label, topology="complete", quota=2, max_bytes=600):
            monkeypatch.setattr(ms, "_CHANNEL", cfg(label, topology, quota, max_bytes))
            monkeypatch.setattr(ms, "_SEAT", C.SeatState())
            return self

        def ledger(self):
            return C.Ledger(C.ledger_path(EID), episode_id=EID)

        def off(self):
            monkeypatch.setattr(ms, "_CHANNEL", None)
            return self

        def clock_moves(self, days):
            """Every wait from now on advances the game date by `days`."""
            advance["days"] = days
            return self

        def date(self):
            return roster["get_tick_info"]["game_date"]
    return Seat()


def _date_plus(date, days):
    import cycle_cadence as cc
    o = cc.ordinal(date) + days
    y, rem = divmod(o, cc.DAYS_PER_YEAR)
    m = max(i for i in range(12) if cc.MONTH_CUM_DAYS[i] <= rem)
    return f"{y}.{m + 1}.{rem - cc.MONTH_CUM_DAYS[m] + 1}"


def test_a_message_reaches_the_recipient_inside_its_next_observe_and_is_stamped(seat):
    seat.as_seat("D1")
    env = run(ms.msg_send(to="K", text="hold the north"))
    assert env["ok"] is True and env["verdict"] == "applied", env
    assert env["result"]["sender"] == "D1" and env["result"]["recipient"] == "K"
    assert env["result"]["tick"] == 9 and env["result"]["game_date"] == "867.2.1"
    assert "text" not in env["result"]            # the text is in the ledger, hashed in the reply
    assert C.materialise(seat.ledger().records())[0]["read"] == C.UNREAD

    seat.as_seat("K")
    obs = run(ms.observe())
    msgs = obs["result"]["messages"]
    assert [m["text"] for m in msgs] == ["hold the north"]
    assert msgs[0]["sender"] == "D1" and msgs[0]["tick"] == 9 and msgs[0]["game_date"] == "867.2.1"
    row = C.materialise(seat.ledger().records())[0]
    assert row["read"] == "read" and row["reader"] == "K"
    assert (row["read_tick"], row["read_game_date"], row["read_cycle"]) == (9, "867.2.1", 0)

    # the SECOND observe shows nothing and stamps nothing again
    obs2 = run(ms.observe())
    assert obs2["result"]["messages"] == []
    assert sum(r.get("kind") == C.KIND_RECEIPT for r in seat.ledger().records()) == 1


def test_deliver_re_checks_the_topology_on_the_readers_side(ledger):
    """A row stamped `delivered` under a table the reader's table forbids is
    not delivered on the stamp's say-so (witness note): it stays unread."""
    C.attempt(cfg("D1", "complete"), ledger, to="D2", text="lateral", game_date="867.1.5", tick=3, cycle=0)
    assert C.materialise(ledger.records())[0]["verdict"] == C.V_DELIVERED
    assert C.deliver(cfg("D2", "star"), ledger, game_date="867.1.6", tick=4, cycle=0) == []
    assert C.materialise(ledger.records())[0]["read"] == C.UNREAD
    assert sum(r.get("kind") == C.KIND_RECEIPT for r in ledger.records()) == 0
    # the same row IS delivered to a reader whose table allows the edge
    got = C.deliver(cfg("D2", "complete"), ledger, game_date="867.1.6", tick=4, cycle=0)
    assert [m["text"] for m in got] == ["lateral"]


def test_a_message_only_the_recipient_can_see(seat):
    seat.as_seat("D1")
    run(ms.msg_send(to="K", text="for the king"))
    seat.as_seat("D2")
    assert run(ms.observe())["result"]["messages"] == []
    assert all(r["read"] == C.UNREAD for r in C.materialise(seat.ledger().records()))


def test_refusals_carry_the_ledger_verdict_as_the_gate_and_are_logged(seat):
    seat.as_seat("D1", "star")
    env = run(ms.msg_send(to="D2", text="psst"))
    assert env["ok"] is False and env["blocked"]["gate"] == "topology_denied"
    env = run(ms.msg_send(to="K", text="x" * 601))
    assert env["blocked"]["gate"] == "text_too_long"
    for _ in range(2):
        assert run(ms.msg_send(to="K", text="ok"))["verdict"] == "applied"
    env = run(ms.msg_send(to="K", text="three"))
    assert env["blocked"]["gate"] == "quota_exceeded"
    assert [r["verdict"] for r in C.materialise(seat.ledger().records())] == [
        "topology_denied", "text_too_long", "delivered", "delivered", "quota_exceeded"]
    for gate in ("topology_denied", "text_too_long", "quota_exceeded", C.GATE_UNAVAILABLE):
        assert gate in R.GATE_TABLE


def test_the_quota_is_keyed_on_waits_the_seat_made(seat):
    """Free play (no registered cadence): every wait that waited is a cycle."""
    seat.as_seat("D1")
    for _ in range(2):
        run(ms.msg_send(to="K", text="ok"))
    assert run(ms.msg_send(to="K", text="no"))["blocked"]["gate"] == "quota_exceeded"
    w = run(ms.session(action="wait", seconds=1))
    assert "waited_s" in w["result"], w
    assert ms._SEAT.cycles.cycle == 1
    env = run(ms.msg_send(to="K", text="again"))
    assert env["verdict"] == "applied" and env["result"]["cycle"] == 1


def test_under_the_registered_cadence_the_quota_resets_at_the_game_day_boundary_not_per_wait(seat, monkeypatch):
    """v3 §5.2 / §7.2 (signing blocker f): ONE cycle — the ≥ cadence game-day
    boundary the harness enforces, not "any wait that waited"."""
    monkeypatch.setenv("CK3_CYCLE_GAME_DAYS", "90")
    seat.as_seat("D1").clock_moves(30)
    assert ms._SEAT.cycles.cadence_days == 90
    for _ in range(2):
        assert run(ms.msg_send(to="K", text="ok"))["verdict"] == "applied"
    assert run(ms.msg_send(to="K", text="no"))["blocked"]["gate"] == "quota_exceeded"
    # two waits of 30 game-days each: 60 days in, still cycle 0, still over quota
    for _ in range(2):
        w = run(ms.session(action="wait", seconds=1))
        assert "waited_s" in w["result"] and w["result"]["clock_ran"] is True, w
    assert ms._SEAT.cycles.cycle == 0
    assert run(ms.msg_send(to="K", text="still no"))["blocked"]["gate"] == "quota_exceeded"
    # the third wait carries the date to day 90: the boundary, a fresh quota
    run(ms.session(action="wait", seconds=1))
    assert ms._SEAT.cycles.cycle == 1 and seat.date() == "867.5.2"
    env = run(ms.msg_send(to="K", text="again"))
    assert env["verdict"] == "applied" and env["result"]["cycle"] == 1
    # the receipt the recipient's observe stamps carries the READER's cycle by the same rule
    seat.as_seat("K")
    run(ms.observe())
    rows = C.materialise(seat.ledger().records())
    assert [r["cycle"] for r in rows] == [0, 0, 0, 0, 1] and rows[-1]["read_cycle"] == 0
    # a wait refused at the horizon waits nothing and moves no cycle
    monkeypatch.setenv("CK3_EPISODE_HORIZON_END", "867.1.1")
    seat.as_seat("D1")
    w = run(ms.session(action="wait", seconds=1))
    assert w["blocked"]["gate"] == "horizon_reached" and ms._SEAT.cycles.cycle == 0


def test_without_an_episode_id_nothing_is_sent_and_the_refusal_is_typed(seat, monkeypatch):
    seat.as_seat("D1")

    async def qb(_state, action, params, timeout=15.0):
        if action == "get_tick_info":
            return {"paused": True, "game_speed": 1}
        if action == "get_controlled_roster":
            return {"primary_character_id": 30853, "roster": [30853], "player_character_id": 0}
        return {"character_id": 30853}
    monkeypatch.setattr(ms, "_query_bridge", qb)
    env = run(ms.msg_send(to="K", text="lost"))
    assert env["ok"] is False and env["blocked"]["gate"] == C.GATE_UNAVAILABLE
    assert not C.ledger_path(EID).exists()


def test_a_labelless_process_sends_nothing_and_delivers_nothing_by_name(seat, monkeypatch):
    seat.as_seat("D1")
    run(ms.msg_send(to="K", text="from D1"))
    monkeypatch.setattr(ms, "_CHANNEL", C.ChannelConfig(
        arm="flat_hom", topology="complete", seats=SEATS, seat="", quota_per_cycle=2,
        max_text_bytes=600))
    env = run(ms.msg_send(to="K", text="from nobody"))
    assert env["blocked"]["gate"] == C.GATE_UNAVAILABLE
    assert env["blocked"]["measured"]["reason"] == C.R_SEAT_UNDECLARED
    obs = run(ms.observe())
    assert "messages" not in obs["result"]
    assert any(w["code"] == C.W_UNREAD and C.R_SEAT_UNDECLARED in w["detail"]
               for w in obs["warnings"])
    rows = C.materialise(seat.ledger().records())
    assert [r["sender"] for r in rows] == ["D1"] and rows[0]["read"] == C.UNREAD


def test_a_seat_with_no_channel_answers_unavailable_not_a_crash(seat):
    seat.off()
    env = run(ms.msg_send(to="K", text="x"))
    assert env["blocked"]["gate"] == C.GATE_UNAVAILABLE
    assert "messages" not in run(ms.observe())["result"]


# ── the structural pins ─────────────────────────────────────────────────────

SRC = (HERE / "mcp_server.py").read_text(encoding="utf-8")


def test_deliver_has_one_caller_and_it_is_observes_hook():
    tree = ast.parse(SRC)
    callers = {}
    for fn in ast.walk(tree):
        if isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            for node in ast.walk(fn):
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) \
                        and node.func.attr == "deliver":
                    callers.setdefault(fn.name, 0)
                    callers[fn.name] += 1
    assert callers == {"_deliver_inbox": 1}, callers
    hook_callers = [fn.name for fn in ast.walk(tree)
                    if isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and any(isinstance(n, ast.Call) and getattr(n.func, "id", "") == "_deliver_inbox"
                            for n in ast.walk(fn))]
    assert hook_callers == ["observe"], hook_callers


def test_msg_send_is_registered_under_a_guard_never_by_decorator():
    assert re.search(r"^if _CHANNEL is not None and _CHANNEL\.enabled:\n    mcp\.add_tool\(msg_send\)",
                     SRC, re.M)
    assert not re.search(r"@mcp\.tool\(\)\nasync def msg_send", SRC)


def test_the_boot_refusal_is_typed_on_stderr_and_exits_3(tmp_path):
    import subprocess
    r = subprocess.run([sys.executable, "-c", "import mcp_server"], cwd=str(HERE),
                       env={"PATH": "/usr/bin:/bin", "CK3_EPISODE_ARM": "orders_hom"},
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 3
    assert json.loads(r.stderr.strip().splitlines()[-1])["reason"] == C.R_TOPOLOGY_UNDECLARED
