// F12 (cleanup 2026.11, lane l6): the TLS guard offset the bridge reads must be
// the one AI_INTERACTION_CORE compares, and the context the one it LEAs.
//
// The bytes are AI_INTERACTION_CORE's first 0x50 (ELF 0x4cc4540, binary MD5
// 09ba163bbff47f2eb8a69692536307e6). The compiled-in values in ck3_offsets.h
// must equal what the scan reads from them: one definition, checked against the
// instructions it came from.
//
// Controls that run:
//   - the old reading (guard 0x6e8, the decompile's long* index as a byte
//     offset) is NOT what the scan returns;
//   - the prologue with the `lea` cut off finds no context and reports failure,
//     so the scan cannot pass by returning a constant.
#include "tls_prologue_scan.h"
#include "ck3_offsets.h"

#include <cstdio>

static const uint8_t kPrologue[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xb8, 0x03, 0x00,
    0x00, 0x4c, 0x89, 0xc3, 0x49, 0x89, 0xce, 0x49, 0x89, 0xd7, 0x48, 0x89, 0x74, 0x24, 0x48, 0x49,
    0x89, 0xfc, 0x64, 0x80, 0x3c, 0x25, 0xc0, 0xc8, 0xff, 0xff, 0x00, 0x0f, 0x84, 0x84, 0x05, 0x00,
    0x00, 0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0xa8, 0x88, 0xc5, 0xff,
    0xff, 0x48, 0x89, 0xef, 0x4c, 0x89, 0xe6, 0x4c, 0x89, 0xf2, 0xe8, 0x71, 0x0d, 0xd8, 0x01, 0x8b,
};

static int fails = 0;
static void chk(const char* name, bool ok, unsigned long got) {
    std::printf("%s\t%d\t(0x%lx)\n", name, ok ? 1 : 0, got);
    if (!ok) fails++;
}

int main() {
    bridge::TlsPrologue p;
    const bool found = bridge::scan_tls_prologue(kPrologue, sizeof(kPrologue), &p);
    chk("scan_finds_both", found, 0);
    chk("guard_is_fs_minus_0x3740", p.guard == 0x3740, p.guard);
    chk("context_is_fs_base_minus_0x3a78", p.context == 0x3a78, p.context);
    chk("header_guard_matches_the_instructions",
        p.guard == ck3::interaction_ctx::TLS_GUARD_FS_OFFSET,
        (unsigned long)ck3::interaction_ctx::TLS_GUARD_FS_OFFSET);
    chk("header_context_matches_the_instructions",
        p.context == ck3::interaction_ctx::TLS_CONTEXT_FS_OFFSET,
        (unsigned long)ck3::interaction_ctx::TLS_CONTEXT_FS_OFFSET);
    chk("control_old_reading_0x6e8_is_not_the_guard", p.guard != 0x6e8, p.guard);

    // Cut before the lea (offset 0x3a): the mov %fs:0 is there, the lea is not.
    bridge::TlsPrologue cut;
    const bool cut_found = bridge::scan_tls_prologue(kPrologue, 0x3a, &cut);
    chk("control_without_the_lea_reports_failure", !cut_found && cut.context == 0, cut.context);

    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
