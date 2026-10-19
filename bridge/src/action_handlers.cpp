// action_handlers.cpp — Action execution handlers: send_interaction, declare_war,
// send_decision, respond_event, intent queue IPC, pause/speed, AI subsystem toggles.
//
// Split from action_dispatch.cpp (mechanical refactor, no logic changes).

#include "action_dispatch_internal.h"
#include "refusal_record.h"
#include "scope_slot_policy.h"   // ck-zgh4: the scope latch + its one refusal text
#include "lobby_view_hook.h"
#include "observer_data_functions.h"
#include "script_block_parser.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <rapidjson/document.h>

namespace bridge {

// =============================================================================
// Intent Queue IPC Handlers
// =============================================================================

bool do_queue_intent(const char* params_json, std::string* response_out) {
    // intent_type selects the dispatch path. Defaults to "interaction".
    // For "decision", the action label comes from "decision_type" (preferred)
    // or "interaction_type" (fallback so older callers can pass any field).
    const char* intent_type_str = json_get_string(params_json, "intent_type");
    IntentType intent_type = IntentType::INTERACTION;
    if (intent_type_str && intent_type_str[0]) {
        if (strcmp(intent_type_str, "decision") == 0) {
            intent_type = IntentType::DECISION;
        } else if (strcmp(intent_type_str, "war_declare") == 0 ||
                   strcmp(intent_type_str, "war") == 0) {
            intent_type = IntentType::WAR_DECLARE;
        } else if (strcmp(intent_type_str, "interaction") != 0) {
            if (response_out) *response_out = R"({"success":false,"action":"queue_intent","reason":"unknown_intent_type"})";
            return false;
        }
    }

    const char* interaction_type = json_get_string(params_json, "interaction_type");
    if (intent_type == IntentType::DECISION) {
        // Decision label may be passed as "decision_type" — preferred for clarity.
        const char* decision_type = json_get_string(params_json, "decision_type");
        if (decision_type && decision_type[0]) interaction_type = decision_type;
    }
    if (!interaction_type || !interaction_type[0]) {
        if (response_out) {
            *response_out = (intent_type == IntentType::DECISION)
                ? R"({"success":false,"action":"queue_intent","reason":"missing_decision_type"})"
                : R"({"success":false,"action":"queue_intent","reason":"missing_interaction_type"})";
        }
        return false;
    }

    // Resolve actor — requires explicit actor_id or a previously set controlled character.
    // No PLAYER_CHAR_ID fallback: the LLM controls AI characters, not the player.
    int actor_id_raw = json_get_int(params_json, "actor_id");
    uint32_t actor_id;
    if (actor_id_raw > 0) {
        actor_id = static_cast<uint32_t>(actor_id_raw);
    } else {
        uint32_t resolved = g_controlled_primary.load(std::memory_order_relaxed);
        if (resolved == 0 || resolved == 0xFFFFFFFF) {
            if (response_out) *response_out = R"({"success":false,"action":"queue_intent","reason":"actor_id_required"})";
            return false;
        }
        actor_id = resolved;
    }
    if (reject_player_actor(actor_id, "queue_intent", response_out)) return false;

    int target_id_raw = json_get_int(params_json, "target_id");
    uint32_t target_id = (target_id_raw > 0) ? static_cast<uint32_t>(target_id_raw) : 0;

    int max_age = json_get_int(params_json, "max_age_ticks");
    uint32_t max_age_ticks = (max_age > 0) ? static_cast<uint32_t>(max_age) : 100;
    bool bypass = json_get_bool(params_json, "bypass", false);

    // War-specific parameters
    int claimant_raw = json_get_int(params_json, "claimant_id");
    uint32_t claimant_id = (claimant_raw > 0) ? static_cast<uint32_t>(claimant_raw) : 0;
    int cb_index = json_get_int(params_json, "cb_index");
    if (cb_index == 0) {
        // json_get_int returns 0 for missing keys; distinguish "not provided" from "index 0"
        // by checking if the key actually exists in the JSON string
        const char* cb_idx_str = strstr(params_json, "\"cb_index\"");
        if (!cb_idx_str) cb_index = -1;  // not provided -> auto-select
    }

    // Title-specific parameter (for revoke_title, declare_war claim)
    // Try "title_id" first, fall back to "cb_title_id" (declare_war convention)
    int title_id_raw = json_get_int(params_json, "title_id");
    if (title_id_raw <= 0) {
        int cb_tid = json_get_int(params_json, "cb_title_id");
        if (cb_tid > 0) title_id_raw = cb_tid;
    }

    // Marriage-specific parameters
    int sec_actor_raw = json_get_int(params_json, "secondary_actor_id");
    uint32_t secondary_actor_id = (sec_actor_raw > 0) ? static_cast<uint32_t>(sec_actor_raw) : 0;
    int sec_recip_raw = json_get_int(params_json, "secondary_recipient_id");
    uint32_t secondary_recipient_id = (sec_recip_raw > 0) ? static_cast<uint32_t>(sec_recip_raw) : 0;

    // Copy interaction_type since json_get_string uses thread-local buffer
    std::string itype(interaction_type);

    // Generate a unique request_id for tracking this intent
    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "qi_%u_%lu_%u",
             actor_id & 0xFFFFFF, (unsigned long)current_tick,
             generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    bool ok = queue_intent(actor_id, target_id, itype.c_str(), max_age_ticks,
                           request_id.c_str(), bypass, claimant_id, cb_index,
                           secondary_actor_id, secondary_recipient_id,
                           title_id_raw, /*debug_all_paths=*/false,
                           intent_type);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(ok);
        r.key("action"); r.value_string("queue_intent");
        const char* type_label = (intent_type == IntentType::DECISION) ? "decision" :
                                 (intent_type == IntentType::WAR_DECLARE) ? "war_declare" :
                                 "interaction";
        r.key("intent_type"); r.value_string(type_label);
        if (intent_type == IntentType::DECISION) {
            r.key("decision_type"); r.value_string(itype.c_str());
        } else {
            r.key("interaction_type"); r.value_string(itype.c_str());
        }
        r.key("actor_id"); r.value_uint(actor_id & 0xFFFFFF);
        r.key("target_id"); r.value_uint(target_id & 0xFFFFFF);
        if (claimant_id > 0) { r.key("claimant_id"); r.value_uint(claimant_id & 0xFFFFFF); }
        if (cb_index >= 0) { r.key("cb_index"); r.value_int(cb_index); }
        if (secondary_actor_id > 0) { r.key("secondary_actor_id"); r.value_uint(secondary_actor_id & 0xFFFFFF); }
        if (secondary_recipient_id > 0) { r.key("secondary_recipient_id"); r.value_uint(secondary_recipient_id & 0xFFFFFF); }
        r.key("max_age_ticks"); r.value_uint(max_age_ticks);
        // ck-hzq3: renamed from "request_id" to avoid collision with the
        // top-level IPC request_id injected by ipc_server.cpp. Python's
        // json.loads keeps the LAST duplicate-key value, so when both
        // coexisted the client's correlation UUID was silently overwritten
        // by this internal intent tracking ID → all responses timed out.
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        // ck-jsis0: canonical dispatch identifier alias — query_dispatch_result
        // accepts either name.
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        r.key("pending_count"); r.value_uint(pending_intent_count());
        r.end_object();
        *response_out = r.take();
    }
    return ok;
}

bool do_query_intents(const char* /*params_json*/, std::string* response_out) {
    if (!response_out) return true;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_intents");

    // Pending intents from the consolidated queue
    r.key("pending"); r.begin_array();
    {
        uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
        auto pending = g_intent_queue.snapshot();
        for (const auto& intent : pending) {
            r.begin_object();
            r.key("actor_id"); r.value_uint(intent.actor_id & 0xFFFFFF);
            r.key("target_id"); r.value_uint(intent.target_id & 0xFFFFFF);
            r.key("interaction"); r.value_string(intent.interaction_name.c_str());
            r.key("type"); r.value_uint(static_cast<uint32_t>(intent.type));
            r.key("age_ticks"); r.value_uint(current_tick - intent.queued_tick);
            r.key("ttl_ticks"); r.value_uint(intent.ttl_ticks);
            r.key("request_id"); r.value_string(intent.request_id.c_str());
            r.end_object();
        }
    }
    r.end_array();

    r.key("pending_count"); r.value_uint(g_intent_queue.pending_count());

    // Recently completed intents (for diagnosing dispatch failures)
    r.key("completed"); r.begin_array();
    {
        auto completed = g_intent_queue.completed_snapshot(10);
        for (const auto& c : completed) {
            r.begin_object();
            r.key("actor_id"); r.value_uint(c.actor_id & 0xFFFFFF);
            r.key("target_id"); r.value_uint(c.target_id & 0xFFFFFF);
            r.key("interaction"); r.value_string(c.interaction_name.c_str());
            r.key("request_id"); r.value_string(c.request_id.c_str());
            r.key("executed"); r.value_bool(c.executed);
            r.key("rejected"); r.value_bool(c.rejected);
            if (c.rejected && c.rejection_reason[0]) {
                r.key("rejection_reason"); r.value_string(c.rejection_reason);
            }
            // #98: derive a readable terminal status so the agent can tell "the
            // steered AI declined this" from "still pending" against its dispatch_id
            // (intent(action="list") is the surface the agent polls). Mirrors the
            // status derivation in do_query_intent_results. A self-disadvantaging move
            // (disinherit, grant own title) is terminally resolved via the v5 AI path
            // with rejection_reason "ai_no_valid_target" (or "ttl_expired"); the agent
            // must STOP re-queuing it, not loop.
            const char* status;
            if (c.executed && !c.rejected) {
                status = "executed";
            } else if (c.rejected) {
                bool is_crash = (strstr(c.rejection_reason, "SIGSEGV") != nullptr) ||
                                (strstr(c.rejection_reason, "SIGALRM") != nullptr) ||
                                (strstr(c.rejection_reason, "exception") != nullptr);
                if (is_crash) {
                    status = "crashed";
                } else {
                    // The steered AI evaluated and refused, or the intent TTL-expired
                    // before its AI tick — either way it will NOT land on re-queue.
                    bool declined = (strstr(c.rejection_reason, "ai_no_valid_target") != nullptr) ||
                                    (strstr(c.rejection_reason, "ttl_expired") != nullptr) ||
                                    (strstr(c.rejection_reason, "interaction_not_valid_for_target") != nullptr) ||
                                    (strstr(c.rejection_reason, "target_would_reject") != nullptr);
                    status = declined ? "declined" : "rejected";
                }
            } else {
                status = "rejected";  // consumed but not executed
            }
            r.key("status"); r.value_string(status);
            r.end_object();
        }
    }
    r.end_array();

    r.end_object();
    *response_out = r.take();
    return true;
}

bool do_query_intent_results(const char* params_json, std::string* response_out) {
    if (!response_out) return true;

    int max_count_raw = json_get_int(params_json, "max_count");
    size_t max_count = (max_count_raw > 0) ? static_cast<size_t>(max_count_raw) : 16;

    auto completed = g_intent_queue.completed_snapshot(max_count);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_intent_results");
    r.key("count"); r.value_uint(completed.size());

    r.key("results"); r.begin_array();
    for (const auto& c : completed) {
        r.begin_object();
        r.key("intent_id"); r.value_string(c.request_id.c_str());

        // action: interaction name or intent type label
        if (!c.interaction_name.empty()) {
            r.key("action_name"); r.value_string(c.interaction_name.c_str());
        } else {
            const char* type_label = (c.type == IntentType::WAR_DECLARE) ? "war_declare" :
                                     (c.type == IntentType::DECISION) ? "decision" :
                                     "interaction";
            r.key("action_name"); r.value_string(type_label);
        }

        // Derive status: success / rejected / crashed
        const char* status;
        if (c.executed && !c.rejected) {
            status = "success";
        } else if (c.rejected) {
            // Check if rejection was due to a crash (SIGSEGV, SIGALRM, exception)
            bool is_crash = (strstr(c.rejection_reason, "SIGSEGV") != nullptr) ||
                            (strstr(c.rejection_reason, "SIGALRM") != nullptr) ||
                            (strstr(c.rejection_reason, "exception") != nullptr);
            status = is_crash ? "crashed" : "rejected";
        } else {
            status = "rejected";  // consumed but not executed
        }
        r.key("status"); r.value_string(status);

        if (c.rejected && c.rejection_reason[0]) {
            r.key("rejection_reason"); r.value_string(c.rejection_reason);
        }

        r.key("tick_id"); r.value_uint(c.queued_tick);
        r.key("actor_id"); r.value_uint(c.actor_id & 0xFFFFFF);
        r.key("target_id"); r.value_uint(c.target_id & 0xFFFFFF);
        r.end_object();
    }
    r.end_array();

    r.end_object();
    *response_out = r.take();
    return true;
}

// ck-jsis0: probe a single dispatch by its id. Disambiguates the three states
// the harness previously could not distinguish through query_intent_results
// alone (which surfaced only completed entries and only the most recent N):
//
//   "success"   — found in completed ring, executed=true
//   "rejected"  — found in completed ring, rejected=true (non-crash reason)
//   "crashed"   — found in completed ring, rejection_reason contains SIGSEGV /
//                 SIGALRM / exception (bridge bug, not a game-logic reject)
//   "pending"   — still in the live queue; drain has not consumed it yet.
//                 Returned when the harness queries before the drain hook fires
//                 (or while the actor's tick has not advanced the queue).
//   "not_found" — neither queue has it. Possible causes: TTL expired AND was
//                 then evicted from the completed ring; the id never belonged
//                 to this bridge instance (typo / cross-restart); or the
//                 caller passed an empty id.
//
// Accepts either "dispatch_id" or "intent_request_id" (alias) so older callers
// keep working without code changes.
bool do_query_dispatch_result(const char* params_json, std::string* response_out) {
    if (!response_out) return true;

    const char* rid_raw = json_get_string(params_json, "dispatch_id");
    if (!rid_raw || !rid_raw[0]) {
        rid_raw = json_get_string(params_json, "intent_request_id");
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_dispatch_result");

    if (!rid_raw || !rid_raw[0]) {
        r.key("success"); r.value_bool(false);
        r.key("status"); r.value_string("missing_dispatch_id");
        r.end_object();
        *response_out = r.take();
        return false;
    }

    std::string rid(rid_raw);
    r.key("dispatch_id"); r.value_string(rid.c_str());

    Intent found{};
    if (g_intent_queue.find_completed_by_request_id(rid, &found)) {
        const char* status;
        if (found.executed && !found.rejected) {
            status = "success";
        } else if (found.rejected) {
            bool is_crash = (strstr(found.rejection_reason, "SIGSEGV") != nullptr) ||
                            (strstr(found.rejection_reason, "SIGALRM") != nullptr) ||
                            (strstr(found.rejection_reason, "exception") != nullptr);
            status = is_crash ? "crashed" : "rejected";
        } else {
            // consumed but neither executed nor rejected — treat as rejected
            status = "rejected";
        }
        r.key("success"); r.value_bool(true);
        r.key("status"); r.value_string(status);
        if (found.rejected && found.rejection_reason[0]) {
            r.key("rejection_reason"); r.value_string(found.rejection_reason);
        }
        if (!found.interaction_name.empty()) {
            r.key("action_name"); r.value_string(found.interaction_name.c_str());
        }
        r.key("tick_id"); r.value_uint(found.queued_tick);
        r.key("actor_id"); r.value_uint(found.actor_id & 0xFFFFFF);
        r.key("target_id"); r.value_uint(found.target_id & 0xFFFFFF);
        r.end_object();
        *response_out = r.take();
        return true;
    }

    if (g_intent_queue.find_pending_by_request_id(rid, &found)) {
        uint64_t current = g_tick_id.load(std::memory_order_relaxed);
        uint64_t age = (current >= found.queued_tick) ? (current - found.queued_tick) : 0;
        r.key("success"); r.value_bool(true);
        r.key("status"); r.value_string("pending");
        if (!found.interaction_name.empty()) {
            r.key("action_name"); r.value_string(found.interaction_name.c_str());
        }
        r.key("tick_id"); r.value_uint(found.queued_tick);
        r.key("age_ticks"); r.value_uint(age);
        r.key("ttl_ticks"); r.value_uint(found.ttl_ticks);
        r.key("actor_id"); r.value_uint(found.actor_id & 0xFFFFFF);
        r.key("target_id"); r.value_uint(found.target_id & 0xFFFFFF);
        r.end_object();
        *response_out = r.take();
        return true;
    }

    r.key("success"); r.value_bool(true);
    r.key("status"); r.value_string("not_found");
    r.end_object();
    *response_out = r.take();
    return true;
}

bool do_cancel_intent(const char* params_json, std::string* response_out) {
    const char* request_id = json_get_string(params_json, "request_id");
    if (!request_id || !request_id[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"cancel_intent","reason":"missing_request_id"})";
        return false;
    }

    std::string rid(request_id);
    bool ok = g_intent_queue.cancel_by_request_id(rid);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(ok);
        r.key("action"); r.value_string("cancel_intent");
        // ck-hzq3: renamed to avoid colliding with IPC-injected request_id.
        r.key("intent_request_id"); r.value_string(rid.c_str());
        // ck-jsis0: canonical dispatch identifier alias.
        r.key("dispatch_id"); r.value_string(rid.c_str());
        if (!ok) {
            r.key("reason"); r.value_string("not_found_or_already_consumed");
        }
        r.end_object();
        *response_out = r.take();
    }
    return ok;
}

bool do_clear_intents(const char* /*params_json*/, std::string* response_out) {
    size_t cleared = pending_intent_count();
    clear_pending_intents();
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("clear_intents");
        r.key("cleared_count"); r.value_uint(cleared);
        r.key("pending_count"); r.value_uint(pending_intent_count());
        r.end_object();
        *response_out = r.take();
    }
    return true;
}

// =============================================================================
// Existing Action Handlers
// =============================================================================

bool do_send_interaction(const char* params_json, std::string* response_out) {

    const char* type = json_get_string(params_json, "interaction_type");
    if (!type) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_interaction: missing interaction_type\n");
        if (response_out) *response_out = R"({"success":false,"action":"send_interaction","reason":"missing_interaction_type"})";
        return false;
    }

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "send_interaction", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_interaction: no living actor character\n");
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("send_interaction");
            r.key("reason"); r.value_string("no_living_actor");
            r.key("attempted_type"); r.value_string(type);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    void* interaction = lookup_interaction(type);
    if (!interaction) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_interaction: unknown interaction '%s'\n", type);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("send_interaction");
            r.key("reason"); r.value_string("interaction_not_found");
            r.key("attempted_type"); r.value_string(type);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    uint32_t actor_id = read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID);
    int target_id = json_get_int(params_json, "target_id");
    int title_id = json_get_int(params_json, "title_id");
    bool bypass = json_get_bool(params_json, "bypass", false);
    bool force_accept = json_get_bool(params_json, "force_accept", false);
    // §3b (re_heir_guardian_RE.md): the act/send_interaction path previously DROPPED
    // the secondary scopes, so marriage breeding AND educate_child guardianship
    // dispatched with secondaries=0 (silent no-op). Mirror do_queue_intent (:100-103).
    int sec_actor_raw = json_get_int(params_json, "secondary_actor_id");
    int sec_recip_raw = json_get_int(params_json, "secondary_recipient_id");

    // Copy type since json_get_string uses thread-local buffer
    std::string interaction_name(type);

    // Generate a unique request_id for tracking this intent
    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "si_%u_%lu_%u",
             actor_id & 0xFFFFFF, (unsigned long)current_tick,
             generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    // Compute interaction hash for matching
    uint32_t interaction_hash = game_string_hash(interaction_name.c_str(),
                                                  static_cast<uint32_t>(interaction_name.size()));

    // Enqueue intent instead of direct dispatch.
    Intent intent = {};
    intent.type = IntentType::INTERACTION;
    intent.actor_id = actor_id;
    intent.target_id = (target_id >= 0) ? static_cast<uint32_t>(target_id) : 0;
    intent.interaction_hash = interaction_hash;
    intent.interaction_name = interaction_name;
    intent.title_id = title_id;
    intent.secondary_actor_id     = (sec_actor_raw > 0) ? static_cast<uint32_t>(sec_actor_raw) : 0;
    intent.secondary_recipient_id = (sec_recip_raw > 0) ? static_cast<uint32_t>(sec_recip_raw) : 0;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 10;
    intent.request_id = request_id;
    intent.force_bypass = bypass;
    intent.force_accept = force_accept;

    g_intent_queue.enqueue(std::move(intent));

    ADLOG("BT_TRACE do_send_interaction: enqueued internal_rid=%s itype=%s actor=%u target=%d — building response\n",
          request_id.c_str(), interaction_name.c_str(), actor_id & 0xFFFFFF, target_id);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("status"); r.value_string("queued");
        r.key("action"); r.value_string("send_interaction");
        r.key("interaction_type"); r.value_string(interaction_name.c_str());
        r.key("method"); r.value_string("intent_dispatch");
        if (target_id >= 0) {
            r.key("target_id"); r.value_int(target_id);
        }
        if (intent.secondary_actor_id)     { r.key("secondary_actor_id");     r.value_uint(intent.secondary_actor_id & 0xFFFFFF); }
        if (intent.secondary_recipient_id) { r.key("secondary_recipient_id"); r.value_uint(intent.secondary_recipient_id & 0xFFFFFF); }
        // ck-hzq3: renamed to avoid colliding with IPC-injected request_id.
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        // ck-jsis0: canonical dispatch identifier alias. Surfaces the same value
        // as intent_request_id so callers can use the unified query_dispatch_result
        // probe without first re-deriving which queue the action enqueued into.
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (bypass) { r.key("bypass"); r.value_bool(true); }
        r.key("pending_count"); r.value_uint(g_intent_queue.pending_count());
        r.end_object();
        *response_out = r.take();
    }

    ADLOG("BT_TRACE do_send_interaction: returning true internal_rid=%s response_size=%zu\n",
          request_id.c_str(),
          response_out ? response_out->size() : 0);

    return true;
}

bool do_send_decision(const char* params_json, std::string* response_out) {
    const char* type = json_get_string(params_json, "decision_type");
    if (!type) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_decision: missing decision_type\n");
        if (response_out) *response_out = R"({"success":false,"action":"send_decision","reason":"missing_decision_type"})";
        return false;
    }

    // ── ck-23hv: ACCEPT character_id TOO, AND SAY WHICH KEY WAS USED ────────
    // resolve_actor reads ONLY "actor_id". ck3agent's tools.py:294 builds this
    // call as {"decision_type": ..., "character_id": actor_id} — so the agent's
    // chosen actor was DROPPED and every send_decision in every episode ran
    // against get_controlled_character() instead. No error, no warning: the
    // parameter is well-formed, accepted by the JSON parser, and ignored. The
    // logged character_id was decoration (ck-zgh4, from two episodes' logs).
    //
    // FIXED HERE RATHER THAN IN resolve_actor, deliberately. Eighteen other
    // handlers share that resolver and I could NOT establish by static scan
    // whether any of them reads character_id for a different purpose — three
    // attempts, each imprecise, one of them matching a comment. A fallback in
    // the shared resolver would change the meaning of a call site I cannot
    // enumerate; a fallback HERE touches one handler whose semantics I know.
    //
    // actor_id still wins, so nothing that works today changes.
    void* actor = resolve_actor(params_json);
    const char* actor_key = "actor_id";
    if (json_get_int(params_json, "actor_id") <= 0) {
        const int cid = json_get_int(params_json, "character_id");
        if (cid > 0) {
            actor = lookup_character(static_cast<uint32_t>(cid));
            actor_key = "character_id";
        } else {
            actor_key = "controlled_default";
        }
    }
    // ck-23hv: AND THE RESOLVED ACTOR'S OWN ID, not just the key. The logged
    // character_id was the IGNORED parameter, so no log in either episode says
    // which character a decision actually ran against — and
    // get_controlled_character() is not a constant: g_controlled_primary is
    // stored at four sites and re-pointed by the dead-ruler handoff on
    // succession. So two calls minutes apart can have different real actors
    // while logging identical decorative ids.
    //
    // That is not hypothetical bookkeeping: it means the per-character
    // comparisons drawn from arma4 and arma5 — "same type, same character,
    // different outcome" — were drawn from the decorative field, and the actual
    // actor is unknown for every send_decision ever logged. This line is what
    // makes the next capture interpretable.
    if (g_logfile) fprintf(g_logfile,
        "[CK3-Bridge] send_decision: actor resolved via %s -> character %u\n",
        actor_key, actor ? (char_get_id(actor) & 0xFFFFFF) : 0u);
    if (reject_player_actor(actor, "send_decision", response_out)) return false;

    // ── ck-23hv: RESOLVE THE CHAIN THE ENGINE WILL, AND REFUSE IF IT IS NULL ──
    // AI_DECISION_EVALUATION's FIRST act, before any gate, is
    //     controller = *(core_data + AI_CONTROLLER)      (FUN_06788160)
    //     FUN_04e52ed0(controller, 3, &out)
    // which reads *(controller + CHARACTER_PTR) and hands it UNCHECKED to five
    // getters in a row — GetPrestige, GetPiety, GetGold and two more. A null
    // there is fault_addr 0x1a8 at GetPrestige's first instruction, on the
    // conductor thread, six times across two episodes (ck-duv5 / ck-65lv).
    //
    // WHY A REFUSAL AND NOT A REPAIR. The engine's own helpers for this same
    // object (FUN_04e20b20, FUN_04e20bb0, both called BY the faulting function)
    // null-check EXT_STATE_PTR at +0x20 and return 0 — measured from the
    // disassembly by ck-zgh4. Same object, adjacent field, defended twice. So a
    // null pointer in a CAIStrategy is a state the engine expects; what it does
    // not expect is CHARACTER_PTR being null at this one consumer. We cannot fix
    // the engine's missing check, and we do not need the cause to stop feeding
    // it an input it cannot survive.
    //
    // Third end of the same rope: the actor is validated, the decision pointer
    // is validated, and now the controller binding the engine will dereference.
    // Every read is safe_read (process_vm_readv — cannot fault the process).
    {
        const uintptr_t actor_addr = reinterpret_cast<uintptr_t>(actor);
        // ck-t5bc: NAME THE FIRST HOP TOO. It was a bare 0x1a8 beside two named
        // constants — correctness-neutral, and wrong in two ways that cost
        // nothing to fix. The offset travels if the header ever moves it, and
        // more usefully the hop becomes VISIBLE to the cut-time marker check,
        // which names the other two constants and could not see this one. A
        // verification that cannot see a third of what it verifies is the
        // count-not-presence problem aimed at the deploy gate.
        //
        // REALM_DATA is documented "null if unlanded", so an UNLANDED actor
        // legitimately has core == 0 — and refusing there is still correct,
        // because the engine would fault reading *(null + 0x278). ck-t5bc
        // checked that before gating: "the guard refuses a legitimate state"
        // is the failure mode this shape of fix must be audited for, and the
        // three distinct reason codes keep that case distinguishable.
        const uintptr_t core =
            safe_read<uintptr_t>(actor_addr + ck3::character::REALM_DATA);
        const uintptr_t ctrl = core ? safe_read<uintptr_t>(
                                   core + ck3::core_data::AI_CONTROLLER) : 0;
        const uintptr_t bound = ctrl ? safe_read<uintptr_t>(
                                   ctrl + ck3::ai_ctrl::CHARACTER_PTR) : 0;
        if (!core || !ctrl || !bound) {
            bridge::record_refusal(bridge::RefusalReason::kAiControllerUnbound);
            const char* which = !core ? "core_data" : (!ctrl ? "ai_controller"
                                                             : "controller_character_ptr");
            if (g_logfile) fprintf(g_logfile,
                "[CK3-Bridge] send_decision '%s': REFUSING — %s is null "
                "(core=0x%lx ctrl=0x%lx bound=0x%lx). AI_DECISION_EVALUATION "
                "would read controller+0x18 and hand it to GetPrestige "
                "unchecked.\n",
                type, which, (unsigned long)core, (unsigned long)ctrl,
                (unsigned long)bound);
            if (response_out) {
                ResponseBuilder r;
                r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("send_decision");
                r.key("reason"); r.value_string("ai_controller_unbound");
                r.key("detail"); r.value_string(which);
                // ck-23hv / e0's condition: the refusal must be COUNTABLE per
                // bundle, not just visible in a log. decision_capture.py reads
                // success, reason and character_id off the reply and emits them
                // as an actions[] entry, so carrying the RESOLVED actor here is
                // what makes the fire-rate a measured covariate with a subject
                // rather than a bare count. Without it the bundle records that
                // a refusal happened and not to whom — which is the same gap
                // that made the per-character question unanswerable for the six
                // captures we already have.
                r.key("character_id");
                r.value_uint(actor ? (char_get_id(actor) & 0xFFFFFF) : 0u);
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
    }
    if (!actor || !g_char_is_alive(actor)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_decision: no living actor character\n");
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("send_decision");
            r.key("reason"); r.value_string("no_living_actor");
            r.key("attempted_type"); r.value_string(type);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    void* decision = lookup_decision(type);
    if (!decision) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_decision: unknown decision '%s'\n", type);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("send_decision");
            r.key("reason"); r.value_string("decision_not_found");
            r.key("attempted_type"); r.value_string(type);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // Validate decision via vtable[7] (CDecisionType::is_valid) — confirmed in Phase 18
    // Signature: bool(void*). Returns false for null sentinel TPdxNullObject<CDecisionType>.
    uintptr_t decision_addr = reinterpret_cast<uintptr_t>(decision);
    uintptr_t vtable = read<uintptr_t>(decision_addr);

    // SIGSEGV protection for vtable call (decision pointer could be stale)
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("send_decision");
            r.key("reason"); r.value_string("vtable_validation_crashed");
            r.key("attempted_type"); r.value_string(type);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    bool valid = false;
    try {
        auto is_valid = reinterpret_cast<bool(*)(void*)>(
            read<uintptr_t>(vtable + ck3::interaction_cmd::VTABLE_IS_VALID));
        valid = is_valid(decision);
    } catch (...) {
        valid = false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!valid) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_decision: decision '%s' not valid\n", type);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("send_decision");
            r.key("reason"); r.value_string("vtable_validation_failed");
            r.key("attempted_type"); r.value_string(type);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // AI_TRY_DECISION_CORE — Phase 18 confirmed:
    // Signature: std::string*(std::string* result_out, CCharacter* char, CDecisionType* decision)
    // 3 params (not 4). Returns diagnostic string via hidden first param.
    // On success: "Took decision. ai_will_do was %i" + enqueues CExecuteDecisionCommand
    // On failure: "Does not fulfill ai_potential" / "cannot afford" / "cannot take" / etc.
    auto ai_try_decision = reinterpret_cast<void*(*)(void*, void*, void*)>(
        g_base_addr + ck3::funcs::AI_TRY_DECISION_CORE);

    // Use aligned buffer for std::string (24 bytes SSO + padding)
    // Must call destructor to prevent memory leak from heap-allocated strings
    alignas(8) char result_storage[32] = {};
    void* result_ptr = result_storage;

    watchdog_arm(5);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_decision '%s': crashed (%s)\n", type, sig_name);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("send_decision");
            r.key("reason"); r.value_string(sig_name);
            r.key("attempted_type"); r.value_string(type);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    bool took_decision = false;
    std::string engine_message;
    try {
        ai_try_decision(result_ptr, actor, decision);

        // Read result string — std::string layout: [ptr/sso_buf(16B), size(8B)]
        // SSO: if size <= 15, string data is at result_storage+0 (inline)
        // Heap: if size > 15, ptr at result_storage+0 points to heap data
        size_t str_size = *reinterpret_cast<size_t*>(result_storage + 16);
        if (str_size > 0 && str_size < 4096) {
            const char* str_data;
            if (str_size <= 15) {
                str_data = result_storage;  // SSO inline
            } else {
                str_data = *reinterpret_cast<const char**>(result_storage);  // heap pointer
            }
            engine_message.assign(str_data, str_size);
            took_decision = (engine_message.find("Took decision") != std::string::npos);
        }

        // Call std::string destructor to free heap memory
        // libstdc++ layout: if capacity > 15 (non-SSO), free the heap pointer
        if (str_size > 15) {
            char* heap_ptr = *reinterpret_cast<char**>(result_storage);
            if (heap_ptr) free(heap_ptr);  // libstdc++ uses malloc/free for std::string
        }
    } catch (...) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_decision '%s': exception\n", type);
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] send_decision '%s': %s — %s\n",
                           type, took_decision ? "SUCCESS" : "FAILED", engine_message.c_str());
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(took_decision);
        r.key("action"); r.value_string("send_decision");
        r.key("decision_type"); r.value_string(type);
        if (!engine_message.empty()) {
            r.key("engine_result"); r.value_string(engine_message.c_str(), engine_message.size());
        }
        if (!took_decision && !engine_message.empty()) {
            r.key("reason"); r.value_string(engine_message.c_str(), engine_message.size());
        }
        r.end_object();
        *response_out = r.take();
    }
    return took_decision;
}

// ===========================================================================
// do_set_lifestyle_focus — set a character's lifestyle focus by name.
// Resolves the CFocus* type object from FOCUS_DB (CGameDatabase: entries+0x88,
// count+0x90; match db_object NAME_KEY), then calls the resolved 2-arg game
// function SET_FOCUS_APPLY(CCharacter*, CFocus*) under crash protection.
// (signature resolved by decompile 20260612; see ck3_offsets.h SET_FOCUS_APPLY).
// Player-protection guarded — never sets the human player's focus.
// ===========================================================================
bool do_set_lifestyle_focus(const char* params_json, std::string* response_out) {
    const char* focus_name = json_get_string(params_json, "focus");
    if (!focus_name || !focus_name[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"set_lifestyle_focus","reason":"missing_focus"})";
        return false;
    }
    std::string want(focus_name);

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "set_lifestyle_focus", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_lifestyle_focus","reason":"no_living_actor"})";
        return false;
    }

    // Resolve the focus type object by name from FOCUS_DB.
    ADLOG("set_lifestyle_focus: ENTER want='%s' actor=%p\n", want.c_str(), actor);
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::FOCUS_DB);
    uintptr_t entries = db ? safe_read<uintptr_t>(db + ck3::game_db::ENTRIES_ARRAY) : 0;
    uint32_t count = db ? safe_read<uint32_t>(db + ck3::game_db::ENTRY_COUNT) : 0;
    ADLOG("set_lifestyle_focus: FOCUS_DB db=%lx entries=%lx count=%u\n",
          (unsigned long)db, (unsigned long)entries, count);
    if (!entries || !is_mapped(entries) || count == 0 || count > 5000) {
        if (response_out) *response_out = R"({"success":false,"action":"set_lifestyle_focus","reason":"focus_db_unavailable"})";
        return false;
    }
    uintptr_t focus_obj = 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t f = safe_read<uintptr_t>(entries + static_cast<uintptr_t>(i) * 8);
        if (!f || !is_mapped(f)) continue;
        if (ad_read_std_string(f + ck3::db_object::NAME_KEY) == want) { focus_obj = f; break; }
    }
    ADLOG("set_lifestyle_focus: scan done, focus_obj=%lx\n", (unsigned long)focus_obj);
    if (!focus_obj) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_lifestyle_focus");
            r.key("reason"); r.value_string("focus_not_found");
            r.key("attempted"); r.value_string(want.c_str(), want.size());
            r.key("focus_db_count"); r.value_uint(count);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // Call SET_FOCUS_APPLY(char*, focus_obj*) under watchdog + sigsetjmp guard.
    // SET_FOCUS_APPLY advances the RNG scope; on a thread where
    // RandomScopeManager+0x20 is null it null-derefs in FUN_0774a4b0 (the
    // steered-war crash). It can ALSO null-deref internally on some actors when
    // its lifestyle-container fallback (FUN_068e27b0(realm)->vtable) returns
    // garbage. Either way the fix for the WEDGE is the same: the fake RNG scope is
    // a global that `arm` swaps into RandomScopeManager+0x20 — if the call crashes
    // and we longjmp out WITHOUT restoring it, the manager is left pointing at our
    // fake scope and the game thread wedges (tick stops). So arm BEFORE the
    // sigsetjmp and restore on BOTH paths (normal + crash). (ck-49 harden, 20260614)
    auto set_focus = reinterpret_cast<void(*)(void*, void*)>(g_base_addr + ck3::funcs::SET_FOCUS_APPLY);
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("set_lifestyle_focus");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);  // un-wedge the game thread
        g_in_protected_region = false;
        watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"set_lifestyle_focus","reason":"apply_crashed"})";
        return false;
    }
    ADLOG("set_lifestyle_focus: calling SET_FOCUS_APPLY (rng_armed=%d)...\n", rng_armed ? 1 : 0);
    set_focus(actor, reinterpret_cast<void*>(focus_obj));
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();
    ADLOG("set_lifestyle_focus: SET_FOCUS_APPLY returned\n");

    // Verify by reading back the active focus name (realm_data+0x280 → CActiveFocus).
    std::string applied;
    uintptr_t realm = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::REALM_DATA);
    if (realm && is_mapped(realm)) {
        uintptr_t af = safe_read<uintptr_t>(realm + ck3::lifestyle::ACTIVE_FOCUS_PTR);
        if (af && is_mapped(af)) {
            uintptr_t ft = safe_read<uintptr_t>(af + ck3::active_focus::FOCUS_TYPE_PTR);
            if (ft && is_mapped(ft)) applied = ad_read_std_string(ft + ck3::db_object::NAME_KEY);
        }
    }
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(applied == want);
        r.key("action"); r.value_string("set_lifestyle_focus");
        r.key("requested"); r.value_string(want.c_str(), want.size());
        if (!applied.empty()) { r.key("active_focus"); r.value_string(applied.c_str(), applied.size()); }
        r.end_object();
        *response_out = r.take();
    }
    return applied == want;
}

// ===========================================================================
// do_set_education_focus — §2 (re_heir_education_RE.md): set a CHILD heir's
// childhood education focus. The childhood focus and the adult lifestyle focus
// share ONE engine slot (core_data+0x280) and ONE engine setter (SET_FOCUS_APPLY),
// so this is a thin type-checked alias over do_set_lifestyle_focus: it ONLY accepts
// "education_*" focuses (so an adult lifestyle focus can't be mis-applied to a child
// and vice-versa), then delegates to the already-crash-hardened setter (fake-RNG
// scope + watchdog(3) + sigsetjmp). The readback in do_set_lifestyle_focus reads the
// SAME slot the child's focus is written to, so success is correctly detected — no
// child-specific path or readback-relax needed. Player-protection inherited.
// ===========================================================================
bool do_set_education_focus(const char* params_json, std::string* response_out) {
    const char* focus_name = json_get_string(params_json, "focus");
    if (!focus_name || !focus_name[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"set_education_focus","reason":"missing_focus"})";
        return false;
    }
    // Type-check: only childhood education focuses route here (education_martial,
    // education_diplomacy, education_stewardship, education_intrigue, education_learning).
    if (std::string(focus_name).rfind("education_", 0) != 0) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_education_focus");
            r.key("reason"); r.value_string("not_an_education_focus");
            r.key("attempted"); r.value_string(focus_name);
            r.key("hint"); r.value_string("use set_lifestyle_focus for adult lifestyle focuses");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    // Delegate to the proven hardened setter (same slot, same readback).
    return do_set_lifestyle_focus(params_json, response_out);
}

// ===========================================================================
// do_set_council_task — assign a council task to a ruler's councillor by name.
// RESOLVED & RUNTIME-VALIDATED 20260612 (DC12-14 + live DB walk). Single-call
// setter CHANGE_COUNCIL_TASK(ruler, position_instance, task_type_obj, target)
// builds CChangeCouncilTaskCommand internally and dispatches via
// GAME_COMMAND_DISPATCH (mode 0xe) — no object graph to fabricate.
//   - task_type_obj: linear name-scan of CCouncilTaskTypeDatabase flat hash map
//     (singleton @ COUNCIL_TASK_TYPE_DB; table @ inst+0x1b0, mask @ inst+0x1bc,
//      bucket stride 0x18, value ptr @ bucket+0x10, key std::string @ value+0x10).
//   - position_instance: COUNCIL_POSITION_DB[ COUNCIL_POSITION_RESOLVE(ruler,
//      *(task_type+0x30), 0) ] — same derivation the setter does internally.
//   - target: optional title (param target_title_id) via BUILD_TARGET_SCOPE
//      ({i16 type=8; u32 id@+8}); default NO_TARGET_SCOPE for taskless targets.
// Command advances RNG → wrapped in arm_fake_random_scope (war/focus crash).
// Player-protection guarded.
// ===========================================================================
namespace {
    // CCouncilTaskTypeDatabase flat string-hash map layout (instance-relative).
    constexpr uintptr_t CTT_TABLE_PTR  = 0x1b0;  // ptr to first bucket
    constexpr uintptr_t CTT_MASK       = 0x1bc;  // u32 mask (capacity-1)
    constexpr uintptr_t CTT_STRIDE     = 0x18;   // bucket stride
    constexpr uintptr_t CTT_CTRL       = 0x04;   // u8 control byte (0xff = empty)
    constexpr uintptr_t CTT_VALUE      = 0x10;   // ptr to CCouncilTaskType
    constexpr uintptr_t CTT_NAME       = 0x10;   // key std::string on the task-type obj
    constexpr uintptr_t TASKTYPE_POSREF = 0x30;  // position-type ref on the task-type obj
}

bool do_set_council_task(const char* params_json, std::string* response_out) {
    const char* task_name = json_get_string(params_json, "task");
    if (!task_name || !task_name[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"set_council_task","reason":"missing_task"})";
        return false;
    }
    std::string want(task_name);
    int target_title_id = json_get_int(params_json, "target_title_id");
    int target_character_id = json_get_int(params_json, "target_character_id");

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "set_council_task", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_council_task","reason":"no_living_actor"})";
        return false;
    }

    // --- Resolve task-type object by linear name scan (pure reads) ---
    ADLOG("set_council_task: ENTER task='%s' actor=%p title=%d\n", want.c_str(), actor, target_title_id);
    uintptr_t inst = safe_read_offset<uintptr_t>(ck3::globals::COUNCIL_TASK_TYPE_DB);
    if (!inst || !is_mapped(inst)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_council_task","reason":"task_db_unavailable"})";
        return false;
    }
    uintptr_t table = safe_read<uintptr_t>(inst + CTT_TABLE_PTR);
    uint32_t mask = safe_read<uint32_t>(inst + CTT_MASK);
    ADLOG("set_council_task: task_db inst=%lx table=%lx mask=%u\n",
          (unsigned long)inst, (unsigned long)table, mask);
    if (!table || !is_mapped(table) || mask == 0 || mask > 0xffff) {
        if (response_out) *response_out = R"({"success":false,"action":"set_council_task","reason":"task_db_malformed"})";
        return false;
    }
    uintptr_t tasktype = 0;
    for (uint32_t i = 0; i <= mask; i++) {
        uintptr_t bucket = table + static_cast<uintptr_t>(i) * CTT_STRIDE;
        uint8_t ctrl = safe_read<uint8_t>(bucket + CTT_CTRL);
        if (ctrl == 0xff) continue;
        uintptr_t val = safe_read<uintptr_t>(bucket + CTT_VALUE);
        if (!val || !is_mapped(val)) continue;
        if (ad_read_std_string(val + CTT_NAME) == want) { tasktype = val; break; }
    }
    ADLOG("set_council_task: scan done, tasktype=%lx\n", (unsigned long)tasktype);
    if (!tasktype) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_council_task");
            r.key("reason"); r.value_string("task_not_found");
            r.key("attempted"); r.value_string(want.c_str(), want.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    auto build_scope = reinterpret_cast<void*(*)(void*)>(
        g_base_addr + ck3::funcs::BUILD_TARGET_SCOPE);

    uintptr_t postype_ref = safe_read<uintptr_t>(tasktype + TASKTYPE_POSREF);

    // lane-query item 1: NAME THE SEAT, AND REFUSE ONE THAT CANNOT TAKE THE TASK.
    // A council task belongs to exactly one position type (tasktype+0x30, the
    // same key query_council prints as position_key). The handler always
    // resolved the seat from the task and never read a `seat` argument, so a
    // caller asking for the chancellor to fabricate a claim got the court
    // chaplain (task_fabricate_claim is a chaplain task: dump
    // ck3_20260625_152901_ivar867, tasktype+0x30 -> councillor_court_chaplain)
    // with nothing in the reply saying so, and its retry read
    // no_change_already_set. Now: the reply names the seat it used, and an
    // explicit `seat` that is not the task's seat is refused up front.
    std::string task_seat;
    {
        const char* kp = nullptr; size_t kl = 0;
        if (postype_ref && is_mapped(postype_ref) && read_db_obj_name(postype_ref, kp, kl) && kl)
            task_seat.assign(kp, std::min(kl, (size_t)127));
    }
    {
        const char* seat_arg = json_get_string(params_json, "seat");
        std::string want_seat = seat_arg ? std::string(seat_arg) : std::string();
        auto norm = [](std::string v) {
            for (auto& ch : v) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            const std::string pre = "councillor_";
            if (v.compare(0, pre.size(), pre) == 0) v = v.substr(pre.size());
            return v;
        };
        bool numeric = !want_seat.empty() &&
            want_seat.find_first_not_of("0123456789") == std::string::npos;
        if (!want_seat.empty() && !numeric && !task_seat.empty() &&
                norm(want_seat) != norm(task_seat)) {
            ADLOG("set_council_task: REFUSE seat '%s' cannot take '%s' (task seat %s)\n",
                  want_seat.c_str(), want.c_str(), task_seat.c_str());
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("set_council_task");
                r.key("requested"); r.value_string(want.c_str(), want.size());
                r.key("reason"); r.value_string("seat_cannot_take_task");
                r.key("requested_seat"); r.value_string(want_seat.c_str(), want_seat.size());
                r.key("task_seat"); r.value_string(task_seat.c_str(), task_seat.size());
                r.key("applied"); r.value_bool(false);
                std::string w = "'" + want + "' is a " + task_seat + " task; the game assigns it "
                                "only to that seat. Omit `seat` (the task picks its seat) or pick a "
                                "task of the seat you want (inspect(subject='council') lists seats "
                                "and their current tasks).";
                r.key("warning"); r.value_string(w.c_str(), w.size());
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
    }

    // #118: target-required tasks (task_fabricate_claim, task_conversion,
    // task_integrate_title, …) carry a non-zero target-mode discriminator at
    // tasktype+0x38 (0 = no target; 1 = single-target eval-predicate path, the
    // fabricate-claim case; 2 = title target). The engine's CanAssign runs the
    // TARGET validate (FUN_05cb80e0) BEFORE the councillor check and returns false
    // with NO error string when the target is missing/ineligible — which surfaced
    // to the agent as an opaque `assignable:false` with no reason. Detect the
    // missing-target case up front and return a clear, actionable verdict instead
    // of letting Validate silently fail. (RE: re_council_fabricate_claim_118.md)
    int32_t target_disc = safe_read<int32_t>(tasktype + ck3::funcs::TASKTYPE_TARGET_DISC);
    bool task_needs_target = (target_disc != 0);
    if (task_needs_target && target_title_id <= 0) {
        ADLOG("set_council_task: task '%s' requires a target title (disc=%d) but none supplied\n",
              want.c_str(), (int)target_disc);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_council_task");
            r.key("requested"); r.value_string(want.c_str(), want.size());
            r.key("assignable"); r.value_bool(false);
            r.key("reason"); r.value_string("target_required");
            r.key("warning"); r.value_string(
                "This council task targets a title (e.g. the county to fabricate a "
                "claim on) and needs target_title_id. Enumerate eligible targets with "
                "discover(what='fabricate_targets', actor_id=<this ruler>) and pass an "
                "assignable:true title_id as target_title_id.");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // --- Resolve the ruler's position instance by walking the council (pure reads) ---
    // Mirrors the engine's FUN_068b6f10: for each of the council's positions
    // (council+0x230 = id array, +0x23c = count), look it up in
    // COUNCIL_POSITION_DB and match on the position's TYPE — which the engine
    // reads as *(*(position+0x18 [current task]) + 0x30). The position whose type
    // equals our task's position-ref (tasktype+0x30) is the slot to assign.
    // (Calling FUN_067e2b20 directly returned -1 here; the manual walk is what
    //  the engine does internally and reliably finds the slot. FUN_03ff3a70 only
    //  uses its own internal resolve for a no-op check, so a valid position_instance
    //  is all that is required to build + dispatch the command.)
    constexpr uintptr_t COUNCIL_POS_ARRAY = 0x230;  // ptr to u32[] position ids
    constexpr uintptr_t COUNCIL_POS_COUNT = 0x23c;  // u32 position count
    constexpr uintptr_t POS_CURRENT_TASK  = 0x18;   // position instance → current task obj
    uintptr_t position = 0;
    uintptr_t council = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(actor) + 0x1b8);
    uintptr_t posdb = safe_read_offset<uintptr_t>(ck3::globals::COUNCIL_POSITION_DB);
    if (council && is_mapped(council) && posdb && is_mapped(posdb)) {
        uintptr_t arr  = safe_read<uintptr_t>(council + COUNCIL_POS_ARRAY);
        uint32_t  cnt  = safe_read<uint32_t>(council + COUNCIL_POS_COUNT);
        uintptr_t pdb_arr = safe_read<uintptr_t>(posdb + ck3::hash_map::TABLE_PTR);
        uint32_t  pdb_cap = safe_read<uint32_t>(posdb + ck3::hash_map::CAPACITY);
        if (arr && is_mapped(arr) && pdb_arr && cnt > 0 && cnt < 64) {
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t pid = safe_read<uint32_t>(arr + static_cast<uintptr_t>(i) * 4);
                uint32_t idx = pid & 0xffffff;
                if (idx >= pdb_cap) continue;
                uintptr_t entry = safe_read<uintptr_t>(pdb_arr + ck3::hash_map::ENTRY_HEADER_SIZE
                                                       + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
                if (!entry || !is_mapped(entry)) continue;
                if (safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) != pid) continue;
                uintptr_t cur_task = safe_read<uintptr_t>(entry + POS_CURRENT_TASK);
                if (!cur_task || !is_mapped(cur_task)) continue;
                if (safe_read<uintptr_t>(cur_task + TASKTYPE_POSREF) == postype_ref) { position = entry; break; }
            }
        }
    }
    ADLOG("set_council_task: council=%lx posref=%lx position=%lx\n",
          (unsigned long)council, (unsigned long)postype_ref, (unsigned long)position);
    if (!position) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_council_task");
            r.key("reason"); r.value_string("no_matching_position");
            r.key("requested"); r.value_string(want.c_str(), want.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // Everything that calls game code goes under the crash net + RNG scope.
    // Arm the fake RNG scope BEFORE sigsetjmp so the crash branch can restore it —
    // otherwise a SIGSEGV mid-call leaves RandomScopeManager pointing at our global
    // fake scope and the game thread wedges. (ck-49 harden, 20260614)
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("set_council_task");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"set_council_task","reason":"assign_crashed"})";
        return false;
    }

    // Prior task (before applying), for reporting.
    std::string prior;
    {
        uintptr_t cur = safe_read<uintptr_t>(position + 0x18);
        if (cur && is_mapped(cur)) prior = ad_read_std_string(cur + CTT_NAME);
    }

    // --- Build the target scope (existing title object; type 8 = title) ---
    uint8_t scope_in[16];
    memset(scope_in, 0, sizeof(scope_in));
    void* target_scope = nullptr;
    if (target_title_id > 0) {
        *reinterpret_cast<int16_t*>(scope_in) = 8;
        *reinterpret_cast<uint32_t*>(scope_in + 8) = static_cast<uint32_t>(target_title_id);
        target_scope = build_scope(scope_in);
    } else {
        target_scope = reinterpret_cast<void*>(safe_read_offset<uintptr_t>(ck3::globals::NO_TARGET_SCOPE));
    }

    // ── VALIDATE then STORE (20260614) — native council-task apply, NOT queued ──
    // 1. FUN_05c83430 = the command's vtable Validate (CanAssign): runs the target
    //    oracle PLUS the councillor-side requirement check (FUN_05c85b40 →
    //    FUN_067e3040) and returns bool + the game's localized rejection reason.
    // 2. If assignable, FUN_05cc8660(mgr/*ignored*/, pos_id, scope, tasktype) is the
    //    command-Execute store path (secondary vtable slot FUN_05c83400 → this) →
    //    FUN_05cc9970(position, scope, tasktype) which WRITES *(position+0x18)=
    //    tasktype (the field query_council reads) + the scope/target into
    //    position+0x38.., then finalizes. NOT player-gated, NOT queued → applies
    //    synchronously for an AI ruler. (Supersedes the gate-byte queue dispatch,
    //    which only executed in the null-observer player context.)
    // Command field buffer (no vtable needed): +0x20 pos id, +0x28 tasktype,
    //   +0x30.. scope-prep(buf, ctxB, ctxA, target_scope).
    auto scope_prep = reinterpret_cast<void(*)(void*, uint32_t, uint32_t, void*)>(
        g_base_addr + ck3::funcs::COUNCIL_TASK_SCOPE_PREP);
    auto cmd_validate = reinterpret_cast<char(*)(void*, void*)>(
        g_base_addr + ck3::funcs::COUNCIL_TASK_CMD_EXECUTE);
    auto task_store = reinterpret_cast<void(*)(void*, uint32_t, void*, void*)>(
        g_base_addr + ck3::funcs::COUNCIL_TASK_STORE);

    uint32_t ctx_a = safe_read<uint32_t>(position + ck3::funcs::COUNCIL_POS_CTX_A);  // councillor char id
    uint32_t ctx_b = safe_read<uint32_t>(position + ck3::funcs::COUNCIL_POS_CTX_B);  // ruler char id
    uint32_t pos_id = safe_read<uint32_t>(position + ck3::globals::COUNCIL_POS_KEY);

    // #72/B1 death-guard (SAFE, pure-read): a task stored on a DEAD or EMPTY
    // councillor seat is inert — no councillor executes it, so it produces no
    // progress (the same silent-stall class as the seat_vacant case). ctx_a is
    // the seated councillor char-id. Resolve it and reject up front if the seat
    // is empty (sentinel) or the councillor is dead. DEATH_DATE (0x1c8) is a ptr
    // that is null while alive, non-null once dead — same test the suppress hook
    // uses (council_suppress_hook.cpp:110-113).
    {
        void* cc = (ctx_a && (ctx_a & 0xFFFFFF) != 0xFFFFFF)
                   ? lookup_character(ctx_a) : nullptr;
        bool cc_dead = false;
        if (!cc || is_sentinel(cc)) {
            cc_dead = true;  // empty/recycled seat — no councillor seated
        } else {
            uintptr_t dd = safe_read<uintptr_t>(
                reinterpret_cast<uintptr_t>(cc) + ck3::character::DEATH_DATE);
            cc_dead = (dd != 0);
        }
        if (cc_dead) {
            if (rng_armed) restore_fake_random_scope(saved_rng);
            g_in_protected_region = false;
            watchdog_disarm();
            ADLOG("set_council_task: REJECT — councillor seat dead/empty (ctx_a=%u)\n", ctx_a);
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("set_council_task");
                r.key("requested"); r.value_string(want.c_str(), want.size());
                r.key("assignable"); r.value_bool(false);
                r.key("applied"); r.value_bool(false);
                r.key("reason"); r.value_string("councillor_dead");
                r.key("seat_vacant"); r.value_bool(true);
                r.key("warning"); r.value_string(
                    "The seat's councillor is DEAD or the seat is EMPTY — this task "
                    "would be inert (no councillor to execute it, no progress). Seat a "
                    "live councillor first: act(type='appoint', position_id=<this seat>, "
                    "character_id=<courtier>), or wait for the AI (Council subsystem) "
                    "to fill it, then re-assign the task.");
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
    }

    uint8_t cmd[0x50];
    memset(cmd, 0, sizeof(cmd));
    *reinterpret_cast<uint32_t*>(cmd + 0x20) = pos_id;
    *reinterpret_cast<uintptr_t*>(cmd + 0x28) = tasktype;
    scope_prep(cmd + 0x30, ctx_b, ctx_a, target_scope);   // out[0]=ctxA councillor, out[1]=ctxB ruler

    // libstdc++ empty std::string for the rejection-reason out-param.
    uint8_t errbuf[32];
    memset(errbuf, 0, sizeof(errbuf));
    *reinterpret_cast<uintptr_t*>(errbuf) = reinterpret_cast<uintptr_t>(errbuf + 0x10);  // _M_p → SSO buf

    ADLOG("set_council_task: cmd Validate (pos=%u ctxA=%u ctxB=%u target=%p)...\n",
          pos_id, ctx_a, ctx_b, target_scope);
    char assignable = cmd_validate(cmd, errbuf);
    ADLOG("set_council_task: Validate=%d; %s\n", (int)assignable, assignable ? "storing" : "skip store");

    // Capture + free any heap-allocated rejection string (same libstdc++ allocator).
    std::string reject;
    {
        uintptr_t sp = *reinterpret_cast<uintptr_t*>(errbuf);
        uint64_t slen = *reinterpret_cast<uint64_t*>(errbuf + 8);
        if (sp && is_mapped(sp) && slen > 0 && slen < 512)
            reject.assign(reinterpret_cast<const char*>(sp), slen);
        if (sp && sp != reinterpret_cast<uintptr_t>(errbuf + 0x10))
            ::operator delete(reinterpret_cast<void*>(sp));
    }

    // Only store when the game says it's assignable (else we'd set an invalid task).
    if (assignable) {
        task_store(nullptr, pos_id, cmd + 0x30, reinterpret_cast<void*>(tasktype));
        ADLOG("set_council_task: store returned\n");
    }

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    // Read back the current task (position+0x18) — the store writes it synchronously.
    std::string current_task;
    {
        uintptr_t cur = safe_read<uintptr_t>(position + 0x18);
        if (cur && is_mapped(cur)) current_task = ad_read_std_string(cur + CTT_NAME);
    }

    // A1/A9 (20260621): make `applied` HONEST. The in-memory read-back
    // (current_task == want) is true even when the call did nothing durable:
    //   (1) PAUSED — FUN_05cc9970 writes position+0x18 unconditionally, but a
    //       paused store is not a ticked/durable apply (the per-ruler council
    //       routine + task-progress machinery + suppress hook only run on a
    //       RUNNING AI tick; see bridge-pause-state-rules). Report it honestly
    //       rather than over-claiming success. (REPORTING-ONLY — no write gate,
    //       to avoid the steered-war hang regression class, 4d14ce3.)
    //   (2) NO-OP — the seat already held `want` (prior == want), so read-back
    //       equals want though nothing changed this call.
    bool was_paused = is_paused();
    bool readback_ok = (current_task == want);
    bool changed   = readback_ok && (prior != want);
    bool unchanged = readback_ok && (prior == want);
    // A durable apply requires an actual change AND a running game.
    bool applied = changed && !was_paused;

    // L20 stickiness: once the agent has deliberately set a council task, lock
    // this ruler's council so the native per-ruler council-AI routine
    // (FUN_04dca2a0) is skipped for it on subsequent AI ticks — otherwise the
    // engine re-assigns and overwrites our task next tick. Only locks on a
    // confirmed durable apply (a paused store gets no tick to act on, so the
    // lock would be inert anyway); release_character clears the lock. Suppresses
    // ONLY the council routine for this one ruler, not war/other AI.
    if (applied) {
        uint32_t actor_id = char_get_id(actor) & 0xFFFFFF;
        council_lock_add(actor_id);
    }

    // Honest reason + success. success is true only when we durably changed the
    // task. A redundant call (already set) or a paused store reports false with
    // a reason so the agent does not treat it as a fresh apply.
    const char* reason = nullptr;
    bool success = applied;
    if (!assignable) {
        // Genuine reject. The councillor-side check (FUN_05c85b40) writes a
        // localized reject_reason, but the EARLIER target validate (FUN_05cb80e0)
        // returns false with NO string — so for target-required tasks an empty
        // reject means "the game's target oracle rejected this title/target".
        // Surface that explicitly (#118) so the agent stops seeing a bare
        // assignable:false: pick a different (assignable) target, or — for a
        // tribal/administrative realm or wrong county — accept that fabrication is
        // not available there. (Feudal/clan realm + an assignable county is the
        // path; discover(what='fabricate_targets') lists the ones that pass.)
        success = false;
        if (reject.empty()) {
            reason = task_needs_target
                ? "target_rejected"   // FUN_05cb80e0 culled this target (ineligible county / government can't fabricate)
                : "not_assignable";   // councillor/realm cannot take this task
        }
    } else if (was_paused && readback_ok) {
        reason = "paused_not_confirmed";  // landed in memory, not a ticked apply
        success = false;
    } else if (unchanged) {
        reason = "no_change_already_set"; // redundant; seat already held `want`
        success = false;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(success);
        r.key("action"); r.value_string("set_council_task");
        r.key("requested"); r.value_string(want.c_str(), want.size());
        r.key("assignable"); r.value_bool(assignable != 0);   // game's real CanAssign verdict
        r.key("applied"); r.value_bool(applied);              // durable change (read-back && changed && !paused)
        r.key("was_paused"); r.value_bool(was_paused);
        r.key("unchanged"); r.value_bool(unchanged);
        if (reason) { r.key("reason"); r.value_string(reason); }
        if (was_paused && readback_ok) { r.key("pending_task"); r.value_string(want.c_str(), want.size()); }
        if (!current_task.empty()) { r.key("current_task"); r.value_string(current_task.c_str(), current_task.size()); }
        if (!prior.empty()) { r.key("prior_task"); r.value_string(prior.c_str(), prior.size()); }
        if (!reject.empty()) { r.key("reject_reason"); r.value_string(reject.c_str(), reject.size()); }
        if (target_title_id > 0) { r.key("target_title_id"); r.value_int(target_title_id); }
        // lane-query item 1: the seat this task was applied to (or tried).
        if (!task_seat.empty()) { r.key("seat"); r.value_string(task_seat.c_str(), task_seat.size()); }
        r.key("position_id"); r.value_uint(pos_id & 0xFFFFFF);
        if (ctx_a && (ctx_a & 0xFFFFFF) != 0xFFFFFF) {
            r.key("councillor_id"); r.value_uint(ctx_a & 0xFFFFFF);
            void* cc = lookup_character(ctx_a);
            if (cc && !is_sentinel(cc)) {
                std::string cn = ad_read_char_name(reinterpret_cast<uintptr_t>(cc));
                if (!cn.empty()) { r.key("councillor_name"); r.value_string(cn.c_str(), cn.size()); }
            }
        }
        // Vacant-seat warning: ctx_a is the seated councillor id. A task stored on
        // an EMPTY seat is inert — no councillor executes it, so it produces no
        // progress (the silent-stall the agent kept hitting). Tell the agent to
        // seat the councillor first via act(type="appoint", ...).
        bool seat_vacant = (ctx_a == 0 || ctx_a == 0xFFFFFF);
        r.key("seat_vacant"); r.value_bool(seat_vacant);
        if (seat_vacant) {
            r.key("warning"); r.value_string(
                "Seat is VACANT — this task has no councillor to execute it and will "
                "produce no progress. Seat one first: act(type='appoint', position_id="
                "<this seat>, character_id=<courtier>), or wait for the AI (Council "
                "subsystem) to fill it, then re-assign the task.");
        }
        r.end_object();
        *response_out = r.take();
    }
    return applied;
}

// ===========================================================================
// do_query_fabricate_targets — enumerate the counties that are ELIGIBLE targets
// for a given AI ruler's targeted council task (default task_fabricate_claim),
// so the agent can pick a real target instead of guessing. READ-ONLY: builds a
// temporary eval scope per candidate and runs the task's data-driven validity
// triggers; no command is dispatched, no game state mutates.
//
// RESOLVED 20260613 (re_council_task_targets_20260613 + pyghidra DC). The engine's
// own validity path is CHANGE_COUNCIL_TASK→FUN_03ff3930 for disc-1 (single-target)
// tasks:
//     uVar3 = *(u32*)(position + COUNCIL_POS_CTX_B);          // FUN_05cc86b0
//     COUNCIL_TASK_SCOPE_PREP(scope_buf, uVar3,
//                             *(u32*)(position + COUNCIL_POS_CTX_A), target_scope);
//     valid = COUNCIL_TASK_TARGET_VALIDATE(tasktype, scope_buf, 0);   // 1 = eligible
// target_scope = BUILD_TARGET_SCOPE(&{i16 8; u32 county_title_id}) returns the
// existing title object (no allocation) — so iterating thousands of counties is
// leak-free. Game calls advance RNG → wrapped in arm_fake_random_scope, watchdog
// + sigsetjmp net (identical safety domain to do_set_council_task). Classified as
// is_game_state_query: the agent calls it while PAUSED (observe state), where no
// parallel AI mutates the graph — same safety as the inline read queries.
// ===========================================================================
bool do_query_fabricate_targets(const char* params_json, std::string* response_out) {
    const char* task_name = json_get_string(params_json, "task");
    std::string want = (task_name && task_name[0]) ? std::string(task_name)
                                                    : std::string("task_fabricate_claim");
    int max_results = json_get_int(params_json, "max_results");
    if (max_results <= 0 || max_results > 200) max_results = 40;
    // ck-fabtargets: the old loop stopped at the first `max_results` ELIGIBLE
    // counties in raw title-DB index order, so the truly ASSIGNABLE (range-
    // respecting, i.e. nearby) targets were truncated off behind far counties
    // that merely pass the oracle. Now scan the whole county DB (bounded by a
    // validate-call budget — this runs PAUSED so a longer scan is safe),
    // collect eligible hits then sort assignable-first. Assignable hits are
    // stored unconditionally (never subject to the storage cap); the cap only
    // bounds the non-assignable oracle-only rows so the payload stays bounded.
    int scan_budget = json_get_int(params_json, "scan_budget");
    if (scan_budget <= 0 || scan_budget > 200000) scan_budget = 50000;
    constexpr size_t HIT_STORAGE_CAP = 512;
    // ck-fabtargets (L23): when true, return ONLY de-jure-of-actor targets
    // (relevance 0) — the natural reconquest/consolidation frontier.
    bool de_jure_only = json_get_bool(params_json, "de_jure_only", false);
    // war_validity_discovery: by default targets[] is the ACTIONABLE
    // (apply-would-stick) set — drop oracle-only / non-assignable rows so a
    // naive consumer that keys on list-membership does not pick a county
    // set_council_task won't apply. Pass assignable_only=false to also see the
    // oracle-only diagnostic rows.
    bool assignable_only = json_get_bool(params_json, "assignable_only", true);

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "query_fabricate_targets", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"query_fabricate_targets","reason":"no_living_actor"})";
        return false;
    }

    // --- Resolve task-type object by name scan (pure reads; mirrors setter) ---
    uintptr_t inst = safe_read_offset<uintptr_t>(ck3::globals::COUNCIL_TASK_TYPE_DB);
    if (!inst || !is_mapped(inst)) {
        if (response_out) *response_out = R"({"success":false,"action":"query_fabricate_targets","reason":"task_db_unavailable"})";
        return false;
    }
    uintptr_t table = safe_read<uintptr_t>(inst + CTT_TABLE_PTR);
    uint32_t mask = safe_read<uint32_t>(inst + CTT_MASK);
    if (!table || !is_mapped(table) || mask == 0 || mask > 0xffff) {
        if (response_out) *response_out = R"({"success":false,"action":"query_fabricate_targets","reason":"task_db_malformed"})";
        return false;
    }
    uintptr_t tasktype = 0;
    for (uint32_t i = 0; i <= mask; i++) {
        uintptr_t bucket = table + static_cast<uintptr_t>(i) * CTT_STRIDE;
        if (safe_read<uint8_t>(bucket + CTT_CTRL) == 0xff) continue;
        uintptr_t val = safe_read<uintptr_t>(bucket + CTT_VALUE);
        if (!val || !is_mapped(val)) continue;
        if (ad_read_std_string(val + CTT_NAME) == want) { tasktype = val; break; }
    }
    if (!tasktype) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("query_fabricate_targets");
            r.key("reason"); r.value_string("task_not_found");
            r.key("attempted"); r.value_string(want.c_str(), want.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // --- Resolve the ruler's matching council position instance (council walk) ---
    uintptr_t postype_ref = safe_read<uintptr_t>(tasktype + TASKTYPE_POSREF);
    constexpr uintptr_t COUNCIL_POS_ARRAY = 0x230;
    constexpr uintptr_t COUNCIL_POS_COUNT = 0x23c;
    constexpr uintptr_t POS_CURRENT_TASK  = 0x18;
    uintptr_t position = 0;
    uintptr_t council = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(actor) + 0x1b8);
    uintptr_t posdb = safe_read_offset<uintptr_t>(ck3::globals::COUNCIL_POSITION_DB);
    if (council && is_mapped(council) && posdb && is_mapped(posdb)) {
        uintptr_t arr  = safe_read<uintptr_t>(council + COUNCIL_POS_ARRAY);
        uint32_t  cnt  = safe_read<uint32_t>(council + COUNCIL_POS_COUNT);
        uintptr_t pdb_arr = safe_read<uintptr_t>(posdb + ck3::hash_map::TABLE_PTR);
        uint32_t  pdb_cap = safe_read<uint32_t>(posdb + ck3::hash_map::CAPACITY);
        if (arr && is_mapped(arr) && pdb_arr && cnt > 0 && cnt < 64) {
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t pid = safe_read<uint32_t>(arr + static_cast<uintptr_t>(i) * 4);
                uint32_t idx = pid & 0xffffff;
                if (idx >= pdb_cap) continue;
                uintptr_t entry = safe_read<uintptr_t>(pdb_arr + ck3::hash_map::ENTRY_HEADER_SIZE
                                                       + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
                if (!entry || !is_mapped(entry)) continue;
                if (safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) != pid) continue;
                uintptr_t cur_task = safe_read<uintptr_t>(entry + POS_CURRENT_TASK);
                if (!cur_task || !is_mapped(cur_task)) continue;
                if (safe_read<uintptr_t>(cur_task + TASKTYPE_POSREF) == postype_ref) { position = entry; break; }
            }
        }
    }
    if (!position) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("query_fabricate_targets");
            r.key("reason"); r.value_string("no_matching_position");
            r.key("requested"); r.value_string(want.c_str(), want.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // Councillor context carried into the eval scope (see header recipe).
    uint32_t ctx_a = safe_read<uint32_t>(position + ck3::funcs::COUNCIL_POS_CTX_A);
    uint32_t ctx_b = safe_read<uint32_t>(position + ck3::funcs::COUNCIL_POS_CTX_B);
    uint32_t pos_id = safe_read<uint32_t>(position + ck3::globals::COUNCIL_POS_KEY);
    // war_validity_discovery: ctx_a (+0x38) is the SEATED councillor id; sentinel
    // 0/0xFFFFFF == the seat is VACANT. A vacant seat means set_council_task would
    // store an INERT task on an empty chair (lesson L29 / council-fabrication
    // silent stall). The sibling WRITE handler do_set_council_task already emits
    // this signal (action_handlers.cpp:1067); the READ surface omitted it. Mirror
    // it so the discovery surface stops lying about assignability.
    bool seat_vacant = (ctx_a == 0 || ctx_a == 0xFFFFFF);
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID);

    // ck-fabtargets (L23): compute the actor's DE-JURE region (the set of its
    // de-jure duchy + kingdom title IDs) so we can surface NEARBY / strategically
    // relevant targets first instead of the lowest-title-id (far English) counties
    // the old title_id sort surfaced. This reuses the de-jure-region walk from
    // do_query_borders (query_handlers.cpp:1432-1461): enumerate the actor's held
    // titles, and for each county/duchy walk DE_JURE_PARENT up to its kingdom.
    // Pure reads (lookup_title_obj/get_title_tier carry their own is_mapped guards),
    // so this is safe outside the protected region.
    std::set<uint32_t> actor_de_jure_duchies;
    std::set<uint32_t> actor_de_jure_kingdoms;
    {
        uintptr_t tr = resolve_title_realm(reinterpret_cast<uintptr_t>(actor));
        if (tr) {
            uintptr_t title_arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
            int32_t title_cnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
            if (title_cnt < 0 || title_cnt > 500) title_cnt = 0;
            if (title_arr && is_mapped(title_arr) && title_cnt > 0) {
                for (int32_t i = 0; i < title_cnt; i++) {
                    uint32_t tid = safe_read<uint32_t>(title_arr + i * 4);
                    if (tid == 0 || tid == 0xFFFFFFFF) continue;
                    uintptr_t tobj = lookup_title_obj(tid);
                    if (!tobj) continue;
                    int tier = get_title_tier(tobj);
                    if (tier == 2) {  // county → de-jure duchy → de-jure kingdom
                        uint32_t duchy_id = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT);
                        if (duchy_id && duchy_id != 0xFFFFFFFF) {
                            actor_de_jure_duchies.insert(duchy_id & 0xFFFFFF);
                            uintptr_t duchy_obj = lookup_title_obj(duchy_id);
                            if (duchy_obj) {
                                uint32_t kingdom_id = safe_read<uint32_t>(duchy_obj + ck3::title_obj::DE_JURE_PARENT);
                                if (kingdom_id && kingdom_id != 0xFFFFFFFF)
                                    actor_de_jure_kingdoms.insert(kingdom_id & 0xFFFFFF);
                            }
                        }
                    } else if (tier == 3) {  // duchy → de-jure kingdom
                        actor_de_jure_duchies.insert(tid & 0xFFFFFF);
                        uint32_t kingdom_id = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT);
                        if (kingdom_id && kingdom_id != 0xFFFFFFFF)
                            actor_de_jure_kingdoms.insert(kingdom_id & 0xFFFFFF);
                    } else if (tier == 4) {  // kingdom held directly
                        actor_de_jure_kingdoms.insert(tid & 0xFFFFFF);
                    }
                }
            }
        }
    }

    // ck-fabtargets (L23): relevance of a candidate county to the actor.
    //   0 = de-jure within the actor's own duchy/kingdom region (reconquest frontier)
    //   2 = everything else (fallback, ordered by title_id)
    // (Tier 1 = realm-adjacent is intentionally skipped — computing 4-way border
    // adjacency for ~848 counties per call is expensive/awkward, and de-jure
    // relevance alone fixes the King-of-Italy case: Italian counties surface first.)
    auto county_relevance = [&](uint32_t county_title_id) -> int {
        if (actor_de_jure_duchies.empty() && actor_de_jure_kingdoms.empty()) return 2;
        uintptr_t cobj = lookup_title_obj(county_title_id);
        if (!cobj) return 2;
        // Walk the de-jure liege chain: county → duchy → kingdom.
        uint32_t duchy_id = safe_read<uint32_t>(cobj + ck3::title_obj::DE_JURE_PARENT);
        if (duchy_id && duchy_id != 0xFFFFFFFF) {
            if (actor_de_jure_duchies.count(duchy_id & 0xFFFFFF)) return 0;
            uintptr_t dobj = lookup_title_obj(duchy_id);
            if (dobj) {
                uint32_t kingdom_id = safe_read<uint32_t>(dobj + ck3::title_obj::DE_JURE_PARENT);
                if (kingdom_id && kingdom_id != 0xFFFFFFFF
                    && actor_de_jure_kingdoms.count(kingdom_id & 0xFFFFFF))
                    return 0;
            }
        }
        return 2;
    };

    auto build_scope = reinterpret_cast<void*(*)(void*)>(
        g_base_addr + ck3::funcs::BUILD_TARGET_SCOPE);
    auto scope_prep = reinterpret_cast<void(*)(void*, uint32_t, uint32_t, void*)>(
        g_base_addr + ck3::funcs::COUNCIL_TASK_SCOPE_PREP);
    auto validate = reinterpret_cast<char(*)(void*, void*, long)>(
        g_base_addr + ck3::funcs::COUNCIL_TASK_TARGET_VALIDATE);
    // Full CanAssign (target oracle + councillor-side requirement check) — run only
    // on the ≤max_results oracle hits to annotate which are TRULY assignable (i.e.
    // set_council_task would apply + stick), not just target-eligible.
    auto cmd_validate = reinterpret_cast<char(*)(void*, void*)>(
        g_base_addr + ck3::funcs::COUNCIL_TASK_CMD_EXECUTE);

    // Title DB iteration bounds (county = tier 2).
    uintptr_t tdb = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    uintptr_t ttable = tdb ? safe_read<uintptr_t>(tdb + ck3::title_db::ARRAY_PTR) : 0;
    uint32_t tcap = tdb ? safe_read<uint32_t>(tdb + ck3::title_db::ARRAY_COUNT) : 0;
    if (!ttable || !is_mapped(ttable) || tcap == 0 || tcap > 500000) {
        if (response_out) *response_out = R"({"success":false,"action":"query_fabricate_targets","reason":"title_db_unavailable"})";
        return false;
    }

    // Collect eligible counties under the crash net + RNG scope.
    struct Hit { uint32_t title_id; uint32_t holder_id; std::string name; bool assignable; int relevance; };
    std::vector<Hit> hits;
    size_t n_unassignable_stored = 0;  // ck-fabtargets: cap applies only to these
    uint32_t scanned = 0, considered = 0, total_eligible = 0, total_assignable = 0;

    // Arm RNG scope BEFORE sigsetjmp so the crash branch restores it (else a
    // SIGSEGV mid-scan leaves the game thread wedged). (ck-49 harden, 20260614)
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("query_fabricate_targets");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(12);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"query_fabricate_targets","reason":"validate_crashed"})";
        return false;
    }

    for (uint32_t i = 0; i < tcap && considered < static_cast<uint32_t>(scan_budget); i++) {
        uintptr_t entry = safe_read<uintptr_t>(ttable + ck3::title_db::ELEMENT_PTR_OFFSET
                                               + static_cast<uintptr_t>(i) * ck3::title_db::ELEMENT_STRIDE);
        if (!entry || !is_mapped(entry)) continue;
        uintptr_t type_ptr = safe_read<uintptr_t>(entry + ck3::title_obj::TITLE_TYPE_PTR);
        if (!type_ptr || !is_mapped(type_ptr)) continue;
        if (safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER) != 2) continue;  // county only
        scanned++;
        uint32_t title_id = safe_read<uint32_t>(entry + ck3::title_obj::TITLE_ID);
        if (title_id == 0 || title_id == 0xFFFFFFFF) continue;
        uint32_t holder = safe_read<uint32_t>(entry + ck3::title_obj::HOLDER_CHAR_ID);
        // Skip unheld counties and counties the actor already holds (no claim to fab).
        if (holder == 0xFFFFFFFF) continue;
        if ((holder & 0xFFFFFF) == (actor_id & 0xFFFFFF)) continue;
        considered++;

        uint8_t scope_in[16];
        memset(scope_in, 0, sizeof(scope_in));
        *reinterpret_cast<int16_t*>(scope_in) = 8;                       // type 8 = title
        *reinterpret_cast<uint32_t*>(scope_in + 8) = title_id;
        void* target_scope = build_scope(scope_in);
        if (!target_scope) continue;

        uint8_t scope_buf[32];
        memset(scope_buf, 0, sizeof(scope_buf));
        scope_prep(scope_buf, ctx_b, ctx_a, target_scope);
        char ok = validate(reinterpret_cast<void*>(tasktype), scope_buf, 0);
        if (ok) {
            Hit h; h.title_id = title_id; h.holder_id = holder & 0xFFFFFF; h.assignable = false;
            // ck-fabtargets (L23): relevance key (0=de-jure-of-actor, 2=other).
            h.relevance = county_relevance(title_id);
            // Name string lives at title+0x58 (GCC SSO std::string; first 8 bytes
            // are a char* to the buffer) — same read other title handlers use.
            uintptr_t name_ptr = safe_read<uintptr_t>(entry + ck3::title_obj::TITLE_DISPLAY_NAME);
            if (name_ptr && is_mapped(name_ptr)) {
                char nb[128] = {};
                for (int k = 0; k < 127; k++) { nb[k] = safe_read<char>(name_ptr + k); if (!nb[k]) break; }
                if (nb[0]) h.name = nb;
            }
            // Full CanAssign verdict (target oracle PLUS councillor-side check): build
            // the command field buffer and call the Validate slot. assignable=true ⇒
            // set_council_task(task, target_title_id) would actually apply + stick.
            uint8_t vcmd[0x50];
            memset(vcmd, 0, sizeof(vcmd));
            *reinterpret_cast<uint32_t*>(vcmd + 0x20) = pos_id;
            *reinterpret_cast<uintptr_t*>(vcmd + 0x28) = tasktype;
            scope_prep(vcmd + 0x30, ctx_b, ctx_a, target_scope);
            uint8_t verr[32];
            memset(verr, 0, sizeof(verr));
            *reinterpret_cast<uintptr_t*>(verr) = reinterpret_cast<uintptr_t>(verr + 0x10);
            h.assignable = (cmd_validate(vcmd, verr) != 0);
            uintptr_t vsp = *reinterpret_cast<uintptr_t*>(verr);
            if (vsp && vsp != reinterpret_cast<uintptr_t>(verr + 0x10))
                ::operator delete(reinterpret_cast<void*>(vsp));
            total_eligible++;
            if (h.assignable) total_assignable++;
            // ck-fabtargets: ALWAYS store assignable hits — they are the
            // actionable, range-respecting targets and are inherently few
            // (bounded by total_assignable). The HIT_STORAGE_CAP applies ONLY
            // to the non-assignable (oracle-only, informational) rows so the
            // payload stays bounded. Previously a single capped push dropped
            // assignable hits that appeared late in DB-scan order, BEFORE the
            // assignable-first sort could promote them.
            if (h.assignable) {
                hits.push_back(std::move(h));
            } else if (n_unassignable_stored < HIT_STORAGE_CAP) {
                hits.push_back(std::move(h));
                n_unassignable_stored++;
            }
        }
    }
    bool scan_truncated = (considered >= static_cast<uint32_t>(scan_budget));

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    // ck-fabtargets (L23): optional de_jure_only filter — drop everything that
    // isn't in the actor's own de-jure region (relevance 0). The agent uses this
    // to get ONLY consolidation/reconquest targets.
    if (de_jure_only) {
        hits.erase(std::remove_if(hits.begin(), hits.end(),
                                  [](const Hit& h) { return h.relevance != 0; }),
                   hits.end());
    }

    // war_validity_discovery: make list-membership == actionability by default.
    // Mirrors the de_jure_only filter pattern. eligible_count/assignable_count
    // still report the FULL scan scope, so the agent sees the true scope.
    if (assignable_only) {
        hits.erase(std::remove_if(hits.begin(), hits.end(),
                                  [](const Hit& h) { return !h.assignable; }),
                   hits.end());
    }

    // Sort assignable-first (the actionable, range-respecting targets — these
    // are what was being truncated off behind far oracle-only hits), THEN by
    // relevance ascending (de-jure-of-actor counties before far foreign ones —
    // ck-fabtargets L23: the old title_id-only sort surfaced the lowest-id =
    // far English counties for a King of Italy; relevance pulls Italian counties
    // to the front), then by title_id for stable ordering. Agent reads the front.
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.assignable != b.assignable) return a.assignable;   // assignable first
        if (a.relevance != b.relevance) return a.relevance < b.relevance;  // de-jure first
        return a.title_id < b.title_id;
    });

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("query_fabricate_targets");
        r.key("task"); r.value_string(want.c_str(), want.size());
        r.key("actor_id"); r.value_uint(actor_id & 0xFFFFFF);
        r.key("counties_scanned"); r.value_uint(scanned);
        r.key("counties_considered"); r.value_uint(considered);
        // Totals reflect the FULL scan (not just the returned slice), so the
        // agent knows the true scope. assignable_count is the actionable subset.
        r.key("eligible_count"); r.value_uint(total_eligible);
        r.key("assignable_count"); r.value_uint(total_assignable);  // FULL CanAssign (apply would stick)
        // war_validity_discovery: disclose seat occupancy. When the executor seat
        // is vacant, set_council_task stores an INERT task — no target is truly
        // actionable regardless of per-row assignable (lesson L29).
        r.key("seat_vacant"); r.value_bool(seat_vacant);
        if (seat_vacant) {
            r.key("seat_warning");
            r.value_string("executor council seat is VACANT — set_council_task would store an inert task; seat a councillor first");
        }
        r.key("assignable_only"); r.value_bool(assignable_only);
        // scan_truncated: the validate-call budget was exhausted before the whole
        // county DB was scanned, so more targets may exist beyond what we saw.
        r.key("scan_truncated"); r.value_bool(scan_truncated);
        // returned: how many of the sorted (assignable-first) hits we emit.
        size_t emit_n = hits.size() < static_cast<size_t>(max_results) ? hits.size() : static_cast<size_t>(max_results);
        r.key("returned"); r.value_uint(static_cast<uint32_t>(emit_n));
        r.key("targets"); r.begin_array();
        for (size_t k = 0; k < emit_n; k++) {
            const auto& h = hits[k];
            r.begin_object();
            r.key("title_id"); r.value_uint(h.title_id);
            if (!h.name.empty()) { r.key("title_key"); r.value_string(h.name.c_str(), h.name.size()); }
            r.key("holder_id"); r.value_uint(h.holder_id);
            // target-eligible (oracle) is implied by presence; assignable adds the
            // councillor-side check — true ⇒ set_council_task will apply + stick.
            r.key("assignable"); r.value_bool(h.assignable);
            // ck-fabtargets (L23): relevance to the actor — de_jure=true means the
            // county is within the actor's own de-jure duchy/kingdom region (the
            // reconquest/consolidation frontier); these sort to the front.
            r.key("relevance"); r.value_uint(static_cast<uint32_t>(h.relevance));
            r.key("de_jure"); r.value_bool(h.relevance == 0);
            r.end_object();
        }
        r.end_array();
        r.end_object();
        *response_out = r.take();
    }
    return true;
}

// ===========================================================================
// Building construction (CAddConstructionCommand) — RE-resolved dispatch path
// (re/findings/re_building_construction.md "DISPATCH PATH RESOLVED"). The 0x30B
// command's fields: +0x20 actor id, +0x24 location id, +0x28 slot, +0x2c
// building-type index. ADD_CONSTRUCTION_VALIDITY only reads those fields (never
// the vtable), so the dry-run "construction_check" fills a plain buffer and calls
// it WITHOUT dispatching — used to nail the location-id encoding live. The
// dispatch path replicates ADD_CONSTRUCTION_DISPATCH under the RNG-scope guard.
// ===========================================================================
namespace {
    // Resolve a building-type ordinal index by name from BUILDING_TYPE_DB.
    // Returns -1 if not found. (entry+0x8 == array index == the command's +0x2c.)
    int resolve_building_index(const std::string& want, uint32_t* out_count) {
        uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::BUILDING_TYPE_DB);
        uintptr_t arr = db ? safe_read<uintptr_t>(db + 0x88) : 0;
        uint32_t count = db ? safe_read<uint32_t>(db + 0x90) : 0;
        if (out_count) *out_count = count;
        if (!arr || !is_mapped(arr) || count == 0 || count > 5000) return -1;
        for (uint32_t i = 0; i < count; i++) {
            uintptr_t e = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
            if (!e || !is_mapped(e)) continue;
            if (ad_read_std_string(e + 0x10) == want) {
                uint32_t idx = safe_read<uint32_t>(e + 8);  // == i, the command's +0x2c
                return static_cast<int>(idx);
            }
        }
        return -1;
    }
    // Fill the 0x30-byte command's parameter fields (no vtable needed for the
    // validity check; the dispatch path sets the vtable via the real ctor).
    void fill_construction_cmd(uint8_t* cmd /*>=0x30*/, uint32_t actor_id,
                               uint32_t loc_id, uint32_t slot, uint32_t bt_index) {
        *reinterpret_cast<uint32_t*>(cmd + 0x20) = actor_id;
        *reinterpret_cast<uint32_t*>(cmd + 0x24) = loc_id;
        *reinterpret_cast<uint32_t*>(cmd + 0x28) = slot;
        *reinterpret_cast<uint32_t*>(cmd + 0x2c) = bt_index;
    }
}

// Read-only: does the engine consider (actor, location, slot, building) buildable?
// Calls ADD_CONSTRUCTION_VALIDITY (no mutation). Safe to run inline while paused.
bool do_construction_check(const char* params_json, std::string* response_out) {
    const char* bname = json_get_string(params_json, "building");
    if (!bname || !bname[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"construction_check","reason":"missing_building"})";
        return false;
    }
    std::string want(bname);
    uint32_t loc_id = static_cast<uint32_t>(json_get_int(params_json, "location_id"));
    uint32_t slot   = static_cast<uint32_t>(json_get_int(params_json, "slot"));

    void* actor = resolve_actor(params_json);
    if (!actor) {
        if (response_out) *response_out = R"({"success":false,"action":"construction_check","reason":"no_actor"})";
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + 0x18);

    uint32_t count = 0;
    int bt_index = resolve_building_index(want, &count);
    if (bt_index < 0) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("construction_check");
            r.key("reason"); r.value_string("building_not_found");
            r.key("attempted"); r.value_string(want.c_str(), want.size());
            r.key("db_count"); r.value_uint(count);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    uint8_t cmd[0x40]; memset(cmd, 0, sizeof(cmd));
    fill_construction_cmd(cmd, actor_id, loc_id, slot, static_cast<uint32_t>(bt_index));

    auto validity = reinterpret_cast<uint8_t(*)(void*, void*)>(
        g_base_addr + ck3::funcs::ADD_CONSTRUCTION_VALIDITY);
    bool valid = false;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("construction_check");
        return false;
    }
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false; watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"construction_check","reason":"check_crashed"})";
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);
    valid = validity(cmd, nullptr) != 0;
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("construction_check");
        r.key("building"); r.value_string(want.c_str(), want.size());
        r.key("building_index"); r.value_int(bt_index);
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("location_id"); r.value_uint(loc_id);
        r.key("slot"); r.value_uint(slot);
        r.key("buildable"); r.value_bool(valid);
        r.end_object();
        *response_out = r.take();
    }
    return true;
}

// WRITE: start construction of `building` at `location_id` (barony title id) in
// `slot` for the actor. Replicates ADD_CONSTRUCTION_DISPATCH (FUN_047f5570) with
// an EXPLICIT actor (the UI wrapper uses the local player). Builds the command via
// the real ctor (sets the vtable) and dispatches through GAME_COMMAND_DISPATCH
// (mode 0xe) under the RNG-scope guard + watchdog. The engine validates and
// applies server-side on a tick (the UI-side dry-run check is UI-gated and not
// predictive). Player-guarded. Verify by polling query_county_details.
bool do_add_construction(const char* params_json, std::string* response_out) {
    const char* bname = json_get_string(params_json, "building");
    if (!bname || !bname[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"add_construction","reason":"missing_building"})";
        return false;
    }
    std::string want(bname);
    uint32_t loc_id = static_cast<uint32_t>(json_get_int(params_json, "location_id"));
    uint32_t slot   = static_cast<uint32_t>(json_get_int(params_json, "slot"));
    if (loc_id == 0) {
        if (response_out) *response_out = R"({"success":false,"action":"add_construction","reason":"missing_location_id"})";
        return false;
    }

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "add_construction", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"add_construction","reason":"no_living_actor"})";
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + 0x18);  // FULL engine id

    uint32_t count = 0;
    int bt_index = resolve_building_index(want, &count);
    if (bt_index < 0) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("add_construction");
            r.key("reason"); r.value_string("building_not_found");
            r.key("attempted"); r.value_string(want.c_str(), want.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    ADLOG("add_construction: ENTER actor_id=0x%x loc=%u slot=%u bt='%s' idx=%d\n",
          actor_id, loc_id, slot, want.c_str(), bt_index);

    // Function pointers (see ck3_offsets.h funcs: construction + dispatch).
    auto ctor = reinterpret_cast<void(*)(void*, uint32_t, uint32_t, uint32_t, void*)>(
        g_base_addr + ck3::funcs::ADD_CONSTRUCTION_CTOR);
    auto dispatch = reinterpret_cast<void(*)(void*, void*, int, int)>(
        g_base_addr + ck3::funcs::GAME_COMMAND_DISPATCH);
    auto base_dtor = reinterpret_cast<void(*)(void*)>(g_base_addr + 0x0755b650);  // FUN_0765b650

    // bt_ref: ctor reads cmd+0x2c = *(bt_ref+8). {pad8; u32 building_index @ +8}.
    uint8_t bt_ref[16]; memset(bt_ref, 0, sizeof(bt_ref));
    *reinterpret_cast<uint32_t*>(bt_ref + 8) = static_cast<uint32_t>(bt_index);

    uint8_t cmd[0x40]; memset(cmd, 0, sizeof(cmd));
    void* out_a = nullptr;
    void* out_b = nullptr;

    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("add_construction");
        return false;
    }
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false; watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"add_construction","reason":"dispatch_crashed"})";
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);

    // Build the command (sets vtable + fields), then prepare + dispatch (mode 0xe).
    ctor(cmd, actor_id, loc_id, slot, bt_ref);
    void** vt = *reinterpret_cast<void***>(cmd);            // cmd[0] = vtable
    auto prepare = reinterpret_cast<void*(*)(void*, void*)>(vt[9]);  // vtable+0x48
    prepare(&out_a, cmd);
    out_b = out_a; out_a = nullptr;
    dispatch(reinterpret_cast<void*>(g_base_addr + ck3::globals::COMMAND_DISPATCH_MGR),
             &out_b, 0xe, 0);
    // Cleanup: virtual dtor (vtable+8) on out holders, then base dtor on cmd.
    if (out_b) { auto d = reinterpret_cast<void(*)(void*)>((*reinterpret_cast<void***>(out_b))[1]); d(out_b); }
    if (out_a) { auto d = reinterpret_cast<void(*)(void*)>((*reinterpret_cast<void***>(out_a))[1]); d(out_a); }
    base_dtor(cmd);

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();
    ADLOG("add_construction: dispatched\n");

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("add_construction");
        r.key("dispatched"); r.value_bool(true);
        r.key("building"); r.value_string(want.c_str(), want.size());
        r.key("building_index"); r.value_int(bt_index);
        r.key("location_id"); r.value_uint(loc_id);
        r.key("slot"); r.value_uint(slot);
        r.end_object();
        *response_out = r.take();
    }
    return true;
}

// Resolve a CBuildingType* object by name from BUILDING_TYPE_DB (the pointer, not
// just the index). Returns 0 if not found.
static uintptr_t resolve_building_obj(const std::string& want) {
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::BUILDING_TYPE_DB);
    uintptr_t arr = db ? safe_read<uintptr_t>(db + 0x88) : 0;
    uint32_t count = db ? safe_read<uint32_t>(db + 0x90) : 0;
    if (!arr || !is_mapped(arr) || count == 0 || count > 5000) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t e = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (!e || !is_mapped(e)) continue;
        if (ad_read_std_string(e + 0x10) == want) return e;
    }
    return 0;
}

// WRITE (non-gated): start constructing `building` in the holding at `location_id`
// (a barony title id) for the actor. Uses the CAddBuildingEffect apply path
// (re/findings/re_building_construction.md DC35-40) — the scripted-effect builder
// the game AI uses, which has NO player gate (unlike the CAddConstructionCommand
// UI path). Steps: resolve CBuildingType* by name; resolve the province from the
// title scope ({i16 8; u32 id@+8} → CONSTRUCTION_SCOPE_TO_PROVINCE); then
// CONSTRUCTION_START(province+0x1F8, building, 0) which sets the holding's
// under-construction building at slot_container+0x60 (free; effect path).
// Player-guarded; game-thread action; RNG-scope guarded.
bool do_construct_building(const char* params_json, std::string* response_out) {
    const char* bname = json_get_string(params_json, "building");
    if (!bname || !bname[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"construct_building","reason":"missing_building"})";
        return false;
    }
    std::string want(bname);
    uint32_t loc_id = static_cast<uint32_t>(json_get_int(params_json, "location_id"));
    if (loc_id == 0) {
        if (response_out) *response_out = R"({"success":false,"action":"construct_building","reason":"missing_location_id"})";
        return false;
    }

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "construct_building", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"construct_building","reason":"no_living_actor"})";
        return false;
    }

    uintptr_t building = resolve_building_obj(want);
    if (!building) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("construct_building");
            r.key("reason"); r.value_string("building_not_found");
            r.key("attempted"); r.value_string(want.c_str(), want.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    ADLOG("construct_building: ENTER bld='%s' obj=%lx loc=%u actor=%p\n",
          want.c_str(), (unsigned long)building, loc_id, actor);

    auto precheck = reinterpret_cast<bool(*)(void*, void*)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_PRECHECK);
    // category-specific starters (CAddBuildingEffect apply routing)
    auto start_special = reinterpret_cast<void(*)(void*, void*, int)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_START);            // cat 2
    auto start_duchy   = reinterpret_cast<void(*)(void*, void*)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_START_DUCHY);      // cat 1
    auto find_slot     = reinterpret_cast<int(*)(void*, void*)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_FIND_SLOT);        // cat 0
    auto find_slot_fb  = reinterpret_cast<int(*)(void*)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_FIND_SLOT_FB);
    auto finalize_pend = reinterpret_cast<void(*)(void*)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_FINALIZE_PEND);
    auto start_normal  = reinterpret_cast<void(*)(void*, void*, int, uint32_t)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_START_NORMAL);
    auto advance       = reinterpret_cast<void(*)(void*, int)>(
        g_base_addr + ck3::funcs::CONSTRUCTION_ADVANCE);

    // Resolve the province directly: LANDED_TITLE_DB[loc_id] (barony title obj) →
    // PROVINCE_PTR (+0x490). loc_id is the masked barony title id; the scope
    // resolver FUN_0632a6a0 indexes the PROVINCE array (wrong id space), so we
    // navigate the title DB instead (the path query_county_details uses).
    uintptr_t province = 0;
    uintptr_t barony_obj = 0;
    {
        uintptr_t tdb = safe_read_offset<uintptr_t>(ck3::globals::LANDED_TITLE_DB);
        uintptr_t table = tdb ? safe_read<uintptr_t>(tdb + ck3::title_db::ARRAY_PTR) : 0;
        uint32_t cap = tdb ? safe_read<uint32_t>(tdb + ck3::title_db::ARRAY_COUNT) : 0;
        uint32_t idx = loc_id & 0xFFFFFF;
        if (table && is_mapped(table) && idx < cap) {
            uintptr_t barony = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                    + static_cast<uintptr_t>(idx) * ck3::title_db::ELEMENT_STRIDE);
            if (barony && is_mapped(barony)
                && (safe_read<uint32_t>(barony + ck3::title_db::ID_VERIFY_OFFSET) & 0xFFFFFF) == idx) {
                barony_obj = barony;
                province = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
            }
        }
    }

    // #133 build_gate (PURE-MEMORY ownership pre-gate — zero engine calls, crash-free).
    // Refuse a build on a holding the actor does NOT own/control: the barony's holder must
    // be the actor OR the actor must appear in the holder's liege chain (bounded walk).
    // This closes the wrong-holder cheat class. The deeper terrain/innovation/slot gate via
    // the engine conjunction (FUN_05c7ea00 et al.) is OPT-IN (engine_gate=true) below.
    uint32_t actor_full_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID);
    uint32_t actor_id = actor_full_id & 0xFFFFFF;
    bool do_execute = json_get_bool(params_json, "execute", false);
    if (do_execute && barony_obj) {
        uint32_t holder_id = safe_read<uint32_t>(barony_obj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
        bool owns = false;
        if (holder_id != 0 && holder_id != 0xFFFFFF) {
            if (holder_id == actor_id) {
                owns = true;
            } else {
                // Walk the holder's liege chain up to the top realm; the actor owns the
                // holding if they are any liege above the direct holder. Bounded to 16 hops.
                uint32_t cur = holder_id;
                for (int hop = 0; hop < 16 && cur != 0 && cur != 0xFFFFFF; hop++) {
                    void* cc = lookup_character(cur);
                    if (!cc || is_sentinel(cc)) break;
                    uint32_t liege = get_liege_id_from_addr(reinterpret_cast<uintptr_t>(cc));
                    if (liege == 0 || liege == 0xFFFFFF) break;   // reached an independent top
                    if (liege == actor_id) { owns = true; break; }
                    if (liege == cur) break;                       // self-loop guard
                    cur = liege;
                }
            }
        }
        if (!owns) {
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("construct_building");
                r.key("building"); r.value_string(want.c_str(), want.size());
                r.key("location_id"); r.value_uint(loc_id);
                r.key("reason"); r.value_string("refused_not_buildable");
                r.key("detail"); r.value_string("not_owner_or_realm");
                r.end_object();
                *response_out = r.take();
            }
            ADLOG("construct_building: REFUSED not_owner_or_realm actor=%u barony_holder=%u loc=%u\n",
                  actor_id, holder_id, loc_id);
            return false;
        }
    }

    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("construct_building");
        return false;
    }
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false; watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"construct_building","reason":"build_crashed"})";
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);

    // SAFETY: CONSTRUCTION_START (FUN_0611ffa0) currently HARD-CRASHES the game
    // when called from the bridge thread (it pushes to the gamestate construction
    // manager / recomputes the holding via FUN_06120050/FUN_0611fad0 in a way that
    // is unsafe outside the full effect-execution context — SIGABRT, not a
    // catchable SIGSEGV). So the builder call is gated behind an explicit
    // execute=true opt-in; by default this handler only RESOLVES + reports the
    // building object + province (safe) for debugging the invocation. See
    // re/findings/re_building_construction.md (CAddBuildingEffect path).
    // (do_execute resolved earlier for the #133 ownership pre-gate.)
    ADLOG("construct_building: province=%lx execute=%d\n",
          (unsigned long)province, do_execute ? 1 : 0);
    bool started = false;
    bool already = false;
    bool construct_threw = false;
    int32_t category = -1;
    int slot_idx = -1;
    uintptr_t under_construction = 0;
    if (province && is_mapped(province)) {
        uintptr_t slot_container = province + 0x1F8;
        category = safe_read<int32_t>(building + ck3::funcs::BUILDING_CATEGORY_OFF);  // safe read
        // NOTE: every construction game-function (even the read-only precheck
        // FUN_0611d430) ASSERTS/crashes when called outside the game's effect-
        // execution context (confirmed: a resolve-only call that invoked precheck
        // hard-crashed). So ALL game calls are gated behind execute=true; default
        // (resolve-only) uses pure safe_read. See re_building_construction.md.
        bool threw = false;
        if (do_execute) try {
            void* sc = reinterpret_cast<void*>(slot_container);
            void* bo = reinterpret_cast<void*>(building);
            ADLOG("construct_building: [exec] calling precheck...\n");
            already = precheck(sc, bo);
            ADLOG("construct_building: [exec] precheck=%d category=%d\n", already?1:0, category);
            if (!already) {
            if (category == 0) {
                // normal: find the family slot (or a free slot), then start + advance.
                slot_idx = find_slot(sc, bo);
                ADLOG("construct_building: [exec] find_slot=%d\n", slot_idx);
                if (slot_idx < 0) { slot_idx = find_slot_fb(sc); ADLOG("construct_building: [exec] find_slot_fb=%d\n", slot_idx); }
                if (slot_idx >= 0) {
                    ADLOG("construct_building: [exec] finalize_pend...\n");
                    finalize_pend(sc);
                    ADLOG("construct_building: [exec] start_normal...\n");
                    start_normal(sc, bo, slot_idx, 0xffffffff);
                    ADLOG("construct_building: [exec] advance...\n");
                    advance(sc, 1);
                    ADLOG("construct_building: [exec] normal sequence returned\n");
                    // verify: under-construction building at slot_container+0x70
                    under_construction = safe_read<uintptr_t>(slot_container + 0x70);
                    started = (under_construction == building);
                }
            } else if (category == 1) {
                start_duchy(sc, bo);
                under_construction = safe_read<uintptr_t>(slot_container + 0x60);
                started = (under_construction == building);
            } else {  // category 2 (special) or unknown
                start_special(sc, bo, 0);
                under_construction = safe_read<uintptr_t>(slot_container + 0x60);
                started = (under_construction == building);
            }
            }  // if (!already)
        } catch (...) {
            // CK3 game functions can throw C++ exceptions; an uncaught throw on
            // the drain thread → std::terminate → SIGABRT (the "bridge
            // disconnected" crash). The scripted-effect executor catches the
            // same way. If the throw is post-mutation, the build still applied.
            threw = true;
            ADLOG("construct_building: [exec] C++ EXCEPTION caught\n");
        }
        // Verify: effect-path construction applies INSTANTLY (free), so the new
        // building lands directly in the slot array (slot_container+0x18, stride
        // 0x10, building ptr @ +0). Also accept under-construction at +0x70/+0x60.
        if (do_execute) {
            uintptr_t slot_arr = safe_read<uintptr_t>(slot_container + 0x18);
            uintptr_t in_slot = (slot_arr && slot_idx >= 0)
                ? safe_read<uintptr_t>(slot_arr + static_cast<uintptr_t>(slot_idx) * 0x10) : 0;
            uintptr_t uc70 = safe_read<uintptr_t>(slot_container + 0x70);
            uintptr_t uc60 = safe_read<uintptr_t>(slot_container + 0x60);
            if (in_slot == building || uc70 == building || uc60 == building) {
                under_construction = building;
                started = true;
            }
            ADLOG("construct_building: [exec] post threw=%d in_slot=%lx uc70=%lx uc60=%lx started=%d\n",
                  threw?1:0, (unsigned long)in_slot, (unsigned long)uc70, (unsigned long)uc60, started?1:0);
        }
        construct_threw = threw;
    }

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();
    ADLOG("construct_building: started=%d already=%d uc=%lx\n",
          started ? 1 : 0, already ? 1 : 0, (unsigned long)under_construction);

    ADLOG("construct_building: category=%d slot_idx=%d started=%d uc=%lx\n",
          category, slot_idx, started ? 1 : 0, (unsigned long)under_construction);
    bool resolved = (province != 0);
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(do_execute ? (started || already) : resolved);
        r.key("action"); r.value_string("construct_building");
        r.key("building"); r.value_string(want.c_str(), want.size());
        r.key("location_id"); r.value_uint(loc_id);
        r.key("resolved"); r.value_bool(resolved);   // building obj + province both resolved
        r.key("category"); r.value_int(category);
        r.key("executed"); r.value_bool(do_execute);
        if (construct_threw) { r.key("threw_caught"); r.value_bool(true); }
        if (province == 0) {
            r.key("reason"); r.value_string("invalid_location");
        } else if (already) {
            r.key("reason"); r.value_string("already_present_or_constructing");
        } else if (do_execute) {
            r.key("construction_started"); r.value_bool(started);
        } else {
            r.key("note"); r.value_string("resolve-only (dry run); pass execute=true to actually start construction — VERIFIED WORKING 20260614 (castle_01→castle_02)");
        }
        r.end_object();
        *response_out = r.take();
    }
    return do_execute ? (started || already) : resolved;
}

bool do_declare_war(const char* params_json, std::string* response_out) {
    // ck-j3vb0: force_player_path bypass.
    // The default declare_war path queues a WAR_DECLARE intent → AI war
    // pipeline (target eval + post_decl with force=1), which routinely
    // returns dispatched_but_no_war when the AI's evaluator rejects the
    // suggested target (target_id is a HINT to the AI, not a command).
    //
    // When force_player_path=true, route directly through WAR_CREATE_CORE
    // via the WAR_CREATE_CORE_DIRECT intent (the "force" path). This skips
    // both the AI evaluator AND CTX_VALIDATE — guaranteed war declaration
    // assuming the engine's CB_VALIDITY_CHECK accepts the (actor, target,
    // CB, title) tuple. Required param when this flag is set:
    //   casus_belli — CB script name (e.g. "claim_cb"). Use
    //                 query_cb_validity_strict to obtain the CB name string.
    bool force_player_path = json_get_bool(params_json, "force_player_path", false);
    if (force_player_path) {
        const char* cb_name = json_get_string(params_json, "casus_belli");
        if (!cb_name || !cb_name[0]) {
            cb_name = json_get_string(params_json, "cb_name");
        }
        if (!cb_name || !cb_name[0]) {
            ADLOG("declare_war: force_player_path=true but casus_belli missing\n");
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("declare_war");
                r.key("path"); r.value_string("force_player_path");
                r.key("reason"); r.value_string("casus_belli_required_for_force_player_path");
                r.key("hint"); r.value_string(
                    "force_player_path=true bypasses AI target evaluation by calling "
                    "WAR_CREATE_CORE directly, which requires the CB by name (not index). "
                    "Pass casus_belli=\"<cb_script_name>\" — use "
                    "discover(what='valid_cbs_strict') to enumerate valid CB names.");
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
        ADLOG("declare_war: force_player_path=true — routing through WAR_CREATE_CORE direct (cb=%s)\n",
              cb_name);
        return do_call_war_create_core(params_json, response_out,
                                        /*action_label=*/"declare_war",
                                        /*include_path_marker=*/true);
    }

    int target_id = json_get_int(params_json, "target_id");
    if (target_id < 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war: missing target_id\n");
        if (response_out) *response_out = R"({"success":false,"action":"declare_war","reason":"missing_target_id"})";
        return false;
    }

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "declare_war", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war: no living actor character\n");
        if (response_out) *response_out = R"({"success":false,"action":"declare_war","reason":"no_living_actor"})";
        return false;
    }

    void* target = lookup_character(static_cast<uint32_t>(target_id));
    if (!target || is_sentinel(target) || !g_char_is_alive(target)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war: target character %d not found or dead\n", target_id);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("declare_war");
            r.key("reason"); r.value_string("invalid_target");
            r.key("attempted_id"); r.value_int(target_id);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    int cb_title_id = json_get_int(params_json, "cb_title_id");

    // Parse war-specific parameters for CB injection
    int claimant_raw = json_get_int(params_json, "claimant_id");
    uint32_t claimant_id = (claimant_raw > 0) ? static_cast<uint32_t>(claimant_raw) : 0;
    int cb_index = json_get_int(params_json, "cb_index");

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war: parsed cb_title_id=%d claimant_id=%u cb_index=%d\n",
                           cb_title_id, claimant_id, cb_index);
    if (cb_index == 0) {
        const char* cb_idx_str = strstr(params_json, "\"cb_index\"");
        if (!cb_idx_str) cb_index = -1;  // not provided -> auto-select
    }

    // =====================================================================
    // PRE-VALIDATION: Check memory-readable preconditions before dispatching.
    // Delegates to do_pre_validate_war() which checks 20 conditions (A1-A13,
    // R1-R4, S1-S3) via fast memory reads. This catches common failures
    // (imprisoned, incapable, already at war, in debt, truce, alliance, etc.)
    // without burning a game tick on a doomed intent.
    // Skip if caller explicitly sets "skip_prevalidation": true.
    // =====================================================================
    uint32_t actor_id = read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID) & 0xFFFFFF;

    bool skip_preval = json_get_bool(params_json, "skip_prevalidation", false);
    if (!skip_preval) {
        std::string preval_result;
        do_pre_validate_war(params_json, &preval_result);
        // Parse the "valid" field from the JSON response
        const char* valid_str = strstr(preval_result.c_str(), "\"valid\":");
        bool all_conditions_pass = true;
        if (valid_str) {
            valid_str += 8;  // skip past "\"valid\":"
            while (*valid_str == ' ') valid_str++;
            all_conditions_pass = (strncmp(valid_str, "true", 4) == 0);
        }

        if (!all_conditions_pass) {
            ADLOG("declare_war: pre-validation FAILED — aborting before dispatch\n");
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("declare_war");
                r.key("status"); r.value_string("pre_validation_failed");
                r.key("reason"); r.value_string("one_or_more_preconditions_failed");
                r.key("hint"); r.value_string(
                    "Use discover(what='war_validity') for per-condition details. "
                    "Pass skip_prevalidation=true to bypass this check.");
                r.key("pre_validation");
                r.begin_object();
                r.key("raw"); r.value_string(preval_result.c_str(), preval_result.size());
                r.end_object();
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
        ADLOG("declare_war: pre-validation PASSED — proceeding to dispatch\n");
    }

    // Pre-flight: read actor war count before dispatch for verification
    int32_t wars_before = -1;
    uintptr_t actor_tr = resolve_title_realm(reinterpret_cast<uintptr_t>(actor));
    if (actor_tr) {
        wars_before = read<int32_t>(actor_tr + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
    }

    // Pre-flight: read global WAR_DB active war count for silent creation detection
    int32_t global_wars_before = -1;
    uintptr_t war_db_ptr = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (war_db_ptr) {
        uintptr_t wdb_table = safe_read<uintptr_t>(war_db_ptr + ck3::hash_map::TABLE_PTR);
        uint32_t wdb_capacity = safe_read<uint32_t>(war_db_ptr + ck3::hash_map::CAPACITY);
        if (wdb_table && wdb_capacity > 0 && wdb_capacity < 100000) {
            int32_t count = 0;
            for (uint32_t idx = 0; idx < wdb_capacity && idx < 10000; idx++) {
                uintptr_t entry = wdb_table + ck3::hash_map::ENTRY_HEADER_SIZE
                                  + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                uintptr_t wp = safe_read<uintptr_t>(entry);
                if (!wp || !safe_ptr(wp)) continue;
                uint32_t sref = safe_read<uint32_t>(wp + ck3::war_obj::WAR_ID);
                if ((sref & 0xFFFFFF) != idx) continue;
                uint8_t ended = safe_read<uint8_t>(wp + ck3::war_obj::IS_ENDED);
                if (ended == 0) count++;
            }
            global_wars_before = count;
        }
    }

    // Generate unique request_id
    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "dw_%u_%lu_%u",
             actor_id & 0xFFFFFF, (unsigned long)current_tick,
             generate_rng() & 0xFFFF);

    // Route through the WAR_DECLARE intent path so drain_and_execute_intents
    // dispatches via execute_war_intent (AI war pipeline: target eval →
    // declare exec → post-decl with force=1). The prior INTERACTION route
    // went through CSendCharacterInteractionCommand which is structurally
    // incapable of reaching WAR_CREATE_CORE (see ck-e58l.17.1). The AI
    // pipeline is one of the 25 type-7 callers that does reach war creation.
    // Caveat: the AI selects the actual target and CB; target_id and
    // cb_title_id become suggestions rather than commands (ck-e58l.24 FIX-1
    // Option A).
    bool queued = queue_intent(
        actor_id, static_cast<uint32_t>(target_id),
        "declare_war_interaction",
        /*max_age_ticks=*/10,
        /*request_id=*/req_id_buf,
        /*force_bypass=*/false,
        /*claimant_id=*/claimant_id,
        /*cb_index=*/cb_index,
        /*secondary_actor_id=*/0,
        /*secondary_recipient_id=*/0,
        /*title_id=*/cb_title_id,
        /*debug_all_paths=*/false,
        /*intent_type=*/IntentType::WAR_DECLARE);

    std::string request_id(req_id_buf);

    if (!queued) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("declare_war");
            r.key("reason"); r.value_string("queue_intent_failed");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    ADLOG("BT_TRACE do_declare_war: enqueued internal_rid=%s — entering 5s poll\n",
          request_id.c_str());

    // Wait for intent completion — poll completed_snapshot up to 5 seconds.
    // The intent executes on the next game tick (drain_and_execute_intents),
    // which fires at the start of each tick in the serial section.
    // This makes declare_war SYNCHRONOUS: the caller gets the actual result
    // instead of a "queued" status that requires separate polling.
    bool found_result = false;
    bool intent_succeeded = false;
    char result_reason[128] = {};

    for (int poll = 0; poll < 100; poll++) {  // 100 * 50ms = 5 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                if (c.rejected && c.rejection_reason[0]) {
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                }
                ADLOG("BT_TRACE do_declare_war: poll_found internal_rid=%s executed=%d rejected=%d after=%dms\n",
                      request_id.c_str(), c.executed ? 1 : 0, c.rejected ? 1 : 0,
                      (poll + 1) * 50);
                break;
            }
        }
        if (found_result) break;
    }

    // Post-flight: read war counts after dispatch.
    // War creation effects are DEFERRED via CPendingCharacterInteraction — the
    // engine queues the effect during InteractionTrySend and processes it on a
    // subsequent game tick (see re/findings/re_effect_application.md).
    // So we poll war count for up to 2 additional seconds after intent success.
    int32_t wars_after = -1;
    int32_t global_wars_after = -1;
    bool war_created = false;
    bool global_war_created = false;

    // Helper: read global WAR_DB active war count
    auto read_global_wars = [&]() -> int32_t {
        if (!war_db_ptr) return -1;
        uintptr_t wdb_table = safe_read<uintptr_t>(war_db_ptr + ck3::hash_map::TABLE_PTR);
        uint32_t wdb_capacity = safe_read<uint32_t>(war_db_ptr + ck3::hash_map::CAPACITY);
        if (!wdb_table || wdb_capacity == 0 || wdb_capacity >= 100000) return -1;
        int32_t count = 0;
        for (uint32_t idx = 0; idx < wdb_capacity && idx < 10000; idx++) {
            uintptr_t entry = wdb_table + ck3::hash_map::ENTRY_HEADER_SIZE
                              + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
            uintptr_t wp = safe_read<uintptr_t>(entry);
            if (!wp || !safe_ptr(wp)) continue;
            uint32_t sref = safe_read<uint32_t>(wp + ck3::war_obj::WAR_ID);
            if ((sref & 0xFFFFFF) != idx) continue;
            uint8_t ended = safe_read<uint8_t>(wp + ck3::war_obj::IS_ENDED);
            if (ended == 0) count++;
        }
        return count;
    };

    // Check war counts — may need multiple reads due to deferred effect processing
    auto check_war_counts = [&]() {
        if (actor_tr) {
            wars_after = read<int32_t>(actor_tr + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
            war_created = (wars_before >= 0 && wars_after > wars_before);
        }
        if (!war_created) {
            global_wars_after = read_global_wars();
            global_war_created = (global_wars_before >= 0 && global_wars_after > global_wars_before);
        }
    };

    check_war_counts();

    // If intent succeeded but no war detected yet, poll for deferred effect processing.
    if (found_result && intent_succeeded && !war_created && !global_war_created) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] declare_war: intent succeeded but no war yet — "
            "polling for deferred effect (up to 2s)\n");
        for (int retry = 0; retry < 40; retry++) {  // 40 * 50ms = 2 seconds
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check_war_counts();
            if (war_created || global_war_created) {
                if (g_logfile) fprintf(g_logfile,
                    "[CK3-Bridge] declare_war: deferred war detected after %dms\n",
                    (retry + 1) * 50);
                break;
            }
        }
    }

    // ck-65la: do NOT upgrade global-DB-only deltas to war_created. In the
    // WAR_DECLARE path the actor is the attacker, whose active_wars list must
    // gain the new war. Other AI characters declare wars every few ticks in a
    // populated game, and the prior "war may be target-owned" fallback
    // misattributed those unrelated wars to our dispatch — producing a false
    // positive that masked AI_WAR_POST_DECL silently rejecting our actor's
    // declaration despite force=1 and candidate count > 0. Keep the
    // global-delta visible in the response fields for downstream diagnosis,
    // but report the honest actor-level outcome in `status`.
    if (!war_created && global_war_created) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war: global WAR_DB gained a war "
            "(global %d→%d) but actor's active_wars did not change (%d→%d) — NOT attributed to this dispatch "
            "(likely concurrent AI-initiated war; see ck-65la)\n",
            global_wars_before, global_wars_after, wars_before, wars_after);
    }

    ADLOG("BT_TRACE do_declare_war: post-poll internal_rid=%s found_result=%d succeeded=%d war_created=%d — building response\n",
          request_id.c_str(), found_result ? 1 : 0, intent_succeeded ? 1 : 0, war_created ? 1 : 0);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("action"); r.value_string("declare_war");
        // ck-hzq3: renamed to avoid colliding with IPC-injected request_id.
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        // ck-jsis0: canonical dispatch identifier alias.
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        r.key("target_id"); r.value_int(target_id);

        if (!found_result) {
            // Timeout — intent may still be pending (game paused?)
            r.key("success"); r.value_bool(false);
            r.key("status"); r.value_string("timeout");
            r.key("reason"); r.value_string("intent_not_completed_within_5s");
            r.key("hint"); r.value_string("Game must be unpaused for the intent to execute. "
                                           "Check intent results with query_intent_results.");
        } else if (intent_succeeded && war_created) {
            // Best case: intent ran AND war count increased
            r.key("success"); r.value_bool(true);
            r.key("status"); r.value_string("war_created");
        } else if (intent_succeeded && !war_created) {
            // AI war pipeline ran but no war count increase. Under the
            // WAR_DECLARE routing (see ck-e58l.24 FIX-1), this usually means
            // the AI target-evaluation rejected the suggested target or
            // picked zero candidates; target_id is a hint, not a command.
            r.key("success"); r.value_bool(false);
            r.key("status"); r.value_string("dispatched_but_no_war");
            r.key("reason"); r.value_string("ai_pipeline_ran_but_did_not_declare");
            r.key("hint"); r.value_string("The AI war pipeline executed but did not declare. "
                                           "Most common cause: AI rejected the suggested target "
                                           "(candidate count 0, no valid CB, or truce in effect). "
                                           "Check bridge log for 'candidate target count'. "
                                           "To force a specific war regardless of AI evaluation, "
                                           "set force_player_path=true and pass "
                                           "casus_belli=\"<cb_script_name>\" (use "
                                           "discover(what='valid_cbs_strict') for valid CB names).");
        } else {
            // Intent was rejected with a reason
            r.key("success"); r.value_bool(false);
            r.key("status"); r.value_string("rejected");
            if (result_reason[0]) {
                r.key("reason"); r.value_string(result_reason);
            } else {
                r.key("reason"); r.value_string("unknown_rejection");
            }
        }

        r.key("wars_before"); r.value_int(wars_before);
        r.key("wars_after"); r.value_int(wars_after);
        r.key("war_count_changed"); r.value_bool(war_created);
        r.key("global_wars_before"); r.value_int(global_wars_before);
        r.key("global_wars_after"); r.value_int(global_wars_after);
        r.key("global_war_count_changed"); r.value_bool(global_war_created);
        r.end_object();
        *response_out = r.take();
    }

    ADLOG("BT_TRACE do_declare_war: returning internal_rid=%s response_size=%zu\n",
          request_id.c_str(), response_out ? response_out->size() : 0);

    return found_result && intent_succeeded && war_created;
}

// =============================================================================
// War Declaration Debug — try all dispatch paths
// =============================================================================

bool do_declare_war_debug(const char* params_json, std::string* response_out) {
    int target_id = json_get_int(params_json, "target_id");
    if (target_id < 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war_debug: missing target_id\n");
        if (response_out) *response_out = R"({"success":false,"action":"declare_war_debug","reason":"missing_target_id"})";
        return false;
    }

    void* actor = resolve_actor(params_json);
    if (!actor || !g_char_is_alive(actor)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war_debug: no living actor character\n");
        if (response_out) *response_out = R"({"success":false,"action":"declare_war_debug","reason":"no_living_actor"})";
        return false;
    }

    void* target = lookup_character(static_cast<uint32_t>(target_id));
    if (!target || is_sentinel(target) || !g_char_is_alive(target)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] declare_war_debug: target %d not found or dead\n", target_id);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("declare_war_debug");
            r.key("reason"); r.value_string("invalid_target");
            r.key("attempted_id"); r.value_int(target_id);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    uint32_t actor_id = read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID);
    int cb_title_id = json_get_int(params_json, "cb_title_id");

    int claimant_raw = json_get_int(params_json, "claimant_id");
    uint32_t claimant_id = (claimant_raw > 0) ? static_cast<uint32_t>(claimant_raw) : 0;
    int cb_index = json_get_int(params_json, "cb_index");
    if (cb_index == 0) {
        const char* cb_idx_str = strstr(params_json, "\"cb_index\"");
        if (!cb_idx_str) cb_index = -1;
    }

    // Generate unique request_id
    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "dwd_%u_%lu_%u",
             actor_id & 0xFFFFFF, (unsigned long)current_tick,
             generate_rng() & 0xFFFF);

    // Queue with debug_all_paths flag — game thread will try every path
    bool queued = queue_intent(
        actor_id, static_cast<uint32_t>(target_id),
        "declare_war_interaction",
        /*max_age_ticks=*/20,  // longer TTL for debug (tries multiple paths)
        /*request_id=*/req_id_buf,
        /*force_bypass=*/false,
        /*claimant_id=*/claimant_id,
        /*cb_index=*/cb_index,
        /*secondary_actor_id=*/0,
        /*secondary_recipient_id=*/0,
        /*title_id=*/cb_title_id,
        /*debug_all_paths=*/true);

    std::string request_id(req_id_buf);

    if (!queued) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("declare_war_debug");
            r.key("reason"); r.value_string("queue_intent_failed");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // Poll for completion — allow up to 10 seconds (debug tries 5 paths)
    bool found_result = false;
    bool any_war = false;
    std::string debug_log;

    for (int poll = 0; poll < 200; poll++) {  // 200 * 50ms = 10 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                any_war = c.executed && !c.rejected;
                debug_log = c.debug_log;
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        if (!found_result) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("declare_war_debug");
            r.key("status"); r.value_string("timeout");
            r.key("reason"); r.value_string("debug_dispatch_not_completed_within_10s");
            r.key("hint"); r.value_string("Game must be unpaused for the intent to execute.");
            r.end_object();
            *response_out = r.take();
        } else if (!debug_log.empty()) {
            // The debug_log is already a complete JSON object with paths_tried, etc.
            // Wrap it in the action response envelope
            *response_out = "{\"success\":";
            *response_out += any_war ? "true" : "false";
            // ck-hzq3: renamed to avoid colliding with IPC-injected request_id.
            // ck-jsis0: dispatch_id alias surfaces alongside.
            *response_out += ",\"action\":\"declare_war_debug\",\"intent_request_id\":\"";
            *response_out += request_id;
            *response_out += "\",\"dispatch_id\":\"";
            *response_out += request_id;
            *response_out += "\",\"target_id\":";
            *response_out += std::to_string(target_id);
            *response_out += ",";
            // Splice in debug_log contents (skip opening brace)
            *response_out += debug_log.substr(1);  // removes leading '{', keeps trailing '}'
        } else {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("declare_war_debug");
            r.key("status"); r.value_string("completed_no_debug_data");
            r.key("reason"); r.value_string("intent completed but no debug log was captured");
            r.end_object();
            *response_out = r.take();
        }
    }

    return found_result && any_war;
}

// =============================================================================
// Pause / Speed Control
// =============================================================================

bool do_pause_game(const char* /*params_json*/) {
    game_pause();
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Game paused\n");
    return true;
}

bool do_unpause_game(const char* /*params_json*/) {
    game_unpause();
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Game unpaused\n");
    return true;
}

bool do_set_speed(const char* params_json) {
    int speed = json_get_int(params_json, "speed");
    if (speed < 1 || speed > 5) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_speed: speed must be 1-5, got %d\n", speed);
        return false;
    }
    game_set_speed(speed);
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Game speed set to %d\n", speed);
    return true;
}
#ifdef CK3_BRIDGE_DEBUG_VERBS  // rc2 B2: input injection is compiled out of release builds

// =============================================================================
// X11 Mouse Click Injection (ck-77pb, rewritten under ck-vfj7)
//
// CK3 does not link or load SDL2 (verified: ck3 binary's NEEDED list and the
// process map both contain zero libSDL*). The original SDL_PushEvent path was
// based on a wrong premise and `dlsym(RTLD_DEFAULT, "SDL_PushEvent")` always
// returned nil. CK3 talks to the display server through libX11 directly
// (NEEDED in the ck3 binary), and libXtst is already loaded into the process,
// so we drive synthetic input via the XTest extension.
//
// We dlopen libX11/libXtst by soname rather than relying on RTLD_DEFAULT —
// CK3 itself doesn't directly NEED libXtst (it gets pulled in indirectly), so
// global lookup is not guaranteed to find XTestFake* even though the .so is
// mapped. dlopen+dlsym against a private handle is robust either way.
// =============================================================================

#include <mutex>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>

namespace {

using fn_x_open_display_t  = void* (*)(const char*);
using fn_x_close_display_t = int   (*)(void*);
using fn_x_flush_t         = int   (*)(void*);
using fn_x_sync_t          = int   (*)(void*, int /*discard*/);
using fn_xtest_motion_t    = int   (*)(void* /*dpy*/, int /*screen*/,
                                       int /*x*/,    int /*y*/,
                                       unsigned long /*delay*/);
using fn_xtest_button_t    = int   (*)(void* /*dpy*/, unsigned int /*button*/,
                                       int /*is_press*/, unsigned long /*delay*/);

struct XInjector {
    std::mutex mu;
    bool       resolve_attempted = false;
    void*      libx11   = nullptr;
    void*      libxtst  = nullptr;
    void*      display  = nullptr;
    fn_x_open_display_t  XOpenDisplay  = nullptr;
    fn_x_close_display_t XCloseDisplay = nullptr;
    fn_x_flush_t         XFlush        = nullptr;
    fn_x_sync_t          XSync         = nullptr;
    fn_xtest_motion_t    XTestFakeMotion = nullptr;
    fn_xtest_button_t    XTestFakeButton = nullptr;
    const char* failure_reason = nullptr;  // static string, no ownership
};

XInjector& injector() {
    static XInjector inj;
    return inj;
}

// Returns true and a live display on success; sets failure_reason and returns
// false on any failure. Idempotent: subsequent calls return the cached state.
bool ensure_x_ready(XInjector& inj) {
    std::lock_guard<std::mutex> lock(inj.mu);
    if (inj.resolve_attempted) {
        return inj.display != nullptr;
    }
    inj.resolve_attempted = true;

    inj.libx11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!inj.libx11) {
        inj.failure_reason = "libx11_dlopen_failed";
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] X11 inject: dlopen libX11.so.6 failed: %s\n", dlerror());
        return false;
    }
    inj.libxtst = dlopen("libXtst.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!inj.libxtst) {
        inj.failure_reason = "libxtst_dlopen_failed";
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] X11 inject: dlopen libXtst.so.6 failed: %s\n", dlerror());
        return false;
    }

    inj.XOpenDisplay  = reinterpret_cast<fn_x_open_display_t>(dlsym(inj.libx11,  "XOpenDisplay"));
    inj.XCloseDisplay = reinterpret_cast<fn_x_close_display_t>(dlsym(inj.libx11, "XCloseDisplay"));
    inj.XFlush        = reinterpret_cast<fn_x_flush_t>(dlsym(inj.libx11,         "XFlush"));
    inj.XSync         = reinterpret_cast<fn_x_sync_t>(dlsym(inj.libx11,          "XSync"));
    inj.XTestFakeMotion = reinterpret_cast<fn_xtest_motion_t>(dlsym(inj.libxtst, "XTestFakeMotionEvent"));
    inj.XTestFakeButton = reinterpret_cast<fn_xtest_button_t>(dlsym(inj.libxtst, "XTestFakeButtonEvent"));

    if (!inj.XOpenDisplay || !inj.XCloseDisplay || !inj.XFlush ||
        !inj.XTestFakeMotion || !inj.XTestFakeButton) {
        inj.failure_reason = "xtest_symbols_unresolved";
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] X11 inject: symbol resolution failed "
            "(XOpen=%p XClose=%p XFlush=%p XTestMotion=%p XTestButton=%p)\n",
            (void*)inj.XOpenDisplay, (void*)inj.XCloseDisplay,
            (void*)inj.XFlush, (void*)inj.XTestFakeMotion,
            (void*)inj.XTestFakeButton);
        return false;
    }

    // XOpenDisplay(NULL) uses $DISPLAY. Inside the pressure-vessel container
    // CK3 already has a working DISPLAY (Xwayland); a parallel connection is
    // fine — Xlib opens an independent socket per Display*.
    inj.display = inj.XOpenDisplay(nullptr);
    if (!inj.display) {
        inj.failure_reason = "xopendisplay_failed";
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] X11 inject: XOpenDisplay(NULL) returned nil "
            "(DISPLAY=%s)\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return false;
    }

    if (g_logfile) fprintf(g_logfile,
        "[CK3-Bridge] X11 inject ready: display=%p DISPLAY=%s\n",
        inj.display, getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
    return true;
}

// Send one synthetic motion + press + release at (x,y). Returns true if all
// three XTest calls succeeded. Caller must hold inj.mu and have called
// ensure_x_ready() first.
bool xtest_click_locked(XInjector& inj, int x, int y, unsigned int button,
                        int* motion_ok_out, int* down_ok_out, int* up_ok_out) {
    int motion_ok = inj.XTestFakeMotion(inj.display, /*screen=*/-1, x, y, /*delay=*/0);
    int down_ok   = inj.XTestFakeButton(inj.display, button, /*is_press=*/1, /*delay=*/0);
    inj.XFlush(inj.display);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    int up_ok     = inj.XTestFakeButton(inj.display, button, /*is_press=*/0, /*delay=*/0);
    inj.XFlush(inj.display);
    if (motion_ok_out) *motion_ok_out = motion_ok;
    if (down_ok_out)   *down_ok_out   = down_ok;
    if (up_ok_out)     *up_ok_out     = up_ok;
    return motion_ok && down_ok && up_ok;
}

}  // namespace

// =============================================================================
// uinput Mouse Injection Fallback (ck-dq7ym.4)
//
// When the X11/XTest path above is unavailable — typically because the bridge
// process inherited a context without DISPLAY/XAUTHORITY or the X server is
// otherwise unreachable — `select_observe_default` falls back to /dev/uinput.
// The CK3 process runs as `gaming`, which has rw on /dev/uinput via the udev
// rule in SETUP.md, so this path does not require an X session at all.
//
// We create a synthetic USB-shaped mouse, park the cursor at the screen
// origin with a huge negative relative motion, move to the target (x, y), and
// emit BTN_LEFT down/up. The compositor takes ~1s to bind a new uinput
// device the first time, so we cache the fd across calls.
//
// This is a C++ port of the Path-A POC in
// `scripts/gaming_auto_observe.py` / `re/findings/re_observer_mode_autoclick.md`,
// trimmed to mouse-only (select_observe_default clicks at known button
// coordinates; keyboard fallbacks aren't useful for the multiplayer-lobby
// Random Character button).
// =============================================================================

namespace {

struct UInputInjector {
    std::mutex  mu;
    bool        resolve_attempted = false;
    int         fd                = -1;
    const char* failure_reason    = nullptr;  // static string, no ownership
};

UInputInjector& uinput_injector() {
    static UInputInjector inj;
    return inj;
}

// Open /dev/uinput, configure mouse capabilities, create the device, and wait
// for the compositor to bind it. Returns true on success; sets failure_reason
// and returns false on any failure. Idempotent: cached across calls.
bool ensure_uinput_ready(UInputInjector& inj) {
    std::lock_guard<std::mutex> lock(inj.mu);
    if (inj.resolve_attempted) {
        return inj.fd >= 0;
    }
    inj.resolve_attempted = true;

    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        inj.failure_reason = "uinput_open_failed";
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] uinput inject: open(/dev/uinput) failed: %s "
            "(gaming user needs `user:gaming:rw-` ACL on /dev/uinput — see SETUP.md)\n",
            strerror(errno));
        return false;
    }

    auto fail = [&](const char* reason, const char* op) -> bool {
        inj.failure_reason = reason;
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] uinput inject: %s failed: %s\n", op, strerror(errno));
        ::close(fd);
        return false;
    };

    // EV_KEY + BTN_LEFT for the click.
    if (ioctl(fd, UI_SET_EVBIT,  EV_KEY)   < 0) return fail("uinput_set_evbit_failed",  "UI_SET_EVBIT(EV_KEY)");
    if (ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0) return fail("uinput_set_keybit_failed", "UI_SET_KEYBIT(BTN_LEFT)");

    // EV_REL + REL_X/REL_Y for relative motion. We use relative-only because
    // ABS axis setup requires UI_ABS_SETUP + per-axis absinfo and reliable
    // knowledge of screen geometry; "park at origin, then move to target" is
    // simpler and works because the compositor clamps motion at screen edges.
    if (ioctl(fd, UI_SET_EVBIT,  EV_REL) < 0) return fail("uinput_set_evbit_failed", "UI_SET_EVBIT(EV_REL)");
    if (ioctl(fd, UI_SET_RELBIT, REL_X)  < 0) return fail("uinput_set_relbit_failed", "UI_SET_RELBIT(REL_X)");
    if (ioctl(fd, UI_SET_RELBIT, REL_Y)  < 0) return fail("uinput_set_relbit_failed", "UI_SET_RELBIT(REL_Y)");

    // EV_SYN is implicit but advertising it costs nothing and mirrors the
    // gaming_auto_observe.py reference implementation.
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) return fail("uinput_set_evbit_failed", "UI_SET_EVBIT(EV_SYN)");

    // BUS_USB shape: libinput's quirks paths apply to BUS_USB devices; a
    // BUS_VIRTUAL device is more likely to be silently filtered. VID/PID
    // copied from gaming_auto_observe.py — arbitrary but plausible.
    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1209;
    usetup.id.product = 0x0bdc;
    usetup.id.version = 0x0001;
    strncpy(usetup.name, "ck3-bridge-observe", sizeof(usetup.name) - 1);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) return fail("uinput_dev_setup_failed",  "UI_DEV_SETUP");
    if (ioctl(fd, UI_DEV_CREATE)         < 0) return fail("uinput_dev_create_failed", "UI_DEV_CREATE");

    // Wait for the kernel to publish the new input device under /sys, then
    // give libinput a beat to add it. UI_DEV_CREATE returns before the device
    // is fully wired up through the compositor; if we emit events too early
    // they're dropped before reaching CK3.
    char sysname[64] = {0};
    if (ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) >= 0 && sysname[0]) {
        char path[160];
        snprintf(path, sizeof(path), "/sys/devices/virtual/input/%s", sysname);
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (::access(path, F_OK) == 0) break;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));

    inj.fd = fd;
    if (g_logfile) fprintf(g_logfile,
        "[CK3-Bridge] uinput inject ready: fd=%d sysname=%s\n",
        fd, sysname[0] ? sysname : "(unknown)");
    return true;
}

// Emit one input_event on the uinput fd. Returns true on full-size write.
static bool uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    ssize_t n = ::write(fd, &ev, sizeof(ev));
    return n == sizeof(ev);
}

// Send a click at absolute screen coordinates (x, y). Park the cursor at the
// origin with a saturating negative motion, move to (x, y), then BTN_LEFT
// down/up. Caller must hold inj.mu and have called ensure_uinput_ready first.
bool uinput_click_at_absolute_locked(UInputInjector& inj, int x, int y) {
    // 1. Park cursor at (0, 0). Compositor clamps relative motion at screen
    //    edges, so a single -99999 step saturates regardless of resolution.
    if (!uinput_emit(inj.fd, EV_REL, REL_X, -99999)) return false;
    if (!uinput_emit(inj.fd, EV_REL, REL_Y, -99999)) return false;
    if (!uinput_emit(inj.fd, EV_SYN, SYN_REPORT, 0)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 2. Move to (x, y) relative to the origin we just parked at.
    if (x != 0 && !uinput_emit(inj.fd, EV_REL, REL_X, x)) return false;
    if (y != 0 && !uinput_emit(inj.fd, EV_REL, REL_Y, y)) return false;
    if (!uinput_emit(inj.fd, EV_SYN, SYN_REPORT, 0)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 3. Press / release BTN_LEFT.
    if (!uinput_emit(inj.fd, EV_KEY, BTN_LEFT, 1))   return false;
    if (!uinput_emit(inj.fd, EV_SYN, SYN_REPORT, 0)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (!uinput_emit(inj.fd, EV_KEY, BTN_LEFT, 0))   return false;
    if (!uinput_emit(inj.fd, EV_SYN, SYN_REPORT, 0)) return false;
    return true;
}

}  // namespace

bool do_inject_mouse_click(const char* params_json, std::string* response_out) {
    int x_raw = json_get_int(params_json, "x");
    int y_raw = json_get_int(params_json, "y");
    int button_raw = json_get_int(params_json, "button");
    if (button_raw <= 0) button_raw = 1;  // default left
    if (x_raw < 0 || y_raw < 0) {
        if (response_out) {
            *response_out =
                R"({"success":false,"action":"inject_mouse_click","reason":"x_y_required"})";
        }
        return false;
    }

    XInjector& inj = injector();
    if (!ensure_x_ready(inj)) {
        if (response_out) {
            ResponseBuilder r;
            r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action");  r.value_string("inject_mouse_click");
            r.key("reason");  r.value_string(
                inj.failure_reason ? inj.failure_reason : "x_resolve_failed");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // Serialize concurrent injects — Xlib display objects are not safe for
    // multi-threaded use without XInitThreads(), and we can't call that from
    // here (must be the first Xlib call in the process).
    std::lock_guard<std::mutex> lock(inj.mu);
    int motion_ok = 0, down_ok = 0, up_ok = 0;
    bool success = xtest_click_locked(inj, x_raw, y_raw,
        static_cast<unsigned int>(button_raw), &motion_ok, &down_ok, &up_ok);
    ADLOG("inject_mouse_click: x=%d y=%d button=%d motion=%d down=%d up=%d\n",
          x_raw, y_raw, button_raw, motion_ok, down_ok, up_ok);

    if (response_out) {
        ResponseBuilder r;
        r.begin_object();
        r.key("action"); r.value_string("inject_mouse_click");
        r.key("success"); r.value_bool(success);
        r.key("x"); r.value_int(x_raw);
        r.key("y"); r.value_int(y_raw);
        r.key("button"); r.value_int(button_raw);
        r.key("motion_ok"); r.value_bool(motion_ok != 0);
        r.key("down_ok"); r.value_bool(down_ok != 0);
        r.key("up_ok"); r.value_bool(up_ok != 0);
        r.end_object();
        *response_out = r.take();
    }
    return success;
}

// =============================================================================
// Auto-complete the character-select / multiplayer-lobby Observer flow
// (ck-li56t, ck-dq7ym.4). Default coords are the 1920x1080 estimates from
// re/findings/observer_button_coords.md (Random Character button at
// (540, 1015); Observe button at (540, 1060)). Override via params for
// non-default resolutions.
//
// mode="random" (default): click Random Character, picking a random
//   observer-backing character — this is the action that lets the engine
//   progress out of the character-unselected lobby tab.
// mode="observe": click only the Observe toggle (rarely useful on its own;
//   exposed for diagnosis).
// mode="both": click Observe first (set IsObserver=true), then Random
//   Character. Useful when the operator wants to force observer mode
//   before character pick.
//
// method="auto" (default): try XTest first, fall back to uinput on X failure.
// method="x":     XTest only — fail if X is unreachable.
// method="uinput": skip XTest, go straight to /dev/uinput (useful when CK3
//   was launched without DISPLAY/XAUTHORITY and the daemon is calling in
//   from a context that cannot reach an X server).
//
// The uinput path emits raw kernel-level mouse events that would land on
// whatever window is under the cursor, so we gate it on session_state ==
// DORMANT (the lobby/load screen). Callers can override with force=true.
// =============================================================================

bool do_select_observe_default(const char* params_json, std::string* response_out) {
    const char* mode_raw = json_get_string(params_json, "mode");
    std::string mode = (mode_raw && *mode_raw) ? mode_raw : "random";

    const char* method_raw = json_get_string(params_json, "method");
    std::string method = (method_raw && *method_raw) ? method_raw : "auto";
    if (method != "auto" && method != "x" && method != "uinput") {
        if (response_out) {
            *response_out =
                R"({"success":false,"action":"select_observe_default","reason":"bad_method"})";
        }
        return false;
    }

    int x_random  = json_get_int(params_json, "x_random");
    int y_random  = json_get_int(params_json, "y_random");
    int x_observe = json_get_int(params_json, "x_observe");
    int y_observe = json_get_int(params_json, "y_observe");
    if (x_random  <= 0) x_random  = 540;
    if (y_random  <= 0) y_random  = 1015;
    if (x_observe <= 0) x_observe = 540;
    if (y_observe <= 0) y_observe = 1060;

    int delay_ms = json_get_int(params_json, "inter_click_delay_ms");
    if (delay_ms <= 0) delay_ms = 150;

    bool do_observe = (mode == "observe" || mode == "both");
    bool do_random  = (mode == "random"  || mode == "both");
    if (!do_observe && !do_random) {
        if (response_out) {
            *response_out =
                R"({"success":false,"action":"select_observe_default","reason":"bad_mode"})";
        }
        return false;
    }

    // Try X first unless the caller explicitly selected uinput. Sticky-cached
    // inside ensure_x_ready, so a prior failure is fast to re-check.
    XInjector& xinj = injector();
    bool x_ready = (method != "uinput") && ensure_x_ready(xinj);
    const char* x_failure_reason =
        (method != "uinput" && !x_ready)
            ? (xinj.failure_reason ? xinj.failure_reason : "x_resolve_failed")
            : nullptr;

    // method=="x" + X unavailable -> bail without trying uinput.
    if (method == "x" && !x_ready) {
        if (response_out) {
            ResponseBuilder r;
            r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action");  r.value_string("select_observe_default");
            r.key("method");  r.value_string("x");
            r.key("reason");  r.value_string(x_failure_reason);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    bool observe_ok = true, random_ok = true;
    int obs_motion = 0, obs_down = 0, obs_up = 0;
    int rnd_motion = 0, rnd_down = 0, rnd_up = 0;
    std::string used_method;

    if (x_ready) {
        // Existing XTest path — preserve behaviour for the common case.
        std::lock_guard<std::mutex> lock(xinj.mu);
        if (do_observe) {
            observe_ok = xtest_click_locked(xinj, x_observe, y_observe, /*button=*/1,
                                            &obs_motion, &obs_down, &obs_up);
            ADLOG("select_observe_default observe-click (xtest): x=%d y=%d motion=%d down=%d up=%d\n",
                  x_observe, y_observe, obs_motion, obs_down, obs_up);
            if (do_random) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        if (do_random) {
            random_ok = xtest_click_locked(xinj, x_random, y_random, /*button=*/1,
                                           &rnd_motion, &rnd_down, &rnd_up);
            ADLOG("select_observe_default random-click (xtest): x=%d y=%d motion=%d down=%d up=%d\n",
                  x_random, y_random, rnd_motion, rnd_down, rnd_up);
        }
        used_method = "xtest";
    } else {
        // uinput fallback. Gate on session_state==DORMANT so a stray call in
        // ACTIVE doesn't land a kernel-level click on whatever window is
        // under the cursor. force=true skips the gate for diagnostic use.
        bool force = json_get_bool(params_json, "force", false);
        SessionState sstate = get_session_state();
        if (!force && sstate != SessionState::DORMANT) {
            if (response_out) {
                ResponseBuilder r;
                r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action");  r.value_string("select_observe_default");
                r.key("method");  r.value_string("uinput");
                r.key("reason");  r.value_string("not_in_lobby_state");
                r.key("session_state"); r.value_string(session_state_string());
                if (x_failure_reason) {
                    r.key("x_failure_reason"); r.value_string(x_failure_reason);
                }
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }

        UInputInjector& uinj = uinput_injector();
        if (!ensure_uinput_ready(uinj)) {
            if (response_out) {
                ResponseBuilder r;
                r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action");  r.value_string("select_observe_default");
                r.key("method");  r.value_string("uinput");
                r.key("reason");  r.value_string(
                    uinj.failure_reason ? uinj.failure_reason : "uinput_resolve_failed");
                if (x_failure_reason) {
                    r.key("x_failure_reason"); r.value_string(x_failure_reason);
                }
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }

        std::lock_guard<std::mutex> lock(uinj.mu);
        if (do_observe) {
            observe_ok = uinput_click_at_absolute_locked(uinj, x_observe, y_observe);
            ADLOG("select_observe_default observe-click (uinput): x=%d y=%d ok=%d\n",
                  x_observe, y_observe, observe_ok ? 1 : 0);
            if (do_random) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        if (do_random) {
            random_ok = uinput_click_at_absolute_locked(uinj, x_random, y_random);
            ADLOG("select_observe_default random-click (uinput): x=%d y=%d ok=%d\n",
                  x_random, y_random, random_ok ? 1 : 0);
        }
        used_method = "uinput";
    }

    bool success = observe_ok && random_ok;
    if (response_out) {
        ResponseBuilder r;
        r.begin_object();
        r.key("action");  r.value_string("select_observe_default");
        r.key("success"); r.value_bool(success);
        r.key("mode");    r.value_string(mode.c_str());
        r.key("method");  r.value_string(used_method.c_str());
        if (x_failure_reason && used_method != "xtest") {
            r.key("x_failure_reason"); r.value_string(x_failure_reason);
        }
        if (do_observe) {
            r.key("observe_clicked"); r.value_bool(observe_ok);
            r.key("x_observe"); r.value_int(x_observe);
            r.key("y_observe"); r.value_int(y_observe);
        }
        if (do_random) {
            r.key("random_clicked"); r.value_bool(random_ok);
            r.key("x_random"); r.value_int(x_random);
            r.key("y_random"); r.value_int(y_random);
        }
        r.end_object();
        *response_out = r.take();
    }
    return success;
}
#endif  // CK3_BRIDGE_DEBUG_VERBS (rc2 B2)

// =============================================================================
// AI Subsystem Toggle
// =============================================================================

bool do_toggle_ai_subsystem(const char* params_json) {
    const char* subsystem = json_get_string(params_json, "subsystem");
    if (!subsystem) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] toggle_ai_subsystem: missing subsystem name\n");
        return false;
    }

    bool enabled = json_get_bool(params_json, "enabled", true);

    for (size_t i = 0; i < g_subsystem_toggle_count; i++) {
        if (strcmp(g_subsystem_toggles[i].name, subsystem) == 0) {
            auto* flag = reinterpret_cast<uint8_t*>(g_base_addr + g_subsystem_toggles[i].offset);
            *flag = enabled ? 1 : 0;
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] AI subsystem '%s' %s\n",
                    subsystem, enabled ? "enabled" : "disabled");
            return true;
        }
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] toggle_ai_subsystem: unknown subsystem '%s'\n", subsystem);
    return false;
}

// =============================================================================
// Set Character AI
// =============================================================================

bool do_set_character_ai(const char* params_json) {
    // ── ck-vf92: `< 0` ACCEPTS ZERO, AND ZERO IS NOT A CHARACTER ────────────
    // This was the root of ck-bl15's four faults. All four calls were
    // {"character_id":0,"enabled":true} (ck-zgh4, from armb1's SEALED bundle):
    // 0 is not < 0, so this guard passed it, lookup_character(0) returned
    // something neither null nor the sentinel, and the AI_CHAR_TOGGLE store
    // faulted on the controller derived from it.
    //
    // ELEVEN sites in this file take a character_id from json_get_int. TEN
    // reject zero — eight `<= 0`, and `> 0` at do_send_decision (ck-23hv) — so
    // "0 is not a character" is the convention of every other call site, and
    // this one was the sole outlier.
    //
    // ABSENCE WAS ALREADY HANDLED, contrary to the first diagnosis, and this is
    // why the fix is `<= 0` and NOT a json_has_key guard. MEASURED against the
    // real helper: json_get_int returns -1 — not 0 — for an absent key, for a
    // null value, and for malformed JSON, all of which `< 0` already refused.
    // Only an explicit 0 (as an int, or as the string "0") ever slipped past.
    // A presence guard here would be a check that cannot fail for the reason it
    // names. json_has_key is used below ONLY to make the REASON accurate, never
    // to decide the verdict.
    const bool cid_present = json_has_key(params_json, "character_id");
    int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) {
        bridge::record_refusal(bridge::RefusalReason::kCharacterIdInvalid);
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] set_character_ai: REFUSING — character_id %s "
            "(parsed %d); 0 is not a character and would reach the "
            "AI_CHAR_TOGGLE store (ck-vf92)\n",
            cid_present ? "present but not a valid character id" : "absent",
            cid);
        return false;
    }

    bool enabled = json_get_bool(params_json, "enabled", true);

    void* character = lookup_character(static_cast<uint32_t>(cid));
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_character_ai: character %d not found\n", cid);
        return false;
    }

    void* ctrl = g_get_ai_ctrl(character);
    // ── ck-bl15: `if (ctrl)` IS A NULL TEST, NOT A VALIDITY TEST ────────────
    // AI_CHAR_TOGGLE is five bytes and its whole body is `*(p+0x28) = v` — a
    // STORE. So a non-null-but-invalid controller is not a bad read, it is a
    // WRITE to unmapped memory, and the null check above cannot see it.
    //
    // MEASURED, arm B on deploy-20: four byte-identical recovered faults,
    // thread=ipc_reader, rip_off 0x4d1f190 == ck3::funcs::AI_CHAR_TOGGLE, with
    // fault_addr - rdi == 0x28 — exactly the documented store, on a controller
    // pointer that was NOT MAPPED. ck-duv5 gave the thread and instruction,
    // ck-65lv's frame word named this call site, and ck-2ptx's range check is
    // what revealed the return address lay in the BRIDGE rather than in CK3.
    //
    // Same defect as check_delayed_unpause (ck-cg3b): `if (idler)` guarding a
    // raw engine deref. A pointer the engine handed back is not thereby a
    // pointer the engine can survive being handed back.
    //
    // PROBED WITH safe_read RATHER THAN is_mapped: process_vm_readv answers
    // "can this byte be touched" without a fault and without a /proc parse, and
    // reading the very field the callee will WRITE is the closest available
    // proxy for whether the write will land. is_mapped would answer about the
    // page, which a freed-and-unmapped object also fails but a reused one
    // passes — this is not a liveness oracle and does not claim to be.
    uint8_t probe = 0;
    const bool ctrl_writable_field_readable =
        ctrl && safe_read_bytes(reinterpret_cast<uintptr_t>(ctrl)
                                    + ck3::ai_ctrl::AI_ENABLED,
                                &probe, 1);
    if (ctrl && !ctrl_writable_field_readable) {
        bridge::record_refusal(bridge::RefusalReason::kAiControllerUnmapped);
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] set_character_ai: REFUSING — character %d's AI "
            "controller %p is non-null but its AI_ENABLED field is unreadable, "
            "so AI_CHAR_TOGGLE's store would fault (ck-bl15)\n", cid, ctrl);
        return false;
    }
    if (ctrl_writable_field_readable) {
        g_ai_char_toggle(ctrl, enabled ? 1 : 0);
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Character %d AI %s\n", cid, enabled ? "enabled" : "disabled");
        return true;
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_character_ai: character %d has no AI controller\n", cid);
    return false;
}

// =============================================================================
// Event Response
// =============================================================================

bool do_respond_event(const char* params_json, std::string* response_out) {
    const char* event_id = json_get_string(params_json, "event_id");
    int option = json_get_int(params_json, "option");
    // Also accept "option_index" (sent by MCP tools.py)
    if (option < 0) option = json_get_int(params_json, "option_index");
    int instance_id = json_get_int(params_json, "instance_id");

    // Fallback: if instance_id not provided, try event_id as integer (common agent pattern)
    if (instance_id < 0 && !event_id) {
        instance_id = json_get_int(params_json, "event_id");
    }

    if (option < 0) option = 0;

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] respond_event: event_id=%s instance_id=%d option=%d\n",
            event_id ? event_id : "(null)", instance_id, option);

    // Use instance_id for lookup if provided, otherwise try to find by event_id
    bool ok = false;
    if (instance_id > 0) {
        ok = select_event_option(static_cast<uint32_t>(instance_id), option);
    } else if (event_id) {
        // Legacy path: try to find event by event_id string match
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] respond_event: no instance_id, using legacy clear\n");
        extern bool handle_event_response(const char* event_id, int option);
        ok = handle_event_response(event_id, option);
    } else {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] respond_event: missing event_id and instance_id\n");
        if (response_out) *response_out = R"({"success":false,"action":"respond_event","reason":"missing event_id and instance_id"})";
        return false;
    }

    if (response_out) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            R"({"success":%s,"action":"respond_event","event_id":"%s","instance_id":%d,"option":%d})",
            ok ? "true" : "false",
            event_id ? event_id : "",
            instance_id,
            option);
        *response_out = buf;
    }
    return ok;
}

// =============================================================================
// change_realm_law: change a realm law by name
// Uses AddLawToCharacter(char, law, mode, 0). Static RE (ck-iglm) shows
// the binary's callers pass only two mode values:
//   mode=7 → fire the law's on_pass SCRIPT + the pass notification (used by the
//            default `add_realm_law` console handler + script-effect paths).
//   mode=4 → skip the on_pass script (`add_realm_law_skip_effects`, debug setup).
// CORRECTION (re_write_action_legitimacy_audit.md): mode 7 does NOT deduct the
// prestige/piety cost, does NOT set the ~10yr cooldown, and does NOT run the
// powerful-vassals vote or apply the crown-authority vassal-opinion penalty — it
// is the DEBUG-console path, not the player Change-Law interaction. (The earlier
// re_ck-iglm_add_law_modes.md "cost+cooldown+event" claim was refuted.) So this
// handler is uncosted: it is GATED below for the high-stakes laws (crown authority,
// succession) until the validated command path lands (audit §4). mode 4 stays
// available via an explicit `mode` param for harness setup.
// =============================================================================
// Legitimacy gate: crown-authority and succession laws are the high-stakes ones
// whose real cost/cooldown/vote/opinion consequences this uncosted path skips.
// Refuse them unless the caller explicitly opts into the uncosted write (harness).
static bool is_high_stakes_law(const char* law_name) {
    if (!law_name) return false;
    // crown_authority / succession have always routed here. tribal_authority is the
    // tribal-government analogue of crown_authority (group 1 vs group 0): raising it
    // costs prestige (increase_tribal_authority_prestige_cost) + a ~10yr cooldown and
    // applies a vassal-opinion penalty — exactly the kind of change that MUST go
    // through the engine's costed CAddLawCommand pipeline, never the uncosted setter.
    // Without this entry, act(type=law, law_name=tribal_authority_3) fell through to
    // the illegitimate AddLawToCharacter path (the "cannot enact" failures the agent
    // hit on the feudalize prerequisite — tribal_authority_3 gates
    // convert_whole_realm_to_feudalism_decision). (ck-feudalize-authority)
    return strstr(law_name, "crown_authority")  != nullptr ||
           strstr(law_name, "tribal_authority") != nullptr ||
           strstr(law_name, "succession")       != nullptr;
}
// ck-84: the UNCOSTED AddLawToCharacter path is illegitimate for high-stakes laws.
// do_change_realm_law now routes high-stakes laws through the COSTED player command
// (dispatch_costed_law_command), so the guard below only refuses the case where the
// caller has explicitly FORCED the uncosted setter (mode!=0 / force_uncosted) on a
// high-stakes law. The default high-stakes path is the costed command = LEGITIMATE.
static bool reject_forced_uncosted_high_stakes_law(const char* law_name, const char* params_json,
                                                   const char* action, std::string* response_out) {
    if (!is_high_stakes_law(law_name)) return false;
    // The costed command path is the default for high-stakes laws and is allowed.
    // Only refuse if the caller is FORCING the uncosted AddLawToCharacter setter and
    // has NOT opted into the harness/debug override.
    bool force_uncosted = json_get_bool(params_json, "force_uncosted", false);
    if (!force_uncosted) return false;                                 // costed path → allow
    if (json_get_bool(params_json, "allow_uncosted", false)) return false;  // harness override
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string(action);
        r.key("reason"); r.value_string("high_stakes_law_uncosted");
        r.key("law_name"); r.value_string(law_name);
        r.key("hint"); r.value_string("force_uncosted=true skips prestige cost, the ~10yr cooldown, the powerful-vassals vote, and the vassal-opinion penalty (illegitimate). Drop force_uncosted to use the costed command path, or pass allow_uncosted=true for harness setup.");
        r.end_object();
        *response_out = r.take();
    }
    return true;
}

// ── #109: SPECIFIC law-refusal reason harvesting ────────────────────────────────
// When the CAddLawCommand CanExecute pre-gate (FUN_06e30000) rejects a change, the
// coarse "not_allowed_or_unaffordable" hides WHICH sub-check failed. The CanExecute
// structure (decompiled, ck-84) is:
//   1. !IsRuler(char)                                  → not_a_ruler
//   2. law+0xb08 >= 10 || !((0x23d>>level)&1)          → not_eligible_yet (tier slot)
//   3. CharacterHasLaw(char,law)                       → already_active
//   4. FUN_06e2fd40(law,char,ctx): three scripted trigger blocks at law+0x230 /
//      +0x3f0 / +0x4d0 (can_pass/can_keep/can_have) — these carry the at_war /
//      cooldown / prerequisite conditions — then an affordability check
//      (FUN_06b8f850: prestige/gold/piety shortfall).
//
// We reproduce checks 1-3 with cheap predicate reads (IsRuler, the level byte,
// CharacterHasLaw) and harvest the specific loc-key from the failing trigger block
// in #4 using the proven loud-failure evaluator (EVAL_WITH_FAILURES → records the
// deepest failing leaf's localization key, same mechanism as the decision-eval's
// dv_extract_failure_key). The raw loc-key (e.g. "FAILED_AT_WAR",
// "LAW_CHANGE_ON_COOLDOWN") is surfaced verbatim in failures[]; we also map it to a
// coarse machine reason where the key is recognized.
//
// Crash-safety: this is a READ-ONLY diagnostic. Each game-fn call is wrapped in the
// watchdog + sigsetjmp protected region (caller arms the fake-RNG scope). On ANY
// fault we leave the reason empty and the caller falls back to the coarse refusal —
// so this can only ADD information, never regress the existing safe refusal path.

// Local std::string reader mirroring dv_read_std_string (libstdc++ dual-abi: ptr at
// +0x0, size at +0x8; SSO when ptr points just past the header). Bounded + safe_read.
static size_t law_read_std_string(uintptr_t str_addr, char* out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    uintptr_t data = safe_read<uintptr_t>(str_addr + 0x0);
    uint64_t  len  = safe_read<uint64_t>(str_addr + 0x8);
    if (len == 0 || len > 256) return 0;
    if (!data || !is_mapped(data)) return 0;
    size_t n = (len < out_size - 1) ? (size_t)len : out_size - 1;
    for (size_t i = 0; i < n; i++) {
        char ch = safe_read<char>(data + i);
        if (ch == '\0') { n = i; break; }
        out[i] = ch;
    }
    out[n] = '\0';
    return n;
}

// A loud-failure loc-key is an UPPERCASE snake key (e.g. "FAILED_AT_WAR"). Filter out
// formatting noise / lowercase tooltip fragments.
static bool law_is_loc_key(const char* s, size_t n) {
    if (n < 3 || n > 80) return false;
    bool seen_alpha = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') { seen_alpha = true; continue; }
        if (c == '_' || (c >= '0' && c <= '9')) continue;
        return false;  // lowercase / punctuation → not a loc-key
    }
    return seen_alpha;
}

// Harvest the requirement loc-key from a single law scripted-trigger block via the
// engine's loud evaluator (EVAL_WITH_FAILURES). Returns true + fills out_key on a
// recorded failure. Fully crash-net-wrapped; the caller must have armed the fake-RNG
// scope (trigger leaves advance RNG).
static bool law_extract_block_failure(uintptr_t trigger_block, uintptr_t scope_ctx,
                                      char* out_key, size_t out_size) {
    if (!out_key || out_size == 0) return false;
    out_key[0] = '\0';
    if (!trigger_block || !is_mapped(trigger_block)) return false;

    typedef void (*fn_eval_with_failures)(void**, uintptr_t, uintptr_t);
    typedef void (*fn_diag_dtor)(void*);
    auto fn_populate = reinterpret_cast<fn_eval_with_failures>(
        g_base_addr + ck3::trigger_eval_funcs::EVAL_WITH_FAILURES);
    auto fn_destroy = reinterpret_cast<fn_diag_dtor>(
        g_base_addr + ck3::trigger_eval_funcs::DIAG_OBJECT_DTOR);
    if (!is_mapped(reinterpret_cast<uintptr_t>(fn_populate)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(fn_destroy))) return false;

    void* record = nullptr;
    char buf1[96] = {}; char buf2[96] = {};
    size_t n1 = 0, n2 = 0;

    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) { g_in_protected_region = false; watchdog_disarm(); return false; }
    try {
        fn_populate(&record, trigger_block, scope_ctx);
        if (record && is_mapped(reinterpret_cast<uintptr_t>(record))) {
            uintptr_t rec = reinterpret_cast<uintptr_t>(record);
            n1 = law_read_std_string(rec + ck3::diag_record::STR1_DATA, buf1, sizeof(buf1));
            n2 = law_read_std_string(rec + ck3::diag_record::STR2_DATA, buf2, sizeof(buf2));
        }
    } catch (...) { n1 = n2 = 0; }
    g_in_protected_region = false;
    watchdog_disarm();

    if (record) {
        watchdog_arm(3);
        g_in_protected_region = true;
        sig = sigsetjmp(g_recovery_point, 1);
        if (sig == 0) { try { fn_destroy(record); operator delete(record); } catch (...) {} }
        g_in_protected_region = false;
        watchdog_disarm();
    }

    const char* pick = nullptr; size_t pick_n = 0;
    if (law_is_loc_key(buf1, n1)) { pick = buf1; pick_n = n1; }
    else if (law_is_loc_key(buf2, n2)) { pick = buf2; pick_n = n2; }
    if (!pick) return false;
    size_t copy_n = (pick_n < out_size - 1) ? pick_n : out_size - 1;
    memcpy(out_key, pick, copy_n);
    out_key[copy_n] = '\0';
    return true;
}

// Map a raw loud-failure loc-key (uppercase) to a coarse machine reason for the
// `reason` field. Unknown keys fall through to "not_eligible_yet" (the key itself is
// still surfaced verbatim in failures[]). Substring-matched (keys are localization
// tokens that vary by version/mod).
static const char* law_reason_from_key(const char* key) {
    if (!key || !key[0]) return nullptr;
    auto has = [&](const char* sub) { return strstr(key, sub) != nullptr; };
    if (has("WAR"))                                  return "at_war";
    if (has("COOLDOWN") || has("RECENTLY") || has("WAIT")) return "cooldown_active";
    if (has("PRESTIGE"))                             return "insufficient_prestige";
    if (has("PIETY"))                                return "insufficient_piety";
    if (has("GOLD"))                                 return "insufficient_gold";
    if (has("PREREQUISITE") || has("REQUIRE") || has("PREVIOUS") || has("LEVEL"))
                                                     return "missing_prerequisite";
    return "not_eligible_yet";
}

// Determine the SPECIFIC reason a law change was rejected by the CanExecute gate.
// Fills *out_reason (coarse machine reason) and up to two raw loc-keys into the
// caller-provided fail_keys[2] buffers. Returns true if a specific reason was found
// (otherwise the caller keeps the coarse "not_allowed_or_unaffordable"). The caller
// must hold the armed fake-RNG scope. char_scope must be a BUILD_SCOPES char scope.
struct LawRefusalDetail {
    const char* reason;          // coarse machine reason, or nullptr
    char key_pass[96];           // raw loc-key from can_pass block
    char key_keep[96];           // raw loc-key from can_keep / can_have block
    bool not_ruler;
    bool already_active;
    bool tier_blocked;
};

static void diagnose_law_refusal(void* actor, uintptr_t law_obj, uintptr_t char_scope,
                                 LawRefusalDetail& d) {
    d.reason = nullptr;
    d.key_pass[0] = d.key_keep[0] = '\0';
    d.not_ruler = d.already_active = d.tier_blocked = false;
    if (!law_obj || !is_mapped(law_obj)) return;

    // 1. IsRuler(char) — cheap predicate (FUN_0674f390, char+0x1b8!=0 read directly,
    //    no game-fn call needed).
    {
        uintptr_t c = reinterpret_cast<uintptr_t>(actor);
        uintptr_t title_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
        if (!title_data || !is_mapped(title_data)) {
            d.not_ruler = true;
            d.reason = "not_a_ruler";
            return;
        }
    }

    // 2. Tier slot: law+0xb08 must be < 10 and bit set in 0x23d. (Mirrors CANEXEC.)
    {
        uint8_t level = safe_read<uint8_t>(law_obj + 0xb08);
        bool slot_ok = (level < 10) && (((0x23dUL >> (level & 0x3f)) & 1) != 0);
        if (!slot_ok) {
            d.tier_blocked = true;
            d.reason = "not_eligible_yet";
            // keep scanning trigger blocks below for a more specific key
        }
    }

    // 3. CharacterHasLaw(char,law) — already active? (FUN_068839a0). Wrapped in the
    //    crash net since it is a game-fn call. CHARACTER_HAS_LAW_ENTRY corrects the
    //    legacy CHARACTER_HAS_LAW offset (which lands mid-function).
    {
        auto has_law = reinterpret_cast<char(*)(void*, void*)>(
            g_base_addr + ck3::funcs::CHARACTER_HAS_LAW_ENTRY);
        if (is_mapped(reinterpret_cast<uintptr_t>(has_law))) {
            volatile char active = 0;
            watchdog_arm(2);
            g_in_protected_region = true;
            int sig = sigsetjmp(g_recovery_point, 1);
            if (sig == 0) {
                try { active = has_law(actor, reinterpret_cast<void*>(law_obj)); }
                catch (...) { active = 0; }
            }
            g_in_protected_region = false;
            watchdog_disarm();
            if (active) {
                d.already_active = true;
                d.reason = "already_active";
                return;
            }
        }
    }

    // 4. Harvest the specific failing loc-key from the law's scripted trigger blocks
    //    (can_pass @+0x230, can_keep/can_have @+0x3f0 and +0x4d0). The first block to
    //    yield a loud-failure key wins for the coarse reason; both are surfaced raw.
    // #109 CRASH FIX: the scripted-trigger-block harvest (EVAL_WITH_FAILURES over the
    // law's can_pass/can_keep/can_have blocks) HUNG the game thread on a live at-war
    // law-refusal (SIGALRM masked → uncatchable → hard crash). It was never live-tested
    // in the original #109 commit. GATED OFF by default; the cheap sub-checks above
    // (not_a_ruler / not_eligible_yet / already_active) plus the at-war pre-check in
    // do_change_realm_law provide the reason safely. Set the env opt-in to re-enable for
    // bounded RE of the hang.
    static const bool kLawHarvestEnabled = (getenv("CK3_LAW_TRIGGER_HARVEST") != nullptr);
    if (char_scope && kLawHarvestEnabled) {
        law_extract_block_failure(law_obj + 0x230, char_scope, d.key_pass, sizeof(d.key_pass));
        if (!law_extract_block_failure(law_obj + 0x3f0, char_scope, d.key_keep, sizeof(d.key_keep)))
            law_extract_block_failure(law_obj + 0x4d0, char_scope, d.key_keep, sizeof(d.key_keep));

        const char* mapped = nullptr;
        if (d.key_pass[0]) mapped = law_reason_from_key(d.key_pass);
        if (!mapped && d.key_keep[0]) mapped = law_reason_from_key(d.key_keep);
        if (mapped) d.reason = mapped;  // a trigger key beats the generic tier reason
    }
}

// ck-84: dispatch the COSTED player Change-Realm-Law command (CAddLawCommand) for an
// EXPLICIT actor. Mirrors do_add_construction's dispatch template: build the typed
// CJominiCommandBase command via its ctor, stamp the actor char id (+0x20) and law
// object (+0x28), then prepare (vtable[9]) + GAME_COMMAND_DISPATCH(mgr,&cmd,0xe,0).
// The engine's command pipeline runs CanExecute (cost/cooldown/vote/eligibility) then
// Execute, applying the prestige cost + ~10yr cooldown + powerful-vassals vote +
// vassal-opinion penalty natively.
//
// ── ck-84 ROOT-CAUSE FIX (2026-06-21) ────────────────────────────────────────────
// The previous live attempt HARD-CRASHED because the recorded ADD_LAW_COMMAND_* header
// offsets were 0x100000 TOO HIGH (image VAs stored as header offsets). At runtime
// `g_base_addr + 0x067cd2f0` landed inside FUN_068cd200 — a generic hashmap-INSERT
// routine — so calling it with command-shaped args smashed the stack (uncatchable by
// the SIGSEGV net). The real CAddLawCommand ctor is FUN_067cd2f0 (image), header
// 0x066cd2f0. The offsets are now corrected (verified: func_at is_entry:true +
// decompile + live vtable read from the core dump). With the correct ctor this path is
// structurally identical to the proven-working do_add_construction dispatch.
//
// SAFETY PRE-GATE: before prepare/dispatch we run the command's own CanExecute (vt[7]
// wrapper FUN_067cd410 → CANEXEC FUN_06e30000) SYNCHRONOUSLY under the crash net with a
// null ctx. CanExecute returns nonzero iff the change is eligible+affordable (IsRuler +
// law-group-level bitmask + !CharacterHasLaw + scripted cost eval). If it returns 0 we
// REFUSE cleanly and NEVER reach Execute — so an ineligible/unaffordable change cannot
// trip a fault in the apply path. out_eligible reports the gate result to the caller.
//
// Crash-safety: armed RNG scope (CanExecute's cost eval + Execute's on_pass effect may
// advance RNG → same RandomScopeManager null-deref class as war/lifestyle) + watchdog +
// sigsetjmp protected region, with RNG restore + region/watchdog teardown on EVERY path.
static bool dispatch_costed_law_command(void* actor, uintptr_t law_obj,
                                        bool* out_crashed, bool* out_eligible) {
    if (out_crashed)  *out_crashed = false;
    if (out_eligible) *out_eligible = true;
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + 0x18);  // FULL engine id

    auto ctor = reinterpret_cast<void(*)(void*)>(g_base_addr + ck3::funcs::ADD_LAW_COMMAND_CTOR);
    auto dispatch = reinterpret_cast<void(*)(void*, void*, int, int)>(
        g_base_addr + ck3::funcs::GAME_COMMAND_DISPATCH);
    auto base_dtor = reinterpret_cast<void(*)(void*)>(g_base_addr + 0x0755b650);  // FUN_0765b650

    uint8_t cmd[0x48]; memset(cmd, 0, sizeof(cmd));
    void* out_a = nullptr;
    void* out_b = nullptr;

    // Arm RNG scope BEFORE sigsetjmp so the crash branch can restore it.
    uint64_t saved_rng = 0;
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false; watchdog_disarm();
        if (out_crashed) *out_crashed = true;
        ADLOG("change_realm_law(costed): crashed during command dispatch\n");
        return false;
    }

    bool dispatched = false;
    try {
        // Build the command (sets both vtables + char=-1/law=sentinel), then stamp
        // the explicit actor id and the chosen law object.
        ctor(cmd);
        *reinterpret_cast<uint32_t*>(cmd + 0x20) = actor_id;     // CAddLawCommand char id
        *reinterpret_cast<uintptr_t*>(cmd + 0x28) = law_obj;     // CAddLawCommand CLaw*

        void** vt = *reinterpret_cast<void***>(cmd);             // cmd[0] = vtable

        // ── SAFETY PRE-GATE: CanExecute (vt[7], vtable+0x38) with null ctx ──────────
        // Returns nonzero iff eligible+affordable. On 0 we refuse without dispatching,
        // so the (potentially crash-prone) Execute path is never reached for an
        // ineligible change.
        auto can_exec = reinterpret_cast<uint64_t(*)(void*, void*)>(vt[7]);  // FUN_067cd410(cmd,ctx)
        uint64_t eligible = can_exec(cmd, nullptr);
        if (!eligible) {
            if (out_eligible) *out_eligible = false;
            base_dtor(cmd);
            if (rng_armed) restore_fake_random_scope(saved_rng);
            g_in_protected_region = false; watchdog_disarm();
            ADLOG("change_realm_law(costed): CanExecute rejected (ineligible/unaffordable)\n");
            return false;
        }

        auto prepare = reinterpret_cast<void*(*)(void*, void*)>(vt[9]);  // vtable+0x48
        prepare(&out_a, cmd);
        out_b = out_a; out_a = nullptr;
        dispatch(reinterpret_cast<void*>(g_base_addr + ck3::globals::COMMAND_DISPATCH_MGR),
                 &out_b, 0xe, 0);
        // Cleanup: virtual dtor (vtable+8) on out holders, then base dtor on cmd.
        if (out_b) { auto d = reinterpret_cast<void(*)(void*)>((*reinterpret_cast<void***>(out_b))[1]); d(out_b); }
        if (out_a) { auto d = reinterpret_cast<void(*)(void*)>((*reinterpret_cast<void***>(out_a))[1]); d(out_a); }
        base_dtor(cmd);
        dispatched = true;
    } catch (...) {
        ADLOG("change_realm_law(costed): exception during command dispatch\n");
        dispatched = false;
    }
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();
    return dispatched;
}

// Read a character's prestige (core_data+0x130, int64 fixed-point /100000). 0 if unlanded.
static int64_t read_char_prestige_fp(void* actor) {
    if (!actor) return 0;
    uintptr_t core = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::REALM_DATA);
    if (!core || !is_mapped(core)) return 0;
    return safe_read<int64_t>(core + 0x130);
}

bool do_change_realm_law(const char* params_json, std::string* response_out) {
    const char* law_name = json_get_string(params_json, "law_name");
    if (!law_name || !law_name[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"change_realm_law","reason":"missing_law_name"})";
        return false;
    }
    // ── ck-84: high-stakes (crown_authority_*, *succession*) law routing ────────
    // The COSTED player command path (CAddLawCommand → GAME_COMMAND_DISPATCH) is the
    // LEGITIMATE applier: the engine pipeline runs CanExecute (eligibility + scripted
    // affordability) then Execute, applying the prestige cost + ~10yr cooldown +
    // powerful-vassals vote + vassal-opinion penalty natively.
    //
    // The earlier hard-crash was a 0x100000 offset error (the ctor offset resolved to
    // a hashmap-insert routine, not the command ctor). Fixed 2026-06-21: the corrected
    // ADD_LAW_COMMAND_* offsets make this path structurally identical to the proven
    // do_add_construction dispatch, and a CanExecute pre-gate refuses ineligible/
    // unaffordable changes before they ever reach Execute. So high-stakes laws now
    // DEFAULT to the costed path (legitimate). Flags:
    //   - default (high-stakes) → costed command (CanExecute-gated, costs applied)
    //   - force_uncosted=true (+ allow_uncosted=true, harness) → legacy uncosted setter
    //   - disable_costed_dispatch=true → opt OUT of the costed path (honest refusal,
    //     for rollback if the costed path is found unsafe on a given save)
    bool force_uncosted   = json_get_bool(params_json, "force_uncosted", false);
    bool disable_costed   = json_get_bool(params_json, "disable_costed_dispatch", false);
    bool use_costed = is_high_stakes_law(law_name) && !force_uncosted && !disable_costed;

    if (is_high_stakes_law(law_name) && !use_costed) {
        if (force_uncosted) {
            // explicit uncosted setter for high-stakes — harness only
            if (reject_forced_uncosted_high_stakes_law(law_name, params_json, "change_realm_law", response_out))
                return false;
        } else {
            // disable_costed_dispatch=true → honest refusal (rollback escape hatch)
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("change_realm_law");
                r.key("reason"); r.value_string("high_stakes_law_costed_path_disabled");
                r.key("law_name"); r.value_string(law_name);
                r.key("hint"); r.value_string("disable_costed_dispatch=true opts out of the legitimate costed command path; drop it to use the costed CAddLawCommand (cost+cooldown+vote+opinion), or pass force_uncosted=true + allow_uncosted=true for harness uncosted setup.");
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
    }

    // Optional mode override (ck-iglm). Default 7 = full effects; 4 is the
    // only other value observed in the binary (debug/skip-effects path).
    int mode = json_get_int(params_json, "mode");
    if (mode < 0) mode = 7;
    if (mode != 4 && mode != 7) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("change_realm_law");
            r.key("reason"); r.value_string("invalid_mode");
            r.key("hint"); r.value_string("mode must be 7 (default, full effects) or 4 (skip effects)");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "change_realm_law", response_out)) return false;
    if (!actor) {
        if (response_out) *response_out = R"({"success":false,"action":"change_realm_law","reason":"no_actor"})";
        return false;
    }

    // Verify actor is a ruler
    uintptr_t c = reinterpret_cast<uintptr_t>(actor);
    uintptr_t title_data = read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (!title_data || !safe_ptr(title_data)) {
        if (response_out) *response_out = R"({"success":false,"action":"change_realm_law","reason":"not_a_ruler"})";
        return false;
    }

    // Look up law by name in the realm law DB
    uintptr_t db_ptr = read_offset<uintptr_t>(ck3::globals::REALM_LAW_DB);
    if (!db_ptr || !safe_ptr(db_ptr)) {
        if (response_out) *response_out = R"({"success":false,"action":"change_realm_law","reason":"realm_law_db_not_found"})";
        return false;
    }

    // Walk the law pointer array to find the law by name.
    // NOTE: LAW_LOOKUP_BY_NAME (0x06c332c0) is a bad offset — it points into a
    // destructor (FUN_06d331b0), not a lookup function. Calling it with
    // (db, name, len) executes destructor code and SIGSEGVs. Use direct array
    // iteration instead, which is safe and avoids calling any game functions for
    // the lookup step.
    uintptr_t law_array = read<uintptr_t>(db_ptr + ck3::realm_law_db::LAW_PTR_ARRAY);
    uint32_t law_count = read<uint32_t>(db_ptr + ck3::realm_law_db::LAW_COUNT);
    if (!law_array || !safe_ptr(law_array) || law_count == 0 || law_count > 1000) {
        if (response_out) *response_out = R"({"success":false,"action":"change_realm_law","reason":"realm_law_db_corrupt"})";
        return false;
    }

    uintptr_t law_obj_addr = 0;
    size_t name_len = strlen(law_name);
    for (uint32_t i = 0; i < law_count; i++) {
        uintptr_t law_ptr = read<uintptr_t>(law_array + i * 8);
        if (!law_ptr || !safe_ptr(law_ptr)) continue;
        std::string entry_name = ad_read_std_string(law_ptr + ck3::realm_law_entry::LAW_NAME);
        if (entry_name.size() == name_len && entry_name == law_name) {
            law_obj_addr = law_ptr;
            break;
        }
    }

    if (!law_obj_addr) {
        ADLOG("change_realm_law: law '%s' not found in DB\n", law_name);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("change_realm_law");
            r.key("reason"); r.value_string("law_not_found");
            r.key("law_name"); r.value_string(law_name);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    void* law_obj = reinterpret_cast<void*>(law_obj_addr);

    // ── ck-84: COSTED player command path (legitimate) ──────────────────────────
    // For high-stakes laws (default) or any law with costed=true, dispatch the
    // engine's CAddLawCommand through the player command queue. The engine applies
    // the prestige cost + ~10yr cooldown + powerful-vassals vote + vassal-opinion
    // penalty. We read prestige before/after as a legitimacy witness.
    if (use_costed) {
        // #109 CRASH-SAFETY: a ruler AT WAR cannot legitimately change a high-stakes
        // realm law, and invoking the costed CanExecute / trigger-harvest in that state
        // has hung the game thread (scripted trigger eval runs with SIGALRM masked →
        // no watchdog can bound it → eventual hard crash). Detect at-war via a CHEAP
        // direct read (title_realm WAR_REF_ARRAY count, same as observation.cpp) and
        // refuse with the specific reason BEFORE any engine call. Covers the common
        // campaign case (aggressive conquerors are usually at war) crash-free.
        {
            uintptr_t ac = reinterpret_cast<uintptr_t>(actor);
            uintptr_t tr = safe_read<uintptr_t>(ac + ck3::character::TITLE_REALM_DATA);
            if (tr && is_mapped(tr)) {
                int32_t wc = safe_read<int32_t>(tr + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
                if (wc > 0 && wc < 1000) {
                    if (response_out) {
                        ResponseBuilder r; r.begin_object();
                        r.key("success"); r.value_bool(false);
                        r.key("action"); r.value_string("change_realm_law");
                        r.key("reason"); r.value_string("at_war");
                        r.key("law_name"); r.value_string(law_name);
                        r.key("active_wars"); r.value_int(wc);
                        r.key("hint"); r.value_string("realm laws (crown authority, succession) cannot be changed while at war — end/white-peace your wars first, then raise crown authority.");
                        r.end_object();
                        *response_out = r.take();
                    }
                    return false;
                }
            }
        }
        // ck-zgh4 scope latch, guarded HERE rather than inside
        // dispatch_costed_law_command: that helper reports through out-params
        // (out_crashed / out_eligible) and the caller turns them into specific
        // responses, so a refusal from inside would have to borrow one of those
        // meanings and would reach the seat as "crashed" or "not eligible" —
        // both false. The caller owns the response, so the caller refuses.
        if (bridge::scope_latched()) {
            bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
            if (response_out) *response_out = bridge::scope_latched_refusal_json("change_realm_law");
            return false;
        }
        int64_t prestige_before = read_char_prestige_fp(actor);
        bool crashed = false;
        bool eligible = true;
        bool dispatched = dispatch_costed_law_command(actor, law_obj_addr, &crashed, &eligible);
        int64_t prestige_after = read_char_prestige_fp(actor);
        if (crashed) {
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("change_realm_law");
                r.key("reason"); r.value_string("crashed_during_execution");
                r.key("law_name"); r.value_string(law_name);
                r.key("path"); r.value_string("costed_command");
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
        if (!eligible) {
            // CanExecute pre-gate refused: ineligible (not ruler / wrong CA tier /
            // already has law) or unaffordable (scripted cost). Clean refusal — Execute
            // was never reached, so no state changed and no crash risk.
            //
            // #109: harvest the SPECIFIC reason so the agent can plan around the gate.
            // diagnose_law_refusal reproduces the CanExecute sub-checks (read-only) and
            // pulls the failing trigger block's loud-failure loc-key. Fully crash-net-
            // wrapped; on any fault it leaves reason=nullptr and we keep the coarse key.
            LawRefusalDetail detail;
            // ck-zgh4 scope latch: this block is a READ-ONLY ENRICHMENT of a
            // refusal that has ALREADY been decided — the law change is not
            // happening either way, and `reason` below falls back to the
            // coarse key on its own. So when latched we skip it and make NO
            // engine call at all, rather than refusing: a latch refusal here
            // would REPLACE a correct, specific law-refusal reason with a
            // reason about the bridge, which is strictly worse for the seat
            // and buys no safety. Same shape as the 1989 ruling (skip the
            // optional step), and unlike 1989 nothing here mutates.
            if (bridge::scope_latched()) {
                bridge::g_law_diagnostic_skipped_latched.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                // Build a character scope for the trigger-block diagnostic and arm the
                // fake-RNG scope (trigger leaves advance RNG → null-deref otherwise).
                alignas(8) uint8_t scope_buf[64] = {};
                uintptr_t char_scope = 0;
                auto build_scopes = reinterpret_cast<void(*)(void*, void*)>(
                    g_base_addr + ck3::decision_funcs::BUILD_SCOPES);
                uint64_t saved_rng = 0;
                bool rng_armed = arm_fake_random_scope(&saved_rng);
                if (is_mapped(reinterpret_cast<uintptr_t>(build_scopes))) {
                    watchdog_arm(3);
                    g_in_protected_region = true;
                    if (sigsetjmp(g_recovery_point, 1) == 0) {
                        try { build_scopes(scope_buf, actor); char_scope = reinterpret_cast<uintptr_t>(scope_buf); }
                        catch (...) { char_scope = 0; }
                    } else { char_scope = 0; }
                    g_in_protected_region = false;
                    watchdog_disarm();
                }
                diagnose_law_refusal(actor, law_obj_addr, char_scope, detail);
                if (rng_armed) restore_fake_random_scope(saved_rng);
            }

            const char* reason = detail.reason ? detail.reason : "not_allowed_or_unaffordable";
            ADLOG("change_realm_law(costed): CanExecute refused law '%s' for char %u — reason=%s key_pass='%s' key_keep='%s'\n",
                  law_name, char_get_id(actor) & 0xFFFFFF, reason, detail.key_pass, detail.key_keep);
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("change_realm_law");
                r.key("reason"); r.value_string(reason);
                // failures[]: the raw engine loud-failure loc-keys (most specific signal).
                r.key("failures"); r.begin_array();
                if (detail.not_ruler)      r.value_string("NOT_A_RULER");
                if (detail.already_active) r.value_string("LAW_ALREADY_ACTIVE");
                if (detail.tier_blocked)   r.value_string("TIER_PREREQUISITE_NOT_MET");
                if (detail.key_pass[0])    r.value_string(detail.key_pass);
                if (detail.key_keep[0])    r.value_string(detail.key_keep);
                r.end_array();
                r.key("law_name"); r.value_string(law_name);
                r.key("path"); r.value_string("costed_command");
                r.key("character_id"); r.value_uint(char_get_id(actor) & 0xFFFFFF);
                r.key("prestige_fp"); r.value_int(prestige_before);
                r.key("reason_detail_confidence"); r.value_string("medium");  // #109: trigger-key harvest
                r.key("hint"); r.value_string("the engine's CanExecute gate rejected this change. `reason` is the specific blocker (at_war / cooldown_active / insufficient_prestige / missing_prerequisite / not_eligible_yet / already_active / not_a_ruler); `failures[]` carries the raw engine loc-keys. If reason is generic, the gate is affordability or an unmapped scripted prerequisite.");
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
        ADLOG("change_realm_law(costed): dispatched law '%s' for char %u (prestige %lld->%lld)\n",
              law_name, char_get_id(actor) & 0xFFFFFF,
              (long long)prestige_before, (long long)prestige_after);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(dispatched);
            r.key("action"); r.value_string("change_realm_law");
            r.key("law_name"); r.value_string(law_name);
            r.key("path"); r.value_string("costed_command");
            r.key("dispatched"); r.value_bool(dispatched);
            r.key("character_id"); r.value_uint(char_get_id(actor) & 0xFFFFFF);
            r.key("prestige_before_fp"); r.value_int(prestige_before);
            r.key("prestige_after_fp"); r.value_int(prestige_after);
            // The engine validates+applies on a tick; the change (and cost deduction)
            // may land on the next unpaused tick rather than synchronously. Poll
            // query_realm_laws / query_character to confirm. CanExecute silently drops
            // an ineligible change (no crash) — confirm by re-querying laws.
            r.key("note"); r.value_string("engine applies cost/cooldown/vote on a tick; poll query_realm_laws + prestige to confirm. ineligible changes are silently dropped by CanExecute.");
            r.end_object();
            *response_out = r.take();
        }
        return dispatched;
    }

    // ── Legacy UNCOSTED setter path (non-high-stakes laws, or force_uncosted) ────
    // Use AddLawToCharacter: fn(char, law, mode, extra)
    using fn_add_law = void(*)(void*, void*, int, int);
    auto add_law = reinterpret_cast<fn_add_law>(g_base_addr + ck3::funcs::ADD_REALM_LAW);

    // AddLawToCharacter with mode=7 fires the law-pass EFFECT/EVENT, which advances
    // RNG via random_in_scope → the RandomScopeManager null-deref crash (same as
    // war/council/lifestyle). Fabricate a fake RNG scope bound to this thread around
    // the call. Arm BEFORE sigsetjmp so the crash branch restores it — else a
    // SIGSEGV leaves the game thread wedged. (20260614: missing-RNG-scope was the
    // `crashed_during_execution` cause; ck-49 harden = restore-on-crash.)
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("change_realm_law");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        ADLOG("change_realm_law: crashed during execution for law '%s'\n", law_name);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("change_realm_law");
            r.key("reason"); r.value_string("crashed_during_execution");
            r.key("law_name"); r.value_string(law_name);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    bool success = false;
    try {
        // mode is 7 (default, full effects) or 4 (skip effects) — see ck-iglm.
        add_law(actor, law_obj, mode, 0);
        success = true;
        ADLOG("change_realm_law: applied law '%s' mode=%d to char %u\n",
              law_name, mode, char_get_id(actor) & 0xFFFFFF);
    } catch (...) {
        ADLOG("change_realm_law: exception during execution for law '%s'\n", law_name);
        success = false;
    }
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(success);
        r.key("action"); r.value_string("change_realm_law");
        r.key("law_name"); r.value_string(law_name);
        r.key("mode"); r.value_int(mode);
        if (success) {
            r.key("character_id"); r.value_uint(char_get_id(actor) & 0xFFFFFF);
        } else {
            r.key("reason"); r.value_string("execution_failed");
        }
        r.end_object();
        *response_out = r.take();
    }
    return success;
}

// =============================================================================
// Pre-Validate War — check PDXScript conditions before dispatch
// =============================================================================
// Pure memory reads + IS_IMPRISONED function call. No game thread needed.
// Checks conditions A1–A13 and R1–R4 from is_valid_showing_failures_only.
// Returns a JSON list of pass/fail results for each checkable condition.

// character_has_raised_armies: mirror of PDXScript `has_raised_armies`.
// Iterates the global army enumeration, filters by owner, returns true on
// the first owner_link whose troop_count > 0. Pattern matches do_query_armies.
// Out-params (optional, for debug detail): total army count owned, max troops.
static bool character_has_raised_armies(uint32_t actor_char_id,
                                        int* out_owned_armies,
                                        int* out_max_troops) {
    int owned = 0;
    int max_troops = 0;

    using fn_army_enum = uintptr_t(*)(uintptr_t);
    auto army_enum = reinterpret_cast<fn_army_enum>(
        g_base_addr + ck3::funcs::ARMY_ENUM_SUB);
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::ARMY_ENUM_ENTRY);
    if (!gs) {
        if (out_owned_armies) *out_owned_armies = 0;
        if (out_max_troops) *out_max_troops = 0;
        return false;
    }
    uintptr_t sub = army_enum(gs);
    if (!sub) {
        if (out_owned_armies) *out_owned_armies = 0;
        if (out_max_troops) *out_max_troops = 0;
        return false;
    }

    uintptr_t id_array = safe_read<uintptr_t>(sub + ck3::army_enum::ID_ARRAY_PTR);
    int32_t id_count = safe_read<int32_t>(sub + ck3::army_enum::ID_ARRAY_COUNT);
    if (!id_array || id_count <= 0 || id_count > 10000) {
        if (out_owned_armies) *out_owned_armies = 0;
        if (out_max_troops) *out_max_troops = 0;
        return false;
    }

    bool found_raised = false;
    for (int32_t i = 0; i < id_count; ++i) {
        uint32_t army_id = safe_read<uint32_t>(id_array + i * 4);
        if (army_id == 0 || army_id == 0xFFFFFFFF) continue;
        uintptr_t army_ptr = ad_db_lookup(ck3::globals::ARMY_DB, army_id);
        if (!army_ptr) continue;
        uint32_t link_id = safe_read<uint32_t>(army_ptr + ck3::army_obj::OWNER_LINK_ID);
        uintptr_t link_ptr = ad_db_lookup(ck3::globals::OWNER_LINK_DB, link_id);
        if (!link_ptr) continue;
        uint32_t owner_id = safe_read<uint32_t>(link_ptr + ck3::army_owner_link::OWNER_CHAR_ID);
        if ((owner_id & 0xFFFFFF) != (actor_char_id & 0xFFFFFF)) continue;

        owned++;
        // #112: a raised army's true strength is the SUM of current (alive) men over
        // its regiment list (CArmy+0x18 REG_VEC → ARMY_REGIMENT_DB → +0x38), NOT
        // owner_link+0x168 (which reads 0 for most armies and would make this
        // predicate wrongly report "no raised armies" for a ruler that has them).
        int32_t troop_count = 0;
        {
            uintptr_t reg_vec = safe_read<uintptr_t>(army_ptr + ck3::army_obj::REG_VEC);
            int32_t reg_count = safe_read<int32_t>(army_ptr + ck3::army_obj::REG_COUNT);
            if (reg_vec && reg_count > 0 && reg_count <= 5000) {
                for (int32_t j = 0; j < reg_count; ++j) {
                    uint32_t rid = safe_read<uint32_t>(reg_vec + j * 4);
                    if (rid == 0 || rid == 0xFFFFFFFF) continue;
                    uintptr_t reg = ad_db_lookup(ck3::globals::ARMY_REGIMENT_DB, rid);
                    if (!reg) continue;
                    if (safe_read<uint32_t>(reg + ::army_regiment::ID) != rid) continue;
                    troop_count += safe_read<int32_t>(reg + ::army_regiment::MEN_CURRENT);
                }
            }
        }
        if (troop_count > max_troops) max_troops = troop_count;
        if (troop_count > 0) {
            found_raised = true;
            // Keep iterating only if caller wants full totals; otherwise short-circuit.
            if (!out_owned_armies && !out_max_troops) break;
        }
    }

    if (out_owned_armies) *out_owned_armies = owned;
    if (out_max_troops) *out_max_troops = max_troops;
    return found_raised;
}

bool do_pre_validate_war(const char* params_json, std::string* response_out) {
    int target_id_raw = json_get_int(params_json, "target_id");

    // Resolve actor — 0 means controlled character
    void* actor = resolve_actor(params_json);
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"pre_validate_war","reason":"no_living_actor"})";
        return false;
    }

    if (target_id_raw < 0) {
        if (response_out) *response_out = R"({"success":false,"action":"pre_validate_war","reason":"missing_target_id"})";
        return false;
    }

    void* target = lookup_character(static_cast<uint32_t>(target_id_raw));
    if (!target || is_sentinel(target) || !g_char_is_alive(target)) {
        if (response_out) *response_out = R"({"success":false,"action":"pre_validate_war","reason":"invalid_target"})";
        return false;
    }

    uintptr_t actor_ptr = reinterpret_cast<uintptr_t>(actor);
    uintptr_t target_ptr = reinterpret_cast<uintptr_t>(target);
    uint32_t actor_id = char_get_id(actor) & 0xFFFFFF;
    uint32_t target_id = char_get_id(target) & 0xFFFFFF;

    // Title realm data (null if unlanded)
    uintptr_t actor_tr = resolve_title_realm(actor_ptr);
    uintptr_t target_tr = resolve_title_realm(target_ptr);

    // Core data
    uintptr_t actor_core = safe_read<uintptr_t>(actor_ptr + ck3::character::REALM_DATA);

    // Current game date for truce expiry checks
    int32_t current_date_raw = 0;
    {
        uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
        if (gs) current_date_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    }

    // Track overall validity and individual conditions
    bool all_valid = true;
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("pre_validate_war");
    r.key("actor_id"); r.value_uint(actor_id);
    r.key("target_id"); r.value_uint(target_id);

    r.key("conditions"); r.begin_array();

    // Helper lambda to emit a condition result
    auto emit = [&](const char* id, const char* name, bool pass, const char* detail) {
        r.begin_object();
        r.key("id"); r.value_string(id);
        r.key("condition"); r.value_string(name);
        r.key("pass"); r.value_bool(pass);
        if (detail && detail[0]) {
            r.key("detail"); r.value_string(detail);
        }
        r.end_object();
        if (!pass) all_valid = false;
    };

    // ===================== ACTOR CONDITIONS =====================

    // A1: sworn_peace flag — cannot check (character flag table layout unknown)
    emit("A1", "no_sworn_peace", true, "unchecked — character flags not accessible");

    // A2: Not incapable — check trait array for INCAPACITATING flag
    {
        bool has_incapacitating = false;
        uintptr_t trait_array_ptr = safe_read<uintptr_t>(actor_ptr + ck3::character::TRAIT_ARRAY);
        int32_t trait_count = safe_read<int32_t>(actor_ptr + ck3::character::TRAIT_COUNT);
        uintptr_t tdb = safe_read_offset<uintptr_t>(ck3::globals::TRAIT_DB);
        if (trait_array_ptr && trait_count > 0 && trait_count < 200 && tdb) {
            uintptr_t trait_table = safe_read<uintptr_t>(tdb + ck3::trait_db::ARRAY_PTR);
            int32_t max_trait_id = safe_read<int32_t>(tdb + ck3::trait_db::ARRAY_COUNT);
            for (int32_t i = 0; i < trait_count; i++) {
                uint32_t tid = safe_read<uint32_t>(trait_array_ptr + i * 4);
                if (trait_table && tid < static_cast<uint32_t>(max_trait_id)) {
                    uintptr_t te = safe_read<uintptr_t>(trait_table + tid * 8);
                    if (te) {
                        uint32_t flags = safe_read<uint32_t>(te + ck3::trait_entry::FLAGS);
                        if (flags & ck3::trait_flags::INCAPACITATING) {
                            has_incapacitating = true;
                            break;
                        }
                    }
                }
            }
        }
        emit("A2", "not_incapable", !has_incapacitating,
             has_incapacitating ? "actor has incapacitating trait" : "");
    }

    // A3: can_attack_in_hierarchy — basic check: not attacking own liege/vassal in certain cases
    // Full check requires engine logic; we do a basic same-top-liege check
    {
        uint32_t actor_liege = get_liege_id_from_addr(actor_ptr);
        uint32_t target_liege = get_liege_id_from_addr(target_ptr);
        // If target IS actor's liege, or actor IS target's liege — engine decides
        bool is_own_liege = (target_id == actor_liege);
        bool is_own_vassal = (actor_id == target_liege);
        const char* detail = is_own_liege ? "target is actor's liege — engine evaluates" :
                             is_own_vassal ? "target is actor's vassal — engine evaluates" : "";
        emit("A3", "can_attack_in_hierarchy", true, detail);
    }

    // A4: No raised armies (unless government allows)
    // Mirror PDXScript has_raised_armies: iterate global army enumeration,
    // filter by owner_char_id, return true if any owner_link has troop_count > 0.
    {
        int owned_armies = 0;
        int max_troops = 0;
        uint32_t actor_full_ref = char_get_id(actor);
        bool has_raised = character_has_raised_armies(actor_full_ref,
                                                      &owned_armies, &max_troops);
        char detail_buf[128];
        if (has_raised) {
            snprintf(detail_buf, sizeof(detail_buf),
                     "actor has raised armies (owned=%d, max_troops=%d)",
                     owned_armies, max_troops);
        } else {
            detail_buf[0] = '\0';
        }
        emit("A4", "no_raised_armies", !has_raised, detail_buf);
    }

    // A5: Has valid CB — drive the deferred CB validity pipeline so we mirror
    // the engine's per-CB script `is_valid` trigger rather than vouching for
    // triples that AI_CB_EVALUATE_ALL will reject (ck-l0re,
    // failure_class=cb_validity_trigger_delta). Caller can opt out via
    // skip_cb_validity_check=true (e.g. fast-path discovery scans).
    //
    // ck-5qpg: default routing is now query_cb_validity_strict
    // (cb_query_strict_full_sync), which:
    //   - applies AI_CB_EVALUATE_ALL Stage 1 bilateral pre-filter (target
    //     IsRuler + IsAlive + actor!=target) on the IPC thread,
    //   - applies Stage 2 group-level outer-trigger flag filter on the game
    //     thread, and
    //   - surfaces the per-CB outer-filter dropout count in the [CB_PROBE]
    //     emit detail alongside the inner-valid count.
    // Per-CB outer triggers (allowed_for_character / allowed_against_character
    // / valid_to_start) and the distance gates remain deferred to ck-39e1.
    //
    // strict_cb_filter=false (legacy callers) reverts to cb_query_count_valid_sync
    // — engine inner pipeline only, no outer filter.
    {
        bool skip_cb = json_get_bool(params_json, "skip_cb_validity_check", false);
        // Strict is the new default; flag retained for backwards compatibility
        // with callers that explicitly want the legacy inner-only path.
        bool strict_cb = json_get_bool(params_json, "strict_cb_filter", true);
        int cb_timeout_ms = json_get_int(params_json, "cb_check_timeout_ms");
        if (cb_timeout_ms <= 0) cb_timeout_ms = 2000;

        if (skip_cb) {
            emit("A5", "has_valid_cb", true,
                 "skipped — caller passed skip_cb_validity_check=true");
        } else {
            int total_cbs = 0;
            int outer_dropped = 0;
            int candidates_post_outer = 0;
            char cb_err[64] = {};
            int valid_cbs = -1;
            const char* mode_tag = strict_cb ? "strict" : "engine pipeline";
            // ck-laf5: per_cb lifted to outer scope so the NO_VIABLE_PAIR
            // branch below can compute top-outer-reasons enrichment.
            std::vector<CBQueryResult> per_cb;
            // ck-z44u: the deferred g_cb_query slot is serviced by
            // drain_and_execute_intents on the GAME thread — while PAUSED no
            // tick drains it, so a bare submit+poll here fail-closed with A5
            // "cb_query_timeout" on perfectly healthy pairs (verify-round3
            // §5.3: standalone pre_validate_war AND declare_war's inline
            // pre-validation, both inline-T1 with no deferred-slot servicer).
            // Route the CB precheck through the proven T0 deferred pulse
            // (deferred_drain.cpp record→pulse→drain→restore; P5 exercised it
            // 6/6 clean with exact {paused,speed} restore): called paused it
            // pulses one drain window so the engine answers for real; called
            // unpaused it runs the closure untouched — the hot path never
            // touches the clock. If the pulse machinery itself cannot run,
            // the reply below is a TYPED contract error naming the condition
            // — never a silent timeout.
            const bool paused_at_submit = is_paused();
            std::string pulse_status;  // coordinator's typed JSON on non-Ok (not embedded)
            DeferredDrainStatus drain_status = run_deferred_pause_transparent(
                [&](std::string* /*out*/) {
                    if (strict_cb) {
                        CBStrictSummary sum{};
                        valid_cbs = cb_query_strict_full_sync(actor_id, target_id,
                                                             cb_timeout_ms,
                                                             &sum, &per_cb,
                                                             cb_err, sizeof(cb_err));
                        total_cbs = sum.total_cbs;
                        outer_dropped = sum.outer_filter_dropped;
                        candidates_post_outer = sum.candidates_post_outer;
                    } else {
                        valid_cbs = cb_query_count_valid_sync(actor_id, target_id,
                                                             cb_timeout_ms,
                                                             &total_cbs,
                                                             cb_err, sizeof(cb_err));
                    }
                },
                &pulse_status);
            char detail_buf[512];
            if (drain_status != DeferredDrainStatus::Ok) {
                // The coordinator could not open a drain window, so the engine
                // was never consulted (the closure did NOT run). Typed,
                // fail-closed, naming the exact condition — same posture as
                // the rc<0 branch below.
                const bool busy = (drain_status == DeferredDrainStatus::BusyDeferred);
                snprintf(detail_buf, sizeof(detail_buf),
                         "cb_validity_check_unavailable (%s) — fail-closed; the "
                         "paused CB precheck rides the deferred pulse and it could "
                         "not run: %s; set skip_cb_validity_check=true to bypass",
                         busy ? "deferred_drain_busy" : "drain_unavailable",
                         busy ? "another deferred call holds the drain lock, retry shortly"
                              : "sim not ticking (load/menu/transition) — not fixable "
                                "by pause toggles");
                emit("A5", "has_valid_cb", false, detail_buf);
            } else if (valid_cbs > 0) {
                if (strict_cb) {
                    snprintf(detail_buf, sizeof(detail_buf),
                             "%d/%d CBs valid (%s; outer_dropped=%d, "
                             "candidates_post_outer=%d%s)",
                             valid_cbs, total_cbs, mode_tag,
                             outer_dropped, candidates_post_outer,
                             paused_at_submit ? "; drained via paused pulse" : "");
                } else {
                    snprintf(detail_buf, sizeof(detail_buf),
                             "%d/%d CBs valid (%s%s)",
                             valid_cbs, total_cbs, mode_tag,
                             paused_at_submit ? "; drained via paused pulse" : "");
                }
                emit("A5", "has_valid_cb", true, detail_buf);
            } else if (valid_cbs == 0) {
                if (strict_cb) {
                    // ck-laf5: NO_VIABLE_PAIR enrichment — surface the dominant
                    // outer_reason (top-3) from the per-CB result vector so
                    // callers can distinguish "feudal actor dropped 34
                    // government_specific CBs" from "all dropped on liege
                    // relation". Mirrors PRE_VALIDATE_WAR_NO_VIABLE_PAIR_REASON
                    // in the bead: tagged class instead of opaque count.
                    struct ReasonCount { const char* tag; int n; };
                    ReasonCount rc[16] = {};
                    int rc_size = 0;
                    for (const auto& cb : per_cb) {
                        if (strcmp(cb.outer_status, "dropped") != 0) continue;
                        const char* tag = cb.outer_reason[0]
                            ? cb.outer_reason : "unknown";
                        bool found = false;
                        for (int i = 0; i < rc_size; ++i) {
                            if (strcmp(rc[i].tag, tag) == 0) {
                                rc[i].n++;
                                found = true;
                                break;
                            }
                        }
                        if (!found && rc_size < 16) {
                            rc[rc_size++] = {tag, 1};
                        }
                    }
                    // Sort descending by count (insertion sort, n<=16).
                    for (int i = 1; i < rc_size; ++i) {
                        ReasonCount key = rc[i];
                        int j = i - 1;
                        while (j >= 0 && rc[j].n < key.n) {
                            rc[j + 1] = rc[j];
                            --j;
                        }
                        rc[j + 1] = key;
                    }
                    char reasons_buf[160] = {};
                    int rb_off = 0;
                    const int top = rc_size < 3 ? rc_size : 3;
                    for (int i = 0; i < top; ++i) {
                        int written = snprintf(reasons_buf + rb_off,
                                               sizeof(reasons_buf) - rb_off,
                                               "%s%s=%d",
                                               i == 0 ? "" : ",",
                                               rc[i].tag, rc[i].n);
                        if (written < 0 ||
                            rb_off + written >= (int)sizeof(reasons_buf)) break;
                        rb_off += written;
                    }
                    snprintf(detail_buf, sizeof(detail_buf),
                             "0/%d CBs valid for this pair "
                             "(NO_VIABLE_PAIR — engine would reject; mode=%s; "
                             "outer_dropped=%d, candidates_post_outer=%d; "
                             "top_outer_reasons=[%s])",
                             total_cbs, mode_tag,
                             outer_dropped, candidates_post_outer,
                             reasons_buf[0] ? reasons_buf : "none");
                } else {
                    snprintf(detail_buf, sizeof(detail_buf),
                             "0/%d CBs valid for this pair "
                             "(NO_VIABLE_PAIR — engine would reject; mode=%s)",
                             total_cbs, mode_tag);
                }
                emit("A5", "has_valid_cb", false, detail_buf);
            } else if (valid_cbs == -4) {
                // Strict-mode bilateral pre-filter rejected the pair. The
                // engine's outer loop never even reaches the CB list for
                // these pairs (target dead/non-ruler/self-pair).
                snprintf(detail_buf, sizeof(detail_buf),
                         "bilateral_filter_reject (%s) — engine would reject "
                         "pair before CB iteration",
                         cb_err[0] ? cb_err : "unknown");
                emit("A5", "has_valid_cb", false, detail_buf);
            } else {
                // valid_cbs < 0 (and != -4): query did not produce a
                // definitive answer. Fail-closed: refusing to vouch is the
                // entire point of this gate. The caller can override by
                // setting skip_cb_validity_check=true if they have prior
                // knowledge.
                snprintf(detail_buf, sizeof(detail_buf),
                         "cb_validity_check_unavailable (%s) — fail-closed; "
                         "set skip_cb_validity_check=true to bypass",
                         cb_err[0] ? cb_err : "unknown");
                emit("A5", "has_valid_cb", false, detail_buf);
            }
        }
    }

    // A6: Not allied in war with target
    // Check if actor and target are on the same side in any active war
    bool opposing_in_war = false;  // R5: active war between the pair on OPPOSING sides
    {
        bool allied_in_war = false;
        if (actor_tr) {
            uintptr_t war_array_base = actor_tr + ck3::title_realm::WAR_REF_ARRAY;
            uintptr_t war_ref_data = safe_read<uintptr_t>(war_array_base + 0x00);
            int32_t count = safe_read<int32_t>(war_array_base + 0x0C);
            if (count < 0 || count > 50) count = 0;
            uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);

            for (int32_t i = 0; i < count && !allied_in_war; i++) {
                uint32_t war_ref = safe_read<uint32_t>(war_ref_data + i * 4);
                if (war_ref == 0 || war_ref == 0xFFFFFFFF) continue;

                uintptr_t war_ptr = 0;
                if (war_db) {
                    uint32_t idx = war_ref & 0xFFFFFF;
                    uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                    if (idx < capacity) {
                        uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                        if (table) {
                            uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                                              + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                            war_ptr = safe_read<uintptr_t>(entry);
                            if (war_ptr) {
                                uint32_t stored_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
                                if ((stored_ref & 0xFFFFFF) != idx) war_ptr = 0;
                            }
                        }
                    }
                }
                if (!war_ptr) continue;
                uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
                if (is_ended != 0) continue;

                // Check if both actor and target are on the same side (attacker or defender)
                // Attacker participants at +0x28 (CPdxArray), defender at +0x58
                // STRIDE FIX: ATK_PARTICIPANT(0x28)/DEF_PARTICIPANT(0x58) are
                // CPdxArray of CWarParticipant* — POINTERS, 8-byte stride — and
                // the char id lives at war_participant::CHAR_ID (+0x08) of each
                // dereferenced participant, NOT at struct start with a 0x38 stride.
                // The old reading scanned vtable bytes as char ids and effectively
                // never matched (A6 was meaningless for ALL verdicts). This mirrors
                // the 4 canonical participant readers (query_handlers.cpp:2931,
                // observation.cpp:1227, war_declaration_hook.cpp:116, war_score_hook.cpp:109).
                // Also extends A6 to detect OPPOSING-side membership (engine Gate 0:
                // no active war between the pair regardless of side), so an active
                // opposing-side war is honestly surfaced rather than passing clean.
                auto side_has = [&](size_t offset, uint32_t want) -> bool {
                    uintptr_t arr_data = safe_read<uintptr_t>(war_ptr + offset + 0x00);
                    int32_t arr_count = safe_read<int32_t>(war_ptr + offset + 0x0C);
                    if (arr_count < 0 || arr_count > 100 || !arr_data || !is_mapped(arr_data)) return false;
                    for (int32_t j = 0; j < arr_count; j++) {
                        uintptr_t part = safe_read<uintptr_t>(arr_data + j * 8);
                        if (!part || !is_mapped(part)) continue;
                        uint32_t pid = safe_read<uint32_t>(part + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
                        if (pid == want) return true;
                    }
                    return false;
                };

                bool actor_atk = side_has(ck3::war_obj::ATK_PARTICIPANT, actor_id);
                bool actor_def = side_has(ck3::war_obj::DEF_PARTICIPANT, actor_id);
                bool target_atk = side_has(ck3::war_obj::ATK_PARTICIPANT, target_id);
                bool target_def = side_has(ck3::war_obj::DEF_PARTICIPANT, target_id);
                // Same side together => "allied in war" (original A6 intent).
                if ((actor_atk && target_atk) || (actor_def && target_def)) {
                    allied_in_war = true;
                }
                // Opposite sides => an active war already exists between the pair
                // (engine Gate 0 culls a redundant declaration → war_count 0).
                if ((actor_atk && target_def) || (actor_def && target_atk)) {
                    opposing_in_war = true;
                }
            }
        }
        emit("A6", "not_allied_in_war", !allied_in_war,
             allied_in_war ? "actor and target are on the same side in an active war" : "");
    }

    // A7: Not bankrupt (gold >= 0)
    {
        bool in_debt = false;
        if (actor_core && safe_ptr(actor_core)) {
            int64_t gold = safe_read<int64_t>(actor_core + ck3::core_data::GOLD);
            in_debt = (gold < 0);
            if (in_debt) {
                char buf[64];
                snprintf(buf, sizeof(buf), "actor gold=%.1f", static_cast<double>(gold) / 100000.0);
                emit("A7", "not_in_debt", false, buf);
            } else {
                emit("A7", "not_in_debt", true, "");
            }
        } else {
            emit("A7", "not_in_debt", true, "no core data — assumed OK");
        }
    }

    // A8: Not in activity — cannot check (involved_activity offset not mapped)
    emit("A8", "not_in_activity", true, "unchecked — activity state offset unknown");

    // A9: Not allied to target (culture-conditional — always report alliance status)
    {
        bool is_allied = false;
        // Check via CDiplomaticRelation sorted array for alliance flags
        if (actor_core && safe_ptr(actor_core)) {
            uintptr_t rel_arr = safe_read<uintptr_t>(actor_core + ck3::relation_container::ARRAY_PTR);
            int32_t rel_count = safe_read<int32_t>(actor_core + ck3::relation_container::COUNT);
            if (rel_count < 0 || rel_count > 500) rel_count = 0;

            if (rel_arr && safe_ptr(rel_arr) && rel_count > 0) {
                // Binary search for target in sorted relation array
                int lo = 0, hi = rel_count - 1;
                while (lo <= hi) {
                    int mid = lo + (hi - lo) / 2;
                    uintptr_t entry_base = rel_arr + mid * ck3::relation_container::ENTRY_STRIDE;
                    uint32_t other_id = safe_read<uint32_t>(entry_base + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
                    if (other_id == target_id) {
                        uintptr_t rel_ptr = safe_read<uintptr_t>(entry_base + ck3::relation_container::ENTRY_PTR);
                        if (rel_ptr && safe_ptr(rel_ptr)) {
                            // Check alliance: reason_count > 0 and not broken
                            int32_t reason_count = safe_read<int32_t>(rel_ptr + ck3::alliance::REASON_COUNT);
                            uint8_t broken = safe_read<uint8_t>(rel_ptr + ck3::alliance::BROKEN_FLAG);
                            is_allied = (reason_count > 0 && !broken);
                        }
                        break;
                    } else if (other_id < target_id) {
                        lo = mid + 1;
                    } else {
                        hi = mid - 1;
                    }
                }
            }
        }
        emit("A9", "not_allied", !is_allied,
             is_allied ? "actor is allied to target — culture may block war" : "culture-conditional check");
    }

    // A10: No truce with target (culture-conditional)
    {
        bool has_truce = false;
        uintptr_t actor_diplo = safe_read<uintptr_t>(actor_ptr + ck3::character::DIPLOMATIC_DATA);
        if (actor_diplo && safe_ptr(actor_diplo)) {
            uintptr_t truce_arr = safe_read<uintptr_t>(actor_diplo + ck3::diplomatic_data::TRUCE_LIST_PTR);
            int32_t tc = safe_read<int32_t>(actor_diplo + ck3::diplomatic_data::TRUCE_LIST_COUNT);
            if (tc < 0 || tc > 200) tc = 0;

            for (int32_t i = 0; i < tc && !has_truce; i++) {
                uintptr_t rel = safe_read<uintptr_t>(truce_arr + i * 8);
                if (!rel || !safe_ptr(rel)) continue;

                uint32_t cid1 = safe_read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_LOW) & 0xFFFFFF;
                uint32_t cid2 = safe_read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_HIGH) & 0xFFFFFF;

                size_t expiry_off, reason_off;
                uint32_t other_id;
                if (cid1 == actor_id) {
                    other_id = cid2;
                    expiry_off = ck3::diplomatic_relation::TRUCE_EXPIRY_A;
                    reason_off = ck3::diplomatic_relation::TRUCE_REASON_TYPE_A;
                } else if (cid2 == actor_id) {
                    other_id = cid1;
                    expiry_off = ck3::diplomatic_relation::TRUCE_EXPIRY_B;
                    reason_off = ck3::diplomatic_relation::TRUCE_REASON_TYPE_B;
                } else {
                    continue;
                }

                if (other_id != target_id) continue;

                int32_t reason = safe_read<int32_t>(rel + reason_off);
                if (reason == ck3::diplomatic_relation::TRUCE_REASON_INVALID) continue;

                int32_t expiry = safe_read<int32_t>(rel + expiry_off);
                if (current_date_raw > 0 && expiry > 0 && expiry < current_date_raw) continue;

                has_truce = true;
            }
        }
        emit("A10", "no_truce", !has_truce,
             has_truce ? "actor has active truce with target — culture may block war" : "culture-conditional check");
    }

    // A11: Not imprisoned (actor)  — read-side honesty fix
    // L7 fix: ck3::funcs::IS_IMPRISONED (0x0664adf0) is maybe_realm_predicate_inverted
    // (independence predicate), NOT imprisonment. Use the AUTHORITATIVE pure-memory
    // signal instead (same read as do_query_character query_handlers.cpp:124-130 and
    // R4 above): actor core_data = *(actor+0x1A8); imprisoned == *(core_data+0x288)
    // link != null && mapped. An imprisoned actor cannot declare war.
    {
        bool actor_imprisoned = false;
        uintptr_t actor_core_link = safe_read<uintptr_t>(actor_ptr + ck3::character::REALM_DATA);  // 0x1A8
        if (actor_core_link && is_mapped(actor_core_link)) {
            uintptr_t link = safe_read<uintptr_t>(actor_core_link + ck3::core_data::IMPRISONER_LINK);  // 0x288
            actor_imprisoned = (link != 0 && is_mapped(link));
        }
        emit("A11", "not_imprisoned", !actor_imprisoned,
             actor_imprisoned ? "actor is imprisoned — cannot declare war" : "");
    }

    // A12: No subjugation offer pending — cannot check (variable system not mapped)
    emit("A12", "no_subjugation_pending", true, "unchecked — variable system not accessible");

    // A13: Admin government restriction — cannot fully check
    emit("A13", "admin_gov_allows", true, "unchecked — government type details not mapped");

    // ===================== RECIPIENT CONDITIONS =====================

    // R1: Not a herder government — cannot check (government flags not mapped)
    emit("R1", "not_herder_gov", true, "unchecked — government flags not accessible");

    // R2: Not tributary of actor
    {
        bool is_tributary = false;
        if (target_tr) {
            uintptr_t trib_data = safe_read<uintptr_t>(target_tr + ck3::title_realm::TRIBUTARY_ARRAY + 0x00);
            int32_t trib_count = safe_read<int32_t>(target_tr + ck3::title_realm::TRIBUTARY_ARRAY + 0x0C);
            if (trib_count < 0 || trib_count > 200) trib_count = 0;

            for (int32_t i = 0; i < trib_count; i++) {
                uint32_t trib_ref = safe_read<uint32_t>(trib_data + i * 4) & 0xFFFFFF;
                if (trib_ref == actor_id) {
                    is_tributary = true;
                    break;
                }
            }
        }
        emit("R2", "not_tributary_of_actor", !is_tributary,
             is_tributary ? "target is a tributary of actor" : "");
    }

    // R3: No strong hook on actor — cannot easily check (hook system complex)
    emit("R3", "no_strong_hook", true, "unchecked — hook evaluation requires game thread");

    // R4: If imprisoned, not by actor  (read-side honesty fix — discovery surface)
    //
    // ROOT CAUSE (doubly broken before): (1) it read target_ptr+0x1B8
    // (TITLE_REALM_DATA), not core_data (target_ptr+0x1A8); (2) it called
    // ck3::funcs::IS_IMPRISONED (0x0664adf0), which the L7 note / re_imprisonment.md
    // document is maybe_realm_predicate_inverted (an INDEPENDENCE predicate, not an
    // imprisonment check); (3) it emitted pass=true in BOTH branches so R4 could
    // never fail. Net: an imprisoned target (Louis of Italy / Tortona) reported
    // war_validity=declarable, the agent burned steered attempts, the engine culled
    // the war to war_count 0.
    //
    // FIX: use the AUTHORITATIVE imprisonment signal — the same pure-memory read
    // do_query_character ships at query_handlers.cpp:124-130: core_data =
    // *(target+0x1A8 REALM_DATA); imprisoned == (*(core_data+0x288 IMPRISONER_LINK)
    // != null && mapped). The engine culls steered claim_cb wars on imprisoned
    // targets regardless of imprisoner identity, so fail-on-imprisoned is the
    // correct honest default (imprisoner-id refinement deferred — would be a pure
    // read of *(*link+0) char-id). No engine-function call.
    {
        bool target_imprisoned = false;
        uintptr_t target_core = safe_read<uintptr_t>(target_ptr + ck3::character::REALM_DATA);  // 0x1A8
        if (target_core && is_mapped(target_core)) {
            uintptr_t link = safe_read<uintptr_t>(target_core + ck3::core_data::IMPRISONER_LINK);  // 0x288
            target_imprisoned = (link != 0 && is_mapped(link));
        }
        emit("R4", "not_imprisoned_by_actor", !target_imprisoned,
             target_imprisoned
                 ? "target is imprisoned — engine culls war (war_count 0); pick a free target"
                 : "");
    }

    // ===================== IS_SHOWN CONDITIONS (informational) =====================

    // S1: Not self
    {
        bool is_self = (actor_id == target_id);
        emit("S1", "not_self", !is_self, is_self ? "cannot declare war on self" : "");
    }

    // S2: Target is landed (playable)
    {
        bool is_landed = (target_tr != 0);
        emit("S2", "target_is_landed", is_landed,
             is_landed ? "" : "target is unlanded — cannot be war target");
    }

    // S3: Not already at war with target
    {
        bool at_war = false;
        if (actor_tr) {
            uintptr_t war_array_base = actor_tr + ck3::title_realm::WAR_REF_ARRAY;
            uintptr_t war_ref_data = safe_read<uintptr_t>(war_array_base + 0x00);
            int32_t count = safe_read<int32_t>(war_array_base + 0x0C);
            if (count < 0 || count > 50) count = 0;
            uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);

            for (int32_t i = 0; i < count && !at_war; i++) {
                uint32_t war_ref = safe_read<uint32_t>(war_ref_data + i * 4);
                if (war_ref == 0 || war_ref == 0xFFFFFFFF) continue;

                uintptr_t war_ptr = 0;
                if (war_db) {
                    uint32_t idx = war_ref & 0xFFFFFF;
                    uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                    if (idx < capacity) {
                        uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                        if (table) {
                            uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                                              + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                            war_ptr = safe_read<uintptr_t>(entry);
                            if (war_ptr) {
                                uint32_t stored_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
                                if ((stored_ref & 0xFFFFFF) != idx) war_ptr = 0;
                            }
                        }
                    }
                }
                if (!war_ptr) continue;
                uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
                if (is_ended != 0) continue;

                // Check if actor and target are on opposite sides
                uint32_t primary_atk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
                uint32_t primary_def = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;

                // Quick check: are actor and target the primary parties?
                if ((primary_atk == actor_id && primary_def == target_id) ||
                    (primary_atk == target_id && primary_def == actor_id)) {
                    at_war = true;
                }
            }
        }
        emit("S3", "not_at_war_with_target", !at_war,
             at_war ? "already at war with target" : "");
    }

    // R5: No active war between the pair on opposing sides (engine Gate 0).
    // Broader than S3 (which only matches the PRIMARY attacker/defender pair):
    // R5 catches the pair on opposite sides as ANY participant. computed in A6's
    // participant scan via the stride-corrected reader. The engine culls a
    // redundant declaration against a target you are already at war with.
    emit("R5", "no_active_opposing_war", !opposing_in_war,
         opposing_in_war ? "already at war with target on the opposing side — engine culls redundant declaration" : "");

    r.end_array(); // conditions

    r.key("valid"); r.value_bool(all_valid);
    r.end_object();

    if (response_out) *response_out = r.take();
    return true;
}

// =============================================================================
// Console Command Passthrough
// =============================================================================
// Queues a console command for execution on the game thread via the intent
// queue. The command is parsed and the matching handler is called during
// drain_and_execute_intents(), which runs in the serial section of on_tick.
//
// Params JSON:
//   command: string — full console command (e.g. "AI.declare_war 30966")
//
// Returns synchronously by polling for intent completion (up to 5 seconds).

#ifdef CK3_BRIDGE_DEBUG_VERBS  // rc2 B2: the debug console is compiled out of release builds
bool do_execute_console_command(const char* params_json, std::string* response_out) {
    const char* command = json_get_string(params_json, "command");
    if (!command || !command[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"execute_console_command","reason":"missing_command"})";
        return false;
    }

    // Copy command since json_get_string uses thread-local buffer
    std::string cmd_str(command);

    // Generate a unique request_id for tracking
    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "cc_%lu_%u",
             (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    // Build and queue the intent
    Intent intent = {};
    intent.type = IntentType::CONSOLE_EXECUTE;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.console_command = cmd_str;
    intent.request_id = request_id;
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("execute_console_command: queued '%s' as %s\n", cmd_str.c_str(), request_id.c_str());
    ADLOG("BT_TRACE do_execute_console_command: enqueued internal_rid=%s — entering 5s poll\n",
          request_id.c_str());

    // Poll for completion (synchronous — same pattern as declare_war)
    bool found_result = false;
    bool intent_succeeded = false;
    std::string output_str;
    char result_reason[128] = {};

    for (int poll = 0; poll < 100; poll++) {  // 100 * 50ms = 5 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                output_str = c.console_output;
                if (c.rejected && c.rejection_reason[0]) {
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                }
                ADLOG("BT_TRACE do_execute_console_command: poll_found internal_rid=%s executed=%d rejected=%d after=%dms\n",
                      request_id.c_str(), c.executed ? 1 : 0, c.rejected ? 1 : 0,
                      (poll + 1) * 50);
                break;
            }
        }
        if (found_result) break;
    }

    ADLOG("BT_TRACE do_execute_console_command: post-poll internal_rid=%s found_result=%d succeeded=%d — building response\n",
          request_id.c_str(), found_result ? 1 : 0, intent_succeeded ? 1 : 0);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("execute_console_command");
        r.key("command"); r.value_string(cmd_str.c_str());
        // ck-hzq3: renamed to avoid colliding with IPC-injected request_id.
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        // ck-jsis0: canonical dispatch identifier alias.
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            r.key("output"); r.value_string(output_str.c_str());
            if (!intent_succeeded && result_reason[0]) {
                r.key("reason"); r.value_string(result_reason);
            }
        } else {
            r.key("reason"); r.value_string("timeout");
        }
        r.end_object();
        *response_out = r.take();
    }

    ADLOG("BT_TRACE do_execute_console_command: returning internal_rid=%s succeeded=%d response_size=%zu\n",
          request_id.c_str(), intent_succeeded ? 1 : 0,
          response_out ? response_out->size() : 0);

    return intent_succeeded;
}
#endif  // CK3_BRIDGE_DEBUG_VERBS (rc2 B2)

// =============================================================================
// Scripted Effect Dispatch (ck-7ejs)
// =============================================================================
// Invoke a user-defined `scripted_effect` template by name on a character ROOT
// scope. End-to-end path:
//
//   1. IPC thread: validate inputs, generate request_id, enqueue
//      IntentType::SCRIPTED_EFFECT intent, poll g_intent_queue.completed_snapshot
//      for up to 5s (mirrors do_execute_console_command).
//   2. Game thread: drain_and_execute_intents → execute_scripted_effect_intent
//      (intent_dispatch.cpp). Resolves the template via
//      scripted_template_db::LOOKUP_TEMPLATE_FN against the .bss singleton,
//      builds a stack-allocated dispatcher node, calls SCRIPTED_EFFECT_EXECUTE
//      under watchdog + sigsetjmp protection.
//
// Engine pins consumed here: scripted_template_db::SCRIPTED_EFFECT_DB (ck-j31o),
// scripted_template_db::SCRIPTED_EFFECT_EXECUTE (ck-kz1v),
// scripted_template_db::LOOKUP_TEMPLATE_FN (ck-j31o).
// See re/findings/re_scripted_effect_dispatch.md for the full architecture.
//
// Request shape:
//   { "character_id": <int>, "effect_name": "<name>" }
// (effect_args is reserved for future expansion — the engine path takes
//  pre-bound parameters from the script source AST, which we don't yet
//  reconstruct on the bridge side.)

bool do_execute_scripted_effect(const char* params_json, std::string* response_out) {
    int char_id_raw = json_get_int(params_json, "character_id");
    const char* effect_name = json_get_string(params_json, "effect_name");

    if (char_id_raw <= 0) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_scripted_effect","reason":"character_id_required"})";
        return false;
    }
    if (!effect_name || !effect_name[0]) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_scripted_effect","reason":"effect_name_required"})";
        return false;
    }

    uint32_t char_id = static_cast<uint32_t>(char_id_raw);
    std::string effect_name_str(effect_name);  // copy — json_get_string returns
                                                // a thread-local buffer that can
                                                // be overwritten before the
                                                // game thread reads it.

    // resolve_only (default TRUE): the executor resolves the template by name and
    // reports effect_found/effect_not_found WITHOUT calling Execute. The Execute
    // path is now PINNED (scope_ctx frame, #73 / re_issue73_crack.md) and gated
    // behind an allow-list; resolve_only stays the default so the handler is
    // safe-by-default. Pass resolve_only=false ONLY on a throwaway observer save
    // during RE.
    bool resolve_only = json_get_bool(params_json, "resolve_only", true);

    // Build and queue the intent. Game-thread drain is responsible for actor
    // resolution and the engine call (see execute_scripted_effect_intent).
    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "se_%lu_%u",
             (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::SCRIPTED_EFFECT;
    intent.actor_id = char_id;
    intent.interaction_name = effect_name_str;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    intent.resolve_only = resolve_only;
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("execute_scripted_effect: queued effect='%s' actor=%u as %s\n",
          effect_name_str.c_str(), char_id, request_id.c_str());

    // Poll for completion (synchronous — same pattern as console / decision).
    bool found_result = false;
    bool intent_succeeded = false;
    std::string output_str;
    char result_reason[128] = {};

    for (int poll = 0; poll < 100; poll++) {  // 100 * 50ms = 5 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                output_str = c.console_output;
                if (c.rejected && c.rejection_reason[0]) {
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                }
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("execute_scripted_effect");
        r.key("character_id"); r.value_int(static_cast<int64_t>(char_id));
        r.key("effect_name"); r.value_string(effect_name_str.c_str());
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        // ck-jsis0: canonical dispatch identifier alias.
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (!output_str.empty()) {
                r.key("output"); r.value_string(output_str.c_str());
            }
            if (!intent_succeeded && result_reason[0]) {
                r.key("reason"); r.value_string(result_reason);
            }
        } else {
            r.key("reason"); r.value_string("timeout");
        }
        r.end_object();
        *response_out = r.take();
    }

    return intent_succeeded;
}

// =============================================================================
// break_alliance — the literal CBreakAllowAllianceEffect break (re_break_alliance_impl.md)
// =============================================================================
// STATUS: BUILD-READY but OBSERVER-SAVE-GATED. The RE pins the full legitimate
// chain at HIGH confidence — effect-apply → virtual CanExecute gate →
// break_alliance_cmd (alive + relation re-check) → relation_dissolve (clears the
// CActiveRelation+0x88 reasons vector, sets state byte +0x189, fires on-break
// notifications + player UI). That is the engine's own CanExecute→command→Execute
// pipeline (LEGITIMATE per the write-action audit), NOT a memory poke.
//
// What is NOT yet verified live (MEDIUM, re_break_alliance_impl.md §6): (a) the
// direct-cmd arg ABI — whether break_alliance_cmd(0x06750ef0) wants the
// CScopeObject* or the unwrapped CCharacter* for actor/ally; (b) packed-ref
// masking. And the call must run from the game-thread drain (safe tick context)
// under the RNG-scope + watchdog + sigsetjmp guards — exactly the WAR_CREATE_CORE
// pattern — not from this IPC thread. Wiring that intent-drain path + exposing it
// is GATED behind a throwaway-observer-save smoke of (a)/(b).
//
// SAFE-NOW alternative the agent should use instead for the real need ("don't get
// dragged into ally wars"): the `conqueror`-character-variable scripted-effect,
// which is character-root, needs no recipient, no new RE, and is already reachable
// via execute_scripted_effect's allow-list (`set_variable_conqueror`). The engine
// AI then auto-refuses offensive ally calls. See re_break_alliance_audit.md
// §"The ONE engine-native lever".
//
// IMPLEMENTATION (re_break_alliance_FINAL.md Path B): the real break is now
// implemented as IntentType::BREAK_ALLIANCE, drained on the GAME thread by
// execute_break_alliance_intent under the fake-RNG + watchdog + sigsetjmp guards.
// The arg-ABI is HIGH (two CCharacter* + force, resolved via lookup_character).
//
// It remains OBSERVER-SAVE-GATED behind the DEFAULT-OFF g_enable_break_alliance
// flag (FINAL §5): the one thing not yet verified live is the actual dissolve
// behaviour (MEDIUM), so until a throwaway-observer-save smoke confirms it, this
// handler must NOT fire the engine call against the live campaign. While the flag
// is OFF (the ship default) it returns the HONEST gated refusal + the safe-now
// `conqueror`-variable alternative, exactly as before — no engine call. When the
// flag is armed (set_break_alliance_enabled enabled:true, on a disposable save),
// AND the caller passes confirm_observer_save=true, it queues the BREAK_ALLIANCE
// intent and waits for the game-thread result.
//
// Flag default OFF — a fresh bridge ships inert: the atomic initialises to false.
std::atomic<bool> g_enable_break_alliance{false};

bool do_break_alliance(const char* params_json, std::string* response_out) {
    int actor_id  = json_get_int(params_json, "character_id");
    if (actor_id <= 0) actor_id = json_get_int(params_json, "actor_id");
    int ally_id   = json_get_int(params_json, "target_id");
    if (ally_id <= 0) ally_id = json_get_int(params_json, "ally_id");
    bool confirm  = json_get_bool(params_json, "confirm_observer_save", false);
    bool flag_on  = g_enable_break_alliance.load(std::memory_order_relaxed);

    // --- DEFAULT path: flag OFF (or no observer-save confirm) → honest refusal ---
    // No engine call. The flag/confirm gate keeps the live campaign untouched until
    // the disposable-save smoke (FINAL §5) validates live dissolve behaviour.
    if (!flag_on || !confirm) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("break_alliance");
            if (actor_id > 0) { r.key("actor_id"); r.value_int(actor_id & 0xFFFFFF); }
            if (ally_id  > 0) { r.key("ally_id");  r.value_int(ally_id  & 0xFFFFFF); }
            // Reconciled status messaging (FINAL §7): the arg-ABI is HIGH and the
            // drain path is IMPLEMENTED; what gates the real call is the default-OFF
            // flag + the live-dissolve smoke, NOT (as the old text claimed) an
            // unverified ABI or a missing drain path.
            r.key("reason");
            r.value_string(!flag_on
                ? "break_alliance_disabled:set_break_alliance_enabled_first"
                : "observer_save_gated:pass_confirm_observer_save_true_on_a_disposable_save");
            r.key("implemented"); r.value_bool(true);
            r.key("flag_enabled"); r.value_bool(flag_on);
            r.key("legitimate_chain");
            r.value_string("CBreakAllowAllianceEffect::Execute -> break_alliance_cmd -> relation_dissolve");
            r.key("recommended_alternative");
            r.value_string("execute_scripted_effect effect_name=set_variable_conqueror (ally-call-refusal posture; no RE blocker)");
            r.end_object();
            *response_out = r.take();
        }
        ADLOG("break_alliance: refused (flag_on=%d confirm=%d) actor=%d ally=%d\n",
              flag_on ? 1 : 0, confirm ? 1 : 0, actor_id, ally_id);
        return false;
    }

    // --- ARMED path: flag ON + confirm → queue the game-thread intent ---
    if (actor_id <= 0 || ally_id <= 0) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("break_alliance");
            r.key("reason"); r.value_string("actor_id_and_ally_id_required");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    uint32_t aid = static_cast<uint32_t>(actor_id) & 0xFFFFFF;
    uint32_t lid = static_cast<uint32_t>(ally_id)  & 0xFFFFFF;
    if (reject_player_actor(aid, "break_alliance", response_out)) return false;

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "brk_%u_%lu_%u",
             aid, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::BREAK_ALLIANCE;
    intent.actor_id = aid;
    intent.target_id = lid;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    g_intent_queue.enqueue(std::move(intent));
    ADLOG("break_alliance: queued (armed) actor=%u ally=%u rid=%s\n",
          aid, lid, request_id.c_str());

    bool found = false, ok = false; std::string out_str; char reason_buf[160] = {};
    for (int poll = 0; poll < 100; poll++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found = true; ok = c.executed && !c.rejected; out_str = c.console_output;
                if (c.rejected && c.rejection_reason[0])
                    strncpy(reason_buf, c.rejection_reason, sizeof(reason_buf) - 1);
                break;
            }
        }
        if (found) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(ok);
        r.key("action"); r.value_string("break_alliance");
        r.key("actor_id"); r.value_uint(aid);
        r.key("ally_id");  r.value_uint(lid);
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        if (found) {
            if (!out_str.empty()) { r.key("result_raw"); r.value_string(out_str.c_str(), out_str.size()); }
            if (!ok && reason_buf[0]) { r.key("reason"); r.value_string(reason_buf); }
        } else {
            r.key("reason"); r.value_string("intent_timeout_no_result");
        }
        r.end_object();
        *response_out = r.take();
    }
    return ok;
}

// IPC setter — set_break_alliance_enabled {enabled:bool}. Arms/disarms the
// default-OFF g_enable_break_alliance flag. Bridge-internal RE lever; the agent
// surface stays unwired until the disposable-save smoke passes (FINAL §5).
bool do_set_break_alliance_enabled(const char* params_json, std::string* out) {
    bool enabled = json_get_bool(params_json, "enabled", false);
    g_enable_break_alliance.store(enabled, std::memory_order_relaxed);
    ADLOG("set_break_alliance_enabled: %s\n", enabled ? "ENABLED" : "disabled");
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action");  r.value_string("set_break_alliance_enabled");
        r.key("enabled"); r.value_bool(enabled);
        r.end_object();
        *out = r.take();
    }
    return true;
}

// =============================================================================
// Engine Effect Dispatch — start_war path (ck-bt9x) — SCAFFOLD
// =============================================================================
// Goal: invoke the CStartWarEffect engine effect directly with explicit
// (target_character, casus_belli, claimant, target_title) params, fully
// bypassing the user scripted_effect dispatcher (CJominiScriptedEffect path
// owned by ck-7fyl).
//
// Status: SCAFFOLD. Returns "not_implemented_pending_re" until two RE blockers
// are resolved:
//
//   OQ-C: CStartWarEffect ctor — fields the bridge must populate to make the
//         object survive CStartWarEffect_CreateWar (0x052a11a0). Per ck-pu05
//         and ck-e58l.20, the function reads these fields from `this`:
//           +0x10  scope-context tail pointer (passed to scope evaluators)
//           +0x60  std::string CB-type name (script form)
//           +0x80  pre-resolved CCasusBelliType*
//           +0x98  embedded scope evaluator that yields the defender (target)
//           +0x140 embedded scope evaluator that yields the claimant
//           +0x1d8 CB-option array (stride 0xa8 = CB_PARAMS layout)
//           +0x1e4 CB-option count (i32)
//         Of these, the embedded scope evaluators at +0x98 / +0x140 are PDX
//         scope-tree objects. Constructing them synthetically from C++ is the
//         construction blocker — they are normally built by the script
//         compiler from `target_character = character:<id>` syntax. A naive
//         memset-zero will crash inside FUN_07561f50 (scope-eval).
//
//   OQ-D: scope-context arg (param_2) — CStartWarEffect_CreateWar deref's
//         *param_2 to recover the attacker character. Layout of this arg is
//         not yet pinned.
//
// Once both are pinned, the implementation path mirrors CONSOLE_EXECUTE: parse
// inputs, enqueue an intent, drain on the game thread (bridge_init.cpp's
// drain_and_execute_intents), and read war_created via WAR_DB delta (same
// post-flight pattern as do_declare_war).
//
// See re/findings/re_start_war_effect_dispatch.md (this bead) for the full
// RE survey and follow-ups. Sibling user-scripted path: ck-7fyl.

// =============================================================================
// WAR_CREATE_CORE direct invocation (ck-eyfb)
// =============================================================================
// Bypasses CStartWarEffect (no scope-evaluator construction) and the
// declare_war_interaction pipeline (no AI gate). Mirrors the 22-line
// WAR_DECLARE_SPECIAL_HANDLER (FUN_065db970): builds a minimal CB-params
// struct + direct call to WAR_CREATE_CORE on the game thread.
//
// Designed for the LLM-as-player claim-war scenario (A.6 / ck-1qzw): caller
// supplies a pre-validated (attacker, target, claim CB, claimed title,
// claimant) tuple. Validity gates are the caller's responsibility — the
// engine still runs Block-J vtable[2] but typically passes for hand-curated
// scenarios.
//
// Request shape:
//   { "actor_id":   <int>,                 // attacker character id
//     "target_id":  <int>,                 // defender (title-holder) character id
//     "casus_belli":"<cb script name>",    // e.g. "claim_cb"
//     "cb_title_id":<int>,                 // optional claim/conquest title id (0 = no title)
//     "claimant_id":<int> }                // optional; defaults to actor_id for claim_cb
// Aliases accepted: attacker_id, target_character_id, cb_name, title_id, target_title_id.

bool do_call_war_create_core(const char* params_json, std::string* response_out,
                              const char* action_label,
                              bool include_path_marker) {
    if (!action_label || !action_label[0]) action_label = "call_war_create_core";

    int actor_raw = json_get_int(params_json, "actor_id");
    if (actor_raw <= 0) actor_raw = json_get_int(params_json, "attacker_id");
    int target_raw = json_get_int(params_json, "target_id");
    if (target_raw <= 0) target_raw = json_get_int(params_json, "target_character_id");
    const char* cb_name = json_get_string(params_json, "casus_belli");
    if (!cb_name || !cb_name[0]) cb_name = json_get_string(params_json, "cb_name");
    // ck-yh5d.3: declare_war's force_player_path passes the original
    // params_json through verbatim, and declare_war's caller-facing
    // convention is "cb_title_id" (see do_declare_war:644). Earlier
    // versions only accepted "title_id"/"target_title_id", silently
    // dropping cb_title_id and queuing the WAR_CREATE_CORE_DIRECT
    // intent with title=0. cb_title_id is now the primary alias.
    int title_raw = json_get_int(params_json, "cb_title_id");
    if (title_raw <= 0) title_raw = json_get_int(params_json, "title_id");
    if (title_raw <= 0) title_raw = json_get_int(params_json, "target_title_id");
    int claimant_raw = json_get_int(params_json, "claimant_id");

    // ck-yh5d.3: reflection logging of the parsed param dict before
    // dispatch. Logs every key consumed by this handler so future
    // envelope renames or stripping steps are caught on the first
    // failing call instead of silently bottoming out as title=0.
    if (g_logfile) {
        fprintf(g_logfile,
                "[CK3-Bridge] %s: parsed actor_id=%d target_id=%d cb='%s' "
                "title=%d claimant=%d force_player_path=%d\n",
                action_label, actor_raw, target_raw,
                (cb_name && cb_name[0]) ? cb_name : "(missing)",
                title_raw, claimant_raw, include_path_marker ? 1 : 0);
    }

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string(action_label);
        if (include_path_marker) {
            r.key("path"); r.value_string("force_player_path");
        }
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };

    if (actor_raw <= 0) {
        short_error("actor_id_required");
        return false;
    }
    if (target_raw <= 0) {
        short_error("target_id_required");
        return false;
    }
    if (!cb_name || !cb_name[0]) {
        short_error("casus_belli_required");
        return false;
    }

    uint32_t actor_id  = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "call_war_create_core", response_out)) return false;
    uint32_t target_id = static_cast<uint32_t>(target_raw) & 0xFFFFFF;
    uint32_t claimant_id = (claimant_raw > 0)
                            ? (static_cast<uint32_t>(claimant_raw) & 0xFFFFFF)
                            : 0;  // 0 → executor sends 0xFFFFFFFF (engine sentinel)
    int      title_id  = (title_raw > 0) ? title_raw : 0;
    std::string cb_name_str(cb_name);

    // Build and queue the intent.
    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "wcc_%u_%lu_%u",
             actor_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::WAR_CREATE_CORE_DIRECT;
    intent.actor_id = actor_id;
    intent.target_id = target_id;
    intent.claimant_id = claimant_id;
    intent.title_id = title_id;
    intent.interaction_name = cb_name_str;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("call_war_create_core: queued atk=%u def=%u cb='%s' title=%d claimant=%u rid=%s\n",
          actor_id, target_id, cb_name_str.c_str(), title_id,
          claimant_id, request_id.c_str());

    // Poll for completion (synchronous, mirrors execute_console_command).
    bool found_result = false;
    bool intent_succeeded = false;
    std::string output_str;
    char result_reason[160] = {};

    for (int poll = 0; poll < 100; poll++) {  // 100 * 50ms = 5 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                output_str = c.console_output;
                if (c.rejected && c.rejection_reason[0]) {
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                }
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string(action_label);
        if (include_path_marker) {
            r.key("path"); r.value_string("force_player_path");
        }
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("target_id"); r.value_uint(target_id);
        r.key("casus_belli"); r.value_string(cb_name_str.c_str());
        if (title_id > 0)    { r.key("title_id"); r.value_int(title_id); }
        if (claimant_id != 0){ r.key("claimant_id"); r.value_uint(claimant_id); }
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        // ck-jsis0: canonical dispatch identifier alias.
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (!output_str.empty()) {
                // executor stores a small JSON blob (war_id, war_created, etc.)
                // in console_output — surface it raw so callers can parse.
                r.key("result_raw"); r.value_string(output_str.c_str(), output_str.size());
            }
            if (!intent_succeeded && result_reason[0]) {
                r.key("reason"); r.value_string(result_reason);
                r.key("status"); r.value_string("rejected");
            } else if (intent_succeeded) {
                r.key("status"); r.value_string("war_created");
            }
        } else {
            r.key("status"); r.value_string("timeout");
            r.key("reason"); r.value_string("intent_not_completed_within_5s");
        }
        r.end_object();
        *response_out = r.take();
    }

    return intent_succeeded;
}

// =============================================================================
// do_force_offensive_stance — Fix B1a (re_caiwarcoordinator_FINAL.md,
// re_steered_war_offensive_root.md). Enqueue a WAR_FORCE_OFFENSIVE_STANCE intent
// (drained on the GAME thread by execute_war_force_offensive_stance_intent) that
// drives the engine's OWN per-coordinator stance tick on the steered war's record,
// so the AI prosecutes it OFFENSIVELY (marches to siege). LEGITIMATE engine re-eval
// (the engine re-selects + writes rec+0x70 itself) — NOT a memory poke. DEFAULT-OFF
// behind g_force_offensive_stance; a no-op refusal while the flag is off. Mirrors
// do_call_war_create_core's enqueue + poll contract.
//   Input: { "war_id": <uint32 gen-tagged>, "attacker_id": <uint32 AI char> }
// =============================================================================
bool do_force_offensive_stance(const char* params_json, std::string* response_out) {
    uint32_t war_id = static_cast<uint32_t>(json_get_int(params_json, "war_id"));
    int atk_raw = json_get_int(params_json, "attacker_id");
    if (atk_raw <= 0) atk_raw = json_get_int(params_json, "actor_id");
    uint32_t attacker_id = static_cast<uint32_t>(atk_raw) & 0xFFFFFF;

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("force_offensive_stance");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };

    if (war_id == 0)      { short_error("war_id_required"); return false; }
    if (atk_raw <= 0)     { short_error("attacker_id_required"); return false; }
    if (reject_player_actor(attacker_id, "force_offensive_stance", response_out))
        return false;

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "fos_%u_%lu_%u",
             attacker_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::WAR_FORCE_OFFENSIVE_STANCE;
    intent.actor_id = attacker_id;
    intent.title_id = static_cast<int>(war_id);   // reused: gen-tagged war_id
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;                        // allow redrains (coordinator timing)
    intent.request_id = request_id;
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("force_offensive_stance: queued war=0x%x atk=%u rid=%s\n",
          war_id, attacker_id, request_id.c_str());

    bool found_result = false, intent_succeeded = false;
    std::string output_str;
    char result_reason[160] = {};
    for (int poll = 0; poll < 100; poll++) {       // 100 * 50ms = 5s
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                output_str = c.console_output;
                if (c.rejected && c.rejection_reason[0])
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("force_offensive_stance");
        r.key("war_id"); r.value_uint(war_id & 0xFFFFFF);
        r.key("attacker_id"); r.value_uint(attacker_id);
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (!output_str.empty()) {
                r.key("result_raw"); r.value_string(output_str.c_str(), output_str.size());
            }
            if (!intent_succeeded && result_reason[0]) {
                r.key("reason"); r.value_string(result_reason);
                r.key("status"); r.value_string("rejected");
            } else if (intent_succeeded) {
                r.key("status"); r.value_string("offensive");
            }
        } else {
            r.key("status"); r.value_string("timeout");
            r.key("reason"); r.value_string("intent_not_completed_within_5s");
        }
        r.end_object();
        *response_out = r.take();
    }
    return intent_succeeded;
}

// =============================================================================
// A8.2 — do_raise_armies (re/findings/re_raise_armies.md)
// =============================================================================
// Raise the levies of a CHOSEN AI character. RaiseAllArmiesCore is hard-wired
// to the g_PlayerCharacterID global, so the actual raise runs on the GAME
// THREAD via the guarded global-swap primitive in execute_raise_armies_intent
// (drained from on_tick). This handler validates the actor, queues a
// RAISE_ARMIES intent, and polls for the game-thread result (mirrors
// do_call_war_create_core).
//
// NEEDS-LIVE-HARNESS-VALIDATION: the global swap + indirect raise cannot be
// exercised on a core dump. The primitive lands behind this intent/drain gate;
// validate live (harness scenario strategic_raise_levies) before relying on it.
bool do_raise_armies(const char* params_json, std::string* response_out) {
    int actor_raw = json_get_int(params_json, "actor_id");
    if (actor_raw <= 0) actor_raw = json_get_int(params_json, "character_id");

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("raise_armies");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };

    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }

    uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    // The raise primitive only makes sense for an AI-controlled actor (the whole
    // point is to raise a steered AI's levies, not the player's).
    if (reject_player_actor(actor_id, "raise_armies", response_out)) return false;

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "raise_%u_%lu_%u",
             actor_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::RAISE_ARMIES;
    intent.actor_id = actor_id;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    // Readable action label so the query_intents "completed" entry carries a
    // human label (cosmetic parity with INTERACTION-type intents — Section A #3).
    intent.interaction_name = "raise_armies";
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("raise_armies: queued actor=%u rid=%s\n", actor_id, request_id.c_str());

    bool found_result = false;
    bool intent_succeeded = false;
    char result_reason[160] = {};
    // ★ SECTION-A FIX (re_deferred_write_verdicts.md): the poll window MUST be
    // strictly LESS than the client recv timeout (bridge_client.py recv(timeout=10.0))
    // or the client abandons the read at 10s ~2s before this handler writes its reply
    // → the caller sees a hard socket timeout with no matching request_id instead of
    // the dispatched_pending JSON. This is the IDENTICAL defect 96bae73 fixed for
    // move_army (12s→8s). Shrink 240x50ms (12s) -> 160x50ms (8s) so the typed verdict
    // (levies_raised / rejected + reason) or the dispatched_pending fallback ALWAYS
    // lands inside the 10s recv window; if the intent hasn't drained in 8s (slow
    // observer ticks / paused) we return dispatched_pending + dispatch_id and the agent
    // polls query_intents / query_dispatch_result{dispatch_id} for the typed verdict.
    // The completed-snapshot window stays at 64 so a burst can't evict our request_id.
    for (int poll = 0; poll < 160; poll++) {  // 160 * 50ms = 8s (< 10s client recv)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto completed = g_intent_queue.completed_snapshot(64);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                if (c.rejected && c.rejection_reason[0]) {
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                }
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("raise_armies");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (intent_succeeded) {
                r.key("status"); r.value_string("levies_raised");
            } else {
                r.key("status"); r.value_string("rejected");
                if (result_reason[0]) { r.key("reason"); r.value_string(result_reason); }
            }
        } else {
            // Not a failure: the intent is dispatched + typically executes
            // asynchronously just after this window. The caller should confirm
            // via query_intents (the completed entry shows executed:true).
            r.key("status"); r.value_string("dispatched_pending");
            r.key("reason"); r.value_string("not_confirmed_within_8s; poll query_intents / query_dispatch_result{dispatch_id} to verify executed (then query_armies shows the raised army)");
        }
        r.key("validation"); r.value_string("live_2026-06-21: executes safely (no crash/hang); completion confirmed via query_intents");
        r.end_object();
        *response_out = r.take();
    }
    return intent_succeeded;
}

// =============================================================================
// #96 — do_raise_raiders (re/findings/re_raid_system.md)
// =============================================================================
// Toggle a CHOSEN AI ruler's raised armies into RAIDING mode — the Norse/tribal
// resource engine (raise → raid → loot for gold/prestige/piety, NO casus belli).
// The toggle-raiding dispatch path is hard-wired to g_PlayerCharacterID, so the
// actual toggle runs on the GAME THREAD via the guarded global-swap primitive in
// execute_raise_raiders_intent (drained from on_tick). This handler validates the
// actor, queues a RAISE_RAIDERS intent, and polls for the game-thread result.
//
// IMPORTANT: the engine command is VALIDATOR-GATED (CToggleRaidingCommand::IsValid)
// — an ineligible (non-Norse / no-raised-armies) ruler safely no-ops. RAISE the
// armies first (act type="raise_armies") so there are raisable armies to toggle.
// Verify the raid via query_armies readback (army+0x1b4 raiding flag).
bool do_raise_raiders(const char* params_json, std::string* response_out) {
    int actor_raw = json_get_int(params_json, "actor_id");
    if (actor_raw <= 0) actor_raw = json_get_int(params_json, "character_id");

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("raise_raiders");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };

    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }

    uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    // The raid toggle only makes sense for an AI-controlled actor.
    if (reject_player_actor(actor_id, "raise_raiders", response_out)) return false;

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "raid_%u_%lu_%u",
             actor_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::RAISE_RAIDERS;
    intent.actor_id = actor_id;
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    intent.interaction_name = "raise_raiders";  // readable query_intents label (Section A #3)
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("raise_raiders: queued actor=%u rid=%s\n", actor_id, request_id.c_str());

    bool found_result = false;
    bool intent_succeeded = false;
    char result_reason[160] = {};
    // ★ SECTION-A FIX (re_deferred_write_verdicts.md): same 12s→8s shrink as
    // raise_armies/move_army — keep the poll window strictly < the 10s client recv
    // timeout so the typed verdict (or dispatched_pending fallback) always reaches
    // the caller instead of a bare socket timeout.
    for (int poll = 0; poll < 160; poll++) {  // 160 * 50ms = 8s (< 10s client recv)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto completed = g_intent_queue.completed_snapshot(64);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                if (c.rejected && c.rejection_reason[0]) {
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                }
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("raise_raiders");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (intent_succeeded) {
                r.key("status"); r.value_string("raiding_toggle_dispatched");
            } else {
                r.key("status"); r.value_string("rejected");
                if (result_reason[0]) { r.key("reason"); r.value_string(result_reason); }
            }
        } else {
            r.key("status"); r.value_string("dispatched_pending");
            r.key("reason"); r.value_string("not_confirmed_within_8s; poll query_intents / query_dispatch_result{dispatch_id} to verify executed");
        }
        r.key("hint"); r.value_string("raise armies first (raise_armies); then verify army+0x1b4 raiding flag via query_armies. validator-gated: ineligible/non-Norse ruler no-ops.");
        r.end_object();
        *response_out = r.take();
    }
    return intent_succeeded;
}

// =============================================================================
// #111 — do_create_title (re/findings/re_create_title_action.md)
// =============================================================================
// LEGITIMATE de-jure create-title. The agent passes the AI ruler (actor_id) and the
// target title — either a numeric title_id OR a title_key string (e.g. "d_northumbria",
// resolved from query(domain="conquest_targets")). This handler resolves the key→id,
// validates the actor, queues a CREATE_TITLE intent, and polls for the game-thread
// result. The actual command build + engine CanExecute gate + GAME_COMMAND_DISPATCH(0xe)
// runs on the game thread in execute_create_title_intent (the engine applies the gold
// cost + creates the title natively). An ineligible ruler (doesn't control the de-jure
// counties / can't afford) is refused by the engine gate → "create_title_refused_by_engine_gate".
//
// Resolve a de-jure title key (e.g. "d_northumbria") to its title id by iterating the
// LANDED_TITLE_DB and matching the title-type NAME_KEY. Returns 0xFFFFFFFF if not found.
uint32_t resolve_title_id_by_key(const char* key) {
    if (!key || !key[0]) return 0xFFFFFFFFu;
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::LANDED_TITLE_DB);
    if (!db || !is_mapped(db)) return 0xFFFFFFFFu;
    uintptr_t arr = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
    int32_t   cnt = safe_read<int32_t>(db + ck3::title_db::ARRAY_COUNT);
    if (!arr || !is_mapped(arr) || cnt <= 0 || cnt > 1000000) return 0xFFFFFFFFu;
    std::string want(key);
    for (int32_t tid = 0; tid < cnt; tid++) {
        uintptr_t elem = arr + ck3::title_db::ELEMENT_PTR_OFFSET
                       + static_cast<uintptr_t>(tid) * ck3::title_db::ELEMENT_STRIDE;
        uintptr_t tobj = safe_read<uintptr_t>(elem);
        if (!tobj || !is_mapped(tobj)) continue;
        // verify the resolved object's own id matches (mirrors title_db_lookup)
        if ((safe_read<uint32_t>(tobj + ck3::title_db::ID_VERIFY_OFFSET) & 0xFFFFFF)
            != static_cast<uint32_t>(tid)) continue;
        uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
        if (!type_ptr || !is_mapped(type_ptr)) continue;
        std::string k = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        if (k == want) return static_cast<uint32_t>(tid);
    }
    return 0xFFFFFFFFu;
}

bool do_create_title(const char* params_json, std::string* response_out) {
    int actor_raw = json_get_int(params_json, "actor_id");
    if (actor_raw <= 0) actor_raw = json_get_int(params_json, "character_id");

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("create_title");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };

    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }
    uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "create_title", response_out)) return false;

    // Target title: numeric title_id wins; else resolve title_key.
    int title_id_raw = json_get_int(params_json, "title_id");
    uint32_t title_id = 0xFFFFFFFFu;
    std::string resolved_from;
    if (title_id_raw > 0) {
        title_id = static_cast<uint32_t>(title_id_raw) & 0xFFFFFF;
    } else {
        const char* key = json_get_string(params_json, "title_key");
        if (!key || !key[0]) key = json_get_string(params_json, "title");
        if (key && key[0]) {
            title_id = resolve_title_id_by_key(key);
            resolved_from = key;
        }
    }
    if (title_id == 0xFFFFFFFFu) {
        short_error(resolved_from.empty()
            ? "title_required (pass title_id or title_key, e.g. \"d_northumbria\")"
            : "title_key_not_found");
        return false;
    }

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "crtitle_%u_%lu_%u",
             actor_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::CREATE_TITLE;
    intent.actor_id = actor_id;
    intent.title_id = static_cast<int>(title_id);
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("create_title: queued actor=%u title_id=%u rid=%s\n",
          actor_id, title_id, request_id.c_str());

    bool found_result = false;
    bool intent_succeeded = false;
    char result_reason[160] = {};
    for (int poll = 0; poll < 240; poll++) {  // 240 * 50ms = 12s
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto completed = g_intent_queue.completed_snapshot(64);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                if (c.rejected && c.rejection_reason[0]) {
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                }
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("create_title");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("title_id"); r.value_uint(title_id);
        if (!resolved_from.empty()) { r.key("title_key"); r.value_string(resolved_from.c_str()); }
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (intent_succeeded) {
                r.key("status"); r.value_string("create_title_command_dispatched");
            } else {
                r.key("status"); r.value_string("rejected");
                if (result_reason[0]) { r.key("reason"); r.value_string(result_reason); }
            }
        } else {
            r.key("status"); r.value_string("dispatched_pending");
            r.key("reason"); r.value_string("not_confirmed_within_12s; poll query_intents to verify executed");
        }
        r.key("hint"); r.value_string("engine-gated (not-extant + gold cost) PLUS #131 de-jure-majority gate for kingdom/empire tiers (refuses power-projection force-mints with <51% realm de-jure control). Verify via query_realm / titles readback that the ruler now HOLDS the title.");
        r.end_object();
        *response_out = r.take();
    }
    return intent_succeeded;
}

bool do_execute_engine_effect(const char* params_json, std::string* response_out) {
    int actor_id_raw = json_get_int(params_json, "actor_id");
    if (actor_id_raw <= 0) actor_id_raw = json_get_int(params_json, "character_id");
    const char* effect_name = json_get_string(params_json, "effect_name");
    int target_id_raw = json_get_int(params_json, "target_id");
    if (target_id_raw <= 0) target_id_raw = json_get_int(params_json, "target_character_id");
    const char* cb_name = json_get_string(params_json, "casus_belli");
    if (!cb_name || !cb_name[0]) cb_name = json_get_string(params_json, "cb_name");
    int claimant_id_raw = json_get_int(params_json, "claimant_id");
    int title_id_raw = json_get_int(params_json, "target_title_id");
    if (title_id_raw <= 0) title_id_raw = json_get_int(params_json, "title_id");

    if (actor_id_raw <= 0) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_engine_effect","reason":"actor_id_required"})";
        return false;
    }
    if (!effect_name || !effect_name[0]) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_engine_effect","reason":"effect_name_required"})";
        return false;
    }
    // Only `start_war` is in scope for ck-bt9x; reject other engine effects
    // up front so callers don't think we silently accept every name.
    if (strcmp(effect_name, "start_war") != 0) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("execute_engine_effect");
            r.key("reason"); r.value_string("effect_not_supported");
            r.key("effect_name"); r.value_string(effect_name);
            r.key("supported"); r.value_string("start_war");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    if (target_id_raw <= 0) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_engine_effect","reason":"target_id_required"})";
        return false;
    }
    if (!cb_name || !cb_name[0]) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_engine_effect","reason":"casus_belli_required"})";
        return false;
    }

    // Game-object reads (character lookup, cb-name resolution) are deferred
    // to the game-thread dispatch once the construction path is pinned —
    // IPC-thread safe-action classification requires no game-object reads.

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("execute_engine_effect");
        r.key("reason"); r.value_string("not_implemented_pending_re");
        r.key("actor_id"); r.value_int(actor_id_raw);
        r.key("target_id"); r.value_int(target_id_raw);
        r.key("effect_name"); r.value_string(effect_name);
        r.key("casus_belli"); r.value_string(cb_name);
        if (claimant_id_raw > 0) { r.key("claimant_id"); r.value_int(claimant_id_raw); }
        if (title_id_raw > 0) { r.key("target_title_id"); r.value_int(title_id_raw); }
        r.key("notes"); r.value_string(
            "CStartWarEffect ctor + scope-evaluator construction not yet "
            "pinned (OQ-C/OQ-D) — see re/findings/re_start_war_effect_dispatch.md "
            "(ck-bt9x). CStartWarEffect_CreateWar (0x052a11a0) is decompiled "
            "and its arg shape is known, but invoking it requires a "
            "constructed CStartWarEffect with embedded scope evaluators at "
            "+0x98 (target) and +0x140 (claimant); synthesising those from "
            "C++ is the open construction blocker.");
        r.end_object();
        *response_out = r.take();
    }
    return false;
}

// =============================================================================
// Execute Script Block (ck-kkaxx / ck-zvu5l.3)
// =============================================================================
// Generalised primitive: compile arbitrary PDX-script effect-block text at
// runtime, then execute the resulting CJominiEffect tree against a caller-
// supplied scope.
//
// Today this handler parses the OUTER effect block structurally and routes
// known effects to existing direct-call handlers:
//   - `start_war = { ... }` → do_call_war_create_core (ck-eyfb)
// All other outer effects return `not_implemented_pending_compiler_re` —
// the full PDX-script compiler entry point (text → CJominiEffect tree with
// constructed scope evaluators) is still the open RE blocker tracked by
// ck-zvu5l.1 / ck-zvu5l.2. See re/findings/re_script_block_compiler_quest.md.
//
// Request shape (both accepted):
//   { "script_text":  "<PDX-script effect block>",
//     "scope_dict":   { "character_id": <int>,        // ROOT scope = actor
//                       "target_id":    <int> (opt),
//                       "cb_title_id":  <int> (opt),  // resolves title:<name>
//                       "claimant_id":  <int> (opt) }
//   }
//   { "script_text":  "<...>", "character_id": <int>, "target_id": <int>, ... }
//
// scope_dict takes precedence when present; flat top-level keys remain
// accepted for backward compatibility with the original stub callers.

namespace {

// Extract a sub-object int field. Returns -1 on miss / non-int.
int read_scope_int(const rapidjson::Value& obj, const char* key) {
    auto it = obj.FindMember(key);
    if (it == obj.MemberEnd()) return -1;
    if (it->value.IsInt())   return it->value.GetInt();
    if (it->value.IsUint())  return static_cast<int>(it->value.GetUint());
    return -1;
}

struct ScopeIds {
    int character_id = -1;
    int target_id    = -1;
    int cb_title_id  = -1;
    int claimant_id  = -1;
};

ScopeIds load_scope(const char* params_json) {
    ScopeIds s;
    if (!params_json) return s;

    rapidjson::Document doc;
    doc.Parse(params_json);
    if (!doc.HasParseError() && doc.IsObject()) {
        auto sd_it = doc.FindMember("scope_dict");
        if (sd_it == doc.MemberEnd()) sd_it = doc.FindMember("scope");
        if (sd_it != doc.MemberEnd() && sd_it->value.IsObject()) {
            const auto& sd = sd_it->value;
            s.character_id = read_scope_int(sd, "character_id");
            if (s.character_id <= 0) s.character_id = read_scope_int(sd, "actor_id");
            s.target_id    = read_scope_int(sd, "target_id");
            if (s.target_id <= 0)    s.target_id    = read_scope_int(sd, "target_character_id");
            s.cb_title_id  = read_scope_int(sd, "cb_title_id");
            if (s.cb_title_id <= 0)  s.cb_title_id  = read_scope_int(sd, "title_id");
            s.claimant_id  = read_scope_int(sd, "claimant_id");
        }
    }
    if (s.character_id <= 0) {
        s.character_id = json_get_int(params_json, "character_id");
        if (s.character_id <= 0) s.character_id = json_get_int(params_json, "actor_id");
    }
    if (s.target_id <= 0) {
        s.target_id = json_get_int(params_json, "target_id");
        if (s.target_id <= 0) s.target_id = json_get_int(params_json, "target_character_id");
    }
    if (s.cb_title_id <= 0) {
        s.cb_title_id = json_get_int(params_json, "cb_title_id");
        if (s.cb_title_id <= 0) s.cb_title_id = json_get_int(params_json, "title_id");
    }
    if (s.claimant_id <= 0) {
        s.claimant_id = json_get_int(params_json, "claimant_id");
    }
    return s;
}

void write_err(std::string* response_out, const char* reason,
               const char* detail_key = nullptr, const std::string& detail = std::string()) {
    if (!response_out) return;
    ResponseBuilder r; r.begin_object();
    r.key("success"); r.value_bool(false);
    r.key("action"); r.value_string("execute_script_block");
    r.key("reason"); r.value_string(reason);
    if (detail_key && !detail.empty()) {
        r.key(detail_key); r.value_string(detail.c_str(), detail.size());
    }
    r.end_object();
    *response_out = r.take();
}

// Route a parsed `start_war = { ... }` block through call_war_create_core.
// Returns the handler's success bool; response_out is filled by that handler
// with `action="execute_script_block"` so callers see the originating action.
bool dispatch_start_war(const ScriptBlock& pe, const ScopeIds& scope,
                        std::string* response_out) {
    auto find = [&](const char* key) -> const std::string* {
        auto it = pe.params.find(key);
        return (it == pe.params.end()) ? nullptr : &it->second;
    };
    auto bad_ref = [&](const char* reason, const std::string& detail) {
        write_err(response_out, reason, "detail", detail);
        return false;
    };

    // casus_belli — required.
    const std::string* cb_val = find("casus_belli");
    if (!cb_val || cb_val->empty()) {
        write_err(response_out, "missing_param", "missing_key", std::string("casus_belli"));
        return false;
    }
    std::string cb_name = *cb_val;

    // attacker_war_leader — defaults to scope.character_id (root).
    int actor_id = scope.character_id;
    if (const std::string* atk = find("attacker_war_leader")) {
        if (*atk == "root" || *atk == "this") {
            actor_id = scope.character_id;
        } else {
            std::string stripped = strip_ref_prefix(*atk, "character:");
            int v = 0;
            if (parse_int_strict(stripped, &v) && v > 0) {
                actor_id = v;
            } else {
                return bad_ref("unresolvable_scope_ref",
                               "attacker_war_leader=" + *atk +
                               " (accepted: `root`, `this`, `character:<id>`)");
            }
        }
    }
    if (actor_id <= 0) {
        write_err(response_out, "actor_unresolved",
                  "hint", std::string("set scope_dict.character_id or attacker_war_leader=character:<id>"));
        return false;
    }

    // target (defender) — required (script or scope).
    int target_id = scope.target_id;
    if (const std::string* tgt = find("target")) {
        std::string stripped = strip_ref_prefix(*tgt, "character:");
        int v = 0;
        if (parse_int_strict(stripped, &v) && v > 0) {
            target_id = v;
        } else {
            return bad_ref("unresolvable_scope_ref",
                           "target=" + *tgt +
                           " (accepted: `character:<id>`; or pass numeric via scope_dict.target_id)");
        }
    }
    if (target_id <= 0) {
        write_err(response_out, "missing_param", "missing_key",
                  std::string("target (or scope_dict.target_id)"));
        return false;
    }

    // target_title — optional. Title-name refs (`title:d_X`) require a
    // numeric override via scope_dict.cb_title_id because no title-name → id
    // resolver exists server-side.
    int title_id = scope.cb_title_id;
    if (const std::string* tt = find("target_title")) {
        std::string stripped = strip_ref_prefix(*tt, "title:");
        int v = 0;
        if (parse_int_strict(stripped, &v) && v > 0) {
            title_id = v;
        } else if (title_id <= 0) {
            return bad_ref("unresolvable_title_ref",
                           "target_title=" + *tt +
                           " (no title-name resolver in bridge; pass numeric "
                           "id via scope_dict.cb_title_id)");
        }
    }

    // claim_on — optional. Defaults to scope.claimant_id, or actor for `root`.
    int claimant_id = scope.claimant_id;
    if (const std::string* co = find("claim_on")) {
        if (*co == "root" || *co == "this") {
            claimant_id = actor_id;
        } else {
            std::string stripped = strip_ref_prefix(*co, "character:");
            int v = 0;
            if (parse_int_strict(stripped, &v) && v > 0) {
                claimant_id = v;
            } else {
                return bad_ref("unresolvable_scope_ref",
                               "claim_on=" + *co +
                               " (accepted: `root`, `this`, `character:<id>`)");
            }
        }
    }

    // Build a synthetic params_json for do_call_war_create_core. All values
    // are validated numerics or PDX identifiers (no JSON escaping needed —
    // the parser restricts chars to is_pdx_ident_char).
    char synth[640];
    int n = std::snprintf(synth, sizeof(synth),
        "{\"actor_id\":%d,\"target_id\":%d,\"casus_belli\":\"%s\"",
        actor_id, target_id, cb_name.c_str());
    if (n > 0 && title_id > 0) {
        n += std::snprintf(synth + n, sizeof(synth) - static_cast<std::size_t>(n),
                           ",\"cb_title_id\":%d", title_id);
    }
    if (n > 0 && claimant_id > 0) {
        n += std::snprintf(synth + n, sizeof(synth) - static_cast<std::size_t>(n),
                           ",\"claimant_id\":%d", claimant_id);
    }
    if (n <= 0 || n >= static_cast<int>(sizeof(synth)) - 2) {
        write_err(response_out, "synthetic_params_overflow");
        return false;
    }
    std::snprintf(synth + n, sizeof(synth) - static_cast<std::size_t>(n), "}");

    return do_call_war_create_core(synth, response_out, "execute_script_block");
}

}  // anonymous namespace

bool do_execute_script_block(const char* params_json, std::string* response_out) {
    const char* script_text = json_get_string(params_json, "script_text");
    if (!script_text || !script_text[0]) {
        script_text = json_get_string(params_json, "text");
    }

    ScopeIds scope = load_scope(params_json);

    if (!script_text || !script_text[0]) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_script_block","reason":"script_text_required"})";
        return false;
    }
    if (scope.character_id <= 0) {
        if (response_out) *response_out =
            R"({"success":false,"action":"execute_script_block","reason":"character_id_required"})";
        return false;
    }

    ScriptBlock pe = parse_script_block(script_text);
    if (!pe.valid) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("execute_script_block");
            r.key("reason"); r.value_string("script_parse_error");
            r.key("parse_error"); r.value_string(pe.error.c_str(), pe.error.size());
            r.key("parse_offset"); r.value_uint(static_cast<uint64_t>(pe.error_offset));
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    if (pe.effect_name == "start_war") {
        return dispatch_start_war(pe, scope, response_out);
    }

    // Parameterless block (`<effect> = yes`): no PDX AST to synthesize — the
    // engine has no callable text->parse-tree compiler (proven, #80 §2). Route
    // it through the existing scripted-effect resolver, which queues a crash-
    // protected, allow-list-gated SCRIPTED_EFFECT intent. The intent-layer gate
    // (scripted_effect_is_allow_listed, intent_dispatch.cpp:202) stays the sole
    // legitimacy gate — a literal `add_gold = yes` routes in here but is REFUSED
    // there, so there is no resource-mint end-run.
    if (pe.params.empty()) {
        ResponseBuilder synth; synth.begin_object();
        synth.key("effect_name"); synth.value_string(pe.effect_name.c_str(), pe.effect_name.size());
        synth.key("character_id"); synth.value_int(scope.character_id);
        if (scope.target_id > 0) { synth.key("target_id"); synth.value_int(scope.target_id); }
        synth.end_object();
        std::string synth_params = synth.take();
        return do_execute_scripted_effect(synth_params.c_str(), response_out);
    }

    // Param-bearing block: requires synthesizing a CParser AST node (no runtime
    // text->parse-tree entry exists — proven, #80 §2). Not implementable today.
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("execute_script_block");
        r.key("reason"); r.value_string("literal_block_compiler_proven_impossible");
        r.key("effect_name"); r.value_string(pe.effect_name.c_str(), pe.effect_name.size());
        r.key("character_id"); r.value_int(scope.character_id);
        if (scope.target_id > 0) { r.key("target_id"); r.value_int(scope.target_id); }
        r.key("see"); r.value_string("re/findings/re_issue80_crack.md");
        r.key("notes"); r.value_string(
            "Param-bearing blocks need CParser AST synthesis: the engine has no "
            "runtime text->parse-tree entry (proven, #80 §2). Parameterless "
            "blocks route via execute_scripted_effect; `start_war` routes via "
            "call_war_create_core.");
        r.end_object();
        *response_out = r.take();
    }
    return false;
}

// =============================================================================
// enter_observe_mode (ck-dq7ym.8): programmatic entry into observer mode.
//
// Premise: the GUI button at multiplayer_lobby.gui:1081-1165 (the one the
// uinput fallback clicks at 540,1060) fires two onclick handlers in order:
//
//   [CloseGameView('find_title')]
//   [LobbyView.ToggleObserve]
//
// Both are gui-datafunction references. The owning class is
// `CJominiMultiplayerLobbyView`. See re/offsets/observer_data_functions.h
// `ck3::lobby_view` and `ck3::close_game_view` namespaces for offsets.
//
// This handler:
//   1. Reports whether the live LobbyView singleton has been captured by
//      the constructor hook (bridge/src/lobby_view_hook.cpp).
//   2. If captured, asks the engine to unpause via the existing game_unpause
//      mechanism — the bridge's auto-unpause loop also drives this, but
//      having an explicit IPC-triggered call lets the agent/ck3ctl request
//      the transition immediately on detecting the lobby screen.
//
// What's still missing (filed as follow-up beads — see SESSION-REPORT.md):
//   • Calling `CloseGameView('find_title')` directly. The callback at
//     `ck3::close_game_view::CALLBACK_BODY` (raw 0x388cc90) takes an arg-pack
//     struct (rdx) the bridge can't yet construct. Decompiling the body
//     fully would reveal the layout; once known, this handler should invoke
//     it on-thread with `{view='find_title'}`.
//   • Calling `LobbyView.ToggleObserve` via GameCommandDispatch. The thunk
//     `ck3::observer_data_functions::TOGGLE_OBSERVE_THUNK` (raw 0x591a6d0)
//     wraps a body that enqueues a `CObserveCommand`; constructing that
//     command + dispatching it via `GAME_COMMAND_DISPATCH` is the proper
//     invocation path.
//
// The handler returns structured JSON describing what it could and could not
// do; ck3ctl can use the `singleton_captured` flag to decide between the new
// path and falling back to `select_observe_default` (uinput/X click).
// =============================================================================

bool do_enter_observe_mode(const char* /*params_json*/, std::string* response_out) {
    void* lobby = ::ck3_bridge::get_lobby_view_singleton();
    bool captured = (lobby != nullptr);

    // Best-effort unpause: even without driving CloseGameView/ToggleObserve
    // directly, prodding the engine out of pause exercises the
    // GameCommandDispatch queue drain. For pinned-bookmark saves (per
    // ck-cxsk observation) this alone has been enough on occasion; for
    // others, the click is still required and ck3ctl will fall back.
    bool unpaused = false;
    if (captured) {
        game_unpause();
        unpaused = true;
        ADLOG("enter_observe_mode: singleton=%p, called game_unpause()\n", lobby);
    } else {
        ADLOG("enter_observe_mode: singleton not yet captured (lobby view "
              "constructor hook hasn't fired); caller should fall back\n");
    }

    if (response_out) {
        ResponseBuilder r;
        r.begin_object();
        r.key("action");              r.value_string("enter_observe_mode");
        // Treated as success when the singleton is captured AND we issued an
        // unpause. Even if downstream observer-mode transition still relies
        // on the GUI button click, the fact that the singleton is captured
        // is itself a useful state signal for the caller.
        r.key("success");             r.value_bool(captured);
        r.key("singleton_captured");  r.value_bool(captured);
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
        if (captured) {
            char hex[32];
            snprintf(hex, sizeof(hex), "0x%lx", reinterpret_cast<uintptr_t>(lobby));
            r.key("lobby_view_ptr");  r.value_string(hex);
        }
#endif
        r.key("unpaused");            r.value_bool(unpaused);
        if (!captured) {
            r.key("reason");          r.value_string("lobby_view_not_constructed");
            r.key("hint");             r.value_string(
                "The lobby view singleton is only captured once the engine "
                "opens the multiplayer-lobby UI. If the game hasn't reached "
                "that screen, the constructor hook hasn't fired. Caller "
                "should fall back to select_observe_default (uinput) until "
                "the singleton is captured.");
        }
        r.key("notes"); r.value_string(
            "ck-dq7ym.8 scaffold: programmatic CloseGameView/ToggleObserve "
            "invocation is still open RE — see SESSION-REPORT.md for the "
            "remaining gaps and follow-up beads.");
        r.end_object();
        *response_out = r.take();
    }
    return captured;
}

// =============================================================================
// do_dispatch_faction_intent — enqueue a FACTION create/join intent, drained on
// the game thread by execute_faction_intent. Mirrors do_call_war_create_core.
// params: actor_id, target_id (liege), faction_type (key), faction_action
//         ("create"|"join"|"leave"), optional title_id/claimant_id.
// =============================================================================
bool do_dispatch_faction_intent(const char* params_json, std::string* response_out) {
    int actor_raw  = json_get_int(params_json, "actor_id");
    int liege_raw  = json_get_int(params_json, "target_id");
    if (liege_raw <= 0) liege_raw = json_get_int(params_json, "liege_id");
    const char* ftype = json_get_string(params_json, "faction_type");
    const char* faction_action = json_get_string(params_json, "faction_action");
    if (!faction_action || !faction_action[0]) faction_action = "create";
    int title_raw    = json_get_int(params_json, "title_id");
    int claimant_raw = json_get_int(params_json, "claimant_id");

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("faction_action");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };

    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }
    if (liege_raw <= 0) { short_error("target_id_(liege)_required"); return false; }
    if (!ftype || !ftype[0]) { short_error("faction_type_required"); return false; }

    uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "faction_action", response_out)) return false;
    uint32_t liege_id = static_cast<uint32_t>(liege_raw) & 0xFFFFFF;

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "fac_%u_%lu_%u",
             actor_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::FACTION;
    intent.actor_id = actor_id;
    intent.target_id = liege_id;
    intent.claimant_id = (claimant_raw > 0) ? (static_cast<uint32_t>(claimant_raw) & 0xFFFFFF) : 0;
    intent.title_id = (title_raw > 0) ? title_raw : 0;
    intent.interaction_name = std::string(ftype);       // faction-type key
    intent.console_command = std::string(faction_action); // create/join/leave
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("faction_action: queued actor=%u liege=%u type='%s' action='%s' rid=%s\n",
          actor_id, liege_id, ftype, faction_action, request_id.c_str());

    bool found_result = false, intent_succeeded = false;
    std::string output_str;
    char result_reason[160] = {};
    for (int poll = 0; poll < 100; poll++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto completed = g_intent_queue.completed_snapshot(16);
        for (const auto& c : completed) {
            if (c.request_id == request_id) {
                found_result = true;
                intent_succeeded = c.executed && !c.rejected;
                output_str = c.console_output;
                if (c.rejected && c.rejection_reason[0])
                    strncpy(result_reason, c.rejection_reason, sizeof(result_reason) - 1);
                break;
            }
        }
        if (found_result) break;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("faction_action");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("liege_id"); r.value_uint(liege_id);
        r.key("faction_type"); r.value_string(ftype);
        r.key("faction_action"); r.value_string(faction_action);
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (!output_str.empty()) { r.key("result_raw"); r.value_string(output_str.c_str(), output_str.size()); }
            if (!intent_succeeded && result_reason[0]) { r.key("reason"); r.value_string(result_reason); }
        } else {
            r.key("reason"); r.value_string("intent_timeout_no_result");
        }
        r.end_object();
        *response_out = r.take();
    }
    return intent_succeeded;
}

// =============================================================================
// do_dispatch_scheme_intent (#132) — enqueue a SCHEME-start intent, drained on
// the game thread by execute_scheme_intent. Mirrors do_dispatch_faction_intent.
//
// LEGITIMACY: the executor calls the engine's own CREATE_ACTIVE_SCHEME forward,
// which runs the scheme type's is_shown/is_valid predicates + ValidateSchemeTarget
// + owner/target-alive + HasSchemeOnTarget (cooldown/dedupe) gates INTERNALLY and
// returns the SCHEME_NULL sentinel on any failure. We additionally reject the
// player actor, require a real target character, and reject self-targeting here.
//
// params: actor_id (owner, must be AI), scheme (type key, e.g. "murder"),
//         target_id (victim character).
// =============================================================================
bool do_dispatch_scheme_intent(const char* params_json, std::string* response_out) {
    int actor_raw  = json_get_int(params_json, "actor_id");
    int target_raw = json_get_int(params_json, "target_id");
    const char* scheme = json_get_string(params_json, "scheme");
    if (!scheme || !scheme[0]) scheme = json_get_string(params_json, "scheme_type");
    // lane-scheme 2026-10-18, measured live: the CANCEL branch was unreachable
    // over the wire. The handler receives the whole request
    //   {"action":"scheme_action","params":{...,"action":"cancel",...}}
    // and json_get_string looks at the TOP level first, so "action" always
    // read "scheme_action": every cancel fell into the start branch and was
    // refused target_id_required (3/3 MCP cancels). Read the sub-operation
    // from `params` first; a bare params object (no envelope) still works.
    bool is_cancel = false;
    {
        rapidjson::Document d;
        d.Parse<rapidjson::kParseIterativeFlag>(params_json ? params_json : "");
        if (!d.HasParseError() && d.IsObject()) {
            const rapidjson::Value* scope = &d;
            auto pit = d.FindMember("params");
            if (pit != d.MemberEnd() && pit->value.IsObject()) scope = &pit->value;
            auto ait = scope->FindMember("action");
            is_cancel = (ait != scope->MemberEnd() && ait->value.IsString() &&
                         strcmp(ait->value.GetString(), "cancel") == 0);
        }
    }

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("scheme_action");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };

    // lane-scheme 2026-10-18: PAUSED is refused up front, typed. The intent drains
    // only on a sim tick, so a paused call used to poll 5 s, answer
    // intent_timeout_no_result -- and leave the intent QUEUED, where its tick-
    // based TTL cannot age while paused: the scheme was then created on the first
    // tick after the caller had been told it failed. `game_paused` is the
    // spelling every other tier-3 write uses for this (ipc_server.cpp), and the
    // MCP gateway already answers it by unpausing and retrying.
    if (is_paused()) { short_error("game_paused"); return false; }

    // Bounded wait for this request's verdict, and the second half of the fix:
    // if none arrives, the intent is CANCELLED before we reply, so a reported
    // timeout can never execute later. Only when the cancel finds nothing to
    // cancel (the drain took it meanwhile) is the verdict re-read once more.
    auto await_scheme_result = [&](const std::string& rid, bool* ok_out,
                                   std::string* out_str, char* reason_buf,
                                   size_t reason_len) -> const char* {
        auto take = [&](const Intent& c) {
            *ok_out = c.executed && !c.rejected;
            *out_str = c.console_output;
            if (c.rejected && c.rejection_reason[0])
                snprintf(reason_buf, reason_len, "%s", c.rejection_reason);
        };
        Intent c;
        for (int poll = 0; poll < 100; poll++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (g_intent_queue.find_completed_by_request_id(rid, &c)) { take(c); return nullptr; }
        }
        // Retire, not erase (see IntentQueue::retire_pending_by_request_id). If it
        // was still pending, it will now never run -- unless the drain was already
        // inside it, in which case its own verdict overwrites ours; re-read once.
        const bool retired = g_intent_queue.retire_pending_by_request_id(
            rid, "intent_timeout_cancelled_not_executed");
        for (int poll = 0; poll < 40; poll++) {
            if (g_intent_queue.find_completed_by_request_id(rid, &c)) { take(c); return nullptr; }
            if (g_intent_queue.find_pending_by_request_id(rid, &c) && c.consumed &&
                strcmp(c.rejection_reason, "intent_timeout_cancelled_not_executed") != 0) {
                take(c); return nullptr;   // the drain finished it after the retire
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return retired ? "intent_timeout_cancelled_not_executed" : "intent_timeout_no_result";
    };

    // #scheme_cancel branch: abandon an owned scheme by scheme_id (from
    // query(domain="schemes")). Skips the start-only target/self/scheme-type checks.
    int cancel_scheme_id = 0;
    if (is_cancel) {
        if (actor_raw <= 0) { short_error("actor_id_required"); return false; }
        cancel_scheme_id = json_get_int(params_json, "scheme_id");
        if (cancel_scheme_id <= 0) { short_error("scheme_id_required"); return false; }
        uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
        if (reject_player_actor(actor_id, "scheme_action", response_out)) return false;

        uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
        char req_id_buf[64];
        snprintf(req_id_buf, sizeof(req_id_buf), "schx_%u_%lu_%u",
                 actor_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
        std::string request_id(req_id_buf);

        Intent intent = {};
        intent.type = IntentType::SCHEME;
        intent.actor_id = actor_id;
        intent.title_id = cancel_scheme_id;          // scheme id to cancel
        intent.console_command = "cancel";           // discriminator
        intent.queued_tick = current_tick;
        intent.ttl_ticks = 100;
        intent.request_id = request_id;
        g_intent_queue.enqueue(std::move(intent));
        ADLOG("scheme_action: queued CANCEL actor=%u scheme_id=%d rid=%s\n",
              actor_id, cancel_scheme_id, request_id.c_str());

        bool ok = false; std::string out_str; char reason_buf[160] = {};
        const char* timeout_reason = await_scheme_result(request_id, &ok, &out_str,
                                                         reason_buf, sizeof(reason_buf));
        const bool found = (timeout_reason == nullptr);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(ok);
            r.key("action"); r.value_string("cancel");
            r.key("actor_id"); r.value_uint(actor_id);
            r.key("scheme_id"); r.value_int(cancel_scheme_id);
            r.key("intent_request_id"); r.value_string(request_id.c_str());
            if (found) {
                if (!out_str.empty()) { r.key("result_raw"); r.value_string(out_str.c_str(), out_str.size()); }
                if (!ok && reason_buf[0]) { r.key("reason"); r.value_string(reason_buf); }
            } else {
                r.key("reason"); r.value_string(timeout_reason);
            }
            r.end_object();
            *response_out = r.take();
        }
        return ok;
    }

    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }
    if (target_raw <= 0) { short_error("target_id_required"); return false; }
    if (!scheme || !scheme[0]) { short_error("scheme_type_required"); return false; }

    uint32_t actor_id  = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    uint32_t target_id = static_cast<uint32_t>(target_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "scheme_action", response_out)) return false;
    if (actor_id == target_id) { short_error("cannot_scheme_against_self"); return false; }

    uint64_t current_tick = g_tick_id.load(std::memory_order_relaxed);
    char req_id_buf[64];
    snprintf(req_id_buf, sizeof(req_id_buf), "sch_%u_%lu_%u",
             actor_id, (unsigned long)current_tick, generate_rng() & 0xFFFF);
    std::string request_id(req_id_buf);

    Intent intent = {};
    intent.type = IntentType::SCHEME;
    intent.actor_id = actor_id;
    intent.target_id = target_id;
    intent.interaction_name = std::string(scheme);   // scheme-type key
    intent.console_command = "start";                // #scheme_cancel discriminator
    intent.queued_tick = current_tick;
    intent.ttl_ticks = 100;
    intent.request_id = request_id;
    g_intent_queue.enqueue(std::move(intent));

    ADLOG("scheme_action: queued actor=%u target=%u scheme='%s' rid=%s\n",
          actor_id, target_id, scheme, request_id.c_str());

    bool intent_succeeded = false;
    std::string output_str;
    char result_reason[160] = {};
    const char* timeout_reason = await_scheme_result(request_id, &intent_succeeded,
                                                     &output_str, result_reason,
                                                     sizeof(result_reason));
    const bool found_result = (timeout_reason == nullptr);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(intent_succeeded);
        r.key("action"); r.value_string("scheme_action");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("target_id"); r.value_uint(target_id);
        r.key("scheme"); r.value_string(scheme);
        r.key("intent_request_id"); r.value_string(request_id.c_str());
        r.key("dispatch_id"); r.value_string(request_id.c_str());
        if (found_result) {
            if (!output_str.empty()) { r.key("result_raw"); r.value_string(output_str.c_str(), output_str.size()); }
            if (!intent_succeeded && result_reason[0]) { r.key("reason"); r.value_string(result_reason); }
        } else {
            r.key("reason"); r.value_string(timeout_reason);
        }
        r.end_object();
        *response_out = r.take();
    }
    return intent_succeeded;
}

// =============================================================================
// do_move_capital — relocate the actor's realm capital to a county title.
// SYNCHRONOUS (mirrors do_set_council_task): CAPITAL_VALIDATE_APPLY for honest
// reject, then CAPITAL_RELOCATE under arm_fake_random_scope + watchdog + sigsetjmp.
// params: actor_id, title_id (the target COUNTY title). (re_heavy_re_synthesis.md.)
// =============================================================================
bool do_move_capital(const char* params_json, std::string* response_out) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    int actor_raw = json_get_int(params_json, "actor_id");
    int title_raw = json_get_int(params_json, "title_id");
    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("move_capital");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };
    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }
    if (title_raw <= 0) { short_error("title_id_required"); return false; }

    uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "move_capital", response_out)) return false;
    void* actor = lookup_character(actor_id);
    if (!actor) { short_error("actor_not_found"); return false; }
    if (g_char_is_alive && !g_char_is_alive(actor)) { short_error("actor_not_alive"); return false; }

    uintptr_t county_addr = lookup_title_obj(static_cast<uint32_t>(title_raw) & 0xFFFFFF);
    if (!county_addr) { short_error("county_title_not_found"); return false; }
    void* county = reinterpret_cast<void*>(county_addr);

    auto validate = reinterpret_cast<bool(*)(void*, void*, int, long)>(
        g_base_addr + ck3::funcs::CAPITAL_VALIDATE_APPLY);
    auto relocate = reinterpret_cast<unsigned long(*)(void*, void*, long)>(
        g_base_addr + ck3::funcs::CAPITAL_RELOCATE);

    bool valid = false, relocated = false;
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("move_capital");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        short_error((sig == 1) ? "crashed_SIGSEGV" : (sig == 2) ? "crashed_SIGALRM" : "crashed");
        return false;
    }
    valid = validate(actor, county, 0, 0);
    if (valid) {
        relocate(actor, county, 0);
        relocated = true;
    }
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(relocated);
        r.key("action"); r.value_string("move_capital");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("title_id"); r.value_uint(static_cast<uint32_t>(title_raw) & 0xFFFFFF);
        r.key("validated"); r.value_bool(valid);
        if (!relocated) r.key("reason"); if (!relocated) r.value_string("capital_validate_failed");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("move_capital: actor=%u county=%u validated=%d relocated=%d\n",
          actor_id, static_cast<uint32_t>(title_raw) & 0xFFFFFF, valid ? 1 : 0, relocated ? 1 : 0);
    return relocated;
}

// =============================================================================
// do_set_obligation_level — set a vassal's contract obligation level by name.
// PURE GUARDED MEMORY WRITE (no game-fn call, no RNG): resolve the liege's
// contract whose SUBJECT==vassal, match the obligation type by key name, write
// the uint8 level byte. (re_modify_contract_move_capital.md / re_vassal_contract.md.)
// params: actor_id (liege), vassal_id, obligation (type key), level (0..N).
// =============================================================================
bool do_set_obligation_level(const char* params_json, std::string* response_out) {
    int actor_raw  = json_get_int(params_json, "actor_id");
    int vassal_raw = json_get_int(params_json, "vassal_id");
    const char* obligation = json_get_string(params_json, "obligation");
    int level = json_get_int(params_json, "level");

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("set_obligation_level");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };
    if (actor_raw <= 0)  { short_error("actor_id_required"); return false; }
    if (vassal_raw <= 0) { short_error("vassal_id_required"); return false; }
    if (!obligation || !obligation[0]) { short_error("obligation_required"); return false; }
    if (level < 0 || level > 16) { short_error("level_out_of_range"); return false; }

    uint32_t actor_id  = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "set_obligation_level", response_out)) return false;
    uint32_t vassal_id = static_cast<uint32_t>(vassal_raw) & 0xFFFFFF;

    void* actor = lookup_character(actor_id);
    if (!actor) { short_error("actor_not_found"); return false; }
    uintptr_t tr = resolve_title_realm(reinterpret_cast<uintptr_t>(actor));
    if (!tr || !is_mapped(tr)) { short_error("actor_no_realm"); return false; }

    // Liege's vassal-contract array (PDX: data@+0, count@+0xc) of int32 ref ids.
    uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::VASSAL_CONTRACT_ARRAY);
    int32_t cnt   = safe_read<int32_t>(tr + ck3::title_realm::VASSAL_CONTRACT_ARRAY + 0xC);
    if (!arr || !is_mapped(arr) || cnt <= 0 || cnt > 4096) { short_error("no_vassal_contracts"); return false; }

    uintptr_t contract = 0;
    for (int32_t i = 0; i < cnt; i++) {
        uint32_t ref = safe_read<uint32_t>(arr + static_cast<uintptr_t>(i) * 4);
        if (!ref) continue;
        uintptr_t c = ad_db_lookup(ck3::globals::VASSAL_CONTRACT_DB, ref);
        if (!c || !is_mapped(c)) continue;
        uint32_t subj = safe_read<uint32_t>(c + subject_contract::SUBJECT) & 0xFFFFFF;
        if (subj == vassal_id) { contract = c; break; }
    }
    if (!contract) { short_error("no_contract_with_that_vassal"); return false; }

    // Match obligation type by key name.
    uintptr_t types = safe_read<uintptr_t>(contract + subject_contract::OBL_TYPES_PTR);
    int32_t tcount  = safe_read<int32_t>(contract + subject_contract::OBL_TYPES_COUNT);
    int32_t lcount  = safe_read<int32_t>(contract + subject_contract::OBL_LEVELS_COUNT);
    uintptr_t levels = safe_read<uintptr_t>(contract + subject_contract::OBL_LEVELS_PTR);
    if (!types || !is_mapped(types) || tcount <= 0 || tcount > 64 ||
        !levels || !is_mapped(levels)) { short_error("contract_layout_invalid"); return false; }

    int idx = -1;
    std::string available;
    for (int32_t i = 0; i < tcount; i++) {
        uintptr_t type_ptr = safe_read<uintptr_t>(types + static_cast<uintptr_t>(i) * 8);
        if (!type_ptr || !is_mapped(type_ptr)) continue;
        std::string key = ad_read_std_string(type_ptr + subject_contract_type::KEY_NAME_PTR);
        if (!available.empty()) available += ",";
        available += key;
        if (key == obligation) { idx = i; break; }
    }
    if (idx < 0) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("set_obligation_level");
        r.key("reason"); r.value_string("obligation_not_in_contract");
        r.key("available_obligations"); r.value_string(available.c_str(), available.size());
        r.end_object();
        if (response_out) *response_out = r.take();
        return false;
    }
    if (idx >= lcount) { short_error("level_index_out_of_range"); return false; }

    // Direct guarded uint8 write — the obligation-level byte.
    uintptr_t level_addr = levels + static_cast<uintptr_t>(idx);
    if (!is_mapped(level_addr)) { short_error("level_addr_unmapped"); return false; }
    uint8_t old_level = *reinterpret_cast<volatile uint8_t*>(level_addr);

    // LEGITIMACY GATE (re_write_action_legitimacy_audit.md §3.3): this is a raw byte
    // write that bypasses CVassalContractChange::Execute — so RAISING obligations would
    // grant extra tax/levy with NO vassal-acceptance, NO opinion penalty, NO cooldown
    // (a cheat). Only allow LOWERING (vassal-favorable, consequence-free). Raising must
    // go through the real contract-change command (deferred — needs the Execute path +
    // the "Increased Obligations" opinion modifier; see audit §4).
    if (static_cast<uint8_t>(level) > old_level &&
        !json_get_bool(params_json, "allow_uncosted", false)) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("set_obligation_level");
        r.key("reason"); r.value_string("raise_requires_negotiation");
        r.key("old_level"); r.value_int(old_level);
        r.key("requested_level"); r.value_int(level);
        r.key("hint"); r.value_string("raising obligations bypasses vassal consent + opinion penalty (illegitimate); only lowering is allowed via this path");
        r.end_object();
        if (response_out) *response_out = r.take();
        return false;
    }

    *reinterpret_cast<volatile uint8_t*>(level_addr) = static_cast<uint8_t>(level);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("set_obligation_level");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("vassal_id"); r.value_uint(vassal_id);
        r.key("obligation"); r.value_string(obligation);
        r.key("old_level"); r.value_int(old_level);
        r.key("new_level"); r.value_int(level);
        r.key("note"); r.value_string("direct write; verify persistence next cycle (CONTRACT_MGR_TICK may re-eval)");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("set_obligation_level: actor=%u vassal=%u obl='%s' %u->%d\n",
          actor_id, vassal_id, obligation, old_level, level);
    return true;
}

// =============================================================================
// do_appoint_councillor — seat a character in a council position (L29 fix; the
// vacant-chaplain-seat blocker for fabrication). SYNCHRONOUS, clones the
// do_set_council_task council walk + crash-net, then calls the assign-councillor
// effect Execute. Safe at MEDIUM Execute-confidence: a bad call is caught by the
// sigsetjmp net and rejected, never wedges. (re_appoint_councillor.md.)
// params: actor_id (AI ruler), position_id (the idx query_council reports),
//         character_id (courtier to seat), replace (bool, default false).
//
// VACANT-SEAT FILL (#79): default-OFF behind g_enable_vacant_fill until ONE live
// readback confirms (re §7). The fill path calls COUNCIL_SEAT_ASSIGN with the
// candidate's FULL 32-bit handle (char+0x18, incl. the generation byte) — passing
// a bare 24-bit id was the #62/#79 crash (worker FUN_068b4180 validates
// *(cand+0x18)==arg → 24-bit mismatch → null-sentinel→displacement-vcall SIGSEGV).
// The fill path has NO COUNCIL_AI_RECOMPUTE, so it is hang-safe and the sigsetjmp
// watchdog can actually bound it. NOT for the chaplain (4746f75: rarely truly
// vacant — that was a read bug); this is for genuinely-empty seats (open
// spymaster/marshal after a holder died).
// =============================================================================
std::atomic<bool> g_enable_vacant_fill{false};  // #79: armed only after the live confirm

bool do_appoint_councillor(const char* params_json, std::string* response_out) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    int actor_raw = json_get_int(params_json, "actor_id");
    int pos_raw   = json_get_int(params_json, "position_id");
    int char_raw  = json_get_int(params_json, "character_id");
    int replace   = json_get_int(params_json, "replace");

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("appoint_councillor");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };
    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }
    if (pos_raw <= 0)   { short_error("position_id_required"); return false; }
    if (char_raw <= 0)  { short_error("character_id_required"); return false; }

    uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "appoint_councillor", response_out)) return false;
    uint32_t position_id = static_cast<uint32_t>(pos_raw) & 0xFFFFFF;
    uint32_t character_id = static_cast<uint32_t>(char_raw) & 0xFFFFFF;
    void* actor = lookup_character(actor_id);
    if (!actor) { short_error("actor_not_found"); return false; }
    if (g_char_is_alive && !g_char_is_alive(actor)) { short_error("actor_not_alive"); return false; }

    // Resolve the position-instance object (same chase as do_set_council_task),
    // matching the seat by position id (24-bit).
    constexpr size_t COUNCIL_POS_ARRAY = 0x230, COUNCIL_POS_COUNT = 0x23c;
    constexpr size_t POS_HOLDER = 0x30;  // ASSIGN_COUNCILLOR holder char id (vacant=0/0xFFFFFF)
    uintptr_t pos_obj = 0;
    uintptr_t council = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(actor) + 0x1b8);
    uintptr_t posdb = safe_read_offset<uintptr_t>(ck3::globals::COUNCIL_POSITION_DB);
    if (council && is_mapped(council) && posdb && is_mapped(posdb)) {
        uintptr_t arr = safe_read<uintptr_t>(council + COUNCIL_POS_ARRAY);
        uint32_t  cnt = safe_read<uint32_t>(council + COUNCIL_POS_COUNT);
        uintptr_t pdb_arr = safe_read<uintptr_t>(posdb + ck3::hash_map::TABLE_PTR);
        uint32_t  pdb_cap = safe_read<uint32_t>(posdb + ck3::hash_map::CAPACITY);
        if (arr && is_mapped(arr) && pdb_arr && cnt > 0 && cnt < 64) {
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t pid = safe_read<uint32_t>(arr + static_cast<uintptr_t>(i) * 4);
                if ((pid & 0xffffff) != position_id) continue;
                uint32_t idx = pid & 0xffffff;
                if (idx >= pdb_cap) break;
                uintptr_t entry = safe_read<uintptr_t>(pdb_arr + ck3::hash_map::ENTRY_HEADER_SIZE
                                  + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
                if (entry && is_mapped(entry)
                    && safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) == pid) {
                    pos_obj = entry; break;
                }
            }
        }
    }
    if (!pos_obj) { short_error("position_not_found"); return false; }

    uint32_t holder = safe_read<uint32_t>(pos_obj + POS_HOLDER) & 0xFFFFFF;
    bool occupied = (holder != 0 && holder != 0xFFFFFF);
    if (occupied && !replace) { short_error("seat_occupied_pass_replace"); return false; }

    // #62 FIX v2 (2026-06-19, re_resolution_appoint.md — HIGH, verified): TWO bugs fixed.
    // (1) SWAP_COUNCIL_RAW_LEAF was stored as an IMAGE VA (0x068b4d10) → g_base_addr+that
    // resolved +0x310 into a logging fn → the crash. Header corrected to 0x067b4d10
    // (image-0x100000). (2) The leaf takes 4 args, not 3: FUN_068b4d10(council_instance_ptr,
    // owner_char_ptr, u32 char_a_id /*courtier to seat*/, u32 char_b_id /*current holder*/) —
    // confirmed byte-for-byte by the engine wrapper FUN_067e2ba0 which calls
    // leaf(*(owner+0x1b8), owner, a, b). It is recompute-FREE (does the membership swap +
    // council-changed event, NOT the AI recompute) — safe under the existing harness.
    // It is a strict TWO-OCCUPIED swap (precond rejects a vacant seat), so VACANT-fill is
    // gated below (the vacant-fill effect is statically unpinnable). `council` is the
    // ruler's council instance (= actor+0x1b8), already resolved above for pos_obj.
    if (!council || !is_mapped(council)) { short_error("no_council_instance"); return false; }

    // ── VACANT-SEAT FILL: still GATED — none of the candidate paths is safe+working yet
    //    (re_council_vacant_fill_status.md, live-tested 2026-06-19, year ~905):
    //    • COUNCIL_SEAT_ASSIGN (FUN_067e2b40→FUN_068b4180, council-instance based) is the
    //      CORRECT structure but its DISPLACEMENT block CRASHES on a landed candidate and
    //      HANGS (froze the game, watchdog couldn't bound it) moving an existing councillor.
    //    • COUNCIL_SEAT_HOLDER_SET (FUN_05cc9d90, pos_obj based) ran clean (no crash/hang) but
    //      DID NOT fill the seat (applied=false): do_appoint's pos_obj is the COUNCIL_POSITION_DB
    //      entry, but the live holder query (council_instance_walk) reads a DIFFERENT object →
    //      writing the DB entry doesn't update the seat the game/query see.
    //    REMAINING WORK: resolve the council-INSTANCE position object (council=actor+0x1b8,
    //    array @+0x230) and either find its instance holder-set, OR drive COUNCIL_SEAT_ASSIGN
    //    with run_validation/oracle (param_6=0) + a FRESH ELIGIBLE courtier (untested — needs a
    //    court-cleric candidate source) and confirm the sentinel path doesn't crash/hang.
    //    Occupied-seat SWAP works (below).
    //    #79 (re_council_vacant_fill_status.md, calling-convention root-cause): the
    //    prior crash was NOT an impossible engine path — COUNCIL_SEAT_ASSIGN's worker
    //    FUN_068b4180 validates the candidate via *(cand+0x18)==arg, and the bridge had
    //    passed a bare 24-bit id where the engine passes the FULL 32-bit handle (the
    //    +0x18 word carries a non-zero generation byte in bits 24-31). The mismatch
    //    drove the null-sentinel→displacement-vcall SIGSEGV. Fix: read the candidate's
    //    FULL handle and pass THAT. Default-OFF behind g_enable_vacant_fill until one
    //    live readback confirms (re §7). Fill path is hang-safe (no COUNCIL_AI_RECOMPUTE).
    if (!occupied) {
        if (!g_enable_vacant_fill.load(std::memory_order_relaxed)) {
            short_error("vacant_fill_disabled:set_vacant_fill_enabled_first");
            return false;
        }
        // Resolve the candidate and read its FULL 32-bit handle (NOT masked).
        void* cand = lookup_character(character_id);
        if (!cand) { short_error("candidate_not_found"); return false; }
        if (g_char_is_alive && !g_char_is_alive(cand)) { short_error("candidate_not_alive"); return false; }
        uint32_t cand_handle = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(cand) + ck3::character::ID);
        if ((cand_handle & 0xFFFFFF) != character_id) {
            short_error("candidate_handle_mismatch"); return false;
        }

        auto assign = reinterpret_cast<void(*)(void*, uint32_t, uint32_t, char, char)>(
            g_base_addr + ck3::funcs::COUNCIL_SEAT_ASSIGN);

        // ck-zgh4 scope latch: a restore was refused earlier in this
        // process, so the slot cannot be reasoned about. REFUSE — do not
        // proceed unarmed, which is what ignoring the arm's false would do.
        if (bridge::scope_latched()) {
            bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
            if (response_out) *response_out = bridge::scope_latched_refusal_json("appoint_councillor");
            return false;
        }
        watchdog_arm(4);
        g_in_protected_region = true;
        int fsig = sigsetjmp(g_recovery_point, 1);
        if (fsig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            short_error("vacant_fill_crashed");
            ADLOG("appoint_councillor[vacant_fill]: actor=%u pos=%u char=%u handle=0x%x — CRASHED (caught)\n",
                  actor_id, position_id, character_id, cand_handle);
            return false;
        }
        uint64_t f_saved_rng = 0; bool f_rng_armed = arm_fake_random_scope(&f_saved_rng);

        // FULL handle in arg2 (the #79 fix); run_validation=0 (run oracle), skip_eligibility=0.
        assign(actor, cand_handle, position_id, 0, 0);

        if (f_rng_armed) restore_fake_random_scope(f_saved_rng);
        g_in_protected_region = false; watchdog_disarm();

        // Authoritative, phase-independent readback: pos_obj+0x38 (COUNCIL_POS_COUNCILLOR_ID),
        // NOT pos_obj+0x30.
        uint32_t now = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_COUNCILLOR_ID) & 0xFFFFFF;
        bool fill_applied = (now == character_id);

        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(fill_applied);
            r.key("action"); r.value_string("appoint_councillor");
            r.key("mode"); r.value_string("vacant_fill");
            r.key("actor_id"); r.value_uint(actor_id);
            r.key("position_id"); r.value_uint(position_id);
            r.key("character_id"); r.value_uint(character_id);
            r.key("applied"); r.value_bool(fill_applied);
            r.key("new_holder"); r.value_uint(now);
            if (!fill_applied) r.key("reason"), r.value_string("seat_holder_unchanged");
            r.end_object();
            *response_out = r.take();
        }
        ADLOG("appoint_councillor[vacant_fill]: actor=%u pos=%u char=%u handle=0x%x new_holder=%u applied=%d\n",
              actor_id, position_id, character_id, cand_handle, now, fill_applied ? 1 : 0);
        return fill_applied;
    }

    // ── OCCUPIED/replace branch (swap-leaf path, unchanged) ──
    auto swap_leaf = reinterpret_cast<void(*)(void*, void*, uint32_t, uint32_t)>(
        g_base_addr + ck3::funcs::SWAP_COUNCIL_RAW_LEAF);

    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("appoint_councillor");
        return false;
    }
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false; watchdog_disarm();
        short_error("appoint_crashed");
        ADLOG("appoint_councillor: actor=%u pos=%u char=%u — CRASHED (caught)\n",
              actor_id, position_id, character_id);
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);

    // 4-arg: (council_instance, owner_char_ptr, char_a=courtier to seat, char_b=current holder).
    // The two characters trade seats (char_a takes char_b's seat).
    swap_leaf(reinterpret_cast<void*>(council), actor, character_id, holder);

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    uint32_t new_holder = safe_read<uint32_t>(pos_obj + POS_HOLDER) & 0xFFFFFF;
    bool applied = (new_holder == character_id);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(applied);
        r.key("action"); r.value_string("appoint_councillor");
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("position_id"); r.value_uint(position_id);
        r.key("character_id"); r.value_uint(character_id);
        r.key("applied"); r.value_bool(applied);
        r.key("new_holder"); r.value_uint(new_holder);
        if (!applied) r.key("reason"), r.value_string("seat_holder_unchanged");
        r.key("note"); r.value_string(
            "raw recompute-free seat swap called; vacant-seat char_a/char_b encoding is "
            "UNCONFIRMED — needs live write-test confirmation. See task #62.");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("appoint_councillor: actor=%u pos=%u char=%u prev_holder=%u new_holder=%u applied=%d (raw swap leaf)\n",
          actor_id, position_id, character_id, holder, new_holder, applied ? 1 : 0);
    return applied;
}

// IPC setter — set_vacant_fill_enabled {enabled:bool}. Arms/disarms the default-OFF
// #79 g_enable_vacant_fill flag gating the vacant-seat fill path in
// do_appoint_councillor. Bridge-internal RE lever; stays OFF until the §7 live confirm.
bool do_set_vacant_fill_enabled(const char* params_json, std::string* out) {
    bool enabled = json_get_bool(params_json, "enabled", false);
    g_enable_vacant_fill.store(enabled, std::memory_order_relaxed);
    ADLOG("set_vacant_fill_enabled: %s\n", enabled ? "ENABLED" : "disabled");
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action");  r.value_string("set_vacant_fill_enabled");
        r.key("enabled"); r.value_bool(enabled);
        r.end_object();
        *out = r.take();
    }
    return true;
}

// Resolve a CMenAtArmsType* by key name from MAA_TYPE_DB (mirror resolve_building_obj).
static uintptr_t resolve_maa_type_obj(const std::string& want) {
    uintptr_t db  = safe_read_offset<uintptr_t>(ck3::globals::MAA_TYPE_DB);
    uintptr_t arr = db ? safe_read<uintptr_t>(db + ck3::globals::MAA_TYPE_DB_ARR_OFF) : 0;
    uint32_t  cnt = db ? safe_read<uint32_t>(db + ck3::globals::MAA_TYPE_DB_CNT_OFF) : 0;
    if (!arr || !is_mapped(arr) || cnt == 0 || cnt > 5000) return 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        uintptr_t e = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (!e || !is_mapped(e)) continue;
        if (ad_read_std_string(e + 0x10) == want) return e;
    }
    return 0;
}

// =============================================================================
// do_maa_recruit — recruit a men-at-arms regiment of a chosen type. Command-
// dispatch path, near-clone of do_add_construction (mode 0xe). The doomstack
// engine's first step. (re_maa_write_path.md — RECRUIT, HIGH.)
// params: actor_id (AI ruler), maa_type (type key, e.g. "huscarl").
// =============================================================================
bool do_maa_recruit(const char* params_json, std::string* response_out) {
    const char* mt = json_get_string(params_json, "maa_type");
    if (!mt || !mt[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"maa_recruit","reason":"maa_type_required"})";
        return false;
    }
    std::string want(mt);

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "maa_recruit", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"maa_recruit","reason":"no_living_actor"})";
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + 0x18);  // FULL engine id

    uintptr_t maa_type_obj = resolve_maa_type_obj(want);
    if (!maa_type_obj) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("maa_recruit");
            r.key("reason"); r.value_string("maa_type_not_found");
            r.key("attempted"); r.value_string(want.c_str(), want.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // ── AFFORDABILITY GATE (legitimacy; pure-memory, zero engine calls) ──
    // Execute charges the upfront recruit cost via FUN_06f07fa0 regardless of treasury,
    // so without this gate a recruit can drive gold negative = a cheat-class write. Mirror
    // the #133 build_gate: read ruler gold (REALM_DATA+GOLD, int64 fp/100000) and the MaA
    // type's buy_cost (::maa_type::RECRUIT_GOLD_COST +0x68, int64 fp/100000). Refuse ONLY when
    // the cost is known (>0) and unaffordable; a 0 cost read = unknown → stay permissive.
    // Base buy_cost is the dominant legitimacy floor; terrain/innovation modifiers shift the
    // real price but the engine still validates server-side on the tick.
    {
        uintptr_t core = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::REALM_DATA);
        int64_t gold_fp = (core && is_mapped(core)) ? safe_read<int64_t>(core + ck3::core_data::GOLD) : 0;
        int64_t cost_fp = safe_read<int64_t>(maa_type_obj + ::maa_type::RECRUIT_GOLD_COST);
        if (cost_fp > 0 && gold_fp < cost_fp) {
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("maa_recruit");
                r.key("reason"); r.value_string("insufficient_gold");
                r.key("maa_type"); r.value_string(want.c_str(), want.size());
                r.key("cost"); r.value_float(static_cast<double>(cost_fp) / 100000.0);
                r.key("gold"); r.value_float(static_cast<double>(gold_fp) / 100000.0);
                r.end_object();
                *response_out = r.take();
            }
            ADLOG("maa_recruit: actor=0x%x type='%s' REFUSED insufficient_gold (cost=%.2f gold=%.2f)\n",
                  actor_id, want.c_str(), static_cast<double>(cost_fp) / 100000.0,
                  static_cast<double>(gold_fp) / 100000.0);
            return false;
        }
    }

    // #62 FIX v2 (2026-06-19, re_create_maa_regiment_command.md): the bare effect-leaf
    // 0x062645c0 is DISPROVEN (hangs/crashes — it needs a DB-RESOLVED owner `this` that
    // only the command Execute supplies). The PROPER apply path is the full
    // CCreateMAARegimentCommand: ctor → Execute → base_dtor, exactly like
    // do_hire_mercenary. Execute resolves owner char + MaA type from the DBs (sentinel
    // fallback) then calls the create-leaf with a valid this — applying the recruit +
    // gold charge. Drive it on the GAME thread (this handler is is_game_thread_action)
    // under the proven arm_fake_random_scope + watchdog + sigsetjmp harness.
    //   ctor(cmd, actor_id, char_id, maa_type_obj, origin=0xFFFFFFFF, flag=1)
    //   Execute(cmd)  [PRIMARY this]   base_dtor(cmd)
    auto ctor = reinterpret_cast<void(*)(void*, uint32_t, uint32_t, void*, uint32_t, uint8_t)>(
        g_base_addr + ck3::funcs::CREATE_MAA_CMD_CTOR);
    auto execute = reinterpret_cast<void(*)(void*)>(
        g_base_addr + ck3::funcs::CREATE_MAA_CMD_EXECUTE);
    auto base_dtor = reinterpret_cast<void(*)(void*)>(
        g_base_addr + ck3::funcs::CREATE_MAA_CMD_BASE_DTOR);

    uint8_t cmd[0x40]; memset(cmd, 0, sizeof(cmd));   // struct >=0x38; 0x40 buffer is safe

    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("maa_recruit");
        return false;
    }
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false; watchdog_disarm();
        ADLOG("maa_recruit: recovery-longjmp sig=%d (Execute faulted)\n", sig);
        if (response_out) *response_out = R"({"success":false,"action":"maa_recruit","reason":"apply_crashed"})";
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);

    ctor(cmd, actor_id & 0xFFFFFF, actor_id & 0xFFFFFF,
         reinterpret_cast<void*>(maa_type_obj), 0xFFFFFFFF /*origin/count sentinel*/, 1 /*charge gold*/);
    execute(cmd);
    base_dtor(cmd);

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    // No cheap regiment-count readback here; report applied + a note to confirm via
    // query (Execute either applies or faults → caught by sigsetjmp above).
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("maa_recruit");
        r.key("applied"); r.value_bool(true);
        r.key("maa_type"); r.value_string(want.c_str(), want.size());
        r.key("note"); r.value_string("CCreateMAARegimentCommand ctor+Execute driven (resolves owner+type, creates+attaches regiment, charges gold); confirm via query(domain='armies')");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("maa_recruit: actor=0x%x type='%s' APPLIED (command Execute)\n", actor_id, want.c_str());
    return true;
}

// Ownership gate for the MaA station/unstation leaves (legitimacy; pure-memory, no
// engine calls). The Execute leaves accept ANY regiment_id, so without this an actor
// could re-station a regiment it does not own. Mirror the live-verified query path
// (query_handlers.cpp ~15247): the actor's MaA regiment refs live on TITLE_REALM_DATA
// in four CPdxArray<uint32> ref arrays (personal/title/mercenary/holy_order; data at
// +0x00, count at +0x0C). Returns true iff full_ref (masked low-24) is present in one
// of them. Bounded: count clamped to (0,500]; bad/empty arrays → not-owned.
static bool actor_owns_regiment(void* actor, uint32_t full_ref) {
    uintptr_t tr = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::TITLE_REALM_DATA);
    if (!tr || !is_mapped(tr)) return false;
    const uint32_t want = full_ref & 0xFFFFFF;
    const size_t sources[] = {
        ck3::title_realm::PERSONAL_MAA_PTR,
        ck3::title_realm::TITLE_MAA_REG_PTR,
        ck3::title_realm::MERC_REGIMENT_PTR,
        ck3::title_realm::HOLY_ORDER_REG_PTR,
    };
    for (size_t off : sources) {
        uintptr_t arr = safe_read<uintptr_t>(tr + off);
        int32_t cnt = safe_read<int32_t>(tr + off + 0x0C);
        if (cnt <= 0 || cnt > 500 || !arr || !is_mapped(arr)) continue;
        for (int32_t i = 0; i < cnt; ++i) {
            uint32_t ref = safe_read<uint32_t>(arr + i * 4);
            if ((ref & 0xFFFFFF) == want) return true;
        }
    }
    return false;
}

// =============================================================================
// do_maa_station / do_maa_unstation — set or clear a MaA regiment's origin
// province (the "station in a bonus holding" lever). Synchronous game-thread
// command-dispatch, near-clone of do_maa_recruit but FLAGS 0x7 (not 0xe) and a
// 2-arg / 1-arg ctor. (re_maa_station_ctor.md — HIGH.)
//   station:   params actor_id (AI ruler), regiment_id, location_id (province/holding)
//   unstation: params actor_id (AI ruler), regiment_id
// =============================================================================
static bool maa_origin_dispatch(const char* params_json, std::string* response_out,
                                bool station) {
    const char* action_label = station ? "maa_station" : "maa_unstation";

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, action_label, response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string(action_label);
            r.key("reason"); r.value_string("no_living_actor");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    int rid = json_get_int(params_json, "regiment_id");
    int lid = json_get_int(params_json, "location_id");
    uint32_t regiment_id = rid > 0 ? static_cast<uint32_t>(rid) : 0;
    uint32_t location_id = lid > 0 ? static_cast<uint32_t>(lid) : 0;
    if (regiment_id == 0 || (station && location_id == 0)) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string(action_label);
            r.key("reason"); r.value_string(station ? "regiment_id_and_location_id_required"
                                                     : "regiment_id_required");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // #62 FIX v3 (2026-06-19): the leaf validates regobj+0x10 == cmd+0x20 against the FULL
    // regiment ref (with generation/high bits), but the roster/agent passes the masked
    // low-24 id → validity fails → silent no-op. Reconstruct the full ref from the masked id.
    {
        uintptr_t regobj = ad_db_lookup(ck3::globals::REGIMENT_DB, regiment_id);
        if (regobj && is_mapped(regobj)) {
            uint32_t full = safe_read<uint32_t>(regobj + 0x10);
            if ((full & 0xFFFFFF) == (regiment_id & 0xFFFFFF)) regiment_id = full;  // full ref incl. high bits
        }
    }

    // ── OWNERSHIP GATE (legitimacy; pure-memory, zero engine calls) ──
    // The Execute leaf reads cmd+0x20 and re-stations whatever regiment_id it is given —
    // no ownership check. Without this an actor could re-station a regiment it does not
    // own = a cheat-class write. Refuse when the (reconstructed full) regiment ref is NOT
    // in any of the actor's MaA regiment ref arrays. This is the dominant legitimacy floor;
    // origin-province legality is still validated engine-side on the tick.
    if (!actor_owns_regiment(actor, regiment_id)) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string(action_label);
            r.key("reason"); r.value_string("regiment_not_owned");
            r.key("regiment_id"); r.value_int(static_cast<int64_t>(regiment_id & 0xFFFFFF));
            r.end_object();
            *response_out = r.take();
        }
        ADLOG("%s: regid=0x%x REFUSED regiment_not_owned by actor\n", action_label, regiment_id);
        return false;
    }

    // #62 FIX v2 (2026-06-19, re_resolution_maa_station.md): the Set/Remove ctors
    // (0x065981a0/0x06599020) + CanExec (0x06598dd0) were MID-FUNCTION hashmap helpers
    // (calling them as ctors corrupted memory). The Execute leaves read ONLY scalar cmd fields
    // and never deref the cmd vtable — so a memset-zeroed cmd[0x40] with the two id fields is a
    // complete, correct input; NO ctor, NO dtor. station leaf reads cmd+0x20=regiment_id (FULL
    // ref), cmd+0x24=origin PROVINCE INDEX (1-based, via FUN_0632a6a0 province array — NOT a
    // barony/title id); unstation reads ONLY cmd+0x20. Origin is written to regiment+0x118
    // (province ptr) by FUN_0635ba00; unstation sets it to the province sentinel.
    // Callee tree is bounded (array index + field setters, no loops/mutex/RNG-wedge) → safe.
    auto station_leaf = reinterpret_cast<void(*)(void*)>(
        g_base_addr + ck3::funcs::MAA_STATION_APPLY_LEAF);
    auto unstation_leaf = reinterpret_cast<void(*)(void*)>(
        g_base_addr + ck3::funcs::MAA_UNSTATION_APPLY_LEAF);

    uint8_t cmd[0x40]; memset(cmd, 0, sizeof(cmd));
    *reinterpret_cast<uint32_t*>(cmd + 0x20) = regiment_id;
    if (station) *reinterpret_cast<uint32_t*>(cmd + 0x24) = location_id;

    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json(station ? "maa_station" : "maa_unstation");
        return false;
    }
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false; watchdog_disarm();
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string(action_label);
            r.key("reason"); r.value_string("apply_crashed");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);

    // DIAG: read the regiment's origin field (regiment+0x118) before/after the leaf,
    // from the game thread, to definitively confirm apply.
    uintptr_t _diag_reg = ad_db_lookup(ck3::globals::REGIMENT_DB, regiment_id);
    uint64_t _o_before = (_diag_reg && is_mapped(_diag_reg)) ? safe_read<uint64_t>(_diag_reg + 0x118) : 0;

    if (station) station_leaf(cmd);   // reads cmd+0x20 regiment, cmd+0x24 origin
    else         unstation_leaf(cmd); // reads cmd+0x20 regiment only

    uint64_t _o_after = (_diag_reg && is_mapped(_diag_reg)) ? safe_read<uint64_t>(_diag_reg + 0x118) : 0;
    ADLOG("%s: regid=0x%x diag_reg=0x%lx origin+0x118 %lx -> %lx (%s)\n",
          action_label, regiment_id, (unsigned long)_diag_reg,
          (unsigned long)_o_before, (unsigned long)_o_after,
          _o_before != _o_after ? "CHANGED" : "unchanged");

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string(action_label);
        r.key("applied"); r.value_bool(true);
        r.key("regiment_id"); r.value_int(static_cast<int64_t>(regiment_id));
        if (station) { r.key("location_id"); r.value_int(static_cast<int64_t>(location_id)); }
        r.key("note"); r.value_string("origin-province Execute leaf called directly; poll query(domain='levy_readiness') to confirm the regiment's stationed_title");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("%s: regiment=0x%x location=0x%x APPLIED (direct execute leaf)\n", action_label, regiment_id, location_id);
    return true;
}

bool do_maa_station(const char* params_json, std::string* response_out) {
    return maa_origin_dispatch(params_json, response_out, true);
}
bool do_maa_unstation(const char* params_json, std::string* response_out) {
    return maa_origin_dispatch(params_json, response_out, false);
}

// =============================================================================
// do_hire_mercenary — hire a mercenary company. Synchronous game-thread command-
// dispatch, clone of do_maa_recruit. CHireMercenaryCompanyCommand:
//   ctor(cmd, hirer_char_id, company_id, contract_len) → prepare(vt[9]) →
//   dispatch(mgr, &out, 0x7, 0).  (re_hire_mercenary_wiring.md — HIGH.)
//   Execute calls the RNG-mix cost fn, so the fake RNG scope IS required.
//   company_id comes from discover(hireable_mercenaries). NEEDS LIVE VALIDATION
//   (Execute tail-mutates state; first run should be watched).
// params: actor_id (AI ruler), company_id, optional contract_len (default 1).
// =============================================================================
bool do_hire_mercenary(const char* params_json, std::string* response_out) {
    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "hire_mercenary", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"hire_mercenary","reason":"no_living_actor"})";
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + 0x18);  // FULL engine id

    int cid = json_get_int(params_json, "company_id");
    if (cid <= 0) {
        if (response_out) *response_out = R"({"success":false,"action":"hire_mercenary","reason":"company_id_required"})";
        return false;
    }
    uint32_t company_id = static_cast<uint32_t>(cid);
    int clen = json_get_int(params_json, "contract_len");
    uint32_t contract_len = clen > 0 ? static_cast<uint32_t>(clen) : 1;

    // Verify the company id resolves in the real merc DB before fabricating (honest reject).
    if (ad_db_lookup(ck3::globals::MERC_COMPANY_DB, company_id) == 0) {
        if (response_out) *response_out = R"({"success":false,"action":"hire_mercenary","reason":"company_not_found"})";
        return false;
    }

    // #63 FIX: GAME_COMMAND_DISPATCH only ENQUEUES to the player-command queue
    // (never applies fabricated AI commands). Use strategy B instead: probe
    // CanExecute (HIRE_MERC_CMD_CANEXEC, honest reject — validates both DB
    // lookups + hireable-precheck + contract) then call Execute directly
    // (HIRE_MERC_CMD_EXECUTE, which tail-applies the hire effect). Execute calls
    // the RNG-mix cost fn, so keep the fake RNG scope. (re_hire_mercenary_*.md)
    auto ctor = reinterpret_cast<void(*)(void*, uint32_t, uint32_t, uint32_t)>(
        g_base_addr + ck3::globals::HIRE_MERC_CMD_CTOR);
    auto canexec = reinterpret_cast<uint8_t(*)(void*, int)>(
        g_base_addr + ck3::globals::HIRE_MERC_CMD_CANEXEC);
    auto execute = reinterpret_cast<void(*)(void*)>(
        g_base_addr + ck3::globals::HIRE_MERC_CMD_EXECUTE);
    auto base_dtor = reinterpret_cast<void(*)(void*)>(g_base_addr + 0x0755b650);

    uint8_t cmd[0x40]; memset(cmd, 0, sizeof(cmd));   // struct is 0x30; 0x40 buffer is safe
    uint8_t can = 0;

    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("hire_mercenary");
        return false;
    }
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false; watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"hire_mercenary","reason":"apply_crashed"})";
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);

    ctor(cmd, actor_id & 0xFFFFFF, company_id & 0xFFFFFF, contract_len);
    can = canexec(cmd, 0);
    if (can) execute(cmd);
    base_dtor(cmd);

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    if (!can) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("hire_mercenary");
            r.key("reason"); r.value_string("rejected_or_unaffordable");
            r.key("company_id"); r.value_uint(company_id & 0xFFFFFF);
            r.key("note"); r.value_string("CanExecute returned false (can't afford, not hireable, or invalid contract)");
            r.end_object();
            *response_out = r.take();
        }
        ADLOG("hire_mercenary: actor=0x%x company=0x%x REJECTED (CanExecute=0)\n", actor_id, company_id);
        return false;
    }

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("hire_mercenary");
        r.key("applied"); r.value_bool(true);
        r.key("company_id"); r.value_uint(company_id & 0xFFFFFF);
        r.key("contract_len"); r.value_uint(contract_len);
        r.key("note"); r.value_string("hired (CanExecute passed + Execute applied); confirm via query(domain='armies')");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("hire_mercenary: actor=0x%x company=0x%x contract=%u APPLIED\n",
          actor_id, company_id, contract_len);
    return true;
}

// faith_lookup — resolve a CFaith* from FAITH_DB by faith id. FAITH_DB is a
// TPdxRefDatabase<CFaith,8>: its per-entry validity field is entry+0x08 (faith::
// FAITH_ID, FULL 32-bit), NOT ad_db_lookup's entry+0x10 &0xFFFFFF — so ad_db_lookup
// returns null for valid faiths. Verified live: faith_id 23→"catholic", 12→"ari".
// (#faith RE FIX 2026-06-19.)
static uintptr_t faith_lookup(uint32_t faith_id) {
    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::FAITH_DB);
    if (!db) return 0;
    uint32_t cap = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);   // db+0x2c
    uint32_t idx = faith_id & 0xFFFFFF;
    if (idx >= cap) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR); // db+0x20
    if (!table) return 0;
    uintptr_t entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE + (uintptr_t)idx * ck3::hash_map::ENTRY_STRIDE);
    if (!entry || !is_mapped(entry)) return 0;
    if (safe_read<uint32_t>(entry + faith::FAITH_ID) != faith_id) return 0;   // +0x08, FULL 32-bit
    return entry;
}

// ===========================================================================
// do_set_doctrine — swap one doctrine/tenet on the actor's CURRENT faith.
// Resolves CFaith via FAITH_INDEXED_DB[char+FAITH_ID_DIRECT], resolves the
// requested CFaithDoctrine* by name-scan of FAITH_DOCTRINE_DB, then calls the
// native FAITH_ADD_DOCTRINE(faith, &doctrine) mutator (validated + group-limit
// checked). Plain mutator → NO RNG scope, watchdog+sigsetjmp only.
// (Ported from worktree-agent-a229c89ab82637c48; re_faith_reform_doctrine.md.)
// ===========================================================================
bool do_set_doctrine(const char* params_json, std::string* response_out) {
    const char* doctrine_name = json_get_string(params_json, "doctrine");
    if (!doctrine_name || !doctrine_name[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"set_doctrine","reason":"missing_doctrine"})";
        return false;
    }
    std::string want(doctrine_name);

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "set_doctrine", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_doctrine","reason":"no_living_actor"})";
        return false;
    }

    uint32_t faith_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::FAITH_ID_DIRECT);
    ADLOG("set_doctrine: ENTER want='%s' actor=%p faith_id=%u\n", want.c_str(), actor, faith_id);
    if (faith_id == 0 || faith_id == 0xFFFFFFFF) {
        if (response_out) *response_out = R"({"success":false,"action":"set_doctrine","reason":"no_faith"})";
        return false;
    }
    // #faith RE FIX (2026-06-19): FAITH_INDEXED_DB (0x0bedadd8) is NOT a hash-map (cap=0
    // live) → always returned null ("faith_unavailable"). The real ID-indexed faith DB is
    // FAITH_DB (0x0bedad78, cap=1024). Verified live: char+FAITH_ID_DIRECT(0xB4)=23 →
    // FAITH_DB[23] = CFaith with key "catholic" + 28 doctrines.
    // #faith RE FIX (2026-06-19): FAITH_DB is a TPdxRefDatabase<CFaith,8> whose validity
    // field is entry+0x08 (FULL 32-bit), NOT ad_db_lookup's entry+0x10 &0xFFFFFF — so
    // ad_db_lookup returned null. Use the dedicated faith_lookup helper instead.
    uintptr_t faith_obj_addr = faith_lookup(faith_id);
    if (!faith_obj_addr || !is_mapped(faith_obj_addr)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_doctrine","reason":"faith_unavailable"})";
        return false;
    }

    // #faith RE FIX (2026-06-18): the direct FAITH_ADD_DOCTRINE call had REVERSED
    // args (true sig: AddDoctrine(doctrine_ctx, &faith_ref)) AND used a wrong
    // FAITH_DOCTRINE_DB offset (0x0bfdaf88 was +0x100000 off). Route through the
    // console `add_doctrine` handler (CMD_ADD_DOCTRINE) instead — it calls
    // CFaith_AddDoctrineToContainer(faith, doctrine) in the correct order via the
    // proven SResult/SCommandData ABI (mirrors set_title_law). Resolve the actor's
    // faith KEY from the faith object; the handler resolves both faith + doctrine
    // by key, so we no longer need the doctrine DB at all.
    // #faith RE FIX: CFaith key is at faith::FAITH_KEY (0x98), NOT db_object::NAME_KEY
    // (0x10 = garbage for a CFaith). 0x98 → "catholic".
    std::string faith_key = ad_read_std_string(faith_obj_addr + faith::FAITH_KEY);
    if (faith_key.empty()) {
        if (response_out) *response_out = R"({"success":false,"action":"set_doctrine","reason":"faith_key_unresolved"})";
        return false;
    }

    struct DocArgEntry { const char* data; uint64_t length; uint64_t reserved[2]; };
    struct DocCommandData { DocArgEntry* args; uint32_t pad_08; int32_t arg_count; uint8_t pad_rest[48]; };
    struct DocResult { uint8_t success; uint8_t _pad[7]; std::string message; };

    auto handler = reinterpret_cast<void*(*)(void*, const void*)>(
        g_base_addr + ck3::console_handlers::CMD_ADD_DOCTRINE);
    // ARG ORDER (re_resolution_faith.md verify): the add_doctrine console parser expects
    // args[0]=DOCTRINE (hashed against the doctrine DB), args[1]=FAITH (FaithLookupByName) —
    // <doctrine> [faith]. The prior [faith, doctrine] order made the doctrine lookup always
    // miss → permanent "Invalid doctrine". Doctrine first, faith second.
    DocArgEntry args[2]; memset(args, 0, sizeof(args));
    args[0].data = want.c_str();      args[0].length = want.size();      // doctrine name
    args[1].data = faith_key.c_str(); args[1].length = faith_key.size(); // faith key
    DocCommandData cmd_data; memset(&cmd_data, 0, sizeof(cmd_data));
    cmd_data.args = args; cmd_data.arg_count = 2;
    DocResult result; memset(&result.success, 0, sizeof(result.success) + sizeof(result._pad));

    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("set_doctrine");
        return false;
    }
    uint64_t saved_rng = 0; bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false; watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"set_doctrine","reason":"apply_crashed"})";
        return false;
    }
    ADLOG("set_doctrine: calling CMD_ADD_DOCTRINE (faith='%s' doctrine='%s')...\n",
          faith_key.c_str(), want.c_str());
    handler(&result, &cmd_data);
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    bool applied = (result.success != 0);
    std::string engine_msg;
    if (!result.message.empty())
        engine_msg = result.message.substr(0, std::min(result.message.size(), (size_t)512));
    ADLOG("set_doctrine: handler returned success=%d msg='%s'\n", applied ? 1 : 0, engine_msg.c_str());

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(applied);
        r.key("action"); r.value_string("set_doctrine");
        r.key("requested"); r.value_string(want.c_str(), want.size());
        r.key("faith"); r.value_string(faith_key.c_str(), faith_key.size());
        r.key("faith_id"); r.value_uint(faith_id & 0xFFFFFF);
        if (!engine_msg.empty()) { r.key("engine_result"); r.value_string(engine_msg.c_str(), engine_msg.size()); }
        if (!applied && !engine_msg.empty()) { r.key("reason"); r.value_string(engine_msg.c_str(), engine_msg.size()); }
        else if (!applied) { r.key("reason"); r.value_string("add_doctrine_rejected"); }
        r.end_object();
        *response_out = r.take();
    }
    return applied;
}

// ===========================================================================
// faith_lookup_by_key — resolve a faith_id from a faith key string ("catholic",
// "asatru", "norse_pagan", …) by scanning FAITH_DB. FAITH_DB is a
// TPdxRefDatabase<CFaith,8>: capacity at db+0x2c, table at db+0x20, per-entry
// stride 0x10 with the CFaith* at table+8+id*0x10 and the validity/id at
// entry+faith::FAITH_ID (+0x08, FULL 32-bit). The human-readable faith key
// lives at faith::FAITH_KEY (+0x98). Returns 0xFFFFFFFF on miss.
// (mirrors faith_lookup above; companion to do_set_character_faith.)
// ===========================================================================
static uint32_t faith_lookup_by_key(const std::string& key) {
    if (key.empty()) return 0xFFFFFFFF;
    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::FAITH_DB);
    if (!db) return 0xFFFFFFFF;
    uint32_t cap = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);     // db+0x2c
    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR); // db+0x20
    if (!table || cap == 0) return 0xFFFFFFFF;
    // cap can be a 1024-slot allocation; clamp the scan to a sane bound.
    if (cap > 4096) cap = 4096;
    for (uint32_t idx = 0; idx < cap; ++idx) {
        uintptr_t entry = safe_read<uintptr_t>(
            table + ck3::hash_map::ENTRY_HEADER_SIZE + (uintptr_t)idx * ck3::hash_map::ENTRY_STRIDE);
        if (!entry || !is_mapped(entry)) continue;
        if (safe_read<uint32_t>(entry + faith::FAITH_ID) != idx) continue;  // valid live faith
        std::string k = ad_read_std_string(entry + faith::FAITH_KEY);
        if (k == key) return idx;
    }
    return 0xFFFFFFFF;
}

// ===========================================================================
// do_set_character_faith — convert the ACTOR's OWN faith to a target faith.
//
// THE convert-own-faith lever. CK3's in-game path (convert_to_religion_interaction)
// only opens a GUI faith_conversion view — its on_accept fires NO scripted
// set_character_faith, so it is un-callable headless. The actual conversion is
// performed by the engine setter SetFaithOnCharacter @ funcs::SET_FAITH
// (0x0666bc40), signature SetFaithOnCharacter(CCharacter*, uint faith_id,
// char fire_onaction, char head_of_faith_cleanup). Decompile-verified
// (re_religion_conversion_holywar.md): it looks the faith up in _g_FaithDB by
// id, writes char+0xB4 (FAITH_ID_DIRECT), and — with the two trailing flags = 1 —
// fires on_faith_conversion (OnActionManager_Dispatch event 0x33), runs the
// modifier/HoF/title recalc, and the head-of-faith cleanup. It takes a faith_id
// INTEGER, NOT a faith-scope object → NO scope_ctx frame (#73) needed.
//
// Request: { "actor_id": <int>, "faith": "<faith_key>" }   (e.g. "catholic")
//      or  { "actor_id": <int>, "faith_id": <int> }
//
// Legitimacy: this is the engine's own setter (the same function the GUI view
// and convert_at_war/adventurer interactions ultimately drive). It is a real
// engine mutation with full side effects, not a raw memory poke of char+0xB4.
// Voluntary self-conversion is auto_accept in CK3 (no cost/gate beyond the
// is_shown checks the AGENT is responsible for honoring); the bridge applies it
// directly the way set_doctrine / set_lifestyle_focus do. Game-thread mutation —
// classified safe-action so it runs UNPAUSED (drained on_tick).
// ===========================================================================
bool do_set_character_faith(const char* params_json, std::string* response_out) {
    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "set_character_faith", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_character_faith","reason":"no_living_actor"})";
        return false;
    }

    // Resolve the target faith id — explicit faith_id wins; else resolve by key.
    int faith_id_raw = json_get_int(params_json, "faith_id");
    std::string faith_key;
    uint32_t target_faith_id = 0xFFFFFFFF;
    if (faith_id_raw > 0) {
        target_faith_id = static_cast<uint32_t>(faith_id_raw);
    } else {
        const char* fk = json_get_string(params_json, "faith");
        if (!fk || !fk[0]) {
            if (response_out) *response_out = R"({"success":false,"action":"set_character_faith","reason":"missing_faith"})";
            return false;
        }
        faith_key = fk;
        target_faith_id = faith_lookup_by_key(faith_key);
        if (target_faith_id == 0xFFFFFFFF) {
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("set_character_faith");
                r.key("reason"); r.value_string("faith_not_found");
                r.key("attempted"); r.value_string(faith_key.c_str(), faith_key.size());
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
    }

    // Verify the resolved id maps to a live CFaith and capture its key for the
    // response / readback.
    uintptr_t target_faith_obj = faith_lookup(target_faith_id);
    if (!target_faith_obj || !is_mapped(target_faith_obj)) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_character_faith");
            r.key("reason"); r.value_string("faith_unavailable");
            r.key("faith_id"); r.value_uint(target_faith_id);
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    if (faith_key.empty())
        faith_key = ad_read_std_string(target_faith_obj + faith::FAITH_KEY);

    uint32_t cur_faith_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::FAITH_ID_DIRECT);
    std::string cur_faith_key;
    if (uintptr_t cf = faith_lookup(cur_faith_id)) cur_faith_key = ad_read_std_string(cf + faith::FAITH_KEY);
    ADLOG("set_character_faith: ENTER actor=%p cur_faith=%u('%s') -> %u('%s')\n",
          actor, cur_faith_id, cur_faith_key.c_str(), target_faith_id, faith_key.c_str());

    if ((cur_faith_id & 0xFFFFFF) == (target_faith_id & 0xFFFFFF)) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(true);
            r.key("action"); r.value_string("set_character_faith");
            r.key("reason"); r.value_string("already_target_faith");
            r.key("faith"); r.value_string(faith_key.c_str(), faith_key.size());
            r.key("faith_id"); r.value_uint(target_faith_id & 0xFFFFFF);
            r.end_object();
            *response_out = r.take();
        }
        return true;
    }

    // Call SetFaithOnCharacter(char*, faith_id, fire_onaction=1, hof_cleanup=1)
    // under the same RNG-armed watchdog/sigsetjmp guard set_doctrine /
    // set_lifestyle_focus use: the conversion fires on_faith_conversion which can
    // advance the RNG scope, and on a thread where RandomScopeManager+0x20 is null
    // that null-derefs. Arm the fake RNG scope BEFORE sigsetjmp and restore on
    // BOTH paths so a crash doesn't wedge the game thread.
    auto set_faith = reinterpret_cast<void(*)(void*, unsigned int, char, char)>(
        g_base_addr + ck3::funcs::SET_FAITH);
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("set_character_faith");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"set_character_faith","reason":"apply_crashed"})";
        return false;
    }
    ADLOG("set_character_faith: calling SET_FAITH(actor, %u, 1, 1) (rng_armed=%d)...\n",
          target_faith_id, rng_armed ? 1 : 0);
    set_faith(actor, target_faith_id, 1, 1);
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    // Verify by reading back char+0xB4.
    uint32_t new_faith_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::FAITH_ID_DIRECT);
    bool applied = (new_faith_id & 0xFFFFFF) == (target_faith_id & 0xFFFFFF);
    std::string new_faith_key;
    if (uintptr_t nf = faith_lookup(new_faith_id)) new_faith_key = ad_read_std_string(nf + faith::FAITH_KEY);
    ADLOG("set_character_faith: SET_FAITH returned; readback faith_id=%u('%s') applied=%d\n",
          new_faith_id, new_faith_key.c_str(), applied ? 1 : 0);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(applied);
        r.key("action"); r.value_string("set_character_faith");
        r.key("requested"); r.value_string(faith_key.c_str(), faith_key.size());
        r.key("faith_id"); r.value_uint(target_faith_id & 0xFFFFFF);
        if (!cur_faith_key.empty()) { r.key("previous_faith"); r.value_string(cur_faith_key.c_str(), cur_faith_key.size()); }
        if (!new_faith_key.empty()) { r.key("current_faith"); r.value_string(new_faith_key.c_str(), new_faith_key.size()); }
        if (!applied) { r.key("reason"); r.value_string("faith_not_applied"); }
        r.end_object();
        *response_out = r.take();
    }
    return applied;
}

// ===========================================================================
// culture_lookup_by_id — resolve a CCulture* from a culture id by indexing
// CULTURE_INDEXED_DB (a hash_map: db+0x20=table, db+0x2c=capacity, entry at
// table+8+id*0x10, entry+0x10=id key). Mirrors observation.cpp db_lookup.
// Returns 0 on miss.
// ===========================================================================
static uintptr_t culture_lookup_by_id(uint32_t id) {
    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CULTURE_INDEXED_DB);
    if (!db) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!table) return 0;
    uint32_t cap = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    uint32_t masked = id & 0xFFFFFF;
    if (masked >= cap) return 0;
    uintptr_t entry = safe_read<uintptr_t>(
        table + ck3::hash_map::ENTRY_HEADER_SIZE + (uintptr_t)masked * ck3::hash_map::ENTRY_STRIDE);
    if (!entry || !is_mapped(entry)) return 0;
    if ((safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) & 0xFFFFFF) != masked) return 0;
    return entry;
}

// ===========================================================================
// culture_lookup_by_key — resolve a CCulture* from a script key string
// ("english", "anglo_saxon", "norse", …) by scanning CULTURE_INDEXED_DB and
// matching the scripted key at culture::SCRIPT_KEY (+0x1C8). The display name
// ("English", "Anglo-Saxon") lives at culture::NAME (+0x260) — we deliberately
// match the SCRIPT key the agent passes, not the localized name. Returns 0 on miss.
// (companion to do_set_character_culture; mirrors faith_lookup_by_key.)
// ===========================================================================
static uintptr_t culture_lookup_by_key(const std::string& key) {
    if (key.empty()) return 0;
    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CULTURE_INDEXED_DB);
    if (!db) return 0;
    uint32_t cap = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!table || cap == 0) return 0;
    if (cap > 4096) cap = 4096;
    for (uint32_t idx = 0; idx < cap; ++idx) {
        uintptr_t entry = safe_read<uintptr_t>(
            table + ck3::hash_map::ENTRY_HEADER_SIZE + (uintptr_t)idx * ck3::hash_map::ENTRY_STRIDE);
        if (!entry || !is_mapped(entry)) continue;
        if ((safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) & 0xFFFFFF) != idx) continue;
        std::string k = ad_read_std_string(entry + culture::SCRIPT_KEY);
        if (k == key) return entry;
    }
    return 0;
}

// ===========================================================================
// do_set_character_culture — convert the ACTOR's OWN culture to a target culture.
//
// THE convert-own-culture lever (ck-#130). This is the CRITICAL unlock for the
// e_britannia goal: the e_britannia title's can_create is culture-gated on
// heritage_west_germanic (english/anglo_saxon), heritage_goidelic (gaelic), or
// heritage_brythonic (welsh). A NORSE (north_germanic) ruler — the Halfdan
// k_danelaw line — can form e_britannia by NEITHER path, so the agent must
// convert its OWN culture to e.g. english/anglo_saxon first. There is no in-game
// scripted/interaction lever for a ruler to swap its own culture wholesale
// headlessly, so we route through the engine setter directly — exactly as #128
// did for faith.
//
// The conversion is performed by the engine setter SetCultureOnCharacter @
// funcs::SET_CULTURE (0x0666ada0), signature
//   SetCultureOnCharacter(CCharacter* char, CCulture* culture_obj, char flag).
// Decompile-verified: it guards char+0x19c (dead-flag), reads the old culture
// from CULTURE_INDEXED_DB, writes char+0xB0 (CULTURE_ID_DIRECT) = culture_obj+0x10,
// then runs the culture-change recalc (innovation/heritage/county tracking).
// The console handler CMD_SET_CULTURE drives it with flag=0; we mirror that.
// It takes a culture-OBJECT pointer (NOT an id, unlike SetFaith), so we resolve
// the object via culture_lookup_by_key/_by_id first. No scope_ctx (#73) needed.
//
// Request: { "actor_id": <int>, "culture": "<script_key>" }   (e.g. "english")
//      or  { "actor_id": <int>, "culture_id": <int> }
//
// Legitimacy: this is the engine's own setter (same fn the set_culture console
// effect and culture-conversion machinery drive) with full recalc side effects,
// not a raw poke of char+0xB0. Voluntary self-conversion has no cost gate; the
// bridge applies it directly the way set_doctrine / set_character_faith do.
// Game-thread mutation — classified safe-action so it runs UNPAUSED.
//
// RNG-armed watchdog/sigsetjmp guard mirrors set_character_faith: the culture
// recalc can advance the RNG scope and null-deref on a thread where
// RandomScopeManager+0x20 is null.
// ===========================================================================
bool do_set_character_culture(const char* params_json, std::string* response_out) {
    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "set_character_culture", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_character_culture","reason":"no_living_actor"})";
        return false;
    }

    // Resolve the target culture object — explicit culture_id wins; else by key.
    int culture_id_raw = json_get_int(params_json, "culture_id");
    std::string culture_key;
    uintptr_t target_culture_obj = 0;
    if (culture_id_raw > 0) {
        target_culture_obj = culture_lookup_by_id(static_cast<uint32_t>(culture_id_raw));
    } else {
        const char* ck = json_get_string(params_json, "culture");
        if (!ck || !ck[0]) {
            if (response_out) *response_out = R"({"success":false,"action":"set_character_culture","reason":"missing_culture"})";
            return false;
        }
        culture_key = ck;
        target_culture_obj = culture_lookup_by_key(culture_key);
        if (!target_culture_obj) {
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("set_character_culture");
                r.key("reason"); r.value_string("culture_not_found");
                r.key("attempted"); r.value_string(culture_key.c_str(), culture_key.size());
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
    }

    if (!target_culture_obj || !is_mapped(target_culture_obj)) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_character_culture");
            r.key("reason"); r.value_string("culture_unavailable");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    uint32_t target_culture_id = safe_read<uint32_t>(target_culture_obj + culture::ID) & 0xFFFFFF;
    if (culture_key.empty())
        culture_key = ad_read_std_string(target_culture_obj + culture::SCRIPT_KEY);

    uint32_t cur_culture_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::CULTURE_ID_DIRECT) & 0xFFFFFF;
    std::string cur_culture_key;
    if (uintptr_t cc = culture_lookup_by_id(cur_culture_id)) cur_culture_key = ad_read_std_string(cc + culture::SCRIPT_KEY);
    ADLOG("set_character_culture: ENTER actor=%p cur_culture=%u('%s') -> %u('%s')\n",
          actor, cur_culture_id, cur_culture_key.c_str(), target_culture_id, culture_key.c_str());

    if (cur_culture_id == target_culture_id) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(true);
            r.key("action"); r.value_string("set_character_culture");
            r.key("reason"); r.value_string("already_target_culture");
            r.key("culture"); r.value_string(culture_key.c_str(), culture_key.size());
            r.key("culture_id"); r.value_uint(target_culture_id);
            r.end_object();
            *response_out = r.take();
        }
        return true;
    }

    // Call SetCultureOnCharacter(char*, culture_obj*, flag=0) under the same
    // RNG-armed watchdog/sigsetjmp guard set_character_faith uses.
    auto set_culture = reinterpret_cast<void(*)(void*, void*, char)>(
        g_base_addr + ck3::funcs::SET_CULTURE);
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("set_character_culture");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        if (response_out) *response_out = R"({"success":false,"action":"set_character_culture","reason":"apply_crashed"})";
        return false;
    }
    ADLOG("set_character_culture: calling SET_CULTURE(actor, culture_obj=%p, 0) (rng_armed=%d)...\n",
          reinterpret_cast<void*>(target_culture_obj), rng_armed ? 1 : 0);
    set_culture(actor, reinterpret_cast<void*>(target_culture_obj), 0);
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    // Verify by reading back char+0xB0.
    uint32_t new_culture_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::CULTURE_ID_DIRECT) & 0xFFFFFF;
    bool applied = (new_culture_id == target_culture_id);
    std::string new_culture_key;
    if (uintptr_t nc = culture_lookup_by_id(new_culture_id)) new_culture_key = ad_read_std_string(nc + culture::SCRIPT_KEY);
    ADLOG("set_character_culture: SET_CULTURE returned; readback culture_id=%u('%s') applied=%d\n",
          new_culture_id, new_culture_key.c_str(), applied ? 1 : 0);

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(applied);
        r.key("action"); r.value_string("set_character_culture");
        r.key("requested"); r.value_string(culture_key.c_str(), culture_key.size());
        r.key("culture_id"); r.value_uint(target_culture_id);
        if (!cur_culture_key.empty()) { r.key("previous_culture"); r.value_string(cur_culture_key.c_str(), cur_culture_key.size()); }
        if (!new_culture_key.empty()) { r.key("current_culture"); r.value_string(new_culture_key.c_str(), new_culture_key.size()); }
        if (!applied) { r.key("reason"); r.value_string("culture_not_applied"); }
        r.end_object();
        *response_out = r.take();
    }
    return applied;
}

// ===========================================================================
// do_set_title_law — set a title-scoped law on a CLandedTitle via the game's
// own `add_title_law` console handler (CMD_ADD_TITLE_LAW). Title-scoped by its
// first string arg (title TAG), actor-independent; applies real costs+effects
// (legitimate engine law-setter). RNG-armed (law-pass fires an effect/event).
// (Ported from worktree-agent-a229c89ab82637c48.)
// ===========================================================================
namespace {
    struct STitleLawArgEntry {
        const char* data;
        uint64_t    length;
        uint64_t    reserved[2];
    };
    struct STitleLawCommandData {
        STitleLawArgEntry* args;
        uint32_t pad_08;
        int32_t  arg_count;
        uint8_t  pad_rest[48];
    };
    struct STitleLawResult {
        uint8_t  success;
        uint8_t  _pad[7];
        std::string message;
    };
}

bool do_set_title_law(const char* params_json, std::string* response_out) {
    const char* law_name = json_get_string(params_json, "law_name");
    if (!law_name || !law_name[0]) {
        if (response_out) *response_out = R"({"success":false,"action":"set_title_law","reason":"missing_law_name"})";
        return false;
    }
    // ck-84: set_title_law (TITLE-scoped) has NO mapped costed command path yet
    // (CAddTitleLawEffect::Execute not byte-pinned — see ck3_offsets.h). Keep the
    // strict refusal for high-stakes title laws until that lands. Refuse high-stakes
    // unless allow_uncosted=true (harness).
    if (is_high_stakes_law(law_name) && !json_get_bool(params_json, "allow_uncosted", false)) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_title_law");
            r.key("reason"); r.value_string("high_stakes_law_uncosted");
            r.key("law_name"); r.value_string(law_name);
            r.key("hint"); r.value_string("title-scoped high-stakes laws have no costed command path mapped yet (CAddTitleLawEffect::Execute unpinned); the realm-law costed path (act type=law) covers crown_authority_*. Pass allow_uncosted=true only for harness setup.");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    std::string law(law_name);

    int title_id = json_get_int(params_json, "title_id");
    if (title_id <= 0) {
        if (response_out) *response_out = R"({"success":false,"action":"set_title_law","reason":"missing_title_id"})";
        return false;
    }

    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "set_title_law", response_out)) return false;
    if (!actor || !g_char_is_alive(actor)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_title_law","reason":"no_living_actor"})";
        return false;
    }

    uintptr_t tobj = lookup_title_obj(static_cast<uint32_t>(title_id));
    if (!tobj || !is_mapped(tobj)) {
        if (response_out) *response_out = R"({"success":false,"action":"set_title_law","reason":"title_not_found"})";
        return false;
    }
    std::string title_key;
    uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
    if (type_ptr && is_mapped(type_ptr)) {
        title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
    }
    ADLOG("set_title_law: ENTER title_id=%d key='%s' law='%s' actor=%p\n",
          title_id, title_key.c_str(), law.c_str(), actor);
    if (title_key.empty()) {
        if (response_out) *response_out = R"({"success":false,"action":"set_title_law","reason":"title_key_unresolved"})";
        return false;
    }

    auto handler = reinterpret_cast<void*(*)(void*, const void*)>(
        g_base_addr + ck3::console_handlers::CMD_ADD_TITLE_LAW);

    STitleLawArgEntry args[2];
    memset(args, 0, sizeof(args));
    args[0].data = title_key.c_str();  args[0].length = title_key.size();
    args[1].data = law.c_str();        args[1].length = law.size();

    STitleLawCommandData cmd_data;
    memset(&cmd_data, 0, sizeof(cmd_data));
    cmd_data.args = args;
    cmd_data.arg_count = 2;

    STitleLawResult result;
    memset(&result.success, 0, sizeof(result.success) + sizeof(result._pad));

    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this
    // process, so the slot cannot be reasoned about. REFUSE — do not
    // proceed unarmed, which is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("set_title_law");
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        ADLOG("set_title_law: crashed during execution for law '%s' on '%s'\n",
              law.c_str(), title_key.c_str());
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_title_law");
            r.key("reason"); r.value_string("apply_crashed");
            r.key("title_id"); r.value_int(title_id);
            r.key("law_name"); r.value_string(law.c_str(), law.size());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    ADLOG("set_title_law: calling CMD_ADD_TITLE_LAW (title='%s' law='%s')...\n",
          title_key.c_str(), law.c_str());
    handler(&result, &cmd_data);
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    bool success = (result.success != 0);
    std::string engine_msg;
    if (!result.message.empty()) {
        engine_msg = result.message.substr(0, std::min(result.message.size(), (size_t)512));
    }
    ADLOG("set_title_law: handler returned success=%d msg='%s'\n",
          success ? 1 : 0, engine_msg.c_str());

    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(success);
        r.key("action"); r.value_string("set_title_law");
        r.key("title_id"); r.value_int(title_id);
        r.key("title_key"); r.value_string(title_key.c_str(), title_key.size());
        r.key("law_name"); r.value_string(law.c_str(), law.size());
        if (!engine_msg.empty()) { r.key("engine_result"); r.value_string(engine_msg.c_str(), engine_msg.size()); }
        if (!success && !engine_msg.empty()) { r.key("reason"); r.value_string(engine_msg.c_str(), engine_msg.size()); }
        r.end_object();
        *response_out = r.take();
    }
    return success;
}

} // namespace bridge
