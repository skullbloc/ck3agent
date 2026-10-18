// test/test_drain_registry.cpp — regression tests for ck-6kvq.19.
//
// THE BUG. A tick-polarity call is handed to the game thread and settled when
// the game thread reports back. The correlation was keyed by the client's
// `request_id`:
//
//     if (!pc.request_id.empty()) drain_waiting[pc.request_id] = call.ticket;
//
// with no else, and the settle side began
//
//     if (!request_id || !request_id[0]) return;
//
// `request_id` is OPTIONAL on the wire and ~20 shipped clients omit it. For
// every one of those calls the action WAS enqueued and DID execute with real
// side effects, and then: the ticket stayed Running until TTL and then read
// `expired`, the payload was never erased (unbounded growth over an episode),
// and the flush record counted it as a success. Second defect on the same line:
// request_id is client-chosen and not unique — scripts/bdt.sh:531 sends the
// fixed literal "bdt" on every call, so two in-flight bdt tickets collided and
// the first was orphaned.
//
// Every pre-existing test missed this because every one of them supplied a
// distinct request_id. These drive the wire shapes the omitting clients
// actually send.
//
// NEGATIVE CONTROL: test_drain_registry_negative_control builds the SAME
// sources with -DCK3_DRAIN_LEGACY_RID_KEY=1, restoring the request_id-keyed
// correlation verbatim. It MUST fail. A test that passes with and without the
// fix proves nothing.
//
//   cmake -DBUILD_BRIDGE_TESTS=ON .. && make test_drain_registry
//   ./test_drain_registry

#include "drain_registry.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

// Build the payload a submit would have stored. `rid` is what the CLIENT sent:
// "" is the whole point of this bead.
static PendingCall mk(uint64_t ticket, const char* rid, const char* action,
                      const char* inner) {
    PendingCall pc;
    pc.ticket     = ticket;
    pc.session_id = "sess-a";
    pc.request_id = rid ? rid : "";
    pc.action     = action;
    pc.inner_json = inner;
    return pc;
}

// One full round trip: submit -> hand to the drain -> the drain reports back.
// Returns the settled ticket (0 = never settled, i.e. the bug).
static uint64_t round_trip(DrainRegistry& reg, uint64_t ticket) {
    std::string env;
    if (!reg.hand_off(ticket, &env)) return 0;
    // The game thread reads the request_id off the INNER action, exactly as
    // bridge_init.cpp:1604 does. Empty when the client sent none.
    PendingCall pc;
    reg.peek(ticket, &pc);
    return reg.settle_from_drain(env.c_str(), pc.request_id.c_str());
}

// ---------------------------------------------------------------------------
// THE BUG, head on.
// ---------------------------------------------------------------------------
static void test_no_request_id_still_settles() {
    std::printf("a call with NO request_id settles and leaves no payload\n");

    DrainRegistry reg;
    // The literal shape scripts/bridge_test_suite.py:525 puts on the socket:
    // no request_id anywhere.
    reg.put(7, mk(7, "", "declare_war",
                  "{\"action\":\"declare_war\",\"params\":{\"target_id\":14859}}"));
    CHECK(reg.payload_count() == 1);

    const uint64_t settled = round_trip(reg, 7);

    // The whole bead: this was 0 forever.
    CHECK(settled == 7);
    // ...and this was 1 forever, for every such call, for the whole episode.
    CHECK(reg.payload_count() == 0);
    CHECK(!reg.has_payload(7));
    CHECK(reg.outstanding_count() == 0);
    CHECK(!reg.outstanding(7));
}

// ---------------------------------------------------------------------------
// The second defect on the same line: a REUSED request_id.
// ---------------------------------------------------------------------------
static void test_shared_request_id_both_settle() {
    std::printf("two in-flight calls sharing one request_id (the bdt.sh case)\n");

    DrainRegistry reg;
    // scripts/bdt.sh:531 sends the fixed literal "bdt" on every call.
    reg.put(11, mk(11, "bdt", "get_tick_info", "{\"action\":\"get_tick_info\"}"));
    reg.put(12, mk(12, "bdt", "get_tick_info", "{\"action\":\"get_tick_info\"}"));
    CHECK(reg.payload_count() == 2);

    // Both go out before either comes back — that is what "in flight" means.
    std::string env11, env12;
    CHECK(reg.hand_off(11, &env11));
    CHECK(reg.hand_off(12, &env12));
    CHECK(reg.outstanding_count() == 2);   // was 1: the second overwrote the first

    CHECK(reg.settle_from_drain(env11.c_str(), "bdt") == 11);
    CHECK(reg.settle_from_drain(env12.c_str(), "bdt") == 12);

    CHECK(reg.payload_count() == 0);
    CHECK(reg.outstanding_count() == 0);
}

