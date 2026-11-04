// ai_eval_hook.cpp — Intent execution infrastructure: execute_direct_dispatch,
// drain_and_execute_intents, TLS discovery, queue_intent.
//
// ARCHITECTURE CHANGE: The old hooked_ai_interaction_decision (which fired
// 10,000+ times per tick across parallel threads) has been removed. All intent
// execution now happens via drain_and_execute_intents(), called from on_tick()
// in the serial section BEFORE parallel AI workers launch. This eliminates
// deadlock/hang caused by mutex access in the parallel AI eval hot path.

#include "action_dispatch_internal.h"
#include "diag_dtor.h"   // L20-D: the diag record dtor, witnessed from the engine's call
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "runtime_resolver.h"
#include "cb_outer_filter.h"
#include "pipeline.h"                 // ck-6kvq.10: the pulse ledger (§2.9)
#include "scheme_engine.h"             // l3-schemes: execute_scheme_check_intent
#include "engine_text.h"               // F16: the check's engine sentences, cleaned
#include <Zydis/Zydis.h>
#include "tls_prologue_scan.h"         // F12: the TLS guard and context offsets
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

// TLS offsets: ck3_offsets.h interaction_ctx, confirmed against the running binary at init
static uintptr_t g_tls_guard_offset   = ck3::interaction_ctx::TLS_GUARD_FS_OFFSET;
static uintptr_t g_tls_context_offset = ck3::interaction_ctx::TLS_CONTEXT_FS_OFFSET;

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
// failed, using hardcoded defaults" appeared in both the crashed episode and a
// separate running probe, with a guard default that was wrong: see
// ck3_offsets.h interaction_ctx::TLS_GUARD_FS_OFFSET). So both accesses were
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
              g_tls_offsets_discovered ? "discovered" : "ck3_offsets.h");
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

// Both TLS offsets, read from AI_INTERACTION_CORE's own prologue
// (tls_prologue_scan.h). When the scan fails the compiled-in values
// (ck3_offsets.h interaction_ctx) stay, and the log says which ran.
static bool discover_tls_offsets() {
    uintptr_t func_addr = bridge::game_fn(ck3::funcs::AI_INTERACTION_CORE);
    if (!is_mapped(func_addr)) {
        ADLOG("TLS_DISCOVER: AI_INTERACTION_CORE at 0x%lx is not mapped\n", func_addr);
        return false;
    }
    static constexpr size_t SCAN_SIZE = 256;
    TlsPrologue p;
    if (!scan_tls_prologue(reinterpret_cast<const uint8_t*>(func_addr), SCAN_SIZE, &p)) {
        ADLOG("TLS_DISCOVER: not found in AI_INTERACTION_CORE's first %zu bytes "
              "(guard=0x%lx context=0x%lx)\n", SCAN_SIZE,
              (unsigned long)p.guard, (unsigned long)p.context);
        return false;
    }
    if (p.guard != ck3::interaction_ctx::TLS_GUARD_FS_OFFSET ||
        p.context != ck3::interaction_ctx::TLS_CONTEXT_FS_OFFSET) {
        ADLOG("TLS_DISCOVER: this binary differs from ck3_offsets.h "
              "(guard 0x%lx vs 0x%lx, context 0x%lx vs 0x%lx); using this binary's\n",
              (unsigned long)p.guard, (unsigned long)ck3::interaction_ctx::TLS_GUARD_FS_OFFSET,
              (unsigned long)p.context, (unsigned long)ck3::interaction_ctx::TLS_CONTEXT_FS_OFFSET);
    }
    g_tls_guard_offset = p.guard;
    g_tls_context_offset = p.context;
    ADLOG("TLS_DISCOVER: SUCCESS -- guard fs:-0x%lx (cmp at func+0x%zx), "
          "context fs_base-0x%lx (lea at func+0x%zx)\n",
          (unsigned long)p.guard, p.guard_at, (unsigned long)p.context, p.context_at);
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
// A C++ bool (engine_bool.h): its five engine callers test AL only (objdump
// 0x4cc4883, 0x4cc4992, 0x4321c24 movzbl al, 0x4cc39c4). L14 S3: read as uint64_t
// on Linux until then; the Windows build already masked AL.
using fn_ctx_validate   = bridge::EngineBoolFn<uintptr_t*>;
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

// =============================================================================
// The interaction window's choices (lane l4, re/findings/re_interactions.md)
// =============================================================================

// The name of a scope-name registry id (§2): the registry's id -> name vector, the
// read FUN_081f0c80 makes. Empty for an id the registry does not hold.
std::string scope_name_for_id(uint32_t id) {
    const uintptr_t reg = g_base_addr + ck3::globals::SCOPE_NAME_REGISTRY;
    const uintptr_t names = safe_read<uintptr_t>(reg + ck3::scope_name_registry::NAMES_ARRAY);
    const int32_t count = safe_read<int32_t>(reg + ck3::scope_name_registry::NAMES_COUNT);
    const uint32_t idx = id & 0xFFFFFF;
    if (!names || !is_mapped(names) || count <= 0 || idx >= static_cast<uint32_t>(count)) return {};
    return ad_read_std_string(names + static_cast<uintptr_t>(idx) * ck3::scope_name_registry::NAME_STRIDE);
}

// The send-option flag names an interaction declares, in script order (index = the
// option's slot in the selection array the window toggles).
std::vector<std::string> interaction_send_option_names(void* interaction) {
    std::vector<std::string> out;
    const uintptr_t def = reinterpret_cast<uintptr_t>(interaction);
    if (!def) return out;
    const int32_t n = safe_read<int32_t>(def + ck3::interaction_def::OPTION_COUNT);
    const uintptr_t arr = safe_read<uintptr_t>(def + ck3::interaction_def::OPTION_ARRAY_PTR);
    if (n <= 0 || n > 64 || !arr || !is_mapped(arr)) return out;
    for (int32_t i = 0; i < n; ++i) {
        const uintptr_t opt = arr + static_cast<uintptr_t>(i) * ck3::interaction_def::OPT_STRIDE;
        out.push_back(scope_name_for_id(safe_read<uint32_t>(opt + ck3::interaction_def::OPT_FLAG_NAME_ID)));
    }
    return out;
}

// A living character's full engine id (generation byte included), or 0.
static uint32_t living_full_char_id(uint32_t id) {
    if ((id & 0xFFFFFF) == 0) return 0;
    void* c = lookup_character(id & 0xFFFFFF);
    if (!c || !g_char_is_alive || !g_char_is_alive(c)) return 0;
    const uint32_t full = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(c) + ck3::character::ID);
    return ((full & 0xFFFFFF) == (id & 0xFFFFFF)) ? full : 0;
}

