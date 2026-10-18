#!/usr/bin/env python3
"""ck-jh76 — what does a MISSING TRAILING NEWLINE from the bridge actually do?

Host-only, no game, no bridge. Exit 0 pass / 1 FAIL / 3 NOT EVALUABLE.

WHY THIS EXISTS. send_to_slot writes the JSON body, then sends the trailing
newline SEPARATELY, with its own poll() and its own send() — either of which can
fail after the body is already in the client's socket. That is one of five
silent failure exits, and the question ASK 2 asks is whether it can produce
arma7's symptom: a request received, served promptly, the server healthy, 77
further actions served, and a 15s BridgeTimeout on one reply.

This does NOT claim it is what happened in arma7. It establishes what WOULD
happen, by driving the REAL driver receive loop (ck3agent/server/bridge_client.py
BridgeClient.recv) over a real socketpair — not a model of it, not a reimplemen-
tation. A mechanism that cannot produce the symptom can be struck off; one that
can, stays on the list with a named log signature to search for.

THE SECOND HALF IS THE EVIDENCE AGAINST THE HYPOTHESIS, and it is the reason to
run this rather than argue it. If a missing newline desynchronises the stream,
the NEXT reply should concatenate onto the orphaned frame. ck-zgh4 observed the
run continuing to serve 77 actions, which a corrupted stream should have
disrupted. So: does the cascade happen, and if so is it LOUD or SILENT? A silent
mis-parse would be consistent with a run that limps on; a raised exception would
not, and its absence in arma7's driver log would kill the hypothesis outright.
"""
import json
import os
import socket
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "ck3agent"))
try:
    from server.bridge_client import BridgeClient
except Exception as exc:                       # pragma: no cover
    print(f"NOT EVALUABLE: cannot import the real BridgeClient: {exc}")
    sys.exit(3)

fails = []


def row(name, ok, detail):
    if not ok:
        fails.append(name)
    print(f"{name:<44} {'PASS' if ok else 'FAIL'}  {detail}")


def client_over(pair_end):
    c = BridgeClient.__new__(BridgeClient)   # no connect(); we supply the socket
    c.socket_path = "<socketpair>"
    c.sock = pair_end
    c._buf = b""
    c._last_build_id = None
    c.handshake_ack = None
    return c


