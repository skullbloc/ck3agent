// Event detection — finds pending events targeting the controlled character.
//
// From on_tick (bridge_init.cpp) one of two paths runs per daily tick:
// `poll_and_notify_events()` when an agent is connected (pushes each pending
// event to it once), or `auto_respond_pending_events()` when none is (the
// no-agent fallback: releases the game's own event pause so the simulation is
// not held on a popup).
//
// L16 (ruling R3): the bridge no longer answers an event on request. The
// respond_event action (which dispatched CSelectEventOptionCommand), the
// query_event_probabilities / event_probabilities read and the
// set_auto_respond_events switch had no caller outside their own tests and are
// deleted; the command's layout stays recorded in re/findings/re_event_pipeline.md
// and re/offsets/ck3_offsets.h (SELECT_EVENT_OPTION_CMD_VTABLE*).
//
// The legacy AI_EVENT_OPTION_EVAL hook was never installed; its RE notes are
// kept below as provenance.

#include "bridge.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "ck3_offsets.h"
#include "world_epoch.h"   // ck-hxh7

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csetjmp>
#include <csignal>
#include <atomic>
#include <mutex>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>

namespace bridge {

// SIGSEGV recovery — defined in bridge_init.cpp (inside namespace bridge)
extern thread_local sigjmp_buf g_recovery_point;
extern thread_local bool g_in_protected_region;

// Pointer validation using /proc/self/maps (refreshed once per tick).
// Replaces the old range-only check. Uses the shared g_valid_ranges table
// from bridge.h for actual mapped-region verification.
static inline bool safe_ptr(uintptr_t p) {
    if (p < 0x10000 || p > 0x7FFFFFFFFFFF) return false;
    {
        std::shared_lock<std::shared_mutex> lock(g_ranges_mutex);
        if (!g_valid_ranges.empty()) {
            for (const auto& r : g_valid_ranges) {
                if (p >= r.start && p < r.end) return true;
            }
            return false;
        }
    }
    return true;  // fallback before first tick
}

// Trampoline for the original AI event option evaluator
static void* g_orig_ai_event_option_eval = nullptr;

// =============================================================================
// Sent Event Tracking (avoid re-sending same event every tick)
// =============================================================================

static std::mutex g_sent_events_mutex;
static std::unordered_set<uint32_t> g_sent_event_instance_ids CK3_EARLY_INIT;

// Mark an event as sent to the agent
static void mark_event_sent(uint32_t instance_id) {
    std::lock_guard<std::mutex> lock(g_sent_events_mutex);
    g_sent_event_instance_ids.insert(instance_id);
}

// Check if an event was already sent
static bool was_event_sent(uint32_t instance_id) {
    std::lock_guard<std::mutex> lock(g_sent_events_mutex);
    return g_sent_event_instance_ids.count(instance_id) > 0;
}

// Clear all sent events (e.g., on disconnect)
void clear_all_sent_events() {
    std::lock_guard<std::mutex> lock(g_sent_events_mutex);
    g_sent_event_instance_ids.clear();
}

// ck-hxh7: this set is keyed by EVENT INSTANCE ID, which is reused across saves.
// Its lifetime is the loaded WORLD, not the process — a reused instance id in a
// newly loaded save was permanently suppressed and never forwarded to the agent,
// because clear_all_sent_events() had no caller anywhere. Register it with the
// world-epoch detector. A spurious clear only risks re-notifying an event.
[[maybe_unused]] static const WorldResetRegistrar g_sent_events_world_scoped CK3_EARLY_INIT([] {
    register_world_reset("sent_event_instance_ids", &clear_all_sent_events);
});

// =============================================================================
// Game Pause/Speed Control
// =============================================================================

// Read the CJominiIdler singleton pointer (returns nullptr if not available)
static void* get_idler() {
    return reinterpret_cast<void*>(safe_read_offset<uintptr_t>(ck3::globals::IDLER_SINGLETON));
}

// Read the game state root pointer (returns nullptr if not available)
static void* get_game_state_root() {
    return reinterpret_cast<void*>(safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT));
}

// PAUSE_SET signature: void fn(CJominiIdler* idler, int pause_val, int player_id)
// pause_val: 0 = unpause, 1 = pause
// NOTE: Only safe to call from the game's main thread (on_tick context).
// For cross-thread use, fall back to raw byte writes.
typedef void (*pause_set_fn)(void* idler, int pause_val, int player_id);

