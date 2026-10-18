// session_identity_probe.cpp — DOES A RECONNECT KEEP THE SAME SESSION? (ck-y6fp)
//
// THE DESIGN THIS ANSWERS. ck-y6fp needs the clock hold RE-ARMED when the episode
// goes away, and the natural trigger is the lifetime of the session that declared
// the episode open. ck-9rto found the constraint that kills the obvious version:
// the harness client RECONNECTS TRANSPARENTLY (client.py _reconnect, one
// reconnect-and-retry on ConnectionError/BrokenPipeError/OSError, resuming with a
// stored token). So a deadman keyed on the SOCKET fires on a transient drop and
// freezes a live episode at 0.0000 game-days/sec — ck-zen6's exact failure, caused
// by the fix for ck-zen6.
//
// Keying on SESSION IDENTITY survives it, IF a reconnect really is the same
// session rather than a new one. That is the claim the whole design rests on, and
// I had READ it rather than driven it: ipc_server DETACHES instead of destroying
// in experiment mode, detach_from_slot keeps the resume token and the claims, and
// resume_by_token looks the token up and re-ATTACHES the session it finds. Every
// step of that is a sentence about code. This drives the real SessionRegistry and
// asks it.
//
// Host only. No game, no bridge, no socket.

#include "session.h"
#include "world_identity.h"

#include "bridge.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// The engine boundary, and nothing else — the same set the sibling
// observer_declaration_probe declares. Every session symbol under test is REAL.
namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t             g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex     g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}  // namespace bridge

using bridge::Session;
using bridge::SessionRegistry;

int main() {
    SessionRegistry reg;

    Session* s = reg.create("driver", Session::Mode::CONTROL);
    const std::string sid = s->session_id;
    const std::string tok = s->resume_token;

    // THE SOCKET GOES AWAY. slot_idx < 0 is the FORCED detach — the path taken
    // when the slot is already gone, which is what a dropped connection looks
    // like from here.
    reg.detach_from_slot(sid, -1, 0, 1000);
    const bool findable_after_drop = reg.find(sid) != nullptr;
    const bool detached_not_destroyed =
        findable_after_drop && reg.find(sid)->state == Session::State::DETACHED;

    // THE CLIENT COMES BACK, exactly as client.py does: same token, no new id.
    bridge::WorldIdentity presented;
    SessionRegistry::ResumeOutcome out =
        reg.resume_by_token(tok, presented, 0, 0, 1, 2000);
    const bool resumed_ok = out.session != nullptr;
    const bool same_session = resumed_ok && out.session->session_id == sid;
    const bool reattached =
        resumed_ok && out.session->state == Session::State::ATTACHED;

    // AND THE BOUND EXISTS ALREADY. A session that does NOT come back becomes
    // LOST past max_detach_ms — which is the trigger ck-y6fp can key on without
    // inventing a second timer beside the one the bridge already keeps.
    reg.detach_from_slot(sid, -1, 0, 3000);
    const std::vector<std::string> none_yet = reg.sweep_detached(3100, 30000);
    const std::vector<std::string> lost = reg.sweep_detached(3000 + 60000, 30000);

    std::printf("case\tvalue\n");
    std::printf("findable_after_drop\t%d\n", findable_after_drop ? 1 : 0);
    std::printf("detached_not_destroyed\t%d\n", detached_not_destroyed ? 1 : 0);
    std::printf("resume_status\t%s\n", out.status);
    std::printf("resume_same_session_id\t%d\n", same_session ? 1 : 0);
    std::printf("resume_reattached\t%d\n", reattached ? 1 : 0);
    std::printf("sweep_before_bound_names_none\t%d\n", none_yet.empty() ? 1 : 0);
    std::printf("sweep_past_bound_names_it\t%d\n",
                (lost.size() == 1 && lost[0] == sid) ? 1 : 0);
    return 0;
}
