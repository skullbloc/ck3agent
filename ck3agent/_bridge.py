"""Everything the CLI asks of the bridge. All server-module imports live here."""
from __future__ import annotations

import contextlib
import socket
import struct
import subprocess
import threading
import time
import uuid
from datetime import datetime
from pathlib import Path

from . import __version__, bootstrap_server
from . import _console as con

#: The bridge's own refusal line (bridge/src). Matched exactly: other lines contain "REFUSED".
REFUSAL = "REFUSED: unsupported CK3 build"


def _instance():
    bootstrap_server()
    import instance  # noqa: WPS433  (server module, bare import by design)
    return instance


def socket_path() -> str:
    return _instance().socket_path()


def bridge_log_path() -> str:
    return _instance().bridge_log()


def action_log_path() -> str:
    return _instance().telemetry_file("action_log.jsonl")


def alive(path: str, timeout: float = 1.0) -> bool:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path)
        return True
    except OSError:
        return False
    finally:
        s.close()


@contextlib.contextmanager
def session(sock: str, client: str, mode: str = "observe", timeout: float = 8.0,
            resume_token: str | None = None):
    """A handshaken BridgeClient, closed on exit. Raises OSError if unreachable."""
    bootstrap_server()
    from bridge_client import BridgeClient  # noqa
    bc = BridgeClient(sock)
    if not bc.connect(timeout=3.0):
        raise OSError(f"cannot connect to {sock}")
    try:
        ack = bc.handshake(agent_version=__version__, protocol_version=2,
                           client_name=client, mode=mode, resume_token=resume_token,
                           timeout=timeout)
        if not ack:
            raise OSError(f"no handshake ack within {timeout:.0f}s")
        yield bc
    finally:
        bc.close()


def call(bc, action: str, params: dict | None = None, timeout: float = 8.0) -> dict | None:
    """Send one action and return its reply frame (None on timeout)."""
    bootstrap_server()
    from pipeline_correlation import settle  # noqa
    rid = uuid.uuid4().hex
    bc.send({"actions": [{"action": action, "params": params or {}, "request_id": rid}]})
    try:
        return settle(send=bc.send, recv=bc.recv, action=action, request_id=rid,
                      timeout=timeout, allow_check=True)
    except Exception:  # noqa: BLE001  (TimeoutError, TicketUnsettled, ConnectionError)
        return None


def _tick(r) -> dict | None:
    if isinstance(r, dict) and isinstance(r.get("data"), dict):
        r = {**r["data"], **r}
    return r if isinstance(r, dict) else None


class Link:
    """One bridge session reused across calls, reopened after a failed call.

    A session per probe leaves one closed session per probe in the bridge's
    session accounting; a watcher polling every 15 s would leave hundreds.
    Thread-safe, because `ck3agent run` hands its link to the watcher thread.
    A call on a session that has gone stale is retried once on a fresh one.
    """

    def __init__(self, sock: str, client: str = "ck3agent-cli", mode: str = "observe"):
        self.sock, self.client, self.mode = sock, client, mode
        self._cm = self.bc = None
        self.error = ""
        self.opened = 0
        self._lock = threading.RLock()

    def call(self, action: str, params: dict | None = None, timeout: float = 8.0) -> dict | None:
        with self._lock:
            reused = self.bc is not None
            r = self._call(action, params, timeout)
            if r is None and reused:
                r = self._call(action, params, timeout)
            return r

    def _call(self, action: str, params: dict | None, timeout: float) -> dict | None:
        self.error = ""
        try:
            if self.bc is None:
                if not alive(self.sock):
                    raise OSError("socket not listening")
                cm = session(self.sock, self.client, mode=self.mode)
                self.bc, self._cm = cm.__enter__(), cm
                self.opened += 1
            r = call(self.bc, action, params, timeout)
        except Exception as e:  # noqa: BLE001
            self.error = str(e) or repr(e)
            r = None
        if r is None:
            self.close()
            self.error = self.error or "no reply"
        return r

    def close(self) -> None:
        with self._lock:
            cm, self._cm, self.bc = self._cm, None, None
            if cm is not None:
                with contextlib.suppress(Exception):
                    cm.__exit__(None, None, None)


def tick_info(sock: str, link: Link | None = None) -> dict | None:
    """get_tick_info over `link`, or a short observe session; None if the bridge is not up."""
    own = link is None
    link = link or Link(sock)
    try:
        return _tick(link.call("get_tick_info"))
    finally:
        if own:
            link.close()


def _date(t: dict | None) -> str | None:
    d = (t or {}).get("game_date")
    return d if d and d != "unknown" else None


def game_date(sock: str, link: Link | None = None) -> str | None:
    return _date(tick_info(sock, link))


