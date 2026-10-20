// personality.cpp — AI personality lock system: get/set personality, apply locks,
// save/load personality locks, personality getter hooks.
//
// Split from action_dispatch.cpp (mechanical refactor, no logic changes).

#include "action_dispatch_internal.h"
#include "session.h"       // ck-6kvq.5: the claims half of declare_save_identity
#include "world_epoch.h"
#include "pipeline.h"   // ck-8o5j: the episode-open clock release
#include <cstring>
#include <shared_mutex>
#include <sys/mman.h>

namespace bridge {

// =============================================================================
// Personality Lock State
// =============================================================================

// g_personality_lock_mutex / g_personality_locks / g_quarantined_personality_locks
// are DEFINED in personality_world_reset.cpp (engine-free TU) so the world-reset
// path is host-linkable. Declared in personality_locks.h.

// THE DISK CARRIER MOVED (ck-9fc3). save_personality_locks_locked() and the
// loader now live in personality_persist.cpp — an ENGINE-FREE translation unit,
// so the identity gate that keeps the file inside one world is provable on a
// host with no CK3 running. The old load_personality_locks() is gone with them:
// it had zero callers, took no identity, and wiring it as-was would have carried
// one world's steering into the next. Its replacement is
// load_personality_locks_from_disk(), reachable only through
// world_identity_declare(). Declared in personality_locks.h.

// =============================================================================
// ck-6kvq.5 — the steering-purge hook
// =============================================================================
// Fired by SessionRegistry when a session is discarded because the world its
// claims were taken in has been refuted.  Its job is narrow and mechanical:
// forget the PER-CHARACTER steering state for those character ids, in both the
// live write set and the quarantine.
//
// Why it must exist at all: g_personality_locks is keyed by CHARACTER ID and
// apply_personality_locks() pokes the axes three times per on_tick.  Releasing a
// claim without forgetting the lock would leave the bridge writing a vanished
// world's personality onto whoever holds that id now — which is exactly ck-9fc3,
// reached by the claims door instead of the reload door.
void install_steering_purge_hook() {
    g_session_registry.set_steering_purge_hook(
        [](const std::string& session_id, const std::vector<uint32_t>& char_ids) {
            if (char_ids.empty()) return;
            size_t live = 0, quar = 0;
            {
                std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
                for (uint32_t cid : char_ids) {
                    // Mask exactly as every other lock-map site does (24-bit id
                    // + 8-bit generation byte): a purge that keyed differently
                    // from the writer would miss every entry it came for.
                    const uint32_t key = cid & 0xFFFFFF;
                    live += g_personality_locks.erase(key);
                    quar += g_quarantined_personality_locks.erase(key);
                }
                // Re-stamp the carrier so the file does not outlive the decision.
                save_personality_locks_locked();
            }
            ADLOG("steering purge for session %s: %zu live + %zu quarantined lock(s) "
                  "dropped across %zu character(s)\n",
                  session_id.c_str(), live, quar, char_ids.size());
        });
}

// =============================================================================
// declare_save_identity — the ck-9fc3 entry point
// =============================================================================
// The harness tells the bridge WHICH SAVE is loaded, using the identity that
// already exists in ck-6kvq.2's run_summary.json ("episode_id" + save.sha256).
// The bridge decides, once, whether quarantined agent steering may be restored,
// and publishes the decision and its evidence (world_identity.cpp). Everything
// hard is in that engine-free TU; this is the wire adapter.
bool do_declare_save_identity(const char* params_json, std::string* out) {
    WorldIdentity id;
    // COPY EACH ONE IMMEDIATELY: json_get_string returns a pointer into a
    // thread-local static buffer, so the second call invalidates the first.
    const char* ep = json_get_string(params_json, "episode_id");
    if (ep) id.episode_id = ep;
    const char* sha = json_get_string(params_json, "save_sha256");
    if (sha) id.save_sha256 = sha;
    int year = json_get_int(params_json, "start_date_year");
    if (year > 0) id.start_date_year = static_cast<uint32_t>(year);

    RestoreRecord rec = world_identity_declare(id);
    ADLOG("declare_save_identity: %s — %s\n", rec.decision.c_str(), rec.why.c_str());

    // ck-8o5j: THIS IS THE EPISODE-OPEN SIGNAL THE ARM PATH ALREADY SENDS.
    // ck-zen6 holds the world paused until the episode begins and was keyed on
    // the episode OBSERVER's declaration — which the arm driver never makes, so
    // the node pilot ran 17 minutes at 0.0000 game-days/sec. The driver DOES
    // declare which episode this is, right here, carrying the episode_id; the
    // clock simply was not listening to it. Releasing here needs no new
    // declarer and cannot race, because an episode opens once and releasing
    // confers nothing.
    pipeline_note_episode_open(id.episode_id.c_str());

    // ck-6kvq.5: the CLAIMS half of the same decision.  ck-9fc3 decides what
    // happens to quarantined STEERING; a declaration that refutes the world a
    // session's claims were taken in must also erase that session, or a resume
    // that abstained (no identity had been declared yet) leaves a control
    // session owning characters in a world it never saw — the ck-9fc3 symptom
    // arriving by the other door.  Sessions that vouched for nothing are left
    // alone: a declaration can only ever say NO (world_identity.h).
    std::vector<std::string> erased = ipc_reconcile_declared_world(id, world_epoch());

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(!id.empty());
    r.key("action");  r.value_string("declare_save_identity");
    if (id.empty()) {
        r.key("reason"); r.value_string("save_sha256_required");
    }
    r.key("decision"); r.value_string(rec.decision.c_str());
    // ck-81su: and what the SAME decision did to the shared board's carrier.
    // EMITTED ONLY WHEN THERE WAS A CARRIER — which can only be arm B. A `board`
    // key in arm A would tell an arm-A harness that a channel it must not know
    // about had been considered, and "present and refused" is exactly the third
    // condition decision G5/A2 rejects.
    if (!rec.board_json.empty()) {
        r.key("board"); r.value_raw(rec.board_json.c_str());
    }
    // Honest echo: a harness that declared a different save must be able to SEE
    // that it just cost some sessions their claims, not infer it from silence.
    r.key("sessions_erased"); r.value_int(static_cast<int64_t>(erased.size()));
    r.key("save_identity"); r.value_raw(world_identity_emit_json());
    r.end_object();
    if (out) *out = r.take();
    return !id.empty();
}

// =============================================================================
// get_ai_personality — reads 10 personality axis values for a character
// =============================================================================

bool do_get_ai_personality(const char* params_json, std::string* out) {
    void* char_ptr = resolve_character(params_json);
    if (!char_ptr) {
        ADLOG("get_ai_personality: character not found\n");
        if (out) *out = R"({"success":false,"action":"get_ai_personality","reason":"character_not_found"})";
        return false;
    }

    // Mask to lower 24 bits (character::ID is a 24-bit id + 8-bit generation byte).
    // All lock-map sites MUST mask identically or set/get/apply/hook key on
    // different values and the lock lookup misses → reads return the engine's
    // recomputed defaults (the "drift" in A7). See set/hook/apply below.
    uint32_t cid = read<uint32_t>(reinterpret_cast<uintptr_t>(char_ptr) + ck3::character::ID) & 0xFFFFFF;
    uintptr_t core_data = read<uintptr_t>(reinterpret_cast<uintptr_t>(char_ptr) + ck3::character::REALM_DATA);
    if (!core_data) {
        ADLOG("get_ai_personality: no core_data for character %u\n", cid);
        if (out) *out = R"({"success":false,"action":"get_ai_personality","reason":"no_core_data"})";
        return false;
    }

    uintptr_t personality_ptr = read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR);
    if (!personality_ptr) {
        ADLOG("get_ai_personality: no personality data for character %u\n", cid);
        if (out) *out = R"({"success":false,"action":"get_ai_personality","reason":"no_personality_data"})";
        return false;
    }

