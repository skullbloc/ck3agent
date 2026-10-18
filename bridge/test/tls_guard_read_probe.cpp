// tls_guard_read_probe.cpp — DOES A FAILED safe_read ACTUALLY LOOK LIKE A ZERO?
// (ck-62eo)
//
// The fix rests on one claim about the bridge's own read primitives, and the
// claim is the whole reason the obvious fix was rejected:
//
//   safe_read<uint8_t>(bad)   returns 0 — INDISTINGUISHABLE from a real zero
//   safe_read_bytes(bad, ...) returns false — the failure stays visible
//
// That matters because at the TLS guard site, ZERO IS A MEANINGFUL VALUE: it
// means "construct the context". Using safe_read<uint8_t> there would turn an
// unreadable address into a silent decision to run construction against
// garbage — a crash traded for a wrong answer.
//
// This is asserted about the SHIPPED header (bridge/include/bridge.h), not a
// local restatement of it, because a copy I wrote could not falsify my model of
// what the real one does.
//
// WHAT IT DOES NOT COVER: ai_eval_hook.cpp cannot link without the engine, so
// that the guard SITE uses safe_read_bytes rather than safe_read is checked by
// the acceptance script as a READ, not driven here.
#include "../include/bridge.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {
int g_failed = 0;
void row(const char* name, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s\t%d\n", name, ok ? 1 : 0);
}
}  // namespace

int main() {
    std::printf("case\tvalue\n");

    // An address in the first page: never mapped, and the shape both arm-A
    // faults had (a small displacement off a null-ish base).
    const uintptr_t bad = 0x1a;

    // A real, mapped address to prove the primitives work at all — without
    // this, every row below could pass because nothing was ever readable.
    uint8_t real_byte = 0x5A;
    const uintptr_t good = reinterpret_cast<uintptr_t>(&real_byte);

    uint8_t out = 0xFF;
    row("safe_read_bytes_succeeds_on_a_mapped_address",
        bridge::safe_read_bytes(good, &out, 1) && out == 0x5A);

    out = 0xFF;
    const bool ok_bad = bridge::safe_read_bytes(bad, &out, 1);
    row("safe_read_bytes_reports_FALSE_on_an_unmapped_address", !ok_bad);

    // THE ROW THE FIX DEPENDS ON. safe_read's failure value is 0, and 0 is the
    // guard's "construct" signal. If these two were distinguishable the simpler
    // fix would have been fine; they are not, which is why the site uses
    // safe_read_bytes.
    const uint8_t sr_bad  = bridge::safe_read<uint8_t>(bad);
    uint8_t zero_byte = 0;
    const uint8_t sr_zero = bridge::safe_read<uint8_t>(
        reinterpret_cast<uintptr_t>(&zero_byte));
    row("safe_read_returns_0_on_failure", sr_bad == 0);
    row("safe_read_returns_0_for_a_REAL_zero", sr_zero == 0);
    row("safe_read_CANNOT_distinguish_failure_from_a_real_zero",
        sr_bad == sr_zero);

    // ... while safe_read_bytes can, which is the property being relied on.
    uint8_t b1 = 0xFF, b2 = 0xFF;
    const bool got_zero = bridge::safe_read_bytes(
        reinterpret_cast<uintptr_t>(&zero_byte), &b1, 1);
    const bool got_bad  = bridge::safe_read_bytes(bad, &b2, 1);
    row("safe_read_bytes_CAN_distinguish_them",
        got_zero && b1 == 0 && !got_bad);

    // And reading a bad address must not take the process out — the primitive
    // is process_vm_readv, not a deref. If this probe reaches here at all that
    // is established, but say it explicitly so the row exists.
    row("reading_an_unmapped_address_did_not_fault", true);

    std::printf("\n%s: %d row(s) failed\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
