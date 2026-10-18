// test/test_budget_ledger.cpp — acceptance for R6, the decision-budget
// governor's enforcing half (ck-6kvq.13).
//
// Standalone: compiles budget_ledger.cpp with NO engine, NO game, NO CK3
// binary and not even a stub global, because enforcement is a pure rule and
// must be provable without a running game.
//
//   cmake -DBUILD_BRIDGE_TESTS=ON .. && make -j$(nproc) test_budget_ledger
//   ./test_budget_ledger                       # the unit acceptance sections
//   ./test_budget_ledger --replay events.jsonl # drive the REAL ledger from a
//                                              # recorded fixture, JSONL out
//
// The --replay mode exists so scripts/test_budget_governor.py can feed a
// RECORDED CLAUDE-CLI TRANSCRIPT through this exact code rather than through a
// python re-implementation of it. A second opinion about when an agent is out
// of budget is precisely what this bead must not ship.

#include "budget_ledger.h"

#include <rapidjson/document.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace bridge;

// ---------------------------------------------------------------------------
// Assertion harness (independent of NDEBUG).
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!((a) == (b))) {                                                  \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                     \
    } while (0)

static void section(const char* name) { std::printf("[ %s ]\n", name); }

static BudgetUsage U(int64_t out, int64_t unc, int64_t cr = 0, int64_t cw = 0) {
    BudgetUsage u;
    u.output_tokens = out;
    u.uncached_input_tokens = unc;
    u.cache_read_tokens = cr;
    u.cache_write_tokens = cw;
    return u;
}

// Returns the row, or a default-constructed one when absent. `has_row` is the
// membership test. Deliberately NOT a pointer: a CHECK() that fails does not
// abort, so a test that dereferenced a missing row would segfault instead of
// reporting — and a mutant that deletes rows would "crash" rather than FAIL.
static bool has_row(const std::vector<BudgetRow>& rows, const char* sid, int32_t year) {
    for (const BudgetRow& r : rows)
        if (r.session_id == sid && r.game_year == year) return true;
    return false;
}

static BudgetRow find_row(const std::vector<BudgetRow>& rows,
                          const char* sid, int32_t year) {
    for (const BudgetRow& r : rows)
        if (r.session_id == sid && r.game_year == year) return r;
    return BudgetRow();
}

// ---------------------------------------------------------------------------
// Total JSON accessors. CHECK() does not abort, so a section that indexed a
// rapidjson array directly would SEGFAULT under a mutant that empties it
// (RAPIDJSON_ASSERT compiles out under NDEBUG) — crashing instead of reporting
// which assertion broke. These read defensively and let every CHECK run.
// ---------------------------------------------------------------------------
static const rapidjson::Value& jat(const rapidjson::Value& arr, rapidjson::SizeType i) {
    static const rapidjson::Value kNull;
    if (!arr.IsArray() || i >= arr.Size()) return kNull;
    return arr[i];
}

static const rapidjson::Value& jget(const rapidjson::Value& obj, const char* k) {
    static const rapidjson::Value kNull;
    if (!obj.IsObject() || !obj.HasMember(k)) return kNull;
    return obj[k];
}

static int64_t jnum(const rapidjson::Value& obj, const char* k) {
    const rapidjson::Value& v = jget(obj, k);
    if (v.IsInt64()) return v.GetInt64();
    if (v.IsInt())   return v.GetInt();
    return -1;   // never a legal ledger value, so a miss is visible
}

static std::string jtext(const rapidjson::Value& obj, const char* k) {
    const rapidjson::Value& v = jget(obj, k);
    return v.IsString() ? std::string(v.GetString()) : std::string("<absent>");
}

static bool jflag(const rapidjson::Value& obj, const char* k) {
    const rapidjson::Value& v = jget(obj, k);
    return v.IsBool() && v.GetBool();
}

static rapidjson::SizeType jsize(const rapidjson::Value& v) {
    return v.IsArray() ? v.Size() : 0;
}

