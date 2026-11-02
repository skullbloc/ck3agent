// drain_write_verdict.h — the completed-ring record of one g_msg_queue drain.
//
// (re/findings/re_deferred_write_verdicts.md §B.3) The g_msg_queue drain runs an
// is_game_thread_action handler on the game thread and sends its reply matched to
// request_id. For a WRITE it also stores a consumed Intent {request_id,
// executed/rejected, rejection_reason} in the completed ring the Mechanism-A poll
// surfaces read (query_intents, query_dispatch_result{dispatch_id},
// query_intent_results, inspect(pending)), so the agent polls one surface for
// every deferred write. A game-thread READ (is_game_thread_read:
// construction_check, query_fabricate_targets) stores nothing: it is no write
// verdict, and a ring slot it took would push a real write's verdict out of
// reach of its poll_token (lane l6, witness #3).
//
// Header-only and engine-free: the decision comes in as `records` (the drain
// passes drain_records_write_verdict(action), snapshot.cpp) and the record goes
// out through `store`, so a host test drives the same code the drain runs
// (ck3agent/server/test_polarity_bridge_table.py compiles it beside the real
// classifiers).
#pragma once

#include <cstdint>
#include <cstdio>

#include "intent_queue.h"
#include "rapidjson/document.h"

namespace bridge {

// The actor a drained call names, for its record (L14, surface-eval finding 3:
// every steer row read actor_id 0). The client sends
// {"action", "params": {...}, "request_id"}, so the actor is under params; the
// drain read only the TOP level and so recorded 0 for every Mechanism-B write.
// Read the way json_get_int reads a handler's arguments: the top level first,
// then params; actor_id before character_id. Masked to the 24-bit handle; 0 when
// neither names a positive id.
inline uint32_t drain_actor_id(const rapidjson::Value& inner) {
    if (!inner.IsObject()) return 0;
    const rapidjson::Value* scopes[2] = {&inner, nullptr};
    auto pit = inner.FindMember("params");
    if (pit != inner.MemberEnd() && pit->value.IsObject()) scopes[1] = &pit->value;
    for (const char* key : {"actor_id", "character_id"}) {
        for (const rapidjson::Value* sc : scopes) {
            if (!sc) continue;
            auto it = sc->FindMember(key);
            if (it == sc->MemberEnd()) continue;
            if (it->value.IsInt64() && it->value.GetInt64() > 0)
                return static_cast<uint32_t>(it->value.GetInt64()) & 0xFFFFFF;
        }
    }
    return 0;
}

// Build the record for one drained call. Returns false (nothing to store) when the
// call is not a recorded write or carries no request_id. reply_json == nullptr or
// "" means the drain crashed or produced no reply: a typed game_thread_no_reply
// verdict, never a silent loss. success/reason come from the handler's reply
// ({"success":bool, "reason"|"error"}).
inline bool build_drain_write_verdict(bool records, const char* action_name,
                                      const char* req_id, uint32_t actor_id,
                                      const char* reply_json, Intent* out) {
    if (!records || !out) return false;
    if (!req_id || !req_id[0]) return false;  // no id: nothing to resolve against
    Intent rec = {};
    rec.type = IntentType::INTERACTION;   // generic game-thread write
    rec.actor_id = actor_id;
    rec.request_id = req_id;
    rec.interaction_name = action_name ? action_name : "game_thread_action";
    rec.consumed = true;
    if (!reply_json || !reply_json[0]) {
        rec.executed = false;
        rec.rejected = true;
        snprintf(rec.rejection_reason, sizeof(rec.rejection_reason),
                 "game_thread_no_reply");
    } else {
        rapidjson::Document d;
        d.Parse(reply_json);
        bool ok = false;
        const char* reason = nullptr;
        if (!d.HasParseError() && d.IsObject()) {
            if (d.HasMember("success") && d["success"].IsBool())
                ok = d["success"].GetBool();
            if (d.HasMember("reason") && d["reason"].IsString())
                reason = d["reason"].GetString();
            else if (d.HasMember("error") && d["error"].IsString())
                reason = d["error"].GetString();
        }
        rec.executed = ok;
        rec.rejected = !ok;
        if (!ok) {
            snprintf(rec.rejection_reason, sizeof(rec.rejection_reason), "%s",
                     reason ? reason : "rejected_by_handler");
        }
    }
    *out = rec;
    return true;
}

// The drain's one call: build the record and hand it to `store` (the completed
// ring's store_completed in the bridge). Returns whether a record was stored.
template <class Store>
inline bool record_drain_write_verdict(bool records, const char* action_name,
                                       const char* req_id, uint32_t actor_id,
                                       const char* reply_json, Store&& store) {
    Intent rec;
    if (!build_drain_write_verdict(records, action_name, req_id, actor_id,
                                   reply_json, &rec))
        return false;
    store(rec);
    return true;
}

}  // namespace bridge
