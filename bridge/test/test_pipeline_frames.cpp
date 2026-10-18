// test/test_pipeline_frames.cpp — regression tests for ck-6kvq.18 (bridge half).
//
// THE BUG. A pipeline result frame is the bridge's envelope with the HANDLER's
// JSON spliced in. It carried NO `type` key. For a client that also sent no
// request_id — and ~20 shipped clients send none — that frame had neither a
// type nor a key: unroutable, and impossible to skip-list the way
// tick_start/observation/notification are skip-listed. So those clients
// returned the acceptance ACK as their answer instead, silently, forever
// one frame out of phase (see scripts/test_pipeline_client_correlation.py for
// the client half).
//
// The frame is now typed. These tests parse every produced frame with rapidjson
// and assert it is valid JSON with exactly one, correct, frame type — this
// function rewrites EVERY pipeline reply, so a malformed frame here breaks the
// whole mode at once.
//
// ONE OF THESE TESTS ALREADY EARNED ITS KEEP. The first implementation appended
// the frame type after the handler's keys and reasoned "JSON is last-key-wins,
// so ours shadows a handler's own type". That is true of python's json.loads
// and FALSE of rapidjson, whose FindMember returns the FIRST occurrence — so a
// duplicate `type` would have been read as two different frame types by the
// bridge's own parser and by every python client. The test failed on the fixed
// build and the fix became: rename a handler's top-level "type" to
// "handler_type" so exactly one is emitted.
//
// NEGATIVE CONTROL: test_pipeline_frames_negative_control builds the same
// source with -DCK3_PIPELINE_FRAME_UNTYPED=1, restoring the untyped close. It
// MUST fail.
//
//   cmake -DBUILD_BRIDGE_TESTS=ON .. && make test_pipeline_frames
//   ./test_pipeline_frames

#include "pipeline_frames.h"

#include "rapidjson/document.h"

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

// The envelope prefix pipeline.cpp has already built by the time it splices.
static std::string open_frame(const char* request_id = "rid-1") {
    std::string f = "{";
    if (request_id && request_id[0]) {
        f += "\"request_id\":\"";
        f += request_id;
        f += "\",";
    }
    f += "\"tick\":4211,\"game_date\":\"1066.9.15\",\"ticket\":\"tk_00000007\"";
    f += ",\"flush_id\":3,\"bridge_record\":{\"session_id\":\"s\",";
    f += "\"character_id\":14859,\"flush_id\":3,\"tick_executed\":4211,";
    f += "\"game_date_executed\":\"1066.9.15\"}";
    return f;
}

static std::string build(const char* result_json, const char* action = "query_character") {
    std::string f = open_frame();
    frame_append_result_body(f, result_json ? result_json : "", action, "result");
    return f;
}

// Every frame this function emits must parse. That is the floor.
static rapidjson::Document must_parse(const std::string& frame) {
    rapidjson::Document d;
    d.Parse(frame.c_str());
    ++g_checks;
    if (d.HasParseError() || !d.IsObject()) {
        ++g_failures;
        std::printf("  FAIL: frame does not parse as an object: %s\n",
                    frame.c_str());
    }
    return d;
}

// Safe string read: the NEGATIVE CONTROL build produces frames with no `type`
// at all, and rapidjson's operator[] aborts on a missing member. A control must
// FAIL with a readable report, not SIGABRT.
static const char* str_or(const rapidjson::Value& d, const char* key,
                          const char* fallback = "<absent>") {
    if (!d.IsObject()) return fallback;
    auto it = d.FindMember(key);
    if (it == d.MemberEnd() || !it->value.IsString()) return fallback;
    return it->value.GetString();
}

static void test_ordinary_handler_reply() {
    std::printf("an ordinary handler reply is spliced and typed\n");
    const std::string f = build(
        "{\"success\":true,\"character_id\":14859,\"gold\":123.45}");
    rapidjson::Document d = must_parse(f);
    if (d.HasParseError()) return;

    // THE bead: the frame can be named.
    CHECK(std::strcmp(str_or(d, "type"), "result") == 0);
    // ...without losing anything it carried before.
    CHECK(d.HasMember("request_id"));
    CHECK(d.HasMember("ticket"));
    CHECK(d.HasMember("bridge_record") && d["bridge_record"].IsObject());
    CHECK(d.HasMember("success") && d["success"].GetBool());
    CHECK(d.HasMember("gold") && d["gold"].GetDouble() > 123.0);
    CHECK(d.HasMember("character_id") && d["character_id"].GetInt() == 14859);
}

