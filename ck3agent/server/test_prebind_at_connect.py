"""ck-mxlt / ck-hlpb: the agent must ARRIVE bound, and say who it is.

The registered prompt says "You ARE Harold Godwinson" — present tense — while
the driver expects the agent to seize control of itself as an early act and the
tool that does it is described as "you rarely call this". The materials
disagree. e0 ruled binding to be SETUP, not a strategic act under study, so it
is repaired in machinery and NOT by editing a word the agent reads.

The load-bearing property is NOT "a claim is sent". It is that the claim is sent
ONLY when a character was declared — an interactive session, or any arm that
declares none, must behave exactly as before.
"""
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import mcp_server  # noqa: E402


_NOT_GIVEN = object()


class _Client:
    """The fake bridge client.

    `ack=None` must mean A FAILED HANDSHAKE, not "use the default". The first
    version of this helper coalesced None to a successful ack, so the
    failed-handshake test could not express its own case and reported a defect
    in the code that was really a defect in the fixture. Same shape as ck-iu11's
    `character_id` helper: a double that cannot represent the condition under
    test agrees with whatever the code does.
    """

    def __init__(self, ack=_NOT_GIVEN):
        self.ack = {"status": "ok"} if ack is _NOT_GIVEN else ack
        self.handshake_kwargs = None

    async def handshake(self, **kw):
        self.handshake_kwargs = kw
        return self.ack


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


def _bind(monkeypatch, env, action_result=None, raises=None):
    """Drive _handshake_and_bind with a fake bridge; return (client, state, sent)."""
    sent = []

    async def _fake_action(state, action, params):
        sent.append((action, params))
        if raises:
            raise raises
        return action_result if action_result is not None else {"success": True}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    if env is None:
        monkeypatch.delenv(mcp_server.AGENT_CHARACTER_ENV, raising=False)
    else:
        monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, env)
    c = _Client()
    st = mcp_server.BridgeState()
    _run(mcp_server._handshake_and_bind(c, st))
    return c, st, sent


def test_NO_declared_character_means_NO_claim(monkeypatch):
    """THE GATE. Absence is a state, never a default and never character 0.

    This is what keeps the repair out of every interactive MCP session and out
    of any arm that declares no character — including E0, which forbids writes.
    """
    c, st, sent = _bind(monkeypatch, None)
    assert sent == [], f"claimed with nothing declared: {sent}"
    assert getattr(st, "prebind", None) is None


def test_a_declared_character_IS_claimed_before_the_first_turn(monkeypatch):
    c, st, sent = _bind(monkeypatch, "30853")
    assert sent == [("set_controlled_character",
                     {"character_id": 30853, "disable_native_ai": False})]
    # lane-prebind added `bound_at` (the reply's clock stamp; this fake has none)
    # and `wait` (the fake ack names no session_state: nothing waited, nothing seen).
    assert st.prebind == {"character_id": 30853, "ok": True, "reason": None,
                          "bound_at": {"tick": None, "date": None},
                          "wait": {"waited_s": 0.0, "polls": 0, "game_active": None}}


def test_a_REFUSED_claim_is_RECORDED_not_assumed(monkeypatch):
    """ck-tygb's lesson applied in the same file: a write whose reply is never
    read leaves the client believing something that did not happen. The agent
    still runs — it can claim for itself, exactly as it does today."""
    c, st, sent = _bind(monkeypatch, "30853",
                        action_result={"success": False,
                                       "reason": "cannot_act_as_player"})
    assert sent, "the claim was attempted"
    assert st.prebind["ok"] is False
    assert st.prebind["reason"] == "cannot_act_as_player"


def test_a_claim_that_cannot_even_be_SENT_is_recorded(monkeypatch):
    c, st, sent = _bind(monkeypatch, "30853", raises=ConnectionError("no socket"))
    assert st.prebind["ok"] is False
    assert "no socket" in str(st.prebind["reason"])


def test_a_MALFORMED_declaration_claims_nothing(monkeypatch):
    """Refuse rather than default. 'harold' is not 0 and not a guess."""
    for bad in ("harold", "", "   ", "-1", "0"):
        c, st, sent = _bind(monkeypatch, bad)
        assert sent == [], f"{bad!r} produced a claim: {sent}"


def test_the_handshake_is_v2_and_NAMED(monkeypatch):
    """ck-hlpb. A bare handshake omits protocol_version, so the bridge assumes
    legacy v1 and logs the session as client='unknown'. v1 fails PERMISSIVE
    (a bare handshake yields session_mode 'control', measured), so this is not
    what blocked the claim — but an observe-only request at v1 silently yields
    CONTROL, and a session that cannot say what it asked for cannot be audited.
    """
    c, st, sent = _bind(monkeypatch, None)
    assert c.handshake_kwargs is not None
    assert c.handshake_kwargs.get("client_name"), "the session must be NAMED"
    # THE MODE MUST BE control, AND THIS IS NOT COSMETIC. ck-zgh4 measured the
    # claim handler live: it refuses observe-mode sessions outright — "observe-
    # mode sessions cannot claim characters". Passing `observe` here would make
    # the pre-bind fail with a signature IDENTICAL to the bug it fixes: roster
    # empty, agent idle, nothing obviously wrong. Two beads (ck-mxlt's claim and
    # ck-hlpb's v2 keywords) land in one edit, and this is precisely the line
    # where that swap would go unnoticed.
    assert c.handshake_kwargs.get("mode") == "control", (
        "an observe-mode session CANNOT claim a character; passing observe here "
        "reproduces the exact failure this bead fixes")


def test_a_failed_handshake_claims_NOTHING(monkeypatch):
    """No ack, no session — claiming into a session that does not exist would
    be a write with nowhere to land."""
    sent = []

    async def _fake_action(state, action, params):
        sent.append(action)
        return {"success": True}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, "30853")
    c = _Client(ack=None)
    st = mcp_server.BridgeState()
    _run(mcp_server._handshake_and_bind(c, st))
    assert sent == [], "claimed despite no handshake ack"
