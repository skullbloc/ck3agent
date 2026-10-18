#pragma once
//: ck-j6lk: WAS THIS SESSION EVER A PARTICIPANT IN THE EPISODE — one body, two
//: readers.
//:
//: The detach path in ipc_server.cpp keys R3's claim pin on this, and
//: bridge/test/claimless_detach_probe.cpp asserts it. It lives here because my first
//: version of that probe RE-SPELLED the condition as a local lambda — which proves
//: only that the same expression can be written twice, and would have gone on passing
//: if the branch's own copy had drifted. The probe now calls this function, so the
//: rows are about the shipped predicate rather than about a faithful copy of it.
//:
//: Dependency-free on purpose (no session.h, no socket layer): a host probe must be
//: able to evaluate it from two plain values, and the branch must be able to call it
//: while holding g_client_mutex.
//:
//: THE THREE STATES, ruled by the progenitor ("amend, re-freeze, then fix"), of which
//: only (a) changes any behaviour:
//:   (a) not declared AND claims_ever == 0 -> NOT a participant. The detach suppresses
//:       the detached note, ends the session, destroys it, and accrues nothing. These
//:       are the driver's helpers: episode-driver-C-steer-probe (0 activities) and
//:       episode-driver-C-speed-pin (2), which handshake in control mode, do one job
//:       and close. Pinning them cost 364 silent game-days each against an X of 18 and
//:       would have invalidated every registered episode.
//:   (b) DECLARED, claimed or not -> a participant, today's behaviour exactly. This is
//:       ck-3ru8's window: the world loads ~17s after the first connection, so a real
//:       agent has a stretch where claims_ever is still 0, and a seat that dies there
//:       must remain a MEASURED HOLE. Making a genuine agent failure invisible is the
//:       same defect this bead fixes, pointed the other way.
//:   (c) claims_ever > 0 -> a participant, today's behaviour exactly. This covers a
//:       seat whose declaration went missing, so a dropped field can never un-seat a
//:       session that actually steered.
//:
//: The remaining hole — an agent that fails to declare AND never claims — is not
//: closed here and is not closeable here: the bridge cannot distinguish it from a
//: helper. It is closed by publishing `episode_seat_sessions` in the episode block for the
//: validator to cross-check against the roster's declared_sessions, so a
//: disagreement between two independently produced numbers becomes a named
//: `seat_declaration_mismatch` rather than a silent exemption.
#include <cstdint>

namespace bridge {

//: True only for state (a). Deliberately phrased as the NEGATIVE — "never a
//: participant" — because that is the narrow, behaviour-changing case, and a reader
//: at the call site should see the exception rather than have to invert the rule.
inline bool never_an_episode_participant(bool declared_seat, uint64_t claims_ever) {
    return !declared_seat && claims_ever == 0;
}

}  // namespace bridge