// Write the secondary characters, the title target and the requested send options into
// the TLS context, the way the interaction window does, then read the selection back.
// Runs inside the pipeline's protected region. Returns false with intent->rejection_reason
// set when a choice names something that does not exist or the engine does not take.
bool apply_interaction_window_choices(Intent* intent, void* tls_ctx, void* interaction) {
    using fn_set_char = void (*)(uintptr_t ctx, uint32_t char_ref, char eval);
    using fn_set_object = void (*)(uintptr_t ctx, const void* scope_value, char eval);
    using fn_select = void (*)(uintptr_t ctx, int index);
    const uintptr_t ctx = reinterpret_cast<uintptr_t>(tls_ctx);
    const uintptr_t c = reinterpret_cast<uintptr_t>(tls_ctx);

    struct SecondarySlot { uint32_t id; uintptr_t fn; const char* what; };
    const SecondarySlot slots[2] = {
        {intent->secondary_actor_id, ck3::funcs::INTERACTION_CTX_SET_SECONDARY_ACTOR, "secondary_actor"},
        {intent->secondary_recipient_id, ck3::funcs::INTERACTION_CTX_SET_SECONDARY_RECIPIENT, "secondary_recipient"},
    };
    for (const auto& s : slots) {
        if (s.id == 0) continue;
        const uint32_t full = living_full_char_id(s.id);
        if (!full) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "%s_not_living:%u", s.what, s.id & 0xFFFFFF);
            return false;
        }
        ADLOG("TLS_PIPELINE: %s = %u\n", s.what, full & 0xFFFFFF);
        reinterpret_cast<fn_set_char>(bridge::game_fn(s.fn))(ctx, full, 1);
    }

    if (intent->title_id > 0) {
        const uintptr_t tobj = lookup_title_obj(static_cast<uint32_t>(intent->title_id));
        if (!tobj || !is_mapped(tobj)) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "title_not_found:%d", intent->title_id);
            return false;
        }
        const uint32_t full_tid = safe_read<uint32_t>(tobj + ck3::title_obj::TITLE_ID);
        alignas(16) uint8_t sv[ck3::scope_value::SIZE] = {};
        *reinterpret_cast<uint16_t*>(sv + ck3::scope_value::TYPE) = ::scope_value_type::TITLE;   // global namespace in ck3_offsets.h
        *reinterpret_cast<uint64_t*>(sv + ck3::scope_value::VALUE) = full_tid;
        ADLOG("TLS_PIPELINE: target title = %u\n", full_tid & 0xFFFFFF);
        // The interaction's special data holds the picked title too (re_interactions.md
        // §3b): CRevokeTitle's one title, CGrantTitlesOffer's list. Written before the
        // target object's eval so the scope setup sees both.
        const uintptr_t data = safe_read<uintptr_t>(c + ck3::interaction_ctx::SCRIPTED_DATA_PTR);
        const uintptr_t vt = data && is_mapped(data) ? safe_read<uintptr_t>(data) : 0;
        if (vt && vt == g_base_addr + ck3::special_data::REVOKE_TITLE_VTABLE) {
            *reinterpret_cast<uint32_t*>(data + ck3::special_data::REVOKE_TITLE_ID) = full_tid;
            ADLOG("TLS_PIPELINE: revoke title data = %u\n", full_tid & 0xFFFFFF);
        } else if (vt && vt == g_base_addr + ck3::special_data::GRANT_TITLES_OFFER_VTABLE) {
            int32_t cap = safe_read<int32_t>(data + ck3::special_data::OFFER_CAPACITY);
            uintptr_t arr = safe_read<uintptr_t>(data + ck3::special_data::OFFER_DATA);
            if (!arr || cap < 1) {
                const uintptr_t alloc = safe_read<uintptr_t>(data + ck3::special_data::OFFER_ALLOCATOR);
                const uintptr_t alloc_vt = alloc ? safe_read<uintptr_t>(alloc) : 0;
                const uintptr_t fn = alloc_vt ? safe_read<uintptr_t>(alloc_vt + ck3::special_data::ALLOCATOR_VT_ALLOC) : 0;
                if (!fn || !is_mapped(fn)) {
                    snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                             "title_offer_allocator_unreadable");
                    return false;
                }
                arr = reinterpret_cast<uintptr_t (*)(uintptr_t, int, int)>(fn)(alloc, 1, 4);
                *reinterpret_cast<uintptr_t*>(data + ck3::special_data::OFFER_DATA) = arr;
                *reinterpret_cast<int32_t*>(data + ck3::special_data::OFFER_CAPACITY) = 1;
            }
            *reinterpret_cast<uint32_t*>(arr) = full_tid;
            *reinterpret_cast<int32_t*>(data + ck3::special_data::OFFER_COUNT) = 1;
            ADLOG("TLS_PIPELINE: grant offer = [%u]\n", full_tid & 0xFFFFFF);
        }
        reinterpret_cast<fn_set_object>(
            bridge::game_fn(ck3::funcs::INTERACTION_CTX_SET_TARGET_OBJECT))(ctx, sv, 1);
    }

    if (!intent->send_options.empty()) {
        const std::vector<std::string> names = interaction_send_option_names(interaction);
        std::string available;
        for (const auto& n : names) { if (!available.empty()) available += ","; available += n; }
        size_t start = 0;
        const std::string& req = intent->send_options;
        while (start <= req.size()) {
            size_t end = req.find(',', start);
            if (end == std::string::npos) end = req.size();
            const std::string want = req.substr(start, end - start);
            start = end + 1;
            if (want.empty()) continue;
            int idx = -1;
            for (size_t i = 0; i < names.size(); ++i) if (names[i] == want) { idx = static_cast<int>(i); break; }
            if (idx < 0) {
                snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                         "send_option_unknown:%s", want.c_str());
                intent->console_output = "this interaction's send options: " +
                                         (available.empty() ? std::string("none") : available);
                return false;
            }
            const int32_t sel_count = safe_read<int32_t>(c + ck3::interaction_ctx::OPTION_SELECTION_COUNT);
            if (idx >= sel_count) {
                snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                         "send_option_not_offered:%s", want.c_str());
                return false;
            }
            ADLOG("TLS_PIPELINE: select send option %s (index %d)\n", want.c_str(), idx);
            reinterpret_cast<fn_select>(bridge::game_fn(ck3::funcs::INTERACTION_SELECT_OPTION))(
                ctx, idx);
            const uintptr_t sel = safe_read<uintptr_t>(c + ck3::interaction_ctx::OPTION_SELECTION_PTR);
            if (!sel || !is_mapped(sel) || safe_read<uint8_t>(sel + idx) != 1) {
                snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                         "send_option_not_taken:%s", want.c_str());
                return false;
            }
        }
        // The selection as the engine holds it now (options that start enabled included).
        std::string applied;
        const int32_t sel_count = safe_read<int32_t>(c + ck3::interaction_ctx::OPTION_SELECTION_COUNT);
        const uintptr_t sel = safe_read<uintptr_t>(c + ck3::interaction_ctx::OPTION_SELECTION_PTR);
        for (int32_t i = 0; sel && i < sel_count && i < static_cast<int32_t>(names.size()); ++i) {
            if (safe_read<uint8_t>(sel + i) == 1) { if (!applied.empty()) applied += ","; applied += names[i]; }
        }
        intent->send_options_applied = applied;
    }
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
// F16 (l10 witness 4): what query_interaction_check reads from the send pipeline itself,
// past the game's validity (CTX_VALIDATE): the recipient's pre-answer
// (interaction_acceptance_decision in mode 1, the mode InteractionTrySend_V2 asks for
// the recipient: 0 yes, 1 maybe, 2 no) with the game's own pre-answer sentence, and
// the recipient's ai_accept score (FUN_06b43c70, ck3_offsets.h
// GET_AI_ACCEPT_NEGOT_SCORE: itype+0x1B90 in the recipient's scope, ctx+0x2DC;
// fixed point /100000, for a "maybe" interaction the chance in percent, 0x98967f
// being the decision's ceiling). Mode 0 reads the SECONDARY recipient (ctx+0x2E8):
// with none it is 100.0 for every target (run l10fx5-live-1). re_imprisonment.md.
struct InteractionCheck {
    bool scored = false;
    int answer = -1;
    int64_t accept_score = 0;
    std::string answer_text;
};