    static const char* axis_names[] = {
        "honor", "greed", "rationality", "energy", "boldness",
        "zeal", "vengefulness", "compassion", "sociability", "dominance"
    };

    // Check personality lock map — copy value under lock to avoid dangling pointer
    PersonalityLock pl_copy = {};
    bool has_lock = false;
    {
        std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);  // read-only
        auto it = g_personality_locks.find(cid);
        if (it != g_personality_locks.end()) {
            pl_copy = it->second;
            has_lock = true;
        }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("action"); r.value_string("get_ai_personality");
    r.key("character_id"); r.value_uint(cid);
    r.key("personality"); r.begin_object();
    for (int i = 0; i < 10; i++) {
        int8_t val;
        if (has_lock && pl_copy.locked[i]) {
            val = pl_copy.axes[i];
        } else if (i < 9) {
            val = *reinterpret_cast<int8_t*>(personality_ptr + i);
        } else {
            val = 0;  // dominance: no engine memory backing, default 0
        }
        r.key(axis_names[i]); r.value_int(static_cast<int>(val));
    }
    r.end_object();
    if (has_lock) {
        r.key("locked"); r.value_bool(true);
    }
    // The engine's own bytes, whatever the lock map says: `personality` above
    // reports a held lock's values, so it cannot show whether the writes reached
    // the game. Nine axes (dominance has no engine byte). ADR 0001 P6.
    r.key("engine"); r.begin_object();
    for (int i = 0; i < 9; i++) {
        r.key(axis_names[i]); r.value_int(static_cast<int>(safe_read<int8_t>(personality_ptr + i)));
    }
    r.end_object();