// Delayed unpause using PAUSE_SET for proper state propagation after game load.
// Uses a flag checked in on_tick (game thread safe) instead of calling from a timer thread.
static std::atomic<int> g_delayed_unpause_ticks{0};
// L13 (witness m1): the deferred PAUSE_SET(0) repeat runs on the game thread
// every pass while clock_write runs on whichever thread asked. One mutex over
// both, so a pause issued between the repeat's check and its call cannot be
// undone by the repeat: the pause cancels it under the same lock.
// FAULT-SAFE (witness FF2 m-3): both holders install their OWN recovery point
// around the primitives (ClockFaultGuard), so a fault inside a primitive returns
// into the holder's frame and the lock is released there; the caller's
// g_recovery_point / g_in_protected_region are saved and put back, so the
// primitives' own sigsetjmp no longer leaves the caller's point aimed at a dead
// frame. The repeat only try-locks (retried next pass); clock_write — which can
// run on the game thread too (auto-respond) — waits at most 2 s, then writes
// without the lock, logged.
static std::timed_mutex g_clock_mutex;

namespace {
// Saves the caller's fault recovery state and puts it back on destruction.
struct SavedRecovery {
    sigjmp_buf point;
    bool in_region;
    SavedRecovery() : in_region(g_in_protected_region) {
        memcpy(&point, &g_recovery_point, sizeof(sigjmp_buf));
    }
    ~SavedRecovery() {
        memcpy(&g_recovery_point, &point, sizeof(sigjmp_buf));
        g_in_protected_region = in_region;
    }
};
}  // namespace

static void pause_primitive() {
    // Every read before any write, and no write after a read that faulted: an
    // address or id from a faulted read is not one. The fault stays counted
    // (never cleared here), so the handler that asked answers
    // memory_access_fault (dispatch_router.cpp case_pause_game), and the
    // refusal changed nothing (witness #10: the deferred-unpause store used to
    // come first).
    const int faults = safe_read_fault_count();
    void* idler = get_idler();
    if (!idler) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_pause: CJominiIdler singleton is null\n");
        return;
    }
#ifdef _WIN32
    const int player_id = safe_read<int32_t>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PLAYER_ID);
#endif
    if (safe_read_fault_count() != faults) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_pause: a read faulted; nothing written\n");
        return;
    }
    // A pause cancels the PAUSE_SET(0) that a recent game_unpause queued for the
    // next tick: otherwise that tick, if it comes after this pause, unpauses the
    // game again (P6 full1: the deferred coordinator's re-pause undone on one
    // platform and not the other, by timing alone).
    g_delayed_unpause_ticks.store(0, std::memory_order_relaxed);
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PAUSE_FLAG) = 1;
#ifdef _WIN32
    // Windows (ADR 0001 P6, full4): after game_unpause's synchronous PAUSE_SET(0),
    // the byte alone did not stop the engine: the deferred coordinator's re-pause
    // logged {paused=1} and the clock ran on through the next twelve deferred
    // queries (wars started at ticks 18 and 26). PAUSE_SET(1), the engine's own
    // path, the same call game_unpause makes with 0, guarded the same way. Linux
    // keeps the byte write: every Linux run paused on it.
    {
        g_in_protected_region = true;
        if (sigsetjmp(g_recovery_point, 1) == 0) {
            auto fn = reinterpret_cast<pause_set_fn>(bridge::game_fn(ck3::funcs::PAUSE_SET));
            fn(idler, 1, player_id);
        } else if (g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge] game_pause: PAUSE_SET(1) faulted; the pause byte is set\n");
        }
        g_in_protected_region = false;
    }
#endif
}