// ---------------------------------------------------------------------------
// No regression for the clients that DO mint one.
// ---------------------------------------------------------------------------
static void test_distinct_request_ids_still_settle() {
    std::printf("distinct request_ids (mcp_server.py / harness) still settle\n");

    DrainRegistry reg;
    reg.put(21, mk(21, "3312f62dca3e469ca72cb0831d439598", "set_personality",
                   "{\"action\":\"set_personality\",\"request_id\":\"3312f62dca3e469ca72cb0831d439598\"}"));
    reg.put(22, mk(22, "b7d1a0", "raise_armies", "{\"action\":\"raise_armies\"}"));

    CHECK(round_trip(reg, 21) == 21);
    CHECK(round_trip(reg, 22) == 22);
    CHECK(reg.payload_count() == 0);
}

// ---------------------------------------------------------------------------
// Mixed traffic: the omitting client and the minting client interleaved. This
// is the realistic case — the overlay poller and bdt.sh and the test suite all
// talk to one bridge at once.
// ---------------------------------------------------------------------------
static void test_mixed_fleet_interleaved() {
    std::printf("mixed fleet: rid-less, shared-rid and unique-rid interleaved\n");

    DrainRegistry reg;
    reg.put(31, mk(31, "",    "get_tick_info", "{\"action\":\"get_tick_info\"}"));
    reg.put(32, mk(32, "bdt", "query_realm",   "{\"action\":\"query_realm\"}"));
    reg.put(33, mk(33, "u-1", "query_character","{\"action\":\"query_character\"}"));
    reg.put(34, mk(34, "bdt", "query_realm",   "{\"action\":\"query_realm\"}"));
    reg.put(35, mk(35, "",    "declare_war",   "{\"action\":\"declare_war\"}"));

    std::vector<std::string> envs(36);
    for (uint64_t t = 31; t <= 35; ++t) CHECK(reg.hand_off(t, &envs[t]));
    CHECK(reg.outstanding_count() == 5);

    // The drain reports back out of order, as it does.
    const uint64_t order[] = {33, 35, 31, 34, 32};
    for (uint64_t t : order) {
        PendingCall pc;
        reg.peek(t, &pc);
        CHECK(reg.settle_from_drain(envs[t].c_str(), pc.request_id.c_str()) == t);
    }
    CHECK(reg.payload_count() == 0);
    CHECK(reg.outstanding_count() == 0);
}

// ---------------------------------------------------------------------------
// The tag must ride on the ENVELOPE and never on the inner action, or every
// handler would see a key its client did not write (and action_args would
// resolve it).
// ---------------------------------------------------------------------------
static void test_tag_is_on_the_envelope_not_the_action() {
    std::printf("the ticket tag rides on the envelope, not the inner action\n");

    const std::string inner =
        "{\"action\":\"declare_war\",\"params\":{\"target_id\":14859}}";
    const std::string env = DrainRegistry::build_envelope("sess-a", inner, 7);

    rapidjson::Document d;
    d.Parse(env.c_str());
    CHECK(!d.HasParseError());
    CHECK(d.IsObject());
    CHECK(d.HasMember("actions") && d["actions"].IsArray() && d["actions"].Size() == 1);

    // The inner action is byte-identical to what the client sent.
    const rapidjson::Value& a = d["actions"][0];
    CHECK(!a.HasMember(kDrainTicketKey));
    CHECK(a.HasMember("params") && a["params"].IsObject());
    CHECK(!a["params"].HasMember(kDrainTicketKey));

    // And the tag reads back off the envelope.
    CHECK(DrainRegistry::ticket_from_envelope(d) == 7);
    CHECK(DrainRegistry::ticket_from_envelope(env.c_str()) == 7);

    // Session id is escaped, not concatenated blindly.
    const std::string weird =
        DrainRegistry::build_envelope("a\"b\\c", "{\"action\":\"x\"}", 9);
    rapidjson::Document w;
    w.Parse(weird.c_str());
    CHECK(!w.HasParseError());
    CHECK(std::strcmp(w["session_id"].GetString(), "a\"b\\c") == 0);
}

// ---------------------------------------------------------------------------
// Envelopes the conductor did NOT send must pass straight through. Every other
// enqueuer in the bridge lands on the same drain.
// ---------------------------------------------------------------------------
static void test_foreign_envelopes_are_not_ours() {
    std::printf("a non-conductor envelope settles nothing\n");

    DrainRegistry reg;
    reg.put(41, mk(41, "", "declare_war", "{\"action\":\"declare_war\"}"));
    std::string env;
    CHECK(reg.hand_off(41, &env));

    // An intent-queue envelope: same shape, no tag.
    const char* foreign =
        "{\"session_id\":\"sess-a\",\"actions\":[{\"action\":\"declare_war\"}]}";
    CHECK(DrainRegistry::ticket_from_envelope(foreign) == 0);
    CHECK(reg.settle_from_drain(foreign, "") == 0);
    CHECK(reg.settle_from_drain(foreign, "bdt") == 0);
    CHECK(reg.payload_count() == 1);       // still in flight, untouched
    CHECK(reg.outstanding(41));

    // Garbage must be survivable, not fatal.
    CHECK(DrainRegistry::ticket_from_envelope("not json") == 0);
    CHECK(DrainRegistry::ticket_from_envelope("") == 0);
    CHECK(DrainRegistry::ticket_from_envelope((const char*)nullptr) == 0);
    CHECK(reg.settle_from_drain("{\"_pipeline_ticket\":\"seven\"}", "") == 0);
    CHECK(reg.settle_from_drain("{\"_pipeline_ticket\":0}", "") == 0);
    CHECK(reg.settle_from_drain("{\"_pipeline_ticket\":-3}", "") == 0);

    // And the real one still settles afterwards.
    CHECK(reg.settle_from_drain(env.c_str(), "") == 41);
    CHECK(reg.payload_count() == 0);
}