    // Emit basic context
    uintptr_t c = reinterpret_cast<uintptr_t>(char_ptr);
    uintptr_t realm_data = read<uintptr_t>(c + ck3::character::REALM_DATA);
    if (realm_data) {
        r.key("gold");     r.value_float(static_cast<double>(read<int64_t>(realm_data + ck3::core_data::GOLD)) / 100000.0);
        r.key("prestige"); r.value_float(static_cast<double>(read<int64_t>(realm_data + ck3::core_data::PRESTIGE)) / 100000.0);
        r.key("piety");    r.value_float(static_cast<double>(read<int64_t>(realm_data + ck3::core_data::PIETY)) / 100000.0);
    } else {
        r.key("gold");     r.value_float(0.0);
        r.key("prestige"); r.value_float(0.0);
        r.key("piety");    r.value_float(0.0);
    }
    uintptr_t title_data = read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    r.key("military_strength"); r.value_int(title_data ? read<int32_t>(title_data + ck3::title_realm::MIL_CURRENT) : 0);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// set_ai_personality: write personality axis values (partial update)
bool do_set_ai_personality(const char* params_json, std::string* out) {
    void* char_ptr = resolve_character(params_json);
    // N3 (surface-audit 2026-08-01): lookup_character returns the DB SENTINEL
    // (non-null!) on an id miss — the old null-only check let a bogus
    // character_id fall through and write personality axes ONTO THE SENTINEL
    // OBJECT with success:true (silent-accept). Check both null and sentinel
    // BEFORE anything else touches the pointer.
    if (!char_ptr || is_sentinel(char_ptr)) {
        int req_id = json_get_int(params_json, "character_id");
        ADLOG("set_ai_personality: character %d not found — rejected\n", req_id);
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_ai_personality");
            r.key("reason"); r.value_string("character_not_found");
            if (req_id > 0) { r.key("character_id"); r.value_uint(static_cast<uint32_t>(req_id) & 0xFFFFFF); }
            r.end_object();
            *out = r.take();
        }
        return false;
    }
    if (reject_player_actor(char_ptr, "set_ai_personality", out)) return false;

