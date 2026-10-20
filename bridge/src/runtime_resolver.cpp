// Runtime Offset Resolver for CK3 Bridge DLL
// Discovers function and global addresses by scanning the loaded binary in-process.
// Uses RTTI vtable walking, byte-pattern signatures, and RIP-relative instruction tracing.

#include "runtime_resolver.h"
#include "signatures.h"
#include "ck3_offsets.h"
#include "offset_version.h"
#include "build_id_witness.h"   // ck-nkyc leg (e): retain what we OBSERVED
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#ifndef _WIN32
#include <link.h>    // dl_iterate_phdr
#include <elf.h>
#else
#include "os/os.h"
#include <windows.h>
#endif
#include <Zydis/Zydis.h>

// Bridge logging (defined in bridge_init.cpp, inside namespace bridge)
namespace bridge { extern FILE* g_logfile; }
#define RLOG(fmt, ...) do { \
    if (bridge::g_logfile) { fprintf(bridge::g_logfile, "[resolver] " fmt "\n", ##__VA_ARGS__); fflush(bridge::g_logfile); } \
} while(0)

namespace ck3_resolver {

// ============================================================================
// Memory Region Mapping
// ============================================================================

struct PhdrCallbackData {
    uintptr_t base_addr;
    ResolverContext* ctx;
    bool found;
};

#ifndef _WIN32
static int phdr_callback(struct dl_phdr_info* info, size_t, void* data) {
    auto* d = static_cast<PhdrCallbackData*>(data);

    // Find the main binary (base_addr matches)
    if (info->dlpi_addr != d->base_addr)
        return 0;

    d->found = true;
    auto* ctx = d->ctx;

    // Classify PT_LOAD segments
    MemoryRegion segments[8];
    int nseg = 0;

    for (int i = 0; i < info->dlpi_phnum && nseg < 8; i++) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;

        auto& seg = segments[nseg++];
        seg.start = info->dlpi_addr + ph.p_vaddr;
        seg.end = seg.start + ph.p_memsz;
        seg.readable = (ph.p_flags & PF_R) != 0;
        seg.writable = (ph.p_flags & PF_W) != 0;
        seg.executable = (ph.p_flags & PF_X) != 0;
    }

    // Classify: .text = R+X, .rodata = R only (largest), .data.rel.ro & .data/.bss = R+W
    MemoryRegion* biggest_ro = nullptr;
    int rw_count = 0;
    MemoryRegion* rw_segments[4] = {};

    for (int i = 0; i < nseg; i++) {
        auto& seg = segments[i];
        if (seg.executable && seg.readable) {
            ctx->text = seg;
        } else if (seg.readable && !seg.writable && !seg.executable) {
            if (!biggest_ro || seg.size() > biggest_ro->size())
                biggest_ro = &seg;
        } else if (seg.readable && seg.writable) {
            if (rw_count < 4)
                rw_segments[rw_count++] = &seg;
        }
    }

    if (biggest_ro)
        ctx->rodata = *biggest_ro;

    // First R+W segment is typically .data.rel.ro, second is .data+.bss
    if (rw_count >= 1)
        ctx->data_relro = *rw_segments[0];
    if (rw_count >= 2)
        ctx->data_bss = *rw_segments[1];
    else if (rw_count == 1)
        ctx->data_bss = *rw_segments[0];  // same segment

    ctx->regions_valid = ctx->text.size() > 0 && ctx->rodata.size() > 0;

    RLOG("Mapped segments: .text=%p-%p (%zuMB), .rodata=%p-%p (%zuMB), "
         ".data.rel.ro=%p-%p (%zuMB), .data/.bss=%p-%p (%zuMB)",
         (void*)ctx->text.start, (void*)ctx->text.end, ctx->text.size() >> 20,
         (void*)ctx->rodata.start, (void*)ctx->rodata.end, ctx->rodata.size() >> 20,
         (void*)ctx->data_relro.start, (void*)ctx->data_relro.end, ctx->data_relro.size() >> 20,
         (void*)ctx->data_bss.start, (void*)ctx->data_bss.end, ctx->data_bss.size() >> 20);

    return 1; // stop iteration
}

#endif  // !_WIN32

bool init_context(uintptr_t base_addr, ResolverContext* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->base_addr = base_addr;
#ifdef _WIN32
    // PE sections: .text (code), .rdata (vtables, RTTI COLs, strings), .data.
    // MSVC keeps vtables in .rdata, so it stands in for both the Itanium
    // .rodata and .data.rel.ro. (No caller runs the RTTI scanner on Windows:
    // it is Itanium-only; P1's anchors are pinned by identity instead.)
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_addr);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_addr + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        MemoryRegion r{base_addr + sec->VirtualAddress, base_addr + sec->VirtualAddress + sec->Misc.VirtualSize,
                       (sec->Characteristics & IMAGE_SCN_MEM_READ) != 0,
                       (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0,
                       (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0};
        if (memcmp(sec->Name, ".text", 6) == 0) ctx->text = r;
        else if (memcmp(sec->Name, ".rdata", 7) == 0) { ctx->rodata = r; ctx->data_relro = r; }
        else if (memcmp(sec->Name, ".data", 6) == 0) ctx->data_bss = r;
    }
    ctx->regions_valid = ctx->text.size() > 0 && ctx->rodata.size() > 0;
    return ctx->regions_valid;
#else

    PhdrCallbackData data = { base_addr, ctx, false };
    dl_iterate_phdr(phdr_callback, &data);

    if (!data.found) {
        RLOG("ERROR: Could not find binary with base %p in dl_iterate_phdr", (void*)base_addr);
        return false;
    }

    return ctx->regions_valid;
#endif
}

// ============================================================================
// RTTI VTable Scanner (Itanium C++ ABI)
// ============================================================================

// Find a NUL-terminated string in a memory region
static uintptr_t find_string_in_region(const MemoryRegion& region, const char* needle, size_t needle_len) {
    if (region.size() == 0 || needle_len == 0) return 0;

    const uint8_t* haystack = reinterpret_cast<const uint8_t*>(region.start);
    size_t haystack_len = region.size();

    // memmem searches for substring
    void* found = memmem(haystack, haystack_len, needle, needle_len);
    if (!found) return 0;

    return reinterpret_cast<uintptr_t>(found);
}

// Find typeinfo struct given the name string address
// Itanium ABI: typeinfo = { vtable_ptr, name_ptr [, base_info...] }
// We scan .data.rel.ro for an 8-byte pointer matching name_addr
static uintptr_t find_typeinfo(const ResolverContext* ctx, uintptr_t name_addr) {
    // Scan both .data.rel.ro and .rodata for the typeinfo pointer
    // (typeinfo can be in either, depending on linkage)
    const MemoryRegion* regions[] = { &ctx->data_relro, &ctx->rodata };

    for (const auto* region : regions) {
        if (region->size() == 0) continue;

        const uintptr_t* scan = reinterpret_cast<const uintptr_t*>(region->start);
        size_t count = region->size() / sizeof(uintptr_t);

        for (size_t i = 0; i < count; i++) {
            if (scan[i] == name_addr) {
                // typeinfo.name_ptr is at offset +8 in the struct
                // So typeinfo base is at scan[i] - 8 = &scan[i-1]
                if (i == 0) continue;
                uintptr_t typeinfo_addr = reinterpret_cast<uintptr_t>(&scan[i - 1]);

                // Validate: the word before name_ptr should be a vtable ptr
                // (pointing to __class_type_info vtable, which is in the EXTERNAL segment)
                // We just check it's non-zero and looks like a pointer
                uintptr_t vtable_ptr = scan[i - 1];
                if (vtable_ptr > 0x1000) {
                    return typeinfo_addr;
                }
            }
        }
    }
    return 0;
}

// Find vtable given typeinfo address
// Itanium ABI: vtable layout = { ..., offset_to_top(0), typeinfo_ptr, vfunc0, vfunc1, ... }
// We scan for typeinfo_ptr in .data.rel.ro, then vtable base = found + 8
static uintptr_t find_vtable(const ResolverContext* ctx, uintptr_t typeinfo_addr) {
    const MemoryRegion* regions[] = { &ctx->data_relro, &ctx->rodata };

    for (const auto* region : regions) {
        if (region->size() == 0) continue;

        const uintptr_t* scan = reinterpret_cast<const uintptr_t*>(region->start);
        size_t count = region->size() / sizeof(uintptr_t);

        for (size_t i = 1; i < count - 1; i++) {
            if (scan[i] == typeinfo_addr) {
                // Check offset_to_top (word before typeinfo ptr) is 0 for primary vtable
                if (scan[i - 1] == 0) {
                    // vtable base (vfunc0) is at scan[i+1]
                    uintptr_t vtable_base = reinterpret_cast<uintptr_t>(&scan[i + 1]);

                    // Validate: first entry should point into .text
                    uintptr_t first_vfunc = scan[i + 1];
                    if (ctx->text.contains(first_vfunc)) {
                        return vtable_base;
                    }
                }
            }
        }
    }
    return 0;
}

// RTTI target table
static const RttiTarget rtti_targets[] = {
    // CAIManager — the critical hook targets
    { "11CAIManager", 10, offsetof(ResolvedOffsets, AI_MANAGER_MAIN_TICK),
      offsetof(ResolvedOffsets, VTABLE_CAIManager), "CAIManager::MainTick (vtable[10])" },
    { "11CAIManager", 12, offsetof(ResolvedOffsets, AI_MANAGER_POST_EXECUTE),
      0, "CAIManager::PostExecute (vtable[12])" },

    // CWar
    { "4CWar", -1, 0,
      offsetof(ResolvedOffsets, VTABLE_CWar), "CWar vtable" },

    // CJominiIdler — pause system
    { "13CJominiIdler", -1, 0,
      offsetof(ResolvedOffsets, VTABLE_CJominiIdler), "CJominiIdler vtable" },

    // CActiveScheme
    { "13CActiveScheme", -1, 0,
      offsetof(ResolvedOffsets, VTABLE_CActiveScheme), "CActiveScheme vtable" },

    // CFaction
    { "8CFaction", -1, 0,
      offsetof(ResolvedOffsets, VTABLE_CFaction), "CFaction vtable" },

    // CCharacterInteractionDatabase — interaction DB singleton
    // Mangled: "34CCharacterInteractionDatabase" (length-prefixed Itanium name)
    { "34CCharacterInteractionDatabase", -1, 0,
      offsetof(ResolvedOffsets, VTABLE_CCharacterInteractionDB), "CCharacterInteractionDatabase vtable" },

    // CSchemeManager
    { "14CSchemeManager", -1, 0,
      offsetof(ResolvedOffsets, VTABLE_CSchemeManager), "CSchemeManager vtable" },
};
static constexpr int NUM_RTTI_TARGETS = sizeof(rtti_targets) / sizeof(rtti_targets[0]);

int resolve_rtti_targets(const ResolverContext* ctx, ResolvedOffsets* out) {
    int resolved = 0;

    // Cache: mangled name -> (name_addr, typeinfo_addr, vtable_addr)
    // Multiple targets may reference the same class
    struct ClassCache {
        const char* name;
        uintptr_t name_addr;
        uintptr_t typeinfo_addr;
        uintptr_t vtable_addr;
    };
    ClassCache cache[16] = {};
    int cache_count = 0;

    for (int t = 0; t < NUM_RTTI_TARGETS; t++) {
        const auto& target = rtti_targets[t];

        // Check cache
        ClassCache* cached = nullptr;
        for (int c = 0; c < cache_count; c++) {
            if (strcmp(cache[c].name, target.mangled_name) == 0) {
                cached = &cache[c];
                break;
            }
        }

        if (!cached && cache_count < 16) {
            cached = &cache[cache_count++];
            cached->name = target.mangled_name;
            cached->name_addr = 0;
            cached->typeinfo_addr = 0;
            cached->vtable_addr = 0;

            // Step 1: Find mangled name string
            size_t name_len = strlen(target.mangled_name);
            cached->name_addr = find_string_in_region(ctx->rodata, target.mangled_name, name_len + 1);

            if (!cached->name_addr) {
                // Also search in data.rel.ro (sometimes typeinfo names end up there)
                cached->name_addr = find_string_in_region(ctx->data_relro, target.mangled_name, name_len + 1);
            }

            if (cached->name_addr) {
                // Step 2: Find typeinfo
                cached->typeinfo_addr = find_typeinfo(ctx, cached->name_addr);

                if (cached->typeinfo_addr) {
                    // Step 3: Find vtable
                    cached->vtable_addr = find_vtable(ctx, cached->typeinfo_addr);
                }
            }

            if (cached->vtable_addr) {
                RLOG("RTTI: %s -> name=%p typeinfo=%p vtable=%p",
                     target.mangled_name,
                     (void*)cached->name_addr,
                     (void*)cached->typeinfo_addr,
                     (void*)cached->vtable_addr);
            } else {
                RLOG("RTTI: %s -> FAILED (name=%p, typeinfo=%p)",
                     target.mangled_name,
                     (void*)cached->name_addr,
                     (void*)cached->typeinfo_addr);
            }
        }

        if (!cached || !cached->vtable_addr) continue;

        // Store vtable address if requested
        if (target.vtable_offset > 0) {
            uintptr_t vtable_offset = cached->vtable_addr - ctx->base_addr;
            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(out) + target.vtable_offset) = vtable_offset;
        }

        // Extract vtable entry if slot >= 0
        if (target.vtable_slot >= 0 && target.result_offset > 0) {
            const uintptr_t* vtable = reinterpret_cast<const uintptr_t*>(cached->vtable_addr);
            uintptr_t func_addr = vtable[target.vtable_slot];

            if (ctx->text.contains(func_addr)) {
                uintptr_t func_offset = func_addr - ctx->base_addr;
                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(out) + target.result_offset) = func_offset;

                RLOG("  -> %s = 0x%lx (vtable[%d])", target.label, func_offset, target.vtable_slot);
                resolved++;
            } else {
                RLOG("  -> %s vtable[%d] = %p NOT in .text!", target.label, target.vtable_slot, (void*)func_addr);
            }
        }
    }

    return resolved;
}

