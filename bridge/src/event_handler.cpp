// Event interception — detects game events targeting the controlled character,
// pauses the game, sends event data to the agent, and accepts the agent's
// option choice via `respond_event` IPC (which dispatches CSelectEventOptionCommand).
//
// STATUS: The non-blocking poll/notify/select path is the active flow. From on_tick
// (bridge_init.cpp), `poll_and_notify_events()` runs when an agent is connected and
// pushes pending events to the agent (game stays paused while the agent decides).
// `select_event_option()` runs from the `respond_event` action handler and dispatches
// the engine command directly.
//
// The legacy AI_EVENT_OPTION_EVAL hook (commented out below) was the original plan,
// but the non-blocking path supersedes it: dispatching CSelectEventOptionCommand
// bypasses the AI evaluator entirely, so biasing the AI weight result is unnecessary.
// See re/findings/re_event_pipeline.md and ck-x5ha design notes for the rationale.

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
#include <condition_variable>
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

// =============================================================================
// Event Response State
// =============================================================================

// Pending event response from agent (set by event_response action handler)
static std::mutex g_event_mutex;
static std::condition_variable g_event_cv CK3_EARLY_INIT;
static std::atomic<bool> g_event_pending{false};
static std::atomic<int> g_event_response_option{0};
static char g_pending_event_id[128] = {};

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

// Clear a sent event (after agent responds)
void clear_sent_event(uint32_t instance_id) {
    std::lock_guard<std::mutex> lock(g_sent_events_mutex);
    g_sent_event_instance_ids.erase(instance_id);
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
    auto** pp = reinterpret_cast<void**>(g_base_addr + ck3::globals::IDLER_SINGLETON);
    return *pp;
}

// Read the game state root pointer (returns nullptr if not available)
static void* get_game_state_root() {
    auto** pp = reinterpret_cast<void**>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    return *pp;
}

// PAUSE_SET signature: void fn(CJominiIdler* idler, int pause_val, int player_id)
// pause_val: 0 = unpause, 1 = pause
// NOTE: Only safe to call from the game's main thread (on_tick context).
// For cross-thread use, fall back to raw byte writes.
typedef void (*pause_set_fn)(void* idler, int pause_val, int player_id);

void game_pause() {
    void* idler = get_idler();
    if (!idler) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_pause: CJominiIdler singleton is null\n");
        return;
    }
    auto* flag = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PAUSE_FLAG);
    *flag = 1;
}

// Delayed unpause using PAUSE_SET for proper state propagation after game load.
// Uses a flag checked in on_tick (game thread safe) instead of calling from a timer thread.
static std::atomic<int> g_delayed_unpause_ticks{0};

void game_unpause() {
    void* idler = get_idler();
    if (!idler) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_unpause: CJominiIdler singleton is null\n");
        return;
    }

    // Phase 1: Clear the manual pause flag byte to get ticks flowing
    auto* flag = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PAUSE_FLAG);
    *flag = 0;

    // Phase 2: Clear event-pause count — if an event is holding the game paused,
    // the engine will immediately re-pause even after we clear the manual flag.
    void* gs = get_game_state_root();
    if (gs) {
        auto* event_pause_count = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(gs) + ck3::game_state::EVENT_PAUSE_COUNT);
        if (*event_pause_count > 0) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_unpause: clearing event-pause count (was %d)\n",
                    *event_pause_count);
            *event_pause_count = 0;
        }
        auto* date_pause_count = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(gs) + ck3::game_state::DATE_PAUSE_COUNT);
        if (*date_pause_count > 0) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_unpause: clearing date-pause count (was %d)\n",
                    *date_pause_count);
            *date_pause_count = 0;
        }
    }

    // Phase 3: Call PAUSE_SET *synchronously* on the calling (IPC) thread to
    // propagate the unpause to the UI and the sim scheduler.
    //
    // Cold-unpause bug (re_path_a_live_run_20260610): deferring PAUSE_SET to
    // on_tick is a deadlock from a cold player-pause — on_tick rides the sim
    // tick, which doesn't advance while paused, so the deferred PAUSE_SET never
    // runs and the sim never restarts. Clearing the manual flag (Phase 1) alone
    // is not enough; the engine's scheduler stays parked until PAUSE_SET(0)
    // propagates. Calling it here breaks the cycle. This is safe precisely
    // because the sim is paused: the game thread is parked in its idle loop,
    // not mutating the state PAUSE_SET touches. Guarded by sigsetjmp so a fault
    // degrades to the deferred-tick fallback rather than killing the process.
    bool sync_ok = false;
    if (idler) {
        int player_id = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(idler) + ck3::idler::PLAYER_ID);
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
            auto fn = reinterpret_cast<pause_set_fn>(g_base_addr + ck3::funcs::PAUSE_SET);
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