static void unpause_primitive() {
    // Every read before any write, and no write after a read that faulted (as
    // game_pause): the fault stays counted for the handler that asked.
    const int faults = safe_read_fault_count();
    void* idler = get_idler();
    if (!idler) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_unpause: CJominiIdler singleton is null\n");
        return;
    }
    void* gs = get_game_state_root();
    const uintptr_t epc_at = gs ? reinterpret_cast<uintptr_t>(gs) + ck3::game_state::EVENT_PAUSE_COUNT : 0;
    const uintptr_t dpc_at = gs ? reinterpret_cast<uintptr_t>(gs) + ck3::game_state::DATE_PAUSE_COUNT : 0;
    const int32_t event_pause_count = gs ? safe_read<int32_t>(epc_at) : 0;
    const int32_t date_pause_count  = gs ? safe_read<int32_t>(dpc_at) : 0;
    const int player_id = safe_read<int32_t>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PLAYER_ID);
    if (safe_read_fault_count() != faults) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_unpause: a read faulted; nothing written\n");
        return;
    }

    // Phase 1: Clear the manual pause flag byte to get ticks flowing
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PAUSE_FLAG) = 0;

    // Phase 2: Clear event-pause count — if an event is holding the game paused,
    // the engine will immediately re-pause even after we clear the manual flag.
    if (event_pause_count > 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_unpause: clearing event-pause count (was %d)\n",
                event_pause_count);
        *reinterpret_cast<int32_t*>(epc_at) = 0;
    }
    if (date_pause_count > 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_unpause: clearing date-pause count (was %d)\n",
                date_pause_count);
        *reinterpret_cast<int32_t*>(dpc_at) = 0;
    }

    // Phase 3: Call PAUSE_SET *synchronously* on the calling thread (the IPC
    // thread for a session verb, the game thread for auto_respond) to propagate
    // the unpause to the UI and the clock.
    //
    // Cold-unpause bug (re_path_a_live_run_20260610): a PAUSE_SET deferred to
    // on_tick never ran from a cold player-pause, because on_tick rides the
    // daily tick. Clearing the manual flag (Phase 1) alone is not enough; the
    // clock stays parked until PAUSE_SET(0) propagates. The engine's own pause
    // UI calls it from the main thread while the session thread runs commands
    // (re_frame_update_hook.md §3), which is the posture here. Guarded by
    // sigsetjmp so a fault degrades to the drain's repeat (check_delayed_unpause)
    // rather than killing the process.
    bool sync_ok = false;
    if (idler) {
        watchdog_arm(3);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            if (g_logfile) fprintf(g_logfile,
                "[CK3-Bridge] game_unpause: synchronous PAUSE_SET faulted (sig=%d) — "
                "falling back to deferred tick\n", sig);
        } else {
            auto fn = reinterpret_cast<pause_set_fn>(bridge::game_fn(ck3::funcs::PAUSE_SET));
            fn(idler, 0, player_id);
            g_in_protected_region = false;
            watchdog_disarm();
            sync_ok = true;
        }
    }

    // Fallback / belt-and-suspenders: still queue one deferred PAUSE_SET. If the
    // synchronous call succeeded this is a harmless idempotent repeat once ticks
    // flow; if it faulted, this is the recovery path.
    g_delayed_unpause_ticks.store(1, std::memory_order_relaxed);
    if (g_logfile) fprintf(g_logfile,
        "[CK3-Bridge] game_unpause: flag cleared, synchronous PAUSE_SET %s, "
        "deferred PAUSE_SET queued\n", sync_ok ? "ok" : "skipped/failed");
}

// Called from the game-thread drain: the deferred PAUSE_SET(0) repeat that
// unpause_primitive queued is part of that ONE unpause write (L13 amendment 2:
// it counts nothing).
void check_delayed_unpause() {
    std::unique_lock<std::timed_mutex> clock_lock(g_clock_mutex, std::try_to_lock);
    if (!clock_lock.owns_lock()) return;   // a clock write is in flight; next pass
    SavedRecovery saved_recovery;           // put back before the lock is released
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) != 0) {
        g_in_protected_region = false;
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Delayed unpause: PAUSE_SET faulted; not retried\n");
        return;
    }
    int remaining = g_delayed_unpause_ticks.load(std::memory_order_relaxed);
    if (remaining > 0) {
        remaining--;
        g_delayed_unpause_ticks.store(remaining, std::memory_order_relaxed);
        if (remaining == 0) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Delayed unpause: executing from the game-thread drain\n");
            // Use PAUSE_SET from the game thread for proper state propagation
            const int faults = safe_read_fault_count();
            void* idler = get_idler();
            const int player_id = idler ? safe_read<int32_t>(
                reinterpret_cast<uintptr_t>(idler) + ck3::idler::PLAYER_ID) : 0;
            if (idler && safe_read_fault_count() == faults) {
                auto fn = reinterpret_cast<pause_set_fn>(bridge::game_fn(ck3::funcs::PAUSE_SET));
                fn(idler, 0, player_id);
            } else if (idler && g_logfile) {
                fprintf(g_logfile, "[CK3-Bridge] Delayed unpause: the idler's player id is unreadable; not called\n");
            }
        }
    }
    g_in_protected_region = false;
}

bool is_paused() {
    void* idler = get_idler();
    if (!idler) return false;
    return safe_read<uint8_t>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PAUSE_FLAG) != 0;
}

static void speed_primitive(int speed) {
    const int faults = safe_read_fault_count();
    void* gs = get_game_state_root();
    if (!gs) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_set_speed: game state root is null\n");
        return;
    }
    if (speed < 1) speed = 1;
    // HARD CAP at 3: at speed >3 the game outruns the agent loop, so
    // deaths/wars resolve invisibly between cycles (corpse-steering /
    // war-resolves-in-one-tick failures — Gen-35/Gen-40). This is the single
    // chokepoint every speed write funnels through (clock_write, whatever the
    // origin), so the cap is unconditional and unbypassable regardless of
    // caller. (re_dejure_meter_and_mint.md §3.)
    if (speed > 3) speed = 3;
    const uintptr_t speed_at = reinterpret_cast<uintptr_t>(gs) + ck3::game_state::GAME_SPEED;
    const int old_speed = safe_read<int32_t>(speed_at);
    // No write after a read that faulted (the fault stays counted for the handler).
    if (safe_read_fault_count() != faults) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_set_speed: a read faulted; nothing written\n");
        return;
    }
    // CK3 stores speed 0-indexed (0-4 internal = 1-5 display)
    *reinterpret_cast<int32_t*>(speed_at) = speed - 1;
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_set_speed: display %d->%d (stored %d->%d)\n",
            old_speed + 1, speed, old_speed, speed - 1);
}

