// test/test_action_args.cpp — regression tests for ck-6kvq.15.
//
// THE BUG: pipeline `check` returned {"success":true,"results":[]} for every
// ticket in every state, so the async API's documented retrieval path was
// wholly non-functional while reporting health. Root cause: handle_check read
// inner["tickets"] top-level only, but every real client frames its arguments
// under "params" (mcp_server.py:955 mints
// {"action":..., "params":{...}, "request_id":...}). handle_cancel and
// handle_turn_done had the identical bug.
//
// These tests drive the resolver with the EXACT wire shapes a real client puts
// on the socket, and carry a NEGATIVE CONTROL: legacy_lookup() reproduces the
// pre-fix read, and the control must FAIL every assertion the fix passes. A
// test that passes with and without the fix proves nothing.
//
//   cmake -DBUILD_BRIDGE_TESTS=ON .. && make -j$(nproc) test_action_args
//   ./test_action_args

#include "action_args.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace bridge;

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

// The pre-fix read, kept verbatim as the negative control.
static bool legacy_lookup(const rapidjson::Value& inner, const char* key) {
    return inner.HasMember(key);
}

static rapidjson::Document parse(const char* json) {
    rapidjson::Document d;
    d.Parse(json);
    return d;
}

// The literal frame the probe sent when it reproduced ck-6kvq.15 live.
static const char* kFramedCheck =
    "{\"action\":\"check\",\"params\":{\"tickets\":[\"tk_00000005\"]},"
    "\"request_id\":\"3312f62dca3e469ca72cb0831d439598\"}";
static const char* kFlatCheck =
    "{\"action\":\"check\",\"tickets\":[\"tk_00000005\"],"
    "\"request_id\":\"3312f62dca3e469ca72cb0831d439598\"}";

static void test_framed_check_resolves() {
    std::printf("framed check (the live ck-6kvq.15 repro)\n");
    rapidjson::Document d = parse(kFramedCheck);
    CHECK(!d.HasParseError());

    CHECK(action_has(d, "tickets"));
    const rapidjson::Value& a = action_args(d, "tickets")["tickets"];
    CHECK(a.IsArray());
    CHECK(a.Size() == 1);
    CHECK(a[0].IsString() && std::strcmp(a[0].GetString(), "tk_00000005") == 0);

    // NEGATIVE CONTROL: the pre-fix read finds nothing here. If this ever
    // starts passing, the control has stopped controlling and these tests are
    // no longer evidence of anything.
    CHECK(legacy_lookup(d, "tickets") == false);
}

static void test_flat_check_still_resolves() {
    std::printf("flat check (raw-socket caller — must not regress)\n");
    rapidjson::Document d = parse(kFlatCheck);
    CHECK(action_has(d, "tickets"));
    const rapidjson::Value& a = action_args(d, "tickets")["tickets"];
    CHECK(a.IsArray() && a.Size() == 1);
    // The flat form is the ONE case the old read got right, which is exactly
    // why my hand-rolled probe passed while every real client failed.
    CHECK(legacy_lookup(d, "tickets") == true);
}

static void test_singular_ticket_both_framings() {
    std::printf("singular {ticket:...}, both framings\n");
    rapidjson::Document f = parse("{\"action\":\"check\",\"params\":{\"ticket\":\"tk_00000009\"}}");
    CHECK(action_has(f, "ticket"));
    CHECK(std::strcmp(action_args(f, "ticket")["ticket"].GetString(), "tk_00000009") == 0);
    CHECK(legacy_lookup(f, "ticket") == false);

    rapidjson::Document l = parse("{\"action\":\"check\",\"ticket\":\"tk_00000009\"}");
    CHECK(action_has(l, "ticket"));
    CHECK(std::strcmp(action_args(l, "ticket")["ticket"].GetString(), "tk_00000009") == 0);
}

static void test_cancel_and_turn_done_shapes() {
    std::printf("cancel {ticket} and turn_done {checkpoint}, framed\n");
    // cancel: the bug made every framed cancel answer unknown_ticket no matter
    // what state the ticket was really in.
    rapidjson::Document c = parse("{\"action\":\"cancel\",\"params\":{\"ticket\":\"tk_00000003\"}}");
    CHECK(action_has(c, "ticket"));
    CHECK(std::strcmp(action_args(c, "ticket")["ticket"].GetString(), "tk_00000003") == 0);
    CHECK(legacy_lookup(c, "ticket") == false);

    // turn_done: the bug silently ignored an explicit checkpoint and fell
    // through to "whichever is open" — the exact off-by-one the explicit
    // argument exists to prevent, and silent, which is worse than an error.
    rapidjson::Document t = parse("{\"action\":\"turn_done\",\"params\":{\"checkpoint\":7}}");
    CHECK(action_has(t, "checkpoint"));
    CHECK(action_args(t, "checkpoint")["checkpoint"].GetInt64() == 7);
    CHECK(legacy_lookup(t, "checkpoint") == false);
}

static void test_absence_is_distinguishable_from_empty() {
    std::printf("absent != empty (the shape that let the bug ship)\n");
    // Neither key anywhere: the handler must be able to tell this from an
    // explicitly empty list, so it can refuse in a typed way instead of
    // answering success with nothing in it.
    rapidjson::Document none = parse("{\"action\":\"check\",\"request_id\":\"abc\"}");
    CHECK(!action_has(none, "tickets"));
    CHECK(!action_has(none, "ticket"));

    // An explicitly empty array IS present — a legitimate, successful no-op.
    rapidjson::Document empty = parse("{\"action\":\"check\",\"params\":{\"tickets\":[]}}");
    CHECK(action_has(empty, "tickets"));
    CHECK(action_args(empty, "tickets")["tickets"].Size() == 0);

    // params present but without the key: fall back to inner, report absent.
    rapidjson::Document other = parse("{\"action\":\"check\",\"params\":{\"unrelated\":1}}");
    CHECK(!action_has(other, "tickets"));
}

static void test_mixed_and_degenerate_framings() {
    std::printf("mixed and degenerate framings\n");
    // request_id flat, arguments framed — the real mcp_server.py:955 shape.
    // Per-key resolution is why this works.
    rapidjson::Document mixed = parse(
        "{\"action\":\"check\",\"request_id\":\"rid\",\"params\":{\"tickets\":[\"tk_1\"]}}");
    CHECK(action_has(mixed, "tickets"));
    CHECK(!action_has(mixed, "nope"));

    // Top-level wins when a key sits in BOTH positions: a caller that set it
    // explicitly and flat meant it.
    rapidjson::Document both = parse(
        "{\"action\":\"check\",\"ticket\":\"flat\",\"params\":{\"ticket\":\"framed\"}}");
    CHECK(std::strcmp(action_args(both, "ticket")["ticket"].GetString(), "flat") == 0);

    // params present but not an object — must not crash, must report absent.
    rapidjson::Document bad = parse("{\"action\":\"check\",\"params\":\"oops\"}");
    CHECK(!action_has(bad, "tickets"));

    // A non-object inner, and a null key, must be survivable.
    rapidjson::Document arr = parse("[1,2,3]");
    CHECK(!action_has(arr, "tickets"));
    CHECK(!action_has(mixed, nullptr));
}

int main() {
    std::printf("test_action_args — ck-6kvq.15 params-fallback regression\n\n");
    test_framed_check_resolves();
    test_flat_check_still_resolves();
    test_singular_ticket_both_framings();
    test_cancel_and_turn_done_shapes();
    test_absence_is_distinguishable_from_empty();
    test_mixed_and_degenerate_framings();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
