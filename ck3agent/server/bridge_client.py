"""
Bridge IPC client — connects to the CK3 bridge DLL via Unix domain socket.
Handles NDJSON framing, reconnection, and message dispatch.
"""

import collections
import hashlib
import json
import os
import socket
import time
import logging
from typing import Any, Optional, Callable

log = logging.getLogger(__name__)

# ck-qn03 (R12): the socket follows the INSTANCE (see instance.py for the
# user/legacy layouts); a named instance moves it under
# $CK3_INSTANCE_ROOT/<id>/. Resolved at import, which is what makes every
# derived client (harness, overlay poller, async client) follow automatically —
# pass `socket_path=` explicitly to talk to a different instance in-process.
try:                                   # normal: `ck3agent.server.bridge_client`
    from .instance import bridge_endpoint as _bridge_endpoint
    from .instance import socket_path as _resolve_socket_path
    from .instance import check_peer as _check_peer
    from .instance import check_socket_owner as _check_socket_owner
except ImportError:                    # bare: sys.path points at ck3agent/server
    from instance import bridge_endpoint as _bridge_endpoint
    from instance import socket_path as _resolve_socket_path
    from instance import check_peer as _check_peer
    from instance import check_socket_owner as _check_socket_owner

SOCKET_PATH = _resolve_socket_path()
RECV_BUF = 65536

# --- handshake v2 (ck-6kvq.6) ---------------------------------------------
# The bridge has parsed handshake v2 since M1 (bridge/src/ipc_server.cpp:210-284,
# SUPPORTED_PROTOCOL_MAJOR = 2 at :227) but no python client ever sent it, so
# every client got an implicit CONTROL session and observe mode was unreachable.
#
# Wire contract:
#   protocol_version  int   -- omitted => bridge assumes 1 (legacy)
#   client_name       str   -- read at any version; labels the session in logs
#   mode              str   -- "control" | "observe"; HONOURED ONLY AT >= 2
#   resume_token      str   -- SERVER-MINTED; present it to resume (ck-6kvq.5)
#   world_identity    obj   -- {episode_id, save_sha256, start_date_year}; the
#                              save the client VOUCHES FOR (ck-6kvq.5 / ck-9fc3)
#
# Legacy is preserved byte-for-byte: when no v2 argument is supplied the frame
# sent is exactly the pre-existing {type, agent_version, capabilities}. The live
# single-agent stack therefore keeps its implicit control session unchanged.
PROTOCOL_VERSION = 2          # newest major this client speaks
DEFAULT_CLIENT_NAME = "ck3agent"
MODE_CONTROL = "control"
MODE_OBSERVE = "observe"
VALID_MODES = (MODE_CONTROL, MODE_OBSERVE)


# --- the save/world identity (ck-6kvq.5 + ck-9fc3) -------------------------
# "Is this the same save?" is a fact about a FILE, and the bridge only ever sees
# a WORLD: GAME_STATE_ROOT moves on every load including a reload of the very
# same file, and every 1066 save carries start year 1066. So the bridge is TOLD,
# by the process that issued the load — and the identity it is told is NOT a new
# one. It is ck-6kvq.2's run_summary.json, read straight off the episode bundle's
# provenance:
#
#     episode_id        run_summary.episode_id
#     save_sha256       run_summary.save.sha256   (scripts/save_verify.py)
#     start_date_year   the YEAR of run_summary.save.bookmark_date
#
# Retyping any of these by hand would make it a third identity, which is exactly
# what docs/design/event-pipeline-conductor.md §6.2 forbids.


class BundleProvenanceError(RuntimeError):
    """The bundle cannot supply an identity the bridge would accept."""


