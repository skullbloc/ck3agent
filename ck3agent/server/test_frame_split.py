"""ck-u182 — the frame-split path, tested where the code RUNS.

`acceptance_1qj0_frame_split` owns the pre-fix CONTROL: it stages
`bridge_client.py` from a pinned blob and proves the defect reproduced. That
needs git, and the node's checkout is a `git archive` export — so on the node
the lane exits 3 having graded NOTHING, because its control is its first
section. Measured during deploy-45 verification.

Meanwhile the split path runs in production on every seat and on the driver
(`async_bridge_client` wraps `BridgeClient`; `mcp_server` imports it), and it
FIRED TWICE in registered-d45 slot 1 — 4 documents recovered from frames of
109,262 and 135,749 bytes, 0 bytes dropped. This file is that behaviour,
asserted in the suite CLAUDE.md calls THE gate: no game, no git, no socket.

TWO READERS, TWO LIFETIMES, deliberately. The lane answers "did the fix change
anything, against the code that was broken". This answers "does the split
behave, here, now". Neither subsumes the other, and a disagreement between them
would be a finding rather than a duplicate.
"""
import collections
import json
import os
import socket
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

bridge_client = pytest.importorskip("bridge_client")

DOC_A = '{"a":1}'
DOC_B = '{"type":"action_result","b":2}'


class ScriptedSock:
    """Hands back exactly these chunks, then times out like a quiet bridge."""

    def __init__(self, chunks):
        self.chunks = list(chunks)

    def settimeout(self, _t):
        pass

    def recv(self, _n):
        if self.chunks:
            return self.chunks.pop(0)
        raise socket.timeout("scripted")


def test_the_client_CARRIES_the_frame_split_fix():
    """Reds FIRST and names the absence, so a pre-fix tree gives one legible
    failure instead of twelve fixture ERRORS.

    Measured: against the pinned pre-fix blob this file produced
    `12 errors in 0.03s`, because the autouse fixture below touched
    `FRAMING_RECOVERY` before any test could speak. Twelve errors do red the
    gate, so it discriminated — but "error in setup" says the suite is broken
    where "the fix is absent" says what is true, and the second is the one a
    reader can act on. Same shape as the two rows the refinery caught in
    acceptance_1qj0, and the third time is enough to write it down.
    """
    assert hasattr(bridge_client, "split_frame_line"), (
        "bridge_client has no split_frame_line — this tree predates ck-1qj0 and "
        "a desynchronised frame will kill the episode, not cost a reply")
    assert hasattr(bridge_client, "FRAMING_RECOVERY"), (
        "bridge_client has no FRAMING_RECOVERY — the split may work but nothing "
        "counts it, so the driver can seal no witness of it")


@pytest.fixture(autouse=True)
def _zero_counters():
    """Each test owns the counters. They are module-level by design — the driver
    reads them out of sys.modules to seal them — so a test that inherited a
    previous test's totals would assert on someone else's arithmetic.

    DEFENSIVE on purpose: see the row above. A missing attribute here must not
    turn every test into a setup error.
    """
    if not hasattr(bridge_client, "FRAMING_RECOVERY"):
        yield
        return
    before = dict(bridge_client.FRAMING_RECOVERY)
    bridge_client.FRAMING_RECOVERY.update(
        {"split_lines": 0, "documents_recovered": 0, "bytes_dropped": 0,
         "samples": []})
    yield
    bridge_client.FRAMING_RECOVERY.update(before)


def _client(chunks):
    c = bridge_client.BridgeClient.__new__(bridge_client.BridgeClient)
    # EXACTLY the two attributes the suite's other client tests supply. recv()
    # creates its pending queue lazily for this reason, and a test that passed
    # a third would stop pinning that.
    c.sock, c._buf = ScriptedSock(chunks), b""
    return c


# ── split_frame_line, the pure half ────────────────────────────────────────