// ---------------------------------------------------------------------------
// 1. What is counted: output + uncached input, per session, per GAME YEAR.
// ---------------------------------------------------------------------------
static void test_spend_accrues_per_session_per_game_year() {
    section("spend accrues per session per GAME YEAR");
    BudgetLedger L;
    L.configure(0, true);              // record-only: accrual must not need a cap

    L.observe_clock(1066, "1066.09.15");
    L.report("earl-mercia",  "m1", U(100,  40, 9000, 500));
    L.report("earl-mercia",  "m2", U(200,  60, 8000, 100));
    L.report("king-harold",  "m3", U(500, 250,    0,   0));

    L.observe_clock(1067, "1067.01.01");
    L.report("earl-mercia",  "m4", U(10,   5));

    std::vector<BudgetRow> rows = L.rows();
    CHECK_EQ(rows.size(), static_cast<size_t>(3));

    CHECK(has_row(rows, "earl-mercia", 1066));
    BudgetRow m66 = find_row(rows, "earl-mercia", 1066);
    CHECK_EQ(m66.usage.output_tokens, 300);
    CHECK_EQ(m66.usage.uncached_input_tokens, 100);
    CHECK_EQ(m66.usage.billed(), 400);
    CHECK_EQ(m66.messages, 2);
    CHECK_EQ(m66.first_charge_date, std::string("1066.09.15"));

    // The next game year is a SEPARATE row, not a continuation.
    CHECK(has_row(rows, "earl-mercia", 1067));
    CHECK_EQ(find_row(rows, "earl-mercia", 1067).usage.billed(), 15);

    CHECK(has_row(rows, "king-harold", 1066));
    CHECK_EQ(find_row(rows, "king-harold", 1066).usage.billed(), 750);
    // Sessions do not pool: one agent's spend is invisible to another's.
    CHECK(!has_row(rows, "king-harold", 1067));
}

// ---------------------------------------------------------------------------
// 2. Cache traffic is RECORDED and NEVER billed (D4).
// ---------------------------------------------------------------------------
static void test_cache_recorded_never_billed() {
    section("cache read/write recorded, never billed (D4)");
    BudgetLedger L;
    L.configure(1000, true);
    L.observe_clock(1066, "1066.09.15");

    // A million cache tokens against a 1000-token ceiling. If cache were billed
    // this session would be spent on its first report.
    BudgetVerdict v = L.report("s1", "m1", U(10, 10, 1000000, 500000));
    CHECK(v.allowed);
    CHECK_EQ(v.spend, 20);
    CHECK(!L.is_exhausted("s1"));

    CHECK(has_row(L.rows(), "s1", 1066));
    BudgetRow r = find_row(L.rows(), "s1", 1066);
    CHECK_EQ(r.usage.cache_read_tokens, 1000000);
    CHECK_EQ(r.usage.cache_write_tokens, 500000);
    CHECK_EQ(r.usage.billed(), 20);
}

// ---------------------------------------------------------------------------
// 3. The line: the refusal fires EXACTLY at the ceiling, and not before.
// ---------------------------------------------------------------------------
static void test_refusal_fires_exactly_at_the_ceiling() {
    section("refusal fires EXACTLY at the ceiling");
    BudgetLedger L;
    L.configure(1000, true);
    L.observe_clock(1066, "1066.09.15");

    // One token short: still spending.
    BudgetVerdict v = L.report("s1", "m1", U(600, 399));
    CHECK_EQ(v.spend, 999);
    CHECK(v.allowed);
    CHECK(!v.crossed);
    CHECK(L.evaluate("s1", "declare_war", true).allowed);
    CHECK_EQ(L.refusal_count(), 0);

    // The token that reaches the ceiling spends it. An allowance fully used is
    // an allowance with nothing left.
    v = L.report("s1", "m2", U(1, 0));
    CHECK_EQ(v.spend, 1000);
    CHECK(!v.allowed);
    CHECK(v.crossed);
    CHECK_EQ(v.reason, std::string("budget_exhausted"));
    CHECK(L.is_exhausted("s1"));

    BudgetVerdict d = L.evaluate("s1", "declare_war", true);
    CHECK(!d.allowed);
    CHECK_EQ(d.reason, std::string("budget_exhausted"));
    CHECK_EQ(d.session_id, std::string("s1"));
    CHECK_EQ(d.game_date, std::string("1066.09.15"));
    CHECK_EQ(d.spend, 1000);
    CHECK_EQ(d.ceiling, 1000);
    CHECK_EQ(d.resets_at, std::string("1067.01.01"));
    CHECK_EQ(L.refusal_count(), 1);

    // A second agent is unaffected: the ceiling is per agent.
    CHECK(L.evaluate("s2", "declare_war", true).allowed);
    CHECK_EQ(L.refusal_count(), 1);
}