// ============================================================================
// Signature Scanner
// ============================================================================

// MurmurHash3-32 signature: look for the distinctive constants
// c1 = 0xcc9e2d51, c2 = 0x1b873593 — these appear as immediate operands
static const uint8_t SIG_MURMUR_HASH[] = {
    0x69, 0x00, 0x51, 0x2d, 0x9e, 0xcc,  // imul reg, 0xcc9e2d51
};
static const uint8_t MASK_MURMUR_HASH[] = {
    0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Scan .text for a byte pattern with mask
static uintptr_t scan_pattern(const MemoryRegion& region, const uint8_t* pattern,
                               const uint8_t* mask, size_t len) {
    if (region.size() < len) return 0;

    const uint8_t* start = reinterpret_cast<const uint8_t*>(region.start);
    const uint8_t* end = start + region.size() - len;

    // Find first non-wildcard byte for fast skip
    size_t first_fixed = 0;
    uint8_t first_byte = 0;
    for (size_t i = 0; i < len; i++) {
        if (mask[i] == 0xFF) {
            first_fixed = i;
            first_byte = pattern[i];
            break;
        }
    }

    for (const uint8_t* p = start; p <= end; p++) {
        // Quick check on first fixed byte
        if (p[first_fixed] != first_byte) continue;

        // Full pattern match
        bool match = true;
        for (size_t i = 0; i < len; i++) {
            if ((p[i] & mask[i]) != (pattern[i] & mask[i])) {
                match = false;
                break;
            }
        }

        if (match) {
            return reinterpret_cast<uintptr_t>(p);
        }
    }

    return 0;
}

// Find MurmurHash3 by its constants, then walk backward to function start
static uintptr_t find_murmur_hash(const ResolverContext* ctx) {
    // Search for 0xcc9e2d51 constant in .text
    uintptr_t hit = scan_pattern(ctx->text, SIG_MURMUR_HASH, MASK_MURMUR_HASH, sizeof(SIG_MURMUR_HASH));
    if (!hit) return 0;

    // Walk backward to find function start (look for common prologue patterns)
    // Functions typically start with push/sub or endbr64
    const uint8_t* p = reinterpret_cast<const uint8_t*>(hit);
    for (int back = 0; back < 256; back++) {
        const uint8_t* candidate = p - back;

        // endbr64 = f3 0f 1e fa
        if (candidate[0] == 0xf3 && candidate[1] == 0x0f && candidate[2] == 0x1e && candidate[3] == 0xfa)
            return reinterpret_cast<uintptr_t>(candidate);

        // push rbp; mov rbp, rsp
        if (candidate[0] == 0x55 && candidate[1] == 0x48 && candidate[2] == 0x89 && candidate[3] == 0xe5)
            return reinterpret_cast<uintptr_t>(candidate);

        // push rbx (common for non-rbp functions)
        // We need more context to distinguish function starts; for now check alignment
        // Most functions are 16-byte aligned
        uintptr_t addr = reinterpret_cast<uintptr_t>(candidate);
        if ((addr & 0xF) == 0) {
            // Check if this looks like a function start (push or sub rsp)
            if (candidate[0] == 0x55 || candidate[0] == 0x53 ||
                (candidate[0] == 0x41 && (candidate[1] >= 0x54 && candidate[1] <= 0x57)) ||
                (candidate[0] == 0x48 && candidate[1] == 0x83 && candidate[2] == 0xec)) {
                // Verify: the instruction at hit - back must be in the function
                // (i.e., no ret/int3 between candidate and hit)
                bool has_ret = false;
                for (const uint8_t* scan = candidate; scan < p; scan++) {
                    if (*scan == 0xc3 || *scan == 0xcc) {
                        has_ret = true;
                        break;
                    }
                }
                if (!has_ret)
                    return addr;
            }
        }
    }

    // Fallback: return the hit itself (caller should validate)
    return hit;
}

// ==========================================================================
// Signature definitions for accessor functions.
// Patterns extracted from re/offsets/ck3_offsets.h targets via Ghidra MCP
// memory_read on the analyzed CK3 binary. Each pattern starts at the function
// entry; wildcard bytes mask jump displacements and RIP-relative operands.
//
// Implemented below:
//   IS_ALIVE          (12B)  cmp [rdi+0x1c8],0; sete; ret
//   IS_RULER          (12B)  cmp [rdi+0x1b8],0; setne; ret
//   STRESS_GET        (19B)  load core_data; deref +0x2F8 (32-bit)
//   DREAD_GET         (20B)  load ruler_data; deref +0x350 (64-bit)
//   PIETY_GET         (20B)  load core_data; deref +0x110 (64-bit)
//   PRESTIGE_GET      (20B)  load core_data; deref +0x130 (64-bit)
//   GET_FACTION_LIST  (29B)  load ruler_data; lea +0x120; cmovz null sentinel
//   GOLD_GET          (44B)  5-case currency switch on esi; case 0 reads +0x100
//
// Still deferred (more complex / lower priority):
//   SKILL_GET_BASE / SKILL_GET_CALC  (bounds-checked index access)
//   TRAIT_LIST_GET                   (returns struct {ptr, count})
//   BUILD_INTERACTION_CMD            (923B, MurmurHash3 constants inside)
//   COMMAND_DISPATCH_FN              (146B, references global manager)
//   WAR_SCORE_COMPUTE                (clamping [-100,+100])
//   INCOME_CALC                      (431B, modifier pipeline)
// ==========================================================================

// Scan for IS_ALIVE by its distinctive 12-byte pattern
// cmp qword [rdi+0x1c8], 0; sete al; ret
static const uint8_t SIG_IS_ALIVE[] = {
    0x48, 0x83, 0xBF, 0xC8, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x94, 0xC0, 0xC3,
};
static const uint8_t MASK_IS_ALIVE[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Scan for IS_RULER by its distinctive 12-byte pattern
// cmp qword [rdi+0x1b8], 0; setne al; ret
static const uint8_t SIG_IS_RULER[] = {
    0x48, 0x83, 0xBF, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x95, 0xC0, 0xC3,
};
static const uint8_t MASK_IS_RULER[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Scan for STRESS_GET (22B) — reads core_data+0x2F8
// mov rax,[rdi+0x1a8]; test rax,rax; je +N; mov eax,[rax+0x2F8]; ret
static const uint8_t SIG_STRESS_GET[] = {
    0x48, 0x8B, 0x87, 0xA8, 0x01, 0x00, 0x00,  // mov rax, [rdi+0x1a8]
    0x48, 0x85, 0xC0,                            // test rax, rax
    0x74, 0x00,                                   // je +N (wildcard offset)
    0x8B, 0x80, 0xF8, 0x02, 0x00, 0x00,          // mov eax, [rax+0x2F8]
};
static const uint8_t MASK_STRESS_GET[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,  // wildcard the jump offset
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Scan for DREAD_GET (23B) — reads ruler_data+0x350
// mov rax,[rdi+0x1b8]; test rax,rax; je +N; mov rax,[rax+0x350]; ret
static const uint8_t SIG_DREAD_GET[] = {
    0x48, 0x8B, 0x87, 0xB8, 0x01, 0x00, 0x00,  // mov rax, [rdi+0x1b8]
    0x48, 0x85, 0xC0,                            // test rax, rax
    0x74, 0x00,                                   // je +N (wildcard offset)
    0x48, 0x8B, 0x80, 0x50, 0x03, 0x00, 0x00,   // mov rax, [rax+0x350]
};
static const uint8_t MASK_DREAD_GET[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Scan for PIETY_GET (20B) — reads core_data+0x110 (64-bit fixed-point)
// mov rax,[rdi+0x1a8]; test rax,rax; je +N; mov rax,[rax+0x110]; ret
static const uint8_t SIG_PIETY_GET[] = {
    0x48, 0x8B, 0x87, 0xA8, 0x01, 0x00, 0x00,  // mov rax, [rdi+0x1a8]
    0x48, 0x85, 0xC0,                            // test rax, rax
    0x74, 0x00,                                   // je +N (wildcard)
    0x48, 0x8B, 0x80, 0x10, 0x01, 0x00, 0x00,   // mov rax, [rax+0x110]
};
static const uint8_t MASK_PIETY_GET[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Scan for PRESTIGE_GET (20B) — reads core_data+0x130 (64-bit fixed-point)
// mov rax,[rdi+0x1a8]; test rax,rax; je +N; mov rax,[rax+0x130]; ret
static const uint8_t SIG_PRESTIGE_GET[] = {
    0x48, 0x8B, 0x87, 0xA8, 0x01, 0x00, 0x00,  // mov rax, [rdi+0x1a8]
    0x48, 0x85, 0xC0,                            // test rax, rax
    0x74, 0x00,                                   // je +N (wildcard)
    0x48, 0x8B, 0x80, 0x30, 0x01, 0x00, 0x00,   // mov rax, [rax+0x130]
};
static const uint8_t MASK_PRESTIGE_GET[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Scan for GET_FACTION_LIST (29B) — reads *(*(char+0x1b8)+0x120) with cmov-null
// Real prologue uses lea+cmovz (not je+mov) to fall through to a sentinel.
// mov rax,[rdi+0x1b8]; test rax,rax; lea rax,[rax+0x120]; lea rcx,[rip+sentinel]
// cmovz rax,rcx; ret
static const uint8_t SIG_GET_FACTION_LIST[] = {
    0x48, 0x8B, 0x87, 0xB8, 0x01, 0x00, 0x00,  // mov rax, [rdi+0x1b8]
    0x48, 0x85, 0xC0,                            // test rax, rax
    0x48, 0x8D, 0x80, 0x20, 0x01, 0x00, 0x00,   // lea rax, [rax+0x120]
    0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00,   // lea rcx, [rip+disp32] (wildcarded)
    0x48, 0x0F, 0x44, 0xC1,                      // cmovz rax, rcx
    0xC3,                                         // ret
};
static const uint8_t MASK_GET_FACTION_LIST[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,   // wildcard the RIP-relative disp32
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
};

// Scan for GOLD_GET (44B prologue of a 220B function) — currency switch on esi
// Distinctive 5-case dispatch on currency_type with case 0 reading +0x100.
// push rbx; mov rax,[rdi+0x1a8]; test rax,rax; je rel32;
// cmp esi,4; je rel8; cmp esi,3; je rel8; test esi,esi; jne rel32;
// mov rax,[rax+0x100]; pop rbx; ret
static const uint8_t SIG_GOLD_GET[] = {
    0x53,                                         // push rbx
    0x48, 0x8B, 0x87, 0xA8, 0x01, 0x00, 0x00,    // mov rax, [rdi+0x1a8]
    0x48, 0x85, 0xC0,                              // test rax, rax
    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,            // je rel32 (wildcard disp)
    0x83, 0xFE, 0x04,                              // cmp esi, 4
    0x74, 0x00,                                    // je rel8 (wildcard)
    0x83, 0xFE, 0x03,                              // cmp esi, 3
    0x74, 0x00,                                    // je rel8 (wildcard)
    0x85, 0xF6,                                    // test esi, esi
    0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,            // jne rel32 (wildcard disp)
    0x48, 0x8B, 0x80, 0x00, 0x01, 0x00, 0x00,     // mov rax, [rax+0x100]
    0x5B,                                          // pop rbx
    0xC3,                                          // ret
};
static const uint8_t MASK_GOLD_GET[] = {
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF,
};

int resolve_signatures(const ResolverContext* ctx, ResolvedOffsets* out) {
    int resolved = 0;

    // Find MurmurHash3-32
    uintptr_t murmur = find_murmur_hash(ctx);
    if (murmur) {
        out->STRING_HASH = murmur - ctx->base_addr;
        RLOG("SIG: STRING_HASH (MurmurHash3) = 0x%lx", out->STRING_HASH);
        resolved++;
    } else {
        RLOG("SIG: STRING_HASH -> FAILED");
    }

    // Scan for IS_ALIVE (12B exact match)
    uintptr_t is_alive = scan_pattern(ctx->text, SIG_IS_ALIVE, MASK_IS_ALIVE, sizeof(SIG_IS_ALIVE));
    if (is_alive) {
        out->IS_ALIVE = is_alive - ctx->base_addr;
        RLOG("SIG: IS_ALIVE = 0x%lx", out->IS_ALIVE);
        resolved++;
    } else {
        RLOG("SIG: IS_ALIVE -> FAILED");
    }

    // Scan for IS_RULER (12B exact match)
    uintptr_t is_ruler = scan_pattern(ctx->text, SIG_IS_RULER, MASK_IS_RULER, sizeof(SIG_IS_RULER));
    if (is_ruler) {
        out->IS_RULER = is_ruler - ctx->base_addr;
        RLOG("SIG: IS_RULER = 0x%lx", out->IS_RULER);
        resolved++;
    } else {
        RLOG("SIG: IS_RULER -> FAILED");
    }

    // Scan for STRESS_GET (reads core_data+0x2F8)
    uintptr_t stress = scan_pattern(ctx->text, SIG_STRESS_GET, MASK_STRESS_GET, sizeof(SIG_STRESS_GET));
    if (stress) {
        out->STRESS_GET = stress - ctx->base_addr;
        RLOG("SIG: STRESS_GET = 0x%lx", out->STRESS_GET);
        resolved++;
    } else {
        RLOG("SIG: STRESS_GET -> FAILED");
    }

    // Scan for DREAD_GET (reads ruler_data+0x350)
    uintptr_t dread = scan_pattern(ctx->text, SIG_DREAD_GET, MASK_DREAD_GET, sizeof(SIG_DREAD_GET));
    if (dread) {
        out->DREAD_GET = dread - ctx->base_addr;
        RLOG("SIG: DREAD_GET = 0x%lx", out->DREAD_GET);
        resolved++;
    } else {
        RLOG("SIG: DREAD_GET -> FAILED");
    }

    // Scan for PIETY_GET (reads core_data+0x110)
    uintptr_t piety = scan_pattern(ctx->text, SIG_PIETY_GET, MASK_PIETY_GET, sizeof(SIG_PIETY_GET));
    if (piety) {
        out->PIETY_GET = piety - ctx->base_addr;
        RLOG("SIG: PIETY_GET = 0x%lx", out->PIETY_GET);
        resolved++;
    } else {
        RLOG("SIG: PIETY_GET -> FAILED");
    }

    // Scan for PRESTIGE_GET (reads core_data+0x130)
    uintptr_t prestige = scan_pattern(ctx->text, SIG_PRESTIGE_GET, MASK_PRESTIGE_GET, sizeof(SIG_PRESTIGE_GET));
    if (prestige) {
        out->PRESTIGE_GET = prestige - ctx->base_addr;
        RLOG("SIG: PRESTIGE_GET = 0x%lx", out->PRESTIGE_GET);
        resolved++;
    } else {
        RLOG("SIG: PRESTIGE_GET -> FAILED");
    }

    // Scan for GET_FACTION_LIST (29B with cmov-null sentinel)
    uintptr_t faction_list = scan_pattern(ctx->text, SIG_GET_FACTION_LIST, MASK_GET_FACTION_LIST,
                                          sizeof(SIG_GET_FACTION_LIST));
    if (faction_list) {
        out->GET_FACTION_LIST = faction_list - ctx->base_addr;
        RLOG("SIG: GET_FACTION_LIST = 0x%lx", out->GET_FACTION_LIST);
        resolved++;
    } else {
        RLOG("SIG: GET_FACTION_LIST -> FAILED");
    }

    // Scan for GOLD_GET (5-case currency switch prologue)
    uintptr_t gold = scan_pattern(ctx->text, SIG_GOLD_GET, MASK_GOLD_GET, sizeof(SIG_GOLD_GET));
    if (gold) {
        out->GOLD_GET = gold - ctx->base_addr;
        RLOG("SIG: GOLD_GET = 0x%lx", out->GOLD_GET);
        resolved++;
    } else {
        RLOG("SIG: GOLD_GET -> FAILED");
    }

    // NOTE: SKILL_GET_BASE, TRAIT_LIST_GET, BUILD_INTERACTION_CMD, COMMAND_DISPATCH_FN,
    // WAR_SCORE_COMPUTE and INCOME_CALC are scanned by the v2 database-driven scanner
    // (resolve_signatures_v2 over sig_database[] in signatures.h), which runs immediately
    // after this v1 pass in run_resolver(). They are intentionally not duplicated here.

    return resolved;
}

// ============================================================================
// RIP-Relative Anchor Tracing
// ============================================================================

// Decode x86-64 instructions looking for RIP-relative addressing
// Returns the Nth RIP-relative target address found within scan_bytes
static uintptr_t trace_nth_rip_ref(uintptr_t func_addr, int ordinal, int scan_bytes,
                                    const ResolverContext* ctx) {
    if (!ctx->text.contains(func_addr)) return 0;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(func_addr);
    const uint8_t* end = p + scan_bytes;
    if (reinterpret_cast<uintptr_t>(end) > ctx->text.end)
        end = reinterpret_cast<const uint8_t*>(ctx->text.end);

    int found = 0;

    while (p < end - 6) {
        // Check for RIP-relative addressing (ModR/M byte with mod=00, rm=101)
        // Common patterns:
        // LEA reg, [rip+disp32]: 48 8d XX XX XX XX XX (REX.W + 8d + ModRM + disp32)
        // MOV reg, [rip+disp32]: 48 8b XX XX XX XX XX (REX.W + 8b + ModRM + disp32)
        // MOV [rip+disp32], reg: 48 89 XX XX XX XX XX
        // CMP [rip+disp32], imm: 48 83 3d XX XX XX XX imm8

        bool is_rip_relative = false;
        int insn_len = 0;
        int32_t disp32 = 0;

        // REX prefix (0x40-0x4F), optional
        int rex = 0;
        if ((p[0] & 0xF0) == 0x40) {
            rex = 1;
        }

        uint8_t opcode = p[rex];
        uint8_t modrm = p[rex + 1];

        // Check ModR/M: mod=00, rm=101 (RIP-relative)
        if ((modrm & 0xC7) == 0x05) {
            // LEA, MOV, CMP, etc. with RIP-relative
            if (opcode == 0x8d || opcode == 0x8b || opcode == 0x89 ||
                opcode == 0x3b || opcode == 0x39 || opcode == 0x63 ||
                opcode == 0x8d) {  // LEA with RIP-relative
                memcpy(&disp32, &p[rex + 2], 4);
                insn_len = rex + 2 + 4;
                is_rip_relative = true;
            }
        }

        // Two-byte opcode (0F xx) with RIP-relative
        if (!is_rip_relative && opcode == 0x0f && p + rex + 2 < end) {
            uint8_t opcode2 = p[rex + 1];
            modrm = p[rex + 2];
            if ((modrm & 0xC7) == 0x05) {
                // MOVZX, MOVSX, etc.
                if (opcode2 == 0xb6 || opcode2 == 0xbe || opcode2 == 0xb7 || opcode2 == 0xbf) {
                    memcpy(&disp32, &p[rex + 3], 4);
                    insn_len = rex + 3 + 4;
                    is_rip_relative = true;
                }
            }
        }

        if (is_rip_relative) {
            uintptr_t target = reinterpret_cast<uintptr_t>(p) + insn_len + disp32;

            if (found == ordinal) {
                return target;
            }
            found++;
            p += insn_len;
        } else {
            // Advance by 1 byte (crude but functional for scanning)
            p++;
        }
    }

    return 0;
}

// Anchor definitions
static const RipAnchor rip_anchors[] = {
    // CHAR_ID_TO_PTR's first RIP-relative ref -> CHARACTER_DB
    { offsetof(ResolvedOffsets, CHAR_ID_TO_PTR), 0,
      offsetof(ResolvedOffsets, CHARACTER_DB), 64, 0,
      "CHARACTER_DB (from CHAR_ID_TO_PTR)" },

    // GET_PLAYER_CHAR_ID_FN's only RIP ref -> PLAYER_CHAR_ID
    { offsetof(ResolvedOffsets, GET_PLAYER_CHAR_ID_FN), 0,
      offsetof(ResolvedOffsets, PLAYER_CHAR_ID), 32, 0,
      "PLAYER_CHAR_ID (from GET_PLAYER_CHAR_ID)" },

    // AI_MANAGER_MAIN_TICK references the AI_ENABLED_FLAG global early in its body
    // Trace first RIP-relative ref within 128 bytes to find the flag
    { offsetof(ResolvedOffsets, AI_MANAGER_MAIN_TICK), 0,
      offsetof(ResolvedOffsets, AI_ENABLED_FLAG), 128, 0,
      "AI_ENABLED_FLAG (from AI_MANAGER_MAIN_TICK)" },

    // COMMAND_DISPATCH_FN references the COMMAND_DISPATCH manager singleton
    { offsetof(ResolvedOffsets, COMMAND_DISPATCH_FN), 0,
      offsetof(ResolvedOffsets, COMMAND_DISPATCH), 64, 0,
      "COMMAND_DISPATCH (from COMMAND_DISPATCH_FN)" },

    // --- New anchors: global discovery from signature-resolved functions ---

    // GAME_IS_LOADED_FN: cmp qword [rip+GSR], 0 — first RIP ref → GAME_STATE_ROOT
    { offsetof(ResolvedOffsets, GAME_IS_LOADED_FN), 0,
      offsetof(ResolvedOffsets, GAME_STATE_ROOT), 16, 0,
      "GAME_STATE_ROOT (from GAME_IS_LOADED)" },

    // IS_MANUALLY_PAUSED_FN: lea rax, [rip+IDLER] — first RIP ref → IDLER_SINGLETON
    { offsetof(ResolvedOffsets, IS_MANUALLY_PAUSED_FN), 0,
      offsetof(ResolvedOffsets, IDLER_SINGLETON), 16, 0,
      "IDLER_SINGLETON (from IS_MANUALLY_PAUSED)" },

    // TRAIT_HAS_FLAG_FN: mov r13, [rip+TRAIT_DB] — first RIP ref → TRAIT_DB
    { offsetof(ResolvedOffsets, TRAIT_HAS_FLAG_FN), 0,
      offsetof(ResolvedOffsets, TRAIT_DB), 32, 0,
      "TRAIT_DB (from TRAIT_HAS_FLAG)" },

    // AI_LIFESTYLE_DISPATCH_FN: cmp byte [rip+toggle+3], 0 — first RIP ref → AI_TOGGLE_BASE+3
    // Adjust -3 to recover the base address of the 38-byte toggle array
    { offsetof(ResolvedOffsets, AI_LIFESTYLE_DISPATCH_FN), 0,
      offsetof(ResolvedOffsets, AI_TOGGLE_BASE), 16, -3,
      "AI_TOGGLE_BASE (from AI_LIFESTYLE_DISPATCH, adjust -3)" },
};
static constexpr int NUM_RIP_ANCHORS = sizeof(rip_anchors) / sizeof(rip_anchors[0]);

int resolve_rip_anchors(const ResolverContext* ctx, ResolvedOffsets* out) {
    int resolved = 0;

    for (int i = 0; i < NUM_RIP_ANCHORS; i++) {
        const auto& anchor = rip_anchors[i];

        // Get the source function address
        uintptr_t src_offset = *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<const uint8_t*>(out) + anchor.source_offset);

        if (src_offset == 0) {
            RLOG("RIP: %s -> SKIP (source function not resolved)", anchor.label);
            continue;
        }

        uintptr_t src_addr = ctx->base_addr + src_offset;
        uintptr_t target = trace_nth_rip_ref(src_addr, anchor.rip_ordinal, anchor.scan_bytes, ctx);

        if (target) {
            uintptr_t target_offset = target - ctx->base_addr + anchor.offset_adjust;
            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(out) + anchor.target_offset) = target_offset;
            RLOG("RIP: %s = 0x%lx", anchor.label, target_offset);
            resolved++;
        } else {
            RLOG("RIP: %s -> FAILED", anchor.label);
        }
    }

    return resolved;
}

// ============================================================================
// Hardcoded Defaults (from ck3_offsets.h)
// ============================================================================

void populate_hardcoded(ResolvedOffsets* out) {
    memset(out, 0, sizeof(*out));

    // Tier 1
    out->AI_MANAGER_MAIN_TICK = ck3::funcs::AI_MANAGER_MAIN_TICK;
    out->AI_MANAGER_POST_EXECUTE = ck3::funcs::AI_MANAGER_POST_EXECUTE;
    out->AI_ENABLED_FLAG = ck3::globals::AI_ENABLED_FLAG;
    out->GAME_STATE_ROOT = ck3::globals::GAME_STATE_ROOT;
    out->CHARACTER_DB = ck3::globals::CHARACTER_DB;
    out->IDLER_SINGLETON = ck3::globals::IDLER_SINGLETON;

    // Tier 2
    out->STRING_HASH = ck3::funcs::STRING_HASH;
    out->CHAR_ID_TO_PTR = ck3::funcs::CHAR_ID_TO_PTR;
    out->AI_PERSONALITY_HONOR = ck3::funcs::AI_PERSONALITY_HONOR;
    out->AI_PERSONALITY_GREED = ck3::funcs::AI_PERSONALITY_GREED;
    out->AI_PERSONALITY_RATIONALITY = ck3::funcs::AI_PERSONALITY_RATIONALITY;
    out->AI_PERSONALITY_ENERGY = ck3::funcs::AI_PERSONALITY_ENERGY;
    out->AI_PERSONALITY_BOLDNESS = ck3::funcs::AI_PERSONALITY_BOLDNESS;
    out->AI_PERSONALITY_ZEAL = ck3::funcs::AI_PERSONALITY_ZEAL;
    out->AI_PERSONALITY_VENGEFULNESS = ck3::funcs::AI_PERSONALITY_VENGEFULNESS;
    out->AI_PERSONALITY_COMPASSION = ck3::funcs::AI_PERSONALITY_COMPASSION;
    out->AI_PERSONALITY_SOCIABILITY = ck3::funcs::AI_PERSONALITY_SOCIABILITY;
    out->THREAD_FLAG_STRATEGY = ck3::globals::THREAD_FLAG_STRATEGY;
    out->THREAD_FLAG_MARRIAGE = ck3::globals::THREAD_FLAG_MARRIAGE;
    out->THREAD_FLAG_INTERACT = ck3::globals::THREAD_FLAG_INTERACT;

    // Tier 3
    out->TRAIT_DB = ck3::globals::TRAIT_DB;
    out->INTERACTION_DB = ck3::globals::INTERACTION_DB;
    out->LANDED_TITLE_DB = ck3::globals::LANDED_TITLE_DB;
    out->WAR_DB = ck3::globals::WAR_DB;
    out->FACTION_DB = ck3::globals::FACTION_DB;
    out->PLAYER_CHAR_ID = ck3::globals::PLAYER_CHAR_ID;
    out->AI_TOGGLE_BASE = ck3::globals::AI_TOGGLE_BASE;
    out->COMMAND_DISPATCH = ck3::globals::COMMAND_DISPATCH_MGR;

    // Tier 4
    out->PAUSE_SET = ck3::funcs::PAUSE_SET;
    out->GET_OPINION_OF = ck3::funcs::GET_OPINION_OF;
    out->STRATEGY_EXECUTE = ck3::funcs::STRATEGY_EXECUTE;

    // Tier 5: Character accessor functions
    out->GOLD_GET = ck3::funcs::GOLD_GET;
    out->PIETY_GET = ck3::funcs::PIETY_GET;
    out->PRESTIGE_GET = ck3::funcs::PRESTIGE_GET;
    out->STRESS_GET = ck3::funcs::GET_STRESS;
    out->DREAD_GET = ck3::funcs::GET_DREAD;
    out->SKILL_GET_BASE = ck3::funcs::SKILL_GET_BASE;
    out->SKILL_GET_CALC = ck3::funcs::SKILL_GET_CALC;
    out->TRAIT_LIST_GET = ck3::funcs::TRAIT_GET_LIST;
    out->IS_ALIVE = ck3::funcs::CHAR_IS_ALIVE;
    out->IS_RULER = ck3::funcs::CHAR_IS_RULER;
    out->GET_FACTION_LIST = ck3::funcs::GET_FACTION_LIST;

    // Tier 6: Action dispatch functions
    out->BUILD_INTERACTION_CMD = ck3::funcs::BUILD_SEND_INTERACTION_CMD;
    out->COMMAND_DISPATCH_FN = ck3::funcs::GAME_COMMAND_DISPATCH;
    out->WAR_SCORE_COMPUTE = ck3::funcs::WAR_COMPUTE_SCORE;
    out->INCOME_CALC = ck3::funcs::INCOME_MODIFIER_CALC;
    out->CHAR_GET_AI_CONTROLLER = ck3::funcs::CHAR_GET_AI_CONTROLLER;

    // Tier 8: Hardcoded vtable/global defaults (overridden by Zydis discovery)
    out->SEND_CMD_VTABLE = 0x0bb73e38;   // from ctor at 0x609b410: lea rax,[rip+...] -> 0xbb73e38
    out->SEND_CMD_VTABLE2 = 0x0bb73ee8; // from ctor at 0x609b410: lea rax,[rip+...] -> 0xbb73ee8
    out->DISCOVERED_DISPATCH_MGR = ck3::globals::COMMAND_DISPATCH_MGR;

    // Tier 9: Anchor functions for RIP-relative global discovery
    out->GAME_IS_LOADED_FN = ck3::globals::GAME_IS_LOADED;
    out->IS_MANUALLY_PAUSED_FN = ck3::funcs::IS_MANUALLY_PAUSED;
    out->TRAIT_HAS_FLAG_FN = 0x0664bb70;  // FUN_0674bb70: trait immortal/flag checker
    out->AI_LIFESTYLE_DISPATCH_FN = ck3::funcs::AI_LIFESTYLE_DISPATCH;

    // Mark all as hardcoded
    for (int i = 0; i < 112; i++)
        out->methods[i] = ResolveMethod::HARDCODED;
    out->total_hardcoded = 49; // approximate
}

// ============================================================================
// Cross-Validation
// ============================================================================

struct OffsetPair {
    const char* name;
    size_t offset_in_struct;
    uintptr_t hardcoded_value;
};

int cross_validate(ResolvedOffsets* out) {
    // Compare discovered values against hardcoded
    // Build pairs from known hardcoded values
    const OffsetPair pairs[] = {
        { "AI_MANAGER_MAIN_TICK", offsetof(ResolvedOffsets, AI_MANAGER_MAIN_TICK), ck3::funcs::AI_MANAGER_MAIN_TICK },
        { "AI_MANAGER_POST_EXECUTE", offsetof(ResolvedOffsets, AI_MANAGER_POST_EXECUTE), ck3::funcs::AI_MANAGER_POST_EXECUTE },
        { "STRING_HASH", offsetof(ResolvedOffsets, STRING_HASH), ck3::funcs::STRING_HASH },
        { "CHARACTER_DB", offsetof(ResolvedOffsets, CHARACTER_DB), ck3::globals::CHARACTER_DB },
        { "PLAYER_CHAR_ID", offsetof(ResolvedOffsets, PLAYER_CHAR_ID), ck3::globals::PLAYER_CHAR_ID },
        { "AI_ENABLED_FLAG", offsetof(ResolvedOffsets, AI_ENABLED_FLAG), ck3::globals::AI_ENABLED_FLAG },
        { "COMMAND_DISPATCH", offsetof(ResolvedOffsets, COMMAND_DISPATCH), ck3::globals::COMMAND_DISPATCH_MGR },
        // Accessor functions — cross-validate signature-scanned vs hardcoded
        { "IS_ALIVE", offsetof(ResolvedOffsets, IS_ALIVE), ck3::funcs::CHAR_IS_ALIVE },
        { "IS_RULER", offsetof(ResolvedOffsets, IS_RULER), ck3::funcs::CHAR_IS_RULER },
        { "STRESS_GET", offsetof(ResolvedOffsets, STRESS_GET), ck3::funcs::GET_STRESS },
        { "DREAD_GET", offsetof(ResolvedOffsets, DREAD_GET), ck3::funcs::GET_DREAD },
        // New global anchors — cross-validate RIP-discovered vs hardcoded
        { "GAME_STATE_ROOT", offsetof(ResolvedOffsets, GAME_STATE_ROOT), ck3::globals::GAME_STATE_ROOT },
        { "IDLER_SINGLETON", offsetof(ResolvedOffsets, IDLER_SINGLETON), ck3::globals::IDLER_SINGLETON },
        { "TRAIT_DB", offsetof(ResolvedOffsets, TRAIT_DB), ck3::globals::TRAIT_DB },
        { "AI_TOGGLE_BASE", offsetof(ResolvedOffsets, AI_TOGGLE_BASE), ck3::globals::AI_TOGGLE_BASE },
        { "CHAR_GET_AI_CONTROLLER", offsetof(ResolvedOffsets, CHAR_GET_AI_CONTROLLER), ck3::funcs::CHAR_GET_AI_CONTROLLER },
    };

    int mismatches = 0;

    for (const auto& pair : pairs) {
        uintptr_t discovered = *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<const uint8_t*>(out) + pair.offset_in_struct);

        if (discovered != 0 && discovered != pair.hardcoded_value) {
            RLOG("MISMATCH: %s discovered=0x%lx hardcoded=0x%lx (delta=%+ld)",
                 pair.name, discovered, pair.hardcoded_value,
                 (long)(discovered - pair.hardcoded_value));
            mismatches++;
        } else if (discovered != 0 && discovered == pair.hardcoded_value) {
            RLOG("MATCH: %s = 0x%lx", pair.name, discovered);
        }
    }

    out->total_mismatches = mismatches;
    return mismatches;
}

// ============================================================================
// Main Resolver
// ============================================================================

void log_results(const ResolvedOffsets* out) {
    RLOG("=== Resolver Summary ===");
    RLOG("  Discovered: %d, Hardcoded fallback: %d, Mismatches: %d, Failed: %d",
         out->total_discovered, out->total_hardcoded, out->total_mismatches, out->total_failed);
    RLOG("  Time: %.1f ms", out->resolve_time_ms);
    RLOG("========================");
}

// ============================================================================
// ELF BuildID Verification
// ============================================================================
//
// Reads the .note.gnu.build-id from the loaded binary via dl_iterate_phdr
// and compares it against the expected BuildID in offset_version.h.
// Returns: MATCH, MISMATCH, or NOT_FOUND.

enum class BuildIdResult { MATCH, MISMATCH, NOT_FOUND };

struct BuildIdCallbackData {
    uintptr_t base_addr;
    uint8_t found_id[20];
    size_t found_len;
    bool found;
};

#ifndef _WIN32
static int buildid_phdr_callback(struct dl_phdr_info* info, size_t, void* data) {
    auto* d = static_cast<BuildIdCallbackData*>(data);
    if (info->dlpi_addr != d->base_addr)
        return 0;

    // Walk program headers looking for PT_NOTE
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;

        // Parse ELF notes within this segment
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
        const uint8_t* end = ptr + ph.p_memsz;

        while (ptr + sizeof(Elf64_Nhdr) <= end) {
            const auto* nhdr = reinterpret_cast<const Elf64_Nhdr*>(ptr);
            const uint8_t* name = ptr + sizeof(Elf64_Nhdr);
            // Align name and desc to 4 bytes
            size_t name_aligned = (nhdr->n_namesz + 3) & ~3u;
            const uint8_t* desc = name + name_aligned;
            size_t desc_aligned = (nhdr->n_descsz + 3) & ~3u;

            if (nhdr->n_type == NT_GNU_BUILD_ID &&
                nhdr->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                size_t copy_len = nhdr->n_descsz < sizeof(d->found_id)
                    ? nhdr->n_descsz : sizeof(d->found_id);
                memcpy(d->found_id, desc, copy_len);
                d->found_len = copy_len;
                d->found = true;
                return 1; // stop iteration
            }

            ptr = desc + desc_aligned;
        }
    }
    return 0;
}

static BuildIdResult check_build_id(uintptr_t base_addr) {
    BuildIdCallbackData data = {};
    data.base_addr = base_addr;
    dl_iterate_phdr(buildid_phdr_callback, &data);

    // Resolved FIRST and unconditionally: the strictness witness must exist
    // even for a binary that matches, and even for one whose BuildID cannot be
    // read at all. Both of those are exactly the cases where the old code
    // recorded nothing.
    bridge::BuildIdWitness w;
    bridge::resolve_build_id_strictness(w);

    if (!data.found) {
        RLOG("WARNING: No ELF BuildID found in loaded binary");
        w.status = bridge::BuildIdCheck::NOT_FOUND;
        bridge::set_build_id_witness(w);
        return BuildIdResult::NOT_FOUND;
    }

    // Log the found BuildID
    char hex[41] = {};
    for (size_t i = 0; i < data.found_len && i < 20; i++)
        snprintf(hex + i * 2, 3, "%02x", data.found_id[i]);
    RLOG("ELF BuildID: %s", hex);
    // RETAINED, not merely logged (ck-nkyc leg (e)). This is the only value in
    // the whole handshake that is UNIQUE TO WHAT RAN rather than to a tree.
    w.observed_hex = hex;

    // Compare against expected
    if (data.found_len != ck3_version::BUILD_ID_LEN ||
        memcmp(data.found_id, ck3_version::BUILD_ID, ck3_version::BUILD_ID_LEN) != 0) {
        RLOG("WARNING: BuildID MISMATCH — expected %s, got %s",
             ck3_version::BUILD_ID_HEX, hex);
        RLOG("Offsets from ck3_offsets.h may be WRONG for this binary version");
        w.status = bridge::BuildIdCheck::MISMATCH;
        bridge::set_build_id_witness(w);
        return BuildIdResult::MISMATCH;
    }

    RLOG("BuildID matches expected (%s) — offsets rev %d (%s)",
         ck3_version::BUILD_ID_HEX, ck3_version::OFFSET_REVISION, ck3_version::OFFSET_DATE);
    w.status = bridge::BuildIdCheck::MATCH;
    bridge::set_build_id_witness(w);
    RLOG("BuildID strictness: effective=%s compiled=%s env=%s loosen_attempted=%s",
         w.strict_effective ? "yes" : "no", w.strict_compiled ? "yes" : "no",
         w.strict_env.empty() ? "<unset>" : w.strict_env.c_str(),
         w.loosen_attempted ? "yes" : "no");
    return BuildIdResult::MATCH;
}

#else
// Windows: the identity is the PE CodeView RSDS record (GUID + age), checked by
// the proxy DLL's identity gate against pinned_identity.h before the bridge
// starts at all. The witness READS that result (one check, several readers):
// it never runs a second comparison.
static BuildIdResult check_build_id(uintptr_t) {
    bridge::BuildIdWitness w;
    bridge::resolve_build_id_strictness(w);
    bridge::os::BinaryIdentity id{};
    const auto r = bridge::os::binary_identity(&id);
    if (!bridge::os::succeeded(r) || id.observed_hex[0] == '\0') {
        RLOG("WARNING: no PE identity witness (%s)", r.detail);
        w.status = bridge::BuildIdCheck::NOT_FOUND;
        bridge::set_build_id_witness(w);
        return BuildIdResult::NOT_FOUND;
    }
    w.observed_hex = id.observed_hex;
    const bool match = strcmp(id.check, "match") == 0;
    w.status = match ? bridge::BuildIdCheck::MATCH : bridge::BuildIdCheck::MISMATCH;
    bridge::set_build_id_witness(w);
    RLOG("PE identity (%s): %s, gate check=%s", bridge::os::identity_format_string(id.format),
         id.observed_hex, id.check);
    return match ? BuildIdResult::MATCH : BuildIdResult::MISMATCH;
}
#endif  // !_WIN32

void observe_build_id(uintptr_t base_addr) {
    // The whole of leg (e)'s bridge half: read what is ACTUALLY loaded, record
    // it beside the strictness the process is running under, and say so in the
    // log. check_build_id() already does exactly this and sets the witness; it
    // simply never ran, because its only caller does not.
    //
    // IT DOES NOT ABORT, deliberately. The bead's refusal lives in the DRIVER,
    // at episode start, where a mismatch costs one second instead of ~2.2 h of
    // node time — and where it can say WHICH of five legs failed. Making this
    // abort would change what a bridge does to a running game on the strength
    // of a bead about what a bundle records.
    (void)check_build_id(base_addr);
}

bool run_resolver(uintptr_t base_addr, ResolvedOffsets* out) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    RLOG("Starting runtime resolver (base=%p)", (void*)base_addr);

    // Step 0: Verify BuildID matches expected target binary
    BuildIdResult bid = check_build_id(base_addr);
    if (bid == BuildIdResult::MISMATCH) {
        // Strict mode: abort resolver init on BuildID mismatch.
        // Enabled by compile-time flag CK3_STRICT_BUILD_ID, or at runtime via
        // CK3_STRICT_BUILD_ID=1 environment variable. Runtime env var overrides
        // the compile-time default in both directions (0 to relax, 1 to enforce).
        // ck-nkyc: resolved ONCE, in check_build_id(), so the same value the
        // handshake publishes is the value that decides here. Two derivations
        // of one rule is how a bundle ends up asserting a strictness the
        // resolver did not act on.
        const bool strict = bridge::build_id_witness().strict_effective;

        if (strict) {
            RLOG("FATAL: BuildID mismatch with STRICT mode enabled — aborting resolver init. "
                 "Offsets from ck3_offsets.h are for a different binary version. "
                 "Set CK3_STRICT_BUILD_ID=0 to override.");
            clock_gettime(CLOCK_MONOTONIC, &t1);
            out->resolve_time_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            return false;
        }

        RLOG("BuildID mismatch — signature scanning will still run "
             "(signatures may survive patches), but hardcoded offsets are UNTRUSTED");
    }

    // Step 1: Populate with hardcoded defaults
    // On BuildID mismatch, hardcoded values are populated but marked untrusted.
    // Signature/RTTI scanning may still discover correct offsets for this binary.
    populate_hardcoded(out);

    // Step 2: Map memory regions
    ResolverContext ctx;
    if (!init_context(base_addr, &ctx)) {
        RLOG("ERROR: Failed to map memory regions, using hardcoded offsets only");
        clock_gettime(CLOCK_MONOTONIC, &t1);
        out->resolve_time_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        return false;
    }

    // Step 3: RTTI vtable scanning
    int rtti_count = resolve_rtti_targets(&ctx, out);
    RLOG("RTTI phase: resolved %d offsets", rtti_count);

    // Step 4: Signature scanning (v1 — inline patterns)
    int sig_count = resolve_signatures(&ctx, out);
    RLOG("Signature v1 phase: resolved %d offsets", sig_count);

    // Step 4b: Signature scanning (v2 — database-driven, from signatures.h)
    int sig2_count = ck3_signatures::resolve_signatures_v2(&ctx, out);
    RLOG("Signature v2 phase: resolved %d offsets", sig2_count);

    // Step 5: RIP-relative tracing
    int rip_count = resolve_rip_anchors(&ctx, out);
    RLOG("RIP-trace phase: resolved %d offsets", rip_count);

    // Step 5b: Zydis instruction decoding (vtable/global discovery)
    int zydis_count = resolve_instruction_refs(base_addr, out);
    RLOG("Zydis instruction decode phase: resolved %d offsets", zydis_count);

    // Step 6: Cross-validate
    out->total_discovered = rtti_count + sig_count + sig2_count + rip_count + zydis_count;
    cross_validate(out);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    out->resolve_time_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    log_results(out);

    return true;
}

// ============================================================================
// Zydis-Based Instruction Decoding for VTable/Global Discovery
// ============================================================================
//
// These functions decode x86-64 instructions using Zydis to extract
// RIP-relative references from known functions. This is more reliable than
// the manual byte-pattern RIP tracing above because Zydis correctly handles
// all instruction encodings, prefixes, and operand sizes.

// Scan a function's code for LEA instructions that load addresses from
// a specific memory region (e.g., .data.rel.ro for vtables).
// Returns the Nth such resolved absolute address, or 0 on failure.
static uintptr_t zydis_find_rip_lea_in_region(
    uintptr_t func_addr,
    int ordinal,
    int scan_bytes,
    const MemoryRegion* target_region)
{
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    const uint8_t* code = reinterpret_cast<const uint8_t*>(func_addr);
    size_t offset = 0;
    int found = 0;

    while (offset < static_cast<size_t>(scan_bytes)) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset,
                                                  scan_bytes - offset, &insn, operands))) {
            offset++;
            continue;
        }

        // Look for LEA with RIP-relative memory operand
        if (insn.mnemonic == ZYDIS_MNEMONIC_LEA) {
            for (int i = 0; i < insn.operand_count_visible; i++) {
                if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    operands[i].mem.base == ZYDIS_REGISTER_RIP) {
                    // Resolve absolute address: instruction_addr + insn_length + displacement
                    uintptr_t insn_addr = func_addr + offset;
                    int64_t disp = operands[i].mem.disp.value;
                    uintptr_t resolved = insn_addr + insn.length + disp;

                    // Check if it falls in the target region (if specified)
                    if (!target_region || target_region->contains(resolved)) {
                        if (found == ordinal) {
                            return resolved;
                        }
                        found++;
                    }
                }
            }
        }

        offset += insn.length;
    }

    return 0;
}

