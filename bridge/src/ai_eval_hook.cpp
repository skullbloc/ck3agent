// ai_eval_hook.cpp — Intent execution infrastructure: execute_direct_dispatch,
// drain_and_execute_intents, TLS discovery, queue_intent.
//
// ARCHITECTURE CHANGE: The old hooked_ai_interaction_decision (which fired
// 10,000+ times per tick across parallel threads) has been removed. All intent
// execution now happens via drain_and_execute_intents(), called from on_tick()
// in the serial section BEFORE parallel AI workers launch. This eliminates
// deadlock/hang caused by mutex access in the parallel AI eval hot path.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "named_trigger_eval.h"
#include "runtime_resolver.h"
#include "cb_outer_filter.h"
#include "pipeline.h"                 // ck-6kvq.10: the pulse ledger (§2.9)
#include <Zydis/Zydis.h>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <intrin.h>                    // __readgsqword: the TLS block (P6)
#include "windows/ck3_offsets_win.h"   // the TLS context and the destructor registration (P6)
#endif

namespace bridge {

// Global deferred acceptance query slot
AcceptanceQuery g_acceptance_query = {};

// Global deferred CB validity query slot
CBQuery g_cb_query CK3_EARLY_INIT = {};
// H3: cb_query_busy return counter (instrumentation; see header comment).
std::atomic<uint64_t> g_cb_query_busy_count{0};

// =============================================================================
// D8 hardening (probe P4, minidumps 07-31 20:48 + 08-01 19:41): shared
// CB-option output buffer for every CB_VALIDITY_CHECK call in this file.
//
// CB_VALIDITY_CHECK's second arg is a CDynArray it APPENDS 0x98-byte option
// entries to. When count hits capacity the engine grows the array THROUGH THE
// SUPPLIED ALLOCATOR — and pdx_array::CB_OPTION_ALLOCATOR_VTABLE does NOT
// point at a valid allocator vtable in CK3 1.18.3.1 (both minidumps:
// count==capacity==32, then an indirect call through garbage → SIGSEGV four
// frames inside engine code; the bridge's sigsetjmp net CANNOT catch it
// because CK3's own crash handler owns the sigaction — P4.5). Claim-family
// option counts grow with realm/claim size, so the old per-site 32-entry
// STACK buffers became reachable cliffs (deterministic recurrence as the
// campaign advanced), with the secondary hazard that a growth would hand the
// allocator's free() a stack address.
//
// Defense (P4.7 item 1 — the engine must never be able to reach the growth
// path): one BSS buffer sized far beyond any plausible option enumeration,
// shared by the three call sites, plus a loud post-call tripwire (ADLOG +
// CBQuery::option_capacity_hits) if an enumeration ever fills it. All three
// sites run exclusively on the GAME thread (drain_and_execute_intents /
// execute_war_intent paths), so a single shared buffer is race-free.
static constexpr uint32_t kCBOptionEntryCap = 512;
alignas(16) static uint8_t g_cb_option_storage[kCBOptionEntryCap *
                                               pdx_array::CB_OPTION_ENTRY_STRIDE];

// Global deferred trigger evaluation query slot
TriggerQuery g_trigger_query = {};

// TLS offsets discovered at runtime (fall back to hardcoded if discovery fails)
static uintptr_t g_tls_guard_offset   = 0x6e8;   // default from current binary
static uintptr_t g_tls_context_offset = 0x3a78;   // default from current binary

// ck-62eo: WHICH of those two the pipeline is actually running on. Discovery
// failure was logged once at init and then indistinguishable from success
// everywhere downstream, so a TLS fault could not say whether the offsets were
// measured from this binary or inherited from a different one. In production
// discovery is failing as the NORMAL case, which makes the distinction the
// first thing a reader of such a fault needs.
static bool g_tls_offsets_discovered = false;

// ── ck-62eo: THE TLS GUARD BYTE, READ AND WRITTEN IN ONE PLACE ─────────────
//
// This address is DERIVED, not observed: fs_base minus g_tls_guard_offset,
// where that offset is a HARDCODED FALLBACK whenever discovery fails — and
// discovery fails as the NORMAL case in production ("TLS offset discovery
// failed, using hardcoded defaults (guard=0x6e8, context=0x3a78)" appears in
// both the crashed episode and a separate running probe). So both accesses were
// unvalidated touches of an address computed from a constant the running binary
// may not agree with.
//
// They were raw — no is_mapped, no protected region — while the SAME functions
// is_mapped() the TLS_CONSTRUCT function pointer thirty lines later and wrap
// the call in sigsetjmp. Each validated the pointer it was about to CALL and
// not the pointer it had already DEREFERENCED. Same asymmetry as ck-cg3b and
// ck-duv5: the capability was present and the earlier step lacked it.
//
// IT LIVES HERE BECAUSE THERE WERE TWO COPIES. execute_direct_dispatch and
// process_trigger_query each had their own read-and-write of this byte. I fixed
// one and the acceptance caught the other by COUNTING dereferences instead of
// checking the site I had just edited — two copies of a routine are how one
// gets fixed and the other does not. One declaration, several readers.
enum class TlsGuard { Ready, NeedsConstruct, Unreadable };

// AND THE OBVIOUS READ IS WRONG. safe_read<uint8_t>() returns T{} on failure,
// which is 0 — and 0 is exactly the value meaning "construct the context".
// That would trade a crash for a silent decision to run construction against
// garbage, which is the worse direction. safe_read_bytes returns a bool, so
// "could not read" stays distinguishable from "read a zero", and a guard state
// that cannot be established is a refusal rather than a default.
static TlsGuard read_tls_guard(const uint8_t* guard_byte) {
    uint8_t v = 0;
    if (!safe_read_bytes(reinterpret_cast<uintptr_t>(guard_byte), &v, 1)) {
        ADLOG("TLS_GUARD: byte at %p is UNREADABLE (offset -0x%lx, source=%s) — "
              "REFUSING rather than assuming a guard state\n",
              (const void*)guard_byte, (unsigned long)g_tls_guard_offset,
              g_tls_offsets_discovered ? "discovered" : "HARDCODED_FALLBACK");
        return TlsGuard::Unreadable;
    }
#ifdef _WIN32
    // MSVC packs the thread_local guards of a block into one dword, one bit each:
    // this context's is bit 0 (AI_INTERACTION_CORE: test al,1 / or eax,1).
    return (v & 1) == 0 ? TlsGuard::NeedsConstruct : TlsGuard::Ready;
#else
    return v == 0 ? TlsGuard::NeedsConstruct : TlsGuard::Ready;
#endif
}

// The STORE is guarded too. A readable address is not necessarily writable, and
// this writes to an address derived from the same fallback offset — the more
// dangerous of the two accesses, because a wrong offset here CORRUPTS whatever
// it lands on rather than merely faulting. sigsetjmp is the pattern both
// callers already use for TLS_CONSTRUCT; there is no safe_write primitive.
static bool set_tls_guard(uint8_t* guard_byte) {
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
#ifdef _WIN32
        *guard_byte |= 1;   // the other bits are other thread_locals' guards
#else
        *guard_byte = 1;
#endif
        g_in_protected_region = false;
        return true;
    }
    g_in_protected_region = false;
    ADLOG("TLS_GUARD: SIGSEGV writing the guard byte at %p — the TLS context "
          "was constructed but the guard could not be set\n", (void*)guard_byte);
    return false;
}

// Runtime-discovered vtable and dispatch manager offsets.
// Populated by init_runtime_discovery() during first-tick init.
// Default to hardcoded values; overridden if Zydis discovery succeeds.
static uintptr_t g_send_cmd_vtable_offset  = ck3::funcs::SEND_INTERACTION_CMD_VTABLE;  // CSendCharacterInteractionCommand vtable (from ctor at 0x609b410)
static uintptr_t g_send_cmd_vtable2_offset = ck3::funcs::SEND_INTERACTION_CMD_VTABLE2;  // CGameCommandHelper vtable (from ctor at 0x609b410)
static uintptr_t g_dispatch_mgr_offset     = ck3::globals::COMMAND_DISPATCH_MGR;  // COMMAND_DISPATCH_MGR (DAT_0c69dd68 - 0x100000)

// Address of BUILD_SEND_CMD_FROM_BUF (CSendCharacterInteractionCommand ctor).
// This function does: base_ctor + overwrite_vtables + deep_copy + compute_hashes.
// Set by init_runtime_discovery() to g_base_addr + BUILD_SEND_CMD_FROM_BUF directly.
static uintptr_t g_real_build_send_cmd_addr = 0;

// Controlled character selection (Phase A)
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::vector<uint32_t> g_controlled_chars CK3_EARLY_INIT;
std::mutex g_controlled_mutex;

// =============================================================================
// TLS Offset Auto-Discovery
// =============================================================================

// Scan the AI_INTERACTION_CORE function for FS-segment memory references.
// The function uses two TLS variables accessed via FS:
//   1. A guard byte (cmp byte ptr fs:[rip+disp32], 0) -- smaller offset (~0x6e8)
//   2. An interaction context pointer (mov reg, fs:[rip+disp32]) -- larger offset (~0x3a78)
// These offsets change with recompilation, so we extract them from the machine code.
static bool discover_tls_offsets() {
    // NOTE: The TLS references (guard byte at FS[-0x6e8] and context at FS[-0x3a78])
    // are in AI_INTERACTION_CORE (FUN_04dc4540), NOT AI_INTERACTION_DECISION (FUN_04dc4b90).
    // AI_INTERACTION_DECISION is the outer wrapper; CORE is the callee that actually
    // accesses the thread-local interaction context.
    uintptr_t func_addr = bridge::game_fn(ck3::funcs::AI_INTERACTION_CORE);
    if (!is_mapped(func_addr)) {
        ADLOG("TLS_DISCOVER: AI_INTERACTION_CORE at 0x%lx is not mapped\n", func_addr);
        return false;
    }

    // Decode up to 512 bytes — the TLS references are near the function prologue
    // but AI_INTERACTION_CORE is 1482 bytes; 512 gives comfortable margin.
    static constexpr size_t SCAN_SIZE = 512;
    const uint8_t* code = reinterpret_cast<const uint8_t*>(func_addr);

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    // Collect all FS-relative RIP-displacement values
    std::vector<uintptr_t> fs_offsets;
    size_t offset = 0;

    while (offset < SCAN_SIZE) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset,
                                                  SCAN_SIZE - offset, &insn, operands))) {
            offset++;
            continue;
        }

        // Look for memory operands with FS segment override
        for (int i = 0; i < insn.operand_count_visible; i++) {
            if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[i].mem.segment == ZYDIS_REGISTER_FS) {
                // The TLS access pattern uses RIP-relative addressing with FS override.
                // The displacement is a negative value relative to FS base.
                // In x86-64 ELF TLS, the actual TLS offset comes from the linker
                // and appears as the RIP+disp32 resolved absolute in the GOT/TLS area.
                //
                // However, the Ghidra decompilation shows in_FS_OFFSET[-0x6e8] etc.,
                // meaning the compiler generates: fs:[0] to get the TLS base, then
                // subtracts the offset. We need to find the displacement values.
                //
                // Pattern 1 (guard check): 64 80 3c 25 XX XX XX XX 00
                //   = cmp byte ptr fs:[abs32], 0
                // Pattern 2 (context load): 64 48 8b 04 25 XX XX XX XX
                //   = mov rax, qword ptr fs:[abs32]
                //
                // With Zydis, the absolute address appears as mem.disp.value
                if (operands[i].mem.disp.value != 0) {
                    int64_t disp = operands[i].mem.disp.value;
                    // TLS offsets in x86-64 Linux are negative from FS base,
                    // so the absolute address = FS_BASE + disp (where disp is negative).
                    // The compiler emits fs:[negative_value] which wraps around.
                    // Convert to the positive offset from FS base.
                    uintptr_t abs_offset;
                    if (disp < 0) {
                        abs_offset = static_cast<uintptr_t>(-disp);
                    } else {
                        // fs:[positive] means FS_BASE - (2^64 - disp) ...
                        // Actually for TLS __thread vars, the offset stored is the
                        // negative offset directly. The linker resolves the GOT entry
                        // to contain the negative offset from FS base.
                        // If positive but large, it wraps: treat as unsigned and negate
                        if (disp > 0x7FFFFFFF) {
                            abs_offset = static_cast<uintptr_t>(
                                -static_cast<int64_t>(static_cast<uint64_t>(disp)));
                        } else {
                            // Small positive offset -- unlikely for TLS but record it
                            abs_offset = static_cast<uintptr_t>(disp);
                        }
                    }

                    ADLOG("TLS_DISCOVER: FS-segment ref at func+0x%zx: "
                          "disp=0x%lx (abs_offset=0x%lx) insn_mnemonic=%u\n",
                          offset, (unsigned long)disp, (unsigned long)abs_offset,
                          insn.mnemonic);
                    if (abs_offset > 0 && abs_offset < 0x100000) {
                        fs_offsets.push_back(abs_offset);
                    }
                }
            }
        }

        offset += insn.length;
    }

    if (fs_offsets.size() < 2) {
        ADLOG("TLS_DISCOVER: Found only %zu FS-segment refs (need at least 2)\n",
              fs_offsets.size());
        return false;
    }

    // Sort and deduplicate
    std::sort(fs_offsets.begin(), fs_offsets.end());
    fs_offsets.erase(std::unique(fs_offsets.begin(), fs_offsets.end()), fs_offsets.end());

    if (fs_offsets.size() < 2) {
        ADLOG("TLS_DISCOVER: Only %zu unique FS offsets after dedup\n", fs_offsets.size());
        return false;
    }

    // The guard offset is the smaller value, the context offset is the larger
    uintptr_t guard = fs_offsets[0];
    uintptr_t context = fs_offsets[fs_offsets.size() - 1];

    // Sanity checks: guard should be small (< 0x2000), context should be larger
    if (guard >= context) {
        ADLOG("TLS_DISCOVER: Unexpected ordering: guard=0x%lx >= context=0x%lx\n",
              (unsigned long)guard, (unsigned long)context);
        return false;
    }
    if (guard > 0x4000 || context > 0x20000) {
        ADLOG("TLS_DISCOVER: Offsets out of expected range: guard=0x%lx context=0x%lx\n",
              (unsigned long)guard, (unsigned long)context);
        return false;
    }

    g_tls_guard_offset = guard;
    g_tls_context_offset = context;
    ADLOG("TLS_DISCOVER: SUCCESS -- guard=0x%lx context=0x%lx (from %zu FS refs)\n",
          (unsigned long)guard, (unsigned long)context, fs_offsets.size());
    return true;
}

// =============================================================================
// AI Interaction Decision Hook
// =============================================================================

// Extract the interaction type name from an interaction object.
const char* get_interaction_name(void* interaction) {
    if (!interaction) return "<null>";
    uintptr_t obj = reinterpret_cast<uintptr_t>(interaction);
    uintptr_t str_ptr = read<uintptr_t>(obj + ck3::db_object::NAME_KEY);
    if (!str_ptr || str_ptr < 0x10000) return "<unnamed>";
    uint8_t first = read<uint8_t>(str_ptr);
    if (first >= 0x20 && first < 0x7F) {
        return reinterpret_cast<const char*>(str_ptr);
    }
    return "<unnamed>";
}

// Get the hash of an interaction's type name
uint32_t get_interaction_hash(void* interaction) {
    const char* name = get_interaction_name(interaction);
    if (!name || name[0] == '<') return 0;
    return game_string_hash(name, static_cast<uint32_t>(strlen(name)));
}

// =============================================================================
// Direct Command Construction
// =============================================================================

// Get the FS base register (thread-local storage base) on x86-64 Linux
static uintptr_t get_fs_base() {
    uintptr_t fs = 0;
    syscall(SYS_arch_prctl, ARCH_GET_FS, &fs);
    return fs;
}

// Function pointer types for interaction context pipeline
using fn_ctx_init       = void(*)(uintptr_t* tls_ctx, void* interaction, void* character);
using fn_ctx_reset      = void(*)(uintptr_t* tls_ctx);
using fn_ctx_set_target = void(*)(uintptr_t* tls_ctx, uint32_t target_char_id, char do_setup);
using fn_ctx_validate   = uint64_t(*)(uintptr_t* tls_ctx);
using fn_try_send       = char(*)(uintptr_t* tls_ctx, long extra);
using fn_build_send_cmd = void(*)(void** out_cmd, uintptr_t tls_ctx);
using fn_game_cmd_dispatch = uint64_t(*)(void* dispatch_mgr, void** cmd_ptr, uint32_t priority, int zero);

// =============================================================================
// Scoring Hook — CALL-site patching approach
// =============================================================================
// Instead of hooking GetAiWillDoScore's function prologue (which corrupts
// .eh_frame FDE entries and causes crashes during C++ exception unwinding),
// we patch the CALL instruction inside AI_INTERACTION_CORE (FUN_04dc4540) that
// invokes GetAiWillDoScore. This is safe because:
//   - No function prologue is modified
//   - .eh_frame FDE entries are unaffected (they only describe prologues)
//   - The patch is mid-function, invisible to the DWARF unwinder
//   - Only 4 bytes (the rel32 offset of CALL) are changed
//
// When g_force_target_id is non-zero, the hooked GetAiWillDoScore returns
// MAX_SCORE for the forced target and the real score for everyone else.
// This makes AI_INTERACTION_DECISION reliably select our desired target.
//
// Thread safety: g_force_target_id is only set during POST-TICK serial section
// (drain_and_execute_intents). During orig(ai_manager) parallel workers, it's 0.

static std::atomic<uint32_t> g_force_target_id{0};
static bool g_score_hook_installed = false;

// Address of the 4-byte rel32 field inside the CALL instruction (call_insn_addr + 1)
static uintptr_t g_score_call_site = 0;
// Original 4-byte rel32 displacement (points to real GetAiWillDoScore)
static uint8_t g_original_rel32[4] = {};
// Patched 4-byte rel32 displacement (points to our trampoline -> hook)
static uint8_t g_patch_rel32[4] = {};

// Hook for GetAiWillDoScore (FUN_06b441c0, 96 bytes)
// param_1 = TLS interaction context pointer
// Target char ref is at context + 0x2DC
//
// IMPORTANT: Unlike the old prologue-hook approach, we call the ORIGINAL
// GetAiWillDoScore directly (its prologue is intact — we never modified it).
// The decision, shared by both ABIs' entry points below.
static int64_t score_with_forced_target(long* param_1, int64_t real_score) {
    uint32_t forced = g_force_target_id.load(std::memory_order_acquire);
    if (forced != 0) {
        uint32_t target_ref = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uintptr_t>(param_1) + ck3::interaction_ctx::TARGET_CHAR_REF);
        if ((target_ref & 0xFFFFFF) == (forced & 0xFFFFFF)) {
            // Forced target: return astronomical score to guarantee selection
            return 999999999LL;
        }
        // Non-targets: return original score (not 0!)
        // Returning 0 caused AI_INTERACTION_DECISION to think no candidates were valid.
        return real_score;
    }

    return real_score;
}

#ifndef _WIN32
static int64_t hooked_GetAiWillDoScore(long* param_1) {
    // Call the original scoring function directly (prologue is untouched)
    auto orig = reinterpret_cast<int64_t(*)(long*)>(
        bridge::game_fn(ck3::funcs::GET_AI_WILL_DO_SCORE));
    int64_t real_score = 0;
    try {
        real_score = orig(param_1);
    } catch (...) {
        real_score = 0;
    }
    return score_with_forced_target(param_1, real_score);
}
#else
// MSVC (ck3_offsets_win.h funcs::GET_AI_WILL_DO_SCORE): a member function that
// returns the fixed-point score through a hidden pointer, ctx in rcx and the slot
// in rdx, returning the slot. The call site passes them so; this entry point
// takes them so, and writes the same decision into the slot.
static int64_t* hooked_GetAiWillDoScore(long* param_1, int64_t* out) {
    auto orig = reinterpret_cast<int64_t* (*)(long*, int64_t*)>(
        bridge::game_fn(ck3::funcs::GET_AI_WILL_DO_SCORE));
    int64_t real_score = 0;
    try {
        orig(param_1, &real_score);
    } catch (...) {
        real_score = 0;
    }
    *out = score_with_forced_target(param_1, real_score);
    return out;
}
#endif

// Write bytes to an arbitrary address via /proc/self/mem (no mprotect needed).
// Works on read-only .text pages.
static bool write_code_bytes(uintptr_t addr, const void* data, size_t len) {
#ifdef _WIN32
    // No /proc/self/mem: protect, write, restore and flush (os::write_protected).
    return write_to_readonly(reinterpret_cast<void*>(addr), data, len);
#endif
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) return false;
    ssize_t written = pwrite(fd, data, len, static_cast<off_t>(addr));
    close(fd);
    return written == static_cast<ssize_t>(len);
}

// Prepare the CALL-site hook (called during first-tick init).
// Scans AI_INTERACTION_CORE for the CALL to GetAiWillDoScore, builds a
// trampoline near .text, and prepares the arm/disarm patch bytes.
void install_score_hook() {
    uintptr_t core_fn = bridge::game_fn(ck3::funcs::AI_INTERACTION_CORE);
    uintptr_t score_fn = bridge::game_fn(ck3::funcs::GET_AI_WILL_DO_SCORE);

    if (!is_mapped(core_fn)) {
        ADLOG("SCORE_HOOK: AI_INTERACTION_CORE at 0x%lx not mapped, skipping\n", core_fn);
        return;
    }
    if (!is_mapped(score_fn)) {
        ADLOG("SCORE_HOOK: GetAiWillDoScore at 0x%lx not mapped, skipping\n", score_fn);
        return;
    }

    // Scan AI_INTERACTION_CORE for E8 (CALL rel32) instructions targeting GetAiWillDoScore.
    // FUN_04dc4540 is ~1520 bytes. We scan up to 2048 to be safe.
    const uint8_t* code = reinterpret_cast<const uint8_t*>(core_fn);
    bool found = false;
    for (size_t i = 0; i < 2048; i++) {
        if (code[i] == 0xE8) {
            int32_t rel32;
            memcpy(&rel32, code + i + 1, 4);
            uintptr_t target = core_fn + i + 5 + static_cast<int64_t>(rel32);
            if (target == score_fn) {
                g_score_call_site = core_fn + i + 1;  // address of the rel32 bytes
                memcpy(g_original_rel32, code + i + 1, 4);  // save original
                ADLOG("SCORE_HOOK: Found CALL site at 0x%lx (offset +0x%zx in AI_INTERACTION_CORE)\n",
                      core_fn + i, i);
                found = true;
                break;
            }
        }
    }

    if (!found) {
        ADLOG("SCORE_HOOK: CALL to GetAiWillDoScore not found in AI_INTERACTION_CORE "
              "(scanned %zu bytes from 0x%lx targeting 0x%lx)\n", (size_t)2048, core_fn, score_fn);
        return;
    }

    // Our hook function is in libck3_bridge.so, which is likely > 2GB away from
    // the CK3 .text segment. A rel32 CALL can only reach ±2GB. Solution: allocate
    // a small trampoline in the pool (which IS within ±2GB of .text) that does:
    //   MOV RAX, <hooked_GetAiWillDoScore>; JMP RAX   (12 bytes)
    uint8_t* trampoline = alloc_trampoline_slot();
    if (!trampoline) {
        ADLOG("SCORE_HOOK: Failed to allocate trampoline slot\n");
        g_score_call_site = 0;
        return;
    }

    // Write absolute jump to our hook into the trampoline
    uintptr_t hook_fn = reinterpret_cast<uintptr_t>(hooked_GetAiWillDoScore);
    trampoline[0] = 0x48;  // REX.W
    trampoline[1] = 0xB8;  // MOV RAX, imm64
    memcpy(trampoline + 2, &hook_fn, 8);
    trampoline[10] = 0xFF;  // JMP RAX
    trampoline[11] = 0xE0;

    ADLOG("SCORE_HOOK: Trampoline at %p -> hook at 0x%lx\n",
          trampoline, hook_fn);

    // Compute the rel32 from the CALL instruction to the trampoline.
    // CALL instruction is at (g_score_call_site - 1), so the rel32 resolves
    // relative to (g_score_call_site - 1 + 5) = (g_score_call_site + 4).
    uintptr_t trampoline_addr = reinterpret_cast<uintptr_t>(trampoline);
    int64_t delta = static_cast<int64_t>(trampoline_addr) -
                    static_cast<int64_t>(g_score_call_site + 4);

    if (delta > INT32_MAX || delta < INT32_MIN) {
        ADLOG("SCORE_HOOK: Trampoline too far from CALL site (delta=%ld, max=±2GB)\n", delta);
        g_score_call_site = 0;
        return;
    }

    int32_t new_rel32 = static_cast<int32_t>(delta);
    memcpy(g_patch_rel32, &new_rel32, 4);

    g_score_hook_installed = true;
    ADLOG("SCORE_HOOK: Ready. CALL site=0x%lx, original_target=0x%lx, "
          "trampoline=%p, hook=0x%lx\n",
          g_score_call_site - 1, score_fn, trampoline, hook_fn);
}

// Temporarily activate the hook: patch the CALL's rel32 to point to our trampoline.
// Uses /proc/self/mem to write to read-only .text pages without mprotect.
static void arm_score_hook() {
    if (!g_score_hook_installed) return;
    write_code_bytes(g_score_call_site, g_patch_rel32, 4);
}

// Restore the CALL's original rel32 to point back to GetAiWillDoScore.
static void disarm_score_hook() {
    if (!g_score_hook_installed) return;
    write_code_bytes(g_score_call_site, g_original_rel32, 4);
}

// =============================================================================
// TLS Pipeline Dispatch — Direct command construction bypassing AI_INTERACTION_DECISION
// =============================================================================
//
// Replicates what FUN_04dc4540 does for its winning target, but with OUR
// specified target. This bypasses AI_INTERACTION_DECISION entirely:
//
//   1. Ensure TLS context is initialized (guard byte check)
//   2. CTX_INIT(tls_ctx, interaction, character)
//   3. CTX_RESET(tls_ctx)
//   4. SET_TARGET(tls_ctx, target_char_id, 1)
//   5. IsInteractionValid(tls_ctx) → bool
//   6. CheckAcceptance(tls_ctx, 0) → bool
//   7. BUILD_SEND_CMD_FROM_BUF(wrapper, tls_ctx)
//   8. Clone: wrapper.vtable[9](&clone, &wrapper)
//   9. GAME_COMMAND_DISPATCH(dispatch_mgr, &clone, 7, 0)
//  10. Cleanup: destroy wrapper via vtable[0]

// Function pointer types for TLS pipeline
using fn_tls_construct   = void(*)(void* tls_ctx);
using fn_tls_destructor  = void(*)(void* tls_ctx);

#ifndef _WIN32
} // namespace bridge (temporarily close to declare extern "C" at file scope)
extern "C" int __cxa_thread_atexit(void (*)(void*), void*, void*);
namespace bridge { // reopen
#endif
// The game's TLS-context destructor, registered the way the engine does it.
// Windows: the CRT's per-thread list, with the engine's own thunk (it re-derives
// the context from the TLS block and calls the destructor), as AI_INTERACTION_CORE
// registers it (ck3_offsets_win.h funcs::TLS_REGISTER_DTOR).
static inline void register_game_tls_dtor(void (*dtor)(void*), void* ctx) {
#ifndef _WIN32
    __cxa_thread_atexit(dtor, ctx, nullptr);
#else
    (void)dtor; (void)ctx;
    using fn_register = int (*)(void (*)());
    reinterpret_cast<fn_register>(g_base_addr + ck3_win::funcs::TLS_REGISTER_DTOR)(
        reinterpret_cast<void (*)()>(g_base_addr + ck3_win::funcs::INTERACTION_TLS_DTOR_THUNK));
#endif
}

#ifdef _WIN32
// This thread's game interaction context on Windows: ck3.exe's TLS block is slot 0
// of the TEB's TLS array ([gs:0x58]), the guard and the context at fixed offsets in
// it (ck3_offsets_win.h struct_layout::INTERACTION_TLS_*). Linux derives both from
// the FS base. 0 when the block cannot be read.
static uintptr_t game_tls_block() {
    const uintptr_t tls_array = __readgsqword(0x58);
    return tls_array ? safe_read<uintptr_t>(tls_array) : 0;
}
#endif

// This thread's game interaction context and its construction guard: Linux at fixed
// negative offsets from the FS base (init_tls_offsets), Windows in ck3.exe's TLS
// block. One routine for every caller that drives the context (the trigger query,
// the direct dispatch). nullptr on success, else the refusal reason.
static const char* locate_game_tls(uint8_t** guard_byte, void** tls_ctx) {
#ifdef _WIN32
    const uintptr_t block = game_tls_block();
    if (block == 0) return "tls_block_unreadable";
    *guard_byte = reinterpret_cast<uint8_t*>(block + ck3_win::struct_layout::INTERACTION_TLS_GUARD);
    *tls_ctx = reinterpret_cast<void*>(block + ck3_win::struct_layout::INTERACTION_TLS_CONTEXT);
#else
    uintptr_t fs_base = 0;
    syscall(SYS_arch_prctl, ARCH_GET_FS, &fs_base);
    if (fs_base == 0) return "tls_fs_base_zero";
    *guard_byte = reinterpret_cast<uint8_t*>(fs_base - g_tls_guard_offset);
    *tls_ctx = reinterpret_cast<void*>(fs_base - g_tls_context_offset);
#endif
    return nullptr;
}

// CSendCharacterInteractionCommand's virtual clone and destruction, per ABI. Itanium:
// [0] the complete destructor, [1] the deleting one, [9] clone(out, this). MSVC
// (ck3_offsets_win.h struct_layout::SEND_CMD_VTABLE_*): [0] the scalar deleting
// destructor (this, flags: 1 frees), clone(this, out) at [8]. The command object
// itself is the same 0x368 bytes on both.
static uintptr_t game_command_slot(void* cmd, size_t off) {
    const uintptr_t vt = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(cmd));
    return vt ? safe_read<uintptr_t>(vt + off) : 0;
}
static uintptr_t game_command_clone_fn(void* cmd) {
#ifdef _WIN32
    return game_command_slot(cmd, ck3_win::struct_layout::SEND_CMD_VTABLE_CLONE);
#else
    return game_command_slot(cmd, ck3::interaction_cmd::VTABLE_CLONE);
#endif
}
static void* clone_game_command(uintptr_t clone_fn, void* cmd) {
    void* out = nullptr;
#ifdef _WIN32
    reinterpret_cast<void* (*)(void*, void**)>(clone_fn)(cmd, &out);
#else
    reinterpret_cast<void* (*)(void**, void*)>(clone_fn)(&out, cmd);
#endif
    return out;
}
// Destroy and free a heap command (a clone the engine did not take).
static void delete_game_command(void* cmd) {
#ifdef _WIN32
    const uintptr_t f = game_command_slot(cmd, ck3_win::struct_layout::SEND_CMD_VTABLE_DELETING_DTOR);
    if (f && is_mapped(f)) reinterpret_cast<void* (*)(void*, uint32_t)>(f)(cmd, 1);
#else
    const uintptr_t f = game_command_slot(cmd, ck3::interaction_cmd::VTABLE_DELETING_DTOR);
    if (f && is_mapped(f)) reinterpret_cast<void (*)(void*)>(f)(cmd);
#endif
}
// Destroy a command in place (the stack-built wrapper), freeing nothing.
static bool destroy_game_command_in_place(void* cmd) {
    const uintptr_t f = game_command_slot(cmd, 0);
    if (!f || !is_mapped(f)) return false;
#ifdef _WIN32
    reinterpret_cast<void* (*)(void*, uint32_t)>(f)(cmd, 0);
#else
    reinterpret_cast<void (*)(void*)>(f)(cmd);
#endif
    return true;
}

// DEAD_FOR_WAR_PER_CKE58L17.1 — declare_war_interaction is NOT a viable caller of
// this function. Production dispatch now routes WAR_DECLARE intents through
// execute_war_intent (action_handlers.cpp:680, ai_eval_hook.cpp:3803). The
// previously-present war-specific branches (CB injection, Gate-2 stamp,
// recorder bypass) have been stripped per ck-e58l.33 / audit FIX-6.
//
// Keep this function for non-war targeted interactions: marriage, gift,
// sway, vassalization, imprison, etc. See is_direct_dispatch_interaction()
// for the current whitelist.
static bool execute_tls_pipeline_dispatch(Intent* intent, void* character, void* interaction) {
    uint32_t actor_id = read<uint32_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::ID);
    uint32_t target_id = intent->target_id;

    ADLOG("TLS_PIPELINE: === BEGIN === interaction=%s actor=%u target=%u\n",
          intent->interaction_name.c_str(), actor_id & 0xFFFFFF, target_id & 0xFFFFFF);

    if (target_id == 0) {
        ADLOG("TLS_PIPELINE: target_id is 0 — cannot use TLS pipeline without explicit target\n");
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "tls_pipeline_requires_target");
        return false;
    }

    // --- Step 1: locate this thread's TLS context and ensure it is initialized ---
    uint8_t* guard_byte = nullptr;
    void* tls_ctx = nullptr;
    if (const char* tls_err = locate_game_tls(&guard_byte, &tls_ctx)) {
        ADLOG("TLS_PIPELINE: cannot locate the TLS context (%s)\n", tls_err);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "%s", tls_err);
        return false;
    }
    ADLOG("TLS_PIPELINE: guard_byte at %p, tls_ctx at %p\n", guard_byte, tls_ctx);

    const TlsGuard guard_state = read_tls_guard(guard_byte);
    if (guard_state == TlsGuard::Unreadable) {
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "tls_guard_unreadable");
        return false;
    }

    // Check TLS guard — if NeedsConstruct, the context has to be built
    if (guard_state == TlsGuard::NeedsConstruct) {
        ADLOG("TLS_PIPELINE: TLS guard is 0 — constructing context\n");

        auto tls_construct = reinterpret_cast<fn_tls_construct>(
            bridge::game_fn(ck3::funcs::INTERACTION_TLS_CONSTRUCT));

        if (!is_mapped(reinterpret_cast<uintptr_t>(tls_construct))) {
            ADLOG("TLS_PIPELINE: TLS_CONSTRUCT at 0x%lx not mapped\n",
                  bridge::game_fn(ck3::funcs::INTERACTION_TLS_CONSTRUCT));
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "tls_construct_not_mapped");
            return false;
        }

        // Protected call: construct TLS context
        watchdog_arm(3);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
            ADLOG("TLS_PIPELINE: TLS_CONSTRUCT crashed (%s)\n", sig_name);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "tls_construct_%s", sig_name);
            return false;
        }
        try {
            tls_construct(tls_ctx);
        } catch (...) {
            g_in_protected_region = false;
            watchdog_disarm();
            ADLOG("TLS_PIPELINE: TLS_CONSTRUCT threw exception\n");
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "tls_construct_exception");
            return false;
        }
        g_in_protected_region = false;
        watchdog_disarm();

        // Register destructor via __cxa_thread_atexit for proper cleanup
        // Signature: int __cxa_thread_atexit(void (*dtor)(void*), void* obj, void* dso_handle)
        auto tls_dtor = reinterpret_cast<fn_tls_destructor>(
            bridge::game_fn(ck3::funcs::INTERACTION_TLS_DESTRUCTOR));
        // dso_handle: use the address of a known global in the CK3 binary
        // The original code uses &PTR (some global in the binary), but we can
        // pass nullptr since we're in the same module context as the game thread.
        register_game_tls_dtor(reinterpret_cast<void(*)(void*)>(tls_dtor), tls_ctx);

        if (!set_tls_guard(guard_byte)) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "tls_guard_unwritable");
            return false;
        }
        ADLOG("TLS_PIPELINE: TLS context constructed and guard set to 1\n");
    } else {
        ADLOG("TLS_PIPELINE: TLS guard already set — context initialized\n");
    }

    // --- Function pointer resolution ---
    auto ctx_init = reinterpret_cast<fn_ctx_init>(
        bridge::game_fn(ck3::funcs::INTERACTION_CTX_INIT));
    auto ctx_reset = reinterpret_cast<fn_ctx_reset>(
        bridge::game_fn(ck3::funcs::INTERACTION_CTX_RESET));
    auto ctx_set_target = reinterpret_cast<fn_ctx_set_target>(
        bridge::game_fn(ck3::funcs::INTERACTION_CTX_SET_TARGET));
    auto ctx_validate = reinterpret_cast<fn_ctx_validate>(
        bridge::game_fn(ck3::funcs::INTERACTION_CTX_VALIDATE));
    auto check_acceptance = reinterpret_cast<fn_try_send>(
        bridge::game_fn(ck3::funcs::INTERACTION_TRY_SEND));

    // Validate all function pointers are mapped
    if (!is_mapped(reinterpret_cast<uintptr_t>(ctx_init)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_reset)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_set_target)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_validate)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(check_acceptance))) {
        ADLOG("TLS_PIPELINE: one or more pipeline functions not mapped\n");
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "tls_pipeline_funcs_not_mapped");
        return false;
    }

    // BUILD_SEND_CMD address (real function body, after thunk)
    uintptr_t build_cmd_addr = g_real_build_send_cmd_addr;
