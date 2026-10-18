"""pipeline_correlation.py — THE shared client path for pipeline-mode replies.

ck-6kvq.18 / ck-6kvq.19. One module, imported by every NDJSON client that sends
an action and waits for its answer, because the three clients that got this
wrong got it wrong three different ways and a fourth copy would have been a
fourth way.

WHAT A PIPELINE-MODE BRIDGE ACTUALLY DOES. A tick- or paused-polarity call is
answered TWICE:

    -->  {"actions":[{"action":"query_character","character_id":14859,
                      "request_id":"<rid>"}]}
    <--  {"type":"accepted","accepted":true,"ticket":"tk_00000049",
          "polarity":"paused","flush_eta_ms":9999,"action":"query_character",
          "for_request_id":"<rid>"}                     <- NOW, the ACK
    ...  pings, tick_start, checkpoint_open ...          <- interleaved pushes
    <--  {"request_id":"<rid>", ...,"ticket":"tk_00000049","bridge_record":{...},
          "success":true,"gold":123.45,...,"type":"result"}   <- AFTER the flush

The ack is deliberately keyed `for_request_id` and NOT `request_id`
(bridge/src/pipeline.cpp emit_accepted, design §8.2) so that a client routing
STRICTLY by request_id ignores it. THE BRIDGE IS NOT AT FAULT. What was false
was the inference that the asymmetry alone bought backward compatibility: it
protects a strict router and nothing else, and four shipped clients were not
strict routers. Each returned the ACK as the answer — which has no success, no
gold, no dynasty_id — and then ran one frame out of phase forever. Silent: no
exception, no timeout, no non-zero exit.

LIVE EVIDENCE, 2026-09-13 (ck-zgh4, on the node, on a main-based bridge). The
node's READINESS PROBE read that first frame as the result of query_character,
found no dynasty_id, and reported "the character DB is not live" — a CONFIDENT
WRONG ANSWER ABOUT THE WORLD, and indistinguishable from a dead world. The
payload arrived only via a later `check` carrying that ticket. That is why this
module does not merely skip the ack: it KEEPS THE TICKET and settles it.

THE TWO WAYS A RESULT ARRIVES, and why both are needed:

  1. THE PUSH. The flush broadcasts the result frame from the game thread
     (ipc_server.cpp send_observation with no target slot -> broadcast). A
     client that stays connected and keeps reading gets it, keyed by its own
     request_id.
  2. THE CHECK. `check` is ANY-polarity, is served inline, never waits for a
     flush, and returns the ticket's stored `result` once it is terminal
     (pipeline.cpp handle_check). This is the path the node probe eventually
     used, and it is the only path available to a client whose push was missed
     — a reconnect between the ack and the flush, a recv budget shorter than
     flush_eta_ms, or a hook that cannot hold the socket open for ten seconds.

So: wait for the push until the ack's own ETA says it should have landed, then
ASK. Waiting alone is not a settlement protocol; it is a hope.

PUBLIC SURFACE

    PIPELINE_PUSH_TYPES   frames that are unsolicited BY CONSTRUCTION
    TERMINAL_STATES       ticket states that will never change again
    frame_matches(msg, request_id) -> "match" | "skip"
    is_our_ack(msg, request_id) -> bool
    ack_eta_seconds(msg) -> float
    ticket_of(msg) -> str | None
    build_check_envelope(ticket, request_id) -> dict
    settle(send=..., recv=..., ...) -> dict          THE driver
    TicketUnsettled                                   typed give-up

Every name a client swaps for its negative control is module-level and
rebindable on purpose; see scripts/test_pipeline_client_correlation.py.
"""

from __future__ import annotations

import time
import uuid

# Frames the bridge emits without being asked. A skip list can only be
# exhaustive over frames that can be NAMED, which is why ck-6kvq.18 also gave
# `type` to the pipeline result ("result") and refusal ("refused") frames — they
# had none, so for a client that also sent no request_id they carried neither a
# type nor a key and were literally unroutable.
PIPELINE_PUSH_TYPES = frozenset({
    "ping",
    "tick_start",
    "observation",
    "notification",
    "event",
    "accepted",            # THE ack that was read as the result
    "checkpoint_open",     # gate broadcasts: no request_id, ever
    "checkpoint_overdue",
    "handshake_ack",
})

# ticket_table.cpp ticket_state_name(). "unknown" is handle_check's typed
# refusal for an id the bridge never minted or that aged out of the ring.
TERMINAL_STATES = frozenset({"done", "failed", "expired", "unknown"})
LIVE_STATES = frozenset({"pending", "running"})

# How long past the ack's own flush ETA we keep listening for the push before
# falling back to `check`. The push is cheaper — no round trip, and it carries
# the attribution spine verbatim — so it gets first refusal.
PUSH_GRACE_S = 0.5
# ...but never blind for longer than this. A flush period is seconds (the node
# saw flush_eta_ms 9999), and a client that simply waits it out is a client that
# cannot tell "not flushed yet" from "my push was lost" — which is the state the
# ck-zgh4 readiness probe was in when it declared a live world dead. So ask
# early, once. The check reply carries the ticket's OWN remaining flush_eta_ms,
# so the poll interval corrects itself to the real flush period immediately
# afterwards: two round trips in the worst case, not a poll storm.
FIRST_CHECK_S = 0.5
# Between two checks of a still-running ticket that did not quote an ETA.
CHECK_POLL_S = 0.25