// Scan a function for MOV instructions that store an immediate or RIP-relative
// value into a memory operand (e.g., MOV [RDI], <vtable_addr> via LEA+MOV pair).
// This handles the common pattern:
//   LEA RAX, [RIP+disp]   ; load vtable address
//   MOV [RDI], RAX         ; store into object
// We track the last LEA result and return it when we see the store pattern.
static uintptr_t zydis_find_vtable_store(
    uintptr_t func_addr,
    int scan_bytes,
    const MemoryRegion* data_relro_region)
{
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    const uint8_t* code = reinterpret_cast<const uint8_t*>(func_addr);
    size_t offset = 0;

    // Track the most recent LEA result that targets .data.rel.ro
    uintptr_t last_lea_target = 0;

    while (offset < static_cast<size_t>(scan_bytes)) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset,
                                                  scan_bytes - offset, &insn, operands))) {
            offset++;
            continue;
        }

        if (insn.mnemonic == ZYDIS_MNEMONIC_LEA) {
            for (int i = 0; i < insn.operand_count_visible; i++) {
                if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    operands[i].mem.base == ZYDIS_REGISTER_RIP) {
                    uintptr_t insn_addr = func_addr + offset;
                    int64_t disp = operands[i].mem.disp.value;
                    uintptr_t resolved = insn_addr + insn.length + disp;

                    if (data_relro_region && data_relro_region->contains(resolved)) {
                        last_lea_target = resolved;
                    }
                }
            }
        }

        // MOV [reg], reg — storing vtable pointer into object
        // The pattern is: LEA rax, [rip+vtable] ; MOV [rdi], rax (or MOV [param_1], rax)
        // We detect MOV with a memory destination (base=RDI or RSI, no disp) and register source
        if (insn.mnemonic == ZYDIS_MNEMONIC_MOV && last_lea_target != 0) {
            // Check: operand 0 is memory (destination), operand 1 is register (source)
            if (insn.operand_count_visible >= 2 &&
                operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                // Memory destination with base register and zero/no displacement
                // This is the store: *param_1 = vtable_addr
                ZydisRegister base = operands[0].mem.base;
                int64_t disp = operands[0].mem.disp.value;
                if (disp == 0 &&
                    (base == ZYDIS_REGISTER_RDI || base == ZYDIS_REGISTER_RSI ||
                     base == ZYDIS_REGISTER_RCX || base == ZYDIS_REGISTER_RDX ||
                     base == ZYDIS_REGISTER_RAX || base == ZYDIS_REGISTER_RBX)) {
                    return last_lea_target;
                }
            }
        }

        offset += insn.length;
    }

    return 0;
}