#ifdef _WIN32
    // init_runtime_discovery reads Linux code shapes and is not run on Windows; the
    // constructor is a recipe there (os/windows/recipes.cpp).
    if (!build_cmd_addr) build_cmd_addr = bridge::game_fn(ck3::funcs::BUILD_SEND_CMD_FROM_BUF);
#endif
    if (!build_cmd_addr || !is_mapped(build_cmd_addr)) {
        ADLOG("TLS_PIPELINE: BUILD_SEND_CMD not resolved (0x%lx)\n", build_cmd_addr);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "build_send_cmd_not_resolved");
        return false;
    }

    // Dispatch manager — DAT_0c69dd68 is an INLINE STRUCT in .bss, NOT a pointer.
    // Pass the ADDRESS of the struct directly (same as v7 path).
    uintptr_t dispatch_mgr_ptr = g_base_addr + g_dispatch_mgr_offset;
    if (!is_mapped(dispatch_mgr_ptr)) {
        ADLOG("TLS_PIPELINE: dispatch_mgr not available (0x%lx)\n", dispatch_mgr_ptr);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "dispatch_mgr_not_available");
        return false;
    }

    // --- Steps 2-6: CTX_INIT → CTX_RESET → SET_TARGET → Validate → CheckAcceptance ---
    // All under SIGSEGV + SIGALRM protection
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        ADLOG("TLS_PIPELINE: pipeline crashed (%s)\n", sig_name);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "tls_pipeline_%s", sig_name);
        return false;
    }

    // DEAD_FOR_WAR_PER_CKE58L17.1: formerly `bool is_war = ...` and a Step 4.5
    // CB-injection block ran here for declare_war_interaction. declare_war no
    // longer reaches this function (routed through execute_war_intent). All
    // war-specific setup stripped.

    bool pipeline_ok = false;
    try {
        // Step 2: CTX_INIT(tls_ctx, interaction, character)
        ADLOG("TLS_PIPELINE: CTX_INIT(tls_ctx=%p, interaction=%p, character=%p)\n",
              tls_ctx, interaction, character);
        ctx_init(reinterpret_cast<uintptr_t*>(tls_ctx), interaction, character);

        // Step 3: CTX_RESET(tls_ctx)
        ADLOG("TLS_PIPELINE: CTX_RESET\n");
        ctx_reset(reinterpret_cast<uintptr_t*>(tls_ctx));

        // Step 4: SET_TARGET(tls_ctx, target_char_id, 1)
        ADLOG("TLS_PIPELINE: SET_TARGET(target=%u)\n", target_id & 0xFFFFFF);
        ctx_set_target(reinterpret_cast<uintptr_t*>(tls_ctx), target_id, 1);

        // --- Step 4.5 stripped: war CB injection removed (DEAD_FOR_WAR_PER_CKE58L17.1) ---
        // Previously: CASUS_BELLI_DB scan, AttachCBState/AttachCBTitle calls, and
        // ValidateScopes re-run for declare_war_interaction. Removed because
        // declare_war now goes through execute_war_intent (the AI war pipeline).

#if 0  // DEAD_FOR_WAR_PER_CKE58L17.1 — retained as block comment for archaeology
        if (is_war) {
            ADLOG("TLS_PIPELINE: WAR === begin CB injection for declare_war ===\n");
            ADLOG("TLS_PIPELINE: WAR — intent fields: title_id=%d cb_index=%d claimant_id=%u\n",
                  intent->title_id, intent->cb_index, intent->claimant_id & 0xFFFFFF);
            uint8_t* ctx_buf = reinterpret_cast<uint8_t*>(tls_ctx);

            // A) Set war_target and claimant on the TLS context
            *reinterpret_cast<uint32_t*>(ctx_buf + ck3::interaction_ctx::WAR_TARGET_REF) =
                static_cast<uint32_t>(target_id);
            uint32_t claimant = intent->claimant_id;
            if (claimant == 0) claimant = actor_id;
            *reinterpret_cast<uint32_t*>(ctx_buf + ck3::interaction_ctx::CLAIMANT_REF) = claimant;
            ADLOG("TLS_PIPELINE: WAR — A) war_target=%u, claimant=%u\n",
                  target_id & 0xFFFFFF, claimant & 0xFFFFFF);

            // B) Look up valid CB from CASUS_BELLI_DB
            uintptr_t cb_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CASUS_BELLI_DB);
            if (cb_db && is_mapped(cb_db)) {
                uintptr_t cb_arr = safe_read<uintptr_t>(cb_db + ck3::game_db::ENTRIES_ARRAY);
                int32_t cb_count = safe_read<int32_t>(cb_db + ck3::game_db::LIVE_COUNT);
                if (cb_count < 0 || cb_count > 500) cb_count = 0;

                ADLOG("TLS_PIPELINE: WAR — B) scanning %d CB types\n", cb_count);

                void* attacker = lookup_character(actor_id);
                void* defender = lookup_character(target_id);

                if (attacker && defender && cb_arr && is_mapped(cb_arr)) {
                    auto cb_validity_check = reinterpret_cast<bool(*)(void*, void*, void*, void*, int)>(
                        bridge::game_fn(ck3::funcs::CB_VALIDITY_CHECK));

                    bool want_claim = (intent->title_id > 0);

                    // If cb_index is specified, use ONLY that CB — skip validity check
                    // and want_claim filtering. The agent already chose this CB.
                    if (intent->cb_index >= 0 && intent->cb_index < cb_count) {
                        volatile uintptr_t cb_type = safe_read<uintptr_t>(cb_arr + intent->cb_index * 8);
                        if (cb_type && is_mapped(cb_type)) {
                            war_cb_type = cb_type;
                            const char* name_ptr = nullptr;
                            size_t name_len = 0;
                            if (read_db_obj_name(cb_type, name_ptr, name_len)) {
                                size_t copy_len = (name_len < sizeof(war_cb_name) - 1) ? name_len : sizeof(war_cb_name) - 1;
                                memcpy(war_cb_name, name_ptr, copy_len);
                                war_cb_name[copy_len] = '\0';
                            }
                            ADLOG("TLS_PIPELINE: WAR — B) using specified cb_index=%d: %s (ptr=0x%lx)\n",
                                  intent->cb_index, war_cb_name, war_cb_type);
                        } else {
                            ADLOG("TLS_PIPELINE: WAR — B) specified cb_index=%d has invalid pointer — aborting (no auto-select)\n",
                                  intent->cb_index);
                        }
                    } else if (intent->cb_index >= 0) {
                        ADLOG("TLS_PIPELINE: WAR — B) specified cb_index=%d out of range (count=%d) — aborting (no auto-select)\n",
                              intent->cb_index, cb_count);
                    } else {
                    // Auto-select: cb_index not specified, find first valid CB
                    for (int32_t i = 0; i < cb_count && i < 100; i++) {
                        volatile uintptr_t cb_type = safe_read<uintptr_t>(cb_arr + i * 8);
                        if (!cb_type || !is_mapped(cb_type)) continue;

                        uint8_t enabled = safe_read<uint8_t>(cb_type + ck3::cb_type::ENABLED_FLAG);
                        if (!enabled) continue;

                        if (want_claim) {
                            int32_t tmode = safe_read<int32_t>(cb_type + ck3::cb_type::TARGET_MODE);
                            if (tmode != ck3::cb_type::TARGET_CLAIM) continue;
                        }

                        char cb_name_buf[128] = {};
                        const char* name_ptr = nullptr;
                        size_t name_len = 0;
                        if (read_db_obj_name(cb_type, name_ptr, name_len)) {
                            size_t copy_len = (name_len < sizeof(cb_name_buf) - 1) ? name_len : sizeof(cb_name_buf) - 1;
                            memcpy(cb_name_buf, name_ptr, copy_len);
                            cb_name_buf[copy_len] = '\0';
                        }

                        // CB_VALIDITY_CHECK with SIGSEGV recovery (uses inner sigsetjmp)
                        bool valid = false;
                        watchdog_arm(2);
                        g_in_protected_region = true;
                        int cb_sig = sigsetjmp(g_recovery_point, 1);
                        if (cb_sig != 0) {
                            g_in_protected_region = false;
                            watchdog_disarm();
                            ADLOG("TLS_PIPELINE: WAR — CB_VALIDITY_CHECK crashed for %s (sig=%d)\n",
                                  cb_name_buf, cb_sig);
                            continue;
                        }
                        try {
                            // D8 hardening: shared large BSS buffer instead of a
                            // 32-entry stack array — see g_cb_option_storage.
                            memset(g_cb_option_storage, 0, sizeof(g_cb_option_storage));
                            void* alloc_vtable = reinterpret_cast<void*>(g_base_addr + pdx_array::CB_OPTION_ALLOCATOR_VTABLE);
                            struct {
                                void*    data;
                                uint32_t capacity;
                                uint32_t count;
                                void**   allocator;
                            } cb_output_list;
                            void* alloc_obj = alloc_vtable;
                            cb_output_list.data = g_cb_option_storage;
                            cb_output_list.capacity = kCBOptionEntryCap;
                            cb_output_list.count = 0;
                            cb_output_list.allocator = &alloc_obj;

                            valid = cb_validity_check(
                                reinterpret_cast<void*>(cb_type),
                                &cb_output_list,
                                attacker, defender, 0);
                            if (cb_output_list.count >= kCBOptionEntryCap) {
                                ADLOG("TLS_PIPELINE: WAR — D8 GUARD: CB option enumeration "
                                      "for %s filled the %u-entry buffer — the fatal engine "
                                      "growth path was one entry away; raise kCBOptionEntryCap\n",
                                      cb_name_buf, kCBOptionEntryCap);
                            }
                        } catch (...) {
                            valid = false;
                            ADLOG("TLS_PIPELINE: WAR — CB_VALIDITY_CHECK exception for %s (idx=%d)\n",
                                  cb_name_buf, i);
                        }
                        g_in_protected_region = false;
                        watchdog_disarm();

                        ADLOG("TLS_PIPELINE: WAR — CB_VALIDITY_CHECK(%s, idx=%d) returned %d\n",
                              cb_name_buf, i, valid ? 1 : 0);

                        if (valid) {
                            war_cb_type = cb_type;
                            strncpy(war_cb_name, cb_name_buf, sizeof(war_cb_name) - 1);
                            ADLOG("TLS_PIPELINE: WAR — B) FOUND valid CB: %s (ptr=0x%lx, idx=%d)\n",
                                  war_cb_name, war_cb_type, i);
                            break;
                        }
                    }
                    } // end auto-select branch
                } else {
                    ADLOG("TLS_PIPELINE: WAR — B) attacker/defender lookup failed\n");
                }
            } else {
                ADLOG("TLS_PIPELINE: WAR — B) CASUS_BELLI_DB not found\n");
            }

            if (war_cb_type == 0) {
                ADLOG("TLS_PIPELINE: WAR — no valid CB found for actor=%u target=%u — aborting\n",
                      actor_id & 0xFFFFFF, target_id & 0xFFFFFF);
                snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                         "no_valid_cb_for_target");
                g_in_protected_region = false;
                watchdog_disarm();
                return false;
            }

            // C) AttachCBState — write cb_type_ref to tls_ctx+0x338
            uintptr_t attach_state_addr = bridge::game_fn(ck3::funcs::WAR_ATTACH_STATE);
            if (is_mapped(attach_state_addr)) {
                auto attach_cb_state = reinterpret_cast<void(*)(void*, uintptr_t)>(attach_state_addr);
                ADLOG("TLS_PIPELINE: WAR — C) AttachCBState(ctx+0x330, cb=0x%lx) [%s]\n",
                      war_cb_type, war_cb_name);
                attach_cb_state(ctx_buf + 0x330, war_cb_type);
            } else {
                // Direct write fallback
                *reinterpret_cast<uintptr_t*>(ctx_buf + 0x338) = war_cb_type;
            }
            ADLOG("TLS_PIPELINE: WAR — C) verify ctx+0x338 = 0x%lx\n",
                  *reinterpret_cast<uintptr_t*>(ctx_buf + 0x338));

            // D) AttachCBTitle — populate title array if CB requires titles
            ADLOG("TLS_PIPELINE: WAR — D) checking title_id=%d (>0 required)\n", intent->title_id);
            if (intent->title_id > 0) {
                static thread_local uint32_t s_tls_war_title_buf[16] = {};
                s_tls_war_title_buf[0] = static_cast<uint32_t>(intent->title_id);

                // Pre-set destination capacity and data pointer
                *reinterpret_cast<uintptr_t*>(ctx_buf + 0x340) = reinterpret_cast<uintptr_t>(s_tls_war_title_buf);
                *reinterpret_cast<int32_t*>(ctx_buf + 0x348) = 16;  // capacity
                *reinterpret_cast<int32_t*>(ctx_buf + 0x34C) = 0;   // count (AttachCBTitle sets this)

                // Build title_source struct: +0x00=claimant_ref, +0x08=title_ids_ptr, +0x14=count
                // WAR_ATTACH_CB copies source[0] to dest+0x28 which maps to CLAIMANT_REF (cb+0x198)
                alignas(16) uint8_t title_source[0x20] = {};
                *reinterpret_cast<uint32_t*>(title_source + 0x00) = claimant;  // claimant char ref (copied to cmd+0x358 = CLAIMANT_REF)
                *reinterpret_cast<uintptr_t*>(title_source + 0x08) = reinterpret_cast<uintptr_t>(s_tls_war_title_buf);
                *reinterpret_cast<int32_t*>(title_source + 0x14) = 1;   // count = 1 title

                uintptr_t attach_cb_addr = bridge::game_fn(ck3::funcs::WAR_ATTACH_CB);
                if (is_mapped(attach_cb_addr)) {
                    auto attach_cb_title = reinterpret_cast<void(*)(void*, void*)>(attach_cb_addr);
                    ADLOG("TLS_PIPELINE: WAR — D) AttachCBTitle(ctx+0x330, title_id=%d)\n",
                          intent->title_id);

                    watchdog_arm(3);
                    g_in_protected_region = true;
                    int title_sig = sigsetjmp(g_recovery_point, 1);
                    if (title_sig != 0) {
                        g_in_protected_region = false;
                        watchdog_disarm();
                        ADLOG("TLS_PIPELINE: WAR — D) AttachCBTitle crashed (sig=%d) — direct write fallback\n",
                              title_sig);
                        *reinterpret_cast<uintptr_t*>(ctx_buf + 0x340) = reinterpret_cast<uintptr_t>(s_tls_war_title_buf);
                        *reinterpret_cast<int32_t*>(ctx_buf + 0x348) = 16;
                        *reinterpret_cast<int32_t*>(ctx_buf + 0x34C) = 1;
                        *reinterpret_cast<uint32_t*>(ctx_buf + 0x358) = claimant;  // claimant char ref
                    } else {
                        try {
                            attach_cb_title(ctx_buf + 0x330, title_source);
                            ADLOG("TLS_PIPELINE: WAR — D) AttachCBTitle completed\n");
                        } catch (...) {
                            ADLOG("TLS_PIPELINE: WAR — D) AttachCBTitle exception — direct write fallback\n");
                            *reinterpret_cast<uintptr_t*>(ctx_buf + 0x340) = reinterpret_cast<uintptr_t>(s_tls_war_title_buf);
                            *reinterpret_cast<int32_t*>(ctx_buf + 0x348) = 16;
                            *reinterpret_cast<int32_t*>(ctx_buf + 0x34C) = 1;
                            *reinterpret_cast<uint32_t*>(ctx_buf + 0x358) = claimant;  // claimant char ref
                        }
                        g_in_protected_region = false;
                        watchdog_disarm();
                    }
                } else {
                    // Direct write fallback
                    *reinterpret_cast<uintptr_t*>(ctx_buf + 0x340) = reinterpret_cast<uintptr_t>(s_tls_war_title_buf);
                    *reinterpret_cast<int32_t*>(ctx_buf + 0x348) = 16;
                    *reinterpret_cast<int32_t*>(ctx_buf + 0x34C) = 1;
                    *reinterpret_cast<uint32_t*>(ctx_buf + 0x358) = claimant;  // claimant char ref
                }

                ADLOG("TLS_PIPELINE: WAR — D) verify: title_count=%d\n",
                      *reinterpret_cast<int32_t*>(ctx_buf + 0x34C));
            } else {
                ADLOG("TLS_PIPELINE: WAR — D) no title_id specified (title_id=%d), skipping AttachCBTitle\n",
                      intent->title_id);
            }

            // E) Stamp game date at ctx+0x2C0 (for Execute staleness check)
            uintptr_t gs_root = safe_read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
            int32_t game_date = 0;
            if (gs_root != 0) {
                game_date = safe_read<int32_t>(gs_root + ck3::game_state::GAME_DATE);
            }
            if (game_date != 0) {
                *reinterpret_cast<int32_t*>(ctx_buf + 0x2C0) = game_date;
                ADLOG("TLS_PIPELINE: WAR — E) stamped game_date=%d at ctx+0x2C0\n", game_date);
            }

            // F) Rebuild scope paths after manual edits
            uintptr_t validate_scopes_addr = bridge::game_fn(ck3::funcs::INTERACTION_CTX_VALIDATE_SCOPES);
            if (is_mapped(validate_scopes_addr)) {
                auto validate_scopes = reinterpret_cast<void(*)(void*, int)>(validate_scopes_addr);
                ADLOG("TLS_PIPELINE: WAR — F) ValidateScopes after CB injection\n");
                validate_scopes(reinterpret_cast<void*>(tls_ctx), 0);
                ADLOG("TLS_PIPELINE: WAR — F) ValidateScopes completed\n");
            }

            ADLOG("TLS_PIPELINE: WAR === CB injection complete (cb=%s) ===\n", war_cb_name);

            // Note: inner sigsetjmp calls (CB validity, AttachCBTitle) overwrote
            // g_recovery_point, so SIGSEGV recovery is unavailable for the remainder of
            // this try block. This is safe because:
            //   - For wars, CTX_VALIDATE is skipped (see Step 5 below)
            //   - Acceptance check is skipped (wars are unilateral)
            //   - No game functions are called until the next sigsetjmp in Steps 7-9
        }
#endif  // DEAD_FOR_WAR_PER_CKE58L17.1 (Step 4.5 war CB injection)

        // Step 5: IsInteractionValid(tls_ctx)
        ADLOG("TLS_PIPELINE: CTX_VALIDATE\n");
        uint64_t valid_result = ctx_validate(reinterpret_cast<uintptr_t*>(tls_ctx));
#ifdef _WIN32
        valid_result &= 0xFF;   // MSVC returns the verdict in al (ck3_offsets_win.h)
#endif
        if (valid_result == 0) {
            ADLOG("TLS_PIPELINE: CTX_VALIDATE returned 0 — interaction not valid for this target\n");
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "interaction_not_valid_for_target");
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }
        ADLOG("TLS_PIPELINE: CTX_VALIDATE returned 0x%lx (valid)\n", (unsigned long)valid_result);

        // Step 6: CheckAcceptance(tls_ctx, 0) → bool
        ADLOG("TLS_PIPELINE: CHECK_ACCEPTANCE\n");
        char acceptance = check_acceptance(reinterpret_cast<uintptr_t*>(tls_ctx), 0);
        if (!acceptance) {
            ADLOG("TLS_PIPELINE: CHECK_ACCEPTANCE returned 0 — target would reject\n");
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "target_would_reject");
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }
        ADLOG("TLS_PIPELINE: CHECK_ACCEPTANCE returned %d (accepted)\n", (int)acceptance);

        pipeline_ok = true;
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        ADLOG("TLS_PIPELINE: pipeline threw C++ exception\n");
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "tls_pipeline_exception");
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!pipeline_ok) return false;

    // --- Steps 7-9: BUILD_SEND_CMD → Clone → DISPATCH ---
    // Build the wrapper command object on the stack
    // CSendCharacterInteractionCommand is a large object. From RE analysis, the
    // ctor at BUILD_SEND_CMD writes vtable pointers and deep-copies from the TLS
    // context buffer. The wrapper size from decompilation: 0x368 bytes.
    alignas(16) uint8_t wrapper_buf[0x400] = {};  // generous padding
    void* wrapper_ptr = &wrapper_buf[0];

    watchdog_arm(5);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        ADLOG("TLS_PIPELINE: BUILD/CLONE/DISPATCH crashed (%s)\n", sig_name);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "tls_build_dispatch_%s", sig_name);
        return false;
    }

    bool dispatched = false;
    try {
        // Step 7: BUILD_SEND_CMD_FROM_BUF(wrapper, tls_ctx)
        // The real function signature: void fn(void* out_cmd, uintptr_t tls_ctx_addr)
        // where tls_ctx_addr is the address of the TLS context (not a pointer to it).
        auto build_send_cmd = reinterpret_cast<void(*)(void*, uintptr_t)>(build_cmd_addr);
        ADLOG("TLS_PIPELINE: BUILD_SEND_CMD(wrapper=%p, tls_ctx=0x%lx) via 0x%lx\n",
              wrapper_ptr, reinterpret_cast<uintptr_t>(tls_ctx), build_cmd_addr);
        build_send_cmd(wrapper_ptr, reinterpret_cast<uintptr_t>(tls_ctx));

        // Validate: wrapper should now have a vtable pointer at offset 0
        uintptr_t wrapper_vtable = *reinterpret_cast<uintptr_t*>(wrapper_ptr);
        if (!wrapper_vtable || !is_mapped(wrapper_vtable)) {
            ADLOG("TLS_PIPELINE: wrapper vtable invalid after BUILD_SEND_CMD (0x%lx)\n",
                  wrapper_vtable);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "wrapper_vtable_invalid");
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }
        ADLOG("TLS_PIPELINE: wrapper vtable = 0x%lx\n", wrapper_vtable);

        // Step 8: Clone via the vtable (Linux [9], MSVC [8]: game_command_clone_fn)
        uintptr_t clone_fn_ptr = game_command_clone_fn(wrapper_ptr);
        if (!clone_fn_ptr || !is_mapped(clone_fn_ptr)) {
            ADLOG("TLS_PIPELINE: clone vtable slot invalid (0x%lx)\n", clone_fn_ptr);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "clone_vtable_entry_invalid");
            destroy_game_command_in_place(wrapper_ptr);   // cleanup wrapper before returning
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }

        // Clone allocates internally via operator new(0x368) and returns it through
        // an out pointer (argument order per ABI: clone_game_command).
        ADLOG("TLS_PIPELINE: CLONE via vtable slot at 0x%lx (wrapper=%p)\n",
              clone_fn_ptr, wrapper_ptr);
        void* clone_ptr = clone_game_command(clone_fn_ptr, wrapper_ptr);

        // Step 8.5 stripped: DEAD_FOR_WAR_PER_CKE58L17.1 — Gate-2 interaction_ptr
        // stamp and command-recorder bypass were war-specific workarounds for
        // CSendCharInteractionCmd::Execute. declare_war no longer reaches this
        // function, so these workarounds are unnecessary.

        // Step 9: Check dispatch gate + GAME_COMMAND_DISPATCH
        // The game checks (DAT_0c69dfb8 & 0xfd) == 0 before dispatching.
        // If the gate is set, skip dispatch (the engine is in a restricted state).
        uint8_t dispatch_gate = safe_read<uint8_t>(g_base_addr + ck3::globals::COMMAND_DISPATCH_GATE);
        if ((dispatch_gate & 0xfd) != 0) {
            ADLOG("TLS_PIPELINE: dispatch gate blocked (gate=0x%02x, 0xfd mask=0x%02x) — skipping dispatch\n",
                  dispatch_gate, dispatch_gate & 0xfd);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "dispatch_gate_blocked_0x%02x", dispatch_gate);
            // Destroy clone since we won't dispatch it
            delete_game_command(clone_ptr);
        } else {
            auto game_dispatch = reinterpret_cast<fn_game_cmd_dispatch>(
                bridge::game_fn(ck3::funcs::GAME_COMMAND_DISPATCH));

            ADLOG("TLS_PIPELINE: GAME_COMMAND_DISPATCH(mgr=0x%lx, clone=%p, priority=7, 0)\n",
                  dispatch_mgr_ptr, clone_ptr);
            game_dispatch(reinterpret_cast<void*>(dispatch_mgr_ptr), &clone_ptr, 7, 0);

            // Post-dispatch: check if engine took ownership (nulled clone_ptr)
            // If not nulled, destroy the clone ourselves (dispatch may have failed)
            if (clone_ptr != nullptr) {
                ADLOG("TLS_PIPELINE: clone_ptr not nulled by dispatch — destroying\n");
                delete_game_command(clone_ptr);
            }

            dispatched = true;
            ADLOG("TLS_PIPELINE: GAME_COMMAND_DISPATCH returned — command sent\n");
        }

        // Step 8.5 recorder-restore removed alongside the earlier Step 8.5 stamp.

        // Step 10: Cleanup wrapper (stack-allocated: destroy in place, free nothing)
        if (!destroy_game_command_in_place(wrapper_ptr)) {
            ADLOG("TLS_PIPELINE: WARNING — wrapper dtor not mapped, skipping cleanup\n");
        }

    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        ADLOG("TLS_PIPELINE: BUILD/CLONE/DISPATCH threw C++ exception\n");
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "tls_build_dispatch_exception");
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (dispatched) {
        ADLOG("TLS_PIPELINE: === SUCCESS === %s sent (actor=%u, target=%u)\n",
              intent->interaction_name.c_str(), actor_id & 0xFFFFFF, target_id & 0xFFFFFF);
    }

    return dispatched;
}

// =============================================================================
// Direct Command Construction (v7) — bypasses TLS pipeline AND AI_INTERACTION_DECISION
// =============================================================================
//
// Complete pipeline for hostile/targeted interactions:
//   1. BUILD_SEND_INTERACTION_CMD(cmd_buf, interaction, actor_id, target_id, 0, 1)
//   2. BUILD_SEND_CMD_FROM_BUF(wrapper, cmd_buf)  — deep copy, sets vtables
//   3. wrapper.vtable[9](&clone, &wrapper)          — heap-allocated clone
//   4. GAME_COMMAND_DISPATCH(dispatch_mgr, &clone, 7, 0)
//   5. Cleanup: destroy cmd_buf via INTERACTION_TLS_DESTRUCTOR, destroy wrapper via vtable[0]
//
// This path does NOT touch the TLS interaction context at all. It constructs the
// command buffer directly from the interaction definition + actor/target IDs, then
// feeds it through the same ctor → clone → dispatch pipeline the engine uses.

// Function pointer types for direct command construction
using fn_build_send_interaction_cmd = void(*)(void* cmd_buf, void* interaction,
                                               uint32_t actor_id, uint32_t target_id,
                                               int zero, int one);
using fn_build_send_cmd_from_buf = void(*)(void* wrapper, void* cmd_buf);
using fn_interaction_data_dtor = void(*)(void* cmd_buf);

// List of interactions that should use direct command construction (v7).
// These are hostile or scheme-like interactions where AI_INTERACTION_DECISION's
// target iteration / scoring is unnecessary or counterproductive (e.g., murder
// targets are not in the AI's normal ai_targets pool).
static bool is_direct_dispatch_interaction(const char* name) {
    // TLS pipeline (v6) dispatch list: interactions that work via CTX_INIT -> SET_TARGET ->
    // VALIDATE -> TRY_SEND -> BUILD_CMD -> DISPATCH with just actor+target.
    // revoke_title_interaction additionally requires a title-scope target written at
    // cmd+0x2F0 (16-byte PDX object ref); see Step 1.5a2 below. The intent struct
    // carries title_id (intent_queue.h) which is resolved against LANDED_TITLE_DB
    // and packed into the scope value before ValidateScopes is called.
    //
    // DEAD_FOR_WAR_PER_CKE58L17.1: declare_war_interaction removed from this list.
    // declare_war now dispatches via execute_war_intent (action_handlers.cpp:680).
    // Keeping it in the list would route declare_war through the structurally
    // dead TLS/v7 path for INTERACTION-type intents — which is the opposite of
    // what ck-0ftj fixed.
    //
    // Name corrections (verified against live game list_interactions, 2026-04-01):
    //   start_fabricate_hook_interaction -> fabricate_hook_interaction
    //   start_seduce_interaction -> seduce_interaction
    //   start_abduct_interaction -> start_abduct (no _interaction suffix)
    //   start_learn_language_interaction -> learn_language_interaction
    //   force_vassalization_interaction -> offer_vassalization_interaction
    //   start_scheme_interaction -> REMOVED (does not exist)
    //   start_romance_interaction -> REMOVED (does not exist)
    //   negotiate_release_interaction -> REMOVED (does not exist)
    //
    // Added peaceful interactions (v5 AI_INTERACTION_DECISION fails for player-char actors):
    //   gift_interaction, sway_interaction, befriend_interaction
    static const char* direct_list[] = {
        // Hostile / scheme interactions
        "start_murder_interaction",
        "start_abduct",                  // NB: no _interaction suffix in game
        "fabricate_hook_interaction",     // was start_fabricate_hook_interaction
        "seduce_interaction",            // was start_seduce_interaction
        "learn_language_interaction",    // was start_learn_language_interaction
        // Diplomatic interactions
        "offer_vassalization_interaction", // was force_vassalization_interaction
        "negotiate_alliance_interaction",
        "call_ally_interaction",
        "demand_conversion_interaction",
        "arrange_marriage_interaction",
        // Vassal management
        "revoke_title_interaction",       // Phase 28: needs title scope at cmd+0x2F0 (set in Step 1.5a2)
        "imprison_interaction",
        "execute_prisoner_interaction",
        "ransom_interaction",
        "release_from_prison_interaction",
        // #126 (Gen-26 F2): disinherit_interaction — the death-free single-heir
        // lever. interaction_category_hostile, actor+recipient(target) only, applies
        // the `disinherited` trait + inheritance_blocker via disinherit_effect on
        // accept (cost: renown + minor legitimacy; NO tyranny/death). It was falling
        // through to the dead v5 AI_INTERACTION_DECISION path (which walks the actor's
        // own candidate pool and ignores the explicit target_id, so it mis-targeted /
        // no-op'd). It carries no scripted_effect scope frame and needs no secondary
        // title scope, so the v7 BUILD_SEND_INTERACTION_CMD(actor,target) path is
        // sufficient — same as imprison/execute above. No scope_ctx (#73) dependency.
        // Engine key verified: common/character_interactions/00_dynast_interactions.txt.
        "disinherit_interaction",
        // Peaceful interactions (must use TLS — v5 AI_INTERACTION_DECISION returns false
        // when actor is the player character, since AI decision functions skip player chars)
        "gift_interaction",
        "sway_interaction",
        "befriend_interaction",
        // A2/A3: these were falling through to v5 AI_INTERACTION_DECISION, which walks
        // the actor's OWN candidate pool and ignores the caller's explicit target_id —
        // so they always returned ai_no_valid_target even when discover(acceptance)
        // succeeded on the same target. Route them through the TLS pipeline so the
        // explicit target is honored. buy_claim is a secondary-scope interaction:
        // it needs the claim's title written at cmd+0x2F0 (pass title_id like
        // revoke_title); without that title scope the TLS path will still reject.
        "elope_interaction",
        "buy_claim_interaction",
        nullptr
    };
    for (const char** p = direct_list; *p; ++p) {
        if (strcmp(name, *p) == 0) return true;
    }
    return false;
}