class TicketUnsettled(TimeoutError):
    """A ticket was accepted and never reached a terminal state in time.

    Carries the ticket so a caller can report WHICH call it lost — "your call
    was lost" and "your war was declared and the verdict went missing" are
    different facts and must not read the same.
    """

    def __init__(self, message, ticket=None, state=None, action=None):
        super().__init__(message)
        self.ticket = ticket
        self.state = state
        self.action = action


def frame_matches(msg, request_id):
    """Classify one received frame against the request we are waiting on.

    "match" -> this is our answer, whatever its type.
    "skip"  -> an unsolicited push, or another request's reply: keep reading.

    The `request_id is None` tolerance at the end is deliberate and must stay:
    a LEGACY-mode bridge does not echo request_id on every path, and this module
    has to keep those clients working unchanged. It is safe only because it is
    now guarded by an exhaustive skip list of named pushes.
    """
    if not isinstance(msg, dict):
        return "skip"
    rid = msg.get("request_id")
    if request_id and rid == request_id:
        return "match"
    if rid is not None:
        return "skip"
    if msg.get("type") in PIPELINE_PUSH_TYPES:
        return "skip"
    return "match"


def is_our_ack(msg, request_id):
    """True when `msg` is the acceptance ack for OUR call.

    Keyed on `for_request_id` — the ack never carries `request_id`, which is the
    entire point of §8.2. A client that sent no request_id gets no
    `for_request_id` back either; in that case, single-flight, the only ack on
    the wire is ours. That case is not hypothetical: ~20 shipped callers omit
    request_id (see ck-6kvq.19).
    """
    if not isinstance(msg, dict) or msg.get("type") != "accepted":
        return False
    if request_id:
        return msg.get("for_request_id") == request_id
    return msg.get("for_request_id") in (None, "")


def ack_eta_seconds(msg):
    """The ack's own flush ETA, in seconds. 0.0 when it does not say.

    The result genuinely CANNOT arrive before this, so treating the wait as a
    timeout is how a correct pipeline-mode bridge gets reported broken.
    """
    eta = msg.get("flush_eta_ms") if isinstance(msg, dict) else None
    if isinstance(eta, bool) or not isinstance(eta, (int, float)):
        return 0.0
    return max(0.0, float(eta) / 1000.0)


def ticket_of(msg):
    """The ticket id on an ack (or on any frame that carries one)."""
    if not isinstance(msg, dict):
        return None
    tid = msg.get("ticket")
    return tid if isinstance(tid, str) and tid else None


def build_check_envelope(ticket, request_id=None):
    """The envelope that asks the bridge to settle one ticket.

    Flat `ticket`, which handle_check reads via action_args and therefore
    accepts flat or under params (ck-6kvq.15). A check is ANY-polarity: it is
    served inline and never waits for a flush, so it cannot deadlock behind the
    very flush we are waiting on.
    """
    inner = {"action": "check", "ticket": ticket}
    if request_id:
        inner["request_id"] = request_id
    return {"actions": [inner]}


def check_entry_for(msg, ticket):
    """Pull our ticket's entry out of a `check` reply, or None."""
    if not isinstance(msg, dict) or msg.get("action") != "check":
        return None
    results = msg.get("results")
    if not isinstance(results, list):
        return None
    for entry in results:
        if isinstance(entry, dict) and entry.get("ticket") == ticket:
            return entry
    return None


def result_from_check_entry(entry, request_id, action):
    """Rebuild the RESULT FRAME the push would have carried, from a check entry.

    A settled call must read the same whichever way it was collected, or every
    consumer downstream needs two code paths. The §2.8 attribution spine is
    reconstructed from the fields handle_check already reports per ticket, and
    `settled_via` records which road it came by so a run can tell how often the
    push was missed.
    """
    payload = entry.get("result")
    frame = {}
    if request_id:
        frame["request_id"] = request_id
    frame["tick"] = entry.get("tick_executed", 0)
    frame["game_date"] = entry.get("game_date", "")
    frame["ticket"] = entry.get("ticket")
    frame["flush_id"] = entry.get("flush_id", 0)
    frame["bridge_record"] = {
        "session_id": entry.get("session_id", ""),
        "character_id": entry.get("character_id", 0),
        "flush_id": entry.get("flush_id", 0),
        "tick_executed": entry.get("tick_executed", 0),
        "game_date_executed": entry.get("game_date", ""),
    }
    if isinstance(payload, dict):
        for k, v in payload.items():
            # The frame's own type must never be shadowed by a handler key —
            # rapidjson resolves a duplicate key FIRST-wins and python
            # LAST-wins, so a duplicate reads as two different frame types.
            frame["handler_type" if k == "type" else k] = v
    frame["action"] = entry.get("action") or action
    frame["settled_via"] = "check"
    frame["ticket_state"] = entry.get("state")
    frame["type"] = "result"
    return frame