void game_unpause_delayed(int delay_ms) {
    // Convert ms to approximate ticks (game runs ~1 tick per day at speed 3 ≈ several per second)
    // Use a conservative estimate: check every tick, count down from N
    // At speed 3, ~5-10 ticks/sec. 3000ms ≈ 15-30 ticks. Use 20 as default.
    int ticks = (delay_ms / 150);  // ~150ms per tick at speed 3
    if (ticks < 5) ticks = 5;
    g_delayed_unpause_ticks.store(ticks, std::memory_order_relaxed);
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Delayed unpause scheduled in ~%d ticks\n", ticks);
}

// Called from on_tick to check and execute delayed unpause
void check_delayed_unpause() {
    int remaining = g_delayed_unpause_ticks.load(std::memory_order_relaxed);
    if (remaining > 0) {
        remaining--;
        g_delayed_unpause_ticks.store(remaining, std::memory_order_relaxed);
        if (remaining == 0) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Delayed unpause: executing from on_tick\n");
            // Use PAUSE_SET from game thread for proper state propagation
            void* idler = get_idler();
            if (idler) {
                int player_id = *reinterpret_cast<int32_t*>(
                    reinterpret_cast<uintptr_t>(idler) + ck3::idler::PLAYER_ID);
                auto fn = reinterpret_cast<pause_set_fn>(g_base_addr + ck3::funcs::PAUSE_SET);
                fn(idler, 0, player_id);
            }
        }
    }
}

bool is_paused() {
    void* idler = get_idler();
    if (!idler) return false;
    auto* flag = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(idler) + ck3::idler::PAUSE_FLAG);
    return *flag != 0;
}

void game_set_speed(int speed) {
    void* gs = get_game_state_root();
    if (!gs) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_set_speed: game state root is null\n");
        return;
    }
    if (speed < 1) speed = 1;
    // HARD CAP at 3: paused-gated reads outrun the agent loop at speed >3, so
    // deaths/wars resolve invisibly between cycles (corpse-steering /
    // war-resolves-in-one-tick failures — Gen-35/Gen-40). This is the single
    // chokepoint every speed-write funnels through (MCP game/session, gateway
    // restore, deferred_drain restore), so the cap is unconditional and
    // unbypassable regardless of caller. (re_dejure_meter_and_mint.md §3.)
    if (speed > 3) speed = 3;
    auto* val = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(gs) + ck3::game_state::GAME_SPEED);
    int old_speed = *val;
    // CK3 stores speed 0-indexed (0-4 internal = 1-5 display)
    *val = speed - 1;
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] game_set_speed: display %d->%d (stored %d->%d)\n",
            old_speed + 1, speed, old_speed, speed - 1);
}

int game_get_speed() {
    void* gs = get_game_state_root();
    if (!gs) return 0;
    auto* val = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(gs) + ck3::game_state::GAME_SPEED);
    return *val;
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
    auto** pp = reinterpret_cast<void**>(g_base_addr + ck3::event_sys::EVENT_MANAGER);
    if (!pp) return nullptr;
    return *pp;
}