// THE test that corrected the design. A duplicate `type` is not a tie-break,
// it is a disagreement: rapidjson resolves it first-wins, python last-wins.
// Exactly one must be emitted.
static int count_top_level_key(const std::string& frame, const char* key) {
    // rapidjson hides duplicates behind FindMember, so count them by hand.
    rapidjson::Document d;
    d.Parse(frame.c_str());
    if (d.HasParseError() || !d.IsObject()) return -1;
    int n = 0;
    for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it)
        if (std::strcmp(it->name.GetString(), key) == 0) ++n;
    return n;
}

static void test_handler_type_cannot_shadow_the_frame_type() {
    std::printf("a handler's own top-level \"type\" is renamed, not duplicated\n");
    const std::string f = build(
        "{\"success\":true,\"type\":\"event\",\"event_id\":\"x\"}");
    rapidjson::Document d = must_parse(f);
    if (d.HasParseError()) return;

    // Exactly one `type`, and it is the FRAME's.
    CHECK(count_top_level_key(f, "type") == 1);
    CHECK(std::strcmp(str_or(d, "type"), "result") == 0);
    // The handler's value is preserved, not dropped.
    CHECK(std::strcmp(str_or(d, "handler_type"), "event") == 0);
    CHECK(d.HasMember("event_id"));

    // An ordinary reply must gain no handler_type and still have exactly one type.
    const std::string plain = build("{\"success\":true}");
    CHECK(count_top_level_key(plain, "type") == 1);
    rapidjson::Document p = must_parse(plain);
    if (!p.HasParseError()) CHECK(!p.HasMember("handler_type"));
}

// The depth-0 scanner must not touch data that merely looks like a key.
static void test_rename_scanner_only_touches_top_level_keys() {
    std::printf("the rename scanner respects nesting, strings and escapes\n");

    // Nested "type" is DATA and must survive untouched.
    {
        const std::string f = build(
            "{\"success\":true,\"faction\":{\"type\":\"populist\"},"
            "\"list\":[{\"type\":\"a\"}]}");
        rapidjson::Document d = must_parse(f);
        if (!d.HasParseError()) {
            CHECK(!d.HasMember("handler_type"));
            CHECK(d.HasMember("faction") &&
                  std::strcmp(str_or(d["faction"], "type"), "populist") == 0);
            CHECK(d.HasMember("list") && d["list"].IsArray() && d["list"].Size() == 1 &&
                  d["list"][0].HasMember("type"));
            CHECK(std::strcmp(str_or(d, "type"), "result") == 0);
        }
    }
    // A string VALUE that spells "type" is not a key.
    {
        const std::string f = build("{\"note\":\"\\\"type\\\" is a word\"}");
        rapidjson::Document d = must_parse(f);
        if (!d.HasParseError()) {
            CHECK(!d.HasMember("handler_type"));
            CHECK(d.HasMember("note"));
            CHECK(std::strcmp(str_or(d, "type"), "result") == 0);
        }
    }
    // A top-level "type" that is NOT the first key still gets renamed, and one
    // that follows a nested object does too (the depth bookkeeping must unwind).
    {
        const std::string f = build(
            "{\"a\":{\"b\":[1,2]},\"type\":\"event\",\"z\":1}");
        rapidjson::Document d = must_parse(f);
        if (!d.HasParseError()) {
            CHECK(count_top_level_key(f, "type") == 1);
            CHECK(d.HasMember("handler_type"));
            CHECK(d.HasMember("z"));
        }
    }
    // "typed" / "subtype" must NOT match — the comparison is exact.
    {
        const std::string f = build("{\"typed\":1,\"subtype\":2}");
        rapidjson::Document d = must_parse(f);
        if (!d.HasParseError()) {
            CHECK(!d.HasMember("handler_type"));
            CHECK(d.HasMember("typed") && d.HasMember("subtype"));
        }
    }
    // The scanner is called on bodies directly too; a truncated key must not
    // run off the end.
    {
        std::string body = "\"success\":true,\"ty";
        rename_top_level_type_key(body);      // must simply return
        CHECK(body == "\"success\":true,\"ty");
    }
}

static void test_nested_objects_survive() {
    std::printf("nested handler objects are not truncated by the brace strip\n");
    const std::string f = build(
        "{\"success\":true,\"realm\":{\"counties\":[{\"id\":183}],\"n\":1}}");
    rapidjson::Document d = must_parse(f);
    if (d.HasParseError()) return;
    CHECK(d.HasMember("realm") && d["realm"].IsObject());
    CHECK(d["realm"].HasMember("counties") && d["realm"]["counties"].IsArray());
    CHECK(d["realm"]["counties"].Size() == 1);
    CHECK(std::strcmp(str_or(d, "type"), "result") == 0);
}

