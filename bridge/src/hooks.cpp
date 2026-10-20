// Inline hooking for x86-64.
// Replaces the first bytes of a target function with a JMP to our hook,
// saving the original bytes to a trampoline for calling through.
//
// Safety: Analyzes instruction boundaries before overwriting. Handles
// RIP-relative instructions by patching displacements in the trampoline.
// Allocates trampoline pool near the target binary for ±2GB reach.
// Expands short branches (rel8) to near branches (rel32) when relocating.

#include "bridge.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include <Zydis/Zydis.h>

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include "windows/ck3_offsets_win.h"   // the reviewed targets (P6)
#endif

// Log to the bridge log file only (stderr is unsafe in pressure-vessel)
static void hlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void hlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (bridge::g_logfile) {
        vfprintf(bridge::g_logfile, fmt, ap);
        fflush(bridge::g_logfile);
    }
    va_end(ap);
}

// ScopedWritable is now defined in bridge.h (bridge::ScopedWritable)

namespace bridge {

// x86-64 absolute jump: mov rax, <addr>; jmp rax = 12 bytes
static constexpr size_t HOOK_SIZE = 12;

// Maximum bytes we might need to copy (HOOK_SIZE + longest single insn - 1)
// x86-64 instructions are at most 15 bytes; worst case we copy HOOK_SIZE + 14
static constexpr size_t MAX_STOLEN_BYTES = 32;

// Trampoline slot: stolen instructions + 12-byte JMP back
// Extra room for RIP-relative fixups that may expand instructions
static constexpr size_t TRAMPOLINE_SLOT_SIZE = MAX_STOLEN_BYTES + 64;

// Maximum number of simultaneous hooks
static constexpr size_t MAX_HOOKS = 32;  // Currently used: 9 personality getter hooks

// =============================================================================
// x86-64 instruction decoder using Zydis
// =============================================================================
// Complete x86-64 decoding via Zydis library. Handles all instruction encodings
// including VEX/AVX, EVEX, MOVSXD, shift/rotate groups, and everything else
// that a hand-rolled decoder would miss.

// Information about a decoded instruction for relocation purposes
struct InsnInfo {
    size_t length;          // total instruction length
    bool has_rip_relative;  // uses RIP-relative addressing
    size_t rip_disp_offset; // offset of the disp32 field within the instruction (from instruction start)
    size_t rip_insn_length; // full instruction length (for computing RIP base = insn_addr + insn_length)
    bool is_branch;         // is a branch/call (rel8 or rel32) — displacement is the ONLY operand
    size_t branch_disp_offset; // offset of the displacement within the branch instruction
    size_t branch_disp_size;   // 1 for rel8, 4 for rel32
};

// One-time initialized Zydis decoder instance
static ZydisDecoder g_zydis_decoder;
static bool g_zydis_initialized = false;

static void ensure_zydis_init() {
    if (!g_zydis_initialized) {
        ZydisDecoderInit(&g_zydis_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        g_zydis_initialized = true;
    }
}

// Decode a single instruction at `code` and populate InsnInfo for relocation.
// Returns InsnInfo with length == 0 on decode failure.
static InsnInfo decode_insn(const uint8_t* code) {
    InsnInfo info = {};

    ensure_zydis_init();

    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    // Decode up to 15 bytes (max x86-64 instruction length)
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_zydis_decoder, code, 15, &insn, operands))) {
        return info; // length == 0 signals failure
    }

    info.length = insn.length;
    info.rip_insn_length = insn.length;

    // Check for RIP-relative memory operands (e.g. mov rax, [rip+disp32])
    for (int i = 0; i < insn.operand_count; i++) {
        if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            operands[i].mem.base == ZYDIS_REGISTER_RIP) {
            info.has_rip_relative = true;
            // The displacement offset within the instruction
            info.rip_disp_offset = insn.raw.disp.offset;
            break;
        }
    }

    // Check for branch/call instructions with relative displacements
    if (insn.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
        // Relative branches: the displacement is in imm[0]
        if (insn.raw.imm[0].is_relative) {
            info.is_branch = true;
            info.has_rip_relative = true;
            info.branch_disp_offset = insn.raw.imm[0].offset;
            info.branch_disp_size = insn.raw.imm[0].size / 8; // bits to bytes
        }
    }

    // CALL rel32 — Zydis may not set branch_type for CALL, handle explicitly
    if (insn.mnemonic == ZYDIS_MNEMONIC_CALL &&
        insn.operand_count > 0 &&
        operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
        insn.raw.imm[0].is_relative) {
        info.is_branch = true;
        info.has_rip_relative = true;
        info.branch_disp_offset = insn.raw.imm[0].offset;
        info.branch_disp_size = insn.raw.imm[0].size / 8;
    }

    return info;
}

