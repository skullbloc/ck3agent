// pipeline_mode.cpp — the mode policy table (ck-6kvq.4, R8). See pipeline_mode.h.
//
// Pure. <cstring> is the only include, and getenv is deliberately NOT called
// here: `parse_pipeline_mode` takes the string so the caller owns the
// environment and the parse stays host-testable.

#include "pipeline_mode.h"

#include <cstring>

namespace bridge {

const char* pipeline_mode_name(PipelineMode m) {
    switch (m) {
        case PipelineMode::Legacy:   return "legacy";
        case PipelineMode::Pipeline: return "pipeline";
        case PipelineMode::Null:     return "null";
    }
    return "legacy";
}

PipelineMode parse_pipeline_mode(const char* s, bool* recognised) {
    // An UNSET variable is the normal case and is not a mistake, so it counts as
    // recognised; a MISSPELLED one is a mistake and must not read as legacy by
    // accident. CK3_PIPELINE_MODE=pipelien starting a legacy episode that the
    // bundle then labels `pipeline` is precisely the "wrong answer that looks
    // like a right one" class this epic keeps finding.
    if (recognised) *recognised = true;
    if (!s || !s[0]) return PipelineMode::Legacy;
    if (strcmp(s, "pipeline") == 0) return PipelineMode::Pipeline;
    if (strcmp(s, "null")     == 0) return PipelineMode::Null;
    if (strcmp(s, "legacy")   == 0) return PipelineMode::Legacy;
    if (recognised) *recognised = false;
    return PipelineMode::Legacy;
}

// THE NEGATIVE CONTROL LIVES HERE, behind CK3_NULL_MODE_LEGACY_POLICY.
//
// Built with that define, the five predicates below revert to the policy the
// bridge actually had before this bead: `pipeline_active()` was `mode !=
// Legacy`, everything else was derived from it or from `mode == Pipeline`, and
// nothing consulted the mode about control sessions or the gate at all. The
// CMake target `test_pipeline_mode_negative_control` compiles exactly that and
// MUST FAIL. It is not part of a green suite — it is the evidence that the
// green suite is measuring something. A test of a policy table would otherwise
// pass by restating the table it is testing.
#if defined(CK3_NULL_MODE_LEGACY_POLICY) && CK3_NULL_MODE_LEGACY_POLICY

bool mode_buffers_calls(PipelineMode m)   { return m != PipelineMode::Legacy; }
bool mode_runs_flush_loop(PipelineMode m) { return m != PipelineMode::Legacy; }
bool mode_owns_clock(PipelineMode m)      { return m == PipelineMode::Pipeline; }
bool mode_admits_control_sessions(PipelineMode)  { return true; }
// Pre-ck-1ek9 an observe session could never issue a clock verb: `set_speed` is
// a mutation and the dispatch gate refused it `observe_mode`, full stop.
bool mode_admits_observer_clock_verbs(PipelineMode) { return false; }

#else

bool mode_buffers_calls(PipelineMode m) {
    return m == PipelineMode::Pipeline;
}

bool mode_runs_flush_loop(PipelineMode m) {
    return m == PipelineMode::Pipeline;
}

bool mode_owns_clock(PipelineMode m) {
    return m == PipelineMode::Pipeline;
}

bool mode_admits_control_sessions(PipelineMode m) {
    return m != PipelineMode::Null;
}

bool mode_admits_observer_clock_verbs(PipelineMode m) {
    // ONLY null, and only because null is the mode in which no control session
    // can exist to do it instead.
    return m == PipelineMode::Null;
}

#endif

const char* pipeline_mode_control_downgrade_reason() {
    return "null_mode_observer_only";
}

}  // namespace bridge
