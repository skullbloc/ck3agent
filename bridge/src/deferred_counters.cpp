// deferred_counters.cpp — the deferred coordinator's refusal counts, as a LEAF
// TU (ck-p2sb, shape required by ck-t5bc's gate).
//
// WHY THESE TWO DEFINITIONS LIVE ALONE RATHER THAN IN deferred_drain.cpp.
// dispatch_router.cpp reports them in get_bridge_state, and several HOST test
// targets compile dispatch_router.cpp against stubs. Defining the counters in
// deferred_drain.cpp made those targets stop linking:
//
//     undefined reference to `bridge::g_busy_deferred_count'
//     -> test_release_handler, test_release_handler_negative_control,
//        test_init_order
//
// and adding deferred_drain.cpp to their source lists does NOT fix it — it
// drags in bridge::is_paused() and bridge::game_get_speed(), ENGINE symbols,
// into targets that are deliberately host-only. So the counters become a leaf:
// two atomics, no engine, no globals, nothing to stub.
//
// THIS IS ck-1ek9's DEFECT REPEATING, and the note beside src/pipeline_mode.cpp
// in bridge/CMakeLists.txt describes it in advance: "ANY host target that
// compiles dispatch_router.cpp needs this TU". A clean git merge, a semantic
// collision only the build could see. The leaf-TU shape is what stops the next
// symbol added to dispatch_router.cpp doing it again.
//
// AND THE FAILURE MODE IS QUIET, which is why it is worth this file rather than
// three source-list edits: the gate reports a missing binary as
// "declared but NOT BUILT" and SKIPS it. These three would have stopped running
// silently — and one of them is ck-sozz's NEGATIVE CONTROL. A negative control
// that is not built cannot fail as designed; it stops being evidence without
// announcing it, which is the vacuous-green family arriving through the build
// system rather than through a check.
//
// Declared in bridge.h beside run_deferred_pause_transparent; incremented in
// src/deferred_drain.cpp; reported by do_get_bridge_state.

#include <atomic>
#include <cstdint>

namespace bridge {

std::atomic<uint64_t> g_busy_deferred_count{0};
std::atomic<uint64_t> g_drain_unavailable_count{0};

}  // namespace bridge
