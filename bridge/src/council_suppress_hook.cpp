// council_suppress_hook.cpp — Per-ruler council-AI suppression (L20 stickiness).
//
// PROBLEM: the native AI re-assigns councillor tasks every AI tick, overwriting
// the bridge's set_council_task. The per-char AI dispatcher FUN_04d89360 runs
//     if (DAT_0bfa5c83 != 0) FUN_04dca2a0(char_ptr, param2);
// and FUN_04dca2a0 (AI_COUNCIL_TASKS_PER_RULER) loops the ruler's council
// positions and re-assigns each task through the same store path the bridge
// uses. The global toggle DAT_0bfa5c83 works but is GLOBAL — it freezes EVERY
// realm's council. There is no per-task lock flag.
//
// FIX: install an inline hook on FUN_04dca2a0. The hook reads the char id from
// char_ptr; if that id is in g_council_locked (rulers whose council the agent
// has deliberately set), it RETURNS WITHOUT calling the original — suppressing
// AI council re-assignment for that ruler only. Everyone else falls through to
// the original trampoline unchanged.
//
// This hook ONLY skips the council-AI routine. It NEVER writes council memory
// and does not touch the FUN_05cc8660 store path. It fails OPEN (calls the
// original) on any unexpected state, so it can never wedge the game thread.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <shared_mutex>
#include <unordered_set>
#include <atomic>

namespace bridge {

// =============================================================================
// Locked-ruler set
// =============================================================================
// Rulers whose council the agent has deliberately set via set_council_task.
// While a ruler id is in this set, the per-ruler council-AI routine is skipped
// so the bridge-set task sticks. Default: empty → behavior unchanged for all
// rulers (the AI keeps full control until the agent locks a specific ruler).
//
// g_council_locked_nonempty is an atomic fast-path flag: the hook runs on the
// game thread for every ruler every AI tick, so when the set is empty we want
// to fall through to the original with ZERO locking / memory-read overhead.

static std::shared_mutex            g_council_locked_mutex;
static std::unordered_set<uint32_t> g_council_locked CK3_EARLY_INIT;
static std::atomic<bool>            g_council_locked_nonempty{false};

// Trampoline to the original FUN_04dca2a0(CCharacter* char_ptr, undefined4 p2).
static void* g_orig_ai_council_tasks = nullptr;

using fn_ai_council_tasks = void(*)(void* char_ptr, uint32_t p2);

// =============================================================================
// Public lock API (called from action_handlers.cpp / dispatch_router.cpp)
// =============================================================================

void council_lock_add(uint32_t char_id) {
    if (!char_id || char_id == 0xFFFFFF) return;
    std::unique_lock<std::shared_mutex> lk(g_council_locked_mutex);
    g_council_locked.insert(char_id);
    g_council_locked_nonempty.store(true, std::memory_order_relaxed);
}

void council_lock_remove(uint32_t char_id) {
    std::unique_lock<std::shared_mutex> lk(g_council_locked_mutex);
    g_council_locked.erase(char_id);
    g_council_locked_nonempty.store(!g_council_locked.empty(),
                                    std::memory_order_relaxed);
}

// =============================================================================
// The hook
// =============================================================================

static void on_ai_council_tasks(void* char_ptr, uint32_t p2) {
    auto orig = reinterpret_cast<fn_ai_council_tasks>(g_orig_ai_council_tasks);

    // FAST PATH / FAIL-OPEN: no rulers locked → run the original immediately,
    // with no locking and no memory reads. This is the overwhelmingly common
    // case (the agent only locks a ruler after deliberately setting a task).
    if (!g_council_locked_nonempty.load(std::memory_order_relaxed)) {
        orig(char_ptr, p2);
        return;
    }

    // FAIL-OPEN: bogus char_ptr → run the original (never skip on doubt).
    uintptr_t c = reinterpret_cast<uintptr_t>(char_ptr);
    if (!is_mapped_range(c + ck3::character::ID, sizeof(uint32_t))) {
        orig(char_ptr, p2);
        return;
    }

    // char_ptr is a live game-thread pointer here; a direct, bounds-checked
    // read is safe and avoids a process_vm_readv syscall on every AI tick.
    uint32_t char_id = (*reinterpret_cast<volatile uint32_t*>(c + ck3::character::ID)) & 0xFFFFFF;

    bool locked = false;
    {
        std::shared_lock<std::shared_mutex> lk(g_council_locked_mutex);
        locked = (g_council_locked.find(char_id) != g_council_locked.end());
    }

    if (locked) {
        // FIXPLAN B1 (SAFE death-guard half): a controlled ruler's DEATH does
        // not auto-release its council lock (mirrors the dead-ruler
        // control-handoff lesson). A stale lock would suppress the SUCCESSOR's
        // (or whoever next holds that char-id slot's) council AI forever. The
        // engine still iterates a dying ruler here, so we get a clean
        // game-thread pointer to test liveness. CHAR_DEATH_DATE (0x1c8) is a
        // ptr that is null while alive and non-null once dead. If the locked
        // char is dead, auto-release the lock and FALL THROUGH to the original
        // (fail-open). Bounds-checked; never derefs an unmapped slot.
        if (is_mapped_range(c + ck3::character::DEATH_DATE, sizeof(uintptr_t))) {
            uintptr_t death_date =
                *reinterpret_cast<volatile uintptr_t*>(c + ck3::character::DEATH_DATE);
            if (death_date != 0) {
                // Dead ruler still holding a lock → drop it and run the native
                // routine. The agent re-locks the live successor explicitly.
                council_lock_remove(char_id);
                if (g_trace) {
                    ADLOG("council_suppress: released stale lock for DEAD ruler %u "
                          "(death-guard) — falling through to native AI\n", char_id);
                }
                orig(char_ptr, p2);
                return;
            }
        }

        // SUPPRESS: skip the native council-task re-assignment for THIS ruler.
        // We return without calling the original — no council memory is read or
        // written by this hook. The bridge-set task stays in place.
        if (g_trace) {
            ADLOG("council_suppress: skipped AI council re-assign for ruler %u\n",
                  char_id);
        }
        return;
    }

    // Not locked → unchanged behavior: run the native council-AI routine.
    orig(char_ptr, p2);
}

// =============================================================================
// Initialization (called from bridge_init, next to init_council_task_hook)
// =============================================================================

void init_council_suppress_hook() {
    uintptr_t target = g_base_addr + ck3::funcs::AI_COUNCIL_TASKS_PER_RULER;
    g_orig_ai_council_tasks =
        install_hook(target, reinterpret_cast<void*>(on_ai_council_tasks));

    if (g_orig_ai_council_tasks) {
        ADLOG("council_suppress_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_ai_council_tasks);
    } else {
        ADLOG("council_suppress_hook: FAILED to install at 0x%lx "
              "(per-ruler council lock disabled; global behavior unchanged)\n",
              target);
    }
}

} // namespace bridge
