"""The seats' notepads (pre-registration v4 draft sections 3 and 7; lane v4note):
the declaration at boot, the write rule, the bounds, the log, the cycle from the
cycle authority, the pads' persistence across a resume and a rebind, the seal,
and the two tools served through the server like msg_send.

THE GAME IS DOUBLED, THE NOTEPAD IS NOT. Every row that claims a seat read or
wrote a pad has called `mcp_server.note_read` / `note_write` against the real
log on disk.
"""

import json
import multiprocessing
import re
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import notepad as N  # noqa: E402

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402

BOUNDS = {N.ENV_PRIVATE_MAX: "64", N.ENV_COUNCIL_MAX: "96", N.ENV_WRITE_MAX: "32"}


def cfg(seat, pads=N.DECL_BOTH, private=64, council=96, write=32):
    return N.NotepadConfig(pads=N.DECLARATIONS[pads], seat=seat, private_max_bytes=private,
                           council_max_bytes=council, max_write_bytes=write)


# ── the declaration at boot ─────────────────────────────────────────────────

def test_no_declaration_is_no_notepad_and_none_is_declared_off():
    assert N.boot({}) is None
    assert N.boot({"CK3_EPISODE_ARM": "k1"}) is None
    off = N.boot({N.ENV_PADS: "none"})
    assert off is not None and not off.enabled


@pytest.mark.parametrize("raw", ["council", "private,council,private", "shared", "council,private"])
def test_a_declaration_outside_the_three_spellings_is_refused(raw):
    with pytest.raises(N.NotepadBootRefusal) as e:
        N.boot({N.ENV_PADS: raw, **BOUNDS})
    assert e.value.reason == N.R_PADS_INVALID


@pytest.mark.parametrize("drop", list(BOUNDS))
def test_each_missing_bound_is_refused_never_defaulted(drop):
    env = {N.ENV_PADS: "private", **BOUNDS}
    del env[drop]
    with pytest.raises(N.NotepadBootRefusal) as e:
        N.boot(env)
    assert e.value.reason == N.R_BOUNDS_UNDECLARED and drop in e.value.detail


def test_a_label_that_is_not_a_seat_is_refused_and_no_label_boots_labelless():
    with pytest.raises(N.NotepadBootRefusal) as e:
        N.boot({N.ENV_PADS: "private", N.ENV_SEAT: "X1", **BOUNDS})
    assert e.value.reason == N.R_SEAT_UNKNOWN
    c = N.boot({N.ENV_PADS: "private", **BOUNDS})
    assert c.enabled and c.seat_refusal() == N.R_SEAT_UNDECLARED


def test_the_transcription_round_trips_through_boot():
    env = N.transcription_for("private,council", private_max_bytes="4096",
                              council_max_bytes="8192", max_write_bytes="1024")
    c = N.boot({**env, N.ENV_SEAT: "D2"})
    assert c.pads == (N.PAD_PRIVATE, N.PAD_COUNCIL) and c.seat == "D2"
    assert (c.private_max_bytes, c.council_max_bytes, c.max_write_bytes) == (4096, 8192, 1024)
    assert N.transcription_for("none") == {N.ENV_PADS: "none"}
    sc = {"notepad.pads.k1": "private", **{k: N.PROPOSED_BOUNDS[k] for k in N.KEY_BOUNDS}}
    assert N.transcription_from_scalars(sc, "k1")[N.ENV_PADS] == "private"
    assert N.transcription_from_scalars(sc, "council") == {}


def test_declared_block_names_absence_as_a_state_and_log_required_keys_on_it():
    assert N.declared_block({}) == {"declared": False, "state": "not_declared", "pads": None}
    blk = N.declared_block({N.ENV_PADS: "private", **BOUNDS})
    assert blk["declared"] and blk["pads"] == ["private"] and blk["max_write_bytes"] == "32"
    assert N.log_required({"notepad": blk})
    assert not N.log_required({"notepad": N.declared_block({N.ENV_PADS: "none"})})
    assert not N.log_required({})                      # a bundle that predates the lane


# ── the store: the write rule, the bounds, the log ──────────────────────────