def refusal_from_check_entry(entry, request_id, action):
    """A ticket that ended without a payload is a TYPED failure, not silence."""
    state = entry.get("state")
    frame = {"type": "refused", "accepted": False, "success": False}
    if request_id:
        frame["request_id"] = request_id
    frame["action"] = entry.get("action") or action
    frame["ticket"] = entry.get("ticket")
    frame["ticket_state"] = state
    frame["reason"] = entry.get("reason") or ("ticket_%s" % (state or "unknown"))
    frame["settled_via"] = "check"
    return frame


def settle(send, recv, action, request_id, timeout,
           now=None, allow_check=True, on_ack=None, classify=None):
    """Drive ONE request to its real answer. THE shared path.

    `send(obj)`          puts one JSON object on the wire.
    `recv(timeout)`      returns the next frame as a dict, or None on timeout.
    `request_id`         ours; may be "" (then the ack is matched structurally).
    `timeout`            the caller's budget, in seconds, EXCLUDING the flush
                         wait — an accepted call extends it by the ETA the
                         bridge itself quoted, because the result cannot
                         physically arrive sooner.
    `allow_check`        False for a caller that must not spend a round trip
                         (a hook on a hard wall-clock budget).
    `on_ack(msg)`        optional observer, called once with our ack.
    `classify`           the match predicate. Defaults to this module's
                         frame_matches; a client passes its OWN module-level
                         name so the negative control, which rebinds that name,
                         actually reaches the code under test.

    Returns the result frame. Raises TimeoutError if nothing ever answers, or
    TicketUnsettled if a ticket was accepted and never settled.
    """
    clock = now or time.monotonic
    matches = classify or frame_matches
    deadline = clock() + timeout
    ticket = None
    push_deadline = None      # when to stop hoping for the push and ASK
    check_rid = None
    next_check = None

    while True:
        remaining = deadline - clock()
        if remaining <= 0:
            break

        # The push has had its chance: settle the ticket by asking.
        if (ticket and allow_check and push_deadline is not None
                and clock() >= push_deadline
                and (next_check is None or clock() >= next_check)):
            check_rid = uuid.uuid4().hex
            send(build_check_envelope(ticket, check_rid))
            next_check = clock() + CHECK_POLL_S

        # Wake at the next thing we mean to DO — give up on the push, or send
        # the next check — but only for caps still in the future. Clamping to a
        # cap that has already passed would spin the loop at recv(0.01).
        wait = remaining
        for cap in (push_deadline, next_check):
            if cap is None:
                continue
            ahead = cap - clock()
            if ahead > 0:
                wait = min(wait, ahead)
        msg = recv(wait)

        if msg is None:
            # A timeout is not a verdict while a ticket is outstanding: the
            # flush may simply not have run yet. Keep going until the caller's
            # deadline, checking as we go.
            if ticket and allow_check:
                push_deadline = clock()
                continue
            break

        # Our own settlement, arriving by check.
        if ticket is not None and check_rid and isinstance(msg, dict) \
                and msg.get("request_id") == check_rid:
            entry = check_entry_for(msg, ticket)
            if entry is None:
                continue
            state = entry.get("state")
            if state in LIVE_STATES:
                eta = entry.get("flush_eta_ms")
                if isinstance(eta, (int, float)) and not isinstance(eta, bool):
                    next_check = max(next_check or 0.0,
                                     clock() + max(CHECK_POLL_S, eta / 1000.0))
                continue
            if state in TERMINAL_STATES:
                if isinstance(entry.get("result"), dict):
                    return result_from_check_entry(entry, request_id, action)
                return refusal_from_check_entry(entry, request_id, action)
            continue

        if matches(msg, request_id) == "match":
            return msg

        if is_our_ack(msg, request_id):
            # ONCE. A second ack must never extend the budget again: an
            # unbounded extension is not a long wait, it is a hang, and the
            # rid-less case cannot fully tell our ack from someone else's on a
            # socket the bridge broadcasts to. The first ack names the ticket;
            # after that the ticket is what we settle on.
            if ticket is not None:
                continue
            ticket = ticket_of(msg)
            if ticket is None:
                continue          # an ack with no ticket settles nothing
            if on_ack is not None:
                on_ack(msg)
            eta = ack_eta_seconds(msg)
            # The result cannot land before the flush. Extend the budget by the
            # bridge's own quote plus the caller's original grace, and give the
            # push until then before spending a round trip on a check.
            deadline = max(deadline, clock() + eta + timeout)
            push_deadline = clock() + min(eta + PUSH_GRACE_S, FIRST_CHECK_S)
            continue
        # Anything else: someone else's reply, or a push. Keep reading.

    if ticket:
        raise TicketUnsettled(
            "%s: ticket %s accepted but never settled within %.1fs "
            "(request_id=%s)" % (action, ticket, timeout, request_id or "(none)"),
            ticket=ticket, action=action)
    raise TimeoutError("%s timed out after %.1fs (request_id=%s)"
                       % (action, timeout, request_id or "(none)"))