    // Mask lower 24 bits — see get_ai_personality (A7 lock-map key consistency).
    uint32_t cid = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(char_ptr) + ck3::character::ID) & 0xFFFFFF;
    uintptr_t core_data = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(char_ptr) + ck3::character::REALM_DATA);
    if (!core_data) {
        ADLOG("set_ai_personality: no core_data for character %u\n", cid);
        if (out) *out = R"({"success":false,"action":"set_ai_personality","reason":"no_core_data"})";
        return false;
    }

    uintptr_t personality_ptr = safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR);
    if (!personality_ptr) {
        ADLOG("set_ai_personality: no personality data for character %u\n", cid);
        if (out) *out = R"({"success":false,"action":"set_ai_personality","reason":"no_personality_data"})";
        return false;
    }

    static const char* axis_names[] = {
        "honor", "greed", "rationality", "energy", "boldness",
        "zeal", "vengefulness", "compassion", "sociability", "dominance"
    };

    // N3 (surface-audit 2026-08-01): typed rejection for out-of-range axis
    // values. The old path silently CLAMPED (e.g. boldness:9999 → 100) and
    // returned success:true. Validate every provided axis BEFORE any engine
    // write or lock mutation so the call is atomic and loud. (Known limit:
    // json_get_int cannot distinguish a non-numeric value from -1, so
    // "boldness":"banana" still reads as -1 — in range. Range garbage, the
    // audited case, is now rejected.)
    for (int i = 0; i < 10; i++) {
        if (!json_has_key(params_json, axis_names[i])) continue;
        int val = json_get_int(params_json, axis_names[i]);
        if (val < -100 || val > 100) {
            ADLOG("set_ai_personality: axis %s=%d out of range [-100,100] for character %u — rejected\n",
                  axis_names[i], val, cid);
            if (out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("set_ai_personality");
                r.key("reason"); r.value_string("axis_out_of_range");
                r.key("axis"); r.value_string(axis_names[i]);
                r.key("value"); r.value_int(val);
                r.key("hint"); r.value_string("axis values must be integers in [-100,100]; no axis was written");
                r.end_object();
                *out = r.take();
            }
            return false;
        }
    }

    int written = 0;
    int retained = 0;

    // A11 (partial-write leak): MERGE the partial update into the EXISTING
    // per-character lock entry — never replace it.
    //   - operator[] returns a reference to the existing entry (or a fresh
    //     zero-init one for a first-time character). We mutate it in place.
    //   - For each axis PRESENT in this call → set locked[i]=true, axes[i]=value
    //     (and mirror into engine memory for i<9).
    //   - For each axis ABSENT from this call → we DO NOT touch pl.locked[i] /
    //     pl.axes[i], so a previously-locked axis stays locked at its prior
    //     value. apply_personality_locks() re-asserts all locked axes every
    //     tick, and get/getter-hook return the locked value, so an unwritten-
    //     but-previously-locked axis never falls through to the engine recompute.
    // The bug was a REPLACE (rebuild the entry from only this call's axes,
    // dropping prior locks → they reverted to native). This loop is a merge:
    // absent axes are `continue`d before any write, preserving their state.
    std::unique_lock<std::shared_mutex> lock(g_personality_lock_mutex);
    auto& pl = g_personality_locks[cid];

    for (int i = 0; i < 10; i++) {
        // Distinguish "key absent" from a legitimate value of -1: check
        // presence FIRST so a real -1 isn't mistaken for missing (and so a
        // missing key can never clear a prior lock — that's the leak).
        if (!json_has_key(params_json, axis_names[i])) {
            if (pl.locked[i]) retained++;   // prior lock preserved (merge)
            continue;
        }
        int val = json_get_int(params_json, axis_names[i]);

        if (val < -100) val = -100;
        if (val > 100) val = 100;

        if (i < 9) {  // Only 9 axes in engine memory; axis 9 (dominance) is lock-only
            *reinterpret_cast<int8_t*>(personality_ptr + i) = static_cast<int8_t>(val);
        }
        pl.axes[i] = static_cast<int8_t>(val);
        pl.locked[i] = true;
        written++;
    }

    ADLOG("set_ai_personality: wrote %d axes, retained %d prior-locked axes for character %u (merge)\n",
          written, retained, cid);

    if (written > 0) {
        save_personality_locks_locked();
    }

    // Return full personality after writing
    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("action"); r.value_string("set_ai_personality");
    r.key("character_id"); r.value_uint(cid);
    r.key("axes_written"); r.value_int(written);
    r.key("axes_retained"); r.value_int(retained);  // prior-locked axes kept across this partial update (A11 merge)
    r.key("personality"); r.begin_object();
    for (int i = 0; i < 10; i++) {
        int8_t val;
        if (i < 9) {
            val = *reinterpret_cast<int8_t*>(personality_ptr + i);
        } else if (pl.locked[i]) {
            val = pl.axes[i];
        } else {
            val = 0;
        }
        r.key(axis_names[i]); r.value_int(static_cast<int>(val));
    }
    r.end_object();

    uintptr_t c = reinterpret_cast<uintptr_t>(char_ptr);
    uintptr_t realm_data = read<uintptr_t>(c + ck3::character::REALM_DATA);
    if (realm_data) {
        r.key("gold");     r.value_float(static_cast<double>(read<int64_t>(realm_data + ck3::core_data::GOLD)) / 100000.0);
        r.key("prestige"); r.value_float(static_cast<double>(read<int64_t>(realm_data + ck3::core_data::PRESTIGE)) / 100000.0);
        r.key("piety");    r.value_float(static_cast<double>(read<int64_t>(realm_data + ck3::core_data::PIETY)) / 100000.0);
    } else {
        r.key("gold");     r.value_float(0.0);
        r.key("prestige"); r.value_float(0.0);
        r.key("piety");    r.value_float(0.0);
    }
    uintptr_t title_data = read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    r.key("military_strength"); r.value_int(title_data ? read<int32_t>(title_data + ck3::title_realm::MIL_CURRENT) : 0);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// apply_personality_locks — re-apply locked personality values after engine tick
