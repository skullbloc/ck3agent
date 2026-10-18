// accepted_ack_probe.cpp — THE ACCEPTANCE ACK, AS BYTES THE BRIDGE PRODUCED.
//
// ck-2r9k named this gap in its own test and could not close it:
//
//     "A CAPTURED FRAME WOULD BE BETTER AND THERE IS NONE. The hub grepped
//      every copied bridge.log (the d42 full smoke and the d39 slots) for an
//      accepted-ack frame and found none; scripts/fixtures/provenance/wire.jsonl
//      carries no `type` at all. Better still would be linking the real emit
//      path the way ck-9rto's board_frame_path_probe.cpp links
//      pipeline_emit_state, which a host-only pytest in `mise run test` cannot
//      reach."
//
// This is that. `test_pipeline_ack_correlation.real_ack` derives the frame by
// REGEX OVER pipeline.cpp's SOURCE — better than typing it, and still a reading
// of the producer's text rather than of its output. A regex cannot see a key
// written by a helper, a field appended conditionally, or an escape that comes
// out differently than it went in. This prints what the function actually
// wrote.
//
// IT DRIVES THE WHOLE SUBMIT PATH, not emit_accepted alone: a real envelope for
// `board_post` goes into the real pipeline_handle_envelope, which runs the real
// classify_polarity, the real FlushScheduler::submit, and emits the real ack.
// So the frame is evidence for two claims at once — what the ack looks like,
// and that a board post IS a buffered TICK_WRITE that gets one.
//
// WHY THAT SECOND CLAIM NEEDED A PROBE. `board_post` is listed in snapshot.cpp
// as bridge-internal, which reads like ANY polarity; it is not, because
// classify_polarity asks "is this a mutation?" BEFORE "is it bridge-internal?".
// Two lists in two files, and the answer is in neither of them alone.
#include "pipeline.h"
#include "episode_policy.h"
#include "board.h"
#include "session.h"
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
// THE REGISTRY, STUBBED OPEN -- and this is the one stub that changes the
// answer, so it is argued rather than copied. The real `is_registered_action`
// lives in dispatch_router.cpp, which drags in the engine; with the inherited
// `return false` this probe got
//     {"type":"refused",...,"reason":"unknown_action"}
// which is a TRUE frame about an UNREGISTERED verb and not the frame under
// test. Arm B registers board_post (board_verb_registration returns 1 when the
// board is enabled, which the line above makes true), and the d42 smoke
// measured ZERO unknown_action over 705 posts -- so open is what a real arm-B
// bridge answers here, and the refusal above was the probe's own gap.
bool is_registered_action(const char* a) {
    return a && a[0] && bridge::board_verb_registration(a) != 0;
}
bool is_deferred_drain_action(const char*) { return false; }
bool is_bridge_management_verb(const char*) { return false; }
size_t load_personality_locks_from_disk() { return 0; }
size_t personality_lock_count() { return 0; }
const char* personality_locks_path() { return ""; }
size_t restore_quarantined_personality_locks() { return 0; }
size_t personality_quarantine_count() { return 0; }
uint64_t personality_quarantine_epoch() { return 0; }
void set_personality_locks_identity(const WorldIdentity&) {}
std::vector<std::string>& observed_frames() {
    static std::vector<std::string> v; return v;
}
// THE CAPTURE POINT. emit_accepted's only output is this call
// (drain_provenance.h: "emit_accepted sends its frame through send_observation
// and DOES NOT LOG"), so recording here is recording the ack itself. Every
// other probe in this tree stubs this to `return true`, which is why no
// captured ack has ever existed.
bool send_observation(const char* p, size_t n) {
    observed_frames().emplace_back(p, n);
    return true;
}
bool peek_personality_locks_identity(WorldIdentity*) { return false; }
std::string build_envelope_clock_fragment() { return "\"tick\":0"; }
}  // namespace bridge

// ── DRIVE THE REAL SUBMIT PATH ─────────────────────────────────────────────
int main() {
    setenv("CK3_EPISODE_ARM", "b", 1);
    setenv("CK3_BOARD_ENABLED", "1", 1);
    setenv("CK3_PIPELINE_MODE", "pipeline", 1);
    // BOTH inits, and the MODE one is not optional: `pipeline_episode_init_from_env`
    // resolves the EPISODE policy, `pipeline_init_from_env` resolves the MODE,
    // and only the second decides whether pipeline_handle_envelope buffers
    // anything at all. Calling one and not the other left this probe in legacy
    // mode, where the envelope is not handled and nothing is emitted -- an
    // empty capture that reads exactly like "the bridge emits no ack".
    bridge::pipeline_init_from_env();
    bridge::pipeline_episode_init_from_env();
    bridge::board_init_from_env();

    // A PROBE THAT IS NOT IN PIPELINE MODE PRODUCES NO ACK AT ALL, and an empty
    // capture would then read as "the bridge emits nothing" instead of "this
    // probe never asked it to". Stated, not assumed.
    printf("MODE %s\n", bridge::pipeline_mode_name(bridge::pipeline_mode()));
    printf("POLARITY_board_post %s\n",
           bridge::polarity_name(bridge::classify_polarity("board_post")));

    const std::string line =
        "{\"actions\":[{\"action\":\"board_post\","
        "\"params\":{\"text\":\"probe\"},"
        "\"request_id\":\"rid-probe-0001\"}]}";
    const bool handled = bridge::pipeline_handle_envelope(line, nullptr);
    printf("HANDLED %s\n", handled ? "true" : "false");

    for (const std::string& f : bridge::observed_frames()) {
        printf("FRAME %s\n", f.c_str());
    }
    printf("FRAME_COUNT %zu\n", bridge::observed_frames().size());
    return bridge::observed_frames().empty() ? 2 : 0;
}
