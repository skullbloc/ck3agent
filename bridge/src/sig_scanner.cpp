// CK3 Signature Scanner — Cross-Patch Offset Recovery
// ====================================================
//
// Scans the loaded binary's .text section for byte patterns defined in signatures.h.
// Integrates with runtime_resolver.cpp to provide signature-based offset discovery.
//
// The scanner uses a two-pass approach:
//   1. Quick scan: find first non-wildcard byte, memchr for fast skip
//   2. Full match: verify all pattern bytes against mask
//
// After scanning, results are cross-validated against hardcoded offsets.
// Mismatches indicate the binary has been patched (expected behavior).

#include "signatures.h"
#include "runtime_resolver.h"
#include <cstring>
#include <cstdio>
#include <ctime>

// Bridge logging
namespace bridge { extern FILE* g_logfile; }
#define SLOG(fmt, ...) do { \
    if (bridge::g_logfile) { fprintf(bridge::g_logfile, "[sig-scan] " fmt "\n", ##__VA_ARGS__); fflush(bridge::g_logfile); } \
} while(0)

namespace ck3_signatures {

// ============================================================================
// Core Pattern Matcher
// ============================================================================

// Match a byte pattern at a specific location
static inline bool pattern_matches(const uint8_t* data, const uint8_t* pattern,
                                     const uint8_t* mask, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if ((data[i] & mask[i]) != (pattern[i] & mask[i]))
            return false;
    }
    return true;
}

// Find the first non-wildcard byte in a pattern (for fast skip optimization)
static inline size_t find_first_fixed(const uint8_t* mask, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (mask[i] == 0xFF) return i;
    }
    return 0; // shouldn't happen — all wildcards would be useless
}

// ============================================================================
// Single Signature Scanner
// ============================================================================

uintptr_t scan_single_signature(
    const uint8_t* text_start,
    size_t text_size,
    uintptr_t text_base,
    const SignatureEntry* sig,
    int* count_out)
{
    if (!sig || !sig->pattern || !sig->mask || sig->length == 0)
        return 0;

    if (text_size < sig->length)
        return 0;

    // Find first fixed byte for fast scanning
    size_t first_fixed = find_first_fixed(sig->mask, sig->length);
    uint8_t first_byte = sig->pattern[first_fixed];

    const uint8_t* scan_end = text_start + text_size - sig->length;
    uintptr_t first_match = 0;
    int matches = 0;

    for (const uint8_t* p = text_start; p <= scan_end; p++) {
        // Quick check on first fixed byte
        if (p[first_fixed] != first_byte)
            continue;

        // Full pattern check
        if (pattern_matches(p, sig->pattern, sig->mask, sig->length)) {
            uintptr_t addr = text_base + (p - text_start) + sig->offset_adjust;
            if (matches == 0)
                first_match = addr;
            matches++;

            // If we've found more than 1, the signature is ambiguous
            // Keep scanning to get total count but cap at 100
            if (matches > 100) break;
        }
    }

    if (count_out)
        *count_out = matches;

    return first_match;
}

// ============================================================================
// Batch Scanner
// ============================================================================

