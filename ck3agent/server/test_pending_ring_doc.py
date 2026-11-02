"""The completed-verdict ring as the player is told it (cleanup 2026.11, L12 item 7).

The bridge keeps the verdicts of the last MAX_COMPLETED drained writes
(bridge/include/intent_queue.h) and inspect(subject="pending") lists at most
redesign.PENDING_LIST_MAX of them. The protocol and the inspect docstring state
both numbers; this checks each statement against its one definition.
"""
import re
from pathlib import Path

import mcp_server
import redesign

REPO = Path(__file__).resolve().parents[2]


def _first(*rels) -> Path:
    """A file in the source layout or the public export's (fail OPEN: one must exist)."""
    p = next((REPO / r for r in rels if (REPO / r).is_file()), None)
    assert p is not None, f"not found in the source or export layout: {rels}"
    return p


def _ring_size() -> int:
    h = _first("bridge/include/intent_queue.h").read_text()
    m = re.findall(r"static constexpr size_t MAX_COMPLETED = (\d+);", h)
    assert len(m) == 1, m
    return int(m[0])


def _texts():
    return {"protocol": _first("release/overlay/ck3agent/protocol/CLAUDE.md",
                               "ck3agent/protocol/CLAUDE.md").read_text(),
            "inspect docstring": mcp_server.inspect.__doc__ or ""}


def test_the_player_is_told_the_ring_size_the_bridge_keeps():
    n = _ring_size()
    for where, text in _texts().items():
        flat = " ".join(text.split())
        assert f"last {n} " in flat and f"{n + 1}th drops the oldest" in flat, where


def test_the_player_is_told_the_listing_cap_inspect_applies():
    for where, text in _texts().items():
        assert f"at most {redesign.PENDING_LIST_MAX})" in " ".join(text.split()), where


def test_the_ring_reader_reads_the_header():
    """Not a discriminating control: pins that the regex found the constant (a
    reader that matched nothing would fail above with an assertion on [])."""
    assert _ring_size() == 128