def peer_pid(sock: str) -> int | None:
    """The pid on the far end of the bridge socket: the running game."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1.0)
    try:
        s.connect(sock)
        raw = s.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
        return struct.unpack("3i", raw)[0] or None
    except (OSError, AttributeError):
        return None
    finally:
        s.close()


def _sessions(r) -> list[dict]:
    bs = _tick(r) or {}
    return [row for row in bs.get("sessions") or [] if isinstance(row, dict)]


def _owner(link: Link, cid: int) -> str:
    for row in _sessions(link.call("get_bridge_state")):
        if cid in (row.get("claimed_character_ids") or []):
            return f"session {row.get('session_id')} ({row.get('session_state') or '?'}, " \
                   f"{row.get('client_name') or '?'})"
    return "an unknown session"


def _claims(link: Link) -> list[int]:
    """Every character any session holds, in session order."""
    out: list[int] = []
    for row in _sessions(link.call("get_bridge_state")):
        for c in row.get("claimed_character_ids") or []:
            if isinstance(c, int) and c not in out:
                out.append(c)
    return out


def held_rulers(sock: str, link: Link | None = None) -> list[int] | None:
    """Every ruler still bound in the game, or None when the bridge cannot be asked."""
    if not alive(sock):
        return None
    own = link is None
    link = link or Link(sock, mode="control")
    try:
        r = link.call("get_bridge_state")
        if r is None:
            return None
        return [c for row in _sessions(r) for c in row.get("claimed_character_ids") or []
                if isinstance(c, int)]
    except Exception:  # noqa: BLE001
        return None
    finally:
        if own:
            link.close()


def teardown(sock: str, wait: float = 15.0, link: Link | None = None) -> list[tuple[str, bool, str]]:
    """Pause, then confirm no ruler is still bound. Never raises.

    Only the session that claimed a ruler may release it (the bridge answers
    anyone else not_owned_by_session), so the release itself is the MCP
    server's, on its own shutdown. This waits up to `wait` seconds for it to
    land, then tries a release of its own and names the holder if that fails.
    "No ruler bound" means no session holds any ruler, not only that the
    bridge's primary is gone: a session may hold several (lane-multi).
    """
    if not alive(sock):
        return [("bridge", False, "socket not listening")]
    out: list[tuple[str, bool, str]] = []
    own = link is None
    link = link or Link(sock, mode="control")
    try:
        r = link.call("pause_game")
        out.append(("pause", bool(r) and r.get("success") is not False,
                    str((r or {}).get("reason") or "") if r else link.error))
        deadline = time.monotonic() + wait
        while True:
            t = _tick(link.call("get_tick_info"))
            if t is None or "controlled_character_id" not in t:
                out.append(("release", False, "no tick info; ruler unknown"))
                return out
            cid = int(t["controlled_character_id"] or 0)
            left = ([cid] if cid else []) + [c for c in _claims(link) if c != cid]
            if not left:
                out.append(("release", True, "no ruler bound"))
                return out
            if time.monotonic() >= deadline:
                break
            time.sleep(1.0)
        done, bad = [], []
        for c in left:
            r = link.call("release_character", {"character_id": c}) or {}
            if r.get("success") is True:
                done.append(str(c))
            else:
                bad.append(f"{c} still held by {_owner(link, c)}: "
                           f"{r.get('reason') or link.error or 'no reply'}")
        out.append(("release", False, "; ".join(bad)) if bad else ("release", True, ", ".join(done)))
    except Exception as e:  # noqa: BLE001
        out.append(("bridge", False, str(e) or repr(e)))
    finally:
        if own:
            link.close()
    return out


def release_session(sock: str, rec: dict, link: Link, wait: float = 15.0) -> dict:
    """Resume the MCP server's session by its token and release what it still holds.

    The server releases on its first signal, but Claude Code SIGKILLs it 500 ms
    after SIGINT, and a claim still in flight at its snapshot is missed. Once
    its connection is gone nothing of it is in flight and the session is
    resumable. An attached session is not (the bridge mints a fresh one
    instead), so this first waits up to `wait` seconds, on `link`, for the
    bridge to show it detached. Never raises.
    """
    sid, tok = rec.get("session_id"), rec.get("resume_token")
    out: dict = {"session_id": sid, "waited_s": 0.0, "resumed": False, "resume_status": None,
                 "claims": None, "released": [], "refused": {}, "error": None}
    if not (sid and tok):
        out["error"] = "session record has no session id or token"
        return out
    t0 = time.monotonic()
    while True:
        rows = _sessions(link.call("get_bridge_state"))
        if not rows and link.error:
            out["error"] = f"bridge: {link.error}"
            return out
        row = next((r for r in rows if r.get("session_id") == sid), None)
        if row is None:                    # the bridge dropped it, and its claims with it
            out["claims"], out["resume_status"] = [], "session_gone"
            return out
        if row.get("session_state") != "attached" or time.monotonic() - t0 >= wait:
            break
        time.sleep(0.25)
    out["waited_s"] = round(time.monotonic() - t0, 2)
    try:
        with session(sock, "ck3agent-run-release", mode="control", resume_token=tok) as bc:
            ack = bc.handshake_ack or {}
            out["resume_status"] = ack.get("resume_status")
            out["resumed"] = ack.get("resumed") is True and ack.get("session_id") == sid
            if not out["resumed"]:
                out["error"] = f"resume refused: {ack.get('resume_status') or 'no status'}"
                return out
            row = next((r for r in _sessions(call(bc, "get_bridge_state"))
                        if r.get("session_id") == sid), None)
            if row is None:
                out["error"] = "resumed session not in get_bridge_state"
                return out
            out["claims"] = [int(c) for c in row.get("claimed_character_ids") or []]
            for cid in out["claims"]:
                r = call(bc, "release_character", {"character_id": cid}) or {}
                if r.get("success") is True:
                    out["released"].append(cid)
                else:
                    out["refused"][str(cid)] = str(r.get("reason") or "no reply")
    except Exception as e:  # noqa: BLE001
        out["error"] = str(e) or repr(e)
    return out


def handshake_report(sock: str) -> bool:
    return handshake_check(sock)[0]


def handshake_check(sock: str) -> tuple[bool, str]:
    """Print the handshake lines; return (ok, session_state). The state matters:
    `dormant` means the bridge answers but the game is still loading or on the
    menu, and doctor used to print "Ready" over it (persona test 2026-10-18)."""
    bootstrap_server()
    from bridge_client import HandshakeRefused  # noqa
    try:
        with session(sock, "ck3agent-doctor") as bc:
            ack = bc.handshake_ack
    except HandshakeRefused as e:
        con.fail("bridge handshake refused", str(e))
        return False, ""
    except Exception as e:  # noqa: BLE001
        con.fail("bridge handshake", str(e) or repr(e))
        return False, ""
    seen = ack.get("observed_build_id") or ack.get("build_id") or "?"
    state = str(ack.get("session_state") or ack.get("status") or "?")
    con.ok("bridge handshake", f"session_state={state}, BuildID {str(seen)[:12]}")
    bv = ack.get("bridge_version")
    if bv and bv != __version__:
        con.fail("bridge version", f"bridge {bv}, ck3agent {__version__}: reinstall so they match")
        return False, state
    if bv:
        con.ok("bridge version", bv)
    return True, state


def last_launch_time(logs_dir: Path) -> float | None:
    """Newest `ck3agent launch`, from the ck3-YYYYmmdd-HHMMSS.log names."""
    best = None
    for p in logs_dir.glob("ck3-*.log"):
        try:
            t = datetime.strptime(p.stem[4:], "%Y%m%d-%H%M%S").timestamp()
        except ValueError:
            continue
        best = t if best is None or t > best else best
    return best


def find_refusal(log: Path, since: float | None) -> list[str] | None:
    """The bridge's refusal lines, if the log holds one written after `since`."""
    try:
        if since is not None and log.stat().st_mtime < since:
            return None
        tail = log.read_text(errors="replace").splitlines()[-400:]
    except OSError:
        return None
    hits = [i for i, line in enumerate(tail) if REFUSAL in line]
    if not hits:
        return None
    return [line.strip() for line in tail[hits[-1]:hits[-1] + 5]]