static void test_degenerate_handler_replies() {
    std::printf("degenerate handler replies still produce VALID json\n");

    // Empty object: nothing to splice, frame must still close and be typed.
    {
        rapidjson::Document d = must_parse(build("{}"));
        if (!d.HasParseError()) {
            CHECK(std::strcmp(str_or(d, "type"), "result") == 0);
            CHECK(!d.HasMember("success"));
        }
    }
    // Object with only whitespace inside.
    {
        rapidjson::Document d = must_parse(build("{   }"));
        if (!d.HasParseError())
            CHECK(d.HasMember("type"));
    }
    // Empty reply: the frame names the action itself so it is not anonymous.
    {
        rapidjson::Document d = must_parse(build(""));
        if (!d.HasParseError()) {
            CHECK(std::strcmp(str_or(d, "action"), "query_character") == 0);
            CHECK(d.HasMember("type"));
        }
    }
    // Not an object at all (a handler that answered with an array or a scalar).
    {
        rapidjson::Document d = must_parse(build("[1,2,3]"));
        if (!d.HasParseError()) {
            CHECK(d.HasMember("action"));
            CHECK(d.HasMember("type"));
        }
    }
    // TRUNCATED object — no closing brace. The pre-fix splice produced INVALID
    // JSON here (it copied the fragment and never closed the frame). This must
    // parse.
    {
        rapidjson::Document d = must_parse(build("{\"success\":true"));
        if (!d.HasParseError()) {
            CHECK(d.HasMember("success"));
            CHECK(d.HasMember("type"));
        }
    }
    // Trailing whitespace after the handler's closing brace.
    {
        rapidjson::Document d = must_parse(build("{\"success\":true}  "));
        if (!d.HasParseError()) {
            CHECK(d.HasMember("success"));
            CHECK(d.HasMember("type"));
        }
    }
}

// A frame for a client that sent no request_id: this is the population the
// bead is about. It must still be NAMEABLE even though it is not keyable.
static void test_frame_without_request_id_is_still_nameable() {
    std::printf("a frame with NO request_id still carries a type\n");
    std::string f = open_frame("");     // ticket-only, no request_id
    frame_append_result_body(f, "{\"success\":true,\"gold\":1}", "declare_war",
                             "result");
    rapidjson::Document d = must_parse(f);
    if (d.HasParseError()) return;
    CHECK(!d.HasMember("request_id"));
    // Before ck-6kvq.18 this frame had neither a key nor a type: unroutable.
    CHECK(std::strcmp(str_or(d, "type"), "result") == 0);
    CHECK(d.HasMember("ticket"));
}


// ══ ck-9vdk ═══════════════════════════════════════════════════════════════
// A ROW THAT CANNOT SAY WHAT WAS EXECUTED.
//
// `escaped_action` used to be emitted ONLY when the handler's reply was
// unusable, so a reply that was a perfectly good JSON object and simply did not
// name itself produced a frame with no `action` key. ck-9vgu measured it in a
// d39 episode: 2,302 of 3,009 ticketed rows carried ticket, request_id,
// character_id, flush_id, tick_executed and game_date_executed — everything
// except the verb. The driver's relay copies `action` off the frame,
// `split_bridge_rows` calls a nameless row unnameable, and `decision_capture`
// declines it outright.
static void test_a_reply_that_does_not_name_itself_is_named_by_the_frame() {
    std::printf("ck-9vdk: a reply that does not name itself is NAMED\n");

    // The shape the two highest-volume reads actually send: valid, useful, and
    // anonymous. query_character and query_realm write no `action` key at all.
    rapidjson::Document d = must_parse(build(
        "{\"success\":true,\"character_id\":14859,\"gold\":123.45}"));
    if (d.HasParseError()) return;
    CHECK(std::strcmp(str_or(d, "action"), "query_character") == 0);
    // EXACTLY ONE. rapidjson reads the first, python the last; a duplicate is
    // a disagreement and not a tie-break.
    CHECK(count_top_level_key(build(
        "{\"success\":true,\"gold\":1}"), "action") == 1);
    // And nothing it carried before is lost.
    CHECK(d.HasMember("success") && d.HasMember("gold"));
    CHECK(d.HasMember("bridge_record"));
}