// DEAD_FOR_WAR_PER_CKE58L17.1 — declare_war_interaction is NOT a viable caller of
// this v7 direct-command pipeline. Production declare_war routes through
// execute_war_intent (action_handlers.cpp:680 / ai_eval_hook.cpp:3803). The
// previously-present war Step 1.5b CB setup (CASUS_BELLI_DB scan, AttachCBState,
// AttachCBTitle, game-date stamp) has been stripped per ck-e58l.33.
//
// Keep for non-war direct-dispatch interactions (marriage spouse setup, revoke
// title scope, etc.) — see is_direct_dispatch_interaction().
static bool execute_direct_command_dispatch(Intent* intent, void* character, void* interaction) {
    // Use volatile to prevent longjmp clobber warnings
    volatile uint32_t actor_id = read<uint32_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::ID);
    volatile uint32_t target_id = intent->target_id;

    ADLOG("DIRECT_CMD_V7: === BEGIN === interaction=%s actor=%u target=%u\n",
          intent->interaction_name.c_str(), actor_id & 0xFFFFFF, target_id & 0xFFFFFF);

    if (target_id == 0) {
        ADLOG("DIRECT_CMD_V7: target_id=0 — direct command construction requires explicit target\n");
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "direct_cmd_requires_target");
        return false;
    }

    // --- Resolve function pointers ---
    auto build_interaction_cmd = reinterpret_cast<fn_build_send_interaction_cmd>(
        bridge::game_fn(ck3::funcs::BUILD_SEND_INTERACTION_CMD));
    auto build_send_cmd = reinterpret_cast<fn_build_send_cmd_from_buf>(
        bridge::game_fn(ck3::funcs::BUILD_SEND_CMD_FROM_BUF));
    auto interaction_dtor = reinterpret_cast<fn_interaction_data_dtor>(
        bridge::game_fn(ck3::funcs::INTERACTION_TLS_DESTRUCTOR));
    auto game_dispatch = reinterpret_cast<fn_game_cmd_dispatch>(
        bridge::game_fn(ck3::funcs::GAME_COMMAND_DISPATCH));

    // Validate all function pointers are mapped
    if (!is_mapped(reinterpret_cast<uintptr_t>(build_interaction_cmd))) {
        ADLOG("DIRECT_CMD_V7: BUILD_SEND_INTERACTION_CMD at 0x%lx not mapped\n",
              bridge::game_fn(ck3::funcs::BUILD_SEND_INTERACTION_CMD));
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "build_interaction_cmd_not_mapped");
        return false;
    }
    if (!is_mapped(reinterpret_cast<uintptr_t>(build_send_cmd))) {
        ADLOG("DIRECT_CMD_V7: BUILD_SEND_CMD_FROM_BUF at 0x%lx not mapped\n",
              bridge::game_fn(ck3::funcs::BUILD_SEND_CMD_FROM_BUF));
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "build_send_cmd_not_mapped");
        return false;
    }
    if (!is_mapped(reinterpret_cast<uintptr_t>(interaction_dtor))) {
        ADLOG("DIRECT_CMD_V7: INTERACTION_TLS_DESTRUCTOR at 0x%lx not mapped\n",
              bridge::game_fn(ck3::funcs::INTERACTION_TLS_DESTRUCTOR));
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "interaction_dtor_not_mapped");
        return false;
    }
    if (!is_mapped(reinterpret_cast<uintptr_t>(game_dispatch))) {
        ADLOG("DIRECT_CMD_V7: GAME_COMMAND_DISPATCH at 0x%lx not mapped\n",
              bridge::game_fn(ck3::funcs::GAME_COMMAND_DISPATCH));
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "game_dispatch_not_mapped");
        return false;
    }

    // Dispatch manager — DAT_0c69dd68 is an INLINE STRUCT in .bss, NOT a pointer.
    // Pass the ADDRESS of the struct directly. Do NOT dereference it.
    // The engine calls: FUN_07660390(&DAT_0c69dd68, &clone, 7, 0)
    // Inside, it accesses param_1+0x25a (gate), param_1+0x3E8 (queue), etc.
    uintptr_t dispatch_mgr_addr = g_base_addr + g_dispatch_mgr_offset;
    if (!is_mapped(dispatch_mgr_addr)) {
        ADLOG("DIRECT_CMD_V7: dispatch_mgr addr 0x%lx not mapped\n", dispatch_mgr_addr);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "dispatch_mgr_not_mapped");
        return false;
    }
    // Sanity: check that the struct has reasonable values at known offsets
    uint8_t gate_byte = safe_read<uint8_t>(dispatch_mgr_addr + ck3::dispatch_mgr::GATE_BYTE);
    uintptr_t queue_ptr = safe_read<uintptr_t>(dispatch_mgr_addr + ck3::dispatch_mgr::QUEUE_PTR);
    ADLOG("DIRECT_CMD_V7: dispatch_mgr at 0x%lx (gate=0x%x, queue=0x%lx)\n",
          dispatch_mgr_addr, gate_byte, queue_ptr);

    // Check dispatch gate before doing expensive work
    uint8_t dispatch_gate = safe_read<uint8_t>(g_base_addr + ck3::globals::COMMAND_DISPATCH_GATE);
    if ((dispatch_gate & 0xfd) != 0) {
        ADLOG("DIRECT_CMD_V7: dispatch gate blocked (gate=0x%02x) — skipping\n", dispatch_gate);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "dispatch_gate_blocked");
        return false;
    }

    // --- Step 1: BUILD_SEND_INTERACTION_CMD ---
    // Allocate cmd_buf on the stack (0x380 bytes, 16-byte aligned)
    alignas(16) uint8_t cmd_buf[ck3::interaction_cmd::CMD_BUF_SIZE] = {};

    ADLOG("DIRECT_CMD_V7: Step 1 — BUILD_SEND_INTERACTION_CMD(buf=%p, interaction=%p, "
          "actor=%u, target=%u, 0, 1)\n",
          cmd_buf, interaction, actor_id, target_id);

    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        ADLOG("DIRECT_CMD_V7: Step 1 BUILD_SEND_INTERACTION_CMD crashed (%s)\n", sig_name);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "build_interaction_cmd_%s", sig_name);
        return false;
    }

    try {
        build_interaction_cmd(cmd_buf, interaction, actor_id, target_id, 0, 1);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        ADLOG("DIRECT_CMD_V7: Step 1 BUILD_SEND_INTERACTION_CMD threw exception\n");
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "build_interaction_cmd_exception");
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    ADLOG("DIRECT_CMD_V7: Step 1 complete — cmd_buf populated\n");

    // --- Step 1.5a: Marriage spouse setup (arrange_marriage_interaction only) ---
    //
    // Marriage dispatch requires 2 extra char refs beyond actor/target rulers:
    //   +0x2DC: spouse from recipient's court (secondary_recipient_id)
    //   +0x2E4: spouse from actor's court (secondary_actor_id)
    // After AutoSetup (type 0x14), +0x2DC and +0x2E4 are 0xFFFFFFFF.
    // We write the spouse refs, then call ValidateScopes to rebuild scope paths.
    //
    if (strcmp(intent->interaction_name.c_str(), "arrange_marriage_interaction") == 0) {
        ADLOG("DIRECT_CMD_V7: MARRIAGE === begin spouse setup ===\n");

        if (intent->secondary_recipient_id != 0) {
            *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::TARGET_CHAR_REF) =
                intent->secondary_recipient_id;
            ADLOG("DIRECT_CMD_V7: MARRIAGE — set +0x2DC (recipient spouse) = %u\n",
                  intent->secondary_recipient_id & 0xFFFFFF);
        } else {
            ADLOG("DIRECT_CMD_V7: MARRIAGE — WARNING: secondary_recipient_id=0, +0x2DC left as-is\n");
        }

        if (intent->secondary_actor_id != 0) {
            *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::SECONDARY_RECIPIENT_REF) =
                intent->secondary_actor_id;
            ADLOG("DIRECT_CMD_V7: MARRIAGE — set +0x2E4 (actor spouse) = %u\n",
                  intent->secondary_actor_id & 0xFFFFFF);
        } else {
            ADLOG("DIRECT_CMD_V7: MARRIAGE — WARNING: secondary_actor_id=0, +0x2E4 left as-is\n");
        }

        // Re-validate scopes after manual ctx edits (SIGSEGV-protected)
        uintptr_t validate_scopes_addr = bridge::game_fn(ck3::funcs::INTERACTION_CTX_VALIDATE_SCOPES);
        if (is_mapped(validate_scopes_addr)) {
            auto validate_scopes = reinterpret_cast<void(*)(void*, int)>(validate_scopes_addr);
            ADLOG("DIRECT_CMD_V7: MARRIAGE — calling ValidateScopes at 0x%lx\n", validate_scopes_addr);

            watchdog_arm(5);
            g_in_protected_region = true;
            int marriage_sig = sigsetjmp(g_recovery_point, 1);
            if (marriage_sig != 0) {
                g_in_protected_region = false;
                watchdog_disarm();
                ADLOG("DIRECT_CMD_V7: MARRIAGE — ValidateScopes crashed (sig=%d)\n", marriage_sig);
                // Non-fatal: continue with dispatch, scopes may still be valid enough
            } else {
                try {
                    validate_scopes(cmd_buf, 0);
                    ADLOG("DIRECT_CMD_V7: MARRIAGE — ValidateScopes completed\n");
                } catch (...) {
                    ADLOG("DIRECT_CMD_V7: MARRIAGE — ValidateScopes threw exception\n");
                }
                g_in_protected_region = false;
                watchdog_disarm();
            }
        } else {
            ADLOG("DIRECT_CMD_V7: MARRIAGE — ValidateScopes not mapped at 0x%lx\n", validate_scopes_addr);
        }

        ADLOG("DIRECT_CMD_V7: MARRIAGE === spouse setup complete ===\n");
    }

    // --- Step 1.5a1b: assign-guardian setup (educate_child_interaction) ---  §3
    //
    // educate_child_interaction is the ONLY other 2-secondary interaction:
    //   secondary_actor     = GUARDIAN  (intent->secondary_actor_id)
    //   secondary_recipient = WARD      (intent->secondary_recipient_id)
    //   on_accept sets set_relation_guardian = scope:guardian on the ward.
    // Slot mapping mirrors arrange_marriage VERBATIM (the only proven 2-secondary
    // template): ward (secondary_recipient_id) -> 0x2DC, guardian (secondary_actor_id)
    // -> 0x2E4. This is the highest-probability mapping, but the scope-declaration
    // ORDER for educate_child may differ from marriage, so which secondary lands in
    // which of {0x2DC,0x2E0,0x2E4} is the ONE genuinely-unknown variable (re_heir_
    // guardian_RE.md §3). ★ EXPERIMENTAL — confirm by readback (query_heir_education
    // guardian_id) on first live run; if guardian not set, fall back to 0x2E0 for the
    // guardian. A wrong slot fails INEFFECTIVE (no guardian set), not crashing — the
    // writes are into our own cmd_buf and ValidateScopes is SIGSEGV-trapped, identical
    // to the marriage wrapper. Strictly improves the current silent no-op (IDs dropped).
    if (strcmp(intent->interaction_name.c_str(), "educate_child_interaction") == 0) {
        ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD === begin guardian setup (EXPERIMENTAL slot map) ===\n");

        if (intent->secondary_recipient_id != 0) {
            *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::TARGET_CHAR_REF) =
                intent->secondary_recipient_id;  // ward -> 0x2DC (marriage mapping)
            ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — set +0x2DC (ward) = %u\n",
                  intent->secondary_recipient_id & 0xFFFFFF);
        } else {
            ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — WARNING: secondary_recipient_id(ward)=0, +0x2DC left as-is\n");
        }

        if (intent->secondary_actor_id != 0) {
            *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::SECONDARY_RECIPIENT_REF) =
                intent->secondary_actor_id;  // guardian -> 0x2E4 (marriage mapping)
            ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — set +0x2E4 (guardian) = %u\n",
                  intent->secondary_actor_id & 0xFFFFFF);
        } else {
            ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — WARNING: secondary_actor_id(guardian)=0, +0x2E4 left as-is\n");
        }

        // Re-validate scopes after manual ctx edits (SIGSEGV-protected) — clone of marriage.
        uintptr_t validate_scopes_addr = bridge::game_fn(ck3::funcs::INTERACTION_CTX_VALIDATE_SCOPES);
        if (is_mapped(validate_scopes_addr)) {
            auto validate_scopes = reinterpret_cast<void(*)(void*, int)>(validate_scopes_addr);
            ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — calling ValidateScopes at 0x%lx\n", validate_scopes_addr);

            watchdog_arm(5);
            g_in_protected_region = true;
            int educate_sig = sigsetjmp(g_recovery_point, 1);
            if (educate_sig != 0) {
                g_in_protected_region = false;
                watchdog_disarm();
                ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — ValidateScopes crashed (sig=%d)\n", educate_sig);
                // Non-fatal: continue with dispatch.
            } else {
                try {
                    validate_scopes(cmd_buf, 0);
                    ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — ValidateScopes completed\n");
                } catch (...) {
                    ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — ValidateScopes threw exception\n");
                }
                g_in_protected_region = false;
                watchdog_disarm();
            }
        } else {
            ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD — ValidateScopes not mapped at 0x%lx\n", validate_scopes_addr);
        }

        ADLOG("DIRECT_CMD_V7: EDUCATE_CHILD === guardian setup complete ===\n");
    }

    // --- Step 1.5a2: secondary title scope target (revoke_title + buy_claim) ---
    //
    // Phase 28 RE: revoke_title needs a title target at cmd+0x2F0 as a 16-byte scope value:
    //   +0x2F0: uint16 type=4 (PDX object ref), +0x2F2: 6 bytes padding, +0x2F8: uint64 title_packed_ref
    // The title_packed_ref is the full 32-bit packed ref from title_obj+0x10 (includes generation byte).
    // After writing, call ValidateScopes to propagate into scope paths.
    //
    // A2/A3: buy_claim_interaction is also a secondary-scope interaction — it needs
    // the claim's title in the same slot. The scope offset (0x2F0) for buy_claim is
    // MEDIUM confidence (inferred from revoke_title); the write is wrapped in the
    // same sigsetjmp/watchdog guard below, so a wrong offset fails safe (no crash,
    // just an ineffective scope) rather than taking down the game.
    const bool needs_title_scope =
        (strcmp(intent->interaction_name.c_str(), "revoke_title_interaction") == 0 ||
         strcmp(intent->interaction_name.c_str(), "buy_claim_interaction") == 0);
    if (needs_title_scope && intent->title_id > 0) {
        ADLOG("DIRECT_CMD_V7: REVOKE_TITLE === begin title scope setup ===\n");

        // Resolve title object from the title DB
        uint32_t title_id = static_cast<uint32_t>(intent->title_id) & 0xFFFFFF;
        uintptr_t title_obj = 0;
        {
            uintptr_t tdb = safe_read<uintptr_t>(g_base_addr + ck3::globals::LANDED_TITLE_DB);
            if (tdb) {
                uintptr_t arr = safe_read<uintptr_t>(tdb + ck3::hash_map::TABLE_PTR);
                uint32_t cnt = safe_read<uint32_t>(tdb + ck3::hash_map::CAPACITY);
                if (arr && title_id < cnt) {
                    title_obj = safe_read<uintptr_t>(arr + ck3::hash_map::ENTRY_HEADER_SIZE
                                                     + static_cast<uintptr_t>(title_id) * ck3::hash_map::ENTRY_STRIDE);
                }
            }
        }

        if (title_obj && is_mapped(title_obj)) {
            // Read the full 32-bit packed ref from title_obj+0x10
            uint32_t title_packed_ref = safe_read<uint32_t>(title_obj + 0x10);
            ADLOG("DIRECT_CMD_V7: REVOKE_TITLE — title_id=%u, packed_ref=0x%08x\n", title_id, title_packed_ref);

            // Write scope value at cmd+0x2F0: type=4, pad=0, id=packed_ref
            *reinterpret_cast<uint16_t*>(cmd_buf + 0x2F0) = 4;  // scope type = PDX object ref
            memset(cmd_buf + 0x2F2, 0, 6);                       // padding
            *reinterpret_cast<uint64_t*>(cmd_buf + 0x2F8) = static_cast<uint64_t>(title_packed_ref);

            // Re-validate scopes (reuse the same ValidateScopes pattern as marriage)
            uintptr_t validate_scopes_addr = bridge::game_fn(ck3::funcs::INTERACTION_CTX_VALIDATE_SCOPES);
            if (is_mapped(validate_scopes_addr)) {
                auto validate_scopes = reinterpret_cast<void(*)(void*, int)>(validate_scopes_addr);
                watchdog_arm(5);
                g_in_protected_region = true;
                int rt_sig = sigsetjmp(g_recovery_point, 1);
                if (rt_sig != 0) {
                    g_in_protected_region = false;
                    watchdog_disarm();
                    ADLOG("DIRECT_CMD_V7: REVOKE_TITLE — ValidateScopes crashed (sig=%d)\n", rt_sig);
                } else {
                    try {
                        validate_scopes(cmd_buf, 0);
                        ADLOG("DIRECT_CMD_V7: REVOKE_TITLE — ValidateScopes completed\n");
                    } catch (...) {
                        ADLOG("DIRECT_CMD_V7: REVOKE_TITLE — ValidateScopes threw exception\n");
                    }
                    g_in_protected_region = false;
                    watchdog_disarm();
                }
            }
        } else {
            ADLOG("DIRECT_CMD_V7: REVOKE_TITLE — title_obj not found for id=%u\n", title_id);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "revoke_title_not_found");
        }

        ADLOG("DIRECT_CMD_V7: REVOKE_TITLE === title scope setup complete ===\n");
    }

    // --- Step 1.5b stripped: DEAD_FOR_WAR_PER_CKE58L17.1 ---
    // Previously: war CB setup for declare_war_interaction — CASUS_BELLI_DB scan,
    // AttachCBState + AttachCBTitle, game-date stamp at raw_buf+0x2C0 (Gate-3
    // misnomer — actually player-char check). All of it is dead because
    // declare_war now routes through execute_war_intent (AI war pipeline), not
    // CSendCharacterInteractionCommand. See re_bridge_offset_audit.md §5 FIX-6
    // and ck-e58l.17.1 for the structural proof that this path cannot reach
    // WAR_CREATE_CORE.

#if 0  // DEAD_FOR_WAR_PER_CKE58L17.1 — preserved for archaeology only
    if (strcmp(intent->interaction_name.c_str(), "declare_war_interaction") == 0) {
        ADLOG("DIRECT_CMD_V7: WAR === begin CB setup for declare_war_interaction ===\n");

        // --- A) Set war_target and claimant ---
        *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::SECONDARY_ACTOR_REF) =
            static_cast<uint32_t>(target_id);
        uint32_t claimant = intent->claimant_id;
        if (claimant == 0) claimant = static_cast<uint32_t>(actor_id);
        *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::SECONDARY_RECIPIENT_REF) = claimant;
        ADLOG("DIRECT_CMD_V7: WAR — A) war_target=%u, claimant=%u\n",
              target_id & 0xFFFFFF, claimant & 0xFFFFFF);

        // Phase 38 finding: PopulateOptions re-run and ReconcileWarOptions are UNNECESSARY.
        // The AI ignores ReconcileWarOptions' return value. PopulateOptions always fails for
        // declare_war because war_target/claimant are -1 during BuildSendInteractionCmd
        // (same as the AI path). Just set fields, AttachCB, stamp date, dispatch.

        // --- B) AttachCBState + AttachCBTitle — populate cmd+0x330 sub-object ---
        //
        // The cmd+0x330 sub-object layout (0x30 bytes):
        //   +0x00: (unused)
        //   +0x08: uint64 cb_type_ref — CCasusBelliType* (written by AttachCBState)
        //   +0x10: uint64 title_data_ptr — dynamic array of uint32 title IDs
        //   +0x18: int32 title_capacity
        //   +0x1C: int32 title_count
        //   +0x20: ptr allocator (CPdxArray allocator object)
        //   +0x28: uint32 claimant_ref — claimant char ref for claim_cb (0xFFFFFFFF for non-claim)
        //
        // Strategy: Look up the first valid CB type from CASUS_BELLI_DB for this
        // actor/target pair, then write cb_type_ref and title data directly into
        // the sub-object fields. For AttachCBTitle, pre-set capacity in the
        // sub-object so the function's memcpy path is used (no allocator realloc).
        //
        // AttachCBState signature: void(long cmd_330_ptr, uint64 cb_type_ref)
        //   Trivial: *(param_1 + 8) = param_2
        //
        // AttachCBTitle signature: void(long cmd_330_ptr, uint32* title_source)
        //   title_source layout: +0x00=claimant_ref, +0x08=title_ids_ptr, +0x14=count
        //   When dest capacity >= source count, just memcpy (no allocator calls).

        volatile uintptr_t found_cb_type = 0;
        char found_cb_name[128] = {};

        // B.1) Look up CB type from CASUS_BELLI_DB
        {
            uintptr_t cb_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CASUS_BELLI_DB);
            if (cb_db && is_mapped(cb_db)) {
                uintptr_t cb_arr = safe_read<uintptr_t>(cb_db + ck3::game_db::ENTRIES_ARRAY);
                int32_t cb_count = safe_read<int32_t>(cb_db + ck3::game_db::LIVE_COUNT);
                if (cb_count < 0 || cb_count > 500) cb_count = 0;

                ADLOG("DIRECT_CMD_V7: WAR — B.1) scanning %d CB types in CASUS_BELLI_DB\n", cb_count);

                void* attacker = lookup_character(actor_id);
                void* defender = lookup_character(target_id);

                if (attacker && defender && cb_arr && is_mapped(cb_arr)) {
                    auto cb_validity_check = reinterpret_cast<bool(*)(void*, void*, void*, void*, int)>(
                        bridge::game_fn(ck3::funcs::CB_VALIDITY_CHECK));

                    // When title_id is set, prefer claim-type CBs (target_mode=6)
                    bool want_claim = (intent->title_id > 0);

                    // If cb_index is specified, use ONLY that CB — skip validity check
                    // and want_claim filtering. The agent already chose this CB.
                    if (intent->cb_index >= 0 && intent->cb_index < cb_count) {
                        volatile uintptr_t cb_type = safe_read<uintptr_t>(cb_arr + intent->cb_index * 8);
                        if (cb_type && is_mapped(cb_type)) {
                            found_cb_type = cb_type;
                            const char* name_ptr = nullptr;
                            size_t name_len = 0;
                            if (read_db_obj_name(cb_type, name_ptr, name_len)) {
                                size_t copy_len = (name_len < sizeof(found_cb_name) - 1) ? name_len : sizeof(found_cb_name) - 1;
                                memcpy(found_cb_name, name_ptr, copy_len);
                                found_cb_name[copy_len] = '\0';
                            }
                            ADLOG("DIRECT_CMD_V7: WAR — B.1) using specified cb_index=%d: %s (ptr=0x%lx)\n",
                                  intent->cb_index, found_cb_name, found_cb_type);
                        } else {
                            ADLOG("DIRECT_CMD_V7: WAR — B.1) specified cb_index=%d has invalid pointer — aborting (no auto-select)\n",
                                  intent->cb_index);
                        }
                    } else if (intent->cb_index >= 0) {
                        ADLOG("DIRECT_CMD_V7: WAR — B.1) specified cb_index=%d out of range (count=%d) — aborting (no auto-select)\n",
                              intent->cb_index, cb_count);
                    } else {
                    // Auto-select: cb_index not specified, find first valid CB
                    for (int32_t i = 0; i < cb_count && i < 100; i++) {
                        volatile uintptr_t cb_type = safe_read<uintptr_t>(cb_arr + i * 8);
                        if (!cb_type || !is_mapped(cb_type)) continue;

                        // Check if CB is enabled
                        uint8_t enabled = safe_read<uint8_t>(cb_type + ck3::cb_type::ENABLED_FLAG);
                        if (!enabled) continue;

                        // When pressing a claim (title_id set), only accept claim-type CBs
                        if (want_claim) {
                            int32_t tmode = safe_read<int32_t>(cb_type + ck3::cb_type::TARGET_MODE);
                            if (tmode != ck3::cb_type::TARGET_CLAIM) continue;  // skip non-claim CBs
                        }

                        // Read CB name for logging
                        const char* name_ptr = nullptr;
                        size_t name_len = 0;
                        char cb_name_buf[128] = {};
                        if (read_db_obj_name(cb_type, name_ptr, name_len)) {
                            size_t copy_len = (name_len < sizeof(cb_name_buf) - 1) ? name_len : sizeof(cb_name_buf) - 1;
                            memcpy(cb_name_buf, name_ptr, copy_len);
                            cb_name_buf[copy_len] = '\0';
                        }

                        // Call CB_VALIDITY_CHECK with SIGSEGV recovery
                        bool valid = false;
                        watchdog_arm(2);
                        g_in_protected_region = true;
                        int cb_sig = sigsetjmp(g_recovery_point, 1);
                        if (cb_sig != 0) {
                            g_in_protected_region = false;
                            watchdog_disarm();
                            ADLOG("DIRECT_CMD_V7: WAR — CB_VALIDITY_CHECK crashed for %s (sig=%d)\n",
                                  cb_name_buf, cb_sig);
                            continue;
                        }
                        try {
                            // D8 hardening: shared large BSS buffer instead of a
                            // 32-entry stack array — see g_cb_option_storage.
                            memset(g_cb_option_storage, 0, sizeof(g_cb_option_storage));
                            void* alloc_vtable = reinterpret_cast<void*>(g_base_addr + pdx_array::CB_OPTION_ALLOCATOR_VTABLE);
                            struct {
                                void*    data;
                                uint32_t capacity;
                                uint32_t count;
                                void**   allocator;
                            } cb_output_list;
                            void* alloc_obj = alloc_vtable;
                            cb_output_list.data = g_cb_option_storage;
                            cb_output_list.capacity = kCBOptionEntryCap;
                            cb_output_list.count = 0;
                            cb_output_list.allocator = &alloc_obj;

                            valid = cb_validity_check(
                                reinterpret_cast<void*>(cb_type),
                                &cb_output_list,
                                attacker, defender, 0);
                            if (cb_output_list.count >= kCBOptionEntryCap) {
                                ADLOG("DIRECT_CMD_V7: WAR — D8 GUARD: CB option enumeration "
                                      "for %s filled the %u-entry buffer — the fatal engine "
                                      "growth path was one entry away; raise kCBOptionEntryCap\n",
                                      cb_name_buf, kCBOptionEntryCap);
                            }
                        } catch (...) {
                            valid = false;
                            ADLOG("DIRECT_CMD_V7: WAR — CB_VALIDITY_CHECK exception for %s (idx=%d)\n",
                                  cb_name_buf, i);
                        }
                        g_in_protected_region = false;
                        watchdog_disarm();

                        ADLOG("DIRECT_CMD_V7: WAR — CB_VALIDITY_CHECK(%s, idx=%d) returned %d\n",
                              cb_name_buf, i, valid ? 1 : 0);

                        if (valid) {
                            found_cb_type = cb_type;
                            strncpy(found_cb_name, cb_name_buf, sizeof(found_cb_name) - 1);
                            ADLOG("DIRECT_CMD_V7: WAR — B.1) FOUND valid CB: %s (ptr=0x%lx, idx=%d)\n",
                                  found_cb_name, found_cb_type, i);
                            break;
                        }
                    }
                    } // end auto-select branch
                } else {
                    ADLOG("DIRECT_CMD_V7: WAR — B.1) attacker/defender lookup failed\n");
                }
            } else {
                ADLOG("DIRECT_CMD_V7: WAR — B.1) CASUS_BELLI_DB not found\n");
            }
        }

        if (found_cb_type == 0) {
            ADLOG("DIRECT_CMD_V7: WAR — B.1) WARNING: no valid CB found for actor=%u target=%u\n",
                  actor_id & 0xFFFFFF, target_id & 0xFFFFFF);
        }

        // B.2) AttachCBState — write cb_type_ref to cmd+0x338
        if (found_cb_type != 0) {
            // WAR_ATTACH_STATE is trivial: *(cmd+0x330+8) = cb_type_ref
            // We can call the actual function or just write directly. Call it for correctness.
            uintptr_t attach_state_addr = bridge::game_fn(ck3::funcs::WAR_ATTACH_STATE);
            if (is_mapped(attach_state_addr)) {
                auto attach_cb_state = reinterpret_cast<void(*)(void*, uintptr_t)>(attach_state_addr);
                ADLOG("DIRECT_CMD_V7: WAR — B.2) calling AttachCBState(cmd+0x330, cb=0x%lx) [%s]\n",
                      found_cb_type, found_cb_name);
                attach_cb_state(cmd_buf + 0x330, found_cb_type);
            } else {
                // Fallback: write directly (same thing AttachCBState does)
                ADLOG("DIRECT_CMD_V7: WAR — B.2) AttachCBState not mapped, writing directly\n");
                *reinterpret_cast<uintptr_t*>(cmd_buf + 0x338) = found_cb_type;
            }

            uintptr_t verify = *reinterpret_cast<uintptr_t*>(cmd_buf + 0x338);
            ADLOG("DIRECT_CMD_V7: WAR — B.2) verify cmd+0x338 = 0x%lx (expected 0x%lx)\n",
                  verify, found_cb_type);
        }

        // B.3) AttachCBTitle — populate title array in cmd+0x330 sub-object
        //
        // The title_source struct for AttachCBTitle:
        //   +0x00: uint32 claim_type
        //   +0x08: ptr title_ids (uint32 array)
        //   +0x14: int32 count
        //
        // Strategy: Pre-set the destination's capacity and data pointer so
        // AttachCBTitle uses the memcpy path (no allocator realloc needed).
        // Use a static buffer that persists through the dispatch pipeline.
        if (found_cb_type != 0 && intent->title_id > 0) {
            // Static title buffer — persists until next war dispatch (single-threaded)
            static uint32_t s_war_title_buf[16] = {};
            s_war_title_buf[0] = static_cast<uint32_t>(intent->title_id);

            // Pre-set destination capacity and data pointer so AttachCBTitle
            // sees capacity(16) >= count(1) and does a simple memcpy.
            *reinterpret_cast<uintptr_t*>(cmd_buf + 0x340) = reinterpret_cast<uintptr_t>(s_war_title_buf);
            *reinterpret_cast<int32_t*>(cmd_buf + 0x348) = 16;  // capacity
            *reinterpret_cast<int32_t*>(cmd_buf + 0x34C) = 0;   // count (AttachCBTitle will set this)

            // Build the title_source struct for AttachCBTitle
            // Layout: +0x00=claimant_ref(u32), +0x08=title_ids_ptr(u64), +0x14=count(u32)
            // WAR_ATTACH_CB copies source[0] to dest+0x28 which maps to CLAIMANT_REF (cb+0x198)
            alignas(16) uint8_t title_source[0x20] = {};
            *reinterpret_cast<uint32_t*>(title_source + 0x00) = claimant;  // claimant char ref (copied to cmd+0x358 = CLAIMANT_REF)
            *reinterpret_cast<uintptr_t*>(title_source + 0x08) = reinterpret_cast<uintptr_t>(s_war_title_buf);
            *reinterpret_cast<int32_t*>(title_source + 0x14) = 1;   // count = 1 title

            uintptr_t attach_cb_addr = bridge::game_fn(ck3::funcs::WAR_ATTACH_CB);
            if (is_mapped(attach_cb_addr)) {
                auto attach_cb_title = reinterpret_cast<void(*)(void*, void*)>(attach_cb_addr);
                ADLOG("DIRECT_CMD_V7: WAR — B.3) calling AttachCBTitle(cmd+0x330, title_source) "
                      "title_id=%d\n", intent->title_id);

                watchdog_arm(3);
                g_in_protected_region = true;
                int title_sig = sigsetjmp(g_recovery_point, 1);
                if (title_sig != 0) {
                    g_in_protected_region = false;
                    watchdog_disarm();
                    ADLOG("DIRECT_CMD_V7: WAR — B.3) AttachCBTitle crashed (sig=%d) — "
                          "falling back to direct write\n", title_sig);
                    // Fallback: write fields directly
                    *reinterpret_cast<uintptr_t*>(cmd_buf + 0x340) =
                        reinterpret_cast<uintptr_t>(s_war_title_buf);
                    *reinterpret_cast<int32_t*>(cmd_buf + 0x348) = 16;  // capacity
                    *reinterpret_cast<int32_t*>(cmd_buf + 0x34C) = 1;   // count
                    *reinterpret_cast<uint32_t*>(cmd_buf + 0x358) = claimant;  // claimant char ref
                } else {
                    try {
                        attach_cb_title(cmd_buf + 0x330, title_source);
                        ADLOG("DIRECT_CMD_V7: WAR — B.3) AttachCBTitle completed\n");
                    } catch (...) {
                        ADLOG("DIRECT_CMD_V7: WAR — B.3) AttachCBTitle threw exception — "
                              "falling back to direct write\n");
                        *reinterpret_cast<uintptr_t*>(cmd_buf + 0x340) =
                            reinterpret_cast<uintptr_t>(s_war_title_buf);
                        *reinterpret_cast<int32_t*>(cmd_buf + 0x348) = 16;
                        *reinterpret_cast<int32_t*>(cmd_buf + 0x34C) = 1;
                        *reinterpret_cast<uint32_t*>(cmd_buf + 0x358) = claimant;  // claimant char ref
                    }
                    g_in_protected_region = false;
                    watchdog_disarm();
                }
            } else {
                // Direct write fallback
                ADLOG("DIRECT_CMD_V7: WAR — B.3) AttachCBTitle not mapped, writing directly\n");
                *reinterpret_cast<uintptr_t*>(cmd_buf + 0x340) =
                    reinterpret_cast<uintptr_t>(s_war_title_buf);
                *reinterpret_cast<int32_t*>(cmd_buf + 0x348) = 16;  // capacity
                *reinterpret_cast<int32_t*>(cmd_buf + 0x34C) = 1;   // count
                *reinterpret_cast<uint32_t*>(cmd_buf + 0x358) = claimant;  // claimant char ref
            }

            int32_t verify_count = *reinterpret_cast<int32_t*>(cmd_buf + 0x34C);
            ADLOG("DIRECT_CMD_V7: WAR — B.3) verify: title_count=%d, title_id=%u\n",
                  verify_count, s_war_title_buf[0]);
        } else if (found_cb_type != 0) {
            // CB found but no title specified — some CBs (e.g. subjugation) don't need titles
            ADLOG("DIRECT_CMD_V7: WAR — B.3) no title_id specified (title_id=%d), "
                  "skipping AttachCBTitle\n", intent->title_id);
        }

        // Phase 38 finding: ReconcileWarOptions is UNNECESSARY — the AI ignores its return value.
        // It's called only for side effects (option byte selection), but the engine's Execute
        // handler reads cmd+0x338 (CB type) directly, not the option byte at cmd+0x300.
        // Skipping ReconcileWarOptions eliminates the false-return confusion and state corruption.

        // --- C) Gate 3 analysis (NO ACTION NEEDED for player-controlled characters) ---
        //
        // Execute Gate 3 checks: *(int32*)(wrapper+0x2F8) == GetCurrentGameDate()
        //   wrapper+0x2F8 = raw_buf+0x2D8 = ACTOR_CHAR_REF (the actor's character ID)
        //   GetCurrentGameDate() at 0x062642d0 returns *(DAT_0bfad8c0) = PLAYER_CHAR_ID
        //
        // So Gate 3 is really: "is the actor the player character?"
        // This PASSES automatically when actor == controlled character (36010 == 36010).
        // The game date stamp at raw_buf+0x2C0 is UNRELATED to this gate.
        //
        // Previous analysis incorrectly identified this as a game date staleness check.
        // The function "GetCurrentGameDate" is MISNAMED — it returns player char ID, not date.
        // Keeping the date stamp for safety (it doesn't hurt), but it's not required.
        uintptr_t gs_root = safe_read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
        int32_t game_date = 0;
        if (gs_root != 0) {
            game_date = safe_read<int32_t>(gs_root + ck3::game_state::GAME_DATE);
        }
        if (game_date != 0) {
            *reinterpret_cast<int32_t*>(cmd_buf + 0x2C0) = game_date;
            ADLOG("DIRECT_CMD_V7: WAR — D) stamped game_date=%d at raw_buf+0x2C0 "
                  "(from GAME_STATE_ROOT+0x08)\n", game_date);
        } else {
            ADLOG("DIRECT_CMD_V7: WAR — D) WARNING: could not read game date "
                  "(gs_root=0x%lx), raw_buf+0x2C0 left as 0\n", gs_root);
        }

        // --- E) Inspect cmd+0x330 CB sub-object ---
        uintptr_t cb_type_ref = *reinterpret_cast<uintptr_t*>(cmd_buf + 0x338);
        int32_t title_count = *reinterpret_cast<int32_t*>(cmd_buf + 0x34C);
        ADLOG("DIRECT_CMD_V7: WAR — E) cmd+0x338 (cb_type_ref)=0x%lx, cmd+0x34C (title_count)=%d\n",
              cb_type_ref, title_count);
        if (cb_type_ref == 0) {
            ADLOG("DIRECT_CMD_V7: WAR — WARNING: cb_type_ref is 0 — CB sub-object not populated\n");
        }

        ADLOG("DIRECT_CMD_V7: WAR === CB setup complete (cb=%s) ===\n", found_cb_name);
    }