// =============================================================================
// Trampoline Pool
// =============================================================================
// Allocates one RWX page and carves out fixed-size slots from it.

static constexpr size_t POOL_SIZE = 4096 * 4;  // 16KB to handle expanded instructions
static uint8_t* g_trampoline_pool = nullptr;
static size_t g_trampoline_next_slot = 0;

// Try to allocate memory near `target` so RIP-relative fixups fit in ±2GB.
// Falls back to a regular mmap if near allocation fails.
static uint8_t* alloc_rwx_near(uintptr_t target, size_t size) {
    long page_size = sysconf(_SC_PAGESIZE);
    // Search ±1GB from target in 64KB steps
    uintptr_t base = target > (uintptr_t)0x40000000 ? target - 0x40000000 : page_size;
    uintptr_t end = target + 0x40000000;

    for (uintptr_t addr = base; addr < end; addr += 0x10000) {
        uintptr_t aligned = addr & ~(page_size - 1);
        void* p = mmap(reinterpret_cast<void*>(aligned), size,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            // Verify it's actually within ±2GB
            intptr_t delta = reinterpret_cast<intptr_t>(p) - static_cast<intptr_t>(target);
            if (delta > -0x7FFFFFFFL && delta < 0x7FFFFFFFL) {
                return static_cast<uint8_t*>(p);
            }
            munmap(p, size);
        }
    }

    // Fallback: let the OS pick (RIP-relative fixups may fail for distant targets)
    hlog("[CK3-Bridge] WARNING: Could not allocate trampoline pool near 0x%lx, using fallback\n",
            target);
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : static_cast<uint8_t*>(p);
}

uint8_t* alloc_trampoline_slot() {
    if (!g_trampoline_pool) {
        // Use g_base_addr as the target hint for near allocation
        g_trampoline_pool = alloc_rwx_near(g_base_addr, POOL_SIZE);
        if (!g_trampoline_pool) {
            return nullptr;
        }
        // Fill with INT3 for safety
        memset(g_trampoline_pool, 0xCC, POOL_SIZE);
        g_trampoline_next_slot = 0;
        hlog("[CK3-Bridge] Trampoline pool at %p (base 0x%lx, delta %ld)\n",
                g_trampoline_pool, g_base_addr,
                (long)(reinterpret_cast<intptr_t>(g_trampoline_pool) - (intptr_t)g_base_addr));
    }

    // Check if we have room
    size_t offset = g_trampoline_next_slot * TRAMPOLINE_SLOT_SIZE;
    if (offset + TRAMPOLINE_SLOT_SIZE > POOL_SIZE) {
        return nullptr; // Pool exhausted
    }

    uint8_t* slot = g_trampoline_pool + offset;
    g_trampoline_next_slot++;
    return slot;
}

// =============================================================================
// Hook Management
// =============================================================================

struct HookEntry {
    uintptr_t target;
    uint8_t original_bytes[MAX_STOLEN_BYTES];
    size_t stolen_len;       // actual number of bytes stolen (>= HOOK_SIZE)
    uint8_t* trampoline;     // points into pool
    bool active;
};

static HookEntry g_hooks[MAX_HOOKS] = {};

// Write an absolute JMP to target_addr at location
// mov rax, <64-bit address>; jmp rax  (12 bytes)
static void write_abs_jmp(void* location, void* target_addr) {
    uint8_t* p = static_cast<uint8_t*>(location);
    p[0] = 0x48;  // REX.W
    p[1] = 0xB8;  // MOV RAX, imm64
    memcpy(p + 2, &target_addr, 8);
    p[10] = 0xFF;  // JMP RAX
    p[11] = 0xE0;
}