int game_get_speed() {
    void* gs = get_game_state_root();
    if (!gs) return 0;
    return safe_read<int32_t>(reinterpret_cast<uintptr_t>(gs) + ck3::game_state::GAME_SPEED);
}

// =============================================================================
// L13 D4: THE ONE CLOCK WRITE.
//
// Every bridge write to the pause flag, PAUSE_SET or the game speed goes through
// clock_write, which names who asked. The three primitives above are static to
// this file and called from here alone (check_delayed_unpause repeats the
// PAUSE_SET(0) an unpause queued, as part of that same write);
// bridge/scripts/inline_safety_static.py fails a row on any other site.
//
// Counting (L13 amendment 2): ONE count per logical clock operation that
// reaches this function, whether or not the game state changed (a pause of a
// paused game counts 1) and whatever the primitive did after a faulted read
// (the caller answers the fault). Internal repeats are not counts: the deferred
// PAUSE_SET repeat is part of its unpause. get_tick_info publishes the four
// counters as clock_writes.
// =============================================================================
static std::atomic<uint64_t> g_clock_writes[4];

static const char* clock_origin_name(ClockOrigin o) {
    switch (o) {
        case ClockOrigin::Session:     return "session";
        case ClockOrigin::AutoRespond: return "auto_respond";
        case ClockOrigin::ObserveMode: return "observe_mode";
        case ClockOrigin::Pipeline:    return "pipeline";
    }
    return "unknown";
}

void clock_write(ClockWrite kind, ClockOrigin origin, int speed) {
    std::unique_lock<std::timed_mutex> clock_lock(g_clock_mutex, std::defer_lock);
    if (!clock_lock.try_lock_for(std::chrono::seconds(2)) && g_logfile)
        fprintf(g_logfile, "[CK3-Bridge] clock_write: the clock mutex was held past 2 s; "
                "writing without it\n");
    {
        SavedRecovery saved_recovery;   // the caller's point survives the primitives' own
        g_in_protected_region = true;
        if (sigsetjmp(g_recovery_point, 1) == 0) {
            switch (kind) {
                case ClockWrite::Pause:   pause_primitive();      break;
                case ClockWrite::Unpause: unpause_primitive();    break;
                case ClockWrite::Speed:   speed_primitive(speed); break;
            }
        } else {
            // A fault inside a primitive lands here, in this frame: the lock is
            // released by clock_lock's destructor on the way out, never leaked.
            ++g_safe_read_fault_count;   // the caller answers memory_access_fault
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] clock_write: a primitive faulted; "
                    "the clock state is what the engine holds\n");
        }
        g_in_protected_region = false;
    }
    const uint64_t n = g_clock_writes[static_cast<int>(origin)].fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (g_logfile) {
        if (kind == ClockWrite::Speed)
            fprintf(g_logfile, "[CK3-Bridge] clock_write: speed %d origin=%s (count %llu)\n",
                    speed, clock_origin_name(origin), static_cast<unsigned long long>(n));
        else
            fprintf(g_logfile, "[CK3-Bridge] clock_write: %s origin=%s (count %llu)\n",
                    kind == ClockWrite::Pause ? "pause" : "unpause",
                    clock_origin_name(origin), static_cast<unsigned long long>(n));
    }
}

ClockWriteCounts clock_write_counts() {
    ClockWriteCounts c;
    c.session      = g_clock_writes[static_cast<int>(ClockOrigin::Session)].load(std::memory_order_relaxed);
    c.auto_respond = g_clock_writes[static_cast<int>(ClockOrigin::AutoRespond)].load(std::memory_order_relaxed);
    c.observe_mode = g_clock_writes[static_cast<int>(ClockOrigin::ObserveMode)].load(std::memory_order_relaxed);
    c.pipeline     = g_clock_writes[static_cast<int>(ClockOrigin::Pipeline)].load(std::memory_order_relaxed);
    return c;
}

// =============================================================================
// Event Manager Access (Round 3 — CONFIRMED, Round 7+ corrected path)
// =============================================================================
//
// The event queue does NOT live on the CEventManager singleton directly.
// Correct access path:
//   game_root = *(g_base_addr + GAME_STATE_ROOT)
//   subsys    = *(game_root + 0xa0)
//   queue     = subsys + 0x2f280
//   array_ptr = *(queue + 0x1ed8)    // array of SPlayerEventData* (8 bytes each)
//   count     = *(queue + 0x1ee4)    // int32 count
//
// The CEventManager singleton is kept for non-queue uses.

