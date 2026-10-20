// marriage_observation_hook.cpp — Post-hook on INTERACTION_TRY_SEND to observe
// AI marriage/alliance interactions. Filters for marriage-type interactions and
// logs both successful and rejected proposals with full parameters.
//
// Architecture: INTERACTION_TRY_SEND is called whenever any character interaction
// is attempted (AI or player). We install an inline hook that calls the original
// function first, then checks if the interaction is marriage-related. If so, we
// read the 4 character refs from the TLS interaction context and store them in a
// thread-safe ring buffer. The IPC handler returns observed marriages to the agent.
//
// This is a pure observer — it does not modify any game state.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <shared_mutex>
#include <deque>
#include <chrono>
#include <cstring>

namespace bridge {

// =============================================================================
// Marriage Observation Record
// =============================================================================

struct MarriageObservationRecord {
    uint64_t tick;                       // game tick
    uint64_t timestamp_ms;               // wall-clock time (epoch ms)
    uint32_t actor_id;                   // ruler proposing (24-bit)
    uint32_t target_id;                  // target ruler / recipient (24-bit)
    uint32_t secondary_actor_id;         // person being married (actor's side)
    uint32_t secondary_recipient_id;     // person being married (target's side)
    char     interaction_name[64];       // e.g. "arrange_marriage"
    char     actor_name[128];            // human-readable actor name
    char     target_name[128];           // human-readable target name
    char     secondary_actor_name[128];  // person being married (actor's side)
    char     secondary_recipient_name[128]; // person being married (target's side)
    int8_t   result;                     // 0 = rejected, non-zero = accepted/sent
};

static std::shared_mutex g_marriage_obs_mutex;
static std::deque<MarriageObservationRecord> g_marriage_observations CK3_EARLY_INIT;
static constexpr size_t MAX_MARRIAGE_OBSERVATIONS = 64;

// Trampoline to original INTERACTION_TRY_SEND
static void* g_orig_interaction_try_send = nullptr;

// =============================================================================
// INTERACTION_TRY_SEND signature (from ai_eval_hook.cpp):
//   char fn_try_send(uintptr_t* tls_ctx, long extra)
// Returns: 0 = rejected/failed, non-zero = accepted/sent
// =============================================================================

using fn_interaction_try_send = char(*)(uintptr_t* tls_ctx, long extra);

// =============================================================================
// Marriage interaction name detection
// =============================================================================

static bool is_marriage_interaction(const char* name) {
    if (!name || name[0] == '<') return false;
    // CK3 marriage-type interaction names include:
    //   arrange_marriage, marry_*,  *_betrothal, *_concubine
    return (strstr(name, "marri") != nullptr ||
            strstr(name, "marry") != nullptr ||
            strstr(name, "betrothal") != nullptr ||
            strstr(name, "concubine") != nullptr);
}

// =============================================================================
// Post-Hook: called instead of INTERACTION_TRY_SEND
// =============================================================================

static char on_interaction_try_send(uintptr_t* tls_ctx, long extra) {
    // Call original first
    auto orig = reinterpret_cast<fn_interaction_try_send>(g_orig_interaction_try_send);
    char result = orig(tls_ctx, extra);

    if (!tls_ctx) return result;

    // Quick check: read interaction type pointer from context offset 0x000
    uintptr_t ctx_addr = reinterpret_cast<uintptr_t>(tls_ctx);
    uintptr_t interaction_ptr = safe_read<uintptr_t>(ctx_addr +
        ck3::interaction_ctx::INTERACTION_TYPE_PTR);
    if (!interaction_ptr || interaction_ptr < 0x10000) return result;

    // Read interaction name — cheap string check
    const char* name = get_interaction_name(reinterpret_cast<void*>(interaction_ptr));
    if (!is_marriage_interaction(name)) return result;

    // --- This is a marriage-type interaction. Capture the parameters. ---

    MarriageObservationRecord rec{};
    rec.tick = g_tick_id.load(std::memory_order_relaxed);
    rec.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    rec.result = result;

    strncpy(rec.interaction_name, name, sizeof(rec.interaction_name) - 1);

    // SIGSEGV protection around memory reads
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        // Read character refs from interaction context
        rec.actor_id = safe_read<uint32_t>(ctx_addr +
            ck3::interaction_ctx::ACTOR_CHAR_REF) & 0xFFFFFF;
        rec.target_id = safe_read<uint32_t>(ctx_addr +
            ck3::interaction_ctx::TARGET_CHAR_REF) & 0xFFFFFF;
        rec.secondary_actor_id = safe_read<uint32_t>(ctx_addr +
            ck3::interaction_ctx::SECONDARY_ACTOR_REF) & 0xFFFFFF;
        rec.secondary_recipient_id = safe_read<uint32_t>(ctx_addr +
            ck3::interaction_ctx::SECONDARY_RECIPIENT_REF) & 0xFFFFFF;

        // Resolve character names for human-readable logging
        uintptr_t actor_char = ad_db_lookup(ck3::globals::CHARACTER_DB, rec.actor_id);
        if (actor_char) {
            std::string n = ad_read_char_name(actor_char);
            strncpy(rec.actor_name, n.c_str(), sizeof(rec.actor_name) - 1);
        }

        uintptr_t target_char = ad_db_lookup(ck3::globals::CHARACTER_DB, rec.target_id);
        if (target_char) {
            std::string n = ad_read_char_name(target_char);
            strncpy(rec.target_name, n.c_str(), sizeof(rec.target_name) - 1);
        }

        // Secondary actor (person being married, actor's side)
        if (rec.secondary_actor_id != 0 && rec.secondary_actor_id != 0xFFFFFF) {
            uintptr_t sec_actor = ad_db_lookup(ck3::globals::CHARACTER_DB,
                                                rec.secondary_actor_id);
            if (sec_actor) {
                std::string n = ad_read_char_name(sec_actor);
                strncpy(rec.secondary_actor_name, n.c_str(),
                        sizeof(rec.secondary_actor_name) - 1);
            }
        }

        // Secondary recipient (person being married, target's side)
        if (rec.secondary_recipient_id != 0 && rec.secondary_recipient_id != 0xFFFFFF) {
            uintptr_t sec_recip = ad_db_lookup(ck3::globals::CHARACTER_DB,
                                                rec.secondary_recipient_id);
            if (sec_recip) {
                std::string n = ad_read_char_name(sec_recip);
                strncpy(rec.secondary_recipient_name, n.c_str(),
                        sizeof(rec.secondary_recipient_name) - 1);
            }
        }

        // Store in ring buffer
        {
            std::unique_lock<std::shared_mutex> lock(g_marriage_obs_mutex);
            g_marriage_observations.push_back(rec);
            while (g_marriage_observations.size() > MAX_MARRIAGE_OBSERVATIONS) {
                g_marriage_observations.pop_front();
            }
        }

        // Log to bridge log
        if (g_trace) ADLOG("MARRIAGE %s: interaction=%s actor=%u(%s) target=%u(%s) "
              "sec_actor=%u(%s) sec_recip=%u(%s) tick=%lu\n",
              result ? "ACCEPTED" : "REJECTED",
              rec.interaction_name,
              rec.actor_id, rec.actor_name,
              rec.target_id, rec.target_name,
              rec.secondary_actor_id, rec.secondary_actor_name,
              rec.secondary_recipient_id, rec.secondary_recipient_name,
              rec.tick);
    } else {
        // SIGSEGV recovery
        ADLOG("marriage_observation_hook: SIGSEGV reading interaction context at %p\n",
              tls_ctx);
    }
    g_in_protected_region = false;