EID = "ep-20261111T000000Z-note-0badf00d"


@pytest.fixture
def store(tmp_path):
    return N.Store(tmp_path / "notepad" / EID / N.LOG_NAME, episode_id=EID)


def W(c, store, pad, text, mode, version=None, cycle=0, date="867.2.1"):
    return N.note_write_attempt(c, store, pad=pad, text=text, mode=mode, version=version, game_date=date,
                   tick=9, cycle=cycle)


def Rd(c, store, pad, cycle=0, date="867.2.1"):
    return N.note_read_attempt(c, store, pad=pad, game_date=date, tick=9, cycle=cycle)


def test_append_then_replace_against_the_version_read(store):
    k = cfg("K")
    assert W(k, store, "private", "plan: ", "append")["verdict"] == N.V_APPLIED
    assert W(k, store, "private", "take Paris", "append")["verdict"] == N.V_APPLIED
    rec, view = Rd(k, store, "private")
    assert view["text"] == "plan: take Paris" and view["version"] == 2
    assert W(k, store, "private", "new plan", "replace")["verdict"] == N.V_VERSION_REQUIRED
    assert W(k, store, "private", "new plan", "replace", version=1)["verdict"] == N.V_VERSION_STALE
    assert W(k, store, "private", "new plan", "replace", version=2)["verdict"] == N.V_APPLIED
    assert Rd(k, store, "private")[1]["text"] == "new plan"


def test_every_read_and_write_is_logged_with_seat_cycle_date_and_bytes(store):
    d1 = cfg("D1")
    W(d1, store, "council", "hold the Loire", "append", cycle=3, date="867.10.1")
    Rd(d1, store, "council", cycle=4, date="868.1.2")
    W(d1, store, "council", "x" * 33, "append", cycle=4, date="868.1.2")   # refused, still logged
    recs = store.records()
    assert [(r["kind"], r["seat"], r["cycle"], r["game_date"], r["verdict"]) for r in recs] == [
        ("write", "D1", 3, "867.10.1", N.V_APPLIED), ("read", "D1", 4, "868.1.2", N.V_APPLIED),
        ("write", "D1", 4, "868.1.2", N.V_WRITE_TOO_LONG)]
    assert recs[0]["bytes"] == 14 and recs[1]["bytes"] == 14 and recs[2]["bytes"] == 33
    assert all(r["episode_id"] == EID for r in recs)
    assert "text" not in recs[1] and "text" not in recs[2]       # reads and refusals keep no text


def test_bounds_refuse_and_never_truncate(store):
    d1 = cfg("D1")
    assert W(d1, store, "private", "y" * 33, "append")["verdict"] == N.V_WRITE_TOO_LONG
    for _ in range(2):
        assert W(d1, store, "private", "y" * 32, "append")["verdict"] == N.V_APPLIED
    assert W(d1, store, "private", "y", "append")["verdict"] == N.V_PAD_FULL      # 65 > 64
    assert Rd(d1, store, "private")[1]["bytes"] == 64
    assert W(d1, store, "council", "z" * 32, "append")["verdict"] == N.V_APPLIED   # council's own bound


def test_a_seat_whose_arm_has_no_council_pad_is_refused_by_name(store):
    k1 = cfg("K", pads=N.DECL_PRIVATE)
    rec, view = Rd(k1, store, "council")
    assert rec["verdict"] == N.V_PAD_NOT_DECLARED and view is None
    assert W(k1, store, "council", "hi", "append")["verdict"] == N.V_PAD_NOT_DECLARED
    assert W(k1, store, "diary", "hi", "append")["verdict"] == N.V_PAD_UNKNOWN
    assert W(k1, store, "private", "hi", "overwrite")["verdict"] == N.V_MODE_INVALID
    assert W(k1, store, "private", "hi", "append", cycle=None)["verdict"] == N.V_CYCLE_UNKNOWN
    assert [r["verdict"] for r in store.records()] == [
        N.V_PAD_NOT_DECLARED, N.V_PAD_NOT_DECLARED, N.V_PAD_UNKNOWN, N.V_MODE_INVALID, N.V_CYCLE_UNKNOWN]


