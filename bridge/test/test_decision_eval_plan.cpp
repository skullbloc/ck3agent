// test_decision_eval_plan.cpp — L22, smoke19 row D1: which failure keys the
// annotated decision eval extracts, and the typed skip list.
//
// Fixture: the smoke19 S.D1 reply (probe-supp.jsonl, 0867.02.13, character
// 11329): favored_movement_consults_heaven, stress_loss_comfort_eater and
// restore_pope_in_rome came back `shown: false` while the bridge log has one
// SIGSEGV safety-net line each at rip_off 0x745aea0 (bridge-segv.log 44764-
// 44770) — the failure-key pass on a not-shown decision. Their is_valid
// verdict was false (the pass ran only for a failing block).
//
// Built twice: plain (must pass) and with -DDECISION_EVAL_PLAN_PREFIX_CONTROL,
// the pre-fix "every failing block" rule, registered WILL_FAIL.
#include <cstdio>
#include <cstring>
#include "decision_eval_plan.h"

using namespace bridge::decision_eval;

static int failures = 0;
static void expect(const char* what, bool got, bool want) {
    const bool ok = got == want;
    if (!ok) ++failures;
    std::printf("%s %s: %s (want %s)\n", ok ? "PASS" : "FAIL", what, got ? "true" : "false",
                want ? "true" : "false");
}

int main() {
    // smoke19 S.D1: three not-shown candidates with a failing is_valid.
    const char* d1[] = {"favored_movement_consults_heaven_decision",
                        "stress_loss_comfort_eater_decision",
                        "restore_pope_in_rome_decision"};
    for (const char* n : d1) {
        char what[160];
        std::snprintf(what, sizeof what, "no failure-key pass for not-shown %s", n);
        expect(what, extract_failure_key(/*shown=*/false, /*block_passed=*/false), false);
    }
    expect("shown + failing block runs the pass", extract_failure_key(true, false), true);
    expect("a passing block never runs it", extract_failure_key(true, true), false);

    // The seeds are named by the engine's own key hash (the old comments had
    // the first two swapped).
    for (const SkipSeed& s : kSkipSeeds) {
        char what[160];
        std::snprintf(what, sizeof what, "murmur3_32(%s) == 0x%08x", s.name, s.hash);
        expect(what, murmur3_32(s.name, (uint32_t)std::strlen(s.name)) == s.hash, true);
    }
    expect("seed_for finds a seed", seed_for(0xcce24e04u) != nullptr, true);
    expect("an unknown hash is learned", seed_for(0x12345678u) == nullptr, true);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