def main():
    body = json.dumps({"request_id": "6a8f5cd6", "ok": True, "tick": 1450}).encode()

    # ── 1. A COMPLETE frame is received, so the harness itself is sound. If this
    #      fails, nothing below means anything.
    srv, cli = socket.socketpair()
    c = client_over(cli)
    srv.sendall(body + b"\n")
    got = c.recv(timeout=2.0)
    row("complete_frame_is_received",
        isinstance(got, dict) and got.get("request_id") == "6a8f5cd6",
        "control: body + newline parses")
    srv.close(); cli.close()

    # ── 2. THE MECHANISM. Body written, newline NOT — exactly what a
    #      kNewlineFailed exit leaves in the client's socket.
    srv, cli = socket.socketpair()
    c = client_over(cli)
    srv.sendall(body)                      # no b"\n"
    t0 = time.monotonic()
    got = c.recv(timeout=1.0)
    waited = time.monotonic() - t0
    row("unterminated_frame_times_out",
        got is None,
        f"recv returned {got!r} after {waited:.2f}s — the deadline, not an error")
    row("timeout_consumes_the_full_deadline",
        0.9 <= waited <= 1.6,
        f"waited {waited:.2f}s of a 1.0s deadline (arma7: 15s of 15s)")
    row("body_is_held_not_lost",
        c._buf == body,
        f"{len(c._buf)} bytes sit in _buf, complete and unyielded")

    # ── 3. THE CASCADE, and whether it is loud. Send the NEXT reply properly;
    #      it completes the orphaned line, so the driver sees body1+body2.
    second = json.dumps({"request_id": "next-one", "ok": True}).encode()
    srv.sendall(second + b"\n")
    raised = None
    try:
        nxt = c.recv(timeout=2.0)
    except Exception as exc:
        raised = exc
        nxt = None
    row("next_reply_concatenates_onto_the_orphan",
        raised is not None,
        f"raised {type(raised).__name__}" if raised else f"returned {nxt!r} — NO cascade")
    row("cascade_is_LOUD_not_silent",
        isinstance(raised, json.JSONDecodeError),
        "json.loads in recv() has no try/except, so it raises rather than "
        "mis-parsing — a greppable signature, not a quiet corruption")
    srv.close(); cli.close()

    # ── 3b. THE DRIVER IS NOT THE ONLY WRITER. The accept loop pings every
    #        live slot roughly every 5s (25 idle polls x 200ms,
    #        ipc_server.cpp:~1352), through send_to_slot — which appends its own
    #        newline. So an orphaned frame does NOT need the driver to issue
    #        another request to be completed: the next KEEPALIVE finishes the
    #        line for it, unprompted, within about five seconds.
    #
    #        This matters because the caveat that made absence uninformative was
    #        "only if that session read again". A ping makes the bridge the one
    #        that acts, so the cascade should fire well inside a 15s deadline.
    srv, cli = socket.socketpair()
    c = client_over(cli)
    srv.sendall(body)                                   # orphan: no newline
    srv.sendall(b'{"type":"ping"}' + b"\n")             # the keepalive, terminated
    raised = None
    try:
        got = c.recv(timeout=2.0)
    except Exception as exc:
        raised = exc
        got = None
    row("keepalive_ping_completes_the_orphan",
        isinstance(raised, json.JSONDecodeError),
        f"a ping alone triggers the cascade: {type(raised).__name__ if raised else got!r}")
    row("cascade_needs_no_driver_request",
        raised is not None,
        "the bridge writes unprompted every ~5s, so the orphan does not sit "
        "idle waiting for the client to ask for something")
    srv.close(); cli.close()

    # ── 4. THE CASCADE IS PER-CONNECTION. Each client slot has its own socket
    #      and BridgeClient its own _buf, so a desync on one session's stream
    #      leaves every other session untouched. This matters for reading
    #      arma7: the 77 actions served after the request may belong to other
    #      sessions entirely (six agents were running), in which case their
    #      continuing to work is NOT evidence against a desync on the one
    #      session that timed out. Demonstrated, not asserted.
    srv_a, cli_a = socket.socketpair()
    srv_b, cli_b = socket.socketpair()
    ca, cb = client_over(cli_a), client_over(cli_b)
    srv_a.sendall(body)                 # session A: orphaned, no newline
    srv_b.sendall(body + b"\n")         # session B: clean
    a_got = ca.recv(timeout=0.4)
    b_got = cb.recv(timeout=2.0)
    row("desync_does_not_cross_sessions",
        a_got is None and isinstance(b_got, dict),
        "A times out while B is unaffected — so a healthy sibling session "
        "proves nothing about the stuck one")
    for s_ in (srv_a, cli_a, srv_b, cli_b):
        s_.close()

    print()
    if not fails:
        print("MECHANISM CONFIRMED, and it carries its own kill condition:")
        print("  A missing trailing newline DOES produce the arma7 symptom —")
        print("  the reply is served, the body reaches the client, and recv()")
        print("  waits out the whole deadline and returns None.")
        print("  BUT the very next reply then raises JSONDecodeError out of")
        print("  recv(). So if arma7's driver log shows NO JSONDecodeError after")
        print("  the abort, the newline exit did NOT fire and this mechanism is")
        print("  struck off. That check is driver-side and costs no new roll.")
        print()
        print("SCOPE LIMITS ON THAT KILL CONDITION, REVISED:")
        print("  (a) SUPERSEDED. It was 'only if that session read again' —")
        print("      but the client does not have to ask. The accept loop pings")
        print("      every live slot ~every 5s through send_to_slot, which")
        print("      appends a newline, so a keepalive completes the orphan")
        print("      unprompted (rows above). Inside a 15s deadline there are")
        print("      ~2-3 such opportunities.")
        print("      WHAT REPLACES IT: absence is evidence against the newline")
        print("      exit only if at least one ping was ATTEMPTED **and")
        print("      SUCCEEDED** on that slot in the window. A socket that is")
        print("      backpressured or unwritable drops the pings too — and")
        print("      successful sends are not logged, so for arma7 this stays")
        print("      NOT EVALUABLE, for a narrower reason than before.")
        print("  (b) The cascade is PER-CONNECTION (row 4). Search the log of")
        print("      the session that timed out, not the run as a whole — the")
        print("      other five agents' replies are on other sockets and would")
        print("      be unaffected either way.")
    print(f"\n{'FAIL: ' + ', '.join(fails) if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