// Scan a function for any RIP-relative reference to .data/.bss region
// (where global singletons live). Returns the Nth such reference.
static uintptr_t zydis_find_rip_ref_to_region(
    uintptr_t func_addr,
    int ordinal,
    int scan_bytes,
    const MemoryRegion* target_region)
{
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    const uint8_t* code = reinterpret_cast<const uint8_t*>(func_addr);
    size_t offset = 0;
    int found = 0;

    while (offset < static_cast<size_t>(scan_bytes)) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset,
                                                  scan_bytes - offset, &insn, operands))) {
            offset++;
            continue;
        }

        for (int i = 0; i < insn.operand_count_visible; i++) {
            if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[i].mem.base == ZYDIS_REGISTER_RIP) {
                uintptr_t insn_addr = func_addr + offset;
                int64_t disp = operands[i].mem.disp.value;
                uintptr_t resolved = insn_addr + insn.length + disp;

                if (target_region && target_region->contains(resolved)) {
                    if (found == ordinal) {
                        return resolved;
                    }
                    found++;
                }
            }
        }

        offset += insn.length;
    }

    return 0;
}

// Follow a thunk (short trampoline function) to find the real function body.
// Thunks are typically < 32 bytes and consist of register shuffling + CALL/JMP.
// Returns the resolved target address, or 0 if not a thunk.
static uintptr_t follow_thunk(uintptr_t func_addr, const MemoryRegion* text_region) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    const uint8_t* code = reinterpret_cast<const uint8_t*>(func_addr);
    size_t offset = 0;

    // Scan first 32 bytes for CALL or JMP with RIP-relative target
    while (offset < 32) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset,
                                                  32 - offset, &insn, operands))) {
            offset++;
            continue;
        }

        if (insn.mnemonic == ZYDIS_MNEMONIC_CALL || insn.mnemonic == ZYDIS_MNEMONIC_JMP) {
            for (int i = 0; i < insn.operand_count_visible; i++) {
                if (operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    // Near CALL/JMP with relative displacement
                    uintptr_t target = func_addr + offset + insn.length +
                                       static_cast<int64_t>(operands[i].imm.value.s);
                    if (text_region && text_region->contains(target)) {
                        RLOG("follow_thunk: 0x%lx -> 0x%lx via %s at +0x%zx",
                             func_addr, target,
                             insn.mnemonic == ZYDIS_MNEMONIC_CALL ? "CALL" : "JMP",
                             offset);
                        return target;
                    }
                }
            }
        }

        offset += insn.length;
    }

    return 0;
}

