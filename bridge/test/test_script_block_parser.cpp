// Unit tests for script_block_parser (ck-zvu5l.3).
//
// Build: enabled by BUILD_BRIDGE_TESTS=ON. CMake target test_script_block_parser.
// Run: ./test_script_block_parser  (exits non-zero on failure)

#include "script_block_parser.h"

#include <cstdio>
#include <cstring>
#include <string>

using bridge::parse_script_block;
using bridge::strip_ref_prefix;
using bridge::parse_int_strict;
using bridge::ScriptBlock;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d): %s\n", #cond, __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

static void test_start_war_bead_example() {
    const char* text =
        "start_war = { casus_belli=claim_cb target_title=title:d_guelders "
        "attacker_war_leader=root }";
    ScriptBlock pe = parse_script_block(text);
    CHECK(pe.valid, "bead example should parse cleanly");
    CHECK(pe.effect_name == "start_war", "outer effect mismatch");
    CHECK(pe.params.size() == 3, "should capture 3 top-level params");
    CHECK(pe.params["casus_belli"] == "claim_cb", "casus_belli wrong");
    CHECK(pe.params["target_title"] == "title:d_guelders",
          "title-ref preserves prefix");
    CHECK(pe.params["attacker_war_leader"] == "root", "attacker ref wrong");
}

static void test_whitespace_variations() {
    const char* text =
        "  start_war\n=\n{\n  casus_belli = claim_cb\n  "
        "target = character:42\n}\n";
    ScriptBlock pe = parse_script_block(text);
    CHECK(pe.valid, "newline-heavy variant should parse");
    CHECK(pe.params["casus_belli"] == "claim_cb", "key=value across lines wrong");
    CHECK(pe.params["target"] == "character:42", "character ref wrong");
}

static void test_quoted_string_value() {
    const char* text = R"(give_title = { title = "k_england" })";
    ScriptBlock pe = parse_script_block(text);
    CHECK(pe.valid, "quoted value should parse");
    CHECK(pe.params["title"] == "k_england", "quoted value content wrong");
}

static void test_nested_block_captured_raw() {
    const char* text = "wrapper = { inner = { a = 1 b = 2 } x = y }";
    ScriptBlock pe = parse_script_block(text);
    CHECK(pe.valid, "nested block should parse");
    CHECK(pe.params["x"] == "y", "trailing key after nested block lost");
    const std::string& inner = pe.params["inner"];
    CHECK(inner.find("a = 1") != std::string::npos &&
          inner.find("b = 2") != std::string::npos,
          "nested raw body should preserve inner content");
}

static void test_comment_skipped() {
    const char* text =
        "start_war = { # this is a comment\n"
        "  casus_belli = claim_cb # trailing comment\n"
        "}";
    ScriptBlock pe = parse_script_block(text);
    CHECK(pe.valid, "comments should be skipped");
    CHECK(pe.params.size() == 1, "should not capture comments as keys");
    CHECK(pe.params["casus_belli"] == "claim_cb", "post-comment value lost");
}

static void test_missing_equals_after_effect_name() {
    ScriptBlock pe = parse_script_block("start_war { casus_belli=claim_cb }");
    CHECK(!pe.valid, "missing `=` should fail");
    CHECK(pe.error == "expected_equals_after_effect_name", "wrong error code");
}

static void test_missing_open_brace() {
    ScriptBlock pe = parse_script_block("start_war = claim_cb");
    CHECK(!pe.valid, "missing `{` should fail");
    CHECK(pe.error == "expected_open_brace", "wrong error code");
}

static void test_unterminated_block() {
    ScriptBlock pe = parse_script_block("start_war = { casus_belli=claim_cb");
    CHECK(!pe.valid, "unterminated block should fail");
    CHECK(pe.error == "unexpected_eof_in_block", "wrong error code");
}

static void test_trailing_garbage() {
    ScriptBlock pe = parse_script_block("a = { b = c } stray");
    CHECK(!pe.valid, "trailing content should fail");
    CHECK(pe.error == "trailing_content_after_block", "wrong error code");
}

