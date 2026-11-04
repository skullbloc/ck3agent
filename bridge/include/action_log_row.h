// action_log_row.h — what one action-log row says about the action (L18-A).
//
// THE DEFECT. The action log (<telemetry>/action_log.jsonl, dispatch_router.cpp
// emit_action_log) splices the handler's reply in as `result` at dispatch time.
// For a write the drain runs LATER (call_war_create_core on the game thread,
// create_title, faction, scheme, interaction, decision), that reply is the
// enqueue receipt: {"success": false, "status": "queued", "reason":
// "no_poll_on_game_thread", "drains_when": "next_pass"}. The row therefore read
// `success:false` for every landed war in the 2026.11 pilots — all 31
// call_war_create_core rows, including Manasseh's war 13 that was won and
// enforced — while the seat's reply said `applied, verified_active: true`. The
// poll's absence was being recorded as the action's failure, and the drain's
// verdict was never written anywhere a reader of the log could find it.
//
// THE FIX, in two parts, both decided here (engine-free, host-tested by
// bridge/test/test_action_log_row.cpp):
//   1. Every row carries `verdict` (what the reply said about the ACTION:
//      applied / rejected / pending / unknown) and, separately, `poll` (what the
//      bridge's wait for it did: answered / skipped_on_game_thread / timed_out /
//      not_applicable). The enqueue receipt is `verdict: pending`, never a
//      failure.
//   2. When the drain completes an intent, one more row, kind "outcome", verb
//      "intent_outcome", carries the drain's verdict and the executor's result
//      (war_id ...) keyed by the same dispatch_id the receipt row's `result`
//      carries — so the log attributes the outcome to the call that asked for
//      it. The verb is deliberately not the call's: readers keyed on a verb
//      (the overlay ticker) count each call once, as before.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "intent_queue.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace bridge {

struct ActionLogVerdict {
    const char* verdict;   // applied | rejected | pending | unknown
    const char* poll;      // answered | skipped_on_game_thread | timed_out | not_applicable
};

// The reply's own words, read once. An unparseable or non-object reply is
// `unknown` — never `rejected`, which would say the action was refused.
inline ActionLogVerdict action_log_verdict_of(const char* result_json) {
    if (!result_json || result_json[0] != '{') return {"unknown", "not_applicable"};
    rapidjson::Document d;
    d.Parse(result_json);
    if (d.HasParseError() || !d.IsObject()) return {"unknown", "not_applicable"};
    auto str = [&](const char* k) -> const char* {
        auto it = d.FindMember(k);
        return (it != d.MemberEnd() && it->value.IsString()) ? it->value.GetString() : "";
    };
    const char* status = str("status");
    const char* reason = str("reason");

    const char* poll = "not_applicable";
    if (strcmp(reason, "no_poll_on_game_thread") == 0) poll = "skipped_on_game_thread";
    else if (strcmp(status, "timeout") == 0 ||
             strcmp(reason, "intent_not_completed_within_5s") == 0 ||
             // L18 round 2: the faction and scheme handlers' own wait ran out and
             // nothing retired the intent (factions.cpp faction_action; the scheme
             // handler after a retire that found it claimed), so the drain may
             // still apply it: pending, decided by the outcome row. A scheme
             // intent the retire DID cancel says intent_timeout_cancelled_not_executed
             // and never runs: that one stays rejected.
             strcmp(reason, "intent_timeout_no_result") == 0) poll = "timed_out";

    // A queued or timed-out intent has not been decided: its verdict is the
    // outcome row's, written when the drain completes it.
    if (strcmp(status, "queued") == 0 || strcmp(poll, "timed_out") == 0)
        return {"pending", poll};
    if (strcmp(status, "war_created") == 0 || strcmp(status, "rejected") == 0) poll = "answered";

    auto ok = d.FindMember("success");
    if (ok == d.MemberEnd() || !ok->value.IsBool()) return {"unknown", poll};
    return {ok->value.GetBool() ? "applied" : "rejected", poll};
}

// The drain's verdict on one completed write intent: the same rule the
// call_war_create_core poll applies (executed && !rejected).
inline const char* intent_outcome_verdict(const Intent& c) {
    return (c.executed && !c.rejected) ? "applied" : "rejected";
}

inline const char* intent_type_label(IntentType t) {
    switch (t) {
        case IntentType::INTERACTION:            return "interaction";
        case IntentType::WAR_DECLARE:            return "war_declare";
        case IntentType::DECISION:               return "decision";
        case IntentType::WAR_CREATE_CORE_DIRECT: return "call_war_create_core";
        case IntentType::FACTION:                return "faction";
        case IntentType::CREATE_TITLE:           return "create_title";
        case IntentType::SCHEME:                 return "scheme";
    }
    return "unknown";
}

// One outcome row, the whole JSONL line without its newline. `ts` and
// `game_date` are the caller's (the writer's clock and the engine calendar).
inline std::string action_outcome_row(const char* ts, const char* game_date, const Intent& c) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("ts");         w.String(ts ? ts : "");
    w.Key("game_date");  w.String(game_date ? game_date : "unknown");
    w.Key("session_id"); w.String(c.session_id.c_str());
    w.Key("kind");       w.String("outcome");
    w.Key("verb");       w.String("intent_outcome");
    w.Key("intent_type"); w.String(intent_type_label(c.type));
    w.Key("dispatch_id"); w.String(c.request_id.c_str());
    w.Key("actor_id");   w.Uint(c.actor_id & 0xFFFFFF);
    w.Key("target_id");  w.Uint(c.target_id & 0xFFFFFF);
    const char* verdict = intent_outcome_verdict(c);
    w.Key("verdict");    w.String(verdict);
    w.Key("poll");       w.String("not_applicable");
    if (strcmp(verdict, "rejected") == 0) {
        w.Key("reason");
        w.String(c.rejection_reason[0] ? c.rejection_reason : "not_executed");
    }
    // The executor's own report (war_id, war_created ...): an object stays an
    // object; anything else is carried as a string, never dropped.
    if (!c.console_output.empty()) {
        w.Key("result_raw");
        rapidjson::Document out;
        out.Parse(c.console_output.c_str());
        if (!out.HasParseError() && out.IsObject()) out.Accept(w);
        else w.String(c.console_output.c_str(), static_cast<rapidjson::SizeType>(c.console_output.size()));
    }
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// Written by dispatch_router.cpp (the action log's one writer); called from the
// drain for each completed write intent (bridge_init.cpp, after tick_maintenance).
void emit_action_outcome_log(const Intent& completed);

}  // namespace bridge