// =============================================================================

// ck-1v1t: THE LOCK MUST NOT BE HELD ACROSS THE STORE, BECAUSE THE STORE CAN
// FAULT AND THE RECOVERY DOES NOT UNWIND.
//
// All three call sites wrap this in the ck-cg3b guard:
//     g_in_protected_region = true;
//     if (sigsetjmp(g_recovery_point, 1) == 0) { try { apply_personality_locks(); } ... }
//     else { blog("SIGSEGV in ... apply_personality_locks"); }
//
// siglongjmp does NOT run C++ destructors. The recovery point is at the CALL
// SITE, outside this function's frame — so a fault inside here skipped
// ~unique_lock() and left g_personality_lock_mutex LOCKED FOREVER. This runs
// three times per on_tick on the GAME THREAD, so the very next call would block
// on a mutex nothing would ever release: the game thread wedged permanently,
// and every set_ai_personality on the reader thread with it.
//
// The fault is not hypothetical — it is the reason the guard exists. ck-cg3b's
// own comment at the call sites says is_mapped() is a time-of-check test and
// the store is the time-of-use, so a page unmapped in between faults. That bead
// converted a crash into a permanent deadlock, which is worse: a crash is
// visible and a wedged game thread looks like a slow one.
//
// THE FIX IS TO NARROW WHAT THE LOCK COVERS, not to add another guard. The map
// read needs the lock; the engine stores do not. So: snapshot under a
// shared_lock (which is what the header documents for reads — this function was
// taking unique_lock for a pure read), release, then store. A longjmp out of
// the store loop now crosses no destructor that matters.
//
// The snapshot is a FIXED STACK BUFFER, deliberately: a std::vector here would
// leak its heap block on the same longjmp. A bounded one-time leak is far less
// severe than a deadlock, but the buffer costs nothing and leaks nothing.
void apply_personality_locks() {
    uint64_t tick = g_tick_id.load(std::memory_order_relaxed);
    if (tick < 5) return;

    struct Entry { uint32_t cid; int8_t axes[10]; bool locked[10]; };
    static constexpr size_t kMaxEntries = 256;   // the map is "a handful" in practice
    Entry entries[kMaxEntries];
    size_t n = 0, total = 0;

    {
        // Read-only: shared_lock, as personality_locks.h documents for reads.
        std::shared_lock<std::shared_mutex> lock(g_personality_lock_mutex);
        total = g_personality_locks.size();
        if (total == 0) return;
        for (const auto& [cid, pl] : g_personality_locks) {
            if (n >= kMaxEntries) break;
            entries[n].cid = cid;
            std::memcpy(entries[n].axes,   pl.axes,   sizeof entries[n].axes);
            std::memcpy(entries[n].locked, pl.locked, sizeof entries[n].locked);
            ++n;
        }
    }   // ---- LOCK RELEASED HERE, BEFORE ANY STORE THAT CAN FAULT ----

    // Fail LOUD rather than silently applying a subset: a truncated apply would
    // leave some steered axes un-written with nothing to say so, and the agent
    // would be steering characters the bridge had quietly stopped steering.
    if (total > n && g_logfile) {
        static uint64_t s_last_warn_tick = 0;
        if (tick - s_last_warn_tick >= 100) {
            s_last_warn_tick = tick;
            fprintf(g_logfile,
                "[CK3-Bridge] apply_personality_locks: %zu locked characters "
                "exceeds the %zu-entry snapshot buffer — only %zu applied this "
                "tick; the rest are NOT being steered (ck-1v1t)\n",
                total, kMaxEntries, n);
            fflush(g_logfile);
        }
    }

    for (size_t k = 0; k < n; ++k) {
        const Entry& e = entries[k];
        void* char_ptr = lookup_character(e.cid);
        if (!char_ptr || is_sentinel(char_ptr)) continue;
        if (!g_char_is_alive(char_ptr)) continue;

        uintptr_t core_data = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(char_ptr) + ck3::character::REALM_DATA);
        if (!core_data) continue;

        uintptr_t personality_ptr = safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR);
        if (!personality_ptr || !is_mapped(personality_ptr)) continue;

        for (int i = 0; i < 9; i++) {  // Engine has 9 axes, NOT 10 — writing i=9 corrupts adjacent memory
            if (e.locked[i]) {
                *reinterpret_cast<int8_t*>(personality_ptr + i) = e.axes[i];
            }
        }
    }
}