// ---------------------------------------------------------------------------
// 4. MUTATIONS ONLY. A spent agent keeps reading, logging and settling.
// ---------------------------------------------------------------------------
static void test_reads_never_refused() {
    section("MUTATIONS ONLY — reads and management verbs never refused");
    BudgetLedger L;
    L.configure(100, true);
    L.observe_clock(1066, "1066.09.15");
    L.report("s1", "m1", U(100, 0));
    CHECK(L.is_exhausted("s1"));

    // is_mutation=false is the dispatch choke point's own classification.
    CHECK(L.evaluate("s1", "query_realm", false).allowed);
    CHECK(L.evaluate("s1", "get_tick_info", false).allowed);
    CHECK_EQ(L.refusal_count(), 0);

    // And the gate predicate itself: what a spent session may still call.
    //                      action              mutation  management
    CHECK(!budget_gate_applies("query_realm",      false, false));
    CHECK(!budget_gate_applies("get_tick_info",    false, true));
    CHECK(!budget_gate_applies("pause_game",       true,  true));
    CHECK(!budget_gate_applies("report_usage",     true,  false));
    CHECK(!budget_gate_applies("get_budget_state", false, false));
    CHECK(!budget_gate_applies("check",            true,  false));
    CHECK(!budget_gate_applies("cancel",           true,  false));
    CHECK(!budget_gate_applies("board_read",       true,  false));
    CHECK(!budget_gate_applies("",                 true,  false));
    // ...and what it may not.
    CHECK(budget_gate_applies("declare_war",       true,  false));
    CHECK(budget_gate_applies("send_interaction",  true,  false));
    CHECK(budget_gate_applies("board_post",        true,  false));
    CHECK(budget_gate_applies("set_ai_personality",true,  false));

    // Every refused mutation is COUNTED, per session and per year.
    L.evaluate("s1", "declare_war", true);
    L.evaluate("s1", "send_interaction", true);
    L.evaluate("s1", "query_realm", false);
    CHECK_EQ(L.refusal_count(), 2);
    CHECK_EQ(L.refusal_count_for("s1"), 2);
    CHECK(has_row(L.rows(), "s1", 1066));
    CHECK_EQ(find_row(L.rows(), "s1", 1066).refusals, 2);

    std::vector<BudgetRefusal> rf = L.recent_refusals();
    CHECK_EQ(rf.size(), static_cast<size_t>(2));
    BudgetRefusal first = rf.empty() ? BudgetRefusal() : rf.front();
    CHECK_EQ(first.action, std::string("declare_war"));
    CHECK_EQ(first.game_date, std::string("1066.09.15"));
    CHECK_EQ(first.spend, 100);
    CHECK_EQ(first.ceiling, 100);
}