#endif  // DEAD_FOR_WAR_PER_CKE58L17.1 (Step 1.5b war CB setup)

    // --- Step 1.5c: Generic send_option selection (any interaction with cb_index) ---
    // For interactions with send_options (release_from_prison banish/recruit, etc.),
    // the agent can pass cb_index to select a specific option. This uses the same
    // option system (interaction+0x2530) as war CB options.
    // Skip for marriage (no options). declare_war no longer reaches this function
    // (see DEAD_FOR_WAR_PER_CKE58L17.1 banner at function top), so a declare_war
    // exclusion here is unnecessary.
    if (intent->cb_index >= 0 &&
        strcmp(intent->interaction_name.c_str(), "arrange_marriage_interaction") != 0) {
        uintptr_t select_addr = bridge::game_fn(ck3::funcs::INTERACTION_SELECT_OPTION);
        if (is_mapped(select_addr)) {
            auto select_option = reinterpret_cast<void(*)(void*, int)>(select_addr);
            ADLOG("DIRECT_CMD_V7: SEND_OPTION — SelectOption(cmd, %d)\n", intent->cb_index);
            watchdog_arm(3);
            g_in_protected_region = true;
            int opt_sig = sigsetjmp(g_recovery_point, 1);
            if (opt_sig != 0) {
                g_in_protected_region = false;
                watchdog_disarm();
                ADLOG("DIRECT_CMD_V7: SEND_OPTION — SelectOption crashed (sig=%d)\n", opt_sig);
            } else {
                try {
                    select_option(cmd_buf, intent->cb_index);
                    ADLOG("DIRECT_CMD_V7: SEND_OPTION — SelectOption(%d) completed\n", intent->cb_index);
                } catch (...) {
                    ADLOG("DIRECT_CMD_V7: SEND_OPTION — SelectOption threw exception\n");
                }
                g_in_protected_region = false;
                watchdog_disarm();
            }
        }
    }

    // --- Steps 2-4: BUILD_SEND_CMD_FROM_BUF → Clone → DISPATCH ---
    alignas(16) uint8_t wrapper_buf[0x400] = {};  // CSendCharInteractionCmd is 0x368B; 0x400 for safety margin
    void* volatile wrapper_ptr = &wrapper_buf[0];
    bool dispatched = false;

    watchdog_arm(5);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        ADLOG("DIRECT_CMD_V7: Steps 2-4 crashed (%s) — cleaning up cmd_buf\n", sig_name);
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "direct_cmd_build_dispatch_%s", sig_name);
        // Cleanup cmd_buf on crash
        try { interaction_dtor(cmd_buf); } catch (...) {}
        return false;
    }

    try {
        // Step 2: BUILD_SEND_CMD_FROM_BUF(wrapper, cmd_buf)
        // This constructs CSendCharacterInteractionCommand from the cmd_buf data.
        // It sets vtables, deep-copies interaction data, computes hashes.
        ADLOG("DIRECT_CMD_V7: Step 2 — BUILD_SEND_CMD_FROM_BUF(wrapper=%p, cmd_buf=%p) via 0x%lx\n",
              wrapper_ptr, cmd_buf, bridge::game_fn(ck3::funcs::BUILD_SEND_CMD_FROM_BUF));
        build_send_cmd(wrapper_ptr, cmd_buf);

        // Validate: wrapper should have a vtable pointer at offset 0
        uintptr_t wrapper_vtable = *reinterpret_cast<uintptr_t*>(wrapper_ptr);
        if (!wrapper_vtable || !is_mapped(wrapper_vtable)) {
            ADLOG("DIRECT_CMD_V7: wrapper vtable invalid after BUILD_SEND_CMD (0x%lx)\n",
                  wrapper_vtable);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "wrapper_vtable_invalid");
            // Cleanup cmd_buf
            try { interaction_dtor(cmd_buf); } catch (...) {}
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }
        ADLOG("DIRECT_CMD_V7: Step 2 complete — wrapper vtable = 0x%lx\n", wrapper_vtable);

        // Step 3: Clone via the vtable (per ABI: game_command_clone_fn / clone_game_command)
        uintptr_t clone_fn_ptr = game_command_clone_fn(wrapper_ptr);
        if (!clone_fn_ptr || !is_mapped(clone_fn_ptr)) {
            ADLOG("DIRECT_CMD_V7: clone vtable slot invalid (0x%lx)\n", clone_fn_ptr);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "clone_vtable_entry_invalid");
            // Cleanup: destroy wrapper then cmd_buf
            try { destroy_game_command_in_place(wrapper_ptr); } catch (...) {}
            try { interaction_dtor(cmd_buf); } catch (...) {}
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }

        ADLOG("DIRECT_CMD_V7: Step 3 — CLONE via vtable slot at 0x%lx\n", clone_fn_ptr);
        void* clone_ptr = clone_game_command(clone_fn_ptr, wrapper_ptr);

        if (!clone_ptr || !is_mapped(reinterpret_cast<uintptr_t>(clone_ptr))) {
            ADLOG("DIRECT_CMD_V7: Clone returned invalid pointer (0x%lx)\n",
                  reinterpret_cast<uintptr_t>(clone_ptr));
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "clone_returned_invalid");
            // Cleanup wrapper + cmd_buf
            try { destroy_game_command_in_place(wrapper_ptr); } catch (...) {}
            try { interaction_dtor(cmd_buf); } catch (...) {}
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }
        ADLOG("DIRECT_CMD_V7: Step 3 complete — clone at %p\n", clone_ptr);

        // Step 3.5: Write interaction_ptr to INTERACTION_DB+0x1000.
        //
        // ⚠️  ck-8cns: The "Gate 2" framing for this stamp is REFUTED. See
        //    re/findings/re_gate2_stamp_verification.md. Summary:
        //    - The compare in CSendCharacterInteractionCmd::Execute (0x0619b800)
        //      runs AFTER SendCharacterInteraction has already dispatched.
        //      It cannot gate the war.
        //    - Slot 14 of the InteractionDB pinned-interaction array (offset
        //      0x1000) is statically bound to &influence_candidacy_interaction
        //      at DB load — no runtime writers exist in the game.
        //    - This stamp permanently corrupts that binding. Removal tracked
        //      in ck-ukox8. Left in place here pending scenario verification
        //      that war declarations still succeed without it.
        //
        // Original (now-incorrect) rationale, kept for context:
        //
        // CSendCharInteractionCmd::Execute (CGameCommandHelper vtable[1]) has Gate 2:
        //   *(wrapper+0x20) == *(*(INTERACTION_DB) + 0x1000)
        // This compares the command's interaction definition pointer against a
        // "current interaction" field in the INTERACTION_DB object. If they don't
        // match, the command is SILENTLY REJECTED (no error, no log, just returns).
        //
        // The field at DB+0x1000 is updated by successful command execution, so it
        // holds the ptr of whatever interaction was last dispatched. Our command's
        // interaction ptr will never match unless we pre-stamp it.
        //
        // Fix: Read the interaction ptr from cmd_buf[0] and write it to DB+0x1000.
        // This is safe because we're on the game thread during DRAIN_INTENTS.
        {
            uintptr_t interaction_ptr = *reinterpret_cast<uintptr_t*>(cmd_buf);
            uintptr_t idb_ptr = safe_read<uintptr_t>(g_base_addr + ck3::globals::INTERACTION_DB);
            if (idb_ptr && is_mapped(idb_ptr) && interaction_ptr && is_mapped(interaction_ptr)) {
                uintptr_t gate_field_addr = idb_ptr + ck3::interaction_db_internal::CURRENT_INTERACTION;
                uintptr_t old_gate = safe_read<uintptr_t>(gate_field_addr);
                *reinterpret_cast<uintptr_t*>(gate_field_addr) = interaction_ptr;
                ADLOG("DIRECT_CMD_V7: Step 3.5 — stamped INTERACTION_DB+0x1000 = 0x%lx "
                      "(was 0x%lx, interaction_ptr from cmd_buf)\n", interaction_ptr, old_gate);
            } else {
                ADLOG("DIRECT_CMD_V7: Step 3.5 — WARNING: could not stamp INTERACTION_DB+0x1000 "
                      "(idb=0x%lx, interaction=0x%lx)\n", idb_ptr, interaction_ptr);
            }
        }

        // Step 4: GAME_COMMAND_DISPATCH(&dispatch_mgr_struct, &clone, 7, 0)
        // dispatch_mgr_addr is the ADDRESS of the inline struct, passed directly (not dereferenced)
        //
        // RECORDER BYPASS: The dispatch manager has a command recorder at +0x3f0.
        // When non-null, GAME_COMMAND_DISPATCH clones our command AGAIN for the
        // recorder's replay log. This second clone's DeepCopy throws because our
        // manually-constructed command has allocator state that doesn't survive
        // a third copy. Temporarily null the recorder to skip this path.
        uintptr_t recorder_addr = dispatch_mgr_addr + ck3::dispatch_mgr::RECORDER_PTR;
        uintptr_t old_recorder = safe_read<uintptr_t>(recorder_addr);
        if (old_recorder) {
            ADLOG("DIRECT_CMD_V7: Temporarily disabling command recorder at 0x%lx (was 0x%lx)\n",
                  recorder_addr, old_recorder);
            // R1c (ck-6kvq.10, §2.9). This pulse has NO working crash restore:
            // `old_recorder` is scoped inside this try block, so neither the
            // catch(...) below nor the enclosing sigsetjmp recovery can put it
            // back. A fault in game_dispatch therefore leaves the engine's
            // command recorder permanently NULL and every later command is
            // silently dropped from the replay log. The ledger is the only thing
            // that restores it.
            (void)bridge::pulse_enter_global(
                "DISPATCH_MGR_RECORDER", "direct_cmd_v7_dispatch",
                recorder_addr, 8, old_recorder);
            *reinterpret_cast<uintptr_t*>(recorder_addr) = 0;
        }

        ADLOG("DIRECT_CMD_V7: Step 4 — GAME_COMMAND_DISPATCH(mgr=0x%lx, clone=%p, priority=7, 0)\n",
              dispatch_mgr_addr, clone_ptr);
        game_dispatch(reinterpret_cast<void*>(dispatch_mgr_addr), &clone_ptr, 7, 0);

        // Restore recorder
        if (old_recorder) {
            *reinterpret_cast<uintptr_t*>(recorder_addr) = old_recorder;
            bridge::pulse_exit_addr(recorder_addr);   // R1c: after the restore
            ADLOG("DIRECT_CMD_V7: Restored command recorder\n");
        }

        // Post-dispatch: if engine took ownership, clone_ptr may be nulled.
        // If not nulled, destroy the clone ourselves.
        if (clone_ptr != nullptr) {
            ADLOG("DIRECT_CMD_V7: clone_ptr not nulled by dispatch — destroying\n");
            try { delete_game_command(clone_ptr); } catch (...) {}
        }

        dispatched = true;
        ADLOG("DIRECT_CMD_V7: Step 4 complete — command dispatched\n");

        // Step 5: Cleanup wrapper (stack-allocated: destroy in place, free nothing)
        ADLOG("DIRECT_CMD_V7: Step 5 — destroying wrapper in place\n");
        destroy_game_command_in_place(wrapper_ptr);

        // Step 6: Cleanup cmd_buf via interaction data destructor
        ADLOG("DIRECT_CMD_V7: Step 6 — destroying cmd_buf via INTERACTION_TLS_DESTRUCTOR\n");
        interaction_dtor(cmd_buf);

    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        ADLOG("DIRECT_CMD_V7: Steps 2-4 threw C++ exception — cleaning up cmd_buf\n");
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "direct_cmd_build_dispatch_exception");
        try { interaction_dtor(cmd_buf); } catch (...) {}
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (dispatched) {
        ADLOG("DIRECT_CMD_V7: === SUCCESS === %s sent (actor=%u, target=%u)\n",
              intent->interaction_name.c_str(), actor_id & 0xFFFFFF, target_id & 0xFFFFFF);
    }

    return dispatched;
}

bool execute_direct_dispatch(Intent* intent, void* character, void* interaction) {
    // === ENTRY: full intent dump (mirror execute_war_intent style for the
    // production INTERACTION dispatch path that declare_war actually traverses) ===
    ADLOG("=== execute_direct_dispatch ENTRY ===\n");
    ADLOG("  intent=%p type=%d actor_id=0x%08x(%u) target_id=0x%08x(%u)\n",
          (void*)intent, (int)intent->type,
          intent->actor_id, intent->actor_id & 0xFFFFFF,
          intent->target_id, intent->target_id & 0xFFFFFF);
    ADLOG("  cb_index=%d title_id=%d claimant_id=0x%08x(%u)\n",
          intent->cb_index, intent->title_id,
          intent->claimant_id, intent->claimant_id & 0xFFFFFF);
    ADLOG("  interaction_hash=0x%08x interaction_name='%s' character=%p interaction=%p\n",
          intent->interaction_hash, intent->interaction_name.c_str(),
          character, interaction);
    ADLOG("  secondary_actor_id=0x%08x secondary_recipient_id=0x%08x\n",
          intent->secondary_actor_id, intent->secondary_recipient_id);
    ADLOG("  force_bypass=%d debug_all_paths=%d queued_tick=%lu ttl_ticks=%u request_id='%s'\n",
          intent->force_bypass, intent->debug_all_paths,
          (unsigned long)intent->queued_tick,
          intent->ttl_ticks, intent->request_id.c_str());

    // RAII scope-exit logger: log return value and final rejection reason regardless of exit point.
    bool _ret = false;
    struct ExitLogger {
        const char* iname;
        uint32_t actor_m;
        uint32_t target_m;
        const Intent* intent_ref;
        bool* ret_ref;
        ~ExitLogger() {
            ADLOG("=== execute_direct_dispatch EXIT: returning %s (interaction='%s' actor=%u target=%u rejection='%s') ===\n",
                  *ret_ref ? "true" : "false",
                  iname, actor_m, target_m,
                  intent_ref->rejection_reason[0] ? intent_ref->rejection_reason : "");
        }
    } _exit_logger { intent->interaction_name.c_str(),
                     intent->actor_id & 0xFFFFFF,
                     intent->target_id & 0xFFFFFF,
                     intent, &_ret };

    // ck-bjkk: preserve the TLS rejection reason through v5 fallback. Previously
    // the v5 path cleared intent->rejection_reason and overwrote it with
    // "ai_no_valid_target", masking which specific TLS step had failed. The
    // harness sees only the final intent result, not bridge logs, so the
    // original TLS reason was invisible to scenario assertions. Now v5 appends
    // "; tls=<reason>" so both halves of the failure surface in one place.
    char tls_reason_saved[128] = {0};

    // DISPATCH PRIORITY (post-ck-0ftj):
    //   1. TLS pipeline (v6): CTX_INIT/Reset/SetRecipient/IsValid -> BUILD -> Clone -> Dispatch.
    //      Used by targeted non-war interactions (marriage, gift, sway, vassalization, etc.).
    //   2. v5 scoring hook: AI_INTERACTION_DECISION with score manipulation. Fallback
    //      for interactions not on the direct-dispatch list.
    //   3. v7 direct command: used only for interactions that explicitly need cmd-buf
    //      setup beyond TLS (none today now that declare_war moved off this path).
    //
    // DEAD_FOR_WAR_PER_CKE58L17.1: declare_war no longer reaches execute_direct_dispatch.
    // WAR_DECLARE intents go through drain_and_execute_intents → execute_war_intent
    // (action_handlers.cpp:680, ai_eval_hook.cpp:3803). The previous
    // "TLS-then-v7-fallback for declare_war" chain has been removed.
    //
    // Priority 1: TLS pipeline for targeted non-war interactions
    if (intent->target_id != 0 &&
        is_direct_dispatch_interaction(intent->interaction_name.c_str())) {
        ADLOG("DISPATCH: target_id=%u + interaction '%s' — trying TLS pipeline (v6)\n",
              intent->target_id & 0xFFFFFF, intent->interaction_name.c_str());
        bool tls_result = execute_tls_pipeline_dispatch(intent, character, interaction);
        if (tls_result) {
            ADLOG("DISPATCH: chose path=TLS (succeeded)\n");
            _ret = true;
            return true;  // TLS pipeline succeeded
        }
        ADLOG("DISPATCH: TLS pipeline (v6) failed — reason='%s' — path=TLS\n", intent->rejection_reason);

        // Non-war peaceful/scheme interactions: if TLS rejection is a LEGITIMATE
        // game-logic rejection (not a pipeline bug), propagate it. v5 fallback
        // calls AI_INTERACTION_DECISION which walks the actor's own target pool
        // and dispatches to its best-scoring target — which is NOT our requested
        // target when CTX_VALIDATE already rejected (actor,target). The score
        // hook only tilts scoring for targets that pass CtxValidate in the inner
        // loop, so a rejected target never wins; v5 silently delivers to some
        // other character and marks the intent "success". Bead ck-88xk (ck-2toa):
        // gift_interaction CONTROL reported success but recipient gold didn't
        // move because v5 gifted a different character. Fix: don't mask the
        // real reason with a misleading success.
        {
            const char* rej = intent->rejection_reason;
            bool legitimate_game_reject =
                strcmp(rej, "interaction_not_valid_for_target") == 0 ||
                strcmp(rej, "target_would_reject") == 0;
            if (legitimate_game_reject) {
                ADLOG("DISPATCH: TLS game-reject '%s' is authoritative, NOT falling back to v5 (would deliver to wrong target)\n",
                      rej);
                ADLOG("DISPATCH: chose path=TLS (rejected)\n");
                _ret = false;
                return false;
            }
            ADLOG("DISPATCH: TLS pipeline bug '%s', falling back path=v5\n", rej);
            // ck-bjkk: save the TLS reason so v5 can surface it in the final
            // rejection string if v5 also fails.
            snprintf(tls_reason_saved, sizeof(tls_reason_saved), "%s", rej);
            intent->rejection_reason[0] = '\0';
        }
    } else if (intent->target_id != 0) {
        ADLOG("DISPATCH: target_id=%u specified but '%s' not on direct dispatch list "
              "— using v5 scoring hook\n",
              intent->target_id & 0xFFFFFF, intent->interaction_name.c_str());
    }

    // Targeted AI_INTERACTION_DECISION dispatch — v5 (fallback)
    //
    // Strategy: Hook GetAiWillDoScore to force our desired target to score
    // highest, enable yesmen to force acceptance, then call AI_INTERACTION_DECISION.
    // The function handles all command construction internally (deep copy, clone,
    // dispatch). We just tilt the scoring.
    //
    // HISTORY:
    // v1-v3: manual command construction — all crashed (deep copy issues)
    // v4: AI_INTERACTION_DECISION auto-target — WORKS but ignores target_id
    // v5: scoring hook + yesmen — forces specific target selection
    // v6: TLS pipeline — direct command construction bypassing AI_INTERACTION_DECISION

    uint32_t actor_id = read<uint32_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::ID);
    uint32_t target_id = intent->target_id;

    ADLOG("DISPATCH_V5: === BEGIN === interaction=%s actor=%u target=%u score_hook=%s\n",
          intent->interaction_name.c_str(), actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
          g_score_hook_installed ? "YES" : "NO");

    auto ai_decide = reinterpret_cast<bool(*)(void*, void*, uint32_t)>(
        bridge::game_fn(ck3::funcs::AI_INTERACTION_DECISION));
    uint32_t rng_seed = (actor_id ^ (g_tick_id.load() * 2654435761u)) & 0x7FFFFFFE;

    // --- Tilt: arm transient score hook + set force target ---
    if (target_id != 0 && g_score_hook_installed) {
        g_force_target_id.store(target_id & 0xFFFFFF, std::memory_order_release);
        arm_score_hook();  // Temporarily patch function prologue
        ADLOG("DISPATCH_V5: Score hook ARMED for target=%u\n", target_id & 0xFFFFFF);
    }

    // Yesmen: disabled by default — the LLM should deal with realistic refusals.
    // Enable only if intent has force_accept flag (e.g., for testing).
    uint8_t* yesmen_flag = reinterpret_cast<uint8_t*>(
        bridge::game_fn(ck3::funcs::DEBUG_FLAGS_BASE));
    uint8_t old_yesmen = 0;
    bool use_yesmen = intent->force_accept;  // opt-in per-intent (e.g. for testing)
    if (use_yesmen && is_mapped(reinterpret_cast<uintptr_t>(yesmen_flag))) {
        old_yesmen = *yesmen_flag;
        (void)bridge::pulse_enter_global(   // R1c (ck-6kvq.10, §2.9)
            "DEBUG_YESMEN", "execute_direct_dispatch",
            reinterpret_cast<uintptr_t>(yesmen_flag), 1, old_yesmen);
        *yesmen_flag = 1;
        ADLOG("DISPATCH_V5: Yesmen enabled (was %u)\n", old_yesmen);
    }

    // Bypass: skips interaction/scheme/decision preconditions (31 callers).
    // Only enabled when explicitly requested via force_bypass on the intent.
    uint8_t* bypass_flag = reinterpret_cast<uint8_t*>(
        bridge::game_fn(ck3::funcs::DEBUG_FLAGS_BASE) + ck3::funcs::DEBUG_BYPASS_OFFSET);
    uint8_t old_bypass = 0;
    bool use_bypass = intent->force_bypass;
    if (use_bypass && is_mapped(reinterpret_cast<uintptr_t>(bypass_flag))) {
        old_bypass = *bypass_flag;
        (void)bridge::pulse_enter_global(   // R1c (ck-6kvq.10, §2.9)
            "DEBUG_BYPASS", "execute_direct_dispatch",
            reinterpret_cast<uintptr_t>(bypass_flag), 1, old_bypass);
        *bypass_flag = 1;
        ADLOG("DISPATCH_V5: Bypass flag enabled (was %u)\n", old_bypass);
    }

    // --- Call AI_INTERACTION_DECISION ---
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        g_force_target_id.store(0, std::memory_order_release);
        disarm_score_hook();  // Restore original bytes
        if (use_yesmen && is_mapped(reinterpret_cast<uintptr_t>(yesmen_flag))) {
            *yesmen_flag = old_yesmen;
            bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(yesmen_flag));  // R1c
        }
        if (use_bypass && is_mapped(reinterpret_cast<uintptr_t>(bypass_flag))) {
            *bypass_flag = old_bypass;
            bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(bypass_flag));  // R1c
        }
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                               (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
        ADLOG("DISPATCH_V5: crashed/timed out (%s)\n", sig_name);
        if (tls_reason_saved[0]) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "ai_decide_%s; tls=%s", sig_name, tls_reason_saved);
        } else {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "ai_decide_%s", sig_name);
        }
        ADLOG("DISPATCH: chose path=v5 (crashed)\n");
        _ret = false;
        return false;
    }

    bool sent = false;
    try {
        sent = ai_decide(character, interaction, rng_seed);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        g_force_target_id.store(0, std::memory_order_release);
        disarm_score_hook();
        if (use_yesmen && is_mapped(reinterpret_cast<uintptr_t>(yesmen_flag))) {
            *yesmen_flag = old_yesmen;
            bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(yesmen_flag));  // R1c
        }
        if (use_bypass && is_mapped(reinterpret_cast<uintptr_t>(bypass_flag))) {
            *bypass_flag = old_bypass;
            bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(bypass_flag));  // R1c
        }
        ADLOG("DISPATCH_V5: exception\n");
        if (tls_reason_saved[0]) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "ai_decide_exception; tls=%s", tls_reason_saved);
        } else {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "ai_decide_exception");
        }
        ADLOG("DISPATCH: chose path=v5 (exception)\n");
        _ret = false;
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    // --- Restore: disarm hook + clear force target + yesmen + bypass ---
    g_force_target_id.store(0, std::memory_order_release);
    disarm_score_hook();  // Restore original bytes before next orig() tick
    if (use_yesmen && is_mapped(reinterpret_cast<uintptr_t>(yesmen_flag))) {
        *yesmen_flag = old_yesmen;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(yesmen_flag));  // R1c
    }
    if (use_bypass && is_mapped(reinterpret_cast<uintptr_t>(bypass_flag))) {
        *bypass_flag = old_bypass;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(bypass_flag));  // R1c
    }

    if (sent) {
        ADLOG("DISPATCH_V5: === SUCCESS === %s sent (actor=%u, forced_target=%u)\n",
              intent->interaction_name.c_str(), actor_id & 0xFFFFFF, target_id & 0xFFFFFF);
        ADLOG("DISPATCH: chose path=v5 (succeeded)\n");
    } else {
        ADLOG("DISPATCH_V5: AI_INTERACTION_DECISION returned false for %s "
              "(actor=%u, target=%u) — no valid target or preconditions failed\n",
              intent->interaction_name.c_str(), actor_id & 0xFFFFFF, target_id & 0xFFFFFF);
        if (tls_reason_saved[0]) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "ai_no_valid_target; tls=%s", tls_reason_saved);
        } else {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "ai_no_valid_target");
        }
        ADLOG("DISPATCH: chose path=v5 (returned false)\n");
    }

    _ret = sent;
    return sent;
}

// =============================================================================
// War Debug Dispatch — try all dispatch paths independently
// =============================================================================
//
// Called from drain_and_execute_intents when intent->debug_all_paths is set.
// Tries each war declaration path independently, logs results per-path.
// Stops trying further paths once a war is successfully created.
// Results are written to intent->debug_log as a JSON array.

static void execute_war_debug_dispatch(Intent* intent, void* character, void* interaction) {
    volatile uint32_t actor_id = read<uint32_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::ID);
    uint32_t target_id = intent->target_id;

    ADLOG("WAR_DEBUG: === BEGIN === actor=%u target=%u\n",
          actor_id & 0xFFFFFF, target_id & 0xFFFFFF);

    // Read war count helper
    auto read_war_count = [&]() -> int32_t {
        uintptr_t tr = resolve_title_realm(reinterpret_cast<uintptr_t>(character));
        if (!tr) return -1;
        return read<int32_t>(tr + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
    };

    int32_t initial_wars = read_war_count();

    // Build JSON array manually with ResponseBuilder-style approach
    std::string paths_json = "[";
    bool war_created = false;
    const char* successful_path = nullptr;
    bool first_entry = true;

    auto append_path_result = [&](const char* path_name, const char* result,
                                   const char* log_msg, int32_t wars_before,
                                   int32_t wars_after) {
        if (!first_entry) paths_json += ",";
        first_entry = false;
        paths_json += "{\"path\":\"";
        paths_json += path_name;
        paths_json += "\",\"result\":\"";
        paths_json += result;
        paths_json += "\",\"wars_before\":";
        paths_json += std::to_string(wars_before);
        paths_json += ",\"wars_after\":";
        paths_json += std::to_string(wars_after);
        paths_json += ",\"log\":\"";
        // Escape the log message for JSON
        for (const char* p = log_msg; *p; ++p) {
            if (*p == '"') paths_json += "\\\"";
            else if (*p == '\\') paths_json += "\\\\";
            else if (*p == '\n') paths_json += "\\n";
            else if (*p == '\r') paths_json += "\\r";
            else paths_json += *p;
        }
        paths_json += "\"}";
    };

    // --- Path 1: TLS pipeline (v6) ---
    if (!war_created && target_id != 0) {
        ADLOG("WAR_DEBUG: trying path 1 — TLS pipeline (v6)\n");
        int32_t wb = read_war_count();

        // Clear any prior rejection reason
        intent->rejection_reason[0] = '\0';

        bool result = execute_tls_pipeline_dispatch(intent, character, interaction);
        int32_t wa = read_war_count();
        bool path_war = (wb >= 0 && wa > wb);

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "execute_tls_pipeline_dispatch returned %s, rejection='%s'",
                 result ? "true" : "false",
                 intent->rejection_reason[0] ? intent->rejection_reason : "none");
        ADLOG("WAR_DEBUG: path 1 result: %s\n", log_buf);

        const char* res_str = path_war ? "war_created" :
                              result ? "dispatched_no_war" :
                              intent->rejection_reason[0] ? intent->rejection_reason : "failed";
        append_path_result("tls_pipeline_v6", res_str, log_buf, wb, wa);

        if (path_war) {
            war_created = true;
            successful_path = "tls_pipeline_v6";
        }
        intent->rejection_reason[0] = '\0';
    }

    // --- Path 2: v7 direct command dispatch ---
    if (!war_created && target_id != 0 &&
        strcmp(intent->interaction_name.c_str(), "declare_war_interaction") == 0) {
        ADLOG("WAR_DEBUG: trying path 2 — v7 direct command\n");
        int32_t wb = read_war_count();

        intent->rejection_reason[0] = '\0';

        bool result = execute_direct_command_dispatch(intent, character, interaction);
        int32_t wa = read_war_count();
        bool path_war = (wb >= 0 && wa > wb);

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "execute_direct_command_dispatch returned %s, rejection='%s'",
                 result ? "true" : "false",
                 intent->rejection_reason[0] ? intent->rejection_reason : "none");
        ADLOG("WAR_DEBUG: path 2 result: %s\n", log_buf);

        const char* res_str = path_war ? "war_created" :
                              result ? "dispatched_no_war" :
                              intent->rejection_reason[0] ? intent->rejection_reason : "failed";
        append_path_result("direct_command_v7", res_str, log_buf, wb, wa);

        if (path_war) {
            war_created = true;
            successful_path = "direct_command_v7";
        }
        intent->rejection_reason[0] = '\0';
    }

    // --- Path 3: v5 AI scoring hook (AI_INTERACTION_DECISION) ---
    if (!war_created) {
        ADLOG("WAR_DEBUG: trying path 3 — v5 AI scoring hook\n");
        volatile int32_t wb = read_war_count();

        intent->rejection_reason[0] = '\0';

        // Replicate the v5 path from execute_direct_dispatch
        auto ai_decide = reinterpret_cast<bool(*)(void*, void*, uint32_t)>(
            bridge::game_fn(ck3::funcs::AI_INTERACTION_DECISION));
        uint32_t rng_seed = (actor_id ^ (g_tick_id.load() * 2654435761u)) & 0x7FFFFFFE;

        // Arm score hook to force target
        if (target_id != 0 && g_score_hook_installed) {
            g_force_target_id.store(target_id & 0xFFFFFF, std::memory_order_release);
            arm_score_hook();
        }

        watchdog_arm(5);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        bool sent = false;
        bool crashed = false;
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            g_force_target_id.store(0, std::memory_order_release);
            disarm_score_hook();
            crashed = true;
            const char* sig_name = (sig == 1) ? "SIGSEGV" :
                                   (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "ai_decide_%s", sig_name);
        } else {
            try {
                sent = ai_decide(character, interaction, rng_seed);
            } catch (...) {
                snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                         "ai_decide_exception");
            }
            g_in_protected_region = false;
            watchdog_disarm();
            g_force_target_id.store(0, std::memory_order_release);
            disarm_score_hook();
        }

        int32_t wa = read_war_count();
        bool path_war = (wb >= 0 && wa > wb);

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "AI_INTERACTION_DECISION returned %s, crashed=%s, rejection='%s'",
                 sent ? "true" : "false",
                 crashed ? "true" : "false",
                 intent->rejection_reason[0] ? intent->rejection_reason : "none");
        ADLOG("WAR_DEBUG: path 3 result: %s\n", log_buf);

        const char* res_str = path_war ? "war_created" :
                              sent ? "dispatched_no_war" :
                              crashed ? intent->rejection_reason :
                              "ai_no_valid_target";
        append_path_result("ai_scoring_v5", res_str, log_buf, wb, wa);

        if (path_war) {
            war_created = true;
            successful_path = "ai_scoring_v5";
        }
        intent->rejection_reason[0] = '\0';
    }

    // --- Path 4: WAR_DECLARE intent type (execute_war_intent — AI pipeline) ---
    if (!war_created) {
        ADLOG("WAR_DEBUG: trying path 4 — execute_war_intent (AI war pipeline)\n");
        int32_t wb = read_war_count();

        // Create a temporary intent with WAR_DECLARE type
        Intent war_intent = *intent;
        war_intent.type = IntentType::WAR_DECLARE;
        war_intent.rejection_reason[0] = '\0';
        war_intent.consumed = false;
        war_intent.executed = false;
        war_intent.rejected = false;

        bool result = execute_war_intent(&war_intent);
        int32_t wa = read_war_count();
        bool path_war = (wb >= 0 && wa > wb);

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "execute_war_intent returned %s, executed=%s, rejected=%s, rejection='%s'",
                 result ? "true" : "false",
                 war_intent.executed ? "true" : "false",
                 war_intent.rejected ? "true" : "false",
                 war_intent.rejection_reason[0] ? war_intent.rejection_reason : "none");
        ADLOG("WAR_DEBUG: path 4 result: %s\n", log_buf);

        const char* res_str = path_war ? "war_created" :
                              result ? "dispatched_no_war" :
                              war_intent.rejection_reason[0] ? war_intent.rejection_reason : "failed";
        append_path_result("war_intent_ai_pipeline", res_str, log_buf, wb, wa);

        if (path_war) {
            war_created = true;
            successful_path = "war_intent_ai_pipeline";
        }
    }

    // --- Path 5: Direct AI war pipeline (mirror console AI.declare_war) ---
    // Uses CHAR_GET_AI_CONTROLLER → war_target_eval → war_declare_exec → war_post_decl
    // with pacing flag cleared. Unlike path 4 (execute_war_intent), this calls the
    // pipeline functions directly without the intent infrastructure.
    if (!war_created) {
        ADLOG("WAR_DEBUG: trying path 5 — direct AI war pipeline\n");
        volatile int32_t wb = read_war_count();

        volatile bool result = false;
        volatile bool crashed = false;
        char crash_reason[128] = {};

        auto get_ai_ctrl = reinterpret_cast<void*(*)(void*)>(
            bridge::game_fn(ck3::funcs::CHAR_GET_AI_CONTROLLER));
        auto war_target_eval = reinterpret_cast<void*(*)(void*)>(
            bridge::game_fn(ck3::funcs::AI_WAR_TARGET_EVAL));
        auto war_declare_exec = reinterpret_cast<void(*)(void*, void*)>(
            bridge::game_fn(ck3::funcs::AI_WAR_DECLARE_EXEC));
        auto war_post_decl = reinterpret_cast<void(*)(void*, int, int)>(
            bridge::game_fn(ck3::funcs::AI_WAR_POST_DECL));

        // Clear pacing flag
        uint8_t* pacing_ptr = reinterpret_cast<uint8_t*>(
            g_base_addr + ck3::globals::AI_WAR_SUPPRESS);
        volatile uint8_t old_pacing = 0;
        volatile bool pacing_mapped = is_mapped(reinterpret_cast<uintptr_t>(pacing_ptr));
        if (pacing_mapped) {
            old_pacing = *pacing_ptr;
            // R1c (ck-6kvq.10, §2.9): the second AI_WAR_SUPPRESS pulse site.
            (void)bridge::pulse_enter_global(
                "AI_WAR_SUPPRESS", "declare_war_debug_path5",
                reinterpret_cast<uintptr_t>(pacing_ptr), 1, old_pacing);
            *pacing_ptr = 0;
        }

        watchdog_arm(8);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            if (pacing_mapped) {
                *pacing_ptr = old_pacing;
                bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pacing_ptr));  // R1c
            }
            crashed = true;
            const char* sig_name = (sig == 1) ? "SIGSEGV" :
                                   (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
            snprintf(crash_reason, sizeof(crash_reason), "direct_ai_pipeline_%s", sig_name);
        } else {
            try {
                void* ai_ctrl = get_ai_ctrl(character);
                if (ai_ctrl && is_mapped(reinterpret_cast<uintptr_t>(ai_ctrl))) {
                    void* war_data = war_target_eval(ai_ctrl);
                    war_declare_exec(ai_ctrl, war_data);
                    uint32_t rng_seed = (actor_id ^ (g_tick_id.load() * 2654435761u)) & 0x7FFFFFFE;
                    war_post_decl(ai_ctrl, static_cast<int>(rng_seed), 1);
                    result = true;
                } else {
                    snprintf(crash_reason, sizeof(crash_reason), "no_ai_controller");
                }
            } catch (...) {
                snprintf(crash_reason, sizeof(crash_reason), "direct_ai_pipeline_exception");
            }
            g_in_protected_region = false;
            watchdog_disarm();
        }

        // Restore pacing
        if (pacing_mapped) {
            *pacing_ptr = old_pacing;
            bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pacing_ptr));  // R1c
        }

        int32_t wa = read_war_count();
        bool path_war = (wb >= 0 && wa > wb);

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "direct_ai_pipeline called=%s, crashed=%s, reason='%s', pacing_was=%u",
                 result ? "true" : "false",
                 crashed ? "true" : "false",
                 crash_reason[0] ? crash_reason : "none",
                 old_pacing);
        ADLOG("WAR_DEBUG: path 5 result: %s\n", log_buf);

        const char* res_str = path_war ? "war_created" :
                              result ? "dispatched_no_war" :
                              crash_reason[0] ? crash_reason : "failed";
        append_path_result("direct_ai_pipeline", res_str, log_buf, wb, wa);

        if (path_war) {
            war_created = true;
            successful_path = "direct_ai_pipeline";
        }
    }

    paths_json += "]";

    // Build full debug_log JSON
    int32_t final_wars = read_war_count();
    std::string debug_json = "{\"paths_tried\":";
    debug_json += paths_json;
    debug_json += ",\"war_created\":";
    debug_json += war_created ? "true" : "false";
    debug_json += ",\"successful_path\":";
    if (successful_path) {
        debug_json += "\"";
        debug_json += successful_path;
        debug_json += "\"";
    } else {
        debug_json += "null";
    }
    debug_json += ",\"initial_wars\":";
    debug_json += std::to_string(initial_wars);
    debug_json += ",\"final_wars\":";
    debug_json += std::to_string(final_wars);
    debug_json += "}";

    intent->debug_log = std::move(debug_json);
    intent->executed = war_created;
    intent->consumed = true;
    if (!war_created) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "all_paths_failed");
    }

    ADLOG("WAR_DEBUG: === DONE === war_created=%s successful_path=%s initial=%d final=%d\n",
          war_created ? "true" : "false",
          successful_path ? successful_path : "none",
          initial_wars, final_wars);
}

// =============================================================================
// Deferred Acceptance Query — evaluate on game thread
// =============================================================================
//
// Called from drain_and_execute_intents() on the game thread. Evaluates whether
// a specific target would accept an interaction from a specific actor by calling
// AI_INTERACTION_DECISION with the score hook forcing the target.

