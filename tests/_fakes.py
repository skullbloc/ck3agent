"""A fake NDJSON bridge on a Unix socket, with the real bridge's ownership rule.

Each connection is its own session. A release from any session but the owner
gets `not_owned_by_session` (bridge/src/dispatch_router.cpp). A closed session
keeps its claims (`pin_claims`), the worst case the real bridge has.

Resume follows session.cpp `resume_by_token`: a detached session's token
rebinds it to the new connection (`resumed: true`); a token whose session is
still attached is refused and a FRESH session is minted (`resumed: false`,
`resume_status: session_attached`), as the real bridge does.

`delays` maps (client_name, action) to seconds slept before replying, outside
the lock: a slow bridge for one client. `snapshot_lag` maps a client_name to
how many of its first get_bridge_state replies leave its claims out: a claim
still in flight when that client takes its snapshot.

`drop_after_claim` maps a client_name to "pin" or "destroy": the bridge closes
that client's connection right after answering its FIRST set_controlled_character.
"pin" leaves the session detached and resumable with its claims (experiment
mode); "destroy" erases the session, its token and its claims, as the real
bridge does outside experiment mode (ipc_server.cpp, SessionRegistry::destroy).
"""
from __future__ import annotations

import itertools
import json
import socket
import threading
import time


class FakeBridge(threading.Thread):
    def __init__(self, path: str, ack: dict | None = None, replies: dict | None = None,
                 pin_claims: bool = True, delays: dict | None = None,
                 snapshot_lag: dict | None = None, drop_after_claim: dict | None = None):
        super().__init__(daemon=True)
        self.ack, self.replies, self.pin_claims = ack or {"session_state": "active"}, replies or {}, pin_claims
        self.delays, self.snapshot_lag = delays or {}, dict(snapshot_lag or {})
        self.drop_after_claim = dict(drop_after_claim or {})
        self.dropped: list = []       # (session_id, how)
        self.seen: list = []          # (action, params)
        self.log: list = []           # (session_id, action, params)
        self.owner: dict[int, str] = {}
        self.sessions: dict[str, dict] = {}
        self.resumes: list = []       # (token, resumed, resume_status)
        self.game_date, self.paused = "900.1.1", False
        self._ids = itertools.count(1)
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(16)

    def handshakes(self) -> int:
        return sum(1 for a, _ in self.seen if a == "handshake")

    def run(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def _handshake(self, msg) -> tuple[str, dict]:
        tok = msg.get("resume_token")
        extra: dict = {}
        with self.lock:
            self.seen.append(("handshake", msg.get("mode")))
            sid = next((s for s, v in self.sessions.items() if tok and v.get("token") == tok), None)
            if sid and self.sessions[sid]["state"] != "attached":
                self.sessions[sid]["state"] = "attached"
                extra = {"resumed": True, "resume_status": "ok"}
            else:
                if tok:
                    extra = {"resumed": False,
                             "resume_status": "session_attached" if sid else "unknown_token"}
                sid = f"s{next(self._ids)}"
                self.sessions[sid] = {"state": "attached", "client_name": msg.get("client_name"),
                                      "mode": msg.get("mode"), "token": f"tok-{sid}"}
            if tok:
                self.resumes.append((tok, extra["resumed"], extra["resume_status"]))
        return sid, {"type": "handshake_ack", "status": "ok", "session_id": sid,
                     "resume_token": self.sessions[sid]["token"], **self.ack, **extra}

    def serve(self, conn):
        sid = client = None
        f = conn.makefile("rwb")
        try:
            for line in f:
                msg = json.loads(line)
                if msg.get("type") == "handshake":
                    client = msg.get("client_name")
                    sid, ack = self._handshake(msg)
                    out = [ack]
                else:
                    out = []
                    for a in msg.get("actions", []):
                        time.sleep(self.delays.get((client, a["action"]), 0))
                        with self.lock:
                            self.seen.append((a["action"], a.get("params")))
                            self.log.append((sid, a["action"], a.get("params")))
                            r = self.reply(sid, client, a)
                        out.append({"type": "action_result", "request_id": a.get("request_id"),
                                    "action": a["action"], **r})
                for o in out:
                    f.write((json.dumps(o) + "\n").encode())
                f.flush()
                how = self.drop_after_claim.get(client)
                if how and any(o.get("action") == "set_controlled_character" for o in out):
                    with self.lock:
                        del self.drop_after_claim[client]
                        self.dropped.append((sid, how))
                        if how == "destroy":
                            self.owner = {c: s for c, s in self.owner.items() if s != sid}
                            del self.sessions[sid]
                            sid = None
                    conn.shutdown(socket.SHUT_RDWR)
                    break
        except (OSError, ValueError):
            pass
        finally:
            with self.lock:
                if sid:
                    self.sessions[sid]["state"] = "detached"
                    if not self.pin_claims:
                        self.owner = {c: s for c, s in self.owner.items() if s != sid}

    def reply(self, sid, client, a) -> dict:
        act, p = a["action"], a.get("params") or {}
        if act in self.replies:
            r = self.replies[act]
            return r(a) if callable(r) else r
        if act == "set_controlled_character":
            self.owner[int(p["character_id"])] = sid
            return {"success": True}
        if act == "release_character":
            cid = int(p.get("character_id") or 0)
            if self.owner.get(cid) != sid:
                return {"success": False, "reason": "not_owned_by_session", "character_id": cid}
            del self.owner[cid]
            return {"success": True}
        if act == "get_tick_info":
            return {"controlled_character_id": min(self.owner) if self.owner else 0,
                    "game_date": self.game_date, "paused": self.paused}
        if act == "get_bridge_state":
            blind = self.snapshot_lag.get(client, 0) > 0
            if blind:
                self.snapshot_lag[client] -= 1
            return {"sessions": [{"session_id": s, "session_state": v["state"],
                                  "client_name": v["client_name"],
                                  "claimed_character_ids": [] if blind and s == sid else
                                  sorted(c for c, o in self.owner.items() if o == s)}
                                 for s, v in self.sessions.items()]}
        if act == "pause_game":
            self.paused = True
        return {"success": True}

    def close(self):
        self.srv.close()