def test_private_pads_are_per_seat_and_the_council_pad_is_shared(store):
    W(cfg("K"), store, "private", "K's", "append")
    W(cfg("D1"), store, "private", "D1's", "append")
    W(cfg("D1"), store, "council", "from D1;", "append")
    W(cfg("D2"), store, "council", "from D2;", "append")
    assert Rd(cfg("K"), store, "private")[1]["text"] == "K's"
    assert Rd(cfg("D2"), store, "private")[1]["text"] == ""
    assert Rd(cfg("K"), store, "council")[1]["text"] == "from D1;from D2;"


def test_a_replace_never_overwrites_what_the_writer_did_not_see(store):
    k, d1 = cfg("K"), cfg("D1")
    W(k, store, "council", "orders", "append")
    v = Rd(k, store, "council")[1]["version"]
    assert Rd(d1, store, "council")[1]["version"] == v
    assert W(d1, store, "council", "D1 rewrote", "replace", version=v)["verdict"] == N.V_APPLIED
    # K's replace from the same base is refused: D1's text is not lost
    assert W(k, store, "council", "K rewrote", "replace", version=v)["verdict"] == N.V_VERSION_STALE
    assert Rd(k, store, "council")[1]["text"] == "D1 rewrote"


def _appender(path, seat, n):
    s = N.Store(path, episode_id=EID)
    for i in range(n):
        N.note_write_attempt(cfg(seat, council=100000, write=100), s, pad="council", text=f"{seat}:{i};",
                mode="append", version=None, game_date="867.2.1", tick=1, cycle=0)


def _replacer(path, seat, base, q):
    s = N.Store(path, episode_id=EID)
    q.put(N.note_write_attempt(cfg(seat, council=100000, write=100), s, pad="council", text=f"by {seat}",
                  mode="replace", version=base, game_date="867.2.1", tick=1, cycle=0)["verdict"])


def test_concurrent_council_writes_from_separate_processes_lose_nothing(store):
    """Six seat processes append 25 times each at once: every append lands, the
    replay reproduces every applied write's sha, and the version is 150. Then
    six replaces from one base: exactly one is applied, five are stale."""
    ctx = multiprocessing.get_context("fork")
    seats = ["K", "D1", "D2", "D3", "D4", "D5"]
    procs = [ctx.Process(target=_appender, args=(store.path, s, 25)) for s in seats]
    for p in procs:
        p.start()
    for p in procs:
        p.join(60)
        assert p.exitcode == 0
    pad = N.pads(store.records())["council"]
    assert pad.version == 150
    for s in seats:
        assert all(f"{s}:{i};" in pad.text for i in range(25))
    q = ctx.Queue()
    procs = [ctx.Process(target=_replacer, args=(store.path, s, 150, q)) for s in seats]
    for p in procs:
        p.start()
    for p in procs:
        p.join(60)
    verdicts = sorted(q.get(timeout=10) for _ in seats)
    assert verdicts.count(N.V_APPLIED) == 1 and verdicts.count(N.V_VERSION_STALE) == 5


def test_a_log_that_does_not_reproduce_its_pads_raises(store):
    W(cfg("K"), store, "private", "true", "append")
    recs = store.records()
    recs[0]["text"] = "forged"
    with pytest.raises(ValueError):
        N.pads(recs)


def test_seal_writes_every_declared_pad_of_every_seat(store):
    W(cfg("K"), store, "private", "mine", "append")
    W(cfg("D1"), store, "council", "ours", "append")
    Rd(cfg("D2"), store, "council")
    files, report = N.seal(store.records(), pads_declared=["private", "council"], seats=["K", "D1", "D2"])
    assert files == {"notepads/private-K.txt": "mine", "notepads/private-D1.txt": "",
                     "notepads/private-D2.txt": "", "notepads/council.txt": "ours"}
    assert report["pads"]["council"]["reads"] == 1 and report["pads"]["council"]["writes_applied"] == 1
    assert report["seats"]["D2"] == {"reads": 1, "writes": 0, "refused": 0}


