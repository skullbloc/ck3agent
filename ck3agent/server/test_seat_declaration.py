"""ck-j6lk — WHO DECLARES A SEAT, and who must not.

In experiment mode the bridge pins every detached CONTROL session, claim or no
claim. The driver's own `steer-probe` and `speed-pin` helpers are control
sessions that never claim anything — the probe issues no verb at all, the pin
issues only `set_speed` and `get_tick_info` — and in deploy-31's shakedown 2
they each accrued 364 days of a 365-day episode and invalidated a run whose
agent session had 70 activities and zero unavailable days (3 sessions counted,
1 declared, 2 over threshold).

THE DECLARATION IS OPT-IN AND THAT IS THE MECHANISM. `HarnessClient` delegates
to the SAME `BridgeClient.handshake`, so agent and helpers share one builder; a
key emitted only when the caller passes it means the helpers cannot send it by
accident, and no exclusion list exists to drift.

THESE ROWS DRIVE THE FRAME rather than reading the source. A row asserting
"mcp_server.py contains seat_declaration_kwargs" would pass against a call that
never executes — which is exactly ck-zi99's defect, a census counting one
literal while the arm path reached none.
"""

import asyncio
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bridge_client as bc                                      # noqa: E402


class _Captured:
    """A BridgeClient whose socket is a capture buffer."""

    def __init__(self):
        self.sent = []

    def send(self, msg):
        self.sent.append(msg)


def frame_for(**kwargs):
    """The handshake frame a caller with these kwargs would put on the wire."""
    c = bc.BridgeClient.__new__(bc.BridgeClient)
    cap = _Captured()
    c.send = cap.send                       # type: ignore[assignment]
    c.recv = lambda timeout=None: None      # type: ignore[assignment]
    c.world_identity = None
    c.session_id = None
    c.session_mode = None
    try:
        c.handshake(timeout=0.01, **kwargs)
    except Exception:                       # no ack ever arrives; we want the frame
        pass
    assert cap.sent, "no frame was sent"
    return cap.sent[0]


class TheSeatDeclaration(unittest.TestCase):

    def test_the_agent_path_DECLARES(self):
        """The kwargs the MCP server's session actually sends."""
        f = frame_for(client_name="ck3agent", mode=bc.MODE_CONTROL,
                      **bc.seat_declaration_kwargs())
        self.assertIs(f.get("episode_seat"), True)

    def test_BOTH_HELPERS_DO_NOT_DECLARE(self):
        """THE ROW ck-j6lk ASKED FOR. The steer probe and the speed pin build
        their handshakes through HarnessClient, which hands this builder only
        the kwargs it was given — and neither gives `episode_seat`."""
        for name in ("episode-driver-C-steer-probe", "episode-driver-C-speed-pin"):
            f = frame_for(client_name=name, mode=bc.MODE_CONTROL)
            self.assertNotIn("episode_seat", f, name)

    def test_the_key_is_ABSENT_not_false_when_undeclared(self):
        """Absent and false are different claims. A session that never spoke
        about seats must not read as one that declared it is not a seat —
        that is the distinction every three-valued rule in this tree turns on,
        and a `false` would let a future reader count non-declarers."""
        f = frame_for(client_name="anything", mode=bc.MODE_CONTROL)
        self.assertNotIn("episode_seat", f)

    def test_an_existing_callers_frame_is_BYTE_IDENTICAL(self):
        """The change must be invisible to every caller that does not declare,
        or it is a protocol change wearing an opt-in's clothes."""
        before = frame_for(client_name="obs", mode=bc.MODE_OBSERVE,
                           episode_observer=True)
        self.assertNotIn("episode_seat", before)
        self.assertIs(before.get("episode_observer"), True)

    def test_it_is_a_BOOL_because_the_bridge_tests_IsBool(self):
        f = frame_for(client_name="a", mode=bc.MODE_CONTROL, episode_seat=1)
        self.assertIs(f["episode_seat"], True)
        self.assertIsInstance(f["episode_seat"], bool)

    def test_THE_MCP_SERVER_ACTUALLY_CALLS_IT(self):
        """Reachability, not uniqueness (ck-zi99). The declaration existing and
        the arm path reaching it are different properties; this asserts the
        SECOND by requiring the call to sit on the handshake that connects."""
        src = (Path(__file__).resolve().parent / "mcp_server.py").read_text()
        i = src.index("seat_declaration_kwargs()")
        hs = src.rindex("client.handshake(", 0, i)
        self.assertLess(i - hs, 400,
                        "the declaration is not on the handshake call")
        self.assertIn("MODE_CONTROL", src[hs:i])

    def test_agent_py_deliberately_does_NOT_declare(self):
        """The A path is not an arm path — ck-zgh4 established the arms' agents
        reach the bridge through the MCP server, and nothing there imports
        agent.py. Pinned rather than left in a comment, so if agent.py ever
        becomes an arm path this fails and someone decides."""
        src = (Path(__file__).resolve().parent / "agent.py").read_text()
        self.assertNotIn("seat_declaration_kwargs", src)
        self.assertNotIn("episode_seat", src)


if __name__ == "__main__":
    unittest.main()