// ---------------------------------------------------------------------------
// 5. The game-year rollover refills the allowance and the session rejoins.
// ---------------------------------------------------------------------------
static void test_year_rollover_refills() {
    section("game-year rollover resets the counter and the session rejoins");
    BudgetLedger L;
    L.configure(100, true);
    L.observe_clock(1066, "1066.09.15");
    L.report("s1", "m1", U(100, 0));
    CHECK(!L.evaluate("s1", "declare_war", true).allowed);

    // Mid-year days do not refill anything.
    L.observe_clock(1066, "1066.12.31");
    CHECK(!L.evaluate("s1", "declare_war", true).allowed);

    // The year rolls: a new key, no entry, full allowance.
    L.observe_clock(1067, "1067.01.01");
    CHECK(!L.is_exhausted("s1"));
    BudgetVerdict v = L.evaluate("s1", "declare_war", true);
    CHECK(v.allowed);
    CHECK_EQ(v.game_year, 1067);
    CHECK_EQ(v.spend, 0);

    // The spent year is still in the record — refilling is not forgetting.
    CHECK(has_row(L.rows(), "s1", 1066));
    BudgetRow r66 = find_row(L.rows(), "s1", 1066);
    CHECK(r66.exhausted);
    CHECK_EQ(r66.refusals, 2);

    // And it can be spent again in the new year, independently.
    L.report("s1", "m2", U(100, 0));
    CHECK(!L.evaluate("s1", "declare_war", true).allowed);
    CHECK_EQ(L.refusal_count(), 3);
}

// ---------------------------------------------------------------------------
// 6. The clock is MONOTONIC: nothing can refill a spent year by going back.
// ---------------------------------------------------------------------------
static void test_clock_is_monotonic() {
    section("a backwards clock cannot refill a spent allowance");
    BudgetLedger L;
    L.configure(100, true);
    L.observe_clock(1070, "1070.03.02");
    L.report("s1", "m1", U(100, 0));
    CHECK(!L.evaluate("s1", "declare_war", true).allowed);

    // A bridge restart, a re-anchor, a glitched read: the year appears to go
    // backwards. It must not hand the agent a fresh allowance.
    L.observe_clock(1066, "1066.09.15");
    CHECK_EQ(L.game_year(), 1070);
    CHECK_EQ(L.clock_regressions(), 1);
    CHECK(L.is_exhausted("s1"));
    CHECK(!L.evaluate("s1", "declare_war", true).allowed);
}

// ---------------------------------------------------------------------------
// 7. A rewritten transcript message is charged once, and never refunds.
// ---------------------------------------------------------------------------
static void test_message_rewrites_charged_once() {
    section("a rewritten message is charged once and never refunds");
    BudgetLedger L;
    L.configure(0, true);
    L.observe_clock(1066, "1066.09.15");

    // The CLI rewrites a message's transcript entry as it streams: the same id
    // arrives three times carrying a growing cumulative total.
    L.report("s1", "msg-a", U(10, 100));
    L.report("s1", "msg-a", U(50, 100));
    L.report("s1", "msg-a", U(90, 100));
    CHECK(has_row(L.rows(), "s1", 1066));
    BudgetRow r = find_row(L.rows(), "s1", 1066);
    CHECK_EQ(r.usage.output_tokens, 90);      // not 150
    CHECK_EQ(r.usage.uncached_input_tokens, 100);
    CHECK_EQ(r.messages, 1);

    // A SHRINKING rewrite is clamped at zero — it cannot refund an agent.
    L.report("s1", "msg-a", U(5, 5));
    r = find_row(L.rows(), "s1", 1066);
    CHECK_EQ(r.usage.output_tokens, 90);
    CHECK_EQ(r.usage.uncached_input_tokens, 100);

    // An empty message id is an explicit delta, charged as given.
    L.report("s1", "", U(7, 0));
    r = find_row(L.rows(), "s1", 1066);
    CHECK_EQ(r.usage.output_tokens, 97);
}

