"""
Async wrapper around BridgeClient for use with the MCP server.
Uses asyncio.to_thread to wrap the synchronous socket operations.
"""

import asyncio
from typing import Optional

from bridge_client import (  # noqa: F401  (HandshakeRefused re-exported)
    BridgeClient,
    HandshakeRefused,
    MODE_CONTROL,
    MODE_OBSERVE,
    PROTOCOL_VERSION,
    SOCKET_PATH,
)


class AsyncBridgeClient:
    """Async-compatible bridge client wrapping the sync BridgeClient."""

    def __init__(self, socket_path: str = SOCKET_PATH, endpoint: Optional[str] = None):
        # endpoint: the Windows bridge's endpoint.json (BridgeClient resolves the
        # default: $CK3_BRIDGE_ENDPOINT, else Windows' own path, else AF_UNIX).
        self._sync = BridgeClient(socket_path, endpoint=endpoint)

    async def connect(self, timeout: float = 5.0) -> bool:
        return await asyncio.to_thread(self._sync.connect, timeout)

    async def close(self):
        await asyncio.to_thread(self._sync.close)

    async def send(self, msg: dict):
        await asyncio.to_thread(self._sync.send, msg)

    async def recv(self, timeout: float = 30.0) -> Optional[dict]:
        return await asyncio.to_thread(self._sync.recv, timeout)

    async def handshake(self, agent_version: str = "0.1.0",
                        capabilities: Optional[list] = None,
                        *,
                        protocol_version: Optional[int] = None,
                        client_name: Optional[str] = None,
                        mode: Optional[str] = None,
                        resume_token: Optional[str] = None,
                        world_identity: Optional[dict] = None,
                        episode_observer: Optional[bool] = None,
                        episode_seat: Optional[bool] = None,
                        timeout: float = 10.0) -> Optional[dict]:
        """Pass handshake v2 options through to the sync client (ck-6kvq.6).

        Called with no arguments this is byte-for-byte the legacy handshake, so
        the MCP server keeps its implicit control session until it asks for
        something else.

        THIS SIGNATURE IS A HAND-COPIED MIRROR OF THE SYNC ONE, AND IT DRIFTED
        (ck-4ae1). `episode_seat` was added to `BridgeClient.handshake` and not
        here, so `mcp_server`'s one handshake path — which splats
        `seat_declaration_kwargs()` — raised `unexpected keyword argument
        'episode_seat'` at startup. The agent had no game tools for a whole
        episode and the CLI reported ck3 as failed. `world_identity` and
        `episode_observer` were missing too; nobody had called them through
        this client yet, so the drift was three wide and only one of them
        was load-bearing.
        
        `test_async_mirrors_sync_handshake` now compares the two parameter
        sets, because a mirror nobody checks is not a mirror. Adding a keyword
        to the sync client and not here is the defect this class of bug is,
        and the next one will be caught by arithmetic rather than by an
        episode.
        """
        return await asyncio.to_thread(
            self._sync.handshake,
            agent_version,
            capabilities,
            protocol_version=protocol_version,
            client_name=client_name,
            mode=mode,
            resume_token=resume_token,
            world_identity=world_identity,
            episode_observer=episode_observer,
            episode_seat=episode_seat,
            timeout=timeout,
        )

    # --- session identity minted by the bridge (populated by handshake) ---
    @property
    def session_id(self) -> Optional[str]:
        return self._sync.session_id

    @property
    def session_mode(self) -> Optional[str]:
        return self._sync.session_mode

    @property
    def session_state(self) -> Optional[str]:
        return self._sync.session_state

    @property
    def handshake_ack(self) -> Optional[dict]:
        return self._sync.handshake_ack

    async def send_actions(self, tick_id: int, actions: list[dict]):
        await asyncio.to_thread(self._sync.send_actions, tick_id, actions)

    async def send_pause(self, paused: bool):
        await asyncio.to_thread(self._sync.send_pause, paused)

    async def send_speed(self, speed: int):
        await asyncio.to_thread(self._sync.send_speed, speed)