def test_a_well_formed_line_is_one_document_and_costs_no_counter():
    assert bridge_client.split_frame_line(DOC_A.encode()) == [{"a": 1}]
    assert bridge_client.FRAMING_RECOVERY["split_lines"] == 0
    assert bridge_client.FRAMING_RECOVERY["samples"] == []


def test_two_documents_on_one_line_both_come_back_in_order():
    got = bridge_client.split_frame_line((DOC_A + DOC_B).encode())
    assert got == [{"a": 1}, {"type": "action_result", "b": 2}]


def test_whitespace_between_documents_is_skipped():
    got = bridge_client.split_frame_line((DOC_A + "  \t " + DOC_B).encode())
    assert len(got) == 2 and got[1]["b"] == 2


def test_an_undecodable_tail_is_dropped_and_its_BYTES_counted():
    tail = '{"c":3,"unterminated":'
    got = bridge_client.split_frame_line((DOC_A + tail).encode())
    assert got == [{"a": 1}], "the complete document before the tail must survive"
    assert bridge_client.FRAMING_RECOVERY["bytes_dropped"] == len(tail.encode())


def test_a_wholly_unparseable_line_yields_nothing_and_drops_all_of_it():
    junk = b"not json at all"
    assert bridge_client.split_frame_line(junk) == []
    assert bridge_client.FRAMING_RECOVERY["bytes_dropped"] == len(junk)


def test_the_sample_is_BOUNDED_so_a_sick_stream_cannot_grow_the_summary():
    for _ in range(bridge_client.FRAMING_SAMPLE_CAP + 4):
        bridge_client.split_frame_line((DOC_A + DOC_B).encode())
    assert (len(bridge_client.FRAMING_RECOVERY["samples"])
            == bridge_client.FRAMING_SAMPLE_CAP)
    # the COUNTS keep going after the samples stop
    assert (bridge_client.FRAMING_RECOVERY["split_lines"]
            == bridge_client.FRAMING_SAMPLE_CAP + 4)


def test_the_sample_keeps_the_length_and_both_edges():
    bridge_client.split_frame_line((DOC_A + DOC_B).encode())
    s = bridge_client.FRAMING_RECOVERY["samples"][0]
    assert s["length_bytes"] == len(DOC_A) + len(DOC_B)
    assert s["documents"] == 2
    assert s["head"].startswith('{"a":1}')
    assert s["tail"].endswith('"b":2}')


# ── recv(), the half that actually ships ──────────────────────────────────

def test_recv_delivers_BOTH_documents_off_one_line_in_order():
    c = _client([(DOC_A + DOC_B).encode() + b"\n"])
    assert c.recv(timeout=5.0) == {"a": 1}
    assert c.recv(timeout=5.0) == {"type": "action_result", "b": 2}


def test_recv_survives_a_garbage_tail_and_keeps_reading_the_NEXT_line():
    c = _client([(DOC_B + '{"d":').encode() + b"\n", DOC_A.encode() + b"\n"])
    assert c.recv(timeout=5.0)["b"] == 2
    assert c.recv(timeout=5.0) == {"a": 1}, (
        "a poisoned line must not cost the frames behind it")


def test_recv_skips_a_ping_that_arrives_ON_a_split_line():
    c = _client([('{"type":"ping"}' + DOC_B).encode() + b"\n"])
    assert c.recv(timeout=5.0)["b"] == 2


def test_recv_returns_None_on_a_quiet_socket_rather_than_raising():
    c = _client([])
    assert c.recv(timeout=0.05) is None


def test_recv_raises_nothing_for_a_line_it_cannot_use_at_all():
    """The whole point: a desynchronised frame costs a REPLY, not the episode.

    Pre-fix this raised JSONDecodeError out of recv(), through call(), to the
    driver's catch-all — registered-d44 slot 1 sealed abort/unknown INVALID at
    1073.10.05 after 51 minutes on exactly that.
    """
    c = _client([b"not json at all\n"])
    assert c.recv(timeout=0.05) is None
    assert bridge_client.FRAMING_RECOVERY["split_lines"] == 1