    return result;
}

// =============================================================================
// Initialization
// =============================================================================

void init_marriage_observation_hook() {
    uintptr_t target = bridge::game_fn(ck3::funcs::INTERACTION_TRY_SEND);
    g_orig_interaction_try_send = install_hook(target,
        reinterpret_cast<void*>(on_interaction_try_send));

    if (g_orig_interaction_try_send) {
        ADLOG("marriage_observation_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_interaction_try_send);
    } else {
        ADLOG("marriage_observation_hook: FAILED to install at 0x%lx\n", target);
    }
}

// =============================================================================
// Query API (called from IPC thread)
// =============================================================================

bool do_query_marriage_observations(const char* params_json, std::string* out) {
    if (!out) return false;

    int limit = json_get_int(params_json, "limit");
    if (limit <= 0 || limit > 64) limit = 64;

    uint64_t since_tick = 0;
    if (json_has_key(params_json, "since_tick")) {
        since_tick = static_cast<uint64_t>(json_get_int(params_json, "since_tick"));
    }

    // Optional: filter by result (0=rejected, 1=accepted, -1=all)
    int filter_result = -1;
    if (json_has_key(params_json, "accepted_only")) {
        if (json_get_int(params_json, "accepted_only") != 0) {
            filter_result = 1;
        }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_marriage_observations");
    r.key("success"); r.value_bool(true);
    r.key("current_tick"); r.value_uint(g_tick_id.load(std::memory_order_relaxed));
    r.key("hook_installed"); r.value_bool(g_orig_interaction_try_send != nullptr);

    r.key("observations"); r.begin_array();
    {
        std::shared_lock<std::shared_mutex> lock(g_marriage_obs_mutex);
        int emitted = 0;
        // Iterate in reverse (newest first)
        for (auto it = g_marriage_observations.rbegin();
             it != g_marriage_observations.rend() && emitted < limit; ++it) {
            if (it->tick < since_tick) continue;
            if (filter_result >= 0 && (it->result != 0) != (filter_result != 0)) continue;

            r.begin_object();
            r.key("tick"); r.value_uint(it->tick);
            r.key("timestamp_ms"); r.value_uint(it->timestamp_ms);
            r.key("interaction"); r.value_string(it->interaction_name);
            r.key("accepted"); r.value_bool(it->result != 0);

            r.key("actor_id"); r.value_uint(it->actor_id);
            r.key("actor_name"); r.value_string(it->actor_name);
            r.key("target_id"); r.value_uint(it->target_id);
            r.key("target_name"); r.value_string(it->target_name);

            r.key("secondary_actor_id"); r.value_uint(it->secondary_actor_id);
            r.key("secondary_actor_name"); r.value_string(it->secondary_actor_name);
            r.key("secondary_recipient_id"); r.value_uint(it->secondary_recipient_id);
            r.key("secondary_recipient_name"); r.value_string(it->secondary_recipient_name);

            r.end_object();
            emitted++;
        }
    }
    r.end_array();

    r.key("total_recorded"); r.value_uint(
        static_cast<uint64_t>(g_marriage_observations.size()));
    r.end_object();

    *out = r.take();
    return true;
}

} // namespace bridge
