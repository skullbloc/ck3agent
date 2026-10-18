"""Handshake v2 contract tests for BridgeClient / AsyncBridgeClient (ck-6kvq.6).

No game, no socket, no bridge. `FakeBridgeSocket` below is a line-for-line model
of the server side in `bridge/src/ipc_server.cpp`:

  * `handle_handshake_for_slot` (:210-284) defaults `protocol_version` to 1,
    refuses a major above `SUPPORTED_PROTOCOL_MAJOR = 2` with a typed ack, gives
    `protocol_version < 2` an implicit CONTROL session regardless of any `mode`
    field, and honours `mode` only at >= 2;
  * the ack emitter (:1017-1050) replies `{type:handshake_ack, ..., status,
    reason, session_id, session_mode, session_state}` and releases the slot
    (closing the connection) when `status != "ok"`.

What the defect was: every python client sent `{"type":"handshake",
"agent_version":..., "capabilities":[...]}` with no `protocol_version` and no
`mode`, so the bridge minted an implicit CONTROL session for everyone and
observe mode — finished on the bridge since M1 — was unreachable.

Run: python3 -m pytest ck3agent/server/test_handshake_v2.py
"""

from __future__ import annotations

import asyncio
import json
import socket
import sys
import time
from pathlib import Path
from typing import Optional

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from async_bridge_client import AsyncBridgeClient  # noqa: E402
from bridge_client import (  # noqa: E402
    BridgeClient,
    HandshakeRefused,
    MODE_CONTROL,
    MODE_OBSERVE,
    PROTOCOL_VERSION,
)


# ---------------------------------------------------------------------------
# Fake bridge socket — models ipc_server.cpp's handshake slot
# ---------------------------------------------------------------------------