def test_the_cycle_is_the_schedules_at_the_date_and_unknown_without_one():
    env = {"CK3_EPISODE_HORIZON_START": "867.1.3", "CK3_CYCLE_GAME_DAYS": "90",
           "CK3_EPISODE_HORIZON_END": "892.1.3"}
    assert N.cycle_at("867.1.3", env) == 0
    assert N.cycle_at("867.4.3", env) == 1                 # day 90: the first boundary
    assert N.cycle_at("869.1.3", env) == 8
    assert N.cycle_at("867.4.3", {}) is None               # no schedule: no guess
    assert N.cycle_at("not a date", env) is None


# ── through the server ──────────────────────────────────────────────────────

CELL_ENV = {"CK3_EPISODE_HORIZON_START": "867.1.3", "CK3_CYCLE_GAME_DAYS": "90",
            "CK3_EPISODE_HORIZON_END": "892.1.3"}


@pytest.fixture
def seat(monkeypatch, tmp_path):
    """A seat's server with the game doubled; the notepad and its log are real."""
    state = ms.BridgeState()
    state.connected = True
    state.tick_id, state.game_date = 9, "867.1.3"
    state._client = type("C", (), {"session_id": "s-seat"})()
    world = {"date": "867.1.3", "primary": 30853}

    def roster():
        return {"get_controlled_roster": {"action": "get_controlled_roster",
                                          "primary_character_id": world["primary"],
                                          "roster": [world["primary"]], "player_character_id": 0},
                "get_bridge_state": {"action": "get_bridge_state", "sessions": [
                    {"session_id": "s-seat", "primary_character_id": world["primary"],
                     "claimed_character_ids": [world["primary"]]}]}}

    async def qb(_state, action, params, timeout=15.0):
        if action == "get_tick_info":
            return {"paused": True, "game_speed": 1, "tick_id": 9, "game_date": world["date"],
                    "pipeline": {"episode_id": EID}}
        r = roster()
        if action in r:
            return r[action]
        return {"character_id": world["primary"], "action": action}

    async def ab(_state, action, params, timeout=5.0):
        return await qb(_state, action, params)

    monkeypatch.setattr(ms, "_get_state", lambda: state)
    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_action_bridge", ab)
    monkeypatch.setenv("CK3_INSTANCE_ID", "note-test")
    monkeypatch.setenv("CK3_INSTANCE_ROOT", str(tmp_path))
    monkeypatch.delenv("CK3_EPISODE_ID", raising=False)
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)

    class Seat:
        def as_seat(self, label, pads=N.DECL_BOTH):
            """A FRESH server process for `label`: a new boot and a new SeatState
            (whose own wait counter starts at 0, as a resumed seat's does)."""
            monkeypatch.setattr(ms, "_NOTEPAD", cfg(label, pads))
            import channel as C
            monkeypatch.setattr(ms, "_SEAT", C.SeatState())
            return self

        def date(self, d):
            world["date"] = d
            state.game_date = d            # the live stamp every bridge reply carries
            return self

        def rebind(self, char_id):
            world["primary"] = char_id
            return self

        def store(self):
            return N.Store(N.log_path(EID), episode_id=EID)
    return Seat()


def run(coro):
    import asyncio
    return asyncio.run(coro)


def test_a_write_and_a_read_through_the_server_carry_the_schedules_cycle(seat):
    seat.as_seat("K").date("867.7.2")                    # day 180: cycle 2
    env = run(ms.note_write(pad="private", text="march on Paris", mode="append"))
    assert env["ok"] is True and env["verdict"] == "applied", env
    assert env["result"]["cycle"] == 2 and env["result"]["game_date"] == "867.7.2"
    env = run(ms.note_read(pad="private"))
    assert env["ok"] is True and env["result"]["text"] == "march on Paris"
    assert env["result"]["version"] == 1 and env["result"]["max_bytes"] == 64
    assert [r["cycle"] for r in seat.store().records()] == [2, 2]