int scan_all_signatures(
    const uint8_t* text_start,
    size_t text_size,
    uintptr_t text_base,
    ScanResult* results,
    int max_results)
{
    int count = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < NUM_SIGNATURES && count < max_results; i++) {
        const auto& sig = sig_database[i];

        int match_count = 0;
        uintptr_t addr = scan_single_signature(
            text_start, text_size, text_base, &sig, &match_count);

        auto& result = results[count++];
        result.name = sig.name;
        result.address = (match_count == 1) ? addr : 0;  // only accept unique matches
        result.confidence = (match_count == 1) ? 100 : (match_count > 1 ? 50 : 0);
        result.match_count = match_count;
        result.type = sig.type;

        if (match_count == 1) {
            SLOG("  FOUND: %-35s = 0x%lx (unique)", sig.name, addr - text_base);
        } else if (match_count > 1) {
            SLOG("  AMBIG: %-35s = %d matches (first at 0x%lx) — NEEDS LONGER SIGNATURE",
                 sig.name, match_count, addr - text_base);
        } else {
            SLOG("  MISS:  %-35s = NOT FOUND", sig.name);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    int found = 0, ambig = 0, missed = 0;
    for (int i = 0; i < count; i++) {
        if (results[i].confidence == 100) found++;
        else if (results[i].confidence == 50) ambig++;
        else missed++;
    }

    SLOG("=== Signature Scan Summary ===");
    SLOG("  Total: %d  Found: %d  Ambiguous: %d  Missed: %d  Time: %.1f ms",
         count, found, ambig, missed, elapsed_ms);
    SLOG("  Recovery rate: %d/%d (%.0f%%)", found, count, 100.0 * found / count);

    return count;
}

// ============================================================================
// Integration with RuntimeResolver
// ============================================================================

int resolve_signatures_v2(const ck3_resolver::ResolverContext* ctx,
                           ck3_resolver::ResolvedOffsets* out)
{
    if (!ctx->regions_valid) return 0;

    const uint8_t* text_start = reinterpret_cast<const uint8_t*>(ctx->text.start);
    size_t text_size = ctx->text.size();
    uintptr_t text_base = ctx->text.start;

    SLOG("Starting v2 signature scan (%zu MB .text region)", text_size >> 20);

    ScanResult results[NUM_SIGNATURES + 1];
    int count = scan_all_signatures(text_start, text_size, text_base, results, NUM_SIGNATURES + 1);

    int resolved = 0;

    for (int i = 0; i < count; i++) {
        const auto& result = results[i];
        if (result.confidence != 100) continue;

        // Find the corresponding database entry to get result_offset
        const SignatureEntry* sig = nullptr;
        for (int j = 0; j < NUM_SIGNATURES; j++) {
            if (sig_database[j].name == result.name) {
                sig = &sig_database[j];
                break;
            }
        }
        if (!sig) continue;

        // Store the discovered offset (relative to binary base)
        uintptr_t offset = result.address - ctx->base_addr;

        // Only store in fields that are actual tracked offsets (not struct-verify-only)
        if (sig->result_offset > 0 && sig->result_offset < sizeof(ck3_resolver::ResolvedOffsets)) {
            uintptr_t* target = reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uint8_t*>(out) + sig->result_offset);

            // Check if this would override a value already found by RTTI
            uintptr_t existing = *target;
            if (existing != 0 && existing != offset) {
                SLOG("  CONFLICT: %s sig=0x%lx vs existing=0x%lx (delta=%+ld)",
                     result.name, offset, existing, (long)(offset - existing));
                // Keep the existing value (RTTI is more reliable than signatures)
            } else {
                *target = offset;
                resolved++;
            }
        }
    }

    SLOG("Signature v2: resolved %d new offsets", resolved);
    return resolved;
}

// ============================================================================
// Signature Extraction Helper
// ============================================================================
// This function reads bytes from the loaded binary at known offsets and generates
// signature patterns with automatic wildcarding of RIP-relative displacements.
// Useful for generating signatures for new functions.

struct ExtractedSig {
    char name[64];
    uint8_t pattern[32];
    uint8_t mask[32];
    int length;
    uintptr_t source_offset;    // offset from binary base where bytes were read
};

// Heuristic: detect RIP-relative addressing in x86-64 instruction stream
// Returns true if bytes at position 'pos' are part of a RIP-relative disp32
[[maybe_unused]]
static bool is_rip_disp32(const uint8_t* code, int pos, int /*total_len*/) {
    // Walk backward from pos to find the instruction start
    // Look for common RIP-relative patterns:
    // - LEA reg, [rip+disp32]:  48 8d XX disp32  (REX.W + 8d + ModRM(mod=00,rm=5))
    // - MOV reg, [rip+disp32]:  48 8b XX disp32
    // - MOV [rip+disp32], reg:  48 89 XX disp32
    // - CMP byte [rip+disp32]:  80 3d disp32 imm8
    // - MOV eax, [rip+disp32]:  8b 05 disp32

    // Check if pos is within a 4-byte displacement after a RIP-relative ModRM
    for (int start = pos - 2; start >= 0 && start >= pos - 4; start--) {
        int modrm_pos = start;
        uint8_t modrm = code[modrm_pos];

        // ModRM with mod=00, rm=101 means RIP-relative
        if ((modrm & 0xC7) != 0x05) continue;

        // The displacement starts right after ModRM
        int disp_start = modrm_pos + 1;
        if (pos >= disp_start && pos < disp_start + 4)
            return true;
    }
    return false;
}