// Read the CEventManager singleton pointer (returns nullptr if not available)
static void* get_event_manager() {
    return reinterpret_cast<void*>(safe_read_offset<uintptr_t>(ck3::event_sys::EVENT_MANAGER));
}

// Get the event queue object by following the correct subsystem path.
// Returns the queue base address, or 0 on failure.
static uintptr_t get_event_queue() {
    const uintptr_t game_root = reinterpret_cast<uintptr_t>(get_game_state_root());
    if (!game_root) return 0;
    const uintptr_t subsys = safe_read<uintptr_t>(game_root + ck3::event_sys::GAME_STATE_SUBSYS_PTR);
    if (!subsys) return 0;
    return subsys + ck3::event_sys::EVENT_QUEUE_OFFSET;
}

// =============================================================================
// Pending Event Enumeration (Round 3 — CONFIRMED, Round 7+ corrected)
// =============================================================================
//
// Corrected understanding:
//   - Queue entries are SPlayerEventData* (0x1C8 bytes each), NOT CEvent*
//   - Each SPlayerEventData has a CEvent* at +0x1b0 (SPEVENT_EVENT_DEF)
//   - CEvent fields (options, narrative flag, state) live on the CEvent*,
//     NOT directly on the SPlayerEventData
//   - Queue accessed via subsystem path, NOT event manager singleton
//
// SPlayerEventData layout:
//   +0x168  string1 ptr       +0x170  string1 len
//   +0x188  string2 ptr       +0x190  string2 len
//   +0x1a8  expiry date (int64)
//   +0x1b0  CEvent* (event definition pointer)
//   +0x1b8  target character ID (int32)
//   +0x1bc  event instance ID (int32)
//   +0x1c0  fire flag (byte)
//
// CEvent layout (accessed via *(SPlayerEventData + 0x1b0)):
//   +0x1b0  options array ptr
//   +0x1bc  option count (int32)
//   +0x271  narrative event flag (byte)
//   +0x278  event state (int: 1=player, 2=pending)
//
// Option struct layout (confirmed Round 3):
//   +0x28   script value block (ai_chance computation input)
//   +0x318  direct ai_chance override (int, if nonzero skip script eval)

struct PendingEventInfo {
    void*    event_ptr;        // raw pointer to SPlayerEventData in game memory
    void*    event_def;        // CEvent* (event definition, from SPlayerEventData+0x1b0)
    uint32_t char_id;          // character ID from SPlayerEventData+0x1b8
    uint32_t instance_id;      // event instance ID from SPlayerEventData+0x1bc
    bool     is_narrative;     // CEvent+0x271 flag (via event_def)
    int32_t  option_count;     // CEvent+0x1bc option count (via event_def)
    void*    options_array;    // *(CEvent+0x1b0) options (via event_def)
};

// Enumerate all pending events for a given character.
// Returns the number found, fills out_events up to max_count.
int enumerate_pending_events(uint32_t player_char_id,
                             PendingEventInfo* out_events,
                             int max_count) {
    uintptr_t queue = get_event_queue();
    if (!queue) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] enumerate_pending_events: event queue is null\n");
        return 0;
    }

    // Read event list pointer and count from queue object.
    // The array can be reallocated by the engine on another thread, so we
    // snapshot the pointer and count once via safe_read, validate them, and
    // cap iteration to prevent runaway loops from stale/corrupt values.
    uintptr_t event_list = safe_read<uintptr_t>(queue + ck3::event_sys::EVENT_LIST_PTR);
    if (!safe_ptr(event_list)) return 0;

    uint32_t count = safe_read<uint32_t>(queue + ck3::event_sys::EVENT_LIST_COUNT);

    if (count == 0) return 0;

    // Hard cap: never iterate more than 32 entries regardless of what the
    // engine reports — protects against corrupt count after reallocation.
    constexpr uint32_t MAX_SAFE_ITER = 32;
    if (count > MAX_SAFE_ITER) count = MAX_SAFE_ITER;

    // Also cap by what the caller can receive
    if (static_cast<int>(count) > max_count && max_count > 0)
        count = static_cast<uint32_t>(max_count);

    int found = 0;

    // Iterate backwards, matching the engine's FUN_05f5ace0 pattern
    for (int i = static_cast<int>(count) - 1; i >= 0 && found < max_count; i--) {
        // Each entry is an 8-byte pointer to SPlayerEventData
        uintptr_t entry_addr = event_list + i * 8;
        if (!safe_ptr(entry_addr)) break;  // array went bad
        uintptr_t sp_event = safe_read<uintptr_t>(entry_addr);
        if (!safe_ptr(sp_event)) continue;

        // Filter: character ID must match (on SPlayerEventData)
        uint32_t char_id_val = safe_read<uint32_t>(sp_event + ck3::event_sys::SPEVENT_CHAR_ID);
        if (char_id_val != player_char_id) continue;

        // Dereference CEvent* from SPlayerEventData+0x1b0
        uintptr_t event_def = safe_read<uintptr_t>(sp_event + ck3::event_sys::SPEVENT_EVENT_DEF);
        if (!safe_ptr(event_def)) continue;

        // Filter: event state must indicate a pending/active event.
        // State 2 = "pending" (original assumption), State 3 = "ready" (observed in live game).
        // Accept both; reject state 0 (empty) and state 1 (player-owned but not ready).
        int32_t state = safe_read<int32_t>(event_def + ck3::event_sys::CEVENT_STATE);
        if (state < 2) continue;

        // Read narrative flag from CEvent
        uint8_t narrative_val = safe_read<uint8_t>(event_def + ck3::event_sys::CEVENT_NARRATIVE_FLAG);
        bool is_narrative = (narrative_val != 0);

        // Read option count from CEvent
        int32_t opt_count = safe_read<int32_t>(event_def + ck3::event_sys::CEVENT_OPTION_COUNT);

        // Read options array pointer from CEvent
        uintptr_t options_raw = safe_read<uintptr_t>(event_def + ck3::event_sys::CEVENT_OPTIONS_ARRAY);
        void* options_array = safe_ptr(options_raw) ? reinterpret_cast<void*>(options_raw) : nullptr;

        // Read instance ID from SPlayerEventData
        uint32_t instance_id = safe_read<uint32_t>(sp_event + ck3::event_sys::SPEVENT_INSTANCE_ID);

        out_events[found].event_ptr     = reinterpret_cast<void*>(sp_event);
        out_events[found].event_def     = reinterpret_cast<void*>(event_def);
        out_events[found].char_id       = player_char_id;
        out_events[found].instance_id   = instance_id;
        out_events[found].is_narrative  = is_narrative;
        out_events[found].option_count  = opt_count;
        out_events[found].options_array = options_array;
        found++;
    }

    return found;
}