// Clear personality lock for a specific character
void clear_personality_lock(uint32_t char_id) {
    std::unique_lock<std::shared_mutex> lock(g_personality_lock_mutex);
    g_personality_locks.erase(char_id);
    save_personality_locks_locked();
}

// =============================================================================
// Personality Getter Hooks
// =============================================================================

// Shared hook: check lock map, return locked value or fall through to original logic.
// Called from parallel AI worker threads (10,000+ times/tick).
// Uses safe_read<T>() (process_vm_readv, lock-free) instead of is_mapped() to avoid
// 45,000+ shared_lock acquisitions per tick causing cache line contention.
static int personality_getter_hook(void* character, int axis) {
    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    if (c < 0x10000) return 0;  // null/low-address quick reject

    uint32_t cid = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;  // mask (A7 key consistency)
    if (cid == 0) return 0;

    // Check lock map (only lock in the hot path)
    {
        std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        auto it = g_personality_locks.find(cid);
        if (it != g_personality_locks.end() && it->second.locked[axis]) {
            return static_cast<int>(it->second.axes[axis]);
        }
    }

    // Not locked — replicate original getter via safe_read (lock-free)
    uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
    if (!core_data) return 0;
    uintptr_t personality_ptr = safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR);
    if (!personality_ptr) return 0;
    return static_cast<int>(safe_read<int8_t>(personality_ptr + axis));
}