static bool execute_tls_pipeline_dispatch(Intent* intent, void* character, void* interaction,
                                          InteractionCheck* check = nullptr) {
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
    // Pass the ADDRESS of the struct directly.
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

        // Step 4.1: the war the interaction is about, as its target scope
        // (call_ally_interaction, join_war_interaction: `scope:target` is the war).
        // ctx+0x2F0 is the target scope object {u16 type; u64 value @+8}; the war
        // type is 0x10 (re/findings/re_war.md §2). SET_TARGET just cleared it, so
        // write it, then re-run SET_TARGET's own tail (scope setup + evaluate) so
        // the saved scopes carry `target` before CTX_VALIDATE reads them.
        if (intent->has_target_war) {
            uint8_t* ctx_bytes = reinterpret_cast<uint8_t*>(tls_ctx);
            *reinterpret_cast<uint16_t*>(ctx_bytes + ck3::interaction_ctx::TARGET_SCOPE_TYPE) =
                ck3::interaction_ctx::SCOPE_TYPE_WAR;
            *reinterpret_cast<uint64_t*>(ctx_bytes + ck3::interaction_ctx::TARGET_SCOPE_VALUE) =
                static_cast<uint64_t>(intent->target_war_id);
            auto scope_setup = reinterpret_cast<void (*)(uintptr_t*, char)>(
                bridge::game_fn(ck3::funcs::INTERACTION_CTX_SCOPE_SETUP));
            auto evaluate = reinterpret_cast<void (*)(uintptr_t*)>(
                bridge::game_fn(ck3::funcs::INTERACTION_POPULATE_OPTIONS));
            ADLOG("TLS_PIPELINE: target scope = war %u; SCOPE_SETUP + EVALUATE\n",
                  intent->target_war_id);
            scope_setup(reinterpret_cast<uintptr_t*>(tls_ctx), 1);
            evaluate(reinterpret_cast<uintptr_t*>(tls_ctx));
        }

        // Step 4.2 (lane l4): what the player sets in the interaction window besides the
        // recipient: the secondary characters, the title target and the send options.
        // SET_TARGET clears the title target, so this runs after it; it runs after the
        // war scope's re-evaluate too, so the option selection it reads back is final.
        if (!apply_interaction_window_choices(intent, tls_ctx, interaction)) {
            ADLOG("TLS_PIPELINE: window choices refused: %s\n", intent->rejection_reason);
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }

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
        const bool valid_result = ctx_validate(reinterpret_cast<uintptr_t*>(tls_ctx));
        if (!valid_result) {
            // The engine's own words for the refusal: the same check run through the
            // interaction menu's tooltip path (re_interactions.md §7), so the player reads
            // which condition failed (a law, prestige, an election, a truce ...).
            std::string engine_why;
            const uintptr_t with_text = bridge::game_fn(ck3::funcs::INTERACTION_CTX_VALIDATE_WITH_TEXT);
            if (is_mapped(with_text)) {
                reinterpret_cast<uint64_t (*)(uintptr_t*, std::string*)>(with_text)(
                    reinterpret_cast<uintptr_t*>(tls_ctx), &engine_why);
            }
            ADLOG("TLS_PIPELINE: CTX_VALIDATE returned 0 — interaction not valid for this target: %s\n",
                  engine_why.c_str());
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "interaction_not_valid_for_target");
            intent->console_output = engine_why.substr(0, 1024);
            g_in_protected_region = false;
            watchdog_disarm();
            return false;
        }
        ADLOG("TLS_PIPELINE: CTX_VALIDATE returned true (valid)\n");

        // Step 5.5 (query_interaction_check only): the recipient's pre-answer and the
        // ai_accept score, the two numbers a player reads in the interaction window
        // ("might succeed", the chance). The engine computes both again inside
        // InteractionTrySend_V2 and BUILD_SEND_CMD; neither sends anything.
        if (check) {
            using fn_decision = uint64_t (*)(uintptr_t*, int, std::string*);
            using fn_score = int64_t (*)(uintptr_t*);
            const uintptr_t decision_fn = bridge::game_fn(ck3::funcs::ACCEPTANCE_DECISION);
            const uintptr_t score_fn = bridge::game_fn(ck3::funcs::GET_AI_ACCEPT_NEGOT_SCORE);
            if (is_mapped(decision_fn) && is_mapped(score_fn)) {
                std::string text;
                check->answer = static_cast<int>(reinterpret_cast<fn_decision>(decision_fn)(
                    reinterpret_cast<uintptr_t*>(tls_ctx), 1, &text) & 0xFF);
                check->accept_score = reinterpret_cast<fn_score>(score_fn)(
                    reinterpret_cast<uintptr_t*>(tls_ctx));
                check->answer_text = clean_engine_text(text);
                check->scored = true;
                ADLOG("TLS_PIPELINE: CHECK answer=%d accept_score=%lld\n", check->answer,
                      (long long)check->accept_score);
            }
        }

        // Step 6: CheckAcceptance(tls_ctx, 0) → bool
        ADLOG("TLS_PIPELINE: CHECK_ACCEPTANCE\n");
        char acceptance = check_acceptance(reinterpret_cast<uintptr_t*>(tls_ctx), 0);
        if (!acceptance) {
            // Lane l4: InteractionTrySend is three engine checks in a row (decompile of
            // 0x6b43eb0): the send cascade (is_valid_showing_failures_only, can_send ...),
            // the recipient's answer (InteractionTrySend_V2: 2 = declines), the cost.
            // Name the one that said no; "the target would reject" was reported for
            // all three, which sent a player after the recipient's opinion when the
            // actor lacked dynasty renown.
            using fn_cascade = char (*)(uintptr_t*, char, char, intptr_t);
            using fn_answer = char (*)(uintptr_t*, intptr_t, intptr_t, intptr_t, intptr_t);
            const uintptr_t cascade_fn = bridge::game_fn(ck3::funcs::INTERACTION_TRY_SEND_CASCADE);
            const uintptr_t answer_fn = bridge::game_fn(ck3::funcs::INTERACTION_TRY_SEND_V2);
            // Where the two checks cannot be called (a build that does not map them), the
            // refusal keeps the undivided name.
            const char* why = "target_would_reject";
            std::string cascade_text;   // the cascade's failure text (its 4th argument, a std::string*)
            if (is_mapped(cascade_fn) && is_mapped(answer_fn)) {
                why = "send_cost_unaffordable";
                if (!reinterpret_cast<fn_cascade>(cascade_fn)(
                        reinterpret_cast<uintptr_t*>(tls_ctx), 1, 1,
                        reinterpret_cast<intptr_t>(&cascade_text))) {
                    why = "send_conditions_not_met";
                    intent->console_output = cascade_text.substr(0, 1024);
                } else if (reinterpret_cast<fn_answer>(answer_fn)(
                               reinterpret_cast<uintptr_t*>(tls_ctx), 0, 0, 0, 0) == 2) {
                    why = "target_would_reject";
                }
            }
            ADLOG("TLS_PIPELINE: CHECK_ACCEPTANCE returned 0 — %s\n", why);
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "%s", why);
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
    // query_interaction_check: the send's own checks all passed; nothing is built or sent.
    if (check) return true;

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
// Direct-dispatch list — which interactions take the TLS pipeline (v6)
// =============================================================================