// get_event_option removed — was unused. Recoverable from git history.

// =============================================================================
// Non-blocking Event Notification (push to agent, don't wait for response)
// =============================================================================

// Read a std::string from game memory. Routed through the canonical guard
// (bridge.h safe_read_game_string) after crash 20260625: this reader was the
// allocation VICTIM on the crashing stack (send_event_notification → std::string
// ctor → malloc on an already-poisoned heap). The old body also constructed
// std::string directly from the raw game pointer when is_mapped_range passed,
// which can fault between the (per-tick stale) map-check and the copy. The guard
// bounds the copy and uses an EFAULT-safe process_vm_readv exclusively.
static std::string ev_read_std_string(uintptr_t str_addr) {
    return safe_read_game_string(str_addr);
}

// Escape a string for JSON embedding (handles quotes, backslashes, control chars)
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
                    out += hex;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

bool send_event_notification(const PendingEventInfo& event) {
    if (!agent_connected()) return false;

    // Try to read the event type key from CEvent (db_object pattern: NAME_KEY at +0x10)
    std::string event_key;
    if (event.event_def) {
        uintptr_t def = reinterpret_cast<uintptr_t>(event.event_def);
        event_key = ev_read_std_string(def + ck3::db_object::NAME_KEY);
    }

    // Also try reading the two strings from SPlayerEventData
    std::string string1, string2;
    if (event.event_ptr) {
        uintptr_t sp = reinterpret_cast<uintptr_t>(event.event_ptr);
        string1 = ev_read_std_string(sp + ck3::event_sys::SPEVENT_STRING1_PTR);
        string2 = ev_read_std_string(sp + ck3::event_sys::SPEVENT_STRING2_PTR);
    }

    // Build JSON notification.
    // Schema matches what ck3agent/server/agent.py and prompts.format_event() expect:
    //   - event_type: gates IMMEDIATE_EVENTS activation; "event_popup" forces activation
    //   - requires_response: also triggers activation regardless of event_type
    //   - event_id: the event's script key
    //   - instance_id: the event queue entry's ID
    //   - options[]: index list (text resolution from CJominiEventOption is open work; see
    //     re/findings/re_event_pipeline.md "Open Questions" #2)
    char buf[3072];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"type\":\"event\""
        ",\"event_type\":\"event_popup\""
        ",\"requires_response\":true"
        ",\"event_id\":\"%s\""
        ",\"instance_id\":%u"
        ",\"char_id\":%u"
        ",\"option_count\":%d"
        ",\"is_narrative\":%s",
        json_escape(event_key.empty() ? "unknown" : event_key).c_str(),
        event.instance_id,
        event.char_id,
        event.option_count,
        event.is_narrative ? "true" : "false");

    // Title falls back to the event key; description uses string1 if provided.
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        ",\"title\":\"%s\"",
        json_escape(event_key.empty() ? "(unknown event)" : event_key).c_str());
    if (!string1.empty()) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"description\":\"%s\"", json_escape(string1).c_str());
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"string1\":\"%s\"", json_escape(string1).c_str());
    }
    if (!string2.empty()) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"string2\":\"%s\"", json_escape(string2).c_str());
    }

    // Emit a stub options[] so the agent can iterate by index. Each option is
    // {id:N, text:"option_N"} until CJominiEventOption text resolution lands.
    pos += snprintf(buf + pos, sizeof(buf) - pos, ",\"options\":[");
    int max_opts = event.option_count;
    if (max_opts < 0) max_opts = 0;
    if (max_opts > 8) max_opts = 8;
    for (int i = 0; i < max_opts; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"id\":%d,\"text\":\"option_%d\"}", i, i);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]");

    pos += snprintf(buf + pos, sizeof(buf) - pos, "}");

    std::string msg(buf, pos);
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Sending event notification: instance=%u key=%s options=%d\n",
            event.instance_id, event_key.c_str(), event.option_count);
    return send_ipc_message(msg);
}