#ifdef _WIN32
// ADR 0001 P6. An inline hook on Windows is installed only at a target whose hook
// prototype was checked against the MSVC function (ck3_offsets_win.h says how for
// each): the ABIs differ in more than registers (a struct over 8 bytes is returned
// through a hidden pointer, a member function takes `this` before it, an engine
// allocator's free sits at another vtable slot), so the Linux prototype is no
// evidence. Every other target is refused, an unmapped (poisoned) one included.
static const uint32_t kWinReviewedHookRvas[] = {
    ck3_win::funcs::AI_WAR_POST_DECL,   // on_ai_war_post_decl(void* ai_ctrl, int rng, int bypass)
    ck3_win::funcs::AI_CB_SELECTOR,     // on_ai_cb_selector(void* result, void* ai_ctrl, int rng)
    ck3_win::funcs::WAR_END_CORE,       // on_war_end(void* mgr, void* war, int outcome, unsigned notify)
    ck3_win::funcs::CREATE_ACTIVE_SCHEME,  // on_create_active_scheme(mgr, type, owner, scope, p5, p6) -> void*
    ck3_win::funcs::INTERACTION_TRY_SEND,  // on_interaction_try_send(uintptr_t* ctx, long extra) -> char
    ck3_win::funcs::WAR_SCORE_UPDATE,   // on_war_score_update(void* war); components adapted in war_score_hook.cpp
    ck3_win::funcs::LOBBY_VIEW_CTOR,    // lobby_view_ctor_hook(this, a2, a3) -> this (MSVC ctor)
};

// The INT3-first install's window. Linux has a SIGTRAP handler for a thread that
// enters the function while its first byte is 0xCC; here a vectored handler does
// the same: a breakpoint inside the window resumes at the INT3's own address, which
// by then (or on a later try) holds the hook's first byte.
static std::atomic<uintptr_t> g_install_lo{0}, g_install_hi{0};
static LONG CALLBACK install_breakpoint_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto a = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    if (a < g_install_lo.load() || a >= g_install_hi.load()) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Rip = static_cast<DWORD64>(a);
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

