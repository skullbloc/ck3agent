// board_frame_path_probe.cpp — WHERE IS THE BOARD BLOCK ON A TICK FRAME? (C6b)
//
// `episode_bundle._record_board_bounds` read the bridge's board stats off
// `tick_info["board"]`, at the TOP LEVEL of a get_tick_info frame.
// `pipeline_emit_state` writes them INSIDE the `pipeline` object it opens. So
// the collector found nothing on every real frame, every arm-B bundle sealed
// `bridge_published_no_board_block`, and ck-5gdx's witness — the one that says
// which manipulation the episode ran — could not fire on the one arm that has
// a manipulation.
//
// WHY THE ACCEPTANCE DID NOT CATCH IT, AND WHY THIS FILE IS C++. The existing
// lane feeds `record_tick_provenance({"board": stats, ...})`: a frame I built
// by hand, to the same wrong model as the code. Two artefacts written from one
// belief agree with each other and with nothing else. The only witness that can
// settle where the block IS, is THE EMITTER — so this probe drives the real
// `pipeline_emit_state` and prints what it actually wrote, and the Python side
// parses that instead of a fixture.
//
// A GREP WOULD NOT DO. ck-9rto mutated a structural claim in this very tree
// with a block comment and with a file CMake never compiles, and it passed both
// times: a grep asks whether text exists somewhere, not whether the bridge
// emits. This prints one line of JSON from a real builder.
//
// THE BOARD IS ARMED THROUGH THE DOCUMENTED OVERRIDE. `board_init_from_env()`
// gates on `episode_policy().arm == "b"` and honours CK3_BOARD_ENABLED as an
// explicit override "so a host test can arm the board without pretending to be
// an episode" (board.cpp). That is this probe, exactly, and the arm is set
// besides so the two agree rather than one covering for the other.
//
// EMITTED AS A WHOLE OBJECT. `pipeline_emit_state` writes `"pipeline":{...}`
// as a FRAGMENT — it does not open the enclosing object, because get_tick_info
// already has one open. The braces here are that enclosing object and nothing
// more, so what the Python side parses is shaped exactly like the frame the
// driver receives.
#include "pipeline.h"
#include "episode_policy.h"
#include "board.h"           //: board_init_from_env(), board()
#include "action_dispatch_internal.h"
#include "bridge.h"
#include "personality_locks.h"
#include "session.h"
#include "world_identity.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <shared_mutex>
#include <string>
#include <vector>
#include <csetjmp>

// The engine boundary, and nothing else. Every other symbol comes from the real
// host TUs in the link line.
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
std::atomic<uint64_t> g_tick_id{0};
bool is_paused() { return false; }
int  game_get_speed() { return 3; }
void game_pause() {}
void game_unpause() {}
void game_set_speed(int) {}
bool do_pause_game(const char*) { return true; }
bool do_unpause_game(const char*) { return true; }
bool do_set_speed(const char*) { return true; }
bool dispatch_action_with_response(const char*, const char*, std::string*) { return true; }
bool enqueue_action_envelope(const std::string&) { return true; }
SessionState get_session_state() { return SessionState::ACTIVE; }
std::string enumerate_decisions() { return "[]"; }
std::string enumerate_interactions() { return "[]"; }
thread_local bool g_in_protected_region = false;
thread_local sigjmp_buf g_recovery_point;
// The action CLASSIFIERS. Stubbed rather than linked (dispatch_router.cpp would
// drag in 29 more engine symbols) because the claim under test — does the hold
// flip — does not depend on how any action classifies. What it depends on is
// pipeline_note_episode_open and pipeline_emit_state, and both are REAL here.
bool is_registered_action(const char*) { return false; }
bool is_deferred_drain_action(const char*) { return false; }
bool is_bridge_management_verb(const char*) { return false; }
size_t load_personality_locks_from_disk() { return 0; }
size_t personality_lock_count() { return 0; }
const char* personality_locks_path() { return ""; }
size_t restore_quarantined_personality_locks() { return 0; }
size_t personality_quarantine_count() { return 0; }
uint64_t personality_quarantine_epoch() { return 0; }
void set_personality_locks_identity(const WorldIdentity&) {}
bool send_observation(const char*, size_t) { return true; }
bool peek_personality_locks_identity(WorldIdentity*) { return false; }
std::string build_envelope_clock_fragment() { return "\"tick\":0"; }
}  // namespace bridge

// ── THE FRAME, AS THE BRIDGE WRITES IT ─────────────────────────────────────
static std::string frame_now() {
    bridge::ResponseBuilder r;
    r.begin_object();
    bridge::pipeline_emit_state(r);   //: writes "pipeline":{...} INTO it
    r.end_object();
    return r.take();
}

int main() {
    // Both switches, and they say the same thing. The arm is what a real
    // episode sets; the override is what board.cpp documents for a host test.
    // Setting one and not the other would leave the probe's own arming
    // ambiguous in exactly the way this bead is about.
    setenv("CK3_EPISODE_ARM", "b", 1);
    setenv("CK3_BOARD_ENABLED", "1", 1);
    bridge::pipeline_episode_init_from_env();
    bridge::board_init_from_env();

    // A PROBE THAT CANNOT ARM THE BOARD MUST SAY SO, not print a frame with no
    // board block in it — that output is byte-identical to the one an arm-A
    // process produces, and the reader could not tell "the board was off" from
    // "the block is not at pipeline.board". An unfalsifiable negative is the
    // thing this probe exists to avoid.
    printf("BOARD_ENABLED %s\n", bridge::board().enabled() ? "true" : "false");
    printf("FRAME %s\n", frame_now().c_str());
    return bridge::board().enabled() ? 0 : 2;
}