// List of interactions dispatched through the TLS pipeline with an explicit
// target. These are hostile or scheme-like interactions where AI_INTERACTION_DECISION's
// target iteration / scoring is unnecessary or counterproductive (e.g., murder
// targets are not in the AI's normal ai_targets pool).
static bool is_direct_dispatch_interaction(const char* name) {
    // TLS pipeline (v6) dispatch list: interactions that work via CTX_INIT -> SET_TARGET ->
    // VALIDATE -> TRY_SEND -> BUILD_CMD -> DISPATCH with just actor+target.
    // revoke_title_interaction, grant_titles_interaction and buy_claim_interaction
    // additionally take a title as the interaction's target object (ctx+0x2F0, a 16-byte
    // scope value); apply_interaction_window_choices writes it from the intent's title_id.
    //
    // DEAD_FOR_WAR_PER_CKE58L17.1: declare_war_interaction removed from this list.
    // declare_war now dispatches via execute_war_intent (action_handlers.cpp:680).
    // Keeping it in the list would route declare_war through the structurally
    // dead TLS path for INTERACTION-type intents — which is the opposite of
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
        "revoke_title_interaction",       // title target at ctx+0x2F0 (see above)
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
        // title scope, so the plain actor+target TLS dispatch is sufficient — same
        // as imprison/execute above. No scope_ctx (#73) dependency.
        // Engine key verified: common/character_interactions/00_dynast_interactions.txt.
        "disinherit_interaction",
        // Lane l4: the succession levers. grant_titles carries the title as the
        // interaction's target object (ctx+0x2F0, written by apply_interaction_window_choices);
        // designate_heir is actor + child.
        "grant_titles_interaction",
        "designate_heir_interaction",
        // Peaceful interactions (must use TLS — v5 AI_INTERACTION_DECISION returns false
        // when actor is the player character, since AI decision functions skip player chars)
        "gift_interaction",
        "sway_interaction",
        "befriend_interaction",
        // A2/A3: these were falling through to v5 AI_INTERACTION_DECISION, which walks
        // the actor's OWN candidate pool and ignores the caller's explicit target_id —
        // so they always returned ai_no_valid_target even when discover(acceptance)
        // succeeded on the same target. Route them through the TLS pipeline so the
        // explicit target is honored. buy_claim takes the claim's title as its target
        // object (ctx+0x2F0, see above).
        "elope_interaction",
        "buy_claim_interaction",
        // Peace (lane l5, F8): the three end-war interactions, from either war
        // leader's seat. The engine derives scope:war from ARE_AT_WAR(actor,
        // recipient) (CEndWar*InteractionData, re/findings/re_war.md §1), so the
        // explicit enemy war leader is all they need. Through v5 they were gated
        // by the AI's own ai_potential/ai_will_do (an AI only offers terms at war
        // score >= 100, for instance), which no player is held to.
        "end_war_attacker_victory_interaction",
        "end_war_attacker_defeat_interaction",
        "end_war_attacker_white_peace_interaction",
        // Offer to join an ally's war (the ally's seat); the war rides as the
        // target scope like call_ally_interaction's.
        "join_war_interaction",
        nullptr
    };
    for (const char** p = direct_list; *p; ++p) {
        if (strcmp(name, *p) == 0) return true;
    }
    return false;
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
    ADLOG("  force_bypass=%d queued_tick=%lu ttl_ticks=%u request_id='%s'\n",
          intent->force_bypass,
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
    //
    // DEAD_FOR_WAR_PER_CKE58L17.1: declare_war no longer reaches execute_direct_dispatch.
    // WAR_DECLARE intents go through drain_and_execute_intents → execute_war_intent.
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
            // Lane l4: the engine's own verdicts and the window choices the bridge could
            // not apply are authoritative too; the v5 fallback would send the
            // interaction without them (or to another target).
            bool legitimate_game_reject =
                strcmp(rej, "interaction_not_valid_for_target") == 0 ||
                strcmp(rej, "target_would_reject") == 0 ||
                strcmp(rej, "send_conditions_not_met") == 0 ||
                strcmp(rej, "send_cost_unaffordable") == 0 ||
                strncmp(rej, "send_option_", 12) == 0 ||
                strncmp(rej, "secondary_actor_not_living", 26) == 0 ||
                strncmp(rej, "secondary_recipient_not_living", 30) == 0 ||
                strncmp(rej, "title_not_found", 15) == 0 ||
                strcmp(rej, "title_offer_allocator_unreadable") == 0;
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
// query_interaction_check — the send pipeline's own checks, nothing sent (F16)
// =============================================================================
//
// One INTERACTION intent whose console_command is "check" or "check:<id>,<id>,...":
// for each target (target_id, or the listed candidates) execute_tls_pipeline_dispatch
// runs exactly the steps a send runs (CTX_INIT .. the window's choices, the game's
// validity CTX_VALIDATE with its sentence, InteractionTrySend: the send conditions,
// the recipient's refusal, the cost) and stops before BUILD_SEND_CMD. Beside it the
// recipient's pre-answer and the ai_accept score (InteractionCheck). The result is a
// JSON object in console_output: {"interaction", "actor_id", "results": [...]}.
static constexpr size_t kCheckMaxTargets = 64;

static void execute_interaction_check_intent(Intent* real, void* character, void* interaction) {
    std::vector<uint32_t> targets;
    const std::string& cmd = real->console_command;
    if (cmd.size() > 6 && cmd.compare(0, 6, "check:") == 0) {
        size_t i = 6;
        while (i < cmd.size() && targets.size() < kCheckMaxTargets) {
            size_t j = cmd.find(',', i);
            if (j == std::string::npos) j = cmd.size();
            const unsigned long v = strtoul(cmd.substr(i, j - i).c_str(), nullptr, 10);
            if (v > 0) targets.push_back(static_cast<uint32_t>(v) & 0xFFFFFF);
            i = j + 1;
        }
    } else if (real->target_id) {
        targets.push_back(real->target_id & 0xFFFFFF);
    }
    real->consumed = true;
    if (targets.empty()) {
        real->rejected = true;
        snprintf(real->rejection_reason, sizeof real->rejection_reason, "target_id_required");
        return;
    }
    if (!is_direct_dispatch_interaction(real->interaction_name.c_str())) {
        // Only the TLS pipeline sends to the named target; the check is its steps.
        real->rejected = true;
        snprintf(real->rejection_reason, sizeof real->rejection_reason, "interaction_not_checkable");
        return;
    }
    ResponseBuilder r;
    r.begin_object();
    r.key("interaction"); r.value_string(real->interaction_name.c_str());
    r.key("actor_id"); r.value_uint(real->actor_id & 0xFFFFFF);
    r.key("results"); r.begin_array();
    for (uint32_t t : targets) {
        Intent one = *real;
        one.target_id = t;
        one.rejection_reason[0] = '\0';
        one.console_output.clear();
        r.begin_object();
        r.key("target_id"); r.value_uint(t);
        void* tc = lookup_character(t);
        if (!tc || is_sentinel(tc)) {
            r.key("sendable"); r.value_bool(false);
            r.key("refused_by"); r.value_string("target_not_found");
            r.end_object();
            continue;
        }
        InteractionCheck chk;
        const bool sendable = execute_tls_pipeline_dispatch(&one, character, interaction, &chk);
        r.key("sendable"); r.value_bool(sendable);
        if (!sendable) {
            r.key("refused_by");
            r.value_string(one.rejection_reason[0] ? one.rejection_reason : "unknown");
            const std::string why = clean_engine_text(one.console_output);
            if (!why.empty()) { r.key("engine_reason"); r.value_string(why.c_str(), why.size()); }
        }
        if (chk.scored) {
            static const char* kAnswers[] = {"yes", "maybe", "no"};
            r.key("answer");
            r.value_string(chk.answer >= 0 && chk.answer <= 2 ? kAnswers[chk.answer] : "unknown");
            r.key("accept_score"); r.value_double(static_cast<double>(chk.accept_score) / 100000.0);
            if (!chk.answer_text.empty()) {
                r.key("answer_text"); r.value_string(chk.answer_text.c_str(), chk.answer_text.size());
            }
        }
        r.end_object();
    }
    r.end_array();
    r.end_object();
    real->console_output = r.take();
    real->executed = true;
    real->rejected = false;
    ADLOG("CHECK_INTERACTION: %s actor=%u targets=%zu\n", real->interaction_name.c_str(),
          real->actor_id & 0xFFFFFF, targets.size());
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
    const uintptr_t vtable = safe_read<uintptr_t>(interaction_addr);
    const uintptr_t is_valid_addr =
        vtable ? safe_read<uintptr_t>(vtable + ck3::interaction_def::VTABLE_IS_VALID) : 0;
    if (!is_valid_addr || !is_mapped(is_valid_addr)) {
        snprintf(g_acceptance_query.error, sizeof(g_acceptance_query.error),
                 "interaction_vtable_unreadable");
        g_acceptance_query.would_accept = false;
        g_acceptance_query.state.store(3, std::memory_order_release);
        ADLOG("ACCEPTANCE_QUERY: interaction '%s' vtable unreadable\n", interaction_type);
        return;
    }
    auto is_valid_fn = reinterpret_cast<bool(*)(void*)>(is_valid_addr);

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
    // 242-260) but never over-rejects. The doctrine fast-path is not mirrored.
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
    // group, disabled/stage2/no-members group flags) stay hard drops — they
    // filter CBs the inner check can't distinguish. kind_relation_mode is
    // diagnostic-only too (lane l5, see its site below).
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
                        int32_t   t_count = safe_read<int32_t>(tlist + pdx_array::COUNT);
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
        // VALID_TO_START_*) need a CB-rooted scope context and are not
        // evaluated here.
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
                // lane l5 (F8, re/findings/re_war.md §7): this gate is the AI's own
                // candidate filter in ai_cb_evaluate_all, keyed on the CB's TARGET
                // mode (maybe_ai_cb_title_relation_mode reads cb_type+0x738) and on
                // two engine lists the bridge approximates as alliance/truce. It
                // says which CBs an AI considers, not which a ruler may declare:
                // live867 l5-r2 dropped invasion_war, claim_cb and county_conquest_cb
                // against every unallied, untruced target. The inner
                // CB_VALIDITY_CHECK is the verdict, as for the #114 pre-drops;
                // BRIDGE_DISTRUST_INNER_CB_VALIDITY=1 restores the drop.
                if (!passes && !distrust_inner_cb_validity) {
                    strict_dropped_kind_relation++;
                    ADLOG("CB_QUERY: kind_relation_mode would drop cb=%s mode=%d "
                          "in_alliance=%u in_truce=%u; the inner check decides\n",
                          cb_name, relation_mode, i2_in_alliance, i2_in_truce);
                } else if (!passes) {
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
// cb_query_acquire_slot — claim the single-flight g_cb_query slot
// =============================================================================
//
// L13 FF1-B/FF2: every g_cb_query user runs on the GAME thread (the slot
// submitters are game-thread reads and service their own slot there; the drain
// is the only other user), so the slot has one serial owner and the claim needs
// no CAS and no busy-wait: a slot that is pending (1) or being serviced (4) on
// this thread cannot clear by waiting, and the callers service a pending one
// first (service_slots_if_game_thread). Off the game thread there is no serial
// owner, so the claim refuses (the caller answers cb_query_busy) rather than
// race one. The pre-L13 busy-wait, stale-slot reclaim and dead-servicer reclaim
// (D8) existed for an IPC-thread claimant racing the drain and are deleted.
//
// Returns true and leaves the slot reset to 0 (empty, ready to submit).
static bool cb_query_acquire_slot() {
    if (!bridge::on_game_thread()) return false;
    const int st = g_cb_query.state.load(std::memory_order_acquire);
    if (st == 0 || st == 2 || st == 3) {
        if (st != 0) {
            g_cb_query.results.clear();
            g_cb_query.state.store(0, std::memory_order_release);
        }
        return true;
    }
    return false;
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
    if (g_cb_query.state.load(std::memory_order_acquire) == 1) service_slots_if_game_thread();
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
    service_slots_if_game_thread();

    // Poll for game-thread completion. Same 50ms cadence as do_query_valid_cbs.
    if (timeout_ms < 50) timeout_ms = 50;
    int waited = 0;
    while (waited < timeout_ms) {
        slot_poll_wait();
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
    // actively servicing (state 4) we leave it (on the game thread, where every
    // slot user runs since L13, a state-4 slot is this thread's own service).
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
//             Not mirrored: faith/realm/government/era cross-checks
//             (FUN_067539c0/0674adf0/066e4fb0) and truce/alliance flag
//             construction (FUN_0678b570/GetTruceList) — see Open
//             Questions in re/findings/re_a1_path_a_re_20260425.md.
//
//   Stage 2 — group-level outer-trigger flag filter (process_cb_query,
//             game thread, when CBQuery::strict_mode=true). Decomp lines
//             357-360: per-group requires DISABLED_FLAG==0 AND
//             STAGE2_FLAG==0 AND members count > 0. The script trigger at
//             group+0x48 is not evaluated (it needs a scope_ctx).
//
// Stage 4 (the score evaluator) is not mirrored.
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
    if (g_cb_query.state.load(std::memory_order_acquire) == 1) service_slots_if_game_thread();
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
    service_slots_if_game_thread();

    if (timeout_ms < 50) timeout_ms = 50;
    int waited = 0;
    while (waited < timeout_ms) {
        slot_poll_wait();
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
// valid_to_start) are not evaluated: they need a CB-rooted scope_ctx, which
// this probe does not build. outer_reason therefore carries only the Stage 1
// and Stage 2 reasons.
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
    // (ck-#95). L13: on the game thread a slot an IPC caller left pending is
    // serviced here first — this thread is the one that would service it, so
    // waiting for it to clear could only time out.
    if (g_cb_query.state.load(std::memory_order_acquire) == 1) service_slots_if_game_thread();
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

    // On the game thread nobody else will service the slot: do it here.
    service_slots_if_game_thread();

    if (timeout_ms < 50) timeout_ms = 50;
    int waited = 0;
    while (waited < timeout_ms) {
        slot_poll_wait();
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
    // L20-D: destroy with the function the engine's own EVAL_WITH_FAILURES calls, or
    // refuse the extraction (diag_dtor.h); never jump to an unwitnessed address.
    const uintptr_t dtor_addr = bridge::resolve_diag_dtor(g_base_addr, is_mapped_range);
    if (!dtor_addr) {
        static bool s_logged = false;
        if (!s_logged && g_logfile) {
            s_logged = true;
            fprintf(g_logfile, "[CK3-Bridge] loud_extract: diag dtor not witnessed at EVAL_WITH_FAILURES+0x%llx; "
                    "failure-key extraction refused\n",
                    (unsigned long long)ck3::trigger_eval_funcs::EVAL_WITH_FAILURES_DTOR_CALL);
        }
        return false;
    }
    auto fn_destroy = reinterpret_cast<fn_diag_dtor>(dtor_addr);
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
// drain_and_execute_intents — Serial intent execution from on_tick
// =============================================================================
//
// Called from on_tick() on the game thread, in the serial section BEFORE
// parallel AI workers launch. This replaces the old hooked_ai_interaction_decision
// approach which fired 10,000+ times per tick across parallel threads.
//
// This is safe: single-threaded, TLS initialized, no contention.

// The game-thread query slots, serviced: each slot an IPC-side or game-thread
// handler submitted (state 1) is evaluated here. Called by the drain every pass,
// and by a handler that submits a slot while it is itself ON the game thread
// (L13 D1: the slot submitters are game-thread reads now), which is the one
// thread that could ever service it — so it services it at once instead of
// polling for a drain that cannot run until it returns.
void service_query_slots() {
    // Process any pending acceptance query (deferred from IPC thread)
    if (g_acceptance_query.state.load(std::memory_order_acquire) == 1) {
        process_acceptance_query();
    }

    // Process any pending CB validity query (deferred from IPC thread).
    // H1 (re_warplanner_lifecycle.md §1.4): service up to kCbDrainPerTick CB
    // submissions per pass instead of exactly one (historically for a second
    // caller resubmitting inside one pulse window; the pulse is gone, and the
    // bound is kept as the per-pass cap). process_cb_query CAS-guards 1→4 and
    // returns at once if not pending.
    static constexpr int kCbDrainPerTick = 2;
    for (int i = 0; i < kCbDrainPerTick &&
         g_cb_query.state.load(std::memory_order_acquire) == 1; ++i) {
        process_cb_query();
    }

    // Process any pending trigger evaluation query (deferred from IPC thread)
    if (g_trigger_query.state.load(std::memory_order_acquire) == 1) {
        process_trigger_query();
    }

    // #64: Process any pending annotated decision-eval query (deferred from IPC thread;
    // the trigger evals must run here on the game thread where the watchdog fires).
    if (g_decision_eval_query.state.load(std::memory_order_acquire) == 1) {
        process_decision_eval_query();
    }

    // Faction options (query_joinable_factions): the engine's create/join/leave/
    // press verdicts are trigger evaluations, so they run here on the game thread.
    if (faction_query_pending()) {
        process_faction_query();
    }

    // Process any pending interaction-cascade probe (ck-e58l.8, deferred from IPC thread)
    if (cascade_query_pending()) {
        process_cascade_query();
    }

}

void service_slots_if_game_thread() {
    if (!bridge::on_game_thread()) return;
    // The process_* functions install their own fault recovery points; the
    // caller's are saved and put back around them.
    sigjmp_buf saved_point;
    memcpy(&saved_point, &g_recovery_point, sizeof(sigjmp_buf));
    const bool saved_protected = g_in_protected_region;
    service_query_slots();
    memcpy(&g_recovery_point, &saved_point, sizeof(sigjmp_buf));
    g_in_protected_region = saved_protected;
}

void slot_poll_wait() {
    // Off the game thread the drain services the slot on its next pass, so the
    // poll waits; on it the slot was serviced at submit (service_slots_if_game_thread)
    // and waiting would only stall every engine command behind the session mutex.
    if (bridge::on_game_thread()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void drain_and_execute_intents() {
    service_query_slots();

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

    for (const auto& queued : snapshot) {
        // L13 FF3 (witness X3): claim the intent under the queue's lock (it is
        // consumed + in flight from here) and execute on our own copy; finish()
        // hands the verdict back under the lock. No pointer into the queue is
        // held across the execution, so an enqueue, cancel or clear on the IPC
        // thread cannot move the record, and no reader copies one being written.
        Intent work;
        if (!g_intent_queue.claim(queued.seq, &work)) continue;   // retired/cancelled/taken
        Intent* real = &work;

        // M1: install the owning session context for the duration of this intent.
        // The guard restores the previous context when the iteration ends. A
        // crash's siglongjmp skips its destructor: the drain's recovery
        // (recover_drain_crash, bridge_init.cpp) restores it instead.
        bridge::ClientContext intent_ctx;
        std::unique_ptr<bridge::ClientContextGuard> intent_ctx_guard;
        if (!real->session_id.empty()) {
            bridge::Session* s = bridge::g_session_registry.find(real->session_id);
            if (s) {
                intent_ctx.session = s;
                intent_ctx.action_type = real->interaction_name.c_str();
                intent_ctx_guard = std::make_unique<bridge::ClientContextGuard>(&intent_ctx);
            }
        }

        // 2026-06-09: record the in-flight intent so the outer recovery handler
        // can convert a game-thread crash here into a structured rejection
        // instead of a silent IPC timeout. By seq (FF4, m10). Cleared after
        // finish().
        g_inflight_intent_seq = work.seq;

        switch (real->type) {
        case IntentType::INTERACTION: {
            void* character = lookup_character(real->actor_id);
            if (!character) {
                ADLOG("DRAIN_INTENTS: actor %u not found — rejecting %s\n",
                      real->actor_id & 0xFFFFFF, real->interaction_name.c_str());
                real->rejected = true;
                snprintf(real->rejection_reason, sizeof(real->rejection_reason),
                         "actor_not_found");
                real->consumed = true;
                break;
            }

            void* interaction = lookup_interaction(real->interaction_name.c_str());
            if (!interaction) {
                ADLOG("DRAIN_INTENTS: interaction '%s' not found — rejecting\n",
                      real->interaction_name.c_str());
                real->rejected = true;
                snprintf(real->rejection_reason, sizeof(real->rejection_reason),
                         "interaction_not_found");
                real->consumed = true;
                break;
            }

            ADLOG("DRAIN_INTENTS: executing INTERACTION %s actor=%u target=%u\n",
                  real->interaction_name.c_str(),
                  real->actor_id & 0xFFFFFF,
                  real->target_id & 0xFFFFFF);

            if (real->console_command.compare(0, 5, "check") == 0) {
                execute_interaction_check_intent(real, character, interaction);
                break;
            }

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
            break;
        }

        case IntentType::WAR_DECLARE: {
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
            ADLOG("DRAIN_INTENTS: executing DECISION actor=%u decision='%s'\n",
                  real->actor_id & 0xFFFFFF, real->interaction_name.c_str());
            execute_decision_intent(real);
            break;
        }

        case IntentType::WAR_CREATE_CORE_DIRECT: {
            ADLOG("DRAIN_INTENTS: executing WAR_CREATE_CORE_DIRECT actor=%u target=%u cb='%s' title=%d claimant=%u\n",
                  real->actor_id & 0xFFFFFF, real->target_id & 0xFFFFFF,
                  real->interaction_name.c_str(), real->title_id,
                  real->claimant_id & 0xFFFFFF);
            execute_war_create_core_intent(real);
            break;
        }

        case IntentType::FACTION: {
            ADLOG("DRAIN_INTENTS: executing FACTION actor=%u liege=%u type='%s' action='%s'\n",
                  real->actor_id & 0xFFFFFF, real->target_id & 0xFFFFFF,
                  real->interaction_name.c_str(), real->console_command.c_str());
            execute_faction_intent(real);
            break;
        }

        case IntentType::CREATE_TITLE: {
            // #111 (re_create_title_action.md): LEGITIMATE de-jure create-title via
            // CCreateTitleCommand + CanExecute gate + GAME_COMMAND_DISPATCH(0xe).
            // Game-thread serial section only — under watchdog+sigsetjmp+fake-RNG.
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
            // console_command picks the sub-operation: "cancel" => RemoveActiveScheme on an
            // owned scheme; "check:..." => CanStartScheme only (scheme_engine.cpp);
            // "start" => CanStartScheme, then CreateActiveScheme.
            if (real->console_command.compare(0, 5, "check") == 0) {
                ADLOG("DRAIN_INTENTS: executing SCHEME CHECK owner=%u scheme='%s' target=%u\n",
                      real->actor_id & 0xFFFFFF, real->interaction_name.c_str(),
                      real->target_id & 0xFFFFFF);
                execute_scheme_check_intent(real);
                break;
            }
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
        // REMOVED: IntentType::MOVE_ARMY case deleted per design (agent does not
        // drive army micro).
        }

        // Intent finished without crashing — hand the verdict back and clear
        // the in-flight marker.
        g_intent_queue.finish(work);
        g_inflight_intent_seq = 0;
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
        ADLOG("WARNING: TLS offset discovery failed, using ck3_offsets.h "
              "(guard=0x%lx, context=0x%lx)\n",
              (unsigned long)g_tls_guard_offset, (unsigned long)g_tls_context_offset);
    }

    ADLOG("TLS offsets initialized (guard=0x%lx, context=0x%lx, source=%s). "
          "Intents execute from the game-thread drain.\n",
          (unsigned long)g_tls_guard_offset, (unsigned long)g_tls_context_offset,
          g_tls_offsets_discovered ? "discovered" : "ck3_offsets.h");
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

    g_intent_queue.enqueue(std::move(intent));

    ADLOG("Intent queued (via queue_intent): %s actor=%u target=%u hash=0x%08x ttl=%u title_id=%d cb_idx=%d claimant=%u bypass=%s\n",
          interaction_type, actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
          hash, ttl, title_id, cb_index, claimant_id & 0xFFFFFF,
          force_bypass ? "YES" : "NO");
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