// ---------------------------------------------------------------------------
// 8. No ceiling configured => record everything, refuse nothing.
// ---------------------------------------------------------------------------
static void test_unarmed_ledger_refuses_nothing() {
    section("no ceiling configured: record everything, refuse nothing");
    BudgetLedger L;
    L.configure(0, true);                    // the shipped default
    L.observe_clock(1066, "1066.09.15");
    L.report("s1", "m1", U(10000000, 10000000));
    CHECK(!L.is_exhausted("s1"));
    CHECK(L.evaluate("s1", "declare_war", true).allowed);
    CHECK_EQ(L.refusal_count(), 0);
    CHECK(has_row(L.rows(), "s1", 1066));
    CHECK_EQ(find_row(L.rows(), "s1", 1066).usage.billed(), 20000000);   // recorded in full

    // Explicitly disarmed with a ceiling present: same answer.
    BudgetLedger D;
    D.configure(100, false);
    D.observe_clock(1066, "1066.09.15");
    D.report("s1", "m1", U(100, 100));
    CHECK(!D.is_exhausted("s1"));
    CHECK(D.evaluate("s1", "declare_war", true).allowed);
}

// ---------------------------------------------------------------------------
// 9. The bundle payload carries per-session per-year spend AND refusal counts.
// ---------------------------------------------------------------------------
static void test_export_carries_spend_and_refusals() {
    section("bundle payload: per-session per-year spend + refusal counts");
    BudgetLedger L;
    L.configure(500, true);
    L.observe_clock(1066, "1066.09.15");
    L.report("s1", "m1", U(400, 100, 1234, 56), /*rounds=*/3);
    L.evaluate("s1", "declare_war", true);
    L.observe_clock(1067, "1067.02.10");
    L.report("s1", "m2", U(1, 1), /*rounds=*/1);
    L.report("s2", "m3", U(2, 2), /*rounds=*/1);

    std::string js = L.export_json();
    rapidjson::Document d;
    CHECK(!d.Parse(js.c_str()).HasParseError());
    CHECK(d.IsObject());
    CHECK_EQ(jtext(d, "source"), std::string("bridge"));
    CHECK_EQ(jnum(d, "per_agent_per_year"), 500);
    CHECK_EQ(jsize(jget(d, "spent")), 3u);

    const rapidjson::Value& row0 = jat(jget(d, "spent"), 0);
    CHECK_EQ(jtext(row0, "session_id"), std::string("s1"));
    CHECK_EQ(jnum(row0, "game_year"), 1066);
    CHECK_EQ(jnum(row0, "billed_tokens"), 500);
    CHECK_EQ(jnum(row0, "cached_input_tokens"), 1234);
    CHECK_EQ(jnum(row0, "cache_write_tokens"), 56);
    CHECK_EQ(jnum(row0, "decisions"), 3);
    CHECK(jflag(row0, "exhausted"));
    CHECK_EQ(jnum(row0, "refusals"), 1);

    const rapidjson::Value& refs = jget(d, "refusals");
    CHECK_EQ(jnum(refs, "total"), 1);
    CHECK_EQ(jnum(jget(refs, "by_session"), "s1"), 1);
    CHECK_EQ(jsize(jget(refs, "recent")), 1u);
    const rapidjson::Value& rf = jat(jget(refs, "recent"), 0);
    CHECK_EQ(jtext(rf, "reason"), std::string("budget_exhausted"));
    CHECK_EQ(jtext(rf, "session_id"), std::string("s1"));
    CHECK_EQ(jtext(rf, "game_date"), std::string("1066.09.15"));
    CHECK_EQ(jnum(rf, "spend"), 500);
    CHECK_EQ(jnum(rf, "ceiling"), 500);

    // The 1067 rows exist and are NOT exhausted — the rollover is in the data.
    CHECK(!jflag(jat(jget(d, "spent"), 1), "exhausted"));
    CHECK_EQ(jnum(jat(jget(d, "spent"), 1), "game_year"), 1067);
}

// ---------------------------------------------------------------------------
// --replay: drive this exact ledger from a recorded fixture.
// ---------------------------------------------------------------------------
static int64_t opt_i64(const rapidjson::Value& v, const char* k, int64_t dflt = 0) {
    if (!v.HasMember(k)) return dflt;
    const rapidjson::Value& m = v[k];
    if (m.IsInt64()) return m.GetInt64();
    if (m.IsInt())   return m.GetInt();
    if (m.IsUint())  return static_cast<int64_t>(m.GetUint());
    if (m.IsDouble())return static_cast<int64_t>(m.GetDouble());
    return dflt;
}

