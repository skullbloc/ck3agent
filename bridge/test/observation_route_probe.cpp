// observation_route_probe.cpp — DOES A RECORD GO TO THE OBSERVER? (ck-h4pn)
//
// emit_management_ledger_record is reached from inside try_inline_safe_query, which
// runs on the READER thread inside the window where g_send_target_slot names the
// calling client (ipc_server.cpp :1580 opens it, :1793 is the call, :1842 closes it).
// send_observation routes by that thread-local, so the ledger record — semantically
// an observation for whoever is listening — unicast back to the agent that had just
// made the call. The ARM relay drains the OBSERVER socket and never received one:
// zero of roughly 386 management calls in ep-02, under any spelling.
//
// THE FRAME WAS NEVER WRONG. ck-9rto drove the relay with the exact shape and it was
// accepted; the emitter's literals are in the .so that ran the set; a ledger row is
// un-ticketed by construction and the file contains no un-ticketed row of any kind.
// Every test that could have caught this inspected the FRAME, and a destination is
// invisible to any test that looks at the message.
//
// SO THE ROUTE IS NOW A NAMED DECISION. must_broadcast is the input the old code did
// not have, and these rows prove it WINS over a set target slot — which is the whole
// fix. A signature that could not express the wrong rule could not be tested against
// it, so the wrong input (a live target slot) is present in every row below.

#include "observation_route.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
static void row(const char* name, bool ok, const std::string& detail) {
    printf("%-58s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

int main() {
    using bridge::ObsRoute;
    using bridge::observation_route;
    const int kSlot = 3;      // a reader-thread target slot, as :1580 sets it
    const int kNoSlot = -1;   // the game thread's resting value

    // ── THE DEFECT'S CELL, and it is the one every ep-02 record was in ───────
    row("a_RECORD_broadcasts_even_INSIDE_the_reader_window",
        observation_route(kSlot, /*must_broadcast=*/true) == ObsRoute::Broadcast,
        "the ledger emitter runs on the reader thread with the slot set; this is "
        "the cell where ~386 records unicast to the calling agent");

    row("and_broadcasts_on_the_game_thread_too",
        observation_route(kNoSlot, true) == ObsRoute::Broadcast,
        "unchanged, which is what makes the row above a statement about the "
        "OVERRIDE rather than about the resting value");

    // ── REPLIES ARE UNTOUCHED, which is the other half of not breaking anything ─
    row("a_REPLY_still_unicasts_to_the_requesting_client",
        observation_route(kSlot, /*must_broadcast=*/false) == ObsRoute::Unicast,
        "the reader thread's reply routing is the reason the thread-local exists "
        "and must keep working — 5 other call sites depend on it");

    row("a_reply_with_no_target_still_broadcasts",
        observation_route(kNoSlot, false) == ObsRoute::Broadcast,
        "the game thread's events and on_tick drains reach every client");

    // ── THE PROPERTY: must_broadcast is NEVER overridden ─────────────────────
    // Four rows agreeing is a table. The claim is that no target slot whatsoever
    // can turn a record into a reply, so assert it across the range rather than at
    // two sample points — a future edit that special-cased slot 0, say, would
    // pass the cells above and fail here.
    bool ever_unicast = false;
    for (int slot = -2; slot <= 16; ++slot)
        if (observation_route(slot, true) != ObsRoute::Broadcast) ever_unicast = true;
    row("NO_target_slot_can_turn_a_record_into_a_reply", !ever_unicast,
        "checked across slots -2..16, including the -1 resting value and 0, which "
        "a `slot > 0` typo would treat as absent");

    // And the converse: without the override, the slot decides — so the override
    // is doing the work, not some unconditional broadcast.
    bool slot_decides = true;
    for (int slot = 0; slot <= 16; ++slot)
        if (observation_route(slot, false) != ObsRoute::Unicast) slot_decides = false;
    if (observation_route(-1, false) != ObsRoute::Broadcast) slot_decides = false;
    row("and_WITHOUT_the_override_the_slot_still_decides", slot_decides,
        "so the fix is an override and not a blanket broadcast — a blanket one "
        "would have sent every reply to every client");

    printf("\n%s (%d failing rows)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
