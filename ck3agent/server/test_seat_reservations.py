"""2026.12.7: a seat cannot claim a character reserved for another declared seat.

v3pilot5 (weak_orders, weak_intent): the king seat called session(action="add")
on all five dukes' declared rulers in its first minute and held them; the dukes
were refused owned_by_other (weak_intent D4: 43 times). The bridge could not
stop it: none of the dukes had bound yet, so every add SUCCEEDED. The cell now
declares each seat's reservations (CK3_RESERVED_FOR_OTHER_SEATS: the OTHER
seats' declared ids) and the server refuses the claim before any bridge call.

Controls, each of which RUNS:
  * PRE-FIX: the scenario rows below, driven through redesign.py at the pre-fix
    blob PRE_FIX_REDESIGN_BLOB, send the add to the bridge (the defect).
  * NON-DISCRIMINATING (labelled): an add of an unreserved id is applied in both
    trees; it passes before and after and cannot tell the fix from no fix.
  * NEGATIVE: without the env, add and control return byte for byte what the
    pre-fix tree returns for the same reply, and nothing is written to the sink.
  * MUTANTS (source-text, loaded as modules): the check dropped, the reason
    swapped, and a self-reservation read as valid; each is caught.
"""
import asyncio
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import redesign  # noqa: E402
import seat_lineage  # noqa: E402
import seat_reservations as resv  # noqa: E402

#: redesign.py at release-prep-2026.10 1ff50e61, the tree before this fix.
PRE_FIX_REDESIGN_BLOB = "8f3fe8f7695bd45703fee2f3a370e2ccfe0197bc"

KING, D1, D2 = 12015, 12137, 14445


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


class _Gw:
    def __init__(self, reply=None):
        self.reply = reply if reply is not None else {
            "success": True, "added": True, "controlled_primary": KING,
            "claimed_character_ids": [KING, D2], "controlled_character_id": D2}
        self.health, self.writes = "ok", []

    async def write(self, action, params):
        self.writes.append((action, dict(params)))
        return self.reply

    async def read(self, action, params):
        if action == "query_character":
            return {"id": params.get("character_id"), "name": "Halfdan", "alive": True}
        return {}

    def game(self):
        return {"tick": 7, "date": "867.01.05", "paused": True, "speed": 1}


def _cell_env(monkeypatch, tmp_path, *, own=KING, reserved=f"{D1},{D2}", sink=True, declared=KING):
    monkeypatch.setenv(resv.AGENT_CHARACTER_ENV, str(own))
    if reserved is None:
        monkeypatch.delenv(resv.RESERVED_ENV, raising=False)
        monkeypatch.delenv(resv.DECLARED_ENV, raising=False)
    else:
        monkeypatch.setenv(resv.RESERVED_ENV, reserved)
        monkeypatch.setenv(resv.DECLARED_ENV, str(declared))
    monkeypatch.setenv("CK3_SEAT_LABEL", "K")
    monkeypatch.setenv(seat_lineage.SESSION_ID_ENV, "seat-K-12015")
    path = tmp_path / "seat-K-12015.seat_lineage.jsonl"
    if sink:
        monkeypatch.setenv(seat_lineage.LINEAGE_OUT_ENV, str(path))
    else:
        monkeypatch.delenv(seat_lineage.LINEAGE_OUT_ENV, raising=False)
    return path


def _claim(mod, action, cid, reply=None):
    gw = _Gw(reply)
    return gw, _run(mod.session_impl(gw, action, character_id=cid))


def refused_reserved(mod, action, cid) -> bool:
    """THE property: a claim of a reserved id is refused reserved_for_another_seat
    and nothing reaches the bridge."""
    gw, out = _claim(mod, action, cid)
    return (out.get("ok") is False and (out.get("blocked") or {}).get("gate") == resv.R_RESERVED
            and gw.writes == [])


def _load(name, text):
    """A module from source text, beside this directory's siblings (registered
    under its own name, so a dataclass in it resolves its module)."""
    spec = importlib.util.spec_from_loader(name, loader=None)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    exec(compile(text, f"<{name}>", "exec"), mod.__dict__)
    return mod


def _pre_fix_redesign():
    try:
        old = subprocess.run(["git", "-C", str(HERE), "cat-file", "blob", PRE_FIX_REDESIGN_BLOB],
                             capture_output=True, check=True, timeout=30).stdout
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        pytest.skip(f"blob {PRE_FIX_REDESIGN_BLOB} is not in this clone (an export has no history)")
    return _load("_redesign_pre_fix", old.decode("utf-8"))


# ── the parser ───────────────────────────────────────────────────────────────

@pytest.mark.parametrize("raw", [None, "", "   "])
def test_absent_or_empty_reserves_nothing(raw):
    r = resv.parse(raw, str(KING))
    assert r.ids == frozenset() and r.error is None and r.declared is False