uintptr_t discover_send_cmd_vtable(uintptr_t base_addr) {
    // FUN_0619b410 (BUILD_SEND_CMD_FROM_BUF) writes:
    //   *param_1 = CSendCharacterInteractionCommand::vftable;
    // This is a LEA rax, [rip+vtable] ; MOV [rdi], rax pattern.
    // The vtable lives in .data.rel.ro.
    //
    // HOWEVER: the offset may point to a thunk (25-byte trampoline), not the
    // real function body. If the initial scan fails, follow the thunk's CALL/JMP
    // to find the real function and scan that instead.

    ResolverContext ctx;
    if (!init_context(base_addr, &ctx)) {
        RLOG("discover_send_cmd_vtable: failed to init context");
        return 0;
    }

    uintptr_t func_addr = base_addr + ck3::funcs::BUILD_SEND_CMD_FROM_BUF;
    if (!ctx.text.contains(func_addr)) {
        RLOG("discover_send_cmd_vtable: BUILD_SEND_CMD_FROM_BUF at 0x%lx not in .text", func_addr);
        return 0;
    }

    // Scan the first 128 bytes — the vtable store is in the prologue
    uintptr_t vtable = zydis_find_vtable_store(func_addr, 128, &ctx.data_relro);
    if (vtable) {
        RLOG("discover_send_cmd_vtable: FOUND primary vtable at 0x%lx (offset=0x%lx)",
             vtable, vtable - base_addr);
        return vtable;
    }

    // Fallback: find the first LEA that targets .data.rel.ro
    vtable = zydis_find_rip_lea_in_region(func_addr, 0, 128, &ctx.data_relro);
    if (vtable) {
        RLOG("discover_send_cmd_vtable: FOUND vtable via LEA fallback at 0x%lx (offset=0x%lx)",
             vtable, vtable - base_addr);
        return vtable;
    }

    // Thunk handling: follow CALL/JMP to the real function body
    uintptr_t real_func = follow_thunk(func_addr, &ctx.text);
    if (real_func) {
        RLOG("discover_send_cmd_vtable: following thunk to real function at 0x%lx", real_func);

        vtable = zydis_find_vtable_store(real_func, 256, &ctx.data_relro);
        if (vtable) {
            RLOG("discover_send_cmd_vtable: FOUND vtable in thunk target at 0x%lx (offset=0x%lx)",
                 vtable, vtable - base_addr);
            return vtable;
        }

        vtable = zydis_find_rip_lea_in_region(real_func, 0, 256, &ctx.data_relro);
        if (vtable) {
            RLOG("discover_send_cmd_vtable: FOUND vtable via LEA in thunk target at 0x%lx (offset=0x%lx)",
                 vtable, vtable - base_addr);
            return vtable;
        }

        // The thunk's CALL target might itself call into the real constructor.
        // Follow one more level of indirection.
        uintptr_t real_func2 = follow_thunk(real_func, &ctx.text);
        if (real_func2) {
            RLOG("discover_send_cmd_vtable: following second-level thunk to 0x%lx", real_func2);
            vtable = zydis_find_vtable_store(real_func2, 256, &ctx.data_relro);
            if (vtable) {
                RLOG("discover_send_cmd_vtable: FOUND vtable in 2nd thunk target at 0x%lx (offset=0x%lx)",
                     vtable, vtable - base_addr);
                return vtable;
            }
            vtable = zydis_find_rip_lea_in_region(real_func2, 0, 256, &ctx.data_relro);
            if (vtable) {
                RLOG("discover_send_cmd_vtable: FOUND vtable via LEA in 2nd thunk at 0x%lx (offset=0x%lx)",
                     vtable, vtable - base_addr);
                return vtable;
            }
        }
    }

    RLOG("discover_send_cmd_vtable: FAILED to find vtable (including thunk chase)");
    return 0;
}

