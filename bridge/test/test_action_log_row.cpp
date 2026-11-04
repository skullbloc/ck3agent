// test/test_action_log_row.cpp — L18-A: the action log says what happened to
// the action, and the poll's absence is not the action's failure.
//
// THE EVIDENCE (pilots-2026.11, docs/cleanup-2026.11/evidence/pilots-2026.11/raw):
// every call_war_create_core row in every seat's actions.jsonl (42 rows, deduped
// by ts+request_id) reads {"success":false,"status":"queued","reason":
// "no_poll_on_game_thread"}, while the bridge logs show 39 executor runs, every
// one "WAR_CREATE_CORE_DIRECT result: war_created=1", and the seats' replies
// said applied with verified_active:true. The fixtures below are those rows and
// that executor's own output format (intent_dispatch.cpp, the snprintf after
// WAR_CREATE_CORE_DIRECT), not invented shapes.
//
// Two halves:
//   decision — the real action_log_row.h on the real fixtures;
//   wiring   — the product's writer and drain call it (comment-stripped source,
//              so prose cannot satisfy a row).
// Usage: test_action_log_row <bridge-source-dir>

#include "action_log_row.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace bridge;

static int g_fails = 0, g_checks = 0;
static void check(bool ok, const char* what, const std::string& got = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what, got.empty() ? "" : " — got ", got.c_str());
}

static rapidjson::Document parse(const std::string& s) {
    rapidjson::Document d;
    d.Parse(s.c_str());
    return d;
}

// scale0/n8/play-s5/runs/20261103-191017.actions.jsonl (n8 shares one sink), Uzur (10647), 0867.02.03:
// the row's `result`, byte for byte.
static const char* kPilotReceipt =
    R"({"success":false,"action":"call_war_create_core","actor_id":10647,"target_id":11840,)"
    R"("casus_belli":"nomadic_conquest_cb","title_id":5442,"claimant_id":10647,)"
    R"("intent_request_id":"wcc_10647_33_44582","dispatch_id":"wcc_10647_33_44582",)"
    R"("status":"queued","reason":"no_poll_on_game_thread","drains_when":"next_pass"})";