def test_a_list_reserves_exactly_its_ids():
    r = resv.parse(f" {D2},{D1} ", str(KING))
    assert r.ids == {D1, D2} and r.error is None and r.declared is True
    assert resv.render([D2, D1, D2]) == f"{D1},{D2}"


@pytest.mark.parametrize("raw", ["12137,x", "12137,,14445", "0", "-5", "12137;14445"])
def test_a_malformed_value_is_an_error_never_no_reservations(raw):
    r = resv.parse(raw, str(KING))
    assert r.error is not None and r.ids == frozenset()


def test_a_self_reservation_is_malformed():
    r = resv.parse(f"{D1},{KING}", str(KING))
    assert r.error is not None and str(KING) in r.error


@pytest.mark.parametrize("declared", [None, "", "x", "0", f"{KING},{D1}"])
def test_reservations_without_the_seats_declared_id_are_malformed(declared):
    assert resv.parse(f"{D1},{D2}", declared).error is not None


# ── the refusal ──────────────────────────────────────────────────────────────

@pytest.mark.parametrize("action", resv.CLAIM_ACTIONS)
def test_a_reserved_claim_is_refused_before_the_bridge(monkeypatch, tmp_path, action):
    sink = _cell_env(monkeypatch, tmp_path)
    gw, out = _claim(redesign, action, D2)
    assert out["ok"] is False and gw.writes == [], out
    b = out["blocked"]
    assert b["gate"] == "reserved_for_another_seat" and b["retryable"] is False
    assert b["is_known_limitation"] is True
    assert "another seat" in b["summary"] and "must not claim" in b["summary"]
    assert b["measured"] == {"character_id": D2, "action": action}
    assert not out.get("result")
    # the attempt is evidence: one claim_refused note naming the seat and the target
    rows = [json.loads(x) for x in sink.read_text().splitlines()]
    assert len(rows) == 1
    r = rows[0]
    assert r["kind"] == seat_lineage.KIND_CLAIM_REFUSED == "claim_refused"
    assert (r["seat_label"], r["seat_session_id"], r["character_id"], r["action"], r["reason"]) == \
        ("K", "seat-K-12015", D2, action, "reserved_for_another_seat")
    assert (r["game_date"], r["tick"]) == ("867.01.05", 7)


def test_the_property_holds_on_this_tree(monkeypatch, tmp_path):
    _cell_env(monkeypatch, tmp_path)
    assert refused_reserved(redesign, "add", D2) and refused_reserved(redesign, "control", D1)


@pytest.mark.parametrize("action", resv.CLAIM_ACTIONS)
def test_the_owner_and_an_unreserved_heir_still_go_to_the_bridge(monkeypatch, tmp_path, action):
    """A reservation never blocks the seat's own declared ruler, and an heir
    reserved for nobody (the seat's rebind after a succession) is claimable."""
    sink = _cell_env(monkeypatch, tmp_path)
    for cid in (KING, 99001):
        gw, out = _claim(redesign, action, cid)
        assert len(gw.writes) == 1 and gw.writes[0][1]["character_id"] == cid, out
        assert (out.get("blocked") or {}).get("gate") != "reserved_for_another_seat"
    assert not sink.exists()


@pytest.mark.parametrize("raw", ["12137,x", f"{D1},{KING}"])
def test_a_malformed_reservation_refuses_every_claim(monkeypatch, tmp_path, raw):
    sink = _cell_env(monkeypatch, tmp_path, reserved=raw)
    for cid in (KING, D2, 99001):
        gw, out = _claim(redesign, "add", cid)
        assert out["ok"] is False and gw.writes == []
        assert out["blocked"]["gate"] == "seat_reservations_unreadable"
        assert out["blocked"]["retryable"] is False and out["blocked"]["measured"]["error"]
    assert [json.loads(x)["reason"] for x in sink.read_text().splitlines()] == \
        ["seat_reservations_unreadable"] * 3


def test_release_is_not_a_claim(monkeypatch, tmp_path):
    _cell_env(monkeypatch, tmp_path)
    gw, _ = _claim(redesign, "release", D2, reply={"success": True})
    assert gw.writes and gw.writes[0][0] == "release_character"


def test_no_sink_outside_a_cell_writes_nothing(monkeypatch, tmp_path):
    _cell_env(monkeypatch, tmp_path, sink=False)
    gw, out = _claim(redesign, "add", D2)
    assert out["blocked"]["gate"] == "reserved_for_another_seat" and gw.writes == []
    assert list(tmp_path.iterdir()) == []