static std::string opt_str(const rapidjson::Value& v, const char* k) {
    if (v.HasMember(k) && v[k].IsString()) return v[k].GetString();
    return std::string();
}

static bool opt_bool(const rapidjson::Value& v, const char* k, bool dflt) {
    if (v.HasMember(k) && v[k].IsBool()) return v[k].GetBool();
    return dflt;
}

static int run_replay(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "replay: cannot open %s\n", path);
        return 2;
    }
    BudgetLedger L;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#') continue;
        rapidjson::Document d;
        if (d.Parse(line.c_str()).HasParseError() || !d.IsObject()) {
            std::fprintf(stderr, "replay: bad JSON at line %d\n", lineno);
            return 2;
        }
        const std::string ev = opt_str(d, "ev");
        if (ev == "config") {
            L.configure(opt_i64(d, "ceiling", 0), opt_bool(d, "enabled", true));
        } else if (ev == "clock") {
            L.observe_clock(static_cast<int32_t>(opt_i64(d, "year", 0)),
                            opt_str(d, "date"));
        } else if (ev == "report") {
            BudgetUsage u = U(opt_i64(d, "output"), opt_i64(d, "uncached_input"),
                              opt_i64(d, "cache_read"), opt_i64(d, "cache_write"));
            BudgetVerdict v = L.report(opt_str(d, "session"), opt_str(d, "message_id"),
                                       u, opt_i64(d, "rounds"));
            std::printf("{\"ev\":\"charged\",\"session\":\"%s\",\"game_year\":%d,"
                        "\"spend\":%lld,\"ceiling\":%lld,\"exhausted\":%s,"
                        "\"crossed\":%s}\n",
                        v.session_id.c_str(), static_cast<int>(v.game_year),
                        static_cast<long long>(v.spend),
                        static_cast<long long>(v.ceiling),
                        v.allowed ? "false" : "true",
                        v.crossed ? "true" : "false");
        } else if (ev == "call") {
            const std::string action = opt_str(d, "action");
            const bool applies = budget_gate_applies(
                action.c_str(), opt_bool(d, "mutation", true),
                opt_bool(d, "management", false));
            BudgetVerdict v = L.evaluate(opt_str(d, "session"), action, applies);
            std::printf("{\"ev\":\"verdict\",\"session\":\"%s\",\"action\":\"%s\","
                        "\"gate_applies\":%s,\"allowed\":%s,\"reason\":\"%s\","
                        "\"game_date\":\"%s\",\"spend\":%lld,\"ceiling\":%lld,"
                        "\"resets_at\":\"%s\"}\n",
                        v.session_id.c_str(), action.c_str(),
                        applies ? "true" : "false",
                        v.allowed ? "true" : "false", v.reason.c_str(),
                        v.game_date.c_str(),
                        static_cast<long long>(v.spend),
                        static_cast<long long>(v.ceiling),
                        v.resets_at.c_str());
        } else {
            std::fprintf(stderr, "replay: unknown ev %s at line %d\n",
                         ev.c_str(), lineno);
            return 2;
        }
    }
    // The last line is always the bundle payload.
    std::printf("%s\n", L.export_json().c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--replay") == 0) return run_replay(argv[2]);

    std::printf("test_budget_ledger (ck-6kvq.13, R6)\n");
    test_spend_accrues_per_session_per_game_year();
    test_cache_recorded_never_billed();
    test_refusal_fires_exactly_at_the_ceiling();
    test_reads_never_refused();
    test_year_rollover_refills();
    test_clock_is_monotonic();
    test_message_rewrites_charged_once();
    test_unarmed_ledger_refuses_nothing();
    test_export_carries_spend_and_refusals();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
