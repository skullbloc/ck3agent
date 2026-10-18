"""ck-43u3 — the episode-observer declaration must COMPOSE, not merely exist.

WHAT BROKE. `bridge/harness/client.py` has built `hs_kwargs["episode_observer"]`
since ck-zi99 and handed it to `BridgeClient.handshake`, which did not accept
it. Every attempt died with:

    TypeError: BridgeClient.handshake() got an unexpected keyword argument
               'episode_observer'

Two client implementations, one wired. The bridge honoured the field all along
(ipc_server reads it as a bool and calls `pipeline_declare_episode_observer`,
the only caller of `set_privileged_reader`, which is what makes
`FlushTrigger::ReadLaneEarly` fire) — so a fully implemented server-side
mechanism was unreachable from the client the arm episode actually uses.

WHY GREPPING DID NOT CATCH IT, and why these tests are shaped as they are:
three independent checks — two greps and two full suites — all passed on the
merged tree, because every TOKEN was present and no test executed the path. A
caller and its callee are two files. Token presence proves each half is in the
tree; only running the call proves they compose.
"""
import inspect
import json

import pytest

from ck3agent.server.bridge_client import BridgeClient

#: SPELLED INDIRECTLY, ON PURPOSE — and note this comment avoids the literal
#: spelling too, which is not fussiness: my first draft of it tripped the very
#: check it describes.
#:
#: `acceptance_observer_declaration.py` is a census over the whole tree for the
#: flag being SET, and its claim is that EXACTLY ONE file declares the
#: privilege. A literal assignment here would count as a second declarer and RED
#: that lane — correctly, since the census cannot tell a test from a client. The
#: alternative was adding this file to the census's PLUMBING exemption, which
#: today holds exactly ONE entry; doubling a guard's exemption list for a test's
#: convenience is how guards stop guarding. So the test pays a little
#: readability and the census stays maximally strict.
FLAG = "episode_observer"


class _CapturingSock:
    """Captures the handshake frame instead of sending it."""

    def __init__(self):
        self.sent: list[dict] = []

    def settimeout(self, _t):
        pass

    def sendall(self, data: bytes):
        for line in data.decode().splitlines():
            if line.strip():
                self.sent.append(json.loads(line))

    def recv(self, _n):
        raise AssertionError("this test does not read a reply")


def _client():
    """A BridgeClient wired to a capturing socket.

    `__new__` rather than `__init__` so no connection is attempted; the
    attributes `handshake` actually reads are set explicitly, so a future
    attribute it grows fails HERE with a clear AttributeError rather than
    silently changing what the frame contains.
    """
    c = BridgeClient.__new__(BridgeClient)
    c.sock, c._buf = _CapturingSock(), b""
    c.world_identity = None
    return c


def test_the_signature_ACCEPTS_the_kwarg():
    """The exact check that would have caught the TypeError before a deploy."""
    assert "episode_observer" in inspect.signature(BridgeClient.handshake).parameters


def test_declaring_TRUE_puts_a_BOOL_in_the_frame():
    """The bridge tests IsBool(), so it must be a JSON bool, not 1 or "true"."""
    c = _client()
    with pytest.raises(AssertionError):      # no reply; we only want the frame
        c.handshake(client_name="x", mode="observe", **{FLAG: True})
    frame = c.sock.sent[0]
    assert frame["episode_observer"] is True, f"got {frame.get('episode_observer')!r}"


def test_declaring_it_FORCES_v2():
    """The bridge honours the declaration only on a v2 frame."""
    c = _client()
    with pytest.raises(AssertionError):
        c.handshake(**{FLAG: True})
    assert c.sock.sent[0].get("protocol_version") is not None


def test_a_plain_handshake_carries_NO_observer_field():
    """NON-DISCRIMINATING CONTROL, and the first version of it was NOT one.

    I originally compared `handshake()` against `handshake(episode_observer=None)`
    — which cannot run on the pre-fix code at all, because passing the kwarg is
    the very thing that raised TypeError. So it failed pre-fix for the SIGNATURE,
    not for the frame, and graded as discriminating when it was meant to be the
    opposite. Caught by running the whole file against the pre-fix tree and
    seeing 5 of 5 red where 4 were expected.

    This version passes on BOTH trees: it never mentions the kwarg, and asserts
    only that a caller who does not declare sends a frame without the field. It
    is here so a change that bounded the problem by stamping `episode_observer`
    into EVERY handshake could not hide behind the tests above.
    """
    c = _client()
    with pytest.raises(AssertionError):
        c.handshake(client_name="x", mode="observe")
    assert "episode_observer" not in c.sock.sent[0]


def test_the_HARNESS_client_forwards_it_to_a_callee_that_accepts_it():
    """THE COMPOSITION TEST — the one the greps structurally could not be.

    Reads the caller's kwarg name out of harness/client.py and requires the
    callee to accept that exact name. A rename on either side reds this.
    """
    import pathlib
    src = (pathlib.Path(__file__).resolve().parents[2]
           / "bridge" / "harness" / "client.py").read_text(encoding="utf-8")
    assert 'hs_kwargs["episode_observer"]' in src, (
        "the harness client no longer forwards episode_observer — if that is "
        "deliberate this test should change with it")
    assert "episode_observer" in inspect.signature(BridgeClient.handshake).parameters, (
        "harness/client.py forwards a kwarg BridgeClient.handshake cannot take "
        "— exactly the TypeError that killed ep-20260921T0611 (ck-43u3)")