class FakeBridgeSocket:
    """Stand-in for the unix socket to the bridge.

    Implements only what BridgeClient.send/recv touch: settimeout, sendall,
    recv, close. Handshake frames are answered exactly as the bridge answers
    them; `sent` records every frame the client wrote so tests can assert on
    the wire format.
    """

    SUPPORTED_PROTOCOL_MAJOR = 2  # ipc_server.cpp:228

    def __init__(self, *, session_id: str = "sess-7f3a91",
                 echo_session_mode: bool = True,
                 silent: bool = False,
                 # ck-6kvq.5. `state` is the shared registry a reconnecting
                 # client's second FakeBridgeSocket inherits — one bridge
                 # PROCESS outlives many sockets, which is the entire premise of
                 # detach-and-resume, so the model has to outlive the socket too.
                 state: Optional[dict] = None,
                 emit_resume_fields: bool = True):
        self.sent: list[dict] = []
        self.closed = False
        self.session_id = session_id
        self.echo_session_mode = echo_session_mode
        self.silent = silent           # never answers — models a hung bridge
        self.minted_mode: Optional[str] = None
        self.minted_client_name: Optional[str] = None
        self.minted_resume_token: Optional[str] = None
        self.emit_resume_fields = emit_resume_fields
        # {resume_token: {"session_id":..., "world": {...}|None, "claims": [...]}}
        self.state = state if state is not None else {}
        self.state.setdefault("sessions", {})
        self.state.setdefault("declared", None)
        self.state.setdefault("declarations", [])
        self.minted_world_identity: Optional[dict] = None
        self.last_resume_status: Optional[str] = None
        # Token minting is a BRIDGE-PROCESS counter, not a per-socket one: a
        # reconnect must never be handed a token a previous socket already
        # issued, or a stale replay could land on a live session's claims.
        self.state.setdefault("token_seq", 0)
        self._out = b""
        self.timeout = None

    # --- socket surface -----------------------------------------------------

    def settimeout(self, t):
        self.timeout = t

    def sendall(self, data: bytes):
        if self.closed:
            raise BrokenPipeError("fake bridge socket is closed")
        for raw in data.decode("utf-8").splitlines():
            if not raw.strip():
                continue
            msg = json.loads(raw)
            self.sent.append(msg)
            self._handle(msg)

    def recv(self, n: int) -> bytes:
        if self._out:
            chunk, self._out = self._out[:n], self._out[n:]
            return chunk
        if self.closed:
            return b""              # peer gone -> BridgeClient raises ConnectionError
        if self.silent:
            raise socket.timeout()  # what a real socket does at its deadline
        return b""

    def close(self):
        self.closed = True

    # --- bridge behaviour ---------------------------------------------------

    def _emit(self, msg: dict):
        self._out += (json.dumps(msg) + "\n").encode("utf-8")

    def _handle(self, msg: dict):
        if self.silent:
            return
        if msg.get("type") == "action":
            self._handle_action(msg)
            return
        if msg.get("type") != "handshake":
            return

        ack = {
            "type": "handshake_ack",
            "bridge_version": "v8-2026-03-20",
            "binary_build_id": "e61ff2642f07094defd140fcc98e078d42541275",
            "offset_revision": 7,
            "status": "ok",
            "reason": "",
            "session_id": "",
            "session_state": "ACTIVE",
        }

        protocol_version = msg.get("protocol_version", 1)  # absent => 1
        if protocol_version > self.SUPPORTED_PROTOCOL_MAJOR:
            ack["status"] = "unsupported_protocol"
            ack["reason"] = "protocol major not supported by this bridge"
            if self.echo_session_mode:
                ack["session_mode"] = ""      # no session minted
            self._emit(ack)
            self.closed = True                # release_slot_locked("handshake refused")
            return

        if protocol_version < 2:
            mode = MODE_CONTROL               # implicit control session
        else:
            mode = str(msg.get("mode", MODE_CONTROL)).lower()
            if mode not in (MODE_CONTROL, MODE_OBSERVE):
                mode = MODE_CONTROL           # session_mode_from_string default

        self.minted_mode = mode
        self.minted_client_name = msg.get("client_name", "unknown")
        # `minted_resume_token` keeps its ck-6kvq.6 meaning: the token the CLIENT
        # PRESENTED. The token the SERVER MINTS goes back in the ack below.
        self.minted_resume_token = msg.get("resume_token")
        self.minted_world_identity = msg.get("world_identity")

        # --- ck-6kvq.5: resume ------------------------------------------------
        # Mirrors handle_handshake_for_slot(): a presented token is looked up; a
        # session whose recorded world the presented identity REFUTES is erased
        # and the resume refused, and the client silently gets a FRESH session —
        # which is exactly why the ack has to say so.
        presented = msg.get("world_identity") or self.state.get("declared")
        resumed, resume_status, identity_reason = False, "", ""
        token = msg.get("resume_token")
        sid = self.session_id
        if token:
            rec = self.state["sessions"].get(token)
            if rec is None:
                resume_status, identity_reason = "unknown_token", ""
            elif rec.get("attached"):
                resume_status = "session_attached"
                identity_reason = "already_bound_to_a_live_slot"
            else:
                recorded = rec.get("world")
                if not recorded or not presented:
                    resumed, resume_status = True, "ok"
                    identity_reason = "same_world_epoch"
                elif recorded.get("save_sha256") == presented.get("save_sha256"):
                    resumed, resume_status = True, "ok"
                    identity_reason = "declared_same_save"
                else:
                    resume_status = "world_changed"
                    identity_reason = "different_save"
                    del self.state["sessions"][token]     # ERASED, not orphaned
            if resumed:
                sid = rec["session_id"]
                rec["attached"] = True
                if presented:
                    rec["world"] = presented
        self.last_resume_status = resume_status

        if not resumed:
            self.state["token_seq"] += 1
            new_token = f"rsm_fake_{self.state['token_seq']:04x}"
            self.state["sessions"][new_token] = {
                "session_id": sid, "world": presented, "attached": True,
            }
            server_token = new_token
        else:
            server_token = token

        if presented:
            self.state["declared"] = presented
            self.state["declarations"].append(presented)

        ack["session_id"] = sid
        if self.echo_session_mode:
            ack["session_mode"] = mode
        if self.emit_resume_fields:
            ack["resume_token"] = server_token
            ack["resumed"] = resumed
            ack["resume_status"] = resume_status
            ack["resume_identity_reason"] = identity_reason
        self._emit(ack)

    def _handle_action(self, msg: dict):
        """Only `declare_save_identity` — the one action the resume path needs
        (ck-9fc3's entry point). Everything else is ignored, as an unmodelled
        action would be by a bridge that has no such handler."""
        for act in msg.get("actions", []):
            if act.get("action") != "declare_save_identity":
                continue
            params = act.get("params") or {}
            sha = params.get("save_sha256")
            prev = self.state.get("declared")
            if not sha:
                decision = "refused_no_declared_identity"
            elif prev and prev.get("save_sha256") != sha:
                decision = "refused_different_save"
            elif prev and prev.get("save_sha256") == sha:
                decision = "restored_from_quarantine"
            else:
                decision = "nothing_to_restore"
            if sha:
                self.state["declared"] = params
                self.state["declarations"].append(params)
            self._emit({
                "type": "response",
                "request_id": act.get("request_id"),
                "success": bool(sha),
                "action": "declare_save_identity",
                "decision": decision,
                "save_identity": dict(params),
            })

    def drop(self):
        """The socket goes away without the session going away — a
        subscription-cap disconnect under episode_pins_claims(). Every session
        this socket minted stays in `state`, detached and resumable."""
        for rec in self.state["sessions"].values():
            rec["attached"] = False
        self.closed = True


