// session_routing_probe.cpp — DOES A DRAINED REPLY GO TO THE CLIENT THAT ASKED?
// (ck-n7n0)
//
// The game thread has no send target, so its replies BROADCAST to every live
// slot and clients are expected to filter by request_id. Measured: 26 of the
// bridge's 180 registered actions are queue-bound and therefore broadcast, and
// they are the WRITES. Measured also: ck3agent's bridge_client.recv() does not
// filter — it returns the first non-ping line, and only the declare path
// filters. So one agent's create_title could be answered, as far as it can
// tell, by another agent's raise_armies confirmation.
//
// This drives the matching RULE. send_to_session_id itself touches the
// g_clients global and cannot link without the engine, so the rule was
// extracted to client_slot.h — which is where the mistakes live anyway.
#include "../include/client_slot.h"

#include <cstdio>
#include <string>

namespace {
int g_failed = 0;
void row(const char* name, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s\t%d\n", name, ok ? 1 : 0);
}
}  // namespace

using namespace bridge;

int main() {
    std::printf("case\tvalue\n");

    constexpr int N = 6;
    ClientSlot slots[N];
    auto open_slot = [&](int i, const char* sid) {
        slots[i].in_use = true; slots[i].fd = 100 + i;
        slots[i].gen = 7 + i;   slots[i].session_id = sid;
    };
    open_slot(0, "sess-alpha");
    open_slot(1, "sess-bravo");
    open_slot(2, "sess-charlie");
    // slot 3 held the session but has since been RELEASED
    slots[3].in_use = false; slots[3].fd = -1; slots[3].session_id = "sess-delta";
    // slot 4 is open with no session yet (connected, pre-handshake)
    slots[4].in_use = true; slots[4].fd = 104; slots[4].session_id = "";

    // ── the asker is found, and it is the RIGHT one ─────────────────────────
    row("an_open_session_is_found",        find_slot_for_session(slots, N, "sess-bravo") == 1);
    row("the_first_slot_is_not_privileged", find_slot_for_session(slots, N, "sess-charlie") == 2);

    // THE ROW THAT MATTERS. If the lookup returned any open slot, every row
    // above would still pass — it must return the slot belonging to THIS
    // session and no other. Asking for alpha must never yield bravo's slot.
    const int a = find_slot_for_session(slots, N, "sess-alpha");
    const int b = find_slot_for_session(slots, N, "sess-bravo");
    row("two_sessions_resolve_to_DIFFERENT_slots", a == 0 && b == 1 && a != b);

    // ── and when it cannot be found, it says so ─────────────────────────────
    // -1 means "fall back to broadcast", never "drop the reply": an engine
    // event has no session, and a client that disconnected mid-flight has no
    // slot. Narrowing delivery must not become silencing it.
    row("a_released_slot_does_not_match",  find_slot_for_session(slots, N, "sess-delta") == -1);
    row("an_unknown_session_does_not_match", find_slot_for_session(slots, N, "sess-nobody") == -1);
    row("an_empty_session_id_does_not_match", find_slot_for_session(slots, N, "") == -1);
    row("a_null_session_id_does_not_match", find_slot_for_session(slots, N, nullptr) == -1);
    row("a_null_slot_array_does_not_match", find_slot_for_session(nullptr, N, "sess-alpha") == -1);

    // A slot with no session must not swallow a lookup for the empty string,
    // and must not match a real session either.
    row("a_handshakeless_slot_matches_nothing",
        find_slot_for_session(slots, N, "sess-alpha") != 4 &&
        find_slot_for_session(slots, N, "") == -1);

    // A session id that is a PREFIX of a live one must not match it — the
    // comparison has to be equality, not a prefix or a substring.
    row("a_prefix_of_a_live_session_does_not_match",
        find_slot_for_session(slots, N, "sess-alph") == -1 &&
        find_slot_for_session(slots, N, "sess") == -1);

    // THE DISCRIMINATION ROW. A lookup that always returned -1 would pass every
    // negative row above; one that always returned 0 would pass some positives.
    // It must do both, so assert the two populations differ.
    row("the_lookup_DISCRIMINATES_rather_than_answering_one_way",
        find_slot_for_session(slots, N, "sess-alpha") >= 0 &&
        find_slot_for_session(slots, N, "sess-nobody") < 0);

    std::printf("\n%s: %d row(s) failed\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