void process_acceptance_query() {
    int expected = 1;  // pending
    if (!g_acceptance_query.state.compare_exchange_strong(expected, 4,
            std::memory_order_acq_rel)) {
        return;  // not pending (0=empty, 2=done, 3=error, 4=in-progress)
    }

    volatile uint32_t actor_id = g_acceptance_query.actor_id;
    volatile uint32_t target_id = g_acceptance_query.target_id;
    // Use volatile to prevent longjmp clobber warning
    const char* volatile interaction_type = g_acceptance_query.interaction_type;

    ADLOG("ACCEPTANCE_QUERY: evaluating %s actor=%u target=%u\n",
          interaction_type, actor_id & 0xFFFFFF, target_id & 0xFFFFFF);

    // Resolve character
    void* character = lookup_character(actor_id);
    if (!character) {
        snprintf(g_acceptance_query.error, sizeof(g_acceptance_query.error),
                 "actor_not_found");
        g_acceptance_query.valid = false;
        g_acceptance_query.would_accept = false;
        g_acceptance_query.state.store(3, std::memory_order_release);  // error
        ADLOG("ACCEPTANCE_QUERY: actor %u not found\n", actor_id & 0xFFFFFF);
        return;
    }


    // ── THE TARGET IS RESOLVED AND NULL-CHECKED BEFORE THE ENGINE SEES IT ──
    // A near-null SIGSEGV (fault_addr=0x1a) killed CK3 on the game thread in
    // arm A's first six-agent pilot, immediately after a deferred
    // query_interaction_acceptance for arrange_marriage actor=35395
    // target=31519. Three other acceptance queries in the same episode were
    // fine, which is what made it look data-dependent: it is a property of THE
    // TARGET, and the target was the one id this path never resolved.
    //
    // THE ACTOR WAS CHECKED. THE INTERACTION WAS CHECKED. THE TARGET WAS
    // HANDED TO THE ENGINE RAW — stored into g_force_target_id and dereferenced
    // by the game's own AI interactions dispatch, which ck3_offsets.h records
    // as reading +0x12, +0x0F, ..., +0x19, +0x1A, +0x1B ... off the character
    // it resolves. A null character plus a +0x1A field read IS fault_addr 0x1a.
    //
    // AND THE CORRECT PATTERN WAS ALREADY IN THIS FILE, one function along: the
    // CB query resolves BOTH ends — attacker_not_found AND defender_not_found —
    // before it calls anything. This is that check, missing here.
    void* target_character = lookup_character(target_id);
    if (!target_character) {
        snprintf(g_acceptance_query.error, sizeof(g_acceptance_query.error),
                 "target_not_found");
        g_acceptance_query.valid = false;
        g_acceptance_query.would_accept = false;
        g_acceptance_query.state.store(3, std::memory_order_release);  // error
        ADLOG("ACCEPTANCE_QUERY: target %u not found — REFUSING rather than "
              "handing an unresolved id to AI_INTERACTION_DECISION\n",
              target_id & 0xFFFFFF);
        return;
    }

    // Resolve interaction
    void* interaction = lookup_interaction(interaction_type);
    if (!interaction) {
        snprintf(g_acceptance_query.error, sizeof(g_acceptance_query.error),
                 "interaction_not_found");
        g_acceptance_query.valid = false;
        g_acceptance_query.would_accept = false;
        g_acceptance_query.state.store(3, std::memory_order_release);
        ADLOG("ACCEPTANCE_QUERY: interaction '%s' not found\n", interaction_type);
        return;
    }

    // Vtable-dispatch sanity ping via CInteractionDef slot 7 (offset 0x38).
    // ck-e58l.31: decomp-verified this slot is a trivial "return 1" stub on CCharacterInteractionType
    // (body: mov al,1; ret at 0x03a5c3e0), so the call does NOT filter invalid interactions — its
    // only value is proving the vtable ptr dispatches cleanly (protected by sigsetjmp below).
    uintptr_t interaction_addr = reinterpret_cast<uintptr_t>(interaction);
    uintptr_t vtable = read<uintptr_t>(interaction_addr);
    auto is_valid_fn = reinterpret_cast<bool(*)(void*)>(read<uintptr_t>(vtable + ck3::interaction_def::VTABLE_IS_VALID));

    bool valid = false;
    // Protected call for is_valid
    watchdog_arm(2);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        snprintf(g_acceptance_query.error, sizeof(g_acceptance_query.error),
                 "is_valid_crashed");
        g_acceptance_query.valid = false;
        g_acceptance_query.would_accept = false;
        g_acceptance_query.state.store(3, std::memory_order_release);
        ADLOG("ACCEPTANCE_QUERY: is_valid crashed\n");
        return;
    }
    try {
        valid = is_valid_fn(interaction);
    } catch (...) {
        valid = false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!valid) {
        g_acceptance_query.valid = false;
        g_acceptance_query.would_accept = false;
        g_acceptance_query.error[0] = '\0';
        g_acceptance_query.state.store(2, std::memory_order_release);  // done
        ADLOG("ACCEPTANCE_QUERY: interaction '%s' is not valid\n", interaction_type);
        return;
    }

    g_acceptance_query.valid = true;

    // Call AI_INTERACTION_DECISION with score hook forcing target
    // WARNING: AI_INTERACTION_DECISION is an "evaluate AND execute" function.
    // If the target would accept, the interaction IS DISPATCHED (not just queried).
    // To prevent unintended dispatch, we temporarily close the dispatch gate.
    auto ai_decide = reinterpret_cast<bool(*)(void*, void*, uint32_t)>(
        bridge::game_fn(ck3::funcs::AI_INTERACTION_DECISION));
    uint32_t rng_seed = (actor_id ^ (g_tick_id.load() * 2654435761u)) & 0x7FFFFFFE;

    // Close the dispatch gate to prevent AI_INTERACTION_DECISION from actually sending
    uintptr_t gate_addr = g_base_addr + ck3::globals::COMMAND_DISPATCH_GATE;
    uint8_t old_gate = safe_read<uint8_t>(gate_addr);
    bool gate_closed = false;
    if ((old_gate & 0xfd) == 0) {
        // Gate is open — close it to block dispatch during acceptance check
        (void)bridge::pulse_enter_global(   // R1c (ck-6kvq.10, §2.9)
            "COMMAND_DISPATCH_GATE", "acceptance_query", gate_addr, 1, old_gate);
        *reinterpret_cast<uint8_t*>(gate_addr) = old_gate | 0x04;  // set bit 2 to block
        gate_closed = true;
        ADLOG("ACCEPTANCE_QUERY: Dispatch gate temporarily closed (was 0x%02x)\n", old_gate);
    }

    // Arm score hook to force target selection
    if (target_id != 0 && g_score_hook_installed) {
        g_force_target_id.store(target_id & 0xFFFFFF, std::memory_order_release);
        arm_score_hook();
        ADLOG("ACCEPTANCE_QUERY: Score hook ARMED for target=%u\n", target_id & 0xFFFFFF);
    }

    // Call AI_INTERACTION_DECISION with SIGSEGV recovery
    watchdog_arm(5);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        g_force_target_id.store(0, std::memory_order_release);
        disarm_score_hook();
        if (gate_closed) {
            *reinterpret_cast<uint8_t*>(gate_addr) = old_gate;
            bridge::pulse_exit_addr(gate_addr);  // R1c
        }
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                               (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
        snprintf(g_acceptance_query.error, sizeof(g_acceptance_query.error),
                 "ai_decide_%s", sig_name);
        g_acceptance_query.would_accept = false;
        g_acceptance_query.state.store(3, std::memory_order_release);
        ADLOG("ACCEPTANCE_QUERY: AI_INTERACTION_DECISION crashed/timed out (%s)\n", sig_name);
        return;
    }

    bool would_accept = false;
    try {
        would_accept = ai_decide(character, interaction, rng_seed);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        g_force_target_id.store(0, std::memory_order_release);
        disarm_score_hook();
        if (gate_closed) {
            *reinterpret_cast<uint8_t*>(gate_addr) = old_gate;
            bridge::pulse_exit_addr(gate_addr);  // R1c
        }
        snprintf(g_acceptance_query.error, sizeof(g_acceptance_query.error),
                 "ai_decide_exception");
        g_acceptance_query.would_accept = false;
        g_acceptance_query.state.store(3, std::memory_order_release);
        ADLOG("ACCEPTANCE_QUERY: AI_INTERACTION_DECISION threw exception\n");
        return;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    // Clear score hook and restore dispatch gate
    g_force_target_id.store(0, std::memory_order_release);
    disarm_score_hook();
    if (gate_closed) {
        *reinterpret_cast<uint8_t*>(gate_addr) = old_gate;
        bridge::pulse_exit_addr(gate_addr);  // R1c
        ADLOG("ACCEPTANCE_QUERY: Dispatch gate restored to 0x%02x\n", old_gate);
    }

    g_acceptance_query.would_accept = would_accept;
    g_acceptance_query.error[0] = '\0';
    g_acceptance_query.state.store(2, std::memory_order_release);  // done

    ADLOG("ACCEPTANCE_QUERY: %s actor=%u target=%u result=%s\n",
          interaction_type, actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
          would_accept ? "ACCEPTED" : "REJECTED");
}

// =============================================================================
// Deferred CB Validity Query — evaluate on game thread
// =============================================================================
//
// Called from drain_and_execute_intents() on the game thread. Iterates the
// CCasusBelliTypeDatabase singleton and calls CB_VALIDITY_CHECK for each
// enabled CB type against the specified attacker/defender pair.

void process_cb_query() {
    int expected = 1;  // pending
    if (!g_cb_query.state.compare_exchange_strong(expected, 4,
            std::memory_order_acq_rel)) {
        return;  // not pending
    }

    const uint64_t svc_start_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    g_cb_query.service_start_ns.store(svc_start_ns, std::memory_order_relaxed);
    g_cb_query.service_tick.store(g_tick_id.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);

    const uint64_t submit_ns_val = g_cb_query.submit_ns.load(std::memory_order_relaxed);
    const uint64_t service_latency_us = submit_ns_val
        ? (svc_start_ns - submit_ns_val) / 1000ULL : 0;

    volatile uint32_t actor_id = g_cb_query.actor_id;
    volatile uint32_t target_id = g_cb_query.target_id;

    ADLOG("CB_QUERY: evaluating actor=%u target=%u service_latency_us=%lu "
          "submit_tick=%lu service_tick=%lu\n",
          actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
          (unsigned long)service_latency_us,
          (unsigned long)g_cb_query.submit_tick.load(std::memory_order_relaxed),
          (unsigned long)g_cb_query.service_tick.load(std::memory_order_relaxed));

    auto stamp_service_end = []() {
        g_cb_query.service_end_ns.store(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()),
            std::memory_order_relaxed);
    };

    // Resolve attacker and defender character pointers
    void* attacker = lookup_character(actor_id);
    if (!attacker) {
        snprintf(g_cb_query.error, sizeof(g_cb_query.error), "attacker_not_found");
        g_cb_query.results.clear();
        stamp_service_end();
        g_cb_query.state.store(3, std::memory_order_release);
        ADLOG("CB_QUERY: attacker %u not found\n", actor_id & 0xFFFFFF);
        return;
    }

    void* defender = lookup_character(target_id);
    if (!defender) {
        snprintf(g_cb_query.error, sizeof(g_cb_query.error), "defender_not_found");
        g_cb_query.results.clear();
        stamp_service_end();
        g_cb_query.state.store(3, std::memory_order_release);
        ADLOG("CB_QUERY: defender %u not found\n", target_id & 0xFFFFFF);
        return;
    }

    // Get CB type database
    uintptr_t cb_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CASUS_BELLI_DB);
    if (!cb_db || !is_mapped(cb_db)) {
        snprintf(g_cb_query.error, sizeof(g_cb_query.error), "cb_database_not_found");
        g_cb_query.results.clear();
        stamp_service_end();
        g_cb_query.state.store(3, std::memory_order_release);
        ADLOG("CB_QUERY: CB database not found\n");
        return;
    }

    uintptr_t cb_arr = safe_read<uintptr_t>(cb_db + ck3::game_db::ENTRIES_ARRAY);
    int32_t cb_count = safe_read<int32_t>(cb_db + ck3::game_db::LIVE_COUNT);
    if (cb_count < 0 || cb_count > 500) cb_count = 0;

    if (!cb_arr || !is_mapped(cb_arr) || cb_count == 0) {
        snprintf(g_cb_query.error, sizeof(g_cb_query.error), "cb_database_empty");
        g_cb_query.results.clear();
        stamp_service_end();
        g_cb_query.state.store(3, std::memory_order_release);
        ADLOG("CB_QUERY: CB database empty or unmapped\n");
        return;
    }

    // CB_VALIDITY_CHECK: bool(void* cb_type, void* output, void* attacker, void* defender, int skip_against)
    auto cb_validity_check = reinterpret_cast<bool(*)(void*, void*, void*, void*, int)>(
        bridge::game_fn(ck3::funcs::CB_VALIDITY_CHECK));
    // A build with no address for the engine's check (Windows: poisoned until
    // mapped) refuses here, instead of faulting once per CB and answering with
    // whatever the fallback reasons made of it (P6 full1: 68 recovered faults).
    if (!is_mapped(reinterpret_cast<uintptr_t>(cb_validity_check))) {
        snprintf(g_cb_query.error, sizeof(g_cb_query.error), "function_not_mapped");
        g_cb_query.results.clear();
        stamp_service_end();
        g_cb_query.state.store(3, std::memory_order_release);
        ADLOG("CB_QUERY: CB_VALIDITY_CHECK not mapped in this build\n");
        return;
    }

    g_cb_query.results.clear();
    g_cb_query.results.reserve(cb_count);

    // ck-9fbs: reset ENABLED_FLAG enumeration. Populated below in the cb_arr
    // loop, copied out by cb_query_strict_full_sync into CBStrictSummary.
    g_cb_query.enabled_count = 0;
    g_cb_query.disabled_count = 0;
    g_cb_query.disabled_cb_names.clear();
    g_cb_query.option_capacity_hits = 0;  // D8 guard tripwire, per service

    const bool strict_mode = g_cb_query.strict_mode;
    int strict_dropped_disabled_group = 0;
    int strict_dropped_stage2_flag    = 0;
    int strict_dropped_no_group       = 0;
    int strict_dropped_outer_trigger  = 0;  // ck-p3wn: group-level is_valid drop count
    int strict_dropped_kind_relation  = 0;  // ck-it6e: per-CB I2 relation-mode drops
    int strict_dropped_per_cb_outer   = 0;  // ck-laf5: per-CB allowed_for_character drop count
    int strict_per_cb_outer_evaluated = 0;  // ck-laf5: per-CB triggers actually evaluated
    int dropped_script_only_group     = 0;
    ADLOG("CB_QUERY: checking %d CB types (strict_mode=%d)\n",
          cb_count, strict_mode ? 1 : 0);

    // ck-p3wn (ck-exya redo): Stage 2 group-level outer-trigger gate.
    //
    // Mirrors engine AI_CB_EVALUATE_ALL decomp line 359 —
    //   `ScriptTriggerEvaluator(group+0x48, scope_ctx) != 0`
    // — closing the bridge↔engine disagreement source named in ck-mvdw. Without
    // this gate, strict mode reports CBs the engine drops at the group's
    // is_valid script for the (actor, target) pair (faith / era / government /
    // story-flag / DLC mismatches).
    //
    // KEY DIFFERENCE FROM ck-exya (commit 04c80ba, REVERTED in e1411d4):
    //   ck-exya called the TLS pipeline on the engine's TLS interaction context
    //   (`fs_base - g_tls_context_offset`). Mutating that slot polluted state
    //   the engine reads on the same thread; AI_CB_EVALUATE_ALL crashed deep
    //   in recursive trigger eval within 3-11 minutes of launch (ck-8dvo).
    //
    //   ck-p3wn uses a STACK-LOCAL `scope_buf[CMD_BUF_SIZE]`. Per Ghidra decomp
    //   of INTERACTION_TLS_CONSTRUCT (FUN_06b3e970), INTERACTION_CTX_INIT
    //   (FUN_06b45300), and InteractionCmd_Destroy (FUN_06b3ec30): each function
    //   operates exclusively on its `param_1` buffer — no FS_BASE writes, no
    //   thread-local state mutated. So calling the pipeline on an arbitrary
    //   buffer is equivalent to calling it on the engine's TLS slot, but
    //   without the cross-call pollution risk.
    //
    // CB_GROUP_DB is iterated up-front and verdicts cached per group_ptr. The
    // per-CB CB_VALIDITY_CHECK loop below may itself rebuild engine state, so
    // we don't reuse our scope_buf inside it — only the cached verdicts.
    //
    // Failure modes (graceful degrade to legacy 3-gate behavior — bridge
    // candidate set becomes a SUPERSET of engine's, the safe direction):
    //   - declare_war_interaction not found / pipeline funcs not mapped
    //   - TLS_CONSTRUCT / CTX_INIT / RESET / SET_TARGET crashes
    //   - SCRIPT_TRIGGER_EVALUATOR per-group crashes (that group treated as fail)
    //   - CB_GROUP_DB unmapped or empty
    //
    // Disable switch: env BRIDGE_NO_STAGE2_GROUP_TRIGGER=1 — defensive killswitch
    // in case of regressions. The bilateral 3-gate path (disabled / stage2 /
    // members) remains correct without this gate.
    std::unordered_map<uintptr_t, bool> group_trigger_cache;
    bool stage2_eval_ok          = false;
    bool stage2_buf_constructed  = false;
    int  group_trigger_evaluated = 0;
    int  group_trigger_passed    = 0;
    alignas(8) uint8_t scope_buf[ck3::interaction_cmd::CMD_BUF_SIZE] = {};

    static const bool stage2_disabled = []() {
        const char* env = getenv("BRIDGE_NO_STAGE2_GROUP_TRIGGER");
        return env && env[0] == '1';
    }();

    if (strict_mode && !stage2_disabled) {
        // --- Step 1: resolve declare_war_interaction (drives the scope keys). ---
        void* declare_war = lookup_interaction("declare_war_interaction");
        if (!declare_war) {
            ADLOG("CB_QUERY: ck-p3wn — declare_war_interaction not found, "
                  "Stage 2 group-trigger gate disabled for this query\n");
        } else {
            // --- Step 2: resolve the pipeline functions. ---
            auto tls_construct  = reinterpret_cast<fn_tls_construct>(
                bridge::game_fn(ck3::funcs::INTERACTION_TLS_CONSTRUCT));
            auto ctx_init       = reinterpret_cast<fn_ctx_init>(
                bridge::game_fn(ck3::funcs::INTERACTION_CTX_INIT));
            auto ctx_reset      = reinterpret_cast<fn_ctx_reset>(
                bridge::game_fn(ck3::funcs::INTERACTION_CTX_RESET));
            auto ctx_set_target = reinterpret_cast<fn_ctx_set_target>(
                bridge::game_fn(ck3::funcs::INTERACTION_CTX_SET_TARGET));
            auto trigger_eval   = reinterpret_cast<bool(*)(uintptr_t, uintptr_t)>(
                g_base_addr + ck3::trigger_eval_funcs::SCRIPT_TRIGGER_EVALUATOR);

            const bool funcs_mapped =
                is_mapped(reinterpret_cast<uintptr_t>(tls_construct))  &&
                is_mapped(reinterpret_cast<uintptr_t>(ctx_init))       &&
                is_mapped(reinterpret_cast<uintptr_t>(ctx_reset))      &&
                is_mapped(reinterpret_cast<uintptr_t>(ctx_set_target)) &&
                is_mapped(reinterpret_cast<uintptr_t>(trigger_eval));

            if (!funcs_mapped) {
                ADLOG("CB_QUERY: ck-p3wn — pipeline funcs not all mapped, "
                      "Stage 2 group-trigger gate disabled for this query\n");
            } else {
                // --- Step 3: TLS_CONSTRUCT on our STACK BUFFER (not engine TLS). ---
                watchdog_arm(3);
                g_in_protected_region = true;
                int csig = sigsetjmp(g_recovery_point, 1);
                if (csig != 0) {
                    g_in_protected_region = false;
                    watchdog_disarm();
                    ADLOG("CB_QUERY: ck-p3wn — TLS_CONSTRUCT crashed (sig=%d) on "
                          "stack scope_buf=%p\n", csig, (void*)scope_buf);
                } else {
                    try {
                        tls_construct(scope_buf);
                        stage2_buf_constructed = true;
                    } catch (...) {
                        ADLOG("CB_QUERY: ck-p3wn — TLS_CONSTRUCT threw on stack buf\n");
                    }
                    g_in_protected_region = false;
                    watchdog_disarm();
                }

                // --- Step 4: CTX_INIT + RESET + SET_TARGET on our buffer. ---
                if (stage2_buf_constructed) {
                    watchdog_arm(5);
                    g_in_protected_region = true;
                    int ssig = sigsetjmp(g_recovery_point, 1);
                    if (ssig != 0) {
                        g_in_protected_region = false;
                        watchdog_disarm();
                        ADLOG("CB_QUERY: ck-p3wn — scope setup crashed (sig=%d)\n", ssig);
                    } else {
                        try {
                            ctx_init(reinterpret_cast<uintptr_t*>(scope_buf),
                                     declare_war, attacker);
                            ctx_reset(reinterpret_cast<uintptr_t*>(scope_buf));
                            ctx_set_target(reinterpret_cast<uintptr_t*>(scope_buf),
                                           target_id, 1);
                            stage2_eval_ok = true;
                        } catch (...) {
                            ADLOG("CB_QUERY: ck-p3wn — scope setup threw\n");
                        }
                        g_in_protected_region = false;
                        watchdog_disarm();
                    }
                }

                // --- Step 5: group is_valid trigger eval is now LAZY. ---
                // ck-3715: the previous pre-evaluation loop ran
                // SCRIPT_TRIGGER_EVALUATOR for every group in CB_GROUP_DB
                // (up to 100), regardless of whether their CBs survive the
                // cheap disabled/stage2/members gates in the per-CB loop
                // below. With ck-laf5 (per-CB allowed_for_character) on top,
                // service time blew past the 2s strict-mode IPC poll
                // timeout. Lazy-eval — only call trigger_eval for groups
                // whose CBs reach the trigger check — drops the work to
                // distinct-groups-of-non-pre-rejected-CBs (typically much
                // less than 100). The cache de-duplicates triggers shared
                // across CBs within a single query.
                ADLOG("CB_QUERY: ck-p3wn — scope set up, lazy group-trigger "
                      "eval armed (stack scope_buf=%p)\n", (void*)scope_buf);
            }
        }
    }

    // ck-it6e Stage 3 I2: per-CB AI_CB_TITLE_RELATION_MODE × alliance/truce gate.
    //
    // Mirrors AI_CB_EVALUATE_ALL decomp lines 229-241 (flag construction) and
    // lines 374-377 (gate). Flags are query-scope (one alliance + one truce
    // lookup per actor/target pair) and consumed per-CB in the loop below
    // alongside AI_CB_TITLE_RELATION_MODE(cb_type, actor) which classifies
    // title.+0x738 → 0/1/2/...
    //
    // Truth table (ck-xdzo §"Truth table"):
    //   in_alliance | in_truce | passes for relation mode ∈
    //   0           | 0        | {0, 3+}
    //   0           | 1        | {0, 2, 3+}
    //   1           | 0        | {0, 1, 2, 3+}
    //   1           | 1        | {0, 1, 2, 3+}
    // Equivalent boolean:
    //   passes = (in_alliance || mode != 1) && (mode != 2 || in_alliance || in_truce)
    //
    // Per ck-xdzo §"Bridge-mirror implications" #1+#2+#4: searching for
    // target_char_ptr (vs the realm-walked derived key the engine uses at
    // FUN_06831c70) is the simpler engine-faithful read — under-rejects the
    // doctrine-positive case (faith doctrine 0x247 fast-path at decomp lines
    // 242-260) but never over-rejects. Doctrine refinement deferred to followup.
    //
    // Failure modes (all → i2_eval_ok=false → gate skipped, results match
    // legacy 5-gate behavior): helpers not mapped, list pointers unmapped,
    // bilateral lookup crash/throw, count out of bounds.
    //
    // Disable switch: env BRIDGE_NO_STAGE3_KIND_RELATION_MODE=1.
    uint8_t i2_in_alliance = 0;
    uint8_t i2_in_truce    = 0;
    bool    i2_eval_ok     = false;

    static const bool i2_disabled = []() {
        const char* env = getenv("BRIDGE_NO_STAGE3_KIND_RELATION_MODE");
        return env && env[0] == '1';
    }();

    // ck-laf5: per-CB allowed_for_character trigger gate killswitch. Disabled
    // when stage2_eval_ok==false (no scope buffer set up) — safe degrade to
    // legacy 5-gate behavior.
    static const bool per_cb_outer_disabled = []() {
        const char* env = getenv("BRIDGE_NO_PER_CB_OUTER_TRIGGER");
        return env && env[0] == '1';
    }();

    // #114: trust the inner engine CB_VALIDITY_CHECK as the authoritative
    // verdict. The two standalone bridge "outer trigger" pre-drops —
    // ck-p3wn (group is_valid at group+0x48) and ck-laf5 (per-CB
    // allowed_for_character at cb+0x38) — evaluate the EXACT SAME two
    // triggers that CB_VALIDITY_CHECK → CB_EXTENDED_TRIGGER (0x6dcec00)
    // evaluates internally a few lines below (it runs bVar1=is_valid on
    // *(cb+0x30)+0x48 and bVar2=allowed_for_character on cb+0x38). BUT the
    // bridge calls the engine evaluator via the 2-arg ScriptTriggerEvaluator
    // (trigger, scope) — which is the param_3==0 FALLBACK leg of the real
    // TriggerBlock evaluator FUN_07659f80. CB_EXTENDED_TRIGGER instead calls
    // it as FUN_0765a030(trigger, scope0, DEFENDER) — the rich leg that
    // re-derives a CB-rooted scope with the defender as a distinct arg. For
    // triggers that reference the defender (county_conquest_cb's
    // allowed_for_character / its group is_valid), the bridge's degraded
    // 2-arg leg returns a DIFFERENT (false) verdict than the engine's →
    // a "dropped:government" FALSE-NEGATIVE on a CB the engine ACCEPTS
    // (#74 / re_directional_conquest_enabler.md proved county_conquest_cb
    // is engine-valid for the feudal Halfdan dynasty; the war creates and
    // prosecutes). Since CB_VALIDITY_CHECK evaluates those triggers
    // faithfully right here in the loop, the standalone pre-drops are
    // redundant and only ever produce false-negatives — so they are
    // demoted to DIAGNOSTIC-ONLY (still evaluated + counted for telemetry,
    // never emit a `dropped` result; the CB falls through to the inner
    // check, which is the verdict). The cheap STRUCTURAL gates (script-only
    // group, disabled/stage2/no-members group flags, kind_relation_mode)
    // stay hard drops — they filter CBs the inner check can't distinguish.
    // Killswitch BRIDGE_DISTRUST_INNER_CB_VALIDITY=1 restores the old
    // pre-drop behavior for A/B debugging.
    static const bool distrust_inner_cb_validity = []() {
        const char* env = getenv("BRIDGE_DISTRUST_INNER_CB_VALIDITY");
        return env && env[0] == '1';
    }();

    if (strict_mode && !i2_disabled) {
        // #119 FIX: alliance via the AUTHORITATIVE relation array (core_data+0x20 →
        // CDiplomaticRelation; allied == REASON_COUNT@+0x94 != 0 && BROKEN_FLAG@+0x189
        // == 0), mirroring engine IsAlliedTo. The previous MAYBE_GET_ALLIANCE_LIST
        // (+0x578) search matched WAR PARTICIPANTS, corrupting the relation-mode gate
        // (false in_alliance). TRUCE keeps GET_TRUCE_LIST (+0x6A0 = genuine truce list).
        auto get_truce_list = reinterpret_cast<uintptr_t(*)(void*)>(
            bridge::game_fn(ck3::funcs::GET_TRUCE_LIST));
        auto simd_find = reinterpret_cast<uintptr_t(*)(uintptr_t, uintptr_t, void*)>(
            bridge::game_fn(ck3::funcs::SIMD_FIND_LONG));

        const bool i2_funcs_mapped =
            is_mapped(reinterpret_cast<uintptr_t>(get_truce_list))    &&
            is_mapped(reinterpret_cast<uintptr_t>(simd_find));

        if (!i2_funcs_mapped) {
            ADLOG("CB_QUERY: ck-it6e — bilateral helpers not mapped, "
                  "I2 gate disabled for this query\n");
        } else {
            watchdog_arm(2);
            g_in_protected_region = true;
            int isig = sigsetjmp(g_recovery_point, 1);
            if (isig != 0) {
                g_in_protected_region = false;
                watchdog_disarm();
                ADLOG("CB_QUERY: ck-it6e — bilateral lookup crashed (sig=%d), "
                      "I2 gate disabled for this query\n", isig);
            } else {
                try {
                    // --- Alliance: authoritative relation-array walk (#119) ---
                    uint32_t def_idm = target_id & 0xFFFFFF;
                    uintptr_t atk_core = safe_read<uintptr_t>(
                        reinterpret_cast<uintptr_t>(attacker) + ck3::character::REALM_DATA);
                    if (atk_core && is_mapped(atk_core) && def_idm != 0) {
                        uintptr_t rel_arr = safe_read<uintptr_t>(atk_core + ck3::relation_container::ARRAY_PTR);
                        int32_t   rel_cnt = safe_read<int32_t>(atk_core + ck3::relation_container::COUNT);
                        if (rel_cnt < 0 || rel_cnt > 5000) rel_cnt = 0;
                        if (rel_arr && is_mapped(rel_arr) && rel_cnt > 0) {
                            for (int32_t ri = 0; ri < rel_cnt; ri++) {
                                uintptr_t ent = rel_arr + ri * ck3::relation_container::ENTRY_STRIDE;
                                uint32_t oid = safe_read<uint32_t>(ent + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
                                if (oid != def_idm) continue;
                                uintptr_t rel = safe_read<uintptr_t>(ent + ck3::relation_container::ENTRY_PTR);
                                if (rel && is_mapped(rel)) {
                                    int32_t reasons = safe_read<int32_t>(rel + ck3::alliance::REASON_COUNT);
                                    uint8_t broken  = safe_read<uint8_t>(rel + ck3::alliance::BROKEN_FLAG);
                                    if (reasons > 0 && reasons <= 20 && broken == 0) i2_in_alliance = 1;
                                }
                                break;
                            }
                        }
                    }
                    uintptr_t tlist = get_truce_list(attacker);
                    if (tlist && is_mapped(tlist)) {
                        uintptr_t t_begin = safe_read<uintptr_t>(tlist + 0);
                        int32_t   t_count = safe_read<int32_t>(tlist + 0xC);
                        if (t_begin && is_mapped(t_begin) &&
                            t_count > 0 && t_count < 1000) {
                            uintptr_t t_end = t_begin + (size_t)t_count * 8;
                            uintptr_t hit = simd_find(t_begin, t_end, defender);
                            i2_in_truce = (hit != 0 && hit != t_end) ? 1 : 0;
                        }
                    }
                    i2_eval_ok = true;
                } catch (...) {
                    ADLOG("CB_QUERY: ck-it6e — bilateral lookup threw, "
                          "I2 gate disabled for this query\n");
                }
                g_in_protected_region = false;
                watchdog_disarm();
            }
            ADLOG("CB_QUERY: ck-it6e — bilateral flags actor=%u target=%u "
                  "in_alliance=%u in_truce=%u eval_ok=%d\n",
                  actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
                  i2_in_alliance, i2_in_truce, i2_eval_ok ? 1 : 0);
        }
    }

    for (int32_t i = 0; i < cb_count && i < 100; i++) {
        uintptr_t cb_type = safe_read<uintptr_t>(cb_arr + i * 8);
        if (!cb_type || !is_mapped(cb_type)) continue;

        // Check if CB is enabled
        uint8_t enabled = safe_read<uint8_t>(cb_type + ck3::cb_type::ENABLED_FLAG);
        if (!enabled) {
            // ck-9fbs: capture the DLC-disabled / has_game_rule subset for
            // the current install. Disabled CBs never reach `results`, so
            // count + name them here for the summary block.
            g_cb_query.disabled_count++;
            const char* dn_ptr = nullptr;
            size_t dn_len = 0;
            if (read_db_obj_name(cb_type, dn_ptr, dn_len) && dn_ptr && dn_len > 0) {
                g_cb_query.disabled_cb_names.emplace_back(dn_ptr, dn_len);
            } else {
                uint32_t dn_idx = safe_read<uint32_t>(cb_type + ck3::cb_type::DB_INDEX);
                char fallback[32];
                snprintf(fallback, sizeof(fallback), "cb_idx_%u", dn_idx);
                g_cb_query.disabled_cb_names.emplace_back(fallback);
            }
            continue;
        }
        g_cb_query.enabled_count++;

        // Read CB metadata (pure memory reads, safe)
        uint32_t cb_idx = safe_read<uint32_t>(cb_type + ck3::cb_type::DB_INDEX);
        int32_t target_mode = safe_read<int32_t>(cb_type + ck3::cb_type::TARGET_MODE);

        // Read CB name (used by both outer-drop result entries and inner-eval
        // result entries below).
        char cb_name[128] = {};
        const char* name_ptr = nullptr;
        size_t name_len = 0;
        if (read_db_obj_name(cb_type, name_ptr, name_len)) {
            size_t copy_len = (name_len < sizeof(cb_name) - 1) ? name_len : sizeof(cb_name) - 1;
            memcpy(cb_name, name_ptr, copy_len);
            cb_name[copy_len] = '\0';
        } else {
            snprintf(cb_name, sizeof(cb_name), "cb_%u", cb_idx);
        }

        // ck-eteb: universal upfront script-only group filter. The engine's
        // AI declare_war pipeline never selects CBs whose group is one of
        // {civil_war, event, migration, religious_script_only} — those CBs are
        // spawned by event scripts (faction wars, peasant uprisings, scripted
        // invasions) and are not candidates for AI-driven war declaration. The
        // engine's per-CB CB_VALIDITY_CHECK can still return true for them
        // (their is_valid scripts pass), so without this filter the bridge
        // returns valid=true for e.g. claimant_faction_war / depose_war and
        // misleads find-and-declare callers. Applied in BOTH strict and
        // non-strict modes since the contamination is the same. See
        // re/findings/re_a1_67cb_rejection_classification_2026-04-25.md for
        // the 36-CB upfront-ineligible enumeration.
        uintptr_t group_ptr = safe_read<uintptr_t>(cb_type + ck3::cb_type::SUB_OBJ_PTR);
        char group_name[64] = {};
        if (group_ptr && is_mapped(group_ptr)) {
            const char* gn_ptr = nullptr;
            size_t gn_len = 0;
            if (read_db_obj_name(group_ptr, gn_ptr, gn_len)) {
                size_t gn_copy = (gn_len < sizeof(group_name) - 1)
                                 ? gn_len : sizeof(group_name) - 1;
                memcpy(group_name, gn_ptr, gn_copy);
                group_name[gn_copy] = '\0';
            }
        }
        // FIXPLAN A8: added "independence" to the drop set. The `independence`
        // group (CB_GROUP_DB idx 12) has exactly two members —
        // independence_war (idx 8, engine-DISABLED enabled=0) and
        // tributary_independence_war (idx 50) — both VASSAL-initiated
        // independence revolts (group=independence, kind=relational/liege_relation),
        // NOT a top-liege offensive attack CB. Verified against
        // re/findings/registries/cb_groups.json (idx 12, member_count 2) +
        // re_a1_67cb_rejection_classification_2026-04-25.md + re_cb_type_params.md.
        // Before this they leaked into the strict enumerator as bogus
        // attacker-declarable candidates (CB_VALIDITY_CHECK could pass their
        // is_valid), misleading find-and-declare callers into a wasted/illegal
        // declare. No legitimate AI offensive declare uses this group.
        const bool group_script_only =
            group_name[0] != '\0' &&
            (strcmp(group_name, "civil_war") == 0 ||
             strcmp(group_name, "event") == 0 ||
             strcmp(group_name, "migration") == 0 ||
             strcmp(group_name, "independence") == 0 ||
             strcmp(group_name, "religious_script_only") == 0);
        if (group_script_only) {
            dropped_script_only_group++;
            CBQueryResult dropped;
            dropped.cb_index = cb_idx;
            strncpy(dropped.cb_name, cb_name, sizeof(dropped.cb_name) - 1);
            dropped.cb_name[sizeof(dropped.cb_name) - 1] = '\0';
            dropped.valid = false;
            dropped.target_mode = target_mode;
            dropped.eval_us = 0;
            if (strict_mode) {
                strncpy(dropped.outer_status, "dropped",
                        sizeof(dropped.outer_status) - 1);
                dropped.outer_status[sizeof(dropped.outer_status) - 1] = '\0';
                strncpy(dropped.outer_reason, "group_script_only",
                        sizeof(dropped.outer_reason) - 1);
                dropped.outer_reason[sizeof(dropped.outer_reason) - 1] = '\0';
                strncpy(dropped.inner_status, "unrun",
                        sizeof(dropped.inner_status) - 1);
                dropped.inner_status[sizeof(dropped.inner_status) - 1] = '\0';
            } else {
                dropped.outer_status[0] = '\0';
                dropped.outer_reason[0] = '\0';
                dropped.inner_status[0] = '\0';
            }
            g_cb_query.results.push_back(dropped);
            continue;
        }

        // Strict mode (ck-39e1 Stage 2 outer-trigger flag mirror, extended in
        // ck-5qpg to surface per-CB outer verdicts with reason strings,
        // completed in ck-p3wn (ck-exya redo) which closes the bridge↔engine
        // disagreement by adding the missing ScriptTriggerEvaluator gate):
        // Apply group-level filter that AI_CB_EVALUATE_ALL runs before
        // CB_VALIDITY_CHECK. The engine iterates CB_GROUP_DB and, per group,
        // requires:  group+0x208 == 0  AND  ScriptTrigger(group+0x48,…) AND
        //            group+0x20a == 0  AND  group+0x3c (members count) > 0.
        // The script-trigger verdict is captured up-front (above) into
        // `group_trigger_cache` keyed on group_ptr, since the per-CB
        // CB_VALIDITY_CHECK below may itself rebuild engine state. If scope
        // setup failed (`stage2_eval_ok==false`) the cache is empty and the
        // legacy 3-gate behavior applies (conservative — returns candidate
        // set ⊇ engine's). Group resolved via cb_type::SUB_OBJ_PTR (already
        // read above for the ck-eteb script-only filter).
        //
        // Per-CB outer triggers (cb_type::ALLOWED_FOR_CHAR / ALLOWED_AGAINST /
        // VALID_TO_START_*) need a CB-rooted scope context — out of scope for
        // this bead, deferred to ck-39e1.
        if (strict_mode) {
            const char* outer_drop_reason = nullptr;
            if (!group_ptr || !is_mapped(group_ptr)) {
                strict_dropped_no_group++;
                outer_drop_reason = "no_group";
            } else {
                uint8_t  grp_disabled = safe_read<uint8_t>(group_ptr + ck3::cb_group::DISABLED_FLAG);
                uint8_t  grp_stage2 = safe_read<uint8_t>(group_ptr + ck3::cb_group::STAGE2_FLAG);
                int32_t  grp_members = safe_read<int32_t>(group_ptr + ck3::cb_group::MEMBERS_COUNT);
                if (grp_disabled != 0) {
                    strict_dropped_disabled_group++;
                    outer_drop_reason = "group_disabled";
                } else if (grp_stage2 != 0) {
                    strict_dropped_stage2_flag++;
                    outer_drop_reason = "group_stage2_flag";
                } else if (grp_members <= 0) {
                    strict_dropped_no_group++;
                    outer_drop_reason = "group_no_members";
                } else if (stage2_eval_ok) {
                    // ck-p3wn: group-level is_valid trigger gate.
                    // ck-3715: lazy-eval. Trigger runs on the actor-bound
                    // stack scope_buf set up above; never touches engine
                    // TLS. Cache de-duplicates across CBs that share a
                    // group within this query. Crash/throw → fail-closed
                    // for that group (cached as false so siblings reuse).
                    auto it = group_trigger_cache.find(group_ptr);
                    bool grp_trigger_ok;
                    if (it != group_trigger_cache.end()) {
                        grp_trigger_ok = it->second;
                    } else {
                        auto trigger_eval_fn =
                            reinterpret_cast<bool(*)(uintptr_t, uintptr_t)>(
                                g_base_addr +
                                ck3::trigger_eval_funcs::SCRIPT_TRIGGER_EVALUATOR);
                        const uintptr_t scope_arg =
                            reinterpret_cast<uintptr_t>(scope_buf)
                            + ck3::interaction_ctx::SCOPE_LIST;
                        grp_trigger_ok = false;
                        if (is_mapped(reinterpret_cast<uintptr_t>(trigger_eval_fn))) {
                            watchdog_arm(2);
                            g_in_protected_region = true;
                            int gsig = sigsetjmp(g_recovery_point, 1);
                            if (gsig != 0) {
                                g_in_protected_region = false;
                                watchdog_disarm();
                                ADLOG("CB_QUERY: ck-p3wn — group_trigger crashed "
                                      "(sig=%d) for group=0x%lx, treating as fail\n",
                                      gsig, (unsigned long)group_ptr);
                                grp_trigger_ok = false;
                            } else {
                                try {
                                    grp_trigger_ok = trigger_eval_fn(
                                        group_ptr + ck3::cb_group::IS_VALID,
                                        scope_arg);
                                } catch (...) {
                                    grp_trigger_ok = false;
                                    ADLOG("CB_QUERY: ck-p3wn — group_trigger threw "
                                          "for group=0x%lx, treating as fail\n",
                                          (unsigned long)group_ptr);
                                }
                                g_in_protected_region = false;
                                watchdog_disarm();
                            }
                        }
                        group_trigger_cache[group_ptr] = grp_trigger_ok;
                        ++group_trigger_evaluated;
                        if (grp_trigger_ok) ++group_trigger_passed;
                    }
                    if (!grp_trigger_ok) {
                        // #114: diagnostic-only unless distrust flag set.
                        // The same group is_valid trigger is re-run with the
                        // engine's full defender scope inside CB_VALIDITY_CHECK
                        // below; this degraded 2-arg verdict only produces
                        // false-negatives, so we count it but do not drop.
                        strict_dropped_outer_trigger++;
                        if (distrust_inner_cb_validity) {
                            outer_drop_reason = "group_outer_trigger";
                        } else {
                            ADLOG("CB_QUERY: #114 — group_outer_trigger would "
                                  "drop cb=%s (group=0x%lx) but inner "
                                  "CB_VALIDITY_CHECK is authoritative; "
                                  "diagnostic-only\n",
                                  cb_name, (unsigned long)group_ptr);
                        }
                    }
                }
            }
            if (outer_drop_reason) {
                CBQueryResult dropped;
                dropped.cb_index = cb_idx;
                strncpy(dropped.cb_name, cb_name, sizeof(dropped.cb_name) - 1);
                dropped.cb_name[sizeof(dropped.cb_name) - 1] = '\0';
                dropped.valid = false;
                dropped.target_mode = target_mode;
                dropped.eval_us = 0;
                strncpy(dropped.outer_status, "dropped", sizeof(dropped.outer_status) - 1);
                dropped.outer_status[sizeof(dropped.outer_status) - 1] = '\0';
                strncpy(dropped.outer_reason, outer_drop_reason, sizeof(dropped.outer_reason) - 1);
                dropped.outer_reason[sizeof(dropped.outer_reason) - 1] = '\0';
                strncpy(dropped.inner_status, "unrun", sizeof(dropped.inner_status) - 1);
                dropped.inner_status[sizeof(dropped.inner_status) - 1] = '\0';
                g_cb_query.results.push_back(dropped);
                continue;
            }
        }

        // ck-it6e Stage 3 I2: per-CB AI_CB_TITLE_RELATION_MODE × alliance/truce
        // gate. Mirrors AI_CB_EVALUATE_ALL decomp lines 374-377. Bilateral
        // flags `i2_in_alliance` / `i2_in_truce` were computed once before the
        // loop. Skipped silently when `i2_eval_ok==false` (helpers unmapped,
        // setup crash, or BRIDGE_NO_STAGE3_KIND_RELATION_MODE=1) — bridge
        // candidate set then matches legacy 5-gate behavior (a SUPERSET of
        // engine's, the safe direction).
        if (strict_mode && i2_eval_ok) {
            auto get_relation_mode = reinterpret_cast<int(*)(void*, void*)>(
                bridge::game_fn(ck3::funcs::AI_CB_TITLE_RELATION_MODE));
            int  relation_mode    = 0;
            bool relation_mode_ok = false;
            if (is_mapped(reinterpret_cast<uintptr_t>(get_relation_mode))) {
                watchdog_arm(2);
                g_in_protected_region = true;
                int rsig = sigsetjmp(g_recovery_point, 1);
                if (rsig != 0) {
                    g_in_protected_region = false;
                    watchdog_disarm();
                    ADLOG("CB_QUERY: ck-it6e — AI_CB_TITLE_RELATION_MODE "
                          "crashed (sig=%d) for cb=%s, skipping I2 for this CB\n",
                          rsig, cb_name);
                } else {
                    try {
                        relation_mode = get_relation_mode(
                            reinterpret_cast<void*>(cb_type), attacker);
                        relation_mode_ok = true;
                    } catch (...) {
                        ADLOG("CB_QUERY: ck-it6e — AI_CB_TITLE_RELATION_MODE "
                              "threw for cb=%s\n", cb_name);
                    }
                    g_in_protected_region = false;
                    watchdog_disarm();
                }
            }
            if (relation_mode_ok) {
                const bool passes =
                    (i2_in_alliance != 0 || relation_mode != 1) &&
                    (relation_mode != 2 || i2_in_alliance != 0 ||
                     i2_in_truce != 0);
                if (!passes) {
                    strict_dropped_kind_relation++;
                    CBQueryResult dropped;
                    dropped.cb_index = cb_idx;
                    strncpy(dropped.cb_name, cb_name,
                            sizeof(dropped.cb_name) - 1);
                    dropped.cb_name[sizeof(dropped.cb_name) - 1] = '\0';
                    dropped.valid = false;
                    dropped.target_mode = target_mode;
                    dropped.eval_us = 0;
                    strncpy(dropped.outer_status, "dropped",
                            sizeof(dropped.outer_status) - 1);
                    dropped.outer_status[sizeof(dropped.outer_status) - 1] = '\0';
                    strncpy(dropped.outer_reason, "kind_relation_mode_filter",
                            sizeof(dropped.outer_reason) - 1);
                    dropped.outer_reason[sizeof(dropped.outer_reason) - 1] = '\0';
                    strncpy(dropped.inner_status, "unrun",
                            sizeof(dropped.inner_status) - 1);
                    dropped.inner_status[sizeof(dropped.inner_status) - 1] = '\0';
                    g_cb_query.results.push_back(dropped);
                    ADLOG("CB_QUERY: ck-it6e — drop cb=%s mode=%d "
                          "in_alliance=%u in_truce=%u\n",
                          cb_name, relation_mode,
                          i2_in_alliance, i2_in_truce);
                    continue;
                }
            }
        }

        // ck-laf5: per-CB allowed_for_character trigger gate.
        //
        // Mirrors AI_CB_EVALUATE_ALL's per-CB outer filter (after the group
        // is_valid gate ck-p3wn covered). Calls SCRIPT_TRIGGER_EVALUATOR on
        // cb_type+ALLOWED_FOR_CHAR with the actor-bound stack scope_buf set
        // up by ck-p3wn. When the trigger returns false, the CB is dropped
        // with outer_reason looked up in the static classification table at
        // bridge/include/cb_outer_filter_table.inc — produced from the 83-CB
        // classifier in re/findings/re_a1_67cb_rejection_classification_2026-04-25.md.
        //
        // Pass/fail is engine-faithful (engine evaluator on the engine's CB
        // trigger). The static table only labels the failure with one of:
        // "government" / "liege" / "tier" / "innovation" / "struggle" /
        // "independence" / "doctrine" / "herder" / "no_outer_filter".
        // CBs not present in the table (e.g. script-only / debug groups, or
        // additions post-classification) fall through with "allowed_for_character"
        // as a generic reason.
        //
        // Gate is skipped when stage2_eval_ok==false (no scope set up — same
        // condition that would skip ck-p3wn's group-level eval) or when
        // BRIDGE_NO_PER_CB_OUTER_TRIGGER=1. Failure modes (mapped funcs, scope
        // crash) degrade gracefully via the same sigsetjmp pattern used for
        // ck-p3wn — drop is conservative (skip, treat as pass) to keep the
        // bridge candidate set as a SUPERSET of the engine's.
        if (strict_mode && stage2_eval_ok && !per_cb_outer_disabled) {
            auto trigger_eval = reinterpret_cast<bool(*)(uintptr_t, uintptr_t)>(
                g_base_addr + ck3::trigger_eval_funcs::SCRIPT_TRIGGER_EVALUATOR);
            const uintptr_t scope_arg =
                reinterpret_cast<uintptr_t>(scope_buf)
                + ck3::interaction_ctx::SCOPE_LIST;
            const uintptr_t afc_trigger = cb_type + ck3::cb_type::ALLOWED_FOR_CHAR;

            bool afc_ok       = true;   // conservative: pass on crash
            bool afc_eval_ran = false;
            if (is_mapped(reinterpret_cast<uintptr_t>(trigger_eval)) &&
                is_mapped(afc_trigger)) {
                watchdog_arm(2);
                g_in_protected_region = true;
                int afc_sig = sigsetjmp(g_recovery_point, 1);
                if (afc_sig != 0) {
                    g_in_protected_region = false;
                    watchdog_disarm();
                    ADLOG("CB_QUERY: ck-laf5 — afc trigger crashed "
                          "(sig=%d) for cb=%s, treating as pass\n",
                          afc_sig, cb_name);
                } else {
                    try {
                        afc_ok = trigger_eval(afc_trigger, scope_arg);
                        afc_eval_ran = true;
                    } catch (...) {
                        afc_ok = true;
                        ADLOG("CB_QUERY: ck-laf5 — afc trigger threw for cb=%s, "
                              "treating as pass\n", cb_name);
                    }
                    g_in_protected_region = false;
                    watchdog_disarm();
                }
            }

            if (afc_eval_ran) {
                strict_per_cb_outer_evaluated++;
                if (!afc_ok) {
                    strict_dropped_per_cb_outer++;
                    const char* reason_tag = cb_outer_filter_lookup(cb_name);
                    if (!reason_tag) reason_tag = "allowed_for_character";
                    // #114: this is the source of the "dropped:government"
                    // FALSE-NEGATIVE. The per-CB allowed_for_character trigger
                    // (cb+0x38) is re-evaluated by CB_VALIDITY_CHECK below with
                    // the engine's full defender scope. The 2-arg eval here
                    // returns false for defender-referencing conquest CBs that
                    // the engine actually accepts (#74). Demote to diagnostic
                    // unless the distrust killswitch is set; let the inner
                    // check decide.
                    if (!distrust_inner_cb_validity) {
                        ADLOG("CB_QUERY: #114 — per_cb_outer would drop cb=%s "
                              "reason=%s but inner CB_VALIDITY_CHECK is "
                              "authoritative; diagnostic-only\n",
                              cb_name, reason_tag);
                    } else {
                    CBQueryResult dropped;
                    dropped.cb_index = cb_idx;
                    strncpy(dropped.cb_name, cb_name,
                            sizeof(dropped.cb_name) - 1);
                    dropped.cb_name[sizeof(dropped.cb_name) - 1] = '\0';
                    dropped.valid = false;
                    dropped.target_mode = target_mode;
                    dropped.eval_us = 0;
                    strncpy(dropped.outer_status, "dropped",
                            sizeof(dropped.outer_status) - 1);
                    dropped.outer_status[sizeof(dropped.outer_status) - 1] = '\0';
                    strncpy(dropped.outer_reason, reason_tag,
                            sizeof(dropped.outer_reason) - 1);
                    dropped.outer_reason[sizeof(dropped.outer_reason) - 1] = '\0';
                    strncpy(dropped.inner_status, "unrun",
                            sizeof(dropped.inner_status) - 1);
                    dropped.inner_status[sizeof(dropped.inner_status) - 1] = '\0';
                    g_cb_query.results.push_back(dropped);
                    ADLOG("CB_QUERY: ck-laf5 — drop cb=%s reason=%s\n",
                          cb_name, reason_tag);
                    continue;
                    }  // end distrust_inner_cb_validity drop branch
                }
            }
        }

        // Call CB_VALIDITY_CHECK with SIGSEGV recovery
        bool valid = false;
        const uint64_t cb_t0_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            const uint64_t cb_crash_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            ADLOG("CB_QUERY: CB_VALIDITY_CHECK crashed for %s (sig=%d) after %lu us\n",
                  cb_name, sig, (unsigned long)((cb_crash_ns - cb_t0_ns) / 1000ULL));
            continue;  // skip this CB, try the next
        }
        try {
            // CB_VALIDITY_CHECK output param is a CDynArray*, NOT a flat buffer.
            // Layout: {void* data(+0), uint32 capacity(+8), uint32 count(+0xC), void** allocator(+0x10)}
            // For claim_cb (mode=6), the function appends 0x98-byte entries.
            //
            // D8 (probe P4): THIS was the crashing site of the 07-31/08-01
            // minidumps — the old 32-entry STACK buffer hit capacity==count==32
            // and the engine took the growth path through the invalid
            // CB_OPTION_ALLOCATOR_VTABLE (SIGSEGV inside engine code that the
            // sigsetjmp wrap cannot catch; CK3's crash handler owns the
            // signal). Use the shared large BSS buffer so the growth path is
            // unreachable, and tripwire loudly if an enumeration ever fills it.
            memset(g_cb_option_storage, 0, sizeof(g_cb_option_storage));
            void* alloc_vtable = reinterpret_cast<void*>(g_base_addr + pdx_array::CB_OPTION_ALLOCATOR_VTABLE);
            struct {
                void*    data;
                uint32_t capacity;
                uint32_t count;
                void**   allocator;
            } cb_output_list;
            void* alloc_obj = alloc_vtable;
            cb_output_list.data = g_cb_option_storage;
            cb_output_list.capacity = kCBOptionEntryCap;
            cb_output_list.count = 0;
            cb_output_list.allocator = &alloc_obj;

            valid = cb_validity_check(
                reinterpret_cast<void*>(cb_type),
                &cb_output_list,
                attacker,
                defender,
                0          // skip_against = 0 (check all triggers)
            );
            if (cb_output_list.count >= kCBOptionEntryCap) {
                g_cb_query.option_capacity_hits++;
                ADLOG("CB_QUERY: D8 GUARD: option enumeration for %s filled the "
                      "%u-entry buffer (count=%u) — the fatal engine growth path "
                      "was one entry away; option data truncated, raise "
                      "kCBOptionEntryCap\n",
                      cb_name, kCBOptionEntryCap, cb_output_list.count);
            }
        } catch (...) {
            valid = false;
            ADLOG("CB_QUERY: CB_VALIDITY_CHECK exception for %s\n", cb_name);
        }
        g_in_protected_region = false;
        watchdog_disarm();

        const uint64_t cb_t1_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const uint64_t cb_eval_us = (cb_t1_ns - cb_t0_ns) / 1000ULL;

        // Store result (both valid and invalid, so caller can see the full picture)
        CBQueryResult result;
        result.cb_index = cb_idx;
        strncpy(result.cb_name, cb_name, sizeof(result.cb_name) - 1);
        result.cb_name[sizeof(result.cb_name) - 1] = '\0';
        result.valid = valid;
        result.target_mode = target_mode;
        result.eval_us = cb_eval_us;
        // Strict-mode verdict surface (ck-5qpg): inner check ran here, so
        // outer is implicitly "pass" under our Stage 1+2 mirror. In non-strict
        // mode the outer fields stay empty; legacy callers ignore them.
        if (strict_mode) {
            strncpy(result.outer_status, "pass", sizeof(result.outer_status) - 1);
            result.outer_status[sizeof(result.outer_status) - 1] = '\0';
            result.outer_reason[0] = '\0';
            const char* inner_tag = valid ? "pass" : "fail";
            strncpy(result.inner_status, inner_tag, sizeof(result.inner_status) - 1);
            result.inner_status[sizeof(result.inner_status) - 1] = '\0';
        } else {
            result.outer_status[0] = '\0';
            result.outer_reason[0] = '\0';
            result.inner_status[0] = '\0';
        }
        g_cb_query.results.push_back(result);

        ADLOG("CB_QUERY: CB_VALIDITY_CHECK(%s, idx=%u, mode=%d) returned %d (%lu us)\n",
              cb_name, cb_idx, target_mode, valid ? 1 : 0,
              (unsigned long)cb_eval_us);
    }

    g_cb_query.error[0] = '\0';

    // ck-p3wn: tear down our stack-local scope buffer. Always paired with
    // a successful TLS_CONSTRUCT (`stage2_buf_constructed`), regardless of
    // whether per-group eval succeeded — the destructor null-checks the
    // smart-ptr at +0x330 and the dynamic buffer at +0x300/+0x310 before
    // freeing, so a partially-set-up buffer is safe to destruct.
    if (stage2_buf_constructed) {
        auto tls_destruct = reinterpret_cast<fn_tls_destructor>(
            bridge::game_fn(ck3::funcs::INTERACTION_TLS_DESTRUCTOR));
        if (is_mapped(reinterpret_cast<uintptr_t>(tls_destruct))) {
            watchdog_arm(2);
            g_in_protected_region = true;
            int dsig = sigsetjmp(g_recovery_point, 1);
            if (dsig != 0) {
                g_in_protected_region = false;
                watchdog_disarm();
                ADLOG("CB_QUERY: ck-p3wn — destructor crashed (sig=%d) on stack scope_buf\n",
                      dsig);
            } else {
                try {
                    tls_destruct(scope_buf);
                } catch (...) {
                    ADLOG("CB_QUERY: ck-p3wn — destructor threw on stack scope_buf\n");
                }
                g_in_protected_region = false;
                watchdog_disarm();
            }
        }
    }

    const uint64_t svc_end_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    g_cb_query.service_end_ns.store(svc_end_ns, std::memory_order_relaxed);
    g_cb_query.state.store(2, std::memory_order_release);  // done

    int valid_count = 0;
    uint64_t total_eval_us = 0;
    uint64_t slowest_us = 0;
    const char* slowest_name = "";
    for (const auto& r : g_cb_query.results) {
        if (r.valid) valid_count++;
        total_eval_us += r.eval_us;
        if (r.eval_us > slowest_us) {
            slowest_us = r.eval_us;
            slowest_name = r.cb_name;
        }
    }
    const int outer_dropped_total = strict_dropped_disabled_group
                                  + strict_dropped_stage2_flag
                                  + strict_dropped_no_group
                                  + strict_dropped_outer_trigger
                                  + strict_dropped_kind_relation
                                  + strict_dropped_per_cb_outer
                                  + dropped_script_only_group;
    const uint64_t total_service_us = (svc_end_ns - svc_start_ns) / 1000ULL;
    ADLOG("CB_QUERY: done — %d/%zu valid CBs for actor=%u target=%u "
          "total_service_us=%lu total_eval_us=%lu slowest=%s(%lu us) "
          "strict_mode=%d outer_filter_dropped=%d "
          "strict_drops(disabled=%d,stage2=%d,no_group=%d,outer_trigger=%d,"
          "kind_relation=%d,per_cb_outer=%d,script_only=%d) "
          "ck-p3wn(scope_ok=%d,grp_eval=%d,grp_pass=%d) "
          "ck-it6e(i2_eval_ok=%d,in_alliance=%u,in_truce=%u) "
          "ck-laf5(per_cb_outer_eval=%d)\n",
          valid_count, g_cb_query.results.size(),
          actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
          (unsigned long)total_service_us, (unsigned long)total_eval_us,
          slowest_name, (unsigned long)slowest_us,
          strict_mode ? 1 : 0,
          outer_dropped_total,
          strict_dropped_disabled_group,
          strict_dropped_stage2_flag,
          strict_dropped_no_group,
          strict_dropped_outer_trigger,
          strict_dropped_kind_relation,
          strict_dropped_per_cb_outer,
          dropped_script_only_group,
          stage2_eval_ok ? 1 : 0,
          group_trigger_evaluated,
          group_trigger_passed,
          i2_eval_ok ? 1 : 0,
          i2_in_alliance,
          i2_in_truce,
          strict_per_cb_outer_evaluated);
}