static std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();) {
        if (src.compare(i, 2, "//") == 0) { while (i < src.size() && src[i] != '\n') i++; continue; }
        if (src.compare(i, 2, "/*") == 0) {
            size_t e = src.find("*/", i + 2);
            i = (e == std::string::npos) ? src.size() : e + 2;
            continue;
        }
        if (src[i] == '"') {   // keep string literals whole (a "//" inside one is not a comment)
            out += src[i++];
            while (i < src.size() && src[i] != '"') { if (src[i] == '\\') out += src[i++]; out += src[i++]; }
            if (i < src.size()) out += src[i++];
            continue;
        }
        out += src[i++];
    }
    return out;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    printf("test_action_log_row (L18-A)\n\ndecision\n");

    // 1. The receipt of a queued war is PENDING, and the poll state is its own field.
    ActionLogVerdict v = action_log_verdict_of(kPilotReceipt);
    check(std::string(v.verdict) == "pending", "the pilot's queued-war receipt reads verdict pending", v.verdict);
    check(std::string(v.poll) == "skipped_on_game_thread",
          "...and poll skipped_on_game_thread (the poll's state, not the action's)", v.poll);

    // CONTROL, non-discriminating: the 0a969f2e row carried no verdict, so a
    // reader had only result.success — false for every landed war.
    {
        rapidjson::Document d = parse(kPilotReceipt);
        check(d.IsObject() && d["success"].IsBool() && !d["success"].GetBool(),
              "[control, non-discriminating] the raw receipt's success is false: what the pre-fix row said");
    }

    // 2. Other reply shapes the writer sees.
    v = action_log_verdict_of(R"({"success":true,"action":"call_war_create_core","status":"war_created"})");
    check(std::string(v.verdict) == "applied" && std::string(v.poll) == "answered",
          "an off-game-thread poll that found war_created: applied, answered",
          std::string(v.verdict) + "/" + v.poll);
    v = action_log_verdict_of(R"({"success":false,"status":"rejected","reason":"claim_not_pressed"})");
    check(std::string(v.verdict) == "rejected" && std::string(v.poll) == "answered",
          "a polled engine refusal: rejected, answered", std::string(v.verdict) + "/" + v.poll);
    v = action_log_verdict_of(R"({"success":false,"status":"timeout","reason":"intent_not_completed_within_5s"})");
    check(std::string(v.verdict) == "pending" && std::string(v.poll) == "timed_out",
          "a poll that timed out: pending (the drain may still run it), timed_out",
          std::string(v.verdict) + "/" + v.poll);
    // L18 round 2: the faction handler's reply when its wait ran out (factions.cpp:
    // success false, reason intent_timeout_no_result, no retire): the drain may
    // still run it, so the receipt is pending, not rejected.
    v = action_log_verdict_of(R"({"success":false,"action":"faction_action","faction_action":"join","actor_id":13191,"intent_request_id":"fac_1","dispatch_id":"fac_1","reason":"intent_timeout_no_result"})");
    check(std::string(v.verdict) == "pending" && std::string(v.poll) == "timed_out",
          "a faction receipt whose wait ran out un-retired: pending, timed_out (the outcome row decides)",
          std::string(v.verdict) + "/" + v.poll);
    v = action_log_verdict_of(R"({"success":false,"action":"scheme_action","reason":"intent_timeout_cancelled_not_executed"})");
    check(std::string(v.verdict) == "rejected",
          "[control] a scheme the retire cancelled never runs: rejected", v.verdict);
    v = action_log_verdict_of(R"({"success":true,"action":"set_ai_personality","locked":true})");
    check(std::string(v.verdict) == "applied" && std::string(v.poll) == "not_applicable",
          "a synchronous write: applied, no poll", std::string(v.verdict) + "/" + v.poll);
    v = action_log_verdict_of(R"({"success":false,"action":"create_title","status":"queued","reason":"no_poll_on_game_thread"})");
    check(std::string(v.verdict) == "pending", "create_title's queued receipt (success:false) is pending", v.verdict);
    v = action_log_verdict_of("not json");
    check(std::string(v.verdict) == "unknown", "an unparseable reply is unknown, never rejected", v.verdict);
    v = action_log_verdict_of(R"({"action":"x"})");
    check(std::string(v.verdict) == "unknown", "a reply with no success and no status is unknown", v.verdict);

    // 3. The outcome row for the drain's verdict, attributed by dispatch_id.
    Intent c{};
    c.type = IntentType::WAR_CREATE_CORE_DIRECT;
    c.actor_id = 10647; c.target_id = 11840;
    c.request_id = "wcc_10647_33_44582";
    c.session_id = "sess_000000004896e4ae_0000001a";
    c.consumed = true; c.executed = true; c.rejected = false;
    c.console_output = R"({"war_created":true,"war_id":11,"is_null_sentinel":false,)"
                       R"("wars_before":11,"wars_after":12,"resolved_cb":"nomadic_conquest_cb"})";
    rapidjson::Document row = parse(action_outcome_row("2026-11-04T00:10:56.100Z", "0867.02.04", c));
    check(row.IsObject(), "the outcome row is one JSON object");
    if (row.IsObject()) {
        auto s = [&](const char* k) { return row.HasMember(k) && row[k].IsString() ? std::string(row[k].GetString()) : std::string("<absent>"); };
        check(s("kind") == "outcome", "kind outcome", s("kind"));
        check(s("verb") == "intent_outcome", "verb intent_outcome (verb-keyed readers count the call once)", s("verb"));
        check(s("intent_type") == "call_war_create_core", "intent_type names the call", s("intent_type"));
        check(s("dispatch_id") == "wcc_10647_33_44582",
              "dispatch_id equals the receipt row's result.dispatch_id (attribution)", s("dispatch_id"));
        check(s("session_id") == c.session_id, "session_id is the queuing session's", s("session_id"));
        check(s("verdict") == "applied", "the landed war's verdict is applied", s("verdict"));
        check(!row.HasMember("reason"), "an applied outcome carries no reason");
        check(row.HasMember("result_raw") && row["result_raw"].IsObject() &&
              row["result_raw"]["war_id"].GetInt() == 11,
              "result_raw carries the executor's object (war_id) as an object");
    }

    c.executed = false; c.rejected = true;
    snprintf(c.rejection_reason, sizeof c.rejection_reason, "ttl_expired");
    c.console_output = "plain text";
    row = parse(action_outcome_row("t", "d", c));
    check(row.IsObject() && std::string(row["verdict"].GetString()) == "rejected" &&
          std::string(row["reason"].GetString()) == "ttl_expired" && row["result_raw"].IsString(),
          "an expired intent: rejected, reason ttl_expired, non-JSON output kept as a string");
    c.rejection_reason[0] = '\0';
    row = parse(action_outcome_row("t", "d", c));
    check(row.IsObject() && std::string(row["reason"].GetString()) == "not_executed",
          "a rejection with no reason says not_executed, never an empty reason");

    // 4. Wiring: the product's writer and drain use the decision.
    printf("\nwiring\n");
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string router = strip_comments(read_file(dir + "/src/dispatch_router.cpp"));
    const std::string init = strip_comments(read_file(dir + "/src/bridge_init.cpp"));
    check(!router.empty() && !init.empty(), "the two product sources were read", dir);
    const size_t emit = router.find("static void emit_action_log(");
    const size_t emit_end = router.find("static void append_action_log_line(const std::string& line) {");
    check(emit != std::string::npos && emit_end != std::string::npos && emit < emit_end &&
          router.substr(emit, emit_end - emit).find("action_log_verdict_of(result_json)") != std::string::npos,
          "emit_action_log writes action_log_verdict_of(result_json) into every row");
    check(router.find("void emit_action_outcome_log(const Intent& completed) {") != std::string::npos &&
          router.find("action_outcome_row(") != std::string::npos,
          "emit_action_outcome_log is defined and writes action_outcome_row");
    const size_t maint = init.find("g_intent_queue.tick_maintenance(tick, completed);");
    const size_t out = init.find("emit_action_outcome_log(c);", maint == std::string::npos ? 0 : maint);
    check(maint != std::string::npos && out != std::string::npos && out - maint < 400,
          "the drain emits an outcome row for each intent tick_maintenance completes");

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