// Poll pending events and send notifications to the agent.
// Called from on_tick. Returns number of new events sent.
int poll_and_notify_events() {
    uint32_t player_id = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
    if (player_id == 0) return 0;

    // Early exit: peek at the event count *before* entering the iteration
    // loop in enumerate_pending_events.  Most ticks have zero pending events,
    // so this avoids touching the dynamic array pointer entirely and
    // eliminates ~90% of crash opportunities from use-after-free.
    {
        uintptr_t queue = get_event_queue();
        if (!queue) return 0;
        if (safe_read<uint32_t>(queue + ck3::event_sys::EVENT_LIST_COUNT) == 0) return 0;
    }

    PendingEventInfo events[8];
    int count = enumerate_pending_events(player_id, events, 8);
    if (count == 0) return 0;

    int new_count = 0;
    for (int i = 0; i < count; i++) {
        if (was_event_sent(events[i].instance_id)) continue;

        if (send_event_notification(events[i])) {
            mark_event_sent(events[i].instance_id);
            new_count++;
        }
    }

    // L13 D3: the bridge no longer pauses on a notification. The engine's own
    // event pause (event_pause_count) holds the game where an event demands it;
    // any other pause is the player's call (session).
    return new_count;
}

// =============================================================================
// AI Event Option Evaluator — superseded by the non-blocking path
// =============================================================================
//
// HOOK TARGET: FUN_05f4d570 (ck3::event_sys::AI_EVENT_OPTION_EVAL)
//
// SUPERSEDED (ck-x5ha): never installed. The agent was to answer through
// CSelectEventOptionCommand, bypassing the AI evaluator; that path (respond_event)
// is itself deleted (L16).
//
// The notes below are kept as RE provenance. The placeholder body has been
// removed; recover it from git history (event_handler.cpp pre ck-x5ha) if the
// hook is ever needed again.
//
// CONFIRMED SIGNATURE (Round 3 decompilation):
//   result* FUN_05f4d570(result* out, void* event_obj, void* context, int option_index)
//
// Call path summary:
//   1. AI_EVAL_ENABLED_FLAG gate (DAT_0c6ce5f0 via FUN_07828970). If disabled,
//      writes an empty std::string-shaped result and returns.
//   2. Pulls option = *(*(event_obj + 0x1b0) + option_index * 8).
//   3. Calls FUN_07545820 (AI_CHANCE_COMPUTE) for the int64 weight, using
//      either the direct override at option+0x318 or the script block at
//      option+0x28 via FUN_07572400 (fixed-point /100000).
//   4. Wraps the result via FUN_038fedb0 keyed "EVENT_OPTION_AI_CHANCE" — the
//      consumer is reasoning/tooltip plumbing, not raw weight comparison, so
//      safely biasing the output requires more layout work than this bead
//      warrants (see re/findings/re_event_pipeline.md "Open Questions").

// =============================================================================
// Auto-Respond to Pending Events (Passthrough Mode)
// =============================================================================
//
// In passthrough mode, the native AI controls the player character but cannot
// handle events that target the player (because is_ai=0). The game pauses and
// shows a popup, blocking all simulation ticks.
//
// This function detects pending events for the player character and reports
// them. It does NOT modify event data (setting scope_type=0 was losing all
// event effects). Instead, it just logs the events and unpauses the game
// so the simulation can continue. No option is chosen: the events stay queued
// and the engine resolves them as it does for an unanswered popup.