// =============================================================================
// Synchronous CB validity probe — IPC-thread entry point (ck-l0re)
// =============================================================================
//
// Reuses g_cb_query to drive the engine's per-CB CB_VALIDITY_CHECK pipeline
// against a specific (actor, target) pair, then returns the count of CBs the
// engine considers valid. Used by do_pre_validate_war (A5) so the bridge can
// reject NO_VIABLE_PAIR triples before queueing a WAR_DECLARE intent.
//
// Slot states match query_valid_cbs: 0=empty, 1=pending, 2=done, 3=error,
// 4=in-progress. We tolerate the slot being in 2/3 (stale completed query)
// by clearing it before submitting; we wait briefly if it is 1/4 (busy).

// =============================================================================
// Shared slot acquisition with bounded retry-on-busy + stale reclaim (ck-#95)
// =============================================================================
//
// The g_cb_query slot is SINGLE-FLIGHT global state shared between the agent's
// CB-validity queries AND the engine's own AI CB evaluations (both drain through
// process_cb_query on the game thread). When the deferred-drain coordinator
// pulses the game unpaused to service an agent query, the engine's concurrent AI
// may grab the slot first → the agent query sees state 1/4 and used to fail-closed
// with cb_query_busy, which the live agent treats as "degraded" → it HOLDS and
// misses expansion (the #95 class). A SECOND failure mode: a prior query that
// timed out (rc=-3) used to leave the slot wedged at state 1 forever (the engine
// never serviced it because it was re-paused), so EVERY subsequent caller's
// busy-wait drained for nothing and returned cb_query_busy — a stuck-busy slot.
//
// This helper resolves both: it polls for a clean slot (0/2/3) across a bounded
// budget of retry attempts, and — critically — RECLAIMS a slot stuck at state 1
// (pending, never picked up by the game thread) once its submit timestamp is
// older than a generous staleness window. State 4 (game thread actively in
// CB_VALIDITY_CHECK) is normally never reclaimed — that would race a live
// writer; we wait it out, which is bounded by the engine's own per-query
// service time.
//
// D8 EXCEPTION (probe P4.7 item 4, 2026-08-01): a state-4 slot whose
// service_start_ns is older than kServiceStaleNs IS reclaimed. If the game
// thread dies inside CB_VALIDITY_CHECK (engine-side SIGSEGV absorbed by CK3's
// own crash handler — the 07-31/08-01 minidump class), the slot would
// otherwise stay 4 forever and every later CB query would return
// cb_query_busy for the rest of the session (a genuine permanent wedge).
// Legitimate service completes in ~0.5-4s (per-CB watchdog bounds hangs), so
// a 30s-old state-4 means the servicing thread is gone. The CAS-based reclaim
// keeps the (vanishingly unlikely) still-alive-but-slow writer race window
// explicit and logged.
//
// Returns true and leaves the slot reset to 0 (empty, ready to submit) on
// success. Returns false (caller emits cb_query_busy) only when the slot stayed
// genuinely busy — actively serviced (state 4) or freshly pending (state 1 with
// a recent submit) — across the whole budget.
//
// Crash-hardened: pure atomic loads + at most a results.clear() on a slot we
// have exclusively reclaimed. No name-cache drain, no game-object traversal, no
// allocation under the AI hook. Polled budgets only (no signal watchdog — the
// reader thread masks SIGALRM; the I15 lesson).
static bool cb_query_acquire_slot() {
    // Per-attempt drain window and overall retry budget. ~3 attempts of 500ms
    // each = ~1.5s overall worst case before declaring busy, comfortably inside
    // the do_query_*_strict timeout (>= 2000ms) so a transient engine-AI
    // collision is ridden out rather than surfaced as a hard failure.
    constexpr int kAttempts        = 3;
    constexpr int kBusyWaitMs      = 500;
    constexpr int kPollIntervalMs  = 25;
    // A state-1 slot whose submit is older than this was abandoned (the game
    // thread never CAS'd it 1→4 — e.g. a prior caller timed out while paused).
    // Safe to reclaim: no game thread owns a never-serviced pending slot.
    constexpr uint64_t kStaleNs    = 8ULL * 1000 * 1000 * 1000;  // 8s
    // D8: a state-4 slot whose service START is older than this has a dead
    // servicing thread (crashed inside CB_VALIDITY_CHECK) — reclaim so one
    // crash cannot wedge query_cbs_for_target for the whole session.
    constexpr uint64_t kServiceStaleNs = 30ULL * 1000 * 1000 * 1000;  // 30s

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        int busy_waited = 0;
        while (busy_waited < kBusyWaitMs) {
            int st = g_cb_query.state.load(std::memory_order_acquire);
            if (st == 0 || st == 2 || st == 3) {
                // Clear any stale completed result, then claim empty.
                if (st == 2 || st == 3) {
                    g_cb_query.results.clear();
                    g_cb_query.state.store(0, std::memory_order_release);
                }
                return true;
            }
            // Stuck-busy reclaim: a long-pending (state 1) slot that the game
            // thread never picked up is abandoned — take it over. Re-check the
            // state under the staleness test to avoid racing a 1→4 transition.
            if (st == 1) {
                const uint64_t now_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const uint64_t submit_ns =
                    g_cb_query.submit_ns.load(std::memory_order_acquire);
                if (submit_ns != 0 && now_ns > submit_ns &&
                    (now_ns - submit_ns) > kStaleNs) {
                    int expected = 1;
                    // CAS 1→0: only succeeds if still pending (not yet 1→4'd by
                    // the game thread). On success we own a clean empty slot.
                    if (g_cb_query.state.compare_exchange_strong(
                            expected, 0, std::memory_order_acq_rel)) {
                        g_cb_query.results.clear();
                        if (g_logfile) fprintf(g_logfile,
                            "[CK3-Bridge] cb_query: reclaimed stale pending slot "
                            "(submit age %llu ms)\n",
                            (unsigned long long)((now_ns - submit_ns) / 1000000ULL));
                        return true;
                    }
                    // Lost the CAS: game thread just grabbed it (now state 4).
                    // Fall through and wait it out.
                }
            }
            // D8 self-reset (P4.7 item 4): bounded STATE-4 reclaim. A service
            // older than kServiceStaleNs means the servicing game thread died
            // mid-CB_VALIDITY_CHECK (engine-side SIGSEGV absorbed by CK3's own
            // crash handler never stores state 2/3) — without this, the slot
            // wedges at 4 and EVERY subsequent CB query in the session returns
            // cb_query_busy.
            if (st == 4) {
                const uint64_t now_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const uint64_t svc_ns =
                    g_cb_query.service_start_ns.load(std::memory_order_acquire);
                if (svc_ns != 0 && now_ns > svc_ns &&
                    (now_ns - svc_ns) > kServiceStaleNs) {
                    int expected = 4;
                    if (g_cb_query.state.compare_exchange_strong(
                            expected, 0, std::memory_order_acq_rel)) {
                        g_cb_query.results.clear();
                        if (g_logfile) fprintf(g_logfile,
                            "[CK3-Bridge] cb_query: reclaimed DEAD in-service slot "
                            "(service age %llu ms) — servicing thread presumed "
                            "crashed (D8 self-reset)\n",
                            (unsigned long long)((now_ns - svc_ns) / 1000000ULL));
                        return true;
                    }
                    // Lost the CAS: the service just completed (state 2/3).
                    // Loop around and claim it via the clean-slot path.
                }
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kPollIntervalMs));
            busy_waited += kPollIntervalMs;
        }
        // This attempt's window expired still busy. Re-submit/retry: loop back
        // for another drain window (bounded by kAttempts) before giving up.
    }
    return false;  // genuinely busy across the whole budget → caller emits busy
}

