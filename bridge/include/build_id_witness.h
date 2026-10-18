// build_id_witness.h — WHAT THE BRIDGE ACTUALLY SAW, and under what rule.
//
// Bead ck-nkyc, leg (e). The offsets in ck3_offsets.h are valid against exactly
// ONE CK3 binary (e61ff2642f07094defd140fcc98e078d42541275), and until this
// header existed NOTHING IN A SEALED BUNDLE RECORDED WHICH BINARY ACTUALLY RAN.
//
// Two separate defects it closes, and they are not the same defect:
//
//  1. THE OBSERVED VALUE WAS THROWN AWAY — AND THE CHECK NEVER RAN AT ALL.
//     runtime_resolver.cpp's check_build_id() reads the running binary's real
//     ELF BuildID via dl_iterate_phdr and sent it to a log line, returning only
//     an enum. WORSE, and found while wiring this: its only caller is
//     run_resolver(), which NOTHING in this library invokes — only the three
//     Zydis discovery helpers are used, which is visible in every real
//     bridge.log (three discovery lines, no resolver banner, no "ELF BuildID"
//     line, ever). So the check was dead code wearing a live check's clothes.
//     `observe_build_id()` is now called from bridge_init()'s verify-the-binary
//     step, which is where the question already belonged. The
//     handshake meanwhile sent ck3_version::BUILD_ID_HEX under the name
//     "binary_build_id": A FIELD WHOSE NAME ASSERTS AN OBSERVATION, CARRYING A
//     COMPILE-TIME CONSTANT. Comparing that constant against the tree's own
//     offset_version.h would be a consistency check wearing a provenance
//     check's clothes. So the OBSERVED value is retained here and reported as a
//     NEW field, `observed_build_id`. `binary_build_id` is NOT renamed on the
//     wire — renaming a wire field is its own hazard — it is DOCUMENTED as the
//     expected constant.
//
//  2. THE MANDATE HAD NO WITNESS. On mismatch the resolver aborts only under
//     CK3_STRICT_BUILD_ID, whose shipped CMake default is OFF and which the
//     environment overrides IN BOTH DIRECTIONS. So a wrong CK3 version could
//     run to horizon with a warning in a log nobody reads. Making strictness
//     "mandatory for registered episodes" fixes nothing on its own, because
//     nothing in the bundle would record whether it was in force: a rule
//     enforced by whoever launched the episode, recorded nowhere, unfalsifiable
//     afterwards.
//
//     A MANDATE THAT LEAVES NO TRACE IN THE EVIDENCE IS A MANDATE THE EVIDENCE
//     CANNOT BE AUDITED AGAINST.
//
// WHY THE BRIDGE IS THE RIGHT WITNESS. It is an LD_PRELOAD inside the game
// process, so its getenv() IS the game process's environment. A LAUNCHER
// READING ITS OWN ENVIRONMENT REPORTS THE VALUE IT WISHED FOR — the same
// distinction ck-zgh4 drew when it read /proc/<pid>/environ rather than the
// launcher's shell.
//
// AND IT RECORDS THE ATTEMPT, NOT ONLY THE OUTCOME. Modelled on §7.1's
// pipeline.cpp fields (exclusion_unarmed_reason / ignored_x_days_env /
// refused_exclusion_fraction_env), which publish OVERRIDE ATTEMPTS THAT WERE
// REFUSED OR IGNORED. A witness that captures the rejected alternatives
// distinguishes "someone tried and the bridge declined" from "nobody tried",
// which an in-force value alone cannot. SILENCE MUST NOT READ THE SAME AS
// COMPLIANCE.
//
// NOTHING HERE CHANGES THE BRIDGE'S BEHAVIOUR. The resolver still aborts, or
// warns and continues, exactly as before. The REFUSAL lives in the driver
// (scripts/bridge_provenance.py), at episode start, where a mismatch costs one
// second instead of ~2.2 h of node time.

#pragma once

#include <string>

namespace bridge {

// The bridge's own verdict on the BuildID it went looking for. `UNCHECKED` is a
// real state and not a placeholder: run_resolver() is where check_build_id() is
// called, and a bridge whose resolver never ran has not passed the check — it
// has not taken it. The driver treats UNCHECKED as a FAILURE for the same
// reason it treats NOT_FOUND as one: an unasked question is not a yes.
enum class BuildIdCheck { UNCHECKED, MATCH, MISMATCH, NOT_FOUND };

const char* build_id_check_string(BuildIdCheck c);

struct BuildIdWitness {
    // What dl_iterate_phdr FOUND in the loaded binary, lowercase hex. Empty
    // when nothing was found or the check never ran.
    std::string observed_hex;
    BuildIdCheck status = BuildIdCheck::UNCHECKED;

    // The rule that was actually in force in THIS process, at the moment the
    // check ran.
    bool strict_effective = false;
    // The compile-time default (CMake option CK3_STRICT_BUILD_ID, shipped OFF).
    // Kept separate from the effective value so a bundle can tell a binary
    // built strict from one made strict at launch.
    bool strict_compiled = false;
    // CK3_STRICT_BUILD_ID as the GAME PROCESS held it, verbatim. Empty when
    // unset.
    std::string strict_env;
    // An attempt to turn strictness OFF that was seen in the environment —
    // recorded whether or not it took effect. True only when the env var was
    // set to something that disables strictness; a redundant "=1" beside a
    // strict-compiled binary is not an attempt to loosen anything.
    bool loosen_attempted = false;
};

// Process-wide, written once by check_build_id() during resolver init and read
// from the IPC thread. Written before the socket exists, so no lock is needed
// for the write; reads are of a struct whose fields are only ever set once.
const BuildIdWitness& build_id_witness();
void set_build_id_witness(const BuildIdWitness& w);

// Fill the strictness half from the CURRENT PROCESS's compile-time define and
// environment. Called from check_build_id() BEFORE the observation, so the
// witness exists on every path — including NOT_FOUND, where the old code
// resolved nothing at all because it only asked about strictness on mismatch.
void resolve_build_id_strictness(BuildIdWitness& w);

} // namespace bridge