// THE OTHER HALF, AND IT IS THE ONE THAT KEEPS THE FIX HONEST: a reply that
// DOES name itself must be byte-identical to before, with the handler's own
// value standing. Otherwise this bead moves a number every mediator reads.
static void test_a_reply_that_names_itself_is_untouched() {
    std::printf("ck-9vdk: a reply that names itself keeps ITS value, once\n");
    const std::string f = build(
        "{\"success\":true,\"action\":\"set_council_task\",\"task\":\"develop\"}",
        "set_council_task");
    rapidjson::Document d = must_parse(f);
    if (d.HasParseError()) return;
    CHECK(std::strcmp(str_or(d, "action"), "set_council_task") == 0);
    CHECK(count_top_level_key(f, "action") == 1);

    // A handler whose own name DISAGREES with the ticket's keeps its own, which
    // is exactly today's behaviour. Recorded rather than changed: which of the
    // two is authoritative is a question about evidence, not about this splice,
    // and answering it here would move a value silently.
    const std::string g = build(
        "{\"success\":true,\"action\":\"handler_said_this\"}", "ticket_said_that");
    rapidjson::Document e = must_parse(g);
    if (e.HasParseError()) return;
    CHECK(std::strcmp(str_or(e, "action"), "handler_said_this") == 0);
    CHECK(count_top_level_key(g, "action") == 1);
}

// A NESTED OR QUOTED `action` IS NOT A TOP-LEVEL ONE. A substring test would
// have called each of these "already named" and left the row anonymous — the
// direction that reads as fixed.
static void test_only_a_TOP_LEVEL_action_counts_as_named() {
    std::printf("ck-9vdk: nested and quoted `action` do not count\n");
    struct Case { const char* body; const char* why; } cases[] = {
        {"{\"detail\":{\"action\":\"nested\"}}",        "nested object"},
        {"{\"rows\":[{\"action\":\"in_array\"}]}",      "inside an array"},
        {"{\"reason\":\"no action available\"}",        "in a string value"},
        {"{\"actions\":[1,2]}",                         "a LONGER key"},
    };
    for (const auto& c : cases) {
        const std::string f = build(c.body, "query_realm");
        rapidjson::Document d = must_parse(f);
        if (d.HasParseError()) continue;
        ++g_checks;
        if (std::strcmp(str_or(d, "action"), "query_realm") != 0) {
            ++g_failures;
            std::printf("  FAIL: %s — frame not named (%s)\n", c.why, f.c_str());
        }
        CHECK(count_top_level_key(f, "action") == 1);
    }
}

// TWO READERS OF ONE TRAVERSAL, AND HERE IS THE MIRROR CHECK.
// `has_top_level_key` is a second walk beside `rename_top_level_type_key`,
// which is deliberate: that mutator rewrites EVERY pipeline reply and a
// behaviour change in it has the widest blast radius in this layer. Keeping
// them separate is only safe if something asserts they AGREE, so: for a corpus
// of bodies, `has_top_level_key(body, "\"type\"")` must be true exactly when
// the rename actually rewrote the body.
static void test_the_two_traversals_agree_about_type() {
    std::printf("ck-9vdk: has_top_level_key agrees with the rename scanner\n");
    const char* bodies[] = {
        "\"type\":\"handler\"",
        "\"success\":true,\"type\":\"handler\"",
        "\"success\":true",
        "\"d\":{\"type\":\"nested\"}",
        "\"rows\":[{\"type\":\"in_array\"}]",
        "\"reason\":\"a type of thing\"",
        "\"types\":[1]",
        "\"reason\":\"he said \\\"type\\\": no\"",
        "\"type",
        "",
        "   ",
        "\"a\":1,\"type\":2,\"b\":3",
    };
    for (const char* b : bodies) {
        std::string mutated = b;
        rename_top_level_type_key(mutated);
        const bool renamed = (mutated != b);
        const bool seen = has_top_level_key(b, "\"type\"");
        ++g_checks;
        if (renamed != seen) {
            ++g_failures;
            std::printf("  FAIL: the two traversals DISAGREE on %s "
                        "(renamed=%d seen=%d)\n", b, renamed ? 1 : 0,
                        seen ? 1 : 0);
        }
    }
}

int main() {
    std::printf("test_pipeline_frames — ck-6kvq.18 typed pipeline egress\n");
#if defined(CK3_PIPELINE_FRAME_UNTYPED)
    std::printf("*** NEGATIVE CONTROL BUILD (untyped frames). "
                "THIS MUST FAIL. ***\n");
#endif
    std::printf("\n");

    test_ordinary_handler_reply();
    test_handler_type_cannot_shadow_the_frame_type();
    test_rename_scanner_only_touches_top_level_keys();
    test_nested_objects_survive();
    test_degenerate_handler_replies();
    test_frame_without_request_id_is_still_nameable();
    test_a_reply_that_does_not_name_itself_is_named_by_the_frame();
    test_a_reply_that_names_itself_is_untouched();
    test_only_a_TOP_LEVEL_action_counts_as_named();
    test_the_two_traversals_agree_about_type();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
