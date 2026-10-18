// ck-bl15 — does the SHIPPED safe_read_bytes actually distinguish the controller
// pointers do_set_character_ai must tell apart?
//
// The fix claims a specific mechanism: probing ONE BYTE at ctrl +
// ck3::ai_ctrl::AI_ENABLED — the very byte AI_CHAR_TOGGLE stores to — answers
// "would that store fault" without faulting.
//
// THIS PROBE INCLUDES bridge.h AND ck3_offsets.h AND CALLS THE SHIPPED
// SYMBOLS. It holds no copy of the expression and no mirrored offset, so a
// change to bridge::safe_read_bytes or to ck3::ai_ctrl::AI_ENABLED reaches
// these rows. An earlier revision claimed exactly this while defining its own
// local copy of both — the copy was faithful, so every row was true of that
// tree and the GUARANTEE was still absent. A restatement of an expression
// cannot fail when the original changes, which is the whole value of a probe.
// Caught by ck-t5bc reading the claim against the code.
//
// WHAT THIS ESTABLISHES: that the predicate separates the three pointer classes.
// WHAT IT DOES NOT: that the pointer arm B saw was any of them, or that a
// readable +0x28 means a LIVE controller. A freed-then-reused object reads
// fine. This is a mappedness oracle, and the fix claims nothing more.
#include "bridge.h"
#include "ck3_offsets.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

static int failures = 0;
static void row(const char* name, bool ok, const char* detail) {
    printf("%-46s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) failures++;
}

// The predicate under test, spelled exactly as do_set_character_ai spells it.
static bool probe_field(void* ctrl) {
    uint8_t b = 0;
    return ctrl && bridge::safe_read_bytes(
        reinterpret_cast<uintptr_t>(ctrl) + ck3::ai_ctrl::AI_ENABLED, &b, 1);
}

int main() {
    const long pg = sysconf(_SC_PAGESIZE);

    // The offset is READ FROM THE HEADER, not mirrored here. Printed so a
    // reader can see which value the rows below were actually taken against.
    printf("using ck3::ai_ctrl::AI_ENABLED = 0x%lx (from re/offsets/ck3_offsets.h)\n\n",
           static_cast<unsigned long>(ck3::ai_ctrl::AI_ENABLED));

    // 1. A fully mapped object: the toggle's store would land. Must be ACCEPTED,
    //    or the guard refuses every legitimate call and the handler is dead.
    void* good = calloc(1, 0x100);
    row("mapped_controller_accepted", probe_field(good), "calloc'd 0x100 object");

    // 2. Non-null but unmapped — arm B's case, four byte-identical faults.
    void* bad = reinterpret_cast<void*>(0xdead0000UL);
    row("unmapped_controller_refused", !probe_field(bad), "0xdead0000");

    // 3. THE DISCRIMINATOR. Base is mapped, +0x28 is not. A guard that checked
    //    the POINTER (is_mapped(ctrl), or a read at +0) passes this and still
    //    faults on the store. Only probing the FIELD catches it.
    uint8_t* page = static_cast<uint8_t*>(
        mmap(nullptr, pg * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (page == MAP_FAILED) { printf("NOT EVALUABLE: mmap failed\n"); return 3; }
    munmap(page + pg, pg);                       // leave page 1 mapped, page 2 not
    void* edge = page + pg - 0x10;               // +0x00 readable, +AI_ENABLED is not
    uint8_t scratch = 0;
    const bool base_readable = bridge::safe_read_bytes(reinterpret_cast<uintptr_t>(edge), &scratch, 1);
    row("straddling_base_is_readable", base_readable,
        "precondition: a pointer-only check WOULD pass this");
    row("straddling_field_refused", !probe_field(edge),
        "base mapped, +0x28 unmapped -> store would fault");

    // 4. Null keeps its own exit. The handler must NOT report null as the new
    //    refusal — it has said "no AI controller" since before this fix.
    row("null_controller_refused", !probe_field(nullptr), "nullptr");

    free(good);
    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