// Get the event queue object by following the correct subsystem path.
// Returns the queue base address, or 0 on failure.
static uintptr_t get_event_queue() {
    auto** gs_pp = reinterpret_cast<void**>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    if (!gs_pp || !*gs_pp) return 0;
    uintptr_t game_root = reinterpret_cast<uintptr_t>(*gs_pp);

    auto* subsys_pp = reinterpret_cast<uintptr_t*>(game_root + ck3::event_sys::GAME_STATE_SUBSYS_PTR);
    if (!subsys_pp || !*subsys_pp) return 0;
    uintptr_t subsys = *subsys_pp;

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
    //   - event_id: bridge-side key for `respond_event` action
    //   - instance_id: queue entry ID, required by select_event_option / CSelectEventOptionCommand
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
    uint32_t player_id = *reinterpret_cast<uint32_t*>(
        g_base_addr + ck3::globals::PLAYER_CHAR_ID);
    if (player_id == 0) return 0;

    // Early exit: peek at the event count *before* entering the iteration
    // loop in enumerate_pending_events.  Most ticks have zero pending events,
    // so this avoids touching the dynamic array pointer entirely and
    // eliminates ~90% of crash opportunities from use-after-free.
    {
        uintptr_t queue = get_event_queue();
        if (!queue) return 0;
        auto* count_p = reinterpret_cast<uint32_t*>(queue + ck3::event_sys::EVENT_LIST_COUNT);
        if (!count_p) return 0;
        uint32_t raw_count = *count_p;
        if (raw_count == 0) return 0;
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

    if (new_count > 0) {
        // Pause game while events are pending for agent to decide
        if (!is_paused()) {
            game_pause();
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Paused game — %d new event(s) for agent\n", new_count);
        }
    }

    return new_count;
}

// =============================================================================
// Event Option Selection via CSelectEventOptionCommand (non-blocking)
// =============================================================================

// Find a pending event by instance_id and select an option.
// This uses the same command dispatch pattern as send_interaction.
// Returns true on success.
bool select_event_option(uint32_t instance_id, int option_index) {
    uint32_t player_id = *reinterpret_cast<uint32_t*>(
        g_base_addr + ck3::globals::PLAYER_CHAR_ID);
    if (player_id == 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] select_event_option: no player character\n");
        return false;
    }

    // Find the event in the queue
    PendingEventInfo events[16];
    int count = enumerate_pending_events(player_id, events, 16);

    PendingEventInfo* target = nullptr;
    for (int i = 0; i < count; i++) {
        if (events[i].instance_id == instance_id) {
            target = &events[i];
            break;
        }
    }

    if (!target) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] select_event_option: instance_id %u not found in queue\n",
                instance_id);
        return false;
    }

    // LEGITIMACY GATE (re_write_action_legitimacy_audit.md §3 #10): an out-of-range
    // option index used to be silently clamped to 0 — which illegitimately applies a
    // DIFFERENT option's scripted effects than the agent requested (a silent wrong-choice
    // write). Refuse cleanly instead; the caller surfaces success:false to the agent.
    // Pure read + compare, no game-thread call.
    if (option_index < 0 || option_index >= target->option_count) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] select_event_option: option %d out of range [0,%d) — refusing (option_out_of_range)\n",
                option_index, target->option_count);
        return false;
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] select_event_option: instance=%u option=%d (of %d)\n",
            instance_id, option_index, target->option_count);

    // Construct and dispatch CSelectEventOptionCommand (Phase 24 RE: 0x28 bytes)
    // This applies the chosen option's scripted effects and auto-removes the event from queue.
    //
    // Layout (confirmed via Ghidra decompilation):
    //   +0x00: vtable1 (primary: 0x0bb13b60 + base)
    //   +0x08: command_flags (0)
    //   +0x18: vtable2 (helper: 0x0bb13c10 + base)
    //   +0x20: event_instance_id (int32)
    //   +0x24: option_index (int32)

    // Clear this event from sent tracking
    clear_sent_event(instance_id);

    // Build the command on the stack
    alignas(16) uint8_t cmd_buf[0x30] = {};  // 0x28 + padding
    *reinterpret_cast<uintptr_t*>(cmd_buf + 0x00) = g_base_addr + 0x0bb13b60;  // vtable1
    *reinterpret_cast<uintptr_t*>(cmd_buf + 0x18) = g_base_addr + 0x0bb13c10;  // vtable2 (helper)
    *reinterpret_cast<int32_t*>(cmd_buf + 0x20) = static_cast<int32_t>(instance_id);
    *reinterpret_cast<int32_t*>(cmd_buf + 0x24) = option_index;

    // Dispatch via GameCommand__Dispatch on the helper sub-object
    // Signature: void(void* helper, uint32_t cmd_type)
    // cmd_type 0x27d4 = CSelectEventOptionCommand type hash
    auto game_cmd_dispatch = reinterpret_cast<void(*)(void*, uint32_t)>(
        g_base_addr + 0x05edde90);  // GameCommand__Dispatch

    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] select_event_option: dispatch crashed (sig=%d)\n", sig);
        // Fallback: clear event-pause count so game doesn't stay stuck
        void* gs = *reinterpret_cast<void**>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
        if (gs) {
            auto* epc = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(gs) + ck3::game_state::EVENT_PAUSE_COUNT);
            if (epc && *epc > 0) *epc = 0;
        }
        if (is_paused()) game_unpause();
        return false;
    }

    bool dispatched = false;
    try {
        game_cmd_dispatch(cmd_buf + 0x18, 0x27d4);
        dispatched = true;
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] select_event_option: dispatched via CSelectEventOptionCommand "
                "(instance=%u, option=%d)\n", instance_id, option_index);
    } catch (...) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] select_event_option: dispatch threw exception\n");
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!dispatched) {
        // Fallback: clear event-pause count
        void* gs = *reinterpret_cast<void**>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
        if (gs) {
            auto* epc = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(gs) + ck3::game_state::EVENT_PAUSE_COUNT);
            if (epc && *epc > 0) *epc = 0;
        }
        if (is_paused()) game_unpause();
    }

    return dispatched;
}