def test_the_gate_vocabulary_names_both(monkeypatch):
    for g in resv.REASONS:
        assert g in redesign.GATE_TABLE and g in redesign.GATE_WHY, g
        assert redesign.why_blocked(g)["gate"] == g          # never unmapped_refusal
    for g in resv.REASONS:
        assert g in redesign.GATE_NEXT, g                       # witness r1 F3
        # the server decides them; the class says so as near as the vocabulary can
        assert redesign.GATE_TABLE[g]["class"] == "steer_vs_control", g
    # server-side, not the bridge's: no set_controlled_character reply carries them
    assert not set(resv.REASONS) & set(redesign.CONTROL_REFUSAL_REASONS)


def test_the_session_tool_says_so():
    import mcp_server
    assert "reserved_for_another_seat" in mcp_server.session.__doc__
    assert mcp_server.AGENT_CHARACTER_ENV == resv.AGENT_CHARACTER_ENV


# ── the sink note changes no lineage ─────────────────────────────────────────

def test_a_claim_refused_row_is_not_lineage(tmp_path):
    p = tmp_path / "s.jsonl"
    note = {"kind": "claim_refused", "character_id": D2, "reason": "reserved_for_another_seat"}
    p.write_text(json.dumps(note) + "\n")
    assert seat_lineage.prior_from_sink(str(p)) is None           # as an empty sink
    failed = {"kind": seat_lineage.KIND_READ_FAILED, "reason": "get_bridge_state:not_bound_yet"}
    p.write_text("".join(json.dumps(r) + "\n" for r in (failed, note, failed)))
    prior = seat_lineage.prior_from_sink(str(p))
    assert prior.consecutive_failed == 2                           # the note did not reset it
    assert seat_lineage._verdict_rows([note]) == []


# ── controls ─────────────────────────────────────────────────────────────────

def test_PRE_FIX_CONTROL_the_pre_fix_tree_sends_the_reserved_add(monkeypatch, tmp_path):
    """Reds on the pre-fix tree: the same scenario there reaches the bridge."""
    old = _pre_fix_redesign()
    _cell_env(monkeypatch, tmp_path)
    assert not refused_reserved(old, "add", D2)
    gw, out = _claim(old, "add", D2)
    assert gw.writes == [("set_controlled_character",
                          {"character_id": D2, "disable_native_ai": False, "add": True})]
    assert out["ok"] is True


def test_NON_DISCRIMINATING_CONTROL_an_unreserved_add_applies_in_both_trees(monkeypatch, tmp_path):
    """Labelled: passes before AND after the fix, so it cannot tell them apart."""
    old = _pre_fix_redesign()
    _cell_env(monkeypatch, tmp_path)
    for mod in (old, redesign):
        gw, out = _claim(mod, "add", 99001)
        assert out["ok"] is True and len(gw.writes) == 1


@pytest.mark.parametrize("action", resv.CLAIM_ACTIONS)
@pytest.mark.parametrize("cid", [D2, KING, 0])
def test_NEGATIVE_CONTROL_without_the_env_add_is_byte_for_byte_the_pre_fix(monkeypatch, tmp_path,
                                                                            action, cid):
    old = _pre_fix_redesign()
    _cell_env(monkeypatch, tmp_path, reserved=None)
    outs = []
    for mod in (old, redesign):
        gw, out = _claim(mod, action, cid)
        outs.append((json.dumps(out, sort_keys=True, default=str), gw.writes))
    assert outs[0] == outs[1]
    assert list(tmp_path.iterdir()) == []


def _mutant(old, new):
    src = (HERE / "redesign.py").read_text(encoding="utf-8")
    assert src.count(old) >= 1, old
    return _load("_redesign_mutant", src.replace(old, new))


def test_MUTANT_dropping_the_check_is_caught(monkeypatch, tmp_path):
    _cell_env(monkeypatch, tmp_path)
    m = _mutant("    why = _resv.refusal(character_id)\n", "    why = None\n")
    assert not refused_reserved(m, "add", D2)


def test_MUTANT_dropping_the_add_sites_call_is_caught(monkeypatch, tmp_path):
    _cell_env(monkeypatch, tmp_path)
    m = _mutant('_reserved = reserved_claim_refusal(gw, "add", character_id)', "_reserved = None")
    assert not refused_reserved(m, "add", D2) and refused_reserved(m, "control", D2)


def test_MUTANT_swapping_the_reason_is_caught(monkeypatch, tmp_path):
    _cell_env(monkeypatch, tmp_path)
    m = _mutant("    why = _resv.refusal(character_id)\n",
                "    why = _resv.refusal(character_id)\n"
                "    why = 'owned_by_other' if why == _resv.R_RESERVED else why\n")
    assert not refused_reserved(m, "add", D2)


def test_MUTANT_reading_a_self_reservation_as_valid_is_caught(monkeypatch, tmp_path):
    """A parser that accepted the seat's own id would block the owner."""
    src = (HERE / "seat_reservations.py").read_text(encoding="utf-8")
    old = "    if own in ids:\n"
    assert old in src
    m = _load("_resv_mutant", src.replace(old, "    if False:\n"))
    r = m.parse(f"{D1},{KING}", str(KING))
    assert r.error is None and KING in r.ids          # the mutant's reading ...
    assert resv.parse(f"{D1},{KING}", str(KING)).error is not None   # ... which this tree refuses