def wait_for_active(sock: str, deadline: float, proc: "subprocess.Popen", pause: bool = True) -> int:
    """After --continue: enter observe mode once the lobby exists (dismisses the prompt),
    then pause the game, because the bridge forces the first tick by unpausing it and
    nothing stops the clock afterwards (a player save lost ten game months before the
    first Claude turn, 2026-10-18). `pause=False` leaves it running.

    0 = game active, 1 = CK3 exited, 3 = timed out.
    """
    last = ""
    while time.time() < deadline:
        if proc.poll() is not None:
            con.fail("CK3 exited", f"code {proc.returncode}")
            return 1
        try:
            with session(sock, "ck3agent-launch", mode="control") as bc:
                r = call(bc, "get_tick_info", timeout=6.0) or {}
                if r.get("session_state") == "active" or bc.handshake_ack.get("session_state") == "active":
                    con.ok("game active", f"date {r.get('game_date')}")
                    if pause:
                        p = call(bc, "pause_game", timeout=6.0) or {}
                        if p.get("success") is False:
                            con.warn("game not paused", f"{p.get('reason') or 'refused'}; the clock is running")
                        else:
                            con.ok("game paused", "Claude unpauses it when it plays; --no-pause leaves it running")
                    return 0
                r = call(bc, "enter_observe_mode", timeout=10.0) or {}
                last = str(r.get("reason") or "")
        except Exception as e:  # noqa: BLE001
            last = str(e) or repr(e)
        time.sleep(5.0)
    con.warn("game not active yet", f"last bridge reason: {last or 'none'}. "
             "A player save needs a click on Continue in the game window.")
    return 3