uintptr_t discover_send_cmd_vtable2(uintptr_t base_addr) {
    // The secondary vtable (CGameCommandHelper) is stored at cmd+0x18.
    // In BUILD_SEND_CMD_FROM_BUF, after the first vtable store at [param_1],
    // there's a second LEA+MOV for [param_1+0x18].
    // We look for the second LEA targeting .data.rel.ro.

    ResolverContext ctx;
    if (!init_context(base_addr, &ctx)) {
        RLOG("discover_send_cmd_vtable2: failed to init context");
        return 0;
    }

    uintptr_t func_addr = base_addr + ck3::funcs::BUILD_SEND_CMD_FROM_BUF;
    if (!ctx.text.contains(func_addr)) {
        RLOG("discover_send_cmd_vtable2: BUILD_SEND_CMD_FROM_BUF not in .text");
        return 0;
    }

    // The second LEA to .data.rel.ro should be the secondary vtable
    uintptr_t vtable2 = zydis_find_rip_lea_in_region(func_addr, 1, 256, &ctx.data_relro);
    if (vtable2) {
        RLOG("discover_send_cmd_vtable2: FOUND secondary vtable at 0x%lx (offset=0x%lx)",
             vtable2, vtable2 - base_addr);
    } else {
        RLOG("discover_send_cmd_vtable2: FAILED to find secondary vtable");
    }

    return vtable2;
}