int extract_signatures_from_offsets(
    const uint8_t* binary_base,
    const uintptr_t* offsets,
    const char** names,
    int num_offsets,
    ExtractedSig* out_sigs,
    int max_sigs)
{
    int count = 0;
    for (int i = 0; i < num_offsets && count < max_sigs; i++) {
        uintptr_t offset = offsets[i];
        if (offset == 0) continue;

        const uint8_t* code = binary_base + offset;
        auto& sig = out_sigs[count];

        strncpy(sig.name, names[i], sizeof(sig.name) - 1);
        sig.name[sizeof(sig.name) - 1] = '\0';
        sig.source_offset = offset;
        sig.length = 20; // default 20 bytes

        // Copy raw bytes
        memcpy(sig.pattern, code, sig.length);

        // Build mask: default all fixed
        memset(sig.mask, 0xFF, sig.length);

        // Auto-wildcard RIP-relative displacements and CALL/JMP rel32 targets
        for (int j = 0; j < sig.length; j++) {
            // CALL rel32: E8 xx xx xx xx
            if (code[j] == 0xE8 && j + 4 < sig.length) {
                sig.mask[j+1] = 0x00;
                sig.mask[j+2] = 0x00;
                sig.mask[j+3] = 0x00;
                sig.mask[j+4] = 0x00;
                j += 4;
                continue;
            }
            // JMP rel32: E9 xx xx xx xx
            if (code[j] == 0xE9 && j + 4 < sig.length) {
                sig.mask[j+1] = 0x00;
                sig.mask[j+2] = 0x00;
                sig.mask[j+3] = 0x00;
                sig.mask[j+4] = 0x00;
                j += 4;
                continue;
            }
            // Short jump: 7x xx (conditional), EB xx (unconditional)
            if ((code[j] >= 0x70 && code[j] <= 0x7F) || code[j] == 0xEB) {
                if (j + 1 < sig.length)
                    sig.mask[j+1] = 0x00;
                j += 1;
                continue;
            }
            // JCC rel32: 0F 8x xx xx xx xx
            if (code[j] == 0x0F && j + 5 < sig.length &&
                code[j+1] >= 0x80 && code[j+1] <= 0x8F) {
                sig.mask[j+2] = 0x00;
                sig.mask[j+3] = 0x00;
                sig.mask[j+4] = 0x00;
                sig.mask[j+5] = 0x00;
                j += 5;
                continue;
            }
            // RIP-relative disp32 detection (after MOV/LEA/CMP with ModRM rm=5)
            // REX prefix (0x40-0x4F) + opcode + ModRM where rm=5
            if (j + 6 < sig.length) {
                int rex = ((code[j] & 0xF0) == 0x40) ? 1 : 0;
                if (j + rex + 5 < sig.length) {
                    uint8_t opcode = code[j + rex];
                    if (j + rex + 1 < sig.length) {
                        uint8_t modrm = code[j + rex + 1];
                        if ((modrm & 0xC7) == 0x05) {
                            // This is a RIP-relative instruction
                            if (opcode == 0x8D || opcode == 0x8B || opcode == 0x89 ||
                                opcode == 0x3B || opcode == 0x39 || opcode == 0x63 ||
                                opcode == 0x8A || opcode == 0x88) {
                                // Wildcard the 4-byte displacement
                                int disp_start = j + rex + 2;
                                for (int k = 0; k < 4 && disp_start + k < sig.length; k++)
                                    sig.mask[disp_start + k] = 0x00;
                                j = disp_start + 3;
                                continue;
                            }
                        }
                    }
                }
            }
            // CMP byte [rip+disp32], imm8: 80 3D disp32 imm8
            if (code[j] == 0x80 && j + 6 < sig.length && code[j+1] == 0x3D) {
                sig.mask[j+2] = 0x00;
                sig.mask[j+3] = 0x00;
                sig.mask[j+4] = 0x00;
                sig.mask[j+5] = 0x00;
                j += 6;
                continue;
            }
        }

        count++;
    }
    return count;
}

// ============================================================================
// Debug: Print a signature in C++ source format
// ============================================================================

void print_signature_cpp(FILE* out, const ExtractedSig* sig) {
    fprintf(out, "// %s (extracted from offset 0x%lx)\n", sig->name, sig->source_offset);
    fprintf(out, "static const uint8_t PAT_%s[] = {\n    ", sig->name);
    for (int i = 0; i < sig->length; i++) {
        fprintf(out, "0x%02X", sig->pattern[i]);
        if (i < sig->length - 1) fprintf(out, ", ");
        if ((i + 1) % 16 == 0 && i < sig->length - 1) fprintf(out, "\n    ");
    }
    fprintf(out, "\n};\n");

    fprintf(out, "static const uint8_t MSK_%s[] = {\n    ", sig->name);
    for (int i = 0; i < sig->length; i++) {
        fprintf(out, "0x%02X", sig->mask[i]);
        if (i < sig->length - 1) fprintf(out, ", ");
        if ((i + 1) % 16 == 0 && i < sig->length - 1) fprintf(out, "\n    ");
    }
    fprintf(out, "\n};\n\n");
}

} // namespace ck3_signatures