// =============================================================================
// Event -> Agent IPC (legacy blocking path)
// =============================================================================

// Build JSON for an event and send it to the agent over IPC
static bool send_event_to_agent(const EventInfo& event) {
    // Build JSON message
    // {"type":"event","event_id":"...","target_char_id":N,"title":"...",
    //  "description":"...","options":[{"index":0,"text":"..."},...]}}
    char buf[4096];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"type\":\"event\","
        "\"event_id\":\"%s\","
        "\"target_char_id\":%u,"
        "\"title\":\"%s\","
        "\"description\":\"%s\","
        "\"options\":[",
        json_escape(event.event_id).c_str(),     // rc2: game text, escaped
        event.target_char_id,
        json_escape(event.title).c_str(),
        json_escape(event.description).c_str());

    for (int i = 0; i < event.option_count && i < 8; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"index\":%d,\"text\":\"%s\"}",
            i, json_escape(event.option_texts[i]).c_str());
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

    return send_observation(buf, pos);
}

int send_event_and_wait(const EventInfo& event, int timeout_ms) {
    if (!agent_connected()) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event '%s' fired but no agent connected, defaulting to option 0\n",
                event.event_id);
        return 0;
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event '%s' fired for char %u (%d options). Pausing and sending to agent.\n",
            event.event_id, event.target_char_id, event.option_count);

    // Pause the game
    game_pause();

    // Set up the pending event
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        strncpy(g_pending_event_id, event.event_id, sizeof(g_pending_event_id) - 1);
        g_pending_event_id[sizeof(g_pending_event_id) - 1] = '\0';
        g_event_response_option = 0;
        g_event_pending = true;
    }

    // Send event to agent
    if (!send_event_to_agent(event)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Failed to send event to agent, defaulting to option 0\n");
        g_event_pending = false;
        game_unpause();
        return 0;
    }

    // Wait for agent response with timeout
    int chosen_option = 0;
    {
        std::unique_lock<std::mutex> lock(g_event_mutex);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        // While waiting, poll for action messages from the agent
        // The event_response action handler will set g_event_pending = false
        while (g_event_pending.load()) {
            if (g_event_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event response timeout (%dms), defaulting to option 0\n",
                        timeout_ms);
                g_event_pending = false;
                break;
            }
        }

        chosen_option = g_event_response_option.load();
    }

    // Validate option index
    if (chosen_option < 0 || chosen_option >= event.option_count) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Agent chose invalid option %d (max %d), clamping to 0\n",
                chosen_option, event.option_count - 1);
        chosen_option = 0;
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Agent chose option %d for event '%s'\n",
            chosen_option, event.event_id);

    // Unpause
    game_unpause();

    return chosen_option;
}

