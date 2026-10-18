// test_debug_verbs.cpp — rc2 B2: the debug verbs are compiled out and refused.
//
// Links the REAL dispatch_router.cpp (same scaffolding as test_release_handler).
//
//   1. CENSUS FROM THE CODE. The debug set is what CK3_BRIDGE_DEBUG_VERBS adds
//      to action_registry.inc: the table is expanded here with and without the
//      define and the difference taken. It must equal the six pinned names, so
//      a verb moved out of the #ifdef, or a new one added to it, reds here.
//      (A NEW ungated verb of the raw-pointer class is caught by
//      check_raw_pointer_inputs.py, which walks the handlers.)
//   2. Each one, through dispatch_action AND through query{type:<name>},
//      answers error=unavailable_in_release; none is registered;
//      debug_verb_compiled_out() says so (the IPC reader uses it).
//   3. Control: an ordinary unknown query type is NOT answered that way.
//
// NEGATIVE CONTROL: test_debug_verbs_negative_control is this source with
// dispatch_router.cpp built -DCK3_BRIDGE_DEBUG_VERBS. Its handlers are the
// weak no-op stubs, so nothing answers unavailable_in_release and it MUST fail
// (ctest WILL_FAIL).
#include "action_dispatch_internal.h"
#include "session.h"
#include "pipeline.h"
#include "intent_queue.h"
#include "name_tables.h"
#include "snapshot.h"
#include "episode_policy.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

namespace bridge {
uintptr_t                g_base_addr = 0;
FILE*                    g_logfile = nullptr;
std::vector<MemRange>    g_valid_ranges;
std::shared_mutex        g_ranges_mutex;
std::mutex               g_controlled_mutex;
std::vector<uint32_t>    g_controlled_chars;
std::atomic<uint32_t>    g_controlled_primary{0};
std::atomic<uint32_t>    g_controlled_heir{0};
std::atomic<bool>        g_agent_mode{false};
std::atomic<bool>        g_auto_respond_events{false};
std::atomic<uint64_t>    g_tick_id{0};
std::atomic<uint64_t>    g_cb_query_busy_count{0};
std::atomic<bool>        g_war_stance_record_walk_enabled{false};
IntentQueue              g_intent_queue;
size_t IntentQueue::pending_count() { return 0; }
void council_lock_remove(uint32_t) {}
bool pipeline_owns_clock()       { return false; }
bool is_clock_verb(const char*)  { return false; }
uint64_t pipeline_current_flush_id() { return 0; }
PipelineMode pipeline_mode() { return PipelineMode::Legacy; }
void game_pause()   {}
void game_unpause() {}
const char* session_state_string() { return "test"; }
std::string get_cached_character_name(uint32_t) { return "(test)"; }
std::string enumerate_interactions() { return "[]"; }
std::string enumerate_decisions()    { return "[]"; }
bool set_character_subsystems(uint32_t, const char*) { return false; }
bool get_character_subsystems(uint32_t, std::string*) { return false; }
bool get_client_lock_info(ClientLockInfo*) { return false; }
bool is_game_state_query(const char*) { return false; }
const char* offset_validation_group_name(int) { return ""; }
OffsetValidationResult validate_offsets() { return OffsetValidationResult{}; }
}  // namespace bridge

using namespace bridge;

// ── 1. the census, from the table itself ────────────────────────────────────
#pragma push_macro("CK3_BRIDGE_DEBUG_VERBS")
#undef CK3_BRIDGE_DEBUG_VERBS
#define BRIDGE_ACTION(NAME, HANDLER) NAME,
static const char* const kReleaseTable[] = {
#include "../src/action_registry.inc"
};
#define CK3_BRIDGE_DEBUG_VERBS 1
static const char* const kDebugTable[] = {
#include "../src/action_registry.inc"
};
#undef BRIDGE_ACTION
#pragma pop_macro("CK3_BRIDGE_DEBUG_VERBS")

namespace {
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "    ok   " : "    FAIL ") << what << "\n";
    if (!ok) ++g_failures;
}
std::string error_of(const std::string& j) {
    const char* k = "\"error\":\"";
    size_t i = j.find(k);
    if (i == std::string::npos) return "";
    i += strlen(k);
    return j.substr(i, j.find('"', i) - i);
}
struct Scoped {
    ClientContext ctx;
    ClientContextGuard guard;
    explicit Scoped(Session* s) : ctx{s, nullptr, nullptr}, guard(&ctx) {}
};
}  // namespace

int main() {
#ifdef CK3_BRIDGE_DEBUG_VERBS
    std::cout << "=== rc2 B2 NEGATIVE CONTROL (dispatch_router.cpp built with "
                 "CK3_BRIDGE_DEBUG_VERBS): must FAIL ===\n";
#else
    std::cout << "=== rc2 B2 debug verbs compiled out ===\n";
#endif
    std::set<std::string> rel(std::begin(kReleaseTable), std::end(kReleaseTable));
    std::set<std::string> debug;
    for (const char* n : kDebugTable)
        if (!rel.count(n)) debug.insert(n);
    const std::set<std::string> pinned = {"execute_console_command", "inject_mouse_click",
                                          "memory_probe", "resolve_global",
                                          "select_observe_default",
                                          "set_cb_selector_override"};
    std::string got;
    for (const auto& n : debug) got += n + " ";
    check(debug == pinned, "debug set from action_registry.inc = the six pinned verbs: " + got);

    Session* s = g_session_registry.create("debug-verbs-test");
    Scoped scope(s);
    for (const auto& n : debug) {
        std::string out;
        dispatch_action_with_response(n.c_str(), "{}", &out);
        check(error_of(out) == "unavailable_in_release", n + " via dispatch_action: " + out);

        std::string q;
        const std::string params = "{\"type\":\"" + n + "\"}";
        dispatch_action_with_response("query", params.c_str(), &q);
        check(error_of(q) == "unavailable_in_release", "query{type:" + n + "}: " + q);

        check(!is_registered_action(n.c_str()), n + " is not registered");
        check(debug_verb_compiled_out(n.c_str()), n + ": debug_verb_compiled_out");
    }

    // 3. Control: the refusal is about these names, not every query.
    {
        std::string q;
        dispatch_action_with_response("query", "{\"type\":\"no_such_query_type\"}", &q);
        check(error_of(q) != "unavailable_in_release",
              "control: an unknown query type is not unavailable_in_release: " + q);
        check(!debug_verb_compiled_out("query_character"), "control: query_character is not a debug verb");
    }

    std::cout << (g_failures ? "FAILED: " : "passed: ") << g_failures << " failure(s)\n";
    return g_failures ? 1 : 0;
}