// Scan callers of a known function to find a RIP-relative LEA in the first
// register parameter (RDI). Pattern: LEA RDI, [rip+global]; CALL target_func.
// Scans .text for E8 (CALL rel32) instructions targeting target_func_addr,
// then scans backwards from each call site for LEA RDI, [rip+disp].
static uintptr_t find_global_from_callers(
    uintptr_t target_func_addr,
    const ResolverContext* ctx,
    const MemoryRegion* expected_region)
{
    // CALL rel32 is E8 xx xx xx xx (5 bytes)
    // We scan .text for CALL instructions and check if the target matches
    const uint8_t* text_start = reinterpret_cast<const uint8_t*>(ctx->text.start);
    size_t text_size = ctx->text.size();

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    int callers_found = 0;

    // Scan for E8 byte (CALL rel32 opcode) as a fast filter
    for (size_t i = 0; i + 5 < text_size; i++) {
        if (text_start[i] != 0xE8)
            continue;

        // Decode the rel32 displacement
        int32_t rel32;
        memcpy(&rel32, text_start + i + 1, 4);
        uintptr_t call_site = ctx->text.start + i;
        uintptr_t call_target = call_site + 5 + static_cast<int64_t>(rel32);

        if (call_target != target_func_addr)
            continue;

        callers_found++;

        // Found a caller! Now scan backwards from the call site for
        // LEA RDI, [rip+disp] (the dispatch_mgr global)
        // Scan the 64 bytes before the call instruction
        size_t scan_start = (i >= 64) ? (i - 64) : 0;
        size_t scan_len = i - scan_start;
        uintptr_t scan_addr = ctx->text.start + scan_start;
        const uint8_t* scan_code = text_start + scan_start;

        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        // Track the last LEA RDI we see before the CALL
        uintptr_t last_lea_rdi_target = 0;
        size_t off = 0;

        while (off < scan_len) {
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, scan_code + off,
                                                      scan_len - off, &insn, operands))) {
                off++;
                continue;
            }

            if (insn.mnemonic == ZYDIS_MNEMONIC_LEA &&
                insn.operand_count_visible >= 2) {
                // Check: destination is RDI, source is RIP-relative memory
                if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    operands[0].reg.value == ZYDIS_REGISTER_RDI &&
                    operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    operands[1].mem.base == ZYDIS_REGISTER_RIP) {
                    uintptr_t insn_addr = scan_addr + off;
                    int64_t disp = operands[1].mem.disp.value;
                    uintptr_t resolved = insn_addr + insn.length + disp;

                    if (!expected_region || expected_region->contains(resolved)) {
                        last_lea_rdi_target = resolved;
                    }
                }
            }

            off += insn.length;
        }

        if (last_lea_rdi_target) {
            RLOG("find_global_from_callers: FOUND global at 0x%lx from caller at 0x%lx "
                 "(caller #%d)", last_lea_rdi_target, call_site, callers_found);
            return last_lea_rdi_target;
        }

        // Stop after checking 8 callers to avoid excessive scanning
        if (callers_found >= 8) break;
    }

    RLOG("find_global_from_callers: scanned %d callers, no match found", callers_found);
    return 0;
}