void* install_hook(uintptr_t target_addr, void* hook_fn) {
#ifdef _WIN32
    {
        bool reviewed = false;
        for (uint32_t rva : kWinReviewedHookRvas)
            reviewed = reviewed || target_addr == g_base_addr + rva;
        if (!reviewed) {
            hlog("[CK3-Bridge] install_hook REFUSED at 0x%llx: not a reviewed Windows hook target "
                 "(ADR 0001 P6: per-hook MSVC signature review)\n", (unsigned long long)target_addr);
            return nullptr;
        }
        static PVOID veh = AddVectoredExceptionHandler(1, install_breakpoint_veh);
        if (!veh) {
            hlog("[CK3-Bridge] install_hook REFUSED at 0x%llx: no breakpoint handler for the "
                 "INT3 window\n", (unsigned long long)target_addr);
            return nullptr;
        }
    }
#endif
    // Find a free hook slot
    HookEntry* entry = nullptr;
    for (size_t i = 0; i < MAX_HOOKS; i++) {
        if (!g_hooks[i].active) {
            entry = &g_hooks[i];
            break;
        }
    }
    if (!entry) {
        hlog("[CK3-Bridge] ERROR: No free hook slots\n");
        return nullptr;
    }

    const uint8_t* code = reinterpret_cast<const uint8_t*>(target_addr);

    // ENDBR64 awareness: on CET-enabled systems, functions may start with
    // endbr64 (F3 0F 1E FA, 4 bytes). Skip it — the hook JMP overwrites it,
    // and the trampoline is called directly so it doesn't need ENDBR64.
    size_t endbr64_skip = 0;
    if (code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x1E && code[3] == 0xFA) {
        endbr64_skip = 4;
        hlog("[CK3-Bridge] ENDBR64 detected at 0x%lx, skipping 4 bytes for instruction stealing\n",
                target_addr);
    }

    // Adjust code pointer past ENDBR64 for instruction decoding.
    // The hook JMP will still be written at byte 0 (overwriting ENDBR64).
    const uint8_t* steal_code = code + endbr64_skip;
    uintptr_t steal_addr = target_addr + endbr64_skip;

    // Walk instructions (past any ENDBR64) until we have >= HOOK_SIZE bytes
    // worth of stealable instructions. The total stolen region must cover
    // at least HOOK_SIZE bytes from the function start (including ENDBR64).
    struct StolenInsn {
        size_t src_offset;  // offset in steal_code (past ENDBR64)
        InsnInfo info;
    };
    StolenInsn insns[16];
    size_t insn_count = 0;
    size_t steal_total = 0;  // bytes decoded past ENDBR64

    // We need to overwrite at least HOOK_SIZE bytes from byte 0, so the
    // stolen instruction region must cover HOOK_SIZE - endbr64_skip bytes
    // (since ENDBR64 is also overwritten but not relocated to trampoline).
    size_t steal_needed = (HOOK_SIZE > endbr64_skip) ? (HOOK_SIZE - endbr64_skip) : 0;

    while (steal_total < steal_needed) {
        if (insn_count >= 16) {
            hlog("[CK3-Bridge] ERROR: Too many instructions in prologue at 0x%lx\n",
                    target_addr);
            return nullptr;
        }

        InsnInfo info = decode_insn(steal_code + steal_total);

        if (info.length == 0) {
            hlog("[CK3-Bridge] ERROR: Unrecognized instruction at 0x%lx + %zu "
                    "(byte: 0x%02x). Refusing to hook.\n",
                    steal_addr, steal_total, steal_code[steal_total]);
            return nullptr;
        }

        insns[insn_count].src_offset = steal_total;
        insns[insn_count].info = info;
        insn_count++;

        steal_total += info.length;

        if (steal_total > MAX_STOLEN_BYTES) {
            hlog("[CK3-Bridge] ERROR: Prologue at 0x%lx requires copying %zu bytes "
                    "(max %zu). Refusing to hook.\n",
                    target_addr, steal_total, MAX_STOLEN_BYTES);
            return nullptr;
        }
    }

    // Total bytes overwritten at the target = ENDBR64 + decoded instructions
    size_t total = endbr64_skip + steal_total;

    // Log what we're doing
    hlog("[CK3-Bridge] Hooking 0x%lx: overwriting %zu bytes (%zu ENDBR64 + %zu stolen in %zu instructions)\n",
            target_addr, total, endbr64_skip, steal_total, insn_count);

    // Hex dump of stolen prologue for debugging
    {
        char hexbuf[MAX_STOLEN_BYTES * 3 + 1];
        size_t pos = 0;
        for (size_t b = 0; b < total && pos + 3 < sizeof(hexbuf); b++) {
            pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "%02x ", code[b]);
        }
        hlog("[CK3-Bridge]   Prologue bytes: %s\n", hexbuf);
    }

    // Allocate trampoline slot from pool
    uint8_t* trampoline = alloc_trampoline_slot();
    if (!trampoline) {
        hlog("[CK3-Bridge] ERROR: Failed to allocate trampoline slot\n");
        return nullptr;
    }

    // Save original bytes
    memcpy(entry->original_bytes, code, total);
    entry->stolen_len = total;

    // Build trampoline: copy stolen instructions (past ENDBR64) with RIP-relative fixups.
    // ENDBR64 is NOT included in the trampoline — it's only needed at indirect
    // branch targets, and the trampoline is called directly via function pointer.
    size_t tramp_offset = 0;

    for (size_t i = 0; i < insn_count; i++) {
        const StolenInsn& si = insns[i];
        const uint8_t* insn_src = steal_code + si.src_offset;
        uintptr_t orig_insn_addr = steal_addr + si.src_offset;

        if (!si.info.has_rip_relative) {
            // Simple copy — no relocation needed
            memcpy(trampoline + tramp_offset, insn_src, si.info.length);
            tramp_offset += si.info.length;
            continue;
        }

        // RIP-relative instruction — need to fix up displacement
        uintptr_t tramp_insn_addr = reinterpret_cast<uintptr_t>(trampoline) + tramp_offset;

        if (si.info.is_branch) {
            // Branch instruction (CALL rel32, JMP rel32, JMP rel8, Jcc rel8/rel32)
            // Compute absolute target, then emit as an absolute or fixed-up branch

            intptr_t orig_disp;
            if (si.info.branch_disp_size == 1) {
                orig_disp = static_cast<int8_t>(insn_src[si.info.branch_disp_offset]);
            } else {
                int32_t d;
                memcpy(&d, insn_src + si.info.branch_disp_offset, 4);
                orig_disp = d;
            }

            // Absolute target = original_insn_addr + insn_length + displacement
            uintptr_t abs_target = orig_insn_addr + si.info.length + orig_disp;

            // For short branches (rel8), we must expand to rel32 or use indirect.
            // For CALL/JMP rel32, we can patch if the new displacement fits in int32.
            // For conditional branches (Jcc rel8), expand to Jcc rel32 (0F 8x).

            uint8_t opcode_byte = insn_src[0];
            // Skip prefixes to find actual opcode
            const uint8_t* op_p = insn_src;
            while ((*op_p >= 0x40 && *op_p <= 0x4F) || *op_p == 0x66 ||
                   *op_p == 0xF2 || *op_p == 0xF3 || *op_p == 0x2E || *op_p == 0x3E ||
                   *op_p == 0x26 || *op_p == 0x36 || *op_p == 0x64 || *op_p == 0x65 || *op_p == 0x67) {
                op_p++;
            }
            opcode_byte = *op_p;
            size_t prefix_len = (size_t)(op_p - insn_src);

            if (opcode_byte >= 0x70 && opcode_byte <= 0x7F) {
                // Short Jcc rel8 -> expand to Jcc rel32 (0F 8x XX XX XX XX)
                // Copy any prefixes first
                memcpy(trampoline + tramp_offset, insn_src, prefix_len);
                size_t wo = tramp_offset + prefix_len;
                trampoline[wo] = 0x0F;
                trampoline[wo + 1] = 0x80 + (opcode_byte - 0x70); // Jcc rel32
                // New instruction length = prefix_len + 2 (opcodes) + 4 (disp32)
                size_t new_insn_len = prefix_len + 6;
                uintptr_t new_insn_end = reinterpret_cast<uintptr_t>(trampoline) + tramp_offset + new_insn_len;
                int64_t new_disp64 = (int64_t)abs_target - (int64_t)new_insn_end;
                if (new_disp64 < INT32_MIN || new_disp64 > INT32_MAX) {
                    hlog("[CK3-Bridge] ERROR: Jcc rel8->rel32 expansion at 0x%lx + %zu: "
                            "displacement 0x%lx out of int32 range\n",
                            target_addr, si.src_offset, (unsigned long)new_disp64);
                    return nullptr;
                }
                int32_t new_disp = static_cast<int32_t>(new_disp64);
                memcpy(trampoline + wo + 2, &new_disp, 4);
                tramp_offset += new_insn_len;
                hlog("[CK3-Bridge]   Relocated Jcc rel8 -> Jcc rel32 at +%zu (target 0x%lx)\n",
                        si.src_offset, (unsigned long)abs_target);

            } else if (opcode_byte == 0xEB) {
                // Short JMP rel8 -> JMP rel32 (0xE9 XX XX XX XX)
                memcpy(trampoline + tramp_offset, insn_src, prefix_len);
                size_t wo = tramp_offset + prefix_len;
                trampoline[wo] = 0xE9;
                size_t new_insn_len = prefix_len + 5;
                uintptr_t new_insn_end = reinterpret_cast<uintptr_t>(trampoline) + tramp_offset + new_insn_len;
                int64_t new_disp64 = (int64_t)abs_target - (int64_t)new_insn_end;
                if (new_disp64 < INT32_MIN || new_disp64 > INT32_MAX) {
                    hlog("[CK3-Bridge] ERROR: JMP rel8->rel32 expansion: displacement out of range\n");
                    return nullptr;
                }
                int32_t new_disp = static_cast<int32_t>(new_disp64);
                memcpy(trampoline + wo + 1, &new_disp, 4);
                tramp_offset += new_insn_len;
                hlog("[CK3-Bridge]   Relocated JMP rel8 -> JMP rel32 at +%zu\n", si.src_offset);

            } else if (opcode_byte == 0xE8 || opcode_byte == 0xE9) {
                // CALL rel32 or JMP rel32 — patch displacement
                memcpy(trampoline + tramp_offset, insn_src, si.info.length);
                uintptr_t new_insn_end = tramp_insn_addr + si.info.length;
                int64_t new_disp64 = (int64_t)abs_target - (int64_t)new_insn_end;
                if (new_disp64 < INT32_MIN || new_disp64 > INT32_MAX) {
                    // Too far — use indirect: push abs_target on stack + ret (for CALL)
                    // or mov rax, abs_target; jmp rax (for JMP)
                    // For simplicity, use mov rax + jmp/call rax
                    if (opcode_byte == 0xE9) {
                        // JMP: mov rax, imm64; jmp rax
                        write_abs_jmp(trampoline + tramp_offset, reinterpret_cast<void*>(abs_target));
                        tramp_offset += HOOK_SIZE;
                    } else {
                        // CALL: mov rax, imm64; call rax
                        uint8_t* t = trampoline + tramp_offset;
                        t[0] = 0x48; t[1] = 0xB8; // MOV RAX, imm64
                        memcpy(t + 2, &abs_target, 8);
                        t[10] = 0xFF; t[11] = 0xD0; // CALL RAX
                        tramp_offset += 12;
                    }
                    hlog("[CK3-Bridge]   Relocated %s rel32 -> indirect at +%zu\n",
                            opcode_byte == 0xE8 ? "CALL" : "JMP", si.src_offset);
                } else {
                    int32_t new_disp = static_cast<int32_t>(new_disp64);
                    memcpy(trampoline + tramp_offset + si.info.branch_disp_offset, &new_disp, 4);
                    tramp_offset += si.info.length;
                    hlog("[CK3-Bridge]   Relocated %s rel32 at +%zu (disp 0x%x -> 0x%x)\n",
                            opcode_byte == 0xE8 ? "CALL" : "JMP", si.src_offset,
                            (unsigned)(int32_t)orig_disp, (unsigned)new_disp);
                }

            } else if (opcode_byte == 0x0F) {
                // Jcc rel32 (0F 80-8F) — patch displacement
                memcpy(trampoline + tramp_offset, insn_src, si.info.length);
                uintptr_t new_insn_end = tramp_insn_addr + si.info.length;
                int64_t new_disp64 = (int64_t)abs_target - (int64_t)new_insn_end;
                if (new_disp64 < INT32_MIN || new_disp64 > INT32_MAX) {
                    hlog("[CK3-Bridge] ERROR: Jcc rel32 at +%zu: displacement out of int32 range\n",
                            si.src_offset);
                    return nullptr;
                }
                int32_t new_disp = static_cast<int32_t>(new_disp64);
                memcpy(trampoline + tramp_offset + si.info.branch_disp_offset, &new_disp, 4);
                tramp_offset += si.info.length;
                hlog("[CK3-Bridge]   Relocated Jcc rel32 at +%zu\n", si.src_offset);
            }
        } else {
            // ModR/M-based RIP-relative addressing [rip+disp32]
            // Copy the instruction, then patch the disp32 field
            memcpy(trampoline + tramp_offset, insn_src, si.info.length);

            if (si.info.rip_disp_offset > 0) {
                // Original: RIP at end of instruction + disp32 = absolute target
                int32_t orig_disp;
                memcpy(&orig_disp, insn_src + si.info.rip_disp_offset, 4);

                uintptr_t abs_target = orig_insn_addr + si.info.length + orig_disp;
                uintptr_t new_insn_end = tramp_insn_addr + si.info.length;
                int64_t new_disp64 = (int64_t)abs_target - (int64_t)new_insn_end;

                if (new_disp64 < INT32_MIN || new_disp64 > INT32_MAX) {
                    hlog("[CK3-Bridge] ERROR: RIP-relative fixup at 0x%lx + %zu: "
                            "new displacement 0x%lx exceeds int32 range\n",
                            target_addr, si.src_offset, (unsigned long)new_disp64);
                    return nullptr;
                }

                int32_t new_disp = static_cast<int32_t>(new_disp64);
                memcpy(trampoline + tramp_offset + si.info.rip_disp_offset, &new_disp, 4);

                hlog("[CK3-Bridge]   Relocated RIP-relative at +%zu (disp 0x%x -> 0x%x, target 0x%lx)\n",
                        si.src_offset, (unsigned)orig_disp, (unsigned)new_disp,
                        (unsigned long)abs_target);
            }

            tramp_offset += si.info.length;
        }
    }

    // Write JMP back to (target + total), i.e. the first un-stolen instruction.
    // CRITICAL: Use JMP rel32 (5 bytes) instead of MOV RAX + JMP RAX (12 bytes)
    // because the stolen prologue may contain CALL instructions whose return value
    // in RAX must be preserved. MOV RAX clobbers it; JMP rel32 does not.
    uintptr_t return_addr = steal_addr + steal_total;
    uintptr_t jmp_from = reinterpret_cast<uintptr_t>(trampoline + tramp_offset) + 5; // end of JMP rel32
    int64_t jmp_disp = static_cast<int64_t>(return_addr) - static_cast<int64_t>(jmp_from);
    if (jmp_disp >= INT32_MIN && jmp_disp <= INT32_MAX) {
        // JMP rel32 — preserves all registers
        trampoline[tramp_offset] = 0xE9;
        int32_t disp32 = static_cast<int32_t>(jmp_disp);
        memcpy(trampoline + tramp_offset + 1, &disp32, 4);
        tramp_offset += 5;
        hlog("[CK3-Bridge]   Trampoline tail: JMP rel32 (preserves RAX)\n");
    } else {
        // Fallback: use R11 (caller-saved scratch register, not used for params)
        // MOV R11, imm64; JMP R11 = 13 bytes
        trampoline[tramp_offset + 0] = 0x49;
        trampoline[tramp_offset + 1] = 0xBB; // MOV R11, imm64
        memcpy(trampoline + tramp_offset + 2, &return_addr, 8);
        trampoline[tramp_offset + 10] = 0x41;
        trampoline[tramp_offset + 11] = 0xFF;
        trampoline[tramp_offset + 12] = 0xE3; // JMP R11
        tramp_offset += 13;
        hlog("[CK3-Bridge]   Trampoline tail: MOV R11 + JMP R11 (preserves RAX, fallback)\n");
    }

    // --- INT3-first atomic hook installation ---
    // Ensures no thread can execute a partial instruction sequence.
    // 1. Write INT3 (0xCC) to byte 0 — any thread hitting the function traps
    // 2. Memory barrier
    // 3. Write bytes 1-11 (rest of MOV RAX + JMP RAX)
    // 4. Memory barrier
    // 5. Write byte 0 with the real first byte (0x48 = REX.W of MOV RAX)
    {
        ScopedWritable guard(reinterpret_cast<void*>(target_addr), total);

        uint8_t* target_bytes = reinterpret_cast<uint8_t*>(target_addr);

#ifdef _WIN32
        g_install_lo.store(target_addr);
        g_install_hi.store(target_addr + total);
#endif
        // Step 1: INT3 at byte 0 — atomically traps any thread entering
        target_bytes[0] = 0xCC;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Step 2: Write the rest of the JMP sequence (bytes 1-11)
        // Prepare the full 12-byte sequence in a local buffer
        uint8_t jmp_buf[HOOK_SIZE];
        jmp_buf[0] = 0x48;  // REX.W (will be written last)
        jmp_buf[1] = 0xB8;  // MOV RAX, imm64
        memcpy(jmp_buf + 2, &hook_fn, 8);
        jmp_buf[10] = 0xFF;  // JMP RAX
        jmp_buf[11] = 0xE0;

        // Write bytes 1 through HOOK_SIZE-1
        memcpy(target_bytes + 1, jmp_buf + 1, HOOK_SIZE - 1);

        // NOP-pad any remaining bytes beyond HOOK_SIZE
        for (size_t i = HOOK_SIZE; i < total; i++) {
            target_bytes[i] = 0x90; // NOP
        }

        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Step 3: Write byte 0 with the real opcode — hook is now live
        target_bytes[0] = 0x48;  // REX.W prefix of MOV RAX, imm64
#ifdef _WIN32
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        g_install_hi.store(0);
        g_install_lo.store(0);
#endif
    }
    // ScopedWritable destructor restores R+X permissions

    // Record the hook
    entry->target = target_addr;
    entry->trampoline = trampoline;
    entry->active = true;

    return trampoline;
}

bool remove_hook(uintptr_t target_addr) {
    for (size_t i = 0; i < MAX_HOOKS; i++) {
        if (g_hooks[i].active && g_hooks[i].target == target_addr) {
            // Restore original bytes (full stolen length, including any NOP-padded area)
            {
                ScopedWritable guard(reinterpret_cast<void*>(target_addr), g_hooks[i].stolen_len);
                memcpy(reinterpret_cast<void*>(target_addr),
                       g_hooks[i].original_bytes, g_hooks[i].stolen_len);
            }

            // Fill trampoline slot with INT3 (no munmap — it's a pool)
            if (g_hooks[i].trampoline) {
                memset(g_hooks[i].trampoline, 0xCC, TRAMPOLINE_SLOT_SIZE);
            }

            g_hooks[i].active = false;
            return true;
        }
    }
    return false;
}

} // namespace bridge
