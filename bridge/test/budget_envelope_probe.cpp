// budget_envelope_probe.cpp — DO THE REPORTER'S BYTES REACH THE LEDGER? (ck-etqk)
//
// Takes the INNER ACTION OBJECT the shipping budget_reporter emits, on argv, and
// pushes it through the bridge's OWN extractors into the bridge's OWN ledger:
//
//     json_get_int / json_get_string   (src/json_helpers.cpp)  — real
//     BudgetLedger::report             (src/budget_ledger.cpp) — real
//
// WHAT THIS MEASURES AND WHAT IT DOES NOT. It measures that the BYTES yield the
// right numbers through the real extractors and that the real ledger moves by
// them — which is what "the ledger is fed" means once the frame reaches a
// handler. IT DOES NOT LINK do_report_usage ITSELF: that lives in
// dispatch_router.cpp behind the ipc_server ladder, and the four extraction
// lines here mirror it rather than being it. Stated plainly rather than dressed
// up: the envelope GATE — the thing ck-etqk is actually about — is graded in
// §A/§B of the acceptance by enumerating the real consumers, and the live
// confirmation is NOT EVALUABLE without an arm world.
//
// Usage:  budget_envelope_probe '<inner action json>'
// Prints ONE json object.

#include "budget_ledger.h"
#include "action_dispatch_internal.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: probe '<inner-action-json>'\n"); return 2; }
    const char* params_json = argv[1];

    // EXACTLY the reads do_report_usage performs, in the same order.
    const char* explicit_sid = bridge::json_get_string(params_json, "session_id");
    std::string sid = explicit_sid ? explicit_sid : "anon";

    bridge::BudgetUsage u;
    u.output_tokens         = bridge::json_get_int(params_json, "output_tokens");
    u.uncached_input_tokens = bridge::json_get_int(params_json, "uncached_input_tokens");
    u.cache_read_tokens     = bridge::json_get_int(params_json, "cache_read_tokens");
    u.cache_write_tokens    = bridge::json_get_int(params_json, "cache_write_tokens");
    const int64_t rounds    = bridge::json_get_int(params_json, "rounds");
    const char* msg_id      = bridge::json_get_string(params_json, "message_id");

    bridge::BudgetVerdict v = bridge::budget_ledger().report(
        sid, msg_id ? msg_id : "", u, rounds);

    printf("{\"session_id\":\"%s\",\"output\":%lld,\"uncached_input\":%lld"
           ",\"cache_read\":%lld,\"cache_write\":%lld,\"rounds\":%lld"
           ",\"billed\":%lld,\"ledger_spend\":%lld}\n",
           sid.c_str(),
           (long long)u.output_tokens, (long long)u.uncached_input_tokens,
           (long long)u.cache_read_tokens, (long long)u.cache_write_tokens,
           (long long)rounds, (long long)u.billed(), (long long)v.spend);
    return 0;
}