// =============================================================================
// Event Response Handler (called from action_dispatch)
// =============================================================================

// Called when the agent sends {"action":"respond_event","params":{"event_id":"...","option":N}}
bool handle_event_response(const char* event_id, int option) {
    std::lock_guard<std::mutex> lock(g_event_mutex);

    if (!g_event_pending.load()) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event response for '%s' but no event pending\n", event_id);
        return false;
    }

    // Verify event_id matches
    if (strcmp(g_pending_event_id, event_id) != 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event response ID mismatch: pending='%s', got='%s'\n",
                g_pending_event_id, event_id);
        return false;
    }

    g_event_response_option = option;
    g_event_pending = false;
    g_event_cv.notify_all();

    return true;
}

// =============================================================================
// AI Event Option Evaluator — superseded by the non-blocking path
// =============================================================================
//
// HOOK TARGET: FUN_05f4d570 (ck3::event_sys::AI_EVENT_OPTION_EVAL)
//
// SUPERSEDED (ck-x5ha): The non-blocking flow at the top of this file
// (poll_and_notify_events + select_event_option) replaces this hook. The agent
// is notified and responds via CSelectEventOptionCommand, which bypasses the
// AI evaluator — there is no longer a need to bias its result.
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
// so the simulation can continue. The agent (or a future CSelectEventOptionCommand
// implementation) should properly respond to events.

int auto_respond_pending_events() {
    // Read player character ID
    uint32_t player_id = *reinterpret_cast<uint32_t*>(
        g_base_addr + ck3::globals::PLAYER_CHAR_ID);
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
    // (events remain in queue for the agent to respond to properly)
    void* gs = *reinterpret_cast<void**>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    if (gs) {
        auto* event_pause_count = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(gs) + ck3::game_state::EVENT_PAUSE_COUNT);
        if (event_pause_count && *event_pause_count > 0) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Clearing event-pause count (was %d)\n",
                    *event_pause_count);
            *event_pause_count = 0;
        }
    }

    // Also unpause the game in case it was paused by the event
    if (is_paused()) {
        game_unpause();
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
            auto* count_p = reinterpret_cast<uint32_t*>(queue + ck3::event_sys::EVENT_LIST_COUNT);
            if (count_p) {
                if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Pending event queue count: %u\n", *count_p);
            }
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

    // AI_EVENT_OPTION_EVAL hook is intentionally NOT installed — the non-blocking
    // poll/notify/select_event_option path supersedes it (ck-x5ha). The offsets are
    // logged for diagnostics only.
    if (has_event_eval && has_event_mgr) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event hook: superseded by non-blocking path (poll_and_notify_events + CSelectEventOptionCommand)\n");
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge]   CSelectEventOptionCommand vtable: 0x%lx\n",
                ck3::event_sys::SELECT_OPT_CMD_VTABLE);
    } else {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event hook offsets unmapped (informational; non-blocking path does not need them)\n");
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event handler: respond_event action available via IPC\n");
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Event handler: poll_and_notify_events() drives bridge→agent flow when an agent is connected\n");
}

void shutdown_event_handler() {
    // Remove event hook if installed
    if (g_orig_ai_event_option_eval) {
        remove_hook(g_base_addr + ck3::event_sys::AI_EVENT_OPTION_EVAL);
        g_orig_ai_event_option_eval = nullptr;
    }

    // Wake any waiting thread
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_event_pending = false;
        g_event_cv.notify_all();
    }
}

} // namespace bridge