int cb_query_count_valid_sync(uint32_t actor_id,
                              uint32_t target_id,
                              int      timeout_ms,
                              int*     out_total,
                              char*    out_error,
                              size_t   error_size) {
    auto set_err = [&](const char* tag) {
        if (out_error && error_size > 0) {
            strncpy(out_error, tag, error_size - 1);
            out_error[error_size - 1] = '\0';
        }
    };
    if (out_total) *out_total = 0;
    if (out_error && error_size > 0) out_error[0] = '\0';

    // Acquire the single-flight slot with bounded retry-on-busy + stale reclaim
    // (ck-#95). Rides out transient collisions with the engine's own AI CB eval
    // and reclaims a slot wedged-pending by a prior timed-out query.
    if (!cb_query_acquire_slot()) {
        g_cb_query_busy_count.fetch_add(1, std::memory_order_relaxed);  // H3
        set_err("cb_query_busy");
        return -1;
    }

    // Submit our request.
    g_cb_query.actor_id  = actor_id;
    g_cb_query.target_id = target_id;
    g_cb_query.strict_mode = false;
    g_cb_query.results.clear();
    g_cb_query.error[0] = '\0';
    g_cb_query.service_start_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_end_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_tick.store(0, std::memory_order_relaxed);

    const uint64_t submit_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    g_cb_query.submit_ns.store(submit_ns, std::memory_order_relaxed);
    g_cb_query.submit_tick.store(g_tick_id.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    g_cb_query.state.store(1, std::memory_order_release);

    // Poll for game-thread completion. Same 50ms cadence as do_query_valid_cbs.
    if (timeout_ms < 50) timeout_ms = 50;
    int waited = 0;
    while (waited < timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
        int s = g_cb_query.state.load(std::memory_order_acquire);
        if (s == 2 || s == 3) {
            if (s == 3) {
                set_err(g_cb_query.error[0] ? g_cb_query.error : "cb_query_error");
                // Clear the slot for the next caller.
                g_cb_query.results.clear();
                g_cb_query.state.store(0, std::memory_order_release);
                return -2;
            }
            int valid_count = 0;
            int total = static_cast<int>(g_cb_query.results.size());
            for (const auto& cb : g_cb_query.results) {
                if (cb.valid) valid_count++;
            }
            if (out_total) *out_total = total;
            // Clear the slot for the next caller. The result is already
            // captured into local counts above.
            g_cb_query.results.clear();
            g_cb_query.state.store(0, std::memory_order_release);
            return valid_count;
        }
    }

    // Timeout. Release the slot so the next caller is not blocked behind a
    // wedged pending request (the stuck-busy failure mode). If the slot is
    // still pending (state 1, game thread never picked it up — e.g. it was
    // re-paused under us), reclaim it via CAS 1→0. If the game thread is
    // actively servicing (state 4) we leave it; cb_query_acquire_slot's stale
    // reclaim will recover the slot for the next caller if it never completes.
    {
        int expected = 1;
        if (g_cb_query.state.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            g_cb_query.results.clear();
        }
    }
    set_err("cb_query_timeout");
    return -3;
}

// =============================================================================
// Strict CB validity probe — Stage 1+2 mirror of AI_CB_EVALUATE_ALL (ck-39e1)
// =============================================================================
//
// Mirrors the outer-filter pipeline of AI_CB_EVALUATE_ALL so the bridge's
// candidate set tracks what the engine accepts at scope. Two layers:
//
//   Stage 1 — bilateral pre-filter (this function, IPC thread).
//             Decomp lines 197-198 of AI_CB_EVALUATE_ALL: target must be
//             a ruler (char+0x1B8 != 0) AND alive (char+0x1C8 == 0). Self-
//             pair check is implicit in the engine's front-buffer iterator.
//
//             Deferred to follow-up RE: faith/realm/government/era cross-
//             checks (FUN_067539c0/0674adf0/066e4fb0) and truce/alliance
//             flag construction (FUN_0678b570/GetTruceList) — see Open
//             Questions in re/findings/re_a1_path_a_re_20260425.md.
//
//   Stage 2 — group-level outer-trigger flag filter (process_cb_query,
//             game thread, when CBQuery::strict_mode=true). Decomp lines
//             357-360: per-group requires DISABLED_FLAG==0 AND
//             STAGE2_FLAG==0 AND members count > 0. Script trigger at
//             group+0x48 deferred (needs scope_ctx; out of scope per bead).
//
// Stage 4 (score evaluator) is explicitly out of scope (per bead — may be
// obviated by Stage 1+2 alone, otherwise pivots to Path B').
//
// On bilateral reject the function returns -4 with `*out_total = 0` and
// a short reason in `*out_error` (caller treats as fail-closed).

int cb_query_count_valid_strict(uint32_t actor_id,
                                uint32_t target_id,
                                int      timeout_ms,
                                int*     out_total,
                                char*    out_error,
                                size_t   error_size) {
    auto set_err = [&](const char* tag) {
        if (out_error && error_size > 0) {
            strncpy(out_error, tag, error_size - 1);
            out_error[error_size - 1] = '\0';
        }
    };
    if (out_total) *out_total = 0;
    if (out_error && error_size > 0) out_error[0] = '\0';

    // Stage 1 bilateral pre-filter — runs on IPC thread via direct memory
    // reads. The engine's IsRuler/IsAlive helpers are pure pointer-field
    // checks (re/offsets/ck3_offsets.h: character::TITLE_REALM_DATA,
    // DEATH_DATE), so we can mirror them without dispatching to the game
    // thread. This avoids round-tripping through process_cb_query for
    // pairs the engine would reject up front.
    if (actor_id == target_id ||
        (actor_id & 0xFFFFFF) == (target_id & 0xFFFFFF)) {
        set_err("bilateral_self_pair");
        return -4;
    }

    void* attacker = lookup_character(actor_id);
    if (!attacker) {
        set_err("bilateral_attacker_not_found");
        return -4;
    }
    void* defender = lookup_character(target_id);
    if (!defender) {
        set_err("bilateral_target_not_found");
        return -4;
    }

    uintptr_t target_addr = reinterpret_cast<uintptr_t>(defender);
    uintptr_t actor_addr  = reinterpret_cast<uintptr_t>(attacker);

    // IsRuler(target): TITLE_REALM_DATA != 0
    uintptr_t target_title_realm =
        safe_read<uintptr_t>(target_addr + ck3::character::TITLE_REALM_DATA);
    if (target_title_realm == 0) {
        set_err("bilateral_target_not_ruler");
        return -4;
    }
    // IsAlive(target): DEATH_DATE == 0
    uintptr_t target_death =
        safe_read<uintptr_t>(target_addr + ck3::character::DEATH_DATE);
    if (target_death != 0) {
        set_err("bilateral_target_dead");
        return -4;
    }
    // The engine pre-validates the actor before invoking AI_CB_EVALUATE_ALL,
    // but mirror the same checks defensively — keeps bridge self-contained
    // for callers (e.g. discovery scans) that bypass A1/A2.
    uintptr_t actor_title_realm =
        safe_read<uintptr_t>(actor_addr + ck3::character::TITLE_REALM_DATA);
    if (actor_title_realm == 0) {
        set_err("bilateral_actor_not_ruler");
        return -4;
    }
    uintptr_t actor_death =
        safe_read<uintptr_t>(actor_addr + ck3::character::DEATH_DATE);
    if (actor_death != 0) {
        set_err("bilateral_actor_dead");
        return -4;
    }

    // Acquire the single-flight slot with bounded retry-on-busy + stale reclaim
    // (ck-#95). Same shape as cb_query_count_valid_sync.
    if (!cb_query_acquire_slot()) {
        g_cb_query_busy_count.fetch_add(1, std::memory_order_relaxed);  // H3
        set_err("cb_query_busy");
        return -1;
    }

    // Submit our request with strict_mode=true. The game thread reads the
    // flag in process_cb_query and applies the Stage 2 group filter.
    g_cb_query.actor_id  = actor_id;
    g_cb_query.target_id = target_id;
    g_cb_query.strict_mode = true;
    g_cb_query.results.clear();
    g_cb_query.error[0] = '\0';
    g_cb_query.service_start_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_end_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_tick.store(0, std::memory_order_relaxed);

    const uint64_t submit_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    g_cb_query.submit_ns.store(submit_ns, std::memory_order_relaxed);
    g_cb_query.submit_tick.store(g_tick_id.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    g_cb_query.state.store(1, std::memory_order_release);

    if (timeout_ms < 50) timeout_ms = 50;
    int waited = 0;
    while (waited < timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
        int s = g_cb_query.state.load(std::memory_order_acquire);
        if (s == 2 || s == 3) {
            if (s == 3) {
                set_err(g_cb_query.error[0] ? g_cb_query.error : "cb_query_error");
                g_cb_query.results.clear();
                g_cb_query.state.store(0, std::memory_order_release);
                return -2;
            }
            int valid_count = 0;
            int total = static_cast<int>(g_cb_query.results.size());
            for (const auto& cb : g_cb_query.results) {
                if (cb.valid) valid_count++;
            }
            if (out_total) *out_total = total;
            g_cb_query.results.clear();
            g_cb_query.state.store(0, std::memory_order_release);
            return valid_count;
        }
    }

    // Timeout — release a still-pending slot so it does not wedge (ck-#95).
    {
        int expected = 1;
        if (g_cb_query.state.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            g_cb_query.results.clear();
        }
    }
    set_err("cb_query_timeout");
    return -3;
}

// =============================================================================
// Strict CB validity probe with per-CB result surface (ck-5qpg)
// =============================================================================
//
// Same Stage 1 + Stage 2 mirror as cb_query_count_valid_strict, but copies
// the full per-CB result vector (with outer_status/outer_reason/inner_status
// fields populated by process_cb_query) out to the IPC-thread caller alongside
// the aggregate summary counts. Used by do_query_cb_validity_strict to fill
// the spec's wire format with per-CB reason strings.
//
// Per-CB outer triggers (allowed_for_character / allowed_against_character /
// valid_to_start) are NOT yet evaluated — those need a CB-rooted scope_ctx
// and are deferred to ck-39e1. When ck-39e1 lands, process_cb_query will
// set additional outer_reason values ("allowed_for_character_failed", etc.)
// and the wire format will surface them automatically.
int cb_query_strict_full_sync(uint32_t actor_id,
                              uint32_t target_id,
                              int      timeout_ms,
                              CBStrictSummary*           out_summary,
                              std::vector<CBQueryResult>* out_results,
                              char*    out_error,
                              size_t   error_size) {
    auto set_err = [&](const char* tag) {
        if (out_error && error_size > 0) {
            strncpy(out_error, tag, error_size - 1);
            out_error[error_size - 1] = '\0';
        }
    };
    if (out_summary) {
        out_summary->total_cbs            = 0;
        out_summary->outer_filter_dropped = 0;
        out_summary->candidates_post_outer= 0;
        out_summary->inner_valid          = 0;
        out_summary->inner_invalid        = 0;
        out_summary->enabled_count        = 0;
        out_summary->disabled_count       = 0;
        out_summary->disabled_cb_names.clear();
    }
    if (out_results) out_results->clear();
    if (out_error && error_size > 0) out_error[0] = '\0';

    // Stage 1 bilateral pre-filter — same as cb_query_count_valid_strict.
    if (actor_id == target_id ||
        (actor_id & 0xFFFFFF) == (target_id & 0xFFFFFF)) {
        set_err("bilateral_self_pair");
        return -4;
    }
    void* attacker = lookup_character(actor_id);
    if (!attacker) { set_err("bilateral_attacker_not_found"); return -4; }
    void* defender = lookup_character(target_id);
    if (!defender) { set_err("bilateral_target_not_found"); return -4; }

    uintptr_t target_addr = reinterpret_cast<uintptr_t>(defender);
    uintptr_t actor_addr  = reinterpret_cast<uintptr_t>(attacker);
    uintptr_t target_title_realm =
        safe_read<uintptr_t>(target_addr + ck3::character::TITLE_REALM_DATA);
    if (target_title_realm == 0) { set_err("bilateral_target_not_ruler"); return -4; }
    uintptr_t target_death =
        safe_read<uintptr_t>(target_addr + ck3::character::DEATH_DATE);
    if (target_death != 0) { set_err("bilateral_target_dead"); return -4; }
    uintptr_t actor_title_realm =
        safe_read<uintptr_t>(actor_addr + ck3::character::TITLE_REALM_DATA);
    if (actor_title_realm == 0) { set_err("bilateral_actor_not_ruler"); return -4; }
    uintptr_t actor_death =
        safe_read<uintptr_t>(actor_addr + ck3::character::DEATH_DATE);
    if (actor_death != 0) { set_err("bilateral_actor_dead"); return -4; }

    // Acquire the single-flight slot with bounded retry-on-busy + stale reclaim
    // (ck-#95).
    if (!cb_query_acquire_slot()) {
        g_cb_query_busy_count.fetch_add(1, std::memory_order_relaxed);  // H3
        set_err("cb_query_busy");
        return -1;
    }

    g_cb_query.actor_id  = actor_id;
    g_cb_query.target_id = target_id;
    g_cb_query.strict_mode = true;
    g_cb_query.results.clear();
    g_cb_query.error[0] = '\0';
    g_cb_query.service_start_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_end_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_tick.store(0, std::memory_order_relaxed);

    const uint64_t submit_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    g_cb_query.submit_ns.store(submit_ns, std::memory_order_relaxed);
    g_cb_query.submit_tick.store(g_tick_id.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    g_cb_query.state.store(1, std::memory_order_release);

    if (timeout_ms < 50) timeout_ms = 50;
    int waited = 0;
    while (waited < timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
        int s = g_cb_query.state.load(std::memory_order_acquire);
        if (s == 2 || s == 3) {
            if (s == 3) {
                set_err(g_cb_query.error[0] ? g_cb_query.error : "cb_query_error");
                g_cb_query.results.clear();
                g_cb_query.state.store(0, std::memory_order_release);
                return -2;
            }
            // Aggregate summary + copy results out.
            int outer_dropped = 0;
            int candidates_post_outer = 0;
            int inner_valid = 0;
            int inner_invalid = 0;
            for (const auto& cb : g_cb_query.results) {
                if (strcmp(cb.outer_status, "dropped") == 0) {
                    outer_dropped++;
                } else {
                    candidates_post_outer++;
                    if (cb.valid) inner_valid++;
                    else inner_invalid++;
                }
            }
            if (out_summary) {
                out_summary->total_cbs            = static_cast<int>(g_cb_query.results.size());
                out_summary->outer_filter_dropped = outer_dropped;
                out_summary->candidates_post_outer= candidates_post_outer;
                out_summary->inner_valid          = inner_valid;
                out_summary->inner_invalid        = inner_invalid;
                out_summary->enabled_count        = g_cb_query.enabled_count;
                out_summary->disabled_count       = g_cb_query.disabled_count;
                out_summary->disabled_cb_names    = g_cb_query.disabled_cb_names;
                out_summary->option_capacity_hits = g_cb_query.option_capacity_hits;  // D8 guard
            }
            if (out_results) {
                *out_results = g_cb_query.results;  // copy
            }
            g_cb_query.results.clear();
            g_cb_query.state.store(0, std::memory_order_release);
            return inner_valid;
        }
    }

    // Timeout — release a still-pending slot so it does not wedge (ck-#95).
    {
        int expected = 1;
        if (g_cb_query.state.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            g_cb_query.results.clear();
        }
    }
    set_err("cb_query_timeout");
    return -3;
}

// =============================================================================
// Loud-failure key extraction (ck-wtk7 Strategy C)
// =============================================================================
//
// For a FAILING trigger slot, re-evaluate via the loud populator
// (FUN_0763ddc0 / trigger_eval_funcs::EVAL_WITH_FAILURES) on a
// bridge-owned 0xD8 record slot. The loud path writes a failure subtree
// into the record whose std::string fields at +0x18 / +0x38 carry
// localization keys (e.g. CHARACTER_HAS_TRUCE) when the sub-condition
// matches a script-data predicate.
//
// Per re/findings/re_gate4_rejection_keys.md §9 Strategy C the strings
// may or may not be populated without the final emit pass
// (FUN_0763dfe0). If extraction yields a SCREAMING_SNAKE_CASE identifier
// of ≥5 chars, we surface it; otherwise we return empty and the caller
// falls back to the slot-derived TRIGGER_FAILED__* key.
//
// Entire call sequence is sigsetjmp-guarded so any misalignment produces
// an empty result rather than a crash.

static bool is_snake_upper_key(const char* s, size_t n) {
    if (n < 5) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

// Read a libstdc++ std::string at addr into out (up to out_size-1 chars,
// NUL-terminated). Returns number of chars written (0 on any error).
static size_t safe_read_std_string(uintptr_t addr, char* out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    uintptr_t data_ptr = 0;
    uint64_t len = 0;   // game_string_view: the string ABI of the game's build
    if (!game_string_view(addr, &data_ptr, &len) || len > 1024) return 0;
    if (!is_mapped(data_ptr)) return 0;
    size_t n = (len < out_size - 1) ? static_cast<size_t>(len) : out_size - 1;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = safe_read<uint8_t>(data_ptr + i);
        if (b == 0) { n = i; break; }
        out[i] = static_cast<char>(b);
    }
    out[n] = '\0';
    return n;
}

// Populate `out_key` with a loc-key harvested from the loud-failure record
// for (trigger_block, scope_ctx). Returns true on any non-empty key; false
// otherwise. Caller owns `out_key` buffer (≥96 bytes recommended).
static bool loud_extract_slot_key(uintptr_t trigger_block,
                                  uintptr_t scope_ctx,
                                  char* out_key, size_t out_size) {
    if (!out_key || out_size == 0) return false;
    out_key[0] = '\0';

    typedef void (*fn_eval_with_failures)(void**, uintptr_t, uintptr_t);
    typedef void (*fn_diag_dtor)(void*);
    auto fn_populate = reinterpret_cast<fn_eval_with_failures>(
        g_base_addr + ck3::trigger_eval_funcs::EVAL_WITH_FAILURES);
    auto fn_destroy = reinterpret_cast<fn_diag_dtor>(
        g_base_addr + ck3::trigger_eval_funcs::DIAG_OBJECT_DTOR);
    if (!is_mapped(reinterpret_cast<uintptr_t>(fn_populate)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(fn_destroy))) {
        return false;
    }

    void* record = nullptr;    // populator allocates via operator new(0xD8)
    char buf1[96] = {};
    char buf2[96] = {};
    size_t n1 = 0;
    size_t n2 = 0;

    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        ADLOG("LOUD_EXTRACT: signal %d during populate/read — key not extracted\n", sig);
        return false;
    }

    try {
        fn_populate(&record, trigger_block, scope_ctx);
        if (record && is_mapped(reinterpret_cast<uintptr_t>(record))) {
            uintptr_t rec = reinterpret_cast<uintptr_t>(record);
            n1 = safe_read_std_string(rec + ck3::diag_record::STR1_DATA,
                                      buf1, sizeof(buf1));
            n2 = safe_read_std_string(rec + ck3::diag_record::STR2_DATA,
                                      buf2, sizeof(buf2));
        }
    } catch (...) {
        n1 = n2 = 0;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    // Separate protected region for destruction — we want to free the
    // record even if reading strings crashed above.
    if (record) {
        watchdog_arm(3);
        g_in_protected_region = true;
        sig = sigsetjmp(g_recovery_point, 1);
        if (sig == 0) {
            try {
                fn_destroy(record);   // FUN_05a5b060: destroy members
                operator delete(record); // _ZdlPv: free the 0xD8 allocation
            } catch (...) { /* leak over crash */ }
        } else {
            ADLOG("LOUD_EXTRACT: signal %d during destroy (record leaked)\n", sig);
        }
        g_in_protected_region = false;
        watchdog_disarm();
    }

    const char* pick = nullptr;
    size_t pick_n = 0;
    if (is_snake_upper_key(buf1, n1)) { pick = buf1; pick_n = n1; }
    else if (is_snake_upper_key(buf2, n2)) { pick = buf2; pick_n = n2; }

    if (!pick) {
        ADLOG("LOUD_EXTRACT: no SCREAMING_SNAKE_CASE key extracted "
              "(str1=%zu '%.*s', str2=%zu '%.*s')\n",
              n1, (int)std::min(n1, (size_t)40), buf1,
              n2, (int)std::min(n2, (size_t)40), buf2);
        return false;
    }

    size_t copy_n = (pick_n < out_size - 1) ? pick_n : out_size - 1;
    memcpy(out_key, pick, copy_n);
    out_key[copy_n] = '\0';
    ADLOG("LOUD_EXTRACT: extracted key '%s'\n", out_key);
    return true;
}

// =============================================================================
// Deferred Trigger Evaluation Query — evaluate on game thread
// =============================================================================
//
// Called from drain_and_execute_intents() on the game thread. Constructs a
// proper interaction scope context via the TLS pipeline (CTX_INIT/CTX_RESET/
// SET_TARGET), then calls the universal trigger evaluator (FUN_07628500) on
// each requested trigger slot. Returns per-slot pass/fail results.

// Trigger slot definitions: offset within CCharacterInteractionType
static const struct {
    int         slot;
    const char* name;
    uint32_t    offset;
} g_trigger_slots[] = {
    // The offsets by their header names (the Linux values unchanged; the Windows
    // build's header carries the measured +0x18 band). The slot names are the
    // reply's vocabulary, older than the ck-angk renames beside them.
    { 0, "is_valid",                       ck3::interaction_def::IS_SHOWN_TRIGGER },        // 0x0BC0
    { 1, "is_shown",                       ck3::interaction_def::IS_VALID_TRIGGER },        // 0x0D80
    { 2, "is_valid_showing_failures_only", ck3::interaction_def::HAS_VALID_TARGET },        // 0x0F40
    { 3, "can_be_sent_despite_rejection",  ck3::interaction_def::CAN_SEND_FINAL_TRIGGER },  // 0x12C0
    { 4, "can_send",                       ck3::interaction_def::CAN_BE_PICKED_TRIGGER },   // 0x1100
    { 5, "auto_accept",                    ck3::interaction_def::IS_VALID_TRIGGER_AI },     // 0x0E60
    { 6, "on_auto_accept",                 ck3::interaction_def::HAS_VALID_TARGET_AI },     // 0x1020
};
static constexpr int NUM_TRIGGER_SLOTS = 7;

void process_trigger_query() {
    int expected = 1;  // pending
    if (!g_trigger_query.state.compare_exchange_strong(expected, 4,
            std::memory_order_acq_rel)) {
        return;  // not pending
    }

    volatile uint32_t actor_id = g_trigger_query.actor_id;
    volatile uint32_t target_id = g_trigger_query.target_id;
    const char* volatile interaction_type = g_trigger_query.interaction_type;
    volatile int requested_slot = g_trigger_query.trigger_slot;

    ADLOG("TRIGGER_QUERY: evaluating %s actor=%u target=%u slot=%d\n",
          interaction_type, actor_id & 0xFFFFFF, target_id & 0xFFFFFF, requested_slot);

    // Validate requested slot
    if (requested_slot < -1 || requested_slot >= NUM_TRIGGER_SLOTS) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                 "invalid_trigger_slot_%d", requested_slot);
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    // Resolve character and interaction
    void* character = lookup_character(actor_id);
    if (!character) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error), "actor_not_found");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        ADLOG("TRIGGER_QUERY: actor %u not found\n", actor_id & 0xFFFFFF);
        return;
    }

    // SAME GAP AS THE ACCEPTANCE QUERY, FOUND BY SWEEPING THE FAMILY RATHER
    // THAN FIXING THE ONE THAT CRASHED. This path also checked the actor and
    // the interaction and handed the TARGET to the engine unresolved. The arm-A
    // crash came through query_interaction_acceptance; query_trigger_result
    // reaches the same dereference by another door, and fixing only the door
    // that was used would have left the crash live.
    void* trigger_target = lookup_character(target_id);
    if (target_id != 0 && !trigger_target) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error), "target_not_found");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        ADLOG("TRIGGER_QUERY: target %u not found — REFUSING rather than "
              "handing an unresolved id to the engine\n", target_id & 0xFFFFFF);
        return;
    }

    void* interaction = lookup_interaction(interaction_type);
    if (!interaction) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error), "interaction_not_found");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        ADLOG("TRIGGER_QUERY: interaction '%s' not found\n", interaction_type);
        return;
    }

    // --- Set up TLS context for scope construction ---
    uint8_t* guard_byte = nullptr;
    void* tls_ctx = nullptr;
    if (const char* tls_err = locate_game_tls(&guard_byte, &tls_ctx)) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error), "%s", tls_err);
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    // ck-62eo: same guard, same helpers. This copy was missed by my first pass
    // and caught by the acceptance counting dereferences.
    const TlsGuard tq_guard = read_tls_guard(guard_byte);
    if (tq_guard == TlsGuard::Unreadable) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                 "tls_guard_unreadable");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    // Ensure TLS context is constructed
    if (tq_guard == TlsGuard::NeedsConstruct) {
        auto tls_construct = reinterpret_cast<fn_tls_construct>(
            bridge::game_fn(ck3::funcs::INTERACTION_TLS_CONSTRUCT));
        if (!is_mapped(reinterpret_cast<uintptr_t>(tls_construct))) {
            snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                     "tls_construct_not_mapped");
            g_trigger_query.result_count = 0;
            g_trigger_query.state.store(3, std::memory_order_release);
            return;
        }

        watchdog_arm(3);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                     "tls_construct_crash");
            g_trigger_query.result_count = 0;
            g_trigger_query.state.store(3, std::memory_order_release);
            return;
        }
        try {
            tls_construct(tls_ctx);
        } catch (...) {
            g_in_protected_region = false;
            watchdog_disarm();
            snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                     "tls_construct_exception");
            g_trigger_query.result_count = 0;
            g_trigger_query.state.store(3, std::memory_order_release);
            return;
        }
        g_in_protected_region = false;
        watchdog_disarm();

        auto tls_dtor = reinterpret_cast<fn_tls_destructor>(
            bridge::game_fn(ck3::funcs::INTERACTION_TLS_DESTRUCTOR));
        register_game_tls_dtor(reinterpret_cast<void(*)(void*)>(tls_dtor), tls_ctx);
        if (!set_tls_guard(guard_byte)) {
            snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                     "tls_guard_unwritable");
            g_trigger_query.result_count = 0;
            g_trigger_query.state.store(3, std::memory_order_release);
            return;
        }
        ADLOG("TRIGGER_QUERY: TLS context constructed\n");
    }

    // Initialize interaction context: CTX_INIT → CTX_RESET → SET_TARGET
    auto ctx_init = reinterpret_cast<fn_ctx_init>(
        bridge::game_fn(ck3::funcs::INTERACTION_CTX_INIT));
    auto ctx_reset = reinterpret_cast<fn_ctx_reset>(
        bridge::game_fn(ck3::funcs::INTERACTION_CTX_RESET));
    auto ctx_set_target = reinterpret_cast<fn_ctx_set_target>(
        bridge::game_fn(ck3::funcs::INTERACTION_CTX_SET_TARGET));

    if (!is_mapped(reinterpret_cast<uintptr_t>(ctx_init)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_reset)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_set_target))) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                 "pipeline_funcs_not_mapped");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    // Build scope context under crash protection
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                 "scope_setup_%s", sig_name);
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    try {
        ctx_init(reinterpret_cast<uintptr_t*>(tls_ctx), interaction, character);
        ctx_reset(reinterpret_cast<uintptr_t*>(tls_ctx));
        ctx_set_target(reinterpret_cast<uintptr_t*>(tls_ctx), target_id, 1);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                 "scope_setup_exception");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    ADLOG("TRIGGER_QUERY: scope context built (CTX_INIT+RESET+SET_TARGET)\n");

    // Read the interaction definition pointer from the context
    uintptr_t ctx_addr = reinterpret_cast<uintptr_t>(tls_ctx);
    uintptr_t interaction_def = safe_read<uintptr_t>(ctx_addr + ck3::interaction_ctx::INTERACTION_TYPE_PTR);
    uintptr_t scope_ctx = ctx_addr + ck3::interaction_ctx::SCOPE_LIST;

    if (!interaction_def || !is_mapped(interaction_def)) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                 "interaction_def_null");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        ADLOG("TRIGGER_QUERY: interaction_def is null/unmapped after CTX_INIT\n");
        return;
    }

    ADLOG("TRIGGER_QUERY: interaction_def=0x%lx scope_ctx=0x%lx\n",
          interaction_def, scope_ctx);

    // Resolve trigger evaluator function
    auto trigger_eval = reinterpret_cast<bool(*)(uintptr_t, uintptr_t)>(
        g_base_addr + ck3::trigger_eval_funcs::SCRIPT_TRIGGER_EVALUATOR);
    if (!is_mapped(reinterpret_cast<uintptr_t>(trigger_eval))) {
        snprintf(g_trigger_query.error, sizeof(g_trigger_query.error),
                 "trigger_eval_not_mapped");
        g_trigger_query.result_count = 0;
        g_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    // Evaluate requested trigger slot(s)
    volatile int start_slot = (requested_slot == -1) ? 0 : requested_slot;
    volatile int end_slot = (requested_slot == -1) ? NUM_TRIGGER_SLOTS : requested_slot + 1;
    volatile int result_idx = 0;

    volatile bool extract_loc_keys = g_trigger_query.extract_loc_keys;

    for (volatile int s = start_slot; s < end_slot; s++) {
        TriggerSlotResult& res = g_trigger_query.results[result_idx];
        res.slot = g_trigger_slots[s].slot;
        strncpy(res.name, g_trigger_slots[s].name, sizeof(res.name) - 1);
        res.name[sizeof(res.name) - 1] = '\0';
        res.offset = g_trigger_slots[s].offset;
        res.loc_key[0] = '\0';

        // Check if trigger block exists (first qword non-null means trigger is defined)
        uintptr_t trigger_block = interaction_def + g_trigger_slots[s].offset;
        uintptr_t trigger_head = safe_read<uintptr_t>(trigger_block);

        if (trigger_head == 0) {
            res.present = false;
            res.result = true;  // absent trigger = auto-pass
            ADLOG("TRIGGER_QUERY: slot %d (%s) at +0x%x: NOT PRESENT (auto-pass)\n",
                  s, g_trigger_slots[s].name, g_trigger_slots[s].offset);
            result_idx++;
            continue;
        }

        res.present = true;

        // Evaluate under crash protection
        watchdog_arm(3);
        g_in_protected_region = true;
        sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            res.result = false;
            ADLOG("TRIGGER_QUERY: slot %d (%s) CRASHED during evaluation\n",
                  s, g_trigger_slots[s].name);
            result_idx++;
            continue;
        }

        bool eval_result = false;
        try {
            eval_result = trigger_eval(trigger_block, scope_ctx);
        } catch (...) {
            eval_result = false;
            ADLOG("TRIGGER_QUERY: slot %d (%s) threw exception\n",
                  s, g_trigger_slots[s].name);
        }
        g_in_protected_region = false;
        watchdog_disarm();

        res.result = eval_result;
        ADLOG("TRIGGER_QUERY: slot %d (%s) at +0x%x: %s\n",
              s, g_trigger_slots[s].name, g_trigger_slots[s].offset,
              eval_result ? "PASS" : "FAIL");

        // ck-wtk7: opt-in loc-key extraction on failing slots via the loud
        // populator. Runs self-contained (own sigjmp guard); on any failure
        // res.loc_key stays empty and caller falls back to slot-name key.
        if (extract_loc_keys && !eval_result) {
            loud_extract_slot_key(trigger_block, scope_ctx,
                                  res.loc_key, sizeof(res.loc_key));
        }

        result_idx++;
    }

    g_trigger_query.result_count = result_idx;
    g_trigger_query.error[0] = '\0';
    g_trigger_query.state.store(2, std::memory_order_release);  // done

    ADLOG("TRIGGER_QUERY: done — %d slots evaluated for %s actor=%u target=%u\n",
          result_idx, interaction_type, actor_id & 0xFFFFFF, target_id & 0xFFFFFF);
}

// =============================================================================
// Console Command Handler Table + Execution
// =============================================================================
//
// Console handlers have ABI: SResult* handler(SResult* out, const SCommandData* data)
// where SResult is returned by value via hidden pointer (Itanium C++ ABI).
//
// SCommandData layout (from decompilation of FUN_05c7e6d0):
//   +0x00: ptr to arg array (each entry 0x20 bytes: +0x00=char*, +0x08=len)
//   +0x0C: int32 arg_count
//
// SResult layout (from decompilation of FUN_041f3db0):
//   +0x00: uint8 success (0=fail, 1=ok)
//   +0x08: std::string message (SSO: data at +0x08, len at +0x10, sso_buf at +0x18)

// Console handler function pointer type
typedef void* (*console_handler_fn)(void* result_out, const void* command_data);

struct ConsoleCommandEntry {
    const char* name;
    uintptr_t handler_offset;  // base-relative
};

static const ConsoleCommandEntry g_console_commands[] = {
    {"AI.declare_war",          ck3::console_handlers::CMD_AI_DECLARE_WAR},
    {"AI.Enable",               ck3::console_handlers::CMD_AI_ENABLE},
    {"AI.Disable",              ck3::console_handlers::CMD_AI_DISABLE},
    {"AI.ToggleAll",            ck3::console_handlers::CMD_AI_TOGGLE_ALL},
    {"AI.try_send_decision",    ck3::console_handlers::CMD_AI_TRY_DECISION},
    {"ai.interaction_values",   ck3::console_handlers::CMD_AI_INTERACTION_VALS},
    {"add_piety",               ck3::console_handlers::CMD_ADD_PIETY},
    {"add_prestige",            ck3::console_handlers::CMD_ADD_PRESTIGE},
    {"add_stress",              ck3::console_handlers::CMD_ADD_STRESS},
    {"add_dread",               ck3::console_handlers::CMD_ADD_DREAD},
    {"add_influence",           ck3::console_handlers::CMD_ADD_INFLUENCE},
    {"add_trait",               ck3::console_handlers::CMD_ADD_TRAIT},
    {"add_claim",               ck3::console_handlers::CMD_ADD_CLAIM},
    {"give_title",              ck3::console_handlers::CMD_GIVE_TITLE},
    {"add_realm_law",           ck3::console_handlers::CMD_ADD_REALM_LAW},
    {"add_title_law",           ck3::console_handlers::CMD_ADD_TITLE_LAW},
    {"add_doctrine",            ck3::console_handlers::CMD_ADD_DOCTRINE},
    {"change_fervor",           ck3::console_handlers::CMD_CHANGE_FERVOR},
    {"add_innovation",          ck3::console_handlers::CMD_ADD_INNOVATION},
    {"add_fascination",         ck3::console_handlers::CMD_ADD_FASCINATION},
    {"add_relation",            ck3::console_handlers::CMD_ADD_RELATION},
    {"add_secret",              ck3::console_handlers::CMD_ADD_SECRET},
    {"helplog",                 ck3::console_handlers::CMD_HELPLOG},
    {"aiwatch",                 ck3::console_handlers::CMD_AIWATCH},
    {"debug_mode",              ck3::console_handlers::CMD_DEBUG_MODE},
    {"script_docs",             ck3::console_handlers::CMD_SCRIPT_DOCS},
    {"scripted_tests",          ck3::console_handlers::CMD_SCRIPTED_TESTS},
    {"debug_event",             ck3::console_handlers::CMD_DEBUG_EVENT},
    {"event_queue",             ck3::console_handlers::CMD_EVENT_QUEUE},
    {"event_counts",            ck3::console_handlers::CMD_EVENT_COUNTS},
    // Round 2: newly discovered handlers (ck-hxy9)
    {"AI.try_send_interaction", ck3::console_handlers::CMD_AI_TRY_INTERACTION},
    {"add_realm_law_skip_effects", ck3::console_handlers::CMD_ADD_REALM_LAW_SKIP},
    {"change_culture",          ck3::console_handlers::CMD_CHANGE_CULTURE},
    {"set_culture",             ck3::console_handlers::CMD_SET_CULTURE},
    {"charinfo",                ck3::console_handlers::CMD_CHARINFO},
    {"set_sexuality",           ck3::console_handlers::CMD_SET_SEXUALITY},
    {"know_schemes",            ck3::console_handlers::CMD_KNOW_SCHEMES},
    {"end_schemes",             ck3::console_handlers::CMD_END_SCHEMES},
    {"tick_day",                ck3::console_handlers::CMD_TICK_DAY},
    {"tick_development",        ck3::console_handlers::CMD_TICK_DEVELOPMENT},
    {"abort_travel_plan",       ck3::console_handlers::CMD_ABORT_TRAVEL},
    {"pause_on",                ck3::console_handlers::CMD_PAUSE_ON},
    {"pause_and_save_on",       ck3::console_handlers::CMD_PAUSE_SAVE_ON},
    {"test_save",               ck3::console_handlers::CMD_TEST_SAVE},
};
static const size_t g_console_command_count = sizeof(g_console_commands) / sizeof(g_console_commands[0]);

static console_handler_fn lookup_console_handler(const char* name) {
    for (size_t i = 0; i < g_console_command_count; i++) {
        if (strcasecmp(g_console_commands[i].name, name) == 0) {
            uintptr_t addr = g_base_addr + g_console_commands[i].handler_offset;
            if (!is_mapped(addr)) {
                ADLOG("console: handler for '%s' at 0x%lx not mapped\n", name, addr);
                return nullptr;
            }
            return reinterpret_cast<console_handler_fn>(addr);
        }
    }
    return nullptr;
}

// Argument entry in SCommandData (0x20 byte stride)
struct ConsoleArgEntry {
    const char* data;       // +0x00
    uint64_t length;        // +0x08
    uint64_t reserved[2];   // +0x10, +0x18
};

// SCommandData (minimal — only fields we know)
struct SCommandData {
    ConsoleArgEntry* args;  // +0x00
    uint32_t pad_08;        // +0x08
    int32_t arg_count;      // +0x0C
    uint8_t pad_rest[48];   // safety padding for unknown fields
};

// SResult (console handler output)
// Handler writes: +0x00 = success byte, +0x08 = std::string (libstdc++ layout).
// Using a real std::string ensures proper heap cleanup when SResult is destructed.
struct SResult {
    uint8_t success;        // +0x00
    uint8_t _pad[7];        // +0x01..0x07
    std::string message;    // +0x08 (libstdc++ SSO: data, length, sso_buf/capacity)
};

// ck-ehbi: harness-level set_truce implementation.
// `set_truce` is a PDXScript effect, not a console handler, so it has no
// fn_ptr entry in g_console_commands[]. The harness still invokes it via
// execute_console_command (bridge/harness/console.py:set_truce), so we
// special-case it here: call the game's GET_OR_CREATE_RELATION +
// TRUCE_ADD_ENTRY primitives directly. Runs on the game thread (caller
// execute_console_intent is driven by drain_and_execute_intents).
//
// Usage: set_truce <char_a_id> <char_b_id> <days>
static bool handle_set_truce(const std::vector<std::string>& tokens,
                              std::string& out_msg) {
    if (tokens.size() != 4) {
        out_msg = "set_truce: expected 3 args (char_a char_b days)";
        return false;
    }

    uint32_t char_a_id = static_cast<uint32_t>(atoi(tokens[1].c_str()));
    uint32_t char_b_id = static_cast<uint32_t>(atoi(tokens[2].c_str()));
    int32_t  days      = atoi(tokens[3].c_str());
    if (char_a_id == 0 || char_b_id == 0 || days <= 0) {
        out_msg = "set_truce: invalid args";
        return false;
    }
    if (char_a_id == char_b_id) {
        out_msg = "set_truce: char_a == char_b";
        return false;
    }

    void* char_a = lookup_character(char_a_id);
    void* char_b = lookup_character(char_b_id);
    if (!char_a || is_sentinel(char_a)) {
        out_msg = "set_truce: char_a not found";
        return false;
    }
    if (!char_b || is_sentinel(char_b)) {
        out_msg = "set_truce: char_b not found";
        return false;
    }

    uintptr_t got_addr = bridge::game_fn(ck3::funcs::GET_OR_CREATE_RELATION);
    uintptr_t tae_addr = bridge::game_fn(ck3::funcs::TRUCE_ADD_ENTRY);
    if (!is_mapped(got_addr) || !is_mapped(tae_addr)) {
        out_msg = "set_truce: engine functions not mapped";
        return false;
    }

    using fn_get_or_create = uintptr_t(*)(void*, void*);
    using fn_truce_add     = void(*)(uintptr_t, uint32_t, int64_t*, void*, int32_t, char);
    auto get_or_create = reinterpret_cast<fn_get_or_create>(got_addr);
    auto truce_add     = reinterpret_cast<fn_truce_add>(tae_addr);

    // Raw 32-bit IDs (CCharacter+0x18). TRUCE_ADD_ENTRY compares these
    // against CDiplomaticRelation::CHAR_ID_LOW/HIGH which store raw values.
    uint32_t raw_id_a = read<uint32_t>(reinterpret_cast<uintptr_t>(char_a)
                                        + ck3::character::ID);
    uint32_t raw_id_b = read<uint32_t>(reinterpret_cast<uintptr_t>(char_b)
                                        + ck3::character::ID);

    int32_t current_date = 0;
    uintptr_t gs = read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (gs && is_mapped(gs)) current_date = read<int32_t>(gs + ck3::game_state::GAME_DATE);
    if (current_date <= 0) {
        out_msg = "set_truce: failed to read current game date";
        return false;
    }

    // Expiry: 24 sub-day ticks per game day. Stored as int64 at relation
    // +0x28/+0x58; upper 32 bits left zero to match the game's own pattern.
    int64_t expiry_date = static_cast<int64_t>(current_date)
                           + static_cast<int64_t>(days) * 24;

    uintptr_t rel = get_or_create(char_a, char_b);
    if (!rel || !is_mapped(rel)) {
        out_msg = "set_truce: GET_OR_CREATE_RELATION returned invalid ptr";
        return false;
    }

    uint32_t slot_low  = read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_LOW);
    uint32_t slot_high = read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_HIGH);
    bool a_in_rel = (slot_low == raw_id_a) || (slot_high == raw_id_a);
    bool b_in_rel = (slot_low == raw_id_b) || (slot_high == raw_id_b);
    if (!a_in_rel || !b_in_rel) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "set_truce: relation ids mismatch (slots=%u/%u want=%u/%u)",
                 slot_low, slot_high, raw_id_a, raw_id_b);
        out_msg = buf;
        return false;
    }

    // Reason string — libstdc++ std::string, matching the ABI
    // TRUCE_ADD_ENTRY uses (_M_assign on param_4).
    std::string reason_str = "bridge_set_truce";

    truce_add(rel, raw_id_a, &expiry_date, &reason_str,
              ck3::diplomatic_relation::TRUCE_REASON_ATTACKER, 0);
    truce_add(rel, raw_id_b, &expiry_date, &reason_str,
              ck3::diplomatic_relation::TRUCE_REASON_DEFENDER, 0);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "truce set between %u and %u for %d days (expires at tick %ld)",
             char_a_id, char_b_id, days, static_cast<long>(expiry_date));
    out_msg = buf;
    return true;
}