uintptr_t discover_dispatch_mgr(uintptr_t base_addr) {
    // GAME_COMMAND_DISPATCH receives dispatch_mgr as its first parameter (RDI),
    // so scanning the function itself won't find it — it doesn't load it from a
    // global. Instead, we scan CALLERS of GAME_COMMAND_DISPATCH for the pattern:
    //   LEA RDI, [rip+dispatch_mgr_global]
    //   CALL GAME_COMMAND_DISPATCH

    ResolverContext ctx;
    if (!init_context(base_addr, &ctx)) {
        RLOG("discover_dispatch_mgr: failed to init context");
        return 0;
    }

    uintptr_t func_addr = base_addr + ck3::funcs::GAME_COMMAND_DISPATCH;
    if (!ctx.text.contains(func_addr)) {
        RLOG("discover_dispatch_mgr: GAME_COMMAND_DISPATCH at 0x%lx not in .text", func_addr);
        return 0;
    }

    // Try .data/.bss first (where mutable globals usually live)
    uintptr_t mgr = find_global_from_callers(func_addr, &ctx, &ctx.data_bss);
    if (mgr) {
        RLOG("discover_dispatch_mgr: FOUND dispatch manager at 0x%lx (offset=0x%lx) via caller scan (.data/.bss)",
             mgr, mgr - base_addr);
        return mgr;
    }

    // Try .data.rel.ro
    mgr = find_global_from_callers(func_addr, &ctx, &ctx.data_relro);
    if (mgr) {
        RLOG("discover_dispatch_mgr: FOUND dispatch manager at 0x%lx (offset=0x%lx) via caller scan (.data.rel.ro)",
             mgr, mgr - base_addr);
        return mgr;
    }

    // Last resort: try without region constraint
    mgr = find_global_from_callers(func_addr, &ctx, nullptr);
    if (mgr) {
        RLOG("discover_dispatch_mgr: FOUND dispatch manager at 0x%lx (offset=0x%lx) via caller scan (any region)",
             mgr, mgr - base_addr);
        return mgr;
    }

    RLOG("discover_dispatch_mgr: FAILED — no callers with LEA RDI found");
    return 0;
}

int resolve_instruction_refs(uintptr_t base_addr, ResolvedOffsets* out) {
    int resolved = 0;

    RLOG("=== Zydis instruction decoding phase ===");

    // Discover CSendCharacterInteractionCommand primary vtable
    uintptr_t vtable1 = discover_send_cmd_vtable(base_addr);
    if (vtable1) {
        out->SEND_CMD_VTABLE = vtable1 - base_addr;
        resolved++;

        // Cross-validate against hardcoded
        uintptr_t hardcoded = 0x0bc73e38;
        if (out->SEND_CMD_VTABLE != hardcoded) {
            RLOG("SEND_CMD_VTABLE: discovered=0x%lx hardcoded=0x%lx (delta=%+ld)",
                 out->SEND_CMD_VTABLE, hardcoded, (long)(out->SEND_CMD_VTABLE - hardcoded));
        } else {
            RLOG("SEND_CMD_VTABLE: MATCH 0x%lx", out->SEND_CMD_VTABLE);
        }
    }

    // Discover secondary vtable
    uintptr_t vtable2 = discover_send_cmd_vtable2(base_addr);
    if (vtable2) {
        out->SEND_CMD_VTABLE2 = vtable2 - base_addr;
        resolved++;

        uintptr_t hardcoded = 0x0bc73ee8;
        if (out->SEND_CMD_VTABLE2 != hardcoded) {
            RLOG("SEND_CMD_VTABLE2: discovered=0x%lx hardcoded=0x%lx (delta=%+ld)",
                 out->SEND_CMD_VTABLE2, hardcoded, (long)(out->SEND_CMD_VTABLE2 - hardcoded));
        } else {
            RLOG("SEND_CMD_VTABLE2: MATCH 0x%lx", out->SEND_CMD_VTABLE2);
        }
    }

    // Discover COMMAND_DISPATCH_MGR
    uintptr_t mgr = discover_dispatch_mgr(base_addr);
    if (mgr) {
        out->DISCOVERED_DISPATCH_MGR = mgr - base_addr;
        resolved++;

        uintptr_t hardcoded = ck3::globals::COMMAND_DISPATCH_MGR;
        if (out->DISCOVERED_DISPATCH_MGR != hardcoded) {
            RLOG("DISPATCH_MGR: discovered=0x%lx hardcoded=0x%lx (delta=%+ld)",
                 out->DISCOVERED_DISPATCH_MGR, hardcoded,
                 (long)(out->DISCOVERED_DISPATCH_MGR - hardcoded));
        } else {
            RLOG("DISPATCH_MGR: MATCH 0x%lx", out->DISCOVERED_DISPATCH_MGR);
        }
    }

    RLOG("Zydis instruction decoding: resolved %d/3 references", resolved);
    return resolved;
}

} // namespace ck3_resolver
