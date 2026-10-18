// build_id_witness.cpp — storage for what the bridge OBSERVED, and under what
// rule it was observing. See build_id_witness.h for why this exists (ck-nkyc,
// leg (e)).
//
// Its own translation unit, and not a corner of runtime_resolver.cpp, for one
// reason: ipc_server.cpp has to READ it to build the handshake ack, and
// runtime_resolver.cpp is the engine-scanning TU — a host build that wants the
// witness's vocabulary should not have to drag Zydis in behind it.

#include "build_id_witness.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <cstdlib>

namespace bridge {

namespace {
// Written once during resolver init, before the IPC socket exists; read from
// the IPC thread thereafter. Not a std::atomic because the ordering that
// matters is establishment-before-publication, and the socket is the
// publication point.
//
// CK3_EARLY_INIT IS NOT DECORATION (ck-22n3). This object holds two
// std::strings, so it needs dynamic initialization, and dll_init's constructor
// attribute carries NO priority — an unprioritized _GLOBAL__sub_I for this TU
// would be placed AFTER dll_init. dll_init calls bridge_init(), which is where
// the resolver runs and where `set_build_id_witness` writes: the constructor
// would then run OVER the write, default-constructing the observed BuildID and
// the strictness away microseconds after they were recorded. That is
// init_order.h's failure shape 1 exactly, and it would present as a handshake
// reporting `unchecked` and an empty strictness env forever — a provenance
// witness silently erased, which is this bead's own defect arriving through the
// back door. bridge/test/test_init_order.cpp asserts the invariant against the
// BUILT .so and named this file when it was missing.
BuildIdWitness g_witness CK3_EARLY_INIT;
}  // namespace

const char* build_id_check_string(BuildIdCheck c) {
    switch (c) {
        case BuildIdCheck::MATCH:     return "match";
        case BuildIdCheck::MISMATCH:  return "mismatch";
        case BuildIdCheck::NOT_FOUND: return "not_found";
        case BuildIdCheck::UNCHECKED: break;
    }
    return "unchecked";
}

const BuildIdWitness& build_id_witness() { return g_witness; }
void set_build_id_witness(const BuildIdWitness& w) { g_witness = w; }

void resolve_build_id_strictness(BuildIdWitness& w) {
    bool compiled = false;
#ifdef CK3_STRICT_BUILD_ID
    compiled = true;
#endif
    w.strict_compiled  = compiled;
    w.strict_effective = compiled;

    const char* env = getenv("CK3_STRICT_BUILD_ID");
    if (!env) return;

    // Verbatim, because the bundle publishes it verbatim: "0", "false" and a
    // typo'd "yes" are three different operator actions and only the first two
    // are the documented spelling. The bridge's own rule below is UNCHANGED —
    // a leading '1' means strict — so a "yes" LOOSENS a strict-compiled build,
    // which is precisely the kind of thing a witness exists to make visible.
    // SANITISED, not escaped: this string is interpolated into the handshake
    // ack's JSON by snprintf, and an operator value containing a quote or a
    // backslash would produce an unparseable ack — a provenance field breaking
    // the very handshake that carries it. Anything outside a conservative set
    // becomes '?', and the length is capped, so the field can report a wrong
    // value but can never corrupt the frame.
    for (const char* c = env; *c && w.strict_env.size() < 64; ++c) {
        const unsigned char u = static_cast<unsigned char>(*c);
        const bool safe = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
                          (u >= 'A' && u <= 'Z') || u == '_' || u == '-' || u == '.';
        w.strict_env.push_back(safe ? static_cast<char>(u) : '?');
    }

    // UNCHANGED BEHAVIOUR, STATED: the environment overrides the compile-time
    // default in BOTH directions. This bead records that fact; it does not
    // re-rule it.
    //
    // AND THE FLAG CURRENTLY GATES NOTHING IN THE BRIDGE. Its only consumer is
    // run_resolver()'s abort, and run_resolver() has no caller — so what this
    // field reports is the ENVIRONMENT THE GAME PROCESS WAS GIVEN, not an
    // enforcement the bridge performed. The enforcement is the DRIVER's refusal
    // at episode start, which is strictly stronger: it stops the episode before
    // it mints an identity, and it says which of five legs failed. Recording
    // the setting is still the point — "strict and matched" and "lax and
    // matched" are the same bundle with different guarantees.
    w.strict_effective = (env[0] == '1');

    // §7.1's model (pipeline.cpp's ignored_/refused_ env fields): publish the
    // ATTEMPT, not only the outcome. An in-force value alone cannot tell
    // "someone tried to loosen this and it stood" from "nobody tried" — and
    // silence must not read the same as compliance.
    w.loosen_attempted = !w.strict_effective;
}

}  // namespace bridge