static void test_empty_input() {
    ScriptBlock pe = parse_script_block("");
    CHECK(!pe.valid, "empty input should fail");
    pe = parse_script_block(nullptr);
    CHECK(!pe.valid, "null input should fail");
    CHECK(pe.error == "null_text", "wrong null error");
}

static void test_empty_block_ok() {
    ScriptBlock pe = parse_script_block("noop_effect = { }");
    CHECK(pe.valid, "empty body should parse");
    CHECK(pe.effect_name == "noop_effect", "name wrong");
    CHECK(pe.params.empty(), "should have no params");
}

// #80: parameterless `<effect> = yes` form parses with empty params (routes
// via execute_scripted_effect); param-bearing block keeps non-empty params.
static void test_parameterless_yes_form() {
    ScriptBlock pe = parse_script_block("noop = yes");
    CHECK(pe.valid, "`noop = yes` should parse");
    CHECK(pe.effect_name == "noop", "parameterless effect name wrong");
    CHECK(pe.params.empty(), "parameterless block must have empty params");

    ScriptBlock pe2 = parse_script_block("add_gold = { gold=100 }");
    CHECK(pe2.valid, "`add_gold = { gold=100 }` should parse");
    CHECK(!pe2.params.empty(), "param-bearing block must have non-empty params");
    CHECK(pe2.params["gold"] == "100", "param value wrong");

    // A non-yes bare value is still malformed (guards against accidental drift).
    ScriptBlock pe3 = parse_script_block("start_war = claim_cb");
    CHECK(!pe3.valid, "`= <ident other than yes>` must still fail");
    CHECK(pe3.error == "expected_open_brace", "wrong error for bare non-yes value");
}

static void test_strip_ref_prefix() {
    CHECK(strip_ref_prefix("title:d_guelders", "title:") == "d_guelders",
          "title prefix not stripped");
    CHECK(strip_ref_prefix("character:42", "character:") == "42",
          "character prefix not stripped");
    CHECK(strip_ref_prefix("root", "character:") == "root",
          "no-prefix string should be unchanged");
    CHECK(strip_ref_prefix("title:", "title:") == "title:",
          "bare prefix (no suffix) should not strip");
}

static void test_parse_int_strict() {
    int v = 0;
    CHECK(parse_int_strict("42", &v) && v == 42, "positive int parse");
    CHECK(parse_int_strict("-7", &v) && v == -7, "negative int parse");
    CHECK(!parse_int_strict("12abc", &v), "trailing chars must fail");
    CHECK(!parse_int_strict("", &v), "empty string must fail");
    CHECK(!parse_int_strict("d_guelders", &v), "ident must not parse as int");
}

// Smoke: large number of params, ensure unordered_map handles it.
static void test_many_params() {
    std::string text = "many = { ";
    for (int i = 0; i < 50; i++) {
        text += "k" + std::to_string(i) + "=v" + std::to_string(i) + " ";
    }
    text += "}";
    ScriptBlock pe = parse_script_block(text.c_str());
    CHECK(pe.valid, "many-param block should parse");
    CHECK(pe.params.size() == 50, "should capture all 50 params");
    CHECK(pe.params["k25"] == "v25", "spot check param 25");
}

int main() {
    test_start_war_bead_example();
    test_whitespace_variations();
    test_quoted_string_value();
    test_nested_block_captured_raw();
    test_comment_skipped();
    test_missing_equals_after_effect_name();
    test_missing_open_brace();
    test_unterminated_block();
    test_trailing_garbage();
    test_empty_input();
    test_empty_block_ok();
    test_parameterless_yes_form();
    test_strip_ref_prefix();
    test_parse_int_strict();
    test_many_params();

    if (failures == 0) {
        std::printf("test_script_block_parser: OK (all checks passed)\n");
        return 0;
    }
    std::fprintf(stderr, "test_script_block_parser: %d failure(s)\n", failures);
    return 1;
}