def connected_client(**fake_kwargs) -> tuple[BridgeClient, FakeBridgeSocket]:
    """A BridgeClient wired to a fake bridge, skipping connect()."""
    fake = FakeBridgeSocket(**fake_kwargs)
    client = BridgeClient("/nonexistent/ck3-agent.sock")
    client.sock = fake  # type: ignore[assignment]
    return client, fake


# ---------------------------------------------------------------------------
# Legacy path — the hard gate: the live single-agent stack must not change
# ---------------------------------------------------------------------------

def test_legacy_handshake_frame_is_byte_for_byte_unchanged():
    client, fake = connected_client()
    ack = client.handshake()

    assert fake.sent == [{
        "type": "handshake",
        "agent_version": "0.1.0",
        "capabilities": ["observe", "act", "pause"],
    }]
    assert ack is not None and ack["type"] == "handshake_ack"


def test_legacy_path_still_yields_a_working_control_session():
    client, fake = connected_client()
    client.handshake(agent_version="9.9.9", capabilities=["observe"])

    assert "protocol_version" not in fake.sent[0]
    assert fake.minted_mode == MODE_CONTROL
    assert client.session_id == fake.session_id
    assert client.session_mode == MODE_CONTROL
    assert client.session_state == "ACTIVE"
    assert client.protocol_version_sent is None


# ---------------------------------------------------------------------------
# v2 fields are sent, and the server-minted session_id is surfaced
# ---------------------------------------------------------------------------

def test_v2_fields_are_sent_and_session_id_is_surfaced():
    client, fake = connected_client(session_id="sess-abc123")
    ack = client.handshake(protocol_version=2,
                           client_name="conductor",
                           mode=MODE_CONTROL,
                           resume_token="tok-ep7")

    frame = fake.sent[0]
    assert frame["protocol_version"] == 2
    assert frame["client_name"] == "conductor"
    assert frame["mode"] == MODE_CONTROL
    assert frame["resume_token"] == "tok-ep7"
    # still carries the legacy fields, so an older bridge is unaffected
    assert frame["type"] == "handshake" and "agent_version" in frame

    assert fake.minted_client_name == "conductor"
    assert fake.minted_resume_token == "tok-ep7"
    assert client.session_id == "sess-abc123" == ack["session_id"]
    assert client.handshake_ack is ack
    assert client.protocol_version_sent == 2


def test_client_name_alone_implies_protocol_version_2():
    """Any v2 field upgrades the frame — `mode` is dead below 2, so never send
    a v2 field at v1 by accident."""
    client, fake = connected_client()
    client.handshake(client_name="observer-lane")

    assert fake.sent[0]["protocol_version"] == PROTOCOL_VERSION == 2
    assert fake.sent[0]["mode"] == MODE_CONTROL


# ---------------------------------------------------------------------------
# Observe mode — the whole point of the bead
# ---------------------------------------------------------------------------