def world_identity_from_bundle(path: "str | os.PathLike") -> dict:
    """Read WorldIdentity{episode_id, save_sha256, start_date_year} from a bundle.

    `path` is either an episode directory (its `run_summary.json` is used) or the
    run_summary.json itself. Returns the dict the handshake frame and
    `declare_save_identity` both take.

    Raises BundleProvenanceError rather than returning a partial identity: a
    declaration missing `save_sha256` vouches for nothing, and the bridge will
    (correctly) refuse it — failing here says WHY, at the point the operator can
    still fix it.
    """
    p = os.fspath(path)
    if os.path.isdir(p):
        p = os.path.join(p, "run_summary.json")
    try:
        with open(p, "r", encoding="utf-8") as fh:
            summary = json.load(fh)
    except OSError as exc:
        raise BundleProvenanceError(f"cannot read bundle provenance at {p}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise BundleProvenanceError(f"{p} is not valid JSON: {exc}") from exc

    return world_identity_from_run_summary(summary, source=p)


def world_identity_from_run_summary(summary: dict, source: str = "run_summary") -> dict:
    """The same, from an already-loaded run_summary document.

    An episode that has a bundle OPEN but not yet sealed holds the document in
    memory and has no file to read — and it is exactly that episode which needs
    to declare, at start, before any control session attaches.
    """
    save = summary.get("save") or {}
    sha = save.get("sha256")
    if not sha:
        raise BundleProvenanceError(
            f"{source} carries no save.sha256 — nothing to vouch for. Seal the bundle "
            "with scripts/episode_bundle.py, which computes it from the file's bytes."
        )
    ident: dict[str, Any] = {
        "episode_id": summary.get("episode_id") or "",
        "save_sha256": sha,
    }
    # start_date_year is CORROBORATION, never proof: the bridge cross-checks it
    # against the loaded world's START_DATE_YEAR and refuses a match when the two
    # disagree. Omitted when the bundle recorded no bookmark date (facts_source
    # "sha256_only"), in which case the refutation clause simply abstains.
    bookmark = save.get("bookmark_date")
    if isinstance(bookmark, str) and bookmark[:1].isdigit():
        try:
            ident["start_date_year"] = int(bookmark.split(".")[0])
        except ValueError:
            pass
    return ident


def sha256_of_save(path: "str | os.PathLike") -> str:
    """The save key, computed the way scripts/save_verify.py computes it.

    For the case where a caller has the .ck3 but not a sealed bundle. Prefer
    world_identity_from_bundle(): a hash with no episode_id beside it cannot be
    cross-checked against the record the episode is being judged on.
    """
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class HandshakeRefused(RuntimeError):
    """The bridge answered the handshake with a non-ok status.

    Raised (rather than returned) so a refusal is typed and immediate instead of
    a silent implicit-control session or a caller blocking on a session that was
    never minted. Today the only refusal the bridge emits is
    status="unsupported_protocol" for a protocol major above its own.
    """

    def __init__(self, status: str, reason: str = "", ack: Optional[dict] = None):
        self.status = status
        self.reason = reason
        self.ack = ack or {}
        super().__init__(
            f"bridge refused handshake: status={status!r} reason={reason!r}"
        )


#: ck-j6lk. THE SOLE SEAT DECLARATION IN THE TREE, as client kwargs.
#:
#: A FUNCTION RATHER THAN A LITERAL AT EACH CALLER, for ck-zi99's reason and
#: not merely for tidiness: "exactly one declaration site" and "the site is on
#: the path an episode takes" are DIFFERENT PROPERTIES, and the observer
#: declaration satisfied only the first — a census counted one literal while
#: the arm path reached none of them. UNIQUENESS IS NOT REACHABILITY. A caller
#: that must declare calls this; a census counts this.
#:
#: WHO CALLS IT: the MCP server's bridge session, which is the session an arm
#: seat actually runs on — ck-zgh4 established from ep-02 that the arms' agents
#: are Claude Code sessions calling MCP tools, and nothing in the MCP server
#: imports agent.py. `agent.py` (the A path) deliberately does NOT declare: the
#: registered runs never execute it, and giving it a v2-only key would change
#: its legacy handshake frame for no arm that exists. That claim is pinned by a
#: row rather than left as a comment.
def seat_declaration_kwargs() -> dict:
    """The handshake kwargs that declare this session intends to hold a seat."""
    return {"episode_seat": True}


#: ck-1qj0. FRAME-SPLIT RECOVERY, counted process-wide so the driver can SEAL it.
#:
#: A line that does not decode used to raise JSONDecodeError out of `recv()`,
#: through `call()`, to the driver's catch-all: registered-d44 slot 1 sealed
#: `abort/unknown` at game date 1073.10.05 after 51 minutes, on
#: "Extra data: line 1 column 133063 (char 133062)" — one line, 133,062 bytes,
#: no newline inside, a second document after it.
#:
#: A WARNING NOBODY READS IS NOT A WITNESS (ck-pro5), so these are counted here
#: rather than only logged, and the driver seals them into the bundle's notes.
FRAMING_RECOVERY: dict = {
    "split_lines": 0,          # lines that needed more than one decode
    "documents_recovered": 0,  # complete documents delivered out of those lines
    "bytes_dropped": 0,        # the undecodable TAIL only, never a whole document
    "samples": [],             # the first few, with length + head/tail bytes
}
#: Enough to diagnose, bounded so a pathological stream cannot grow the summary.
FRAMING_SAMPLE_CAP = 5
FRAMING_SAMPLE_EDGE = 200


def split_frame_line(line: bytes) -> list:
    """Every COMPLETE json document on one framing line, in order.

    SPLIT, DO NOT SKIP. "Extra data" means the FIRST document is complete and
    parseable, so skipping the line would silently drop a reply the driver or a
    seat is waiting on — and a caller timing out on a reply that was actually
    delivered is a worse failure than the crash this replaces. Only a trailing
    remainder that will not decode is dropped, and its size is counted.

    The common case is unchanged and costs one `json.loads`: a well-formed line
    returns a single-element list and touches none of the counters.
    """
    text = line.decode("utf-8", "replace")
    try:
        return [json.loads(text)]
    except json.JSONDecodeError:
        pass

    decoder = json.JSONDecoder()
    out: list = []
    idx, n = 0, len(text)
    while idx < n:
        while idx < n and text[idx] in " \t\r\n":
            idx += 1
        if idx >= n:
            break
        try:
            obj, end = decoder.raw_decode(text, idx)
        except json.JSONDecodeError:
            break          # the TAIL is undecodable; everything before it stands
        out.append(obj)
        idx = end

    rest = text[idx:]
    dropped = len(rest.encode("utf-8", "replace")) if rest.strip() else 0
    FRAMING_RECOVERY["split_lines"] += 1
    FRAMING_RECOVERY["documents_recovered"] += len(out)
    FRAMING_RECOVERY["bytes_dropped"] += dropped
    if len(FRAMING_RECOVERY["samples"]) < FRAMING_SAMPLE_CAP:
        FRAMING_RECOVERY["samples"].append({
            "length_bytes": len(line),
            "documents": len(out),
            "bytes_dropped": dropped,
            "head": text[:FRAMING_SAMPLE_EDGE],
            "tail": text[-FRAMING_SAMPLE_EDGE:],
        })
    log.warning(
        "bridge frame line did not decode as one document: %d bytes, "
        "%d document(s) recovered, %d byte(s) dropped (ck-1qj0)",
        len(line), len(out), dropped)
    return out


class BridgeClient:
    """Manages the Unix socket connection to the in-process bridge DLL."""

    def __init__(self, socket_path: str = SOCKET_PATH, endpoint: Optional[str] = None):
        self.socket_path = socket_path
        #: ADR 0001 (Windows port): the path of the bridge's endpoint.json. Set,
        #: the client connects over TCP loopback and presents the token in it
        #: (the Windows bridge has no AF_UNIX socket and no peer-uid check).
        #: None (the default) takes $CK3_BRIDGE_ENDPOINT, else on Windows the
        #: bridge's own endpoint.json (instance.windows_endpoint_path); with
        #: neither, the AF_UNIX path above, unchanged.
        self.endpoint = endpoint if endpoint is not None else _bridge_endpoint()
        self.sock: Optional[socket.socket] = None
        self._buf = b""
        #: ck-1qj0. Documents already decoded off a framing line and not yet
        #: returned. A split line yields more than one, and `recv()` promises
        #: exactly one per call, so the remainder waits here IN ORDER rather
        #: than being dropped.
        self._pending: collections.deque = collections.deque()
        self._last_build_id: Optional[str] = None
        # Handshake v2 results, populated by handshake(). session_id is minted
        # by the SERVER — nothing the client sends is identity.
        self.handshake_ack: Optional[dict] = None
        self.session_id: Optional[str] = None
        self.session_mode: Optional[str] = None
        self.session_state: Optional[str] = None
        self.protocol_version_sent: Optional[int] = None
        # ck-6kvq.5 resume state. `resume_token` is SERVER-MINTED and captured
        # from the ack; holding it is the whole of what makes a reconnect a
        # RESUME rather than a fresh session that silently lost its claims.
        self.resume_token: Optional[str] = None
        self.resumed: bool = False
        self.resume_status: Optional[str] = None
        self.resume_identity_reason: Optional[str] = None
        # The identity this client vouches for, once set (see set_world_identity).
        self.world_identity: Optional[dict] = None

    def connect(self, timeout: float = 5.0) -> bool:
        """Connect to the bridge socket. Returns True on success.

        Outside the legacy layout, refuses a socket this uid does not own and a
        peer with another uid (rc2 B1).
        """
        if self.endpoint is not None:
            return self._connect_tcp(timeout)
        s = None
        try:
            _check_socket_owner(self.socket_path)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect(self.socket_path)
            _check_peer(s)
            self.sock = s
            log.info("Connected to bridge at %s", self.socket_path)
            return True
        except (socket.error, OSError) as e:
            log.error("Failed to connect to bridge: %s", e)
            if s is not None:
                s.close()
            self.sock = None
            return False

    def _connect_tcp(self, timeout: float) -> bool:
        """The Windows bridge's transport: TCP loopback, admitted by the token in
        endpoint.json ({"transport","endpoint","token","pid"}). The first line
        sent is {"auth": token}; the bridge answers {"type": "auth_ok"} or closes.
        A non-loopback address is refused here, whatever the file says."""
        s = None
        try:
            with open(self.endpoint, encoding="utf-8") as f:
                ep = json.load(f)
            if ep.get("transport") != "tcp_loopback":
                raise OSError(f"endpoint transport {ep.get('transport')!r} is not tcp_loopback")
            host, port = str(ep["endpoint"]).rsplit(":", 1)
            if host != "127.0.0.1":
                raise OSError(f"refusing a non-loopback endpoint: {ep['endpoint']}")
            s = socket.create_connection((host, int(port)), timeout=timeout)
            self.sock = s
            self.send({"auth": ep["token"]})
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                msg = self.recv(timeout=max(0.1, deadline - time.monotonic()))
                if msg is None:
                    break
                if msg.get("type") == "auth_ok":
                    log.info("Connected to bridge at %s (tcp_loopback, pid %s)", ep["endpoint"], ep.get("pid"))
                    return True
                if msg.get("type") in ("auth_refused", "error"):
                    raise OSError(f"bridge refused the token: {msg}")
            raise OSError("no auth_ok from the bridge within the timeout")
        except (socket.error, OSError, ValueError, KeyError) as e:
            log.error("Failed to connect to bridge endpoint %s: %s", self.endpoint, e)
            if s is not None:
                s.close()
            self.sock = None
            return False

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def send(self, msg: dict):
        """Send a JSON message (newline-delimited)."""
        if not self.sock:
            raise ConnectionError("Not connected")
        data = json.dumps(msg, separators=(",", ":")) + "\n"
        self.sock.sendall(data.encode("utf-8"))

    def recv(self, timeout: float = 30.0) -> Optional[dict]:
        """Receive a single newline-delimited JSON message."""
        if not self.sock:
            raise ConnectionError("Not connected")

        # ck-3esm: `timeout` is a DEADLINE for this call, not a budget handed
        # afresh to each syscall. It used to be `settimeout(timeout)` ONCE above
        # the loop, which is a different and much weaker promise: every
        # `sock.recv` below got the FULL timeout again, and the two `continue`s
        # here (keepalive ping, empty line) go round WITHOUT RETURNING, so the
        # caller never regains control to check its own deadline either.
        #
        # THE BRIDGE PINGS EVERY OPEN SLOT ROUGHLY EVERY 5 SECONDS (accept loop,
        # 25 idle polls x 200 ms) and `HarnessClient.call` asks for
        # `min(remaining, 5.0)`. So a ping landing just inside each 5 s window
        # resets the wait forever: recv() never returns, call() never re-checks
        # its absolute deadline, nothing blocks and nothing raises, and the
        # caller waits indefinitely on a call it believes is bounded by 15 s.
        # MEASURED CONSEQUENCE (ck-3esm, arm-C smoke ep-20260921T041851Z): the
        # driver hung on its 10th `query_war_stance`, mid-way through the FIRST
        # sampling pass, and sat there ~4 minutes until the operator killed it —
        # so the poll loop ran exactly ONE iteration, `last_date` never advanced
        # past the date read at seq 2, and the episode's horizon could never
        # fire. A four-game-year horizon became an unbounded spend, and the
        # whole causal chain is this one `settimeout` being outside the loop.
        #
        # Skipping a ping is still correct. What was wrong is that it cost
        # NOTHING: the loop consumed it and re-armed the full timeout.
        deadline = time.monotonic() + timeout

        # ck-1qj0: LAZY, because `recv()`'s working contract is "set `sock` and
        # `_buf` and it works" — ck3agent/server/test_bridge_client_recv_deadline
        # and test_handshake_observer_kwarg both build the client with
        # `BridgeClient.__new__(BridgeClient)` and supply exactly those two
        # attributes. Requiring a third from `__init__` broke seven of their rows,
        # and those rows guard this very loop's deadline behaviour (ck-3esm), so
        # the attribute bends and the tests do not.
        pending = getattr(self, "_pending", None)
        if pending is None:
            pending = self._pending = collections.deque()

        while True:
            # ck-1qj0: anything already decoded off a split line is delivered
            # FIRST and in order, before another byte is read. Draining costs no
            # deadline, because the deadline is only consulted on a socket read.
            if pending:
                msg = pending.popleft()
                # Silently skip keepalive pings from bridge
                if isinstance(msg, dict) and msg.get("type") == "ping":
                    continue
                return msg

            # Check buffer for complete message
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line = self._buf[:nl]
                self._buf = self._buf[nl + 1:]
                if line:
                    # ck-1qj0: ONE document is the overwhelming case and is
                    # unchanged. Two on a line are both delivered; a garbage
                    # line yields none and costs a reply, never the episode.
                    pending.extend(split_frame_line(line))
                continue

            # Read more data, bounded by what is LEFT of the deadline.
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(RECV_BUF)
            except socket.timeout:
                return None
            if not chunk:
                raise ConnectionError("Bridge disconnected")
            self._buf += chunk

    def handshake(self, agent_version: str = "0.1.0",
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
        """Send the handshake and wait for the ack.

        Legacy (no keyword given): the frame is exactly what it has always been —
        {type, agent_version, capabilities} — and the bridge mints an implicit
        CONTROL session. Unchanged on the wire.

        v2: pass any of protocol_version / client_name / mode / resume_token and
        the frame gains all four v2 fields (protocol_version defaults to 2,
        because the bridge honours `mode` ONLY at protocol_version >= 2 —
        requesting observe at v1 silently yields control).

        Returns the ack dict (or None if none arrived before `timeout`). Raises
        HandshakeRefused if the bridge answered with a non-ok status, and
        ValueError for a mode this client does not know.
        """
        if capabilities is None:
            capabilities = ["observe", "act", "pause"]

        if mode is not None:
            mode = str(mode).strip().lower()
            if mode not in VALID_MODES:
                raise ValueError(
                    f"unknown handshake mode {mode!r}; expected one of {VALID_MODES}"
                )

        msg = {
            "type": "handshake",
            "agent_version": agent_version,
            "capabilities": capabilities,
        }
        if world_identity is None:
            world_identity = self.world_identity
        wants_v2 = any(v is not None for v in
                       (protocol_version, client_name, mode, resume_token,
                        world_identity, episode_observer))
        if wants_v2:
            pv = PROTOCOL_VERSION if protocol_version is None else int(protocol_version)
            msg["protocol_version"] = pv
            msg["client_name"] = client_name or DEFAULT_CLIENT_NAME
            msg["mode"] = mode or MODE_CONTROL
            if resume_token:
                msg["resume_token"] = resume_token
            # ck-43u3 / ck-h6nx (D'). THE EPISODE-OBSERVER DECLARATION, which
            # this client could not send. `bridge/harness/client.py` has built
            # `hs_kwargs["episode_observer"]` since ck-zi99 and handed it to
            # THIS function, which did not accept it — so every attempt died on
            # `handshake() got an unexpected keyword argument`. Two client
            # implementations, one wired: the privilege was reachable from
            # HarnessClient and not from here.
            #
            # The bridge has honoured it all along: ipc_server.cpp reads
            # `episode_observer` as a BOOL out of the handshake frame and calls
            # `pipeline_declare_episode_observer`, which is the only caller of
            # `set_privileged_reader` — and that is what makes
            # `FlushTrigger::ReadLaneEarly` fire, serving the declared
            # observer's READ lane on its own submission instead of at the
            # cadence. A fully implemented server-side mechanism, unreachable
            # from the client the arm episode actually uses.
            #
            # Sent as a bool because the bridge tests `IsBool()`; omitted
            # entirely when None, so the frame is byte-identical for every
            # caller that does not declare.
            if episode_observer is not None:
                msg["episode_observer"] = bool(episode_observer)
            # ck-j6lk. THE SEAT DECLARATION — "this session intends to hold a
            # character", sent by an ARM AGENT and by nothing else.
            #
            # WHY IT EXISTS. In experiment mode the bridge pins every detached
            # CONTROL session, claim or no claim, so the driver's own
            # `steer-probe` and `speed-pin` helpers each accrued a full horizon
            # of unavailability and invalidated a run whose agent session was
            # perfectly healthy (deploy-31 shakedown 2: 3 sessions counted, 1
            # declared, 2 over threshold, 364 days each). Those helpers never
            # claim anything — the probe issues NO verb at all, the pin issues
            # only `set_speed` and `get_tick_info`.
            #
            # OPT-IN, AND THAT IS THE MECHANISM. HarnessClient delegates to THIS
            # builder, so the helpers and the agent share one handshake; a key
            # emitted only when the caller passes it means the helpers cannot
            # send it by accident and no exclusion list has to be maintained
            # anywhere. Omitted entirely when None, so every existing caller's
            # frame stays byte-identical.
            #
            # It does NOT replace ck-j6lk's bridge-side rule. The bridge keys
            # the pin on CLAIMS, which is what R3 is about; this declares
            # INTENT, which is what the validator cross-checks against the
            # roster. Two different questions, deliberately not merged.
            if episode_seat is not None:
                msg["episode_seat"] = bool(episode_seat)
            # ck-6kvq.5: vouch for the save on the frame that authorises the claim
            # set. The bridge routes this through ck-9fc3's world_identity_declare()
            # — the SAME entry point the standalone action takes — so a resume that
            # carries it gets its quarantined steering back in the same round trip
            # instead of steering nothing until a second call lands.
            if world_identity:
                msg["world_identity"] = dict(world_identity)
        self.protocol_version_sent = msg.get("protocol_version")
        requested_mode = msg.get("mode", MODE_CONTROL)

        self.handshake_ack = None
        self.session_id = None
        self.session_mode = None
        self.session_state = None
        self.resumed = False
        self.resume_status = None
        self.resume_identity_reason = None

        self.send(msg)

        # Wait for handshake_ack
        resp = self.recv(timeout=timeout)
        if resp and resp.get("type") == "handshake_ack":
            log.info("Handshake OK: %s", json.dumps(resp, indent=2))
            self.handshake_ack = resp
            bid = resp.get("binary_build_id")
            if bid and self._last_build_id and bid != self._last_build_id:
                log.warning(
                    "BUILD_ID changed between connections! "
                    "Previous: %s, Current: %s. "
                    "Bridge binary may have been reloaded.",
                    self._last_build_id, bid,
                )
            if bid:
                self._last_build_id = bid

            status = resp.get("status") or "ok"
            if status != "ok":
                reason = resp.get("reason") or ""
                log.error("Bridge refused handshake: status=%s reason=%s",
                          status, reason)
                # The bridge releases the slot right after a refusal ack
                # (ipc_server.cpp) — drop our end so nothing tries to use a
                # socket the server has already closed.
                self.close()
                raise HandshakeRefused(status, reason, resp)

            self.session_id = resp.get("session_id") or None
            self.session_state = resp.get("session_state") or None
            # ck-6kvq.5. Capture the SERVER-MINTED token: without it this client
            # can never come back to its own claims after a drop.
            self.resume_token = resp.get("resume_token") or None
            # The bridge's HONEST ECHO of what it did with a token we presented.
            # A client that asked to resume, silently got a fresh session, and
            # never checked cannot tell that its earl is now steered by nobody —
            # and in experiment mode that is the difference between a valid
            # episode and a quietly invalid one (§1.4's session_mode lesson).
            self.resumed = bool(resp.get("resumed"))
            self.resume_status = resp.get("resume_status") or None
            self.resume_identity_reason = resp.get("resume_identity_reason") or None
            if resume_token and not self.resumed:
                log.warning(
                    "RESUME REFUSED: presented a token, got a FRESH session "
                    "(status=%s identity=%s). Any claims that session held are gone; "
                    "re-claim before acting.",
                    self.resume_status, self.resume_identity_reason)
            # `session_mode` is the bridge's echo of the mode it actually minted.
            # A bridge that predates the echo omits it; fall back to what we
            # requested (which for a legacy frame is "control", the implicit
            # session the bridge creates for protocol_version < 2).
            self.session_mode = (resp.get("session_mode")
                                 or resp.get("mode")
                                 or requested_mode)
            return resp
        if resp is None:
            log.warning("No handshake_ack within %.1fs", timeout)
        return resp  # May be None or unexpected type

    # ------------------------------------------------------------------
    # ck-6kvq.5 / ck-9fc3 — the save identity
    # ------------------------------------------------------------------

    def set_world_identity(self, identity: dict) -> None:
        """Remember the identity to vouch for on every (re)handshake.

        Set this ONCE from the bundle's provenance
        (`world_identity_from_bundle`) and every reconnect this client makes —
        including the one after a dropped socket — carries it automatically. That
        is what makes a resume restore steering in the same round trip rather
        than steering nothing until a separate declaration lands.
        """
        if not identity or not identity.get("save_sha256"):
            raise BundleProvenanceError(
                "a world identity without save_sha256 vouches for nothing; "
                "the bridge will refuse it (world_identity.h)"
            )
        self.world_identity = dict(identity)

    def declare_save_identity(self, identity: Optional[dict] = None,
                              timeout: float = 15.0) -> Optional[dict]:
        """Tell the bridge WHICH SAVE is loaded (ck-9fc3's entry point).

        Call this after a reload, and after any resume that did not carry the
        identity on its handshake frame. On the same save the bridge restores the
        steering it quarantined at the reload; on a different save it refuses,
        and it also erases any session whose claims were taken in the world the
        declaration refutes (ck-6kvq.5's claims half).

        Returns the bridge's reply dict, whose `decision` is a closed vocabulary
        (see bridge/include/world_identity.h) — a caller SHOULD branch on it
        rather than assume success: `restored_from_quarantine` is the resume
        path working, `refused_different_save` is the guard doing its job, and
        `pending_no_declaration` means nothing has been vouched for yet.
        """
        ident = dict(identity or self.world_identity or {})
        if not ident.get("save_sha256"):
            raise BundleProvenanceError(
                "declare_save_identity needs a save_sha256 — read it off the "
                "bundle with world_identity_from_bundle()"
            )
        request_id = f"declare-{int(time.time() * 1000)}"
        self.send({
            "type": "action",
            "tick_id": 0,
            "actions": [{
                "action": "declare_save_identity",
                "params": ident,
                "request_id": request_id,
            }],
        })
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.recv(timeout=min(5.0, max(0.1, deadline - time.monotonic())))
            if not isinstance(msg, dict):
                continue
            if msg.get("request_id") == request_id:
                self.world_identity = ident
                return msg
        log.warning("declare_save_identity: no reply within %.1fs", timeout)
        return None

    def send_actions(self, tick_id: int, actions: list[dict]):
        """Send action response for a tick."""
        self.send({
            "type": "action",
            "tick_id": tick_id,
            "actions": actions,
        })

    def send_pause(self, paused: bool):
        """Request pause/unpause via action envelope."""
        action = "pause_game" if paused else "unpause_game"
        self.send_actions(0, [{"action": action}])

    def send_speed(self, speed: int):
        """Set game speed (1-5) via action envelope."""
        self.send_actions(0, [{"action": "set_speed", "speed": max(1, min(5, speed))}])
