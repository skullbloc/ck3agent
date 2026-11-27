"""The bridge's `not_on_windows` refusal (bridge/include/win_anchor_refusal.h, answered at
dispatch for a write whose game function has no Windows address) reaches the seat as a named gate,
a known limitation that is never retried, not as `unmapped_refusal` ("a code this server does not
map"), which is what a Proton run read for change_law without this row."""
from pathlib import Path

import redesign as R

ROOT = Path(__file__).resolve().parents[2]


def test_not_on_windows_has_a_gate_that_is_a_known_limitation():
    gate = R.RAW_REASON_TO_GATE["not_on_windows"]
    assert gate == "not_on_this_platform"
    row = R.GATE_TABLE[gate]
    assert row["retryable"] is False and row.get("is_known_limitation") is True
    assert "nothing was sent" in R.GATE_WHY[gate]


def test_the_bridge_spells_the_reason_the_server_maps():
    # the one emitter: dispatch_router.cpp's reply, built from the table header
    src = (ROOT / "bridge/src/dispatch_router.cpp").read_text()
    assert 'r.value_string("not_on_windows")' in src


def test_a_veto_kind_refused_on_windows_reaches_the_seat_as_the_platform_gate():
    # set_ai_veto answers not_on_windows for legacy/faction on Windows (ai_veto.cpp,
    # win_anchor_refusal.h kVetoKindNeeds); the seat's ai_veto path types it through its own table,
    # which read `invalid_argument` (not a known limitation) on a Proton run without this row
    import seat_events as SE
    assert SE.REFUSALS["not_on_windows"] == "not_on_this_platform"
    src = (ROOT / "bridge/src/ai_veto.cpp").read_text()
    assert 'r.value_string("not_on_windows")' in src
