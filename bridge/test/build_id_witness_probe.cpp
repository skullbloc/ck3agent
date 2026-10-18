// build_id_witness_probe.cpp — the ck-nkyc leg (e) fixture GENERATOR.
//
// The strictness half of leg (e) is a claim about what the GAME PROCESS held in
// its environment. A python model of that claim would prove the model
// self-consistent and nothing about the bridge, so the fixtures the acceptance
// judges come from the BRIDGE'S OWN CODE: this probe links the real
// src/build_id_witness.cpp and prints the witness it produces, as JSON, for the
// environment the probe was started with.
//
// TWO TARGETS FROM ONE SOURCE, the ck-k57a / speed_read_probe pattern:
//
//   build_id_witness_probe                 — the shipped rule.
//   build_id_witness_probe_prefix_control  — compiled with
//       CK3_WITNESS_PREFIX_CONTROL, which restores the PRE-FIX behaviour:
//       strictness is resolved ONLY when the BuildID mismatches, so a matching
//       or unreadable binary leaves the witness saying nothing at all. The
//       acceptance requires that control to FAIL — a witness that only appears
//       when the check fails cannot distinguish a compliant run from an
//       unexamined one, which is the whole of "silence must not read the same
//       as compliance".
//
// Usage:  build_id_witness_probe [match|mismatch|not_found]
//         CK3_STRICT_BUILD_ID is read from the probe's own environment, exactly
//         as the bridge reads it from the game's.

#include "build_id_witness.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// The shape of check_build_id()'s bookkeeping, with the dl_iterate_phdr call
// replaced by the outcome the caller names. Everything the acceptance judges —
// WHEN strictness is resolved, and what it resolves to — is the real function.
bridge::BuildIdWitness run(const char* outcome) {
    bridge::BuildIdWitness w;

#ifdef CK3_WITNESS_PREFIX_CONTROL
    // PRE-FIX: nothing is resolved up front. The old code asked about
    // strictness only inside the mismatch branch of run_resolver(), so a
    // matching binary and an unreadable one both left the witness empty.
#else
    // SHIPPED: resolved FIRST and unconditionally, so the witness exists on
    // every path — including NOT_FOUND, where the old code recorded nothing.
    bridge::resolve_build_id_strictness(w);
#endif

    if (strcmp(outcome, "not_found") == 0) {
        w.status = bridge::BuildIdCheck::NOT_FOUND;
        return w;
    }

    // The observed value, RETAINED rather than logged away into an enum.
    w.observed_hex = (strcmp(outcome, "mismatch") == 0)
        ? "0000000000000000000000000000000000000000"
        : "e61ff2642f07094defd140fcc98e078d42541275";

    if (strcmp(outcome, "mismatch") == 0) {
        w.status = bridge::BuildIdCheck::MISMATCH;
#ifdef CK3_WITNESS_PREFIX_CONTROL
        // PRE-FIX: the mismatch branch is the ONLY place the question was ever
        // asked, and its answer went into a local, not into anything a client
        // could read.
        bridge::resolve_build_id_strictness(w);
#endif
        return w;
    }

    w.status = bridge::BuildIdCheck::MATCH;
    return w;
}

void put_json_string(const std::string& v) {
    // The witness sanitises its own env string, so nothing here needs to
    // escape; printing it raw is deliberate, so a probe would EXPOSE a witness
    // that stopped sanitising rather than paper over it.
    printf("\"%s\"", v.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const char* outcome = (argc > 1) ? argv[1] : "match";
    const bridge::BuildIdWitness w = run(outcome);

    printf("{\"observed_build_id\":");
    put_json_string(w.observed_hex);
    printf(",\"build_id_status\":\"%s\"", bridge::build_id_check_string(w.status));
    printf(",\"build_id_strict_effective\":%s", w.strict_effective ? "true" : "false");
    printf(",\"build_id_strict_compiled\":%s", w.strict_compiled ? "true" : "false");
    printf(",\"build_id_strict_env\":");
    put_json_string(w.strict_env);
    printf(",\"build_id_loosen_attempted\":%s}\n",
           w.loosen_attempted ? "true" : "false");
    return 0;
}