# ── witness r1 F1: a succession can carry another seat's declared ruler ──────
# `ck3agent run` rewrites CK3_AGENT_CHARACTER_ID to the rulers a continuation
# CARRIES (cli.py, the carry after a hand-over). After a succession that may be
# another seat's declared ruler (the lineage rebind, the bridge's heir handoff).
# At 1eaab8ac the self check compared against it, read a self-reservation, and
# refused every later claim `seat_reservations_unreadable`.

#: seat_reservations.py at lane/resv 1eaab8ac, the self check against CK3_AGENT_CHARACTER_ID.
R1_RESV_BLOB = "68d4d16e6cf2e75be49d5d87afaed5b4f8005102"


def _succession_env(monkeypatch, tmp_path):
    """K's continuation after K died and the heir is D2's declared ruler."""
    return _cell_env(monkeypatch, tmp_path, own=D2, declared=KING)


def test_a_carried_heir_who_is_another_seats_ruler_does_not_lock_the_seat(monkeypatch, tmp_path):
    _succession_env(monkeypatch, tmp_path)
    assert resv.read().error is None and resv.read().ids == {D1, D2}
    gw, out = _claim(redesign, "add", 99001)            # an unreserved claim still goes through
    assert len(gw.writes) == 1 and out["ok"] is True, out
    assert refused_reserved(redesign, "add", D1)        # D1's ruler is still D1's


def test_R1_CONTROL_the_1eaab8ac_parser_locked_the_seat(monkeypatch, tmp_path):
    """Reds on 1eaab8ac: there the same environment is unreadable."""
    try:
        old = subprocess.run(["git", "-C", str(HERE), "cat-file", "blob", R1_RESV_BLOB],
                             capture_output=True, check=True, timeout=30).stdout
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        pytest.skip(f"blob {R1_RESV_BLOB} is not in this clone (an export has no history)")
    _succession_env(monkeypatch, tmp_path)
    m = _load("_resv_r1", old.decode("utf-8"))
    assert m.refusal(99001) == m.R_UNREADABLE           # the defect
    assert resv.refusal(99001) is None                  # this tree


def test_coming_to_hold_another_seats_ruler_by_rebind_is_witnessed(monkeypatch, tmp_path):
    """Not refused (the lineage route is not a session claim), and not silent: a
    holds_reserved note follows the rebind row."""
    import test_seat_lineage as tsl
    sink = _succession_env(monkeypatch, tmp_path)
    gw = tsl._GW(tsl._row(), alive=True, holder=None)
    prior = tsl._step(gw, None, sink)                    # bound to 5
    gw.alive, gw.holder = False, D2                      # 5 dies; the title passes to D2's ruler
    prior = tsl._step(gw, prior, sink)
    assert gw.writes == [("set_controlled_character", {"character_id": D2, "disable_native_ai": False})]
    rows = [json.loads(x) for x in sink.read_text().splitlines()]
    assert [r["kind"] for r in rows] == ["bound", "rebind", "holds_reserved"]
    assert rows[2]["character_id"] == D2 and rows[2]["via_kind"] == "rebind"
    assert prior.ruler == D2 and not prior.ended
    # a note: the resumed server's lineage is the same without it
    p2 = seat_lineage.prior_from_sink(str(sink))
    assert p2.ruler == D2 and [r["kind"] for r in p2.rows] == ["bound", "rebind"]


def test_a_rebind_to_an_unreserved_heir_writes_no_note(monkeypatch, tmp_path):
    import test_seat_lineage as tsl
    sink = _succession_env(monkeypatch, tmp_path)
    gw = tsl._GW(tsl._row(), alive=True, holder=None)
    prior = tsl._step(gw, None, sink)
    gw.alive, gw.holder = False, 12
    tsl._step(gw, prior, sink)
    assert [json.loads(x)["kind"] for x in sink.read_text().splitlines()] == ["bound", "rebind"]


def test_MUTANT_comparing_against_the_carried_rulers_is_caught(monkeypatch, tmp_path):
    src = (HERE / "seat_reservations.py").read_text(encoding="utf-8")
    old = "    return parse(env.get(RESERVED_ENV), env.get(DECLARED_ENV))\n"
    assert old in src
    m = _load("_resv_carried", src.replace(old, "    return parse(env.get(RESERVED_ENV), "
                                                "env.get(AGENT_CHARACTER_ENV))\n"))
    _succession_env(monkeypatch, tmp_path)
    assert m.refusal(99001) == m.R_UNREADABLE and resv.refusal(99001) is None
