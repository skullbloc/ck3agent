// test/test_diag_dtor.cpp — L20-D: the diag record dtor is the function the engine calls.
//
// THE BUG: ck3::trigger_eval_funcs::DIAG_OBJECT_DTOR was 0x04a5b060, the middle of a
// sort helper (Ghidra FUN_04b5af30). Every failing decision eval jumped there and
// SIGSEGVed (smoke17 bridge.log 24130: rip_off 0x4a5b0d3), so B3 answered
// decision_eval_faulted.
//
// THE FIXTURE is not written from a model: it is the 99 bytes of EVAL_WITH_FAILURES
// (header 0x0753ddc0) and the first 21 bytes of the real dtor (0x0595b060) copied from
// the CK3 1.18.3.1 binary, MD5 09ba163bbff47f2eb8a69692536307e6 (the build the core
// dump ck3_20260625_152901_ivar867 was taken from; its text is file-backed, so the
// dump itself does not carry it). The test checks the HEADER against the ENGINE:
//   1. the call at EVAL_WITH_FAILURES+0x3b names the header's DIAG_OBJECT_DTOR
//      (red on the pre-fix header: it names 0x0595b060, the header said 0x04a5b060);
//   2. the dtor's first store is the diag vtable 0x0bf14e30 (Ghidra image VA: the
//      decompile of FUN_05a5b060 opens `*param_1 = &UNK_0bf14e30`, the value the
//      trigger_diag comment names), a second source independent of the call site;
//   3. resolve_diag_dtor over the engine bytes returns base + the dtor;
//   4. CONTROLS that run: the pre-fix value is refused by the same check; a site
//      whose opcode or displacement is changed is refused; an unmapped site is refused.
#include "diag_dtor.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                     \
    } while (0)

// objdump -d --start-address=0x0753ddc0 --stop-address=0x0753de23 binaries/ck3
static const uint8_t kEvalWithFailures[99] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x49, 0x89, 0xd6,
    0x49, 0x89, 0xf7, 0x48, 0x8b, 0x1f, 0x48, 0x85, 0xdb, 0x75, 0x35, 0x49,
    0x89, 0xfc, 0xbf, 0xd8, 0x00, 0x00, 0x00, 0xe8, 0xcc, 0xfa, 0x0d, 0xfa,
    0x48, 0x89, 0xc3, 0x48, 0x89, 0xc7, 0xe8, 0xd1, 0xb1, 0xff, 0xff, 0x4d,
    0x8b, 0x2c, 0x24, 0x4d, 0x85, 0xed, 0x74, 0x10, 0x4c, 0x89, 0xef, 0xe8,
    0x60, 0xd2, 0x41, 0xfe, 0x4c, 0x89, 0xef, 0xe8, 0x98, 0xd5, 0x0d, 0xfa,
    0x49, 0x89, 0x1c, 0x24, 0x48, 0x89, 0xdf, 0x4c, 0x89, 0xfe, 0x4c, 0x89,
    0xf2, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0xe9, 0xed,
    0xb4, 0xff, 0xff,
};

// The dtor entry: push rbp; push r14; push rbx; mov r14,rdi;
// lea rax,[rip+0x64b9db2]; add rax,0x10; mov [rdi],rax   (the vtable store)
static const uint8_t kDtorEntry[21] = {
    0x55, 0x41, 0x56, 0x53, 0x49, 0x89, 0xfe, 0x48, 0x8d, 0x05, 0xb2, 0x9d,
    0x4b, 0x06, 0x48, 0x83, 0xc0, 0x10, 0x48, 0x89, 0x07,
};

static constexpr uint64_t kPreFixDtor = 0x04a5b060;  // the value smoke17 ran with

int main() {
    namespace te = ck3::trigger_eval_funcs;
    const uint64_t site_off = te::EVAL_WITH_FAILURES + te::EVAL_WITH_FAILURES_DTOR_CALL;
    const uint8_t* site = kEvalWithFailures + te::EVAL_WITH_FAILURES_DTOR_CALL;

    // 1. the engine's call names the header's dtor
    uint64_t target = 0;
    CHECK(bridge::decode_call_rel32(site, site_off, &target));
    CHECK(target == te::DIAG_OBJECT_DTOR);
    CHECK(bridge::witnessed_diag_dtor_offset(site) == te::DIAG_OBJECT_DTOR);
    // the operator-delete call right after it decodes to the _ZdlPv PLT thunk (0x161b3a0)
    uint64_t del = 0;
    CHECK(bridge::decode_call_rel32(site + 5 + 3, site_off + 5 + 3, &del));
    CHECK(del == 0x161b3a0);

    // 2. the dtor's lea + add 0x10 is the diag vtable (Ghidra names it at image VA)
    int32_t disp = 0;
    std::memcpy(&disp, kDtorEntry + 10, sizeof(disp));
    const uint64_t vtable_img = te::DIAG_OBJECT_DTOR + 7 + 7 + static_cast<int64_t>(disp) + 0x10;
    CHECK(vtable_img + 0x100000 == 0x0bf14e30);

    // 3. runtime resolution over the engine bytes
    const uintptr_t lo = reinterpret_cast<uintptr_t>(kEvalWithFailures);
    const uintptr_t hi = lo + sizeof(kEvalWithFailures);
    const uintptr_t base = lo - te::EVAL_WITH_FAILURES;
    auto mapped = [&](uintptr_t a, size_t n) { return a >= lo && a + n <= hi; };
    CHECK(bridge::resolve_diag_dtor(base, mapped) == base + te::DIAG_OBJECT_DTOR);

    // 4. controls (each must refuse)
    CHECK(bridge::witnessed_call_target(site, site_off, kPreFixDtor) == 0);   // the pre-fix header
    uint8_t bad[5];
    std::memcpy(bad, site, 5); bad[0] = 0xE9;                                 // jmp, not call
    CHECK(bridge::witnessed_diag_dtor_offset(bad) == 0);
    std::memcpy(bad, site, 5); bad[1] ^= 0x10;                                // another target
    CHECK(bridge::witnessed_diag_dtor_offset(bad) == 0);
    auto unmapped = [](uintptr_t, size_t) { return false; };
    CHECK(bridge::resolve_diag_dtor(base, unmapped) == 0);
    // non-discriminating control (labelled): the decoder works on any E8; it is the
    // comparison with the header that discriminates, which check 4a shows.
    CHECK(bridge::decode_call_rel32(site, site_off, &target) && target != kPreFixDtor);

    std::printf("test_diag_dtor: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