void execute_console_intent(Intent* intent) {
    if (!intent || intent->console_command.empty()) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "empty_command");
        intent->consumed = true;
        return;
    }

    // Parse: first token is command name, rest are args
    std::string cmd = intent->console_command;
    std::vector<std::string> tokens;
    {
        size_t pos = 0;
        while (pos < cmd.size()) {
            while (pos < cmd.size() && (cmd[pos] == ' ' || cmd[pos] == '\t')) pos++;
            if (pos >= cmd.size()) break;
            size_t start = pos;
            while (pos < cmd.size() && cmd[pos] != ' ' && cmd[pos] != '\t') pos++;
            tokens.push_back(cmd.substr(start, pos - start));
        }
    }

    if (tokens.empty()) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "empty_command");
        intent->consumed = true;
        return;
    }

    const std::string& cmd_name = tokens[0];

    // ck-ehbi: `set_truce` is a PDXScript effect, not a console handler, but
    // the harness still invokes it via execute_console_command. Intercept it
    // before the fn_ptr lookup (which would reject as unknown_command) and
    // dispatch directly to GET_OR_CREATE_RELATION + TRUCE_ADD_ENTRY.
    if (strcasecmp(cmd_name.c_str(), "set_truce") == 0) {
        std::string msg;
        bool ok = false;
        bool crashed = false;
        watchdog_arm(5);
        g_in_protected_region = true;
        if (sigsetjmp(g_recovery_point, 1) == 0) {
            ok = handle_set_truce(tokens, msg);
        } else {
            crashed = true;
            msg = "set_truce: SIGSEGV in handler";
        }
        g_in_protected_region = false;
        watchdog_disarm();

        intent->executed = ok && !crashed;
        intent->rejected = !intent->executed;
        intent->consumed = true;
        intent->console_output = msg;
        if (!intent->executed) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "%.250s", msg.c_str());
        }
        ADLOG("console: set_truce -> success=%d msg='%s'\n",
              intent->executed ? 1 : 0, msg.c_str());
        return;
    }

    console_handler_fn handler = lookup_console_handler(cmd_name.c_str());
    if (!handler) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "unknown_command: %.100s", cmd_name.c_str());
        intent->consumed = true;
        ADLOG("console: unknown command '%s'\n", cmd_name.c_str());
        return;
    }

    // Build argument entries (tokens[1..N] are the arguments)
    size_t arg_count = tokens.size() - 1;
    std::vector<ConsoleArgEntry> arg_entries(arg_count);
    for (size_t i = 0; i < arg_count; i++) {
        arg_entries[i].data = tokens[i + 1].c_str();
        arg_entries[i].length = tokens[i + 1].size();
        arg_entries[i].reserved[0] = 0;
        arg_entries[i].reserved[1] = 0;
    }

    // Build SCommandData
    SCommandData cmd_data = {};
    cmd_data.args = arg_entries.empty() ? nullptr : arg_entries.data();
    cmd_data.arg_count = static_cast<int32_t>(arg_count);

    // Prepare SResult — the handler constructs the std::string at +0x08
    // by writing data/length/sso_buf directly. Using a real std::string
    // ensures proper destructor cleanup (heap free if non-SSO).
    SResult result;
    memset(&result, 0, sizeof(result));

    ADLOG("console: calling handler for '%s' with %zu args\n",
          cmd_name.c_str(), arg_count);

    // Call the handler on the game thread with SIGSEGV protection
    watchdog_arm(5);
    g_in_protected_region = true;
    volatile bool crashed = false;

    if (sigsetjmp(g_recovery_point, 1) == 0) {
        handler(&result, &cmd_data);
    } else {
        crashed = true;
        ADLOG("console: SIGSEGV in handler for '%s'\n", cmd_name.c_str());
    }

    g_in_protected_region = false;
    watchdog_disarm();

    if (crashed) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "handler_crashed");
        intent->consumed = true;
        // Don't destruct result.message — it may be in a corrupted state
        memset(&result, 0, sizeof(result));
        return;
    }

    // Read result
    intent->executed = (result.success != 0);
    intent->rejected = (result.success == 0);
    intent->consumed = true;

    // Extract output string from the std::string the handler constructed
    if (!result.message.empty()) {
        size_t safe_len = std::min(result.message.size(), (size_t)4096);
        intent->console_output = result.message.substr(0, safe_len);
    }

    if (!intent->executed) {
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "handler_returned_failure");
    }

    ADLOG("console: '%s' -> success=%d output='%s'\n",
          cmd_name.c_str(), result.success,
          intent->console_output.substr(0, 100).c_str());

    // SResult destructor cleans up result.message (std::string dtor frees heap if needed)
}

// =============================================================================
// drain_and_execute_intents — Serial intent execution from on_tick
// =============================================================================
//
// Called from on_tick() on the game thread, in the serial section BEFORE
// parallel AI workers launch. This replaces the old hooked_ai_interaction_decision
// approach which fired 10,000+ times per tick across parallel threads.
//
// This is safe: single-threaded, TLS initialized, no contention.

void drain_and_execute_intents() {
    // Process any pending acceptance query (deferred from IPC thread)
    if (g_acceptance_query.state.load(std::memory_order_acquire) == 1) {
        process_acceptance_query();
    }

    // Process any pending CB validity query (deferred from IPC thread).
    // H1 (re_warplanner_lifecycle.md §1.4): service up to kCbDrainPerTick CB
    // submissions per tick instead of exactly one. When the deferred-drain
    // coordinator pulses unpaused for only a tick or two, a SECOND bridge CB
    // caller that resubmits within the same pulse window otherwise has to wait a
    // whole tick → falls into the retry/timeout path unnecessarily. Allowing a
    // bounded few iterations lets a fast resubmit land in the same pulse.
    // SAFE/superset: process_cb_query CAS-guards 1→4 and returns immediately if
    // not pending, so an iteration with no pending submission is a single atomic
    // load + return — no new state, no allocation. The existing acquire-retry in
    // cb_query_acquire_slot still covers a caller that hasn't submitted yet.
    static constexpr int kCbDrainPerTick = 2;
    for (int i = 0; i < kCbDrainPerTick &&
         g_cb_query.state.load(std::memory_order_acquire) == 1; ++i) {
        process_cb_query();
    }

    // Process any pending trigger evaluation query (deferred from IPC thread)
    if (g_trigger_query.state.load(std::memory_order_acquire) == 1) {
        process_trigger_query();
    }

    // Process any pending named-trigger eval query (ck-fo7x.4, deferred from IPC thread)
    if (g_named_trigger_query.state.load(std::memory_order_acquire) == 1) {
        process_named_trigger_query();
    }

    // #64: Process any pending annotated decision-eval query (deferred from IPC thread;
    // the trigger evals must run here on the game thread where the watchdog fires).
    if (g_decision_eval_query.state.load(std::memory_order_acquire) == 1) {
        process_decision_eval_query();
    }

    // Process any pending interaction-cascade probe (ck-e58l.8, deferred from IPC thread)
    if (cascade_query_pending()) {
        process_cascade_query();
    }

    // Fast-path atomic check — avoid the mutex when there are no intents
    if (g_intent_queue.pending_count_fast() == 0) {
        return;
    }

    ADLOG("DRAIN_INTENTS: pending_count_fast=%zu — draining\n",
          g_intent_queue.pending_count_fast());

    // Take a snapshot of pending intents
    std::vector<Intent> snapshot = g_intent_queue.snapshot();
    if (snapshot.empty()) {
        return;
    }

    ADLOG("DRAIN_INTENTS: %zu intents to process\n", snapshot.size());

    for (auto& intent : snapshot) {
        if (intent.consumed) continue;

        // M1: install the owning session context for the duration of this intent.
        // The guard is scoped to the iteration so a crash/longjmp still restores
        // the previous context on destruction.
        bridge::ClientContext intent_ctx;
        std::unique_ptr<bridge::ClientContextGuard> intent_ctx_guard;
        if (!intent.session_id.empty()) {
            bridge::Session* s = bridge::g_session_registry.find(intent.session_id);
            if (s) {
                intent_ctx.session = s;
                intent_ctx.action_type = intent.interaction_name.c_str();
                intent_ctx_guard = std::make_unique<bridge::ClientContextGuard>(&intent_ctx);
            }
        }

        // 2026-06-09: record the in-flight intent so the outer on_tick recovery
        // handler can convert a game-thread crash here into a structured
        // rejection instead of a silent 5s IPC timeout. Cleared after the
        // switch; set again per intent.
        g_inflight_intent_rid = intent.request_id;

        switch (intent.type) {
        case IntentType::INTERACTION: {
            // Resolve character and interaction objects
            void* character = lookup_character(intent.actor_id);
            if (!character) {
                ADLOG("DRAIN_INTENTS: actor %u not found — rejecting %s\n",
                      intent.actor_id & 0xFFFFFF, intent.interaction_name.c_str());
                // Mark in the real queue
                Intent* real = g_intent_queue.find_for_character(
                    intent.actor_id & 0xFFFFFF, IntentType::INTERACTION);
                if (real) {
                    real->rejected = true;
                    snprintf(real->rejection_reason, sizeof(real->rejection_reason),
                             "actor_not_found");
                    real->consumed = true;
                }
                continue;
            }

            void* interaction = lookup_interaction(intent.interaction_name.c_str());
            if (!interaction) {
                ADLOG("DRAIN_INTENTS: interaction '%s' not found — rejecting\n",
                      intent.interaction_name.c_str());
                Intent* real = g_intent_queue.find_for_character(
                    intent.actor_id & 0xFFFFFF, IntentType::INTERACTION);
                if (real) {
                    real->rejected = true;
                    snprintf(real->rejection_reason, sizeof(real->rejection_reason),
                             "interaction_not_found");
                    real->consumed = true;
                }
                continue;
            }

            // Find the real intent in the queue (snapshot is a copy)
            Intent* real = g_intent_queue.find_for_character_interaction(
                intent.actor_id & 0xFFFFFF, intent.interaction_hash);
            if (!real) {
                ADLOG("DRAIN_INTENTS: intent vanished from queue — skipping\n");
                continue;
            }

            ADLOG("DRAIN_INTENTS: executing INTERACTION %s actor=%u target=%u debug=%s\n",
                  real->interaction_name.c_str(),
                  real->actor_id & 0xFFFFFF,
                  real->target_id & 0xFFFFFF,
                  real->debug_all_paths ? "YES" : "NO");

            if (real->debug_all_paths) {
                // Debug mode: try all dispatch paths, collect results
                execute_war_debug_dispatch(real, character, interaction);
            } else {
                bool dispatched = execute_direct_dispatch(real, character, interaction);

                if (dispatched) {
                    ADLOG("DRAIN_INTENTS: SUCCESS %s actor=%u target=%u\n",
                          real->interaction_name.c_str(),
                          real->actor_id & 0xFFFFFF,
                          real->target_id & 0xFFFFFF);
                    real->executed = true;
                    real->consumed = true;
                } else {
                    ADLOG("DRAIN_INTENTS: REJECTED %s actor=%u target=%u — %s\n",
                          real->interaction_name.c_str(),
                          real->actor_id & 0xFFFFFF,
                          real->target_id & 0xFFFFFF,
                          real->rejection_reason);
                    real->rejected = true;
                    real->consumed = true;
                }
            }
            break;
        }

        case IntentType::WAR_DECLARE: {
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::WAR_DECLARE);
            if (!real) continue;

            // Full dequeue dump (ck-l8ge): the IPC enqueue logs the intent on
            // arrival, but between enqueue and dequeue any caller-side mutation
            // would otherwise be invisible. Dump all dispatch-relevant fields
            // here so trace-by-tick reconstruction has a single anchor line per
            // war intent at the moment dispatch begins.
            ADLOG("DRAIN_INTENTS: dequeued WAR_DECLARE actor=%u target=%u "
                  "cb_index=%d title_id=%d claimant=%u force_bypass=%d "
                  "queued_tick=%lu age=%lu ttl=%u request_id='%s'\n",
                  real->actor_id & 0xFFFFFF, real->target_id & 0xFFFFFF,
                  real->cb_index, real->title_id,
                  real->claimant_id & 0xFFFFFF,
                  real->force_bypass ? 1 : 0,
                  (unsigned long)real->queued_tick,
                  (unsigned long)(g_tick_id.load(std::memory_order_relaxed) - real->queued_tick),
                  real->ttl_ticks,
                  real->request_id.c_str());
            execute_war_intent(real);
            ADLOG("DRAIN_INTENTS: WAR_DECLARE post-execute actor=%u executed=%d rejected=%d reason='%s'\n",
                  real->actor_id & 0xFFFFFF,
                  real->executed ? 1 : 0,
                  real->rejected ? 1 : 0,
                  real->rejection_reason[0] ? real->rejection_reason : "");
            break;
        }

        case IntentType::DECISION: {
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::DECISION);
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing DECISION actor=%u decision='%s'\n",
                  real->actor_id & 0xFFFFFF, real->interaction_name.c_str());
            execute_decision_intent(real);
            break;
        }

        case IntentType::CONSOLE_EXECUTE: {
            // Find the real intent by request_id (console intents have no actor)
            Intent* real = nullptr;
            {
                auto snap = g_intent_queue.snapshot();
                for (auto& s : snap) {
                    if (!s.consumed && s.type == IntentType::CONSOLE_EXECUTE &&
                        s.request_id == intent.request_id) {
                        // Find in actual queue
                        real = g_intent_queue.find_for_character(0, IntentType::CONSOLE_EXECUTE);
                        // Walk until we find the right one
                        break;
                    }
                }
            }
            if (!real) {
                // Fallback: find any unconsumed CONSOLE_EXECUTE
                real = g_intent_queue.find_for_character(0, IntentType::CONSOLE_EXECUTE);
            }
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing CONSOLE_EXECUTE '%s'\n",
                  real->console_command.c_str());
            execute_console_intent(real);
            break;
        }

        case IntentType::SCRIPTED_EFFECT: {
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::SCRIPTED_EFFECT);
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing SCRIPTED_EFFECT actor=%u effect='%s'\n",
                  real->actor_id & 0xFFFFFF, real->interaction_name.c_str());
            execute_scripted_effect_intent(real);
            break;
        }

        case IntentType::WAR_CREATE_CORE_DIRECT: {
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::WAR_CREATE_CORE_DIRECT);
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing WAR_CREATE_CORE_DIRECT actor=%u target=%u cb='%s' title=%d claimant=%u\n",
                  real->actor_id & 0xFFFFFF, real->target_id & 0xFFFFFF,
                  real->interaction_name.c_str(), real->title_id,
                  real->claimant_id & 0xFFFFFF);
            execute_war_create_core_intent(real);
            break;
        }

        case IntentType::WAR_FORCE_OFFENSIVE_STANCE: {
            // Fix B1a (re_caiwarcoordinator_FINAL.md): drive the engine's own
            // per-coordinator stance tick on the steered war's record so the AI
            // prosecutes it offensively. Game-thread serial; DEFAULT-OFF behind
            // g_force_offensive_stance. actor_id = attacker; title_id = war_id.
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::WAR_FORCE_OFFENSIVE_STANCE);
            if (!real) continue;
            ADLOG("DRAIN_INTENTS: executing WAR_FORCE_OFFENSIVE_STANCE actor=%u war=0x%x\n",
                  real->actor_id & 0xFFFFFF, static_cast<uint32_t>(real->title_id));
            execute_war_force_offensive_stance_intent(real);
            break;
        }

        case IntentType::FACTION: {
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::FACTION);
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing FACTION actor=%u liege=%u type='%s' action='%s'\n",
                  real->actor_id & 0xFFFFFF, real->target_id & 0xFFFFFF,
                  real->interaction_name.c_str(), real->console_command.c_str());
            execute_faction_intent(real);
            break;
        }

        case IntentType::RAISE_ARMIES: {
            // A8.2 (re_raise_armies.md): raise the chosen AI's levies via the
            // guarded g_PlayerCharacterID global-swap primitive. Game-thread
            // serial section only — mirrors the WAR_DECLARE case.
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::RAISE_ARMIES);
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing RAISE_ARMIES actor=%u\n",
                  real->actor_id & 0xFFFFFF);
            execute_raise_armies_intent(real);
            ADLOG("DRAIN_INTENTS: RAISE_ARMIES post-execute actor=%u executed=%d rejected=%d reason='%s'\n",
                  real->actor_id & 0xFFFFFF,
                  real->executed ? 1 : 0,
                  real->rejected ? 1 : 0,
                  real->rejection_reason[0] ? real->rejection_reason : "");
            break;
        }
        case IntentType::RAISE_RAIDERS: {
            // #96 (re_raid_system.md): toggle the chosen AI's raised armies into
            // RAIDING mode via the guarded g_PlayerCharacterID global-swap
            // primitive. Game-thread serial section only — mirrors RAISE_ARMIES.
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::RAISE_RAIDERS);
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing RAISE_RAIDERS actor=%u\n",
                  real->actor_id & 0xFFFFFF);
            execute_raise_raiders_intent(real);
            ADLOG("DRAIN_INTENTS: RAISE_RAIDERS post-execute actor=%u executed=%d rejected=%d reason='%s'\n",
                  real->actor_id & 0xFFFFFF,
                  real->executed ? 1 : 0,
                  real->rejected ? 1 : 0,
                  real->rejection_reason[0] ? real->rejection_reason : "");
            break;
        }
        case IntentType::CREATE_TITLE: {
            // #111 (re_create_title_action.md): LEGITIMATE de-jure create-title via
            // CCreateTitleCommand + CanExecute gate + GAME_COMMAND_DISPATCH(0xe).
            // Game-thread serial section only — under watchdog+sigsetjmp+fake-RNG.
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::CREATE_TITLE);
            if (!real) continue;

            ADLOG("DRAIN_INTENTS: executing CREATE_TITLE actor=%u title_id=%d\n",
                  real->actor_id & 0xFFFFFF, real->title_id);
            execute_create_title_intent(real);
            ADLOG("DRAIN_INTENTS: CREATE_TITLE post-execute actor=%u executed=%d rejected=%d reason='%s'\n",
                  real->actor_id & 0xFFFFFF,
                  real->executed ? 1 : 0,
                  real->rejected ? 1 : 0,
                  real->rejection_reason[0] ? real->rejection_reason : "");
            break;
        }
        case IntentType::SCHEME: {
            // #132 (docs/notes/hostile-intrigue-research.md): LEGITIMATE start-a-scheme
            // (murder / fabricate_hook / claim_throne) via CreateActiveScheme forward.
            // The engine's own gates run inside that call (self-gating). Game-thread
            // serial section only — under watchdog+sigsetjmp+fake-RNG.
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::SCHEME);
            if (!real) continue;

            // #scheme_cancel: branch start vs cancel on console_command ("cancel" => abandon
            // an owned scheme via RemoveActiveScheme; default/"start" => CreateActiveScheme).
            if (real->console_command == "cancel") {
                ADLOG("DRAIN_INTENTS: executing SCHEME CANCEL owner=%u scheme_id=%d\n",
                      real->actor_id & 0xFFFFFF, real->title_id);
                execute_scheme_cancel_intent(real);
                ADLOG("DRAIN_INTENTS: SCHEME CANCEL post-execute owner=%u executed=%d rejected=%d reason='%s'\n",
                      real->actor_id & 0xFFFFFF, real->executed ? 1 : 0,
                      real->rejected ? 1 : 0,
                      real->rejection_reason[0] ? real->rejection_reason : "");
                break;
            }

            ADLOG("DRAIN_INTENTS: executing SCHEME owner=%u scheme='%s' target=%u\n",
                  real->actor_id & 0xFFFFFF, real->interaction_name.c_str(),
                  real->target_id & 0xFFFFFF);
            execute_scheme_intent(real);
            ADLOG("DRAIN_INTENTS: SCHEME post-execute owner=%u executed=%d rejected=%d reason='%s'\n",
                  real->actor_id & 0xFFFFFF,
                  real->executed ? 1 : 0,
                  real->rejected ? 1 : 0,
                  real->rejection_reason[0] ? real->rejection_reason : "");
            break;
        }
        case IntentType::BREAK_ALLIANCE: {
            // re_break_alliance_FINAL.md Path B: the literal gated break via
            // break_alliance_cmd(actor, ally, 0). Game-thread serial section only —
            // under watchdog+sigsetjmp+fake-RNG. Only ever queued when the
            // g_enable_break_alliance flag is armed (do_break_alliance).
            Intent* real = g_intent_queue.find_for_character(
                intent.actor_id & 0xFFFFFF, IntentType::BREAK_ALLIANCE);
            if (!real) continue;
            ADLOG("DRAIN_INTENTS: executing BREAK_ALLIANCE actor=%u ally=%u\n",
                  real->actor_id & 0xFFFFFF, real->target_id & 0xFFFFFF);
            execute_break_alliance_intent(real);
            ADLOG("DRAIN_INTENTS: BREAK_ALLIANCE post-execute actor=%u executed=%d rejected=%d reason='%s'\n",
                  real->actor_id & 0xFFFFFF,
                  real->executed ? 1 : 0,
                  real->rejected ? 1 : 0,
                  real->rejection_reason[0] ? real->rejection_reason : "");
            break;
        }
        // REMOVED: IntentType::MOVE_ARMY case deleted per design (agent does not
        // drive army micro).
        }

        // Intent finished without crashing — clear the in-flight marker.
        g_inflight_intent_rid.clear();
    }

    ADLOG("DRAIN_INTENTS: done\n");
}

// Discover TLS offsets needed by execute_direct_dispatch.
// Called once during deferred initialization from the first on_tick.
void init_tls_offsets() {
    // Discover TLS offsets — execute_direct_dispatch needs these to access
    // the TLS interaction context on the game thread
    if (discover_tls_offsets()) {
        g_tls_offsets_discovered = true;
    } else {
        g_tls_offsets_discovered = false;
        ADLOG("WARNING: TLS offset discovery failed, using hardcoded defaults "
              "(guard=0x%lx, context=0x%lx)\n",
              (unsigned long)g_tls_guard_offset, (unsigned long)g_tls_context_offset);
    }

    ADLOG("TLS offsets initialized (guard=0x%lx, context=0x%lx, source=%s). "
          "Intents execute from on_tick serial section.\n",
          (unsigned long)g_tls_guard_offset, (unsigned long)g_tls_context_offset,
          g_tls_offsets_discovered ? "discovered" : "HARDCODED_FALLBACK");
}

// Discover vtable and dispatch manager addresses from instruction decoding.
// Called once during deferred initialization from the first on_tick.
// Diagnostic: hex dump + Zydis instruction decode for a function address
static void dump_function_diagnostics(const char* label, uintptr_t addr) {
    ADLOG("RUNTIME_DIAG: %s at 0x%lx (base=0x%lx + 0x%lx)\n",
          label, addr, g_base_addr, addr - g_base_addr);

    if (!is_mapped(addr)) {
        ADLOG("RUNTIME_DIAG: %s address NOT MAPPED — aborting dump\n", label);
        return;
    }

    // Hex dump first 64 bytes
    const uint8_t* code = reinterpret_cast<const uint8_t*>(addr);
    for (int row = 0; row < 4; row++) {
        char hex[200];
        int pos = 0;
        for (int col = 0; col < 16; col++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", code[row * 16 + col]);
        }
        ADLOG("  +0x%02x: %s\n", row * 16, hex);
    }

    // Decode first 10 instructions using Zydis
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

    size_t offset = 0;
    for (int i = 0; i < 10 && offset < 128; i++) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset,
                                                  128 - offset, &insn, operands))) {
            ADLOG("  insn[%d] at +0x%02x: DECODE FAILED\n", i, (int)offset);
            offset++;
            continue;
        }
        char buf[256];
        ZydisFormatterFormatInstruction(&formatter, &insn, operands,
                                         insn.operand_count_visible,
                                         buf, sizeof(buf), addr + offset, NULL);
        ADLOG("  insn[%d] at +0x%02x: %s\n", i, (int)offset, buf);
        offset += insn.length;
    }
}

void init_runtime_discovery() {
    ADLOG("RUNTIME_DISCOVERY: Starting Zydis-based vtable/global discovery\n");

    // Diagnostic dumps BEFORE discovery — see actual instructions at target addresses
    uintptr_t build_send_addr = bridge::game_fn(ck3::funcs::BUILD_SEND_CMD_FROM_BUF);
    uintptr_t dispatch_addr = bridge::game_fn(ck3::funcs::GAME_COMMAND_DISPATCH);
    dump_function_diagnostics("BUILD_SEND_CMD_FROM_BUF", build_send_addr);
    dump_function_diagnostics("GAME_COMMAND_DISPATCH", dispatch_addr);

    // --- Discover vtable2 first (it's more reliably found) ---
    uintptr_t vtable2 = ck3_resolver::discover_send_cmd_vtable2(g_base_addr);
    if (vtable2) {
        uintptr_t new_offset = vtable2 - g_base_addr;
        ADLOG("RUNTIME_DISCOVERY: SEND_CMD_VTABLE2 = 0x%lx (was 0x%lx, delta=%+ld)\n",
              new_offset, g_send_cmd_vtable2_offset,
              (long)(new_offset - g_send_cmd_vtable2_offset));
        g_send_cmd_vtable2_offset = new_offset;
    } else {
        ADLOG("RUNTIME_DISCOVERY: SEND_CMD_VTABLE2 discovery failed, using hardcoded 0x%lx\n",
              g_send_cmd_vtable2_offset);
    }

    // --- Discover vtable1 (primary vtable) ---
    uintptr_t vtable1 = ck3_resolver::discover_send_cmd_vtable(g_base_addr);
    if (vtable1) {
        uintptr_t new_offset = vtable1 - g_base_addr;
        ADLOG("RUNTIME_DISCOVERY: SEND_CMD_VTABLE = 0x%lx (was 0x%lx, delta=%+ld)\n",
              new_offset, g_send_cmd_vtable_offset,
              (long)(new_offset - g_send_cmd_vtable_offset));
        g_send_cmd_vtable_offset = new_offset;
    } else if (vtable2) {
        // Derive vtable1 from vtable2: in the VPS binary, vtable1 is 0xB0 bytes
        // before vtable2 (0x0bc73ee8 - 0x0bc73e38 = 0xB0). This relationship
        // holds because they're adjacent vtables in the same class hierarchy.
        static constexpr uintptr_t VTABLE_DELTA = 0xB0;
        uintptr_t derived = vtable2 - VTABLE_DELTA;
        uintptr_t derived_offset = derived - g_base_addr;

        // Validate: vtable1[0] should be a function pointer in .text
        uintptr_t entry0 = 0;
        if (is_mapped(derived)) {
            entry0 = safe_read<uintptr_t>(derived);
        }
        bool entry0_valid = entry0 > g_base_addr && is_mapped(entry0);

        if (entry0_valid) {
            ADLOG("RUNTIME_DISCOVERY: SEND_CMD_VTABLE = 0x%lx (DERIVED from vtable2 - 0x%lx, "
                  "entry[0]=0x%lx VALID)\n",
                  derived_offset, VTABLE_DELTA, entry0);
            g_send_cmd_vtable_offset = derived_offset;
        } else {
            ADLOG("RUNTIME_DISCOVERY: vtable1 derivation from vtable2 FAILED — "
                  "derived=0x%lx entry[0]=0x%lx (valid=%d)\n",
                  derived, entry0, entry0_valid);
            ADLOG("RUNTIME_DISCOVERY: SEND_CMD_VTABLE using hardcoded 0x%lx\n",
                  g_send_cmd_vtable_offset);
        }
    } else {
        ADLOG("RUNTIME_DISCOVERY: SEND_CMD_VTABLE discovery failed, using hardcoded 0x%lx\n",
              g_send_cmd_vtable_offset);
    }

    // --- Discover dispatch_mgr (from callers of GAME_COMMAND_DISPATCH) ---
    uintptr_t mgr = ck3_resolver::discover_dispatch_mgr(g_base_addr);
    if (mgr) {
        uintptr_t new_offset = mgr - g_base_addr;
        ADLOG("RUNTIME_DISCOVERY: DISPATCH_MGR = 0x%lx (was 0x%lx, delta=%+ld)\n",
              new_offset, g_dispatch_mgr_offset,
              (long)(new_offset - g_dispatch_mgr_offset));
        g_dispatch_mgr_offset = new_offset;

        // Validate: the global should contain a non-null pointer to a valid object
        uintptr_t mgr_value = safe_read<uintptr_t>(mgr);
        if (mgr_value && is_mapped(mgr_value)) {
            uintptr_t recorder = safe_read<uintptr_t>(mgr_value + ck3::dispatch_mgr::RECORDER_PTR);
            ADLOG("RUNTIME_DISCOVERY: dispatch_mgr validation: *mgr=0x%lx (mapped), "
                  "recorder(+0x3f0)=0x%lx\n", mgr_value, recorder);
        } else {
            ADLOG("RUNTIME_DISCOVERY: WARNING — dispatch_mgr at 0x%lx contains 0x%lx "
                  "(null or unmapped). May not be initialized yet.\n",
                  mgr, mgr_value);
        }
    } else {
        ADLOG("RUNTIME_DISCOVERY: DISPATCH_MGR discovery failed, using hardcoded 0x%lx\n",
              g_dispatch_mgr_offset);
    }

    // --- Resolve BUILD_SEND_CMD_FROM_BUF address ---
    // CRITICAL FIX: Do NOT follow_thunk here. The function at BUILD_SEND_CMD_FROM_BUF
    // (0x0609b410) is NOT a thunk — it IS the real CSendCharacterInteractionCommand
    // constructor. Its internal structure is:
    //   1. CALL base_ctor (sets GENERIC CJominiCommandBase vtable at 0xbe187e0)
    //   2. LEA+MOV to OVERWRITE vtable with CSendCharInteractionCmd vtable (0xbb73e38)
    //   3. LEA+MOV to set helper vtable at +0x18 (0xbb73ee8)
    //   4. CALL deep_copy to populate fields from TLS context
    //
    // follow_thunk was incorrectly following the CALL in step 1 to the base_ctor
    // (at 0x0755b630), which only sets the GENERIC vtable and returns. Steps 2-4
    // (vtable overwrite + deep copy) never execute. This caused:
    //   - Wrong vtable (0xbe187e0 instead of 0xbb73e38, delta ~2.7MB)
    //   - vtable[9] (Clone) pointing into a shared library instead of CK3 binary
    //   - Crash on Clone call
    //
    // The fix: use BUILD_SEND_CMD_FROM_BUF directly. It takes (void* out_cmd, uintptr_t tls_ctx).
    {
        g_real_build_send_cmd_addr = bridge::game_fn(ck3::funcs::BUILD_SEND_CMD_FROM_BUF);
        ADLOG("RUNTIME_DISCOVERY: BUILD_SEND_CMD = 0x%lx (direct, no thunk follow)\n",
              g_real_build_send_cmd_addr);
    }

    ADLOG("RUNTIME_DISCOVERY: Complete — vtable1=0x%lx vtable2=0x%lx dispatch_mgr=0x%lx "
          "real_build_cmd=0x%lx\n",
          g_send_cmd_vtable_offset, g_send_cmd_vtable2_offset, g_dispatch_mgr_offset,
          g_real_build_send_cmd_addr);
}

// Queue an intent for later processing by the AI evaluation hook
bool queue_intent(uint32_t actor_id, uint32_t target_id,
                  const char* interaction_type, uint32_t max_age_ticks,
                  const char* request_id_in, bool force_bypass,
                  uint32_t claimant_id, int cb_index,
                  uint32_t secondary_actor_id, uint32_t secondary_recipient_id,
                  int title_id,
                  bool debug_all_paths,
                  IntentType intent_type) {
    if (!interaction_type || !interaction_type[0]) return false;

    uint32_t hash = game_string_hash(interaction_type,
                                      static_cast<uint32_t>(strlen(interaction_type)));
    // Hash is required for INTERACTION dispatch (used to look up the interaction
    // object). Non-INTERACTION types route through a dedicated executor (e.g.
    // execute_war_intent for WAR_DECLARE) and do not consume the hash.
    if (hash == 0 && intent_type == IntentType::INTERACTION) return false;

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    uint32_t ttl = max_age_ticks > 0 ? max_age_ticks : 100;

    std::string req_id;
    if (request_id_in && request_id_in[0]) {
        req_id = request_id_in;
    } else {
        char req_id_buf[64];
        snprintf(req_id_buf, sizeof(req_id_buf), "qi_%u_%lu_%u",
                 actor_id & 0xFFFFFF, (unsigned long)current_tick,
                 generate_rng() & 0xFFFF);
        req_id = req_id_buf;
    }

    Intent intent = {};
    intent.type = intent_type;
    intent.actor_id = actor_id;
    intent.target_id = target_id;
    intent.interaction_hash = hash;
    intent.interaction_name = std::string(interaction_type);
    intent.title_id = title_id;
    intent.claimant_id = claimant_id;
    intent.cb_index = cb_index;
    intent.secondary_actor_id = secondary_actor_id;
    intent.secondary_recipient_id = secondary_recipient_id;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = ttl;
    intent.request_id = req_id;
    intent.force_bypass = force_bypass;
    intent.debug_all_paths = debug_all_paths;

    g_intent_queue.enqueue(std::move(intent));

    ADLOG("Intent queued (via queue_intent): %s actor=%u target=%u hash=0x%08x ttl=%u title_id=%d cb_idx=%d claimant=%u bypass=%s debug=%s\n",
          interaction_type, actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
          hash, ttl, title_id, cb_index, claimant_id & 0xFFFFFF,
          force_bypass ? "YES" : "NO", debug_all_paths ? "YES" : "NO");
    return true;
}

// Get count of pending intents
size_t pending_intent_count() {
    return g_intent_queue.pending_count();
}

// Clear all pending intents
void clear_pending_intents() {
    g_intent_queue.clear();
    ADLOG("All pending intents cleared\n");
}

} // namespace bridge