def test_observe_mode_is_requested_and_honoured():
    client, fake = connected_client()
    ack = client.handshake(mode=MODE_OBSERVE, client_name="meter-lane")

    frame = fake.sent[0]
    assert frame["mode"] == MODE_OBSERVE
    assert frame["protocol_version"] == 2      # implied; mode needs >= 2
    # honoured: the bridge minted an OBSERVE session and says so in the ack
    assert fake.minted_mode == MODE_OBSERVE
    assert ack["session_mode"] == MODE_OBSERVE
    assert client.session_mode == MODE_OBSERVE
    assert client.session_id == fake.session_id


def test_observe_is_silently_downgraded_at_protocol_version_1():
    """Why the client must default to 2: at v1 the bridge ignores `mode` and
    mints CONTROL anyway (ipc_server.cpp:267-272). The ack's echo exposes it."""
    client, fake = connected_client()
    client.handshake(protocol_version=1, mode=MODE_OBSERVE)

    assert fake.sent[0]["mode"] == MODE_OBSERVE
    assert fake.minted_mode == MODE_CONTROL
    assert client.session_mode == MODE_CONTROL


def test_mode_falls_back_to_requested_when_bridge_does_not_echo():
    """A bridge built before the session_mode echo still gives a usable answer."""
    client, fake = connected_client(echo_session_mode=False)
    ack = client.handshake(mode=MODE_OBSERVE)

    assert "session_mode" not in ack
    assert client.session_mode == MODE_OBSERVE


def test_unknown_mode_is_refused_client_side_before_anything_is_sent():
    client, fake = connected_client()
    with pytest.raises(ValueError):
        client.handshake(mode="spectate")
    assert fake.sent == []


# ---------------------------------------------------------------------------
# Refusals are typed and immediate, never a hang
# ---------------------------------------------------------------------------

def test_unsupported_major_is_reported_typed_not_hung():
    client, fake = connected_client()
    started = time.monotonic()
    with pytest.raises(HandshakeRefused) as exc:
        client.handshake(protocol_version=3, timeout=30.0)
    elapsed = time.monotonic() - started

    assert elapsed < 1.0, "refusal must return immediately, not block on timeout"
    assert exc.value.status == "unsupported_protocol"
    assert "not supported" in exc.value.reason
    assert exc.value.ack["type"] == "handshake_ack"
    # no session was minted, and the dead socket is dropped
    assert client.session_id is None
    assert client.sock is None
    assert fake.closed is True


def test_silent_bridge_returns_none_instead_of_blocking_forever():
    client, fake = connected_client(silent=True)
    started = time.monotonic()
    assert client.handshake(timeout=0.05) is None
    assert time.monotonic() - started < 1.0
    assert client.session_id is None


# ---------------------------------------------------------------------------
# Async wrapper passes the options through
# ---------------------------------------------------------------------------

def test_async_wrapper_forwards_v2_options_and_exposes_session():
    fake = FakeBridgeSocket(session_id="sess-async")
    client = AsyncBridgeClient("/nonexistent/ck3-agent.sock")
    client._sync.sock = fake  # type: ignore[assignment]

    ack = asyncio.run(client.handshake(mode=MODE_OBSERVE, client_name="async-lane"))

    assert fake.sent[0]["protocol_version"] == 2
    assert fake.sent[0]["mode"] == MODE_OBSERVE
    assert fake.sent[0]["client_name"] == "async-lane"
    assert ack["session_id"] == "sess-async"
    assert client.session_id == "sess-async"
    assert client.session_mode == MODE_OBSERVE
    assert client.handshake_ack is ack


def test_async_wrapper_default_call_is_the_legacy_frame():
    fake = FakeBridgeSocket()
    client = AsyncBridgeClient("/nonexistent/ck3-agent.sock")
    client._sync.sock = fake  # type: ignore[assignment]

    asyncio.run(client.handshake())

    assert fake.sent == [{
        "type": "handshake",
        "agent_version": "0.1.0",
        "capabilities": ["observe", "act", "pause"],
    }]
    assert client.session_mode == MODE_CONTROL
