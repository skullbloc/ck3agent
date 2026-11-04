// diag_dtor.h — the destructor of the loud evaluator's 0xD8 diagnostic record, as
// the ENGINE names it.
//
// WHY THIS EXISTS (L20-D, smoke17 B3):
//
// Three bridge paths harvest a requirement loc-key from a failing trigger block:
// EVAL_WITH_FAILURES allocates a 0xD8 diag record, the bridge reads two strings out
// of it, then destroys it with DIAG_OBJECT_DTOR and frees it. The header carried
// DIAG_OBJECT_DTOR = 0x04a5b060 (an arithmetic slip, image VA minus 0x1000000), which
// is the middle of an unrelated sort helper. Every failing decision eval therefore
// jumped into that helper, which wrote into the caller's frame and SIGSEGVed
// (smoke17 bridge.log: rip_off 0x4a5b0d3 inside FUN_04b5af30), and the server could
// only answer decision_eval_faulted.
//
// The engine itself says which function destroys the record: EVAL_WITH_FAILURES
// replaces a previous record with `call DIAG_OBJECT_DTOR; call operator delete`, and
// the first call is an `e8 rel32` at EVAL_WITH_FAILURES + EVAL_WITH_FAILURES_DTOR_CALL.
// So the bridge reads that instruction at runtime and calls the header's dtor only
// when the engine's own call names the same function. On disagreement (a wrong
// header value, a patched binary, a Windows build where the RVA is poisoned) it
// REFUSES: the caller skips the extraction rather than jump into unknown code.
//
// Engine-free (no bridge globals) so a unit test can drive it on the engine's bytes.
#pragma once

#include "ck3_offsets.h"

#include <cstdint>
#include <cstring>

namespace bridge {

// Decode a `call rel32` (opcode E8) at image offset insn_off. False for any other
// opcode. The target is an image offset like every header value.
inline bool decode_call_rel32(const uint8_t insn[5], uint64_t insn_off, uint64_t* target_off) {
    if (!insn || !target_off || insn[0] != 0xE8) return false;
    int32_t rel = 0;
    std::memcpy(&rel, insn + 1, sizeof(rel));
    *target_off = insn_off + 5 + static_cast<int64_t>(rel);
    return true;
}

// The call at insn_off names `expected`: returns expected, else 0 (refusal).
inline uint64_t witnessed_call_target(const uint8_t insn[5], uint64_t insn_off, uint64_t expected) {
    uint64_t t = 0;
    if (!decode_call_rel32(insn, insn_off, &t)) return 0;
    return t == expected ? t : 0;
}

// Image offset of the diag dtor if the engine's call site agrees with the header, else 0.
inline uint64_t witnessed_diag_dtor_offset(const uint8_t site[5]) {
    return witnessed_call_target(
        site,
        ck3::trigger_eval_funcs::EVAL_WITH_FAILURES + ck3::trigger_eval_funcs::EVAL_WITH_FAILURES_DTOR_CALL,
        ck3::trigger_eval_funcs::DIAG_OBJECT_DTOR);
}

// Runtime address of the diag dtor, or 0 when it cannot be witnessed. mapped(addr, len)
// says whether [addr, addr+len) is readable (the bridge passes is_mapped_range).
template <class MappedRange>
inline uintptr_t resolve_diag_dtor(uintptr_t base, MappedRange mapped) {
    const uintptr_t site = base + ck3::trigger_eval_funcs::EVAL_WITH_FAILURES
                                + ck3::trigger_eval_funcs::EVAL_WITH_FAILURES_DTOR_CALL;
    if (!mapped(site, 5)) return 0;
    uint8_t insn[5];
    std::memcpy(insn, reinterpret_cast<const void*>(site), sizeof(insn));
    const uint64_t off = witnessed_diag_dtor_offset(insn);
    return off ? base + static_cast<uintptr_t>(off) : 0;
}

}  // namespace bridge