// ---------------------------------------------------------------------------
// A ticket settles exactly once. A duplicate drain report must not erase a
// LATER ticket's payload or double-complete.
// ---------------------------------------------------------------------------
static void test_settle_is_idempotent() {
    std::printf("a ticket settles exactly once\n");

    DrainRegistry reg;
    reg.put(51, mk(51, "", "declare_war", "{\"action\":\"declare_war\"}"));
    std::string env;
    CHECK(reg.hand_off(51, &env));
    CHECK(reg.settle_from_drain(env.c_str(), "") == 51);
    CHECK(reg.settle_from_drain(env.c_str(), "") == 0);   // second report: nothing
    CHECK(!reg.settle(51));
    CHECK(reg.payload_count() == 0);
}

// ---------------------------------------------------------------------------
// Inline service (take) and cancel (drop) must not leave an outstanding
// registration behind, and must not settle.
// ---------------------------------------------------------------------------
static void test_inline_service_and_cancel() {
    std::printf("inline service and cancel consume the payload cleanly\n");

    DrainRegistry reg;
    reg.put(61, mk(61, "", "query_realm", "{\"action\":\"query_realm\"}"));
    PendingCall got;
    CHECK(reg.take(61, &got));
    CHECK(got.action == "query_realm");
    CHECK(got.request_id.empty());
    CHECK(reg.payload_count() == 0);
    CHECK(!reg.take(61, &got));
    // Never handed to the drain, so nothing to settle.
    CHECK(!reg.outstanding(61));
    CHECK(!reg.settle(61));

    reg.put(62, mk(62, "", "declare_war", "{\"action\":\"declare_war\"}"));
    CHECK(reg.drop(62));
    CHECK(!reg.drop(62));
    CHECK(reg.payload_count() == 0);

    // hand_off of a ticket with no payload must fail, not invent one.
    std::string env;
    CHECK(!reg.hand_off(63, &env));
    CHECK(reg.outstanding_count() == 0);
}

// ---------------------------------------------------------------------------
// The conductor thread hands off; the game thread settles. Different threads,
// always. The registry owns its own lock.
// ---------------------------------------------------------------------------
static void test_concurrent_handoff_and_settle() {
    std::printf("concurrent hand-off (conductor) and settle (game thread)\n");

    DrainRegistry reg;
    const uint64_t N = 400;
    for (uint64_t t = 1; t <= N; ++t)
        reg.put(t, mk(t, "", "declare_war", "{\"action\":\"declare_war\"}"));

    std::vector<std::string> envs(N + 1);
    std::thread conductor([&] {
        for (uint64_t t = 1; t <= N; ++t) reg.hand_off(t, &envs[t]);
    });
    conductor.join();

    std::vector<uint64_t> settled_a, settled_b;
    std::thread game_a([&] {
        for (uint64_t t = 1; t <= N; t += 2)
            if (reg.settle_from_drain(envs[t].c_str(), "")) settled_a.push_back(t);
    });
    std::thread game_b([&] {
        for (uint64_t t = 2; t <= N; t += 2)
            if (reg.settle_from_drain(envs[t].c_str(), "")) settled_b.push_back(t);
    });
    game_a.join();
    game_b.join();

    CHECK(settled_a.size() + settled_b.size() == N);
    CHECK(reg.payload_count() == 0);
    CHECK(reg.outstanding_count() == 0);
}

int main() {
    std::printf("test_drain_registry — ck-6kvq.19 ticket-keyed drain settlement\n");
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
    std::printf("*** NEGATIVE CONTROL BUILD (request_id-keyed). "
                "THIS MUST FAIL. ***\n");
#endif
    std::printf("\n");

    test_no_request_id_still_settles();
    test_shared_request_id_both_settle();
    test_distinct_request_ids_still_settle();
    test_mixed_fleet_interleaved();
    test_tag_is_on_the_envelope_not_the_action();
    test_foreign_envelopes_are_not_ours();
    test_settle_is_idempotent();
    test_inline_service_and_cancel();
    test_concurrent_handoff_and_settle();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
