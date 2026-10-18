// drain_registry.h — the conductor's payload store and drain correlation.
//
// WHY THIS EXISTS (ck-6kvq.19):
//
// A tick-polarity call is not served inline. The conductor hands it to the
// GAME THREAD (`enqueue_action_envelope`) and the game thread reports back when
// it has actually run. Something has to remember, across that hand-off, WHICH
// ticket the returning reply belongs to.
//
// The first implementation remembered it by the client's `request_id`:
//
//     if (!pc.request_id.empty()) drain_waiting[pc.request_id] = call.ticket;
//
// `request_id` is OPTIONAL on the wire (ipc_server.cpp:627 and
// bridge_init.cpp both read it conditionally) and ~20 shipped clients omit it
// entirely. For every one of those calls there was no `else`: the action was
// enqueued, it EXECUTED with real side effects, and then nothing could ever
// settle its ticket. The ticket stayed Running until its TTL and then read
// `expired` — which says "your call was lost" about a war that was in fact
// declared — while its payload sat in the pending map forever. The flush
// record counted it as a success.
//
// A second defect rode on the same line: `request_id` is client-chosen and NOT
// required to be unique. scripts/bdt.sh:531 sends the fixed literal "bdt" on
// every call, so two in-flight bdt tickets collided in that map and the first
// was orphaned exactly as above.
//
// THE FIX, and the rule it comes from (same lesson as ck-6kvq.15): SETTLEMENT
// MUST NEVER DEPEND ON A FIELD THE CLIENT WAS FREE NOT TO SEND. Correlation is
// by the BRIDGE-MINTED TICKET, which is unique by construction and always
// present. The ticket rides to the game thread on the drain ENVELOPE — not on
// the inner action — so no handler ever sees a key its client did not write,
// and `action_args` resolution is untouched.
//
// This unit is deliberately engine-free so the hand-off/settle protocol can be
// driven on a host with no game, which is where the bug was invisible before:
// every existing test supplied a request_id.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "rapidjson/document.h"

namespace bridge {

// The full per-call record the conductor needs to execute a buffered call
// later. The scheduler deliberately only carries {session, action, polarity};
// the payload lives here, keyed by ticket.
struct PendingCall {
    uint64_t    ticket = 0;
    std::string session_id;
    std::string request_id;     // OPTIONAL. Never used for correlation.
    std::string action;
    std::string inner_json;     // the single-action object, verbatim
    uint32_t    character_id = 0;
};

// The envelope key the bridge mints. Underscore-prefixed and documented as
// bridge-private: a client never sends it and no handler reads it.
extern const char* const kDrainTicketKey;   // "_pipeline_ticket"

class DrainRegistry {
public:
    // ---- payload store ----------------------------------------------------
    void   put(uint64_t ticket, const PendingCall& pc);

    // Copy out and ERASE. This is inline service: the conductor serves the
    // call itself and owns the result from here on.
    bool   take(uint64_t ticket, PendingCall* out);

    // Copy out and LEAVE IN PLACE. This is drain service: the payload is the
    // record of a call that is still in flight on the game thread.
    bool   peek(uint64_t ticket, PendingCall* out) const;

    bool   has_payload(uint64_t ticket) const;
    size_t payload_count() const;

    // Drop a payload without settling (cancel of a still-Pending ticket).
    bool   drop(uint64_t ticket);

    // ---- drain hand-off ---------------------------------------------------
    // Register `ticket` as outstanding on the game thread and build the
    // envelope to enqueue. Returns false when there is no payload for it.
    //
    // The payload is deliberately LEFT in place: it is the only record that
    // this ticket is mid-flight, and it is erased by settle().
    bool   hand_off(uint64_t ticket, std::string* envelope_out);

    bool   outstanding(uint64_t ticket) const;
    size_t outstanding_count() const;

    // ---- settlement -------------------------------------------------------
    // The game thread reports back. `envelope_json` is the envelope it just
    // executed; `request_id` is whatever it found on the inner action, which
    // may be null or empty and is NOT used to correlate.
    //
    // Returns the ticket that was settled, or 0 when this envelope was not one
    // of ours (every non-conductor enqueuer's envelope lands here too).
    //
    // Settling erases BOTH the outstanding registration and the payload, so a
    // settled ticket cannot leak and cannot be settled twice.
    uint64_t settle_from_drain(const rapidjson::Value& envelope,
                               const char* request_id);
    uint64_t settle_from_drain(const char* envelope_json,
                               const char* request_id);

    // Settle by ticket directly (used by the drain once it has read the tag,
    // and by tests).
    bool     settle(uint64_t ticket);

    void     clear();

    // ---- pure helpers (no state; exposed for the drain and for tests) -----
    static std::string build_envelope(const std::string& session_id,
                                      const std::string& inner_json,
                                      uint64_t ticket);

    // Read the bridge-minted ticket tag off a drain envelope. 0 = untagged,
    // i.e. this envelope did not come from the conductor.
    static uint64_t ticket_from_envelope(const rapidjson::Value& envelope);
    static uint64_t ticket_from_envelope(const char* envelope_json);

private:
    mutable std::mutex                            mu_;
    std::unordered_map<uint64_t, PendingCall>     payloads_;
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
    // NEGATIVE CONTROL ONLY — the pre-fix correlation, keyed by the client's
    // optional, non-unique request_id. Never compiled into the shipped bridge.
    std::unordered_map<std::string, uint64_t>     rid_waiting_;
#else
    std::unordered_set<uint64_t>                  outstanding_;
#endif
};

}  // namespace bridge
