"""ck-switch — a seat has to know WHICH seat it is, and the paths are traced.

`session(action='whoami')` used to report `controlled_primary`, which is the
GLOBAL primary — the front of the claim order. In a six-seat arm every seat
asking "who am I" was told the same character, and for five of them it was not
theirs. A true switch depends on a seat knowing what it holds, so this had to be
right before `control` could be.

THE FIRST VERSION OF THIS WAS WRONG AND NO BEHAVIOURAL TEST COULD HAVE SAID SO.
I wrote a list of four candidate attribute paths from memory — ("state",
"_client"), ("_a", "_client"), ("client",), ("_client",) — and doubles shaped
after that list passed, because a double built from my model cannot falsify my
model. Tracing the two real gateway classes found that `_AgentGateway` keeps its
client at `_a.bridge`, not `_a._client`: on the AUTONOMOUS path, the one that
actually drives an arm episode, every seat would have answered "I do not know
which seat I am". The bare ("client",) was worse than useless — on CK3Agent that
name is the ANTHROPIC client, so the path could reach a wrong object rather than
no object.

So these rows check the REAL classes. If someone renames BridgeState._client or
CK3Agent.bridge, a row here reds — rather than every seat quietly losing its
identity in the next arm.
"""

import ast
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")


def test_the_declared_paths_are_the_two_gateway_shapes():
    assert redesign.OWN_SESSION_CLIENT_PATHS == (("state", "_client"), ("_a", "bridge"))


def test_path_one_resolves_on_the_REAL_BridgeGateway():
    mcp_server = pytest.importorskip("mcp_server")
    # gw.state -> BridgeState, which declares _client
    assert "_client" in getattr(mcp_server.BridgeState, "__annotations__", {}), (
        "BridgeState no longer declares _client — own_session_id's first path is "
        "dead and every MCP seat has just lost its identity")
    gw = mcp_server.BridgeGateway.__new__(mcp_server.BridgeGateway)
    gw.state = mcp_server.BridgeState()
    gw.state._client = type("C", (), {"session_id": "sess_abc_1"})()
    assert redesign.own_session_id(gw) == "sess_abc_1"


def test_path_two_resolves_on_the_REAL_AgentGateway():
    agent = pytest.importorskip("agent")
    # CK3Agent.__init__ must still assign self.bridge — read from the SOURCE,
    # because constructing a CK3Agent wants an API key and a socket.
    src = ast.parse(open(agent.__file__).read())
    cls = next(n for n in ast.walk(src)
               if isinstance(n, ast.ClassDef) and n.name == "CK3Agent")
    assigned = {
        t.attr for fn in cls.body if isinstance(fn, ast.FunctionDef)
        for n in ast.walk(fn) for t in getattr(n, "targets", [])
        if isinstance(t, ast.Attribute) and isinstance(t.value, ast.Name)
        and t.value.id == "self"
    }
    assert "bridge" in assigned, (
        "CK3Agent no longer assigns self.bridge — own_session_id's second path is "
        "dead and the AUTONOMOUS path, the one that drives arm episodes, has just "
        "lost its identity")
    gw = agent._AgentGateway.__new__(agent._AgentGateway)
    gw._a = type("A", (), {"bridge": type("C", (), {"session_id": "sess_xyz_2"})()})()
    assert redesign.own_session_id(gw) == "sess_xyz_2"


def test_the_client_both_paths_land_on_really_exposes_session_id():
    abc = pytest.importorskip("async_bridge_client")
    assert hasattr(abc.AsyncBridgeClient, "session_id"), (
        "both paths land on AsyncBridgeClient; without session_id neither can work")


def test_an_UNKNOWN_seat_says_so_instead_of_borrowing_the_global_primary():
    """None is a STATE. The whole defect was answering with SOMEBODY's character
    when the right answer was "I cannot tell"."""
    assert redesign.own_session_id(object()) is None
    gw = type("G", (), {"state": type("S", (), {"_client": None})()})()
    assert redesign.own_session_id(gw) is None
    # a client present but not yet handshaken carries session_id None
    gw2 = type("G", (), {"state": type("S", (), {"_client": type("C", (), {"session_id": None})()})()})()
    assert redesign.own_session_id(gw2) is None
    # an empty string is not an identity either
    gw3 = type("G", (), {"state": type("S", (), {"_client": type("C", (), {"session_id": ""})()})()})()
    assert redesign.own_session_id(gw3) is None


def test_own_session_row_keys_on_the_SESSION_not_the_global_primary():
    state = {"controlled_primary": 30853,
             "sessions": [{"session_id": "a", "primary_character_id": 30853,
                           "claimed_character_ids": [30853]},
                          {"session_id": "b", "primary_character_id": 55555,
                           "claimed_character_ids": [55555, 77777]}]}
    # seat b is NOT the global primary, and that is the whole point
    row = redesign.own_session_row(state, "b")
    assert row["primary_character_id"] == 55555
    assert row["claimed_character_ids"] == [55555, 77777]
    assert redesign.own_session_row(state, "c") is None
    assert redesign.own_session_row(state, None) is None
    assert redesign.own_session_row(None, "a") is None