def test_a_resumed_seat_stamps_the_episode_cycle_not_its_own_wait_count(seat):
    """The v3reg2 msg_send defect, not repeated: a resumed process has made no
    wait (its counter reads 0), and its note is stamped with the cycle the
    schedule gives at the bridge's date."""
    seat.as_seat("D1").date("868.1.3")                   # day 365: cycle 4
    assert ms._SEAT.cycles.cycle == 0
    env = run(ms.note_write(pad="council", text="resumed", mode="append"))
    assert env["result"]["cycle"] == 4


def test_the_pad_survives_a_resume_and_a_rebind(seat):
    seat.as_seat("D2")
    run(ms.note_write(pad="private", text="my heir is young", mode="append"))
    seat.as_seat("D2").rebind(77777).date("870.1.1")     # a new process; the seat follows an heir
    env = run(ms.note_read(pad="private"))
    assert env["result"]["text"] == "my heir is young" and env["result"]["updated_by"] == "D2"
    assert env["result"]["seat"] == "D2"
    assert all(r["pad_id"] == "private:D2" for r in seat.store().records())


def test_a_k1_seat_asking_for_the_council_pad_gets_a_named_refusal(seat):
    seat.as_seat("K", pads=N.DECL_PRIVATE)
    for env in (run(ms.note_read(pad="council")),
                run(ms.note_write(pad="council", text="x", mode="append"))):
        assert env["ok"] is False and env["blocked"]["gate"] == N.V_PAD_NOT_DECLARED
    assert [r["verdict"] for r in seat.store().records()] == [N.V_PAD_NOT_DECLARED] * 2


def test_outside_a_cell_schedule_the_call_is_refused_cycle_unknown_and_logged(seat, monkeypatch):
    seat.as_seat("K")
    monkeypatch.delenv("CK3_EPISODE_HORIZON_START")
    env = run(ms.note_write(pad="private", text="x", mode="append"))
    assert env["blocked"]["gate"] == N.V_CYCLE_UNKNOWN
    assert seat.store().records()[0]["cycle"] is None


def test_a_labelless_or_undeclared_process_reaches_no_log(seat, monkeypatch):
    monkeypatch.setattr(ms, "_NOTEPAD", N.NotepadConfig(pads=("private",), seat="", private_max_bytes=8,
                                                        council_max_bytes=8, max_write_bytes=8))
    env = run(ms.note_read(pad="private"))
    assert env["blocked"]["gate"] == N.GATE_UNAVAILABLE
    assert env["blocked"]["measured"]["reason"] == N.R_SEAT_UNDECLARED
    monkeypatch.setattr(ms, "_NOTEPAD", None)
    assert run(ms.note_write(pad="private", text="x", mode="append"))["blocked"]["gate"] == N.GATE_UNAVAILABLE
    assert not N.log_path(EID).exists()


def test_every_notepad_gate_is_in_the_gate_table_under_its_own_class():
    for gate in N.REFUSALS + (N.GATE_UNAVAILABLE,):
        assert gate in R.GATE_TABLE, gate
        assert R.GATE_TABLE[gate]["class"] == "notepad"
    assert not R.GATE_TABLE[N.V_PAD_NOT_DECLARED]["retryable"]
    assert R.GATE_TABLE[N.V_VERSION_STALE]["retryable"]


SRC = (HERE / "mcp_server.py").read_text(encoding="utf-8")


def test_the_tools_are_registered_under_a_guard_never_by_decorator():
    assert re.search(r"^if _NOTEPAD is not None and _NOTEPAD\.enabled:\n    mcp\.add_tool\(note_read\)\n"
                     r"    mcp\.add_tool\(note_write\)", SRC, re.M)
    assert not re.search(r"@mcp\.tool\(\)\nasync def note_", SRC)
    assert (N.TOOL_READ, N.TOOL_WRITE) == ("note_read", "note_write")


def test_the_boot_refusal_is_typed_on_stderr_and_exits_3():
    r = subprocess.run([sys.executable, "-c", "import mcp_server"], cwd=str(HERE),
                       env={"PATH": "/usr/bin:/bin", N.ENV_PADS: "private"},
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 3
    got = json.loads(r.stderr.strip().splitlines()[-1])
    assert (got["refused"], got["reason"]) == ("notepad", N.R_BOUNDS_UNDECLARED)