int auto_respond_pending_events() {
    // Read player character ID
    uint32_t player_id = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
    if (player_id == 0) return 0;

    // Enumerate pending events for the player character
    PendingEventInfo events[16];
    int count = enumerate_pending_events(player_id, events, 16);
    if (count == 0) return 0;

    // Log detected events (do NOT modify event data — that loses effects)
    for (int i = 0; i < count; i++) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Detected pending event: sp_event=%p event_def=%p "
                "char=%u instance=%u options=%d narrative=%d\n",
                events[i].event_ptr, events[i].event_def,
                events[i].char_id, events[i].instance_id,
                events[i].option_count, events[i].is_narrative ? 1 : 0);
    }

    // Clear the event-pause count in game state so the game unpauses
    // (events remain in queue for the agent to respond to properly).
    // L13 D3 (kept, flagged to the owner): the unpause is the bridge's
    // no-agent auto-respond fallback acting on the GAME's OWN event pause, so it
    // runs only when that pause was holding the game at this moment
    // (event_pause_count > 0); a pause the player or a tool made is left alone.
    // The unpause clears the event-pause count itself (unpause_primitive), so
    // the count is written only through clock_write, and counted (witness W10).
    void* gs = get_game_state_root();
    const int32_t event_pauses = gs ? safe_read<int32_t>(
        reinterpret_cast<uintptr_t>(gs) + ck3::game_state::EVENT_PAUSE_COUNT) : 0;
    // Both: the game's own event pause is pending AND the game is paused
    // (witness FF2 m-1: without is_paused a running game logged a counted
    // auto_respond unpause that changed nothing).
    if (event_pauses > 0 && is_paused()) {
        clock_write(ClockWrite::Unpause, ClockOrigin::AutoRespond);
    }

    return count;
}

// =============================================================================
// Initialization
// =============================================================================

void init_event_handler() {
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event handler: initializing...\n");

    // Check which offsets are available
    bool has_idler = (ck3::globals::IDLER_SINGLETON != 0);
    bool has_game_state = (ck3::globals::GAME_STATE_ROOT != 0);
    bool has_event_eval = (ck3::event_sys::AI_EVENT_OPTION_EVAL != 0);
    bool has_event_mgr = (ck3::event_sys::EVENT_MANAGER != 0);

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event handler offsets:\n");
    if (g_logfile) fprintf(g_logfile, "  IDLER_SINGLETON:       %s (pause via CJominiIdler+0x%zx)\n",
            has_idler ? "CONFIRMED" : "MISSING", ck3::idler::PAUSE_FLAG);
    if (g_logfile) fprintf(g_logfile, "  GAME_STATE_ROOT:       %s (speed via root+0x%zx)\n",
            has_game_state ? "CONFIRMED" : "MISSING", ck3::game_state::GAME_SPEED);
    if (g_logfile) fprintf(g_logfile, "  AI_EVENT_OPTION_EVAL:  %s (0x%lx)\n",
            has_event_eval ? "CONFIRMED" : "MISSING", ck3::event_sys::AI_EVENT_OPTION_EVAL);
    if (g_logfile) fprintf(g_logfile, "  EVENT_MANAGER:         %s (0x%lx)\n",
            has_event_mgr ? "CONFIRMED" : "MISSING", ck3::event_sys::EVENT_MANAGER);
    if (g_logfile) fprintf(g_logfile, "  EVENT_QUEUE_MUTEX:     0x%lx\n", ck3::event_sys::EVENT_QUEUE_MUTEX);
    if (g_logfile) fprintf(g_logfile, "  AI_EVAL_ENABLED_FLAG:  0x%lx\n", ck3::event_sys::AI_EVAL_ENABLED_FLAG);

    // Validate event queue access via subsystem path at startup
    if (has_game_state) {
        uintptr_t queue = get_event_queue();
        if (queue) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event queue (subsystem path): %p\n",
                    reinterpret_cast<void*>(queue));

            // Try reading queue count as a basic sanity check
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Pending event queue count: %u\n",
                    safe_read<uint32_t>(queue + ck3::event_sys::EVENT_LIST_COUNT));
        } else {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event queue is NULL (game not loaded yet?)\n");
        }
    }

    // Also check the legacy event manager singleton (still used for non-queue operations)
    if (has_event_mgr) {
        void* event_mgr = get_event_manager();
        if (event_mgr) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event manager singleton: %p\n", event_mgr);
        } else {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event manager singleton is NULL (game not loaded yet?)\n");
        }
    }

    // AI_EVENT_OPTION_EVAL hook is intentionally NOT installed (ck-x5ha). The
    // offsets are logged for diagnostics only.
    if (!(has_event_eval && has_event_mgr)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event hook offsets unmapped (informational; nothing installs the hook)\n");
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event handler: poll_and_notify_events() pushes events when an agent is connected; auto_respond_pending_events() releases the event pause when none is\n");
}

void shutdown_event_handler() {
    // Remove event hook if installed
    if (g_orig_ai_event_option_eval) {
        remove_hook(g_base_addr + ck3::event_sys::AI_EVENT_OPTION_EVAL);
        g_orig_ai_event_option_eval = nullptr;
    }
}

} // namespace bridge