// 9 entry points -- one per axis
extern "C" {
    int hook_ai_honor(void* c)        { return personality_getter_hook(c, 0); }
    int hook_ai_greed(void* c)        { return personality_getter_hook(c, 1); }
    int hook_ai_rationality(void* c)  { return personality_getter_hook(c, 2); }
    int hook_ai_energy(void* c)       { return personality_getter_hook(c, 3); }
    int hook_ai_boldness(void* c)     { return personality_getter_hook(c, 4); }
    int hook_ai_zeal(void* c)         { return personality_getter_hook(c, 5); }
    int hook_ai_vengefulness(void* c) { return personality_getter_hook(c, 6); }
    int hook_ai_compassion(void* c)   { return personality_getter_hook(c, 7); }
    int hook_ai_sociability(void* c)  { return personality_getter_hook(c, 8); }
}

void install_personality_hooks() {
    struct HookEntry {
        uintptr_t getter_offset;
        void* hook_fn;
        const char* name;
    };

    HookEntry hooks[] = {
        { ck3::funcs::AI_PERSONALITY_HONOR,        (void*)hook_ai_honor,        "honor" },
        { ck3::funcs::AI_PERSONALITY_GREED,        (void*)hook_ai_greed,        "greed" },
        { ck3::funcs::AI_PERSONALITY_RATIONALITY,  (void*)hook_ai_rationality,  "rationality" },
        { ck3::funcs::AI_PERSONALITY_ENERGY,       (void*)hook_ai_energy,       "energy" },
        { ck3::funcs::AI_PERSONALITY_BOLDNESS,     (void*)hook_ai_boldness,     "boldness" },
        { ck3::funcs::AI_PERSONALITY_ZEAL,         (void*)hook_ai_zeal,         "zeal" },
        { ck3::funcs::AI_PERSONALITY_VENGEFULNESS, (void*)hook_ai_vengefulness, "vengefulness" },
        { ck3::funcs::AI_PERSONALITY_COMPASSION,   (void*)hook_ai_compassion,   "compassion" },
        { ck3::funcs::AI_PERSONALITY_SOCIABILITY,  (void*)hook_ai_sociability,  "sociability" },
    };

    int installed = 0;
    for (auto& h : hooks) {
        uintptr_t addr = g_base_addr + h.getter_offset;

        // Use install_hook() for atomic INT3-first installation.
        // The personality hooks reimplement the getter logic, so the returned
        // trampoline (original function entry) is intentionally unused.
        void* trampoline = install_hook(addr, h.hook_fn);
        if (trampoline) {
            installed++;
            ADLOG("Personality hook INSTALLED: %s (via install_hook, trampoline=%p unused)\n",
                  h.name, trampoline);
        } else {
            ADLOG("Personality hook FAILED: %s (install_hook returned null)\n", h.name);
        }
    }

    ADLOG("Personality getter hooks: %d/9 installed\n", installed);
    ADLOG("Parametric getter hook DISABLED (using apply_personality_locks instead)\n");

    // ck-9fc3 — THE DECISION, RECORDED. This used to call
    // load_personality_locks() here: at hook-install time, with no idea which
    // save was loaded, straight into the live write set that
    // apply_personality_locks() pokes into characters 3x per tick. That is the
    // ck-9fc3 hazard in its worst form, because it crosses PROCESSES: one run's
    // steering landing on the next run's world, keyed by reused character ids.
    //
    // It never actually fired — install_personality_hooks() has itself had zero
    // callers (bridge.h declares it; nothing in bridge/src invokes it), so the
    // bead's "load_personality_locks() has zero callers" was right in effect and
    // slightly off in detail: the caller existed and was unreachable.
    //
    // The lock file is now loaded ONLY through world_identity_declare(), only
    // when the declared save identity proves the same save AND the same episode,
    // and the file itself carries the identity header that makes that decidable
    // (personality_persist.cpp). Process start is not a place where that can be
    // known, so nothing is restored here.
}

} // namespace bridge
