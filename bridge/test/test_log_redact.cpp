// test_log_redact.cpp — the bridge log's redaction of raw frames (log_redact.h),
// driven with every adversarial shape the TK5 round-3 witness listed
// (docs/cleanup-2026.11/evidence/v3pilot/witness-tk5-r3.md, finding 1).
//
// Each row names a SECRET that must not survive and, where it applies, text that
// MUST survive (so a redactor that blanks whole lines does not pass). The rows
// named r3miss_* were missed by the round-3 redactor (string values under
// plain-byte keys only): 12 rows, all red on 5e42c229 (measured on the node).
// The rest were already redacted there and are kept as guards (the partly
// escaped key still spelled "esume_token" in raw bytes); the CONTROL rows
// (nothing secret) must come back byte-identical.
#include "log_redact.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void row(const char* name, const std::string& in, const char* secret,
                const char* keep) {
    const std::string out = bridge::redact_for_log(in);
    const bool gone = !secret || out.find(secret) == std::string::npos;
    const bool kept = !keep || out.find(keep) != std::string::npos;
    const bool ok = gone && kept;
    std::printf("%-46s %s  %s\n", name, ok ? "PASS" : "FAIL", out.substr(0, 150).c_str());
    if (!ok) ++failures;
}

static void control(const char* name, const std::string& in) {
    const std::string out = bridge::redact_for_log(in);
    const bool ok = out == in;
    std::printf("%-46s %s  %s\n", name, ok ? "PASS" : "FAIL", out.substr(0, 150).c_str());
    if (!ok) ++failures;
}

int main() {
    const char* S = "S3CR3T";
    // ── guards (the r3 redactor already covered these) ─────────────────────────
    row("plain_resume_token",
        "{\"type\":\"handshake\",\"client_name\":\"c\",\"resume_token\":\"S3CR3T\"}", S, "\"client_name\":\"c\"");
    row("camel_case_key", "{\"resumeToken\":\"S3CR3T\",\"a\":1}", S, "\"a\":1");
    row("upper_case_key", "{\"RESUME_TOKEN\":\"S3CR3T\"}", S, "RESUME_TOKEN");
    row("whitespace_around_colon", "{\"resume_token\" \t:  \"S3CR3T\" , \"a\":2}", S, "\"a\":2");
    row("nested_object_key", "{\"params\":{\"check_token\":\"S3CR3T\",\"id\":7}}", S, "\"id\":7");
    row("escaped_quote_in_value", "{\"resume_token\":\"S3\\\"CR3T\\\"x\",\"a\":3}", "CR3T", "\"a\":3");
    row("trailing_backslash_in_value", "{\"resume_token\":\"S3CR3T\\\\\",\"a\":4}", S, "\"a\":4");
    row("duplicate_keys", "{\"resume_token\":\"x1\",\"resume_token\":\"S3CR3T\"}", S, "<redacted>");
    row("truncated_frame", "{\"type\":\"handshake\",\"resume_token\":\"S3CR", "S3CR", "handshake");
    row("embedded_nul", std::string("{\"resume_token\":\"S3") + '\0' + "CR3T\"}", "CR3T", nullptr);
    // ── [r3-miss] value types ──────────────────────────────────────────────────
    row("r3miss_array_value", "{\"api_token\":[\"S3CR3T\",\"two\"],\"a\":5}", S, "\"a\":5");
    row("r3miss_object_value", "{\"secret\":{\"v\":\"S3CR3T\"},\"a\":6}", S, "\"a\":6");
    row("r3miss_number_value", "{\"session_token\":918273645,\"a\":7}", "918273645", "\"a\":7");
    row("r3miss_deep_object_value", "{\"token\":{\"x\":[{\"y\":\"S3CR3T\"}]},\"a\":8}", S, "\"a\":8");
    // ── [r3-miss] key spellings ────────────────────────────────────────────────
    row("partly_escaped_key_guard", "{\"\\u0072esume_token\":\"S3CR3T\",\"a\":9}", S, "\"a\":9");
    row("r3miss_fully_escaped_key",
        "{\"\\u0074\\u006F\\u006B\\u0065\\u006E\":\"S3CR3T\"}", S, nullptr);
    row("r3miss_authorization", "{\"Authorization\":\"Bearer S3CR3T\",\"a\":10}", S, "\"a\":10");
    row("r3miss_api_key", "{\"api_key\":\"S3CR3T\",\"a\":11}", S, "\"a\":11");
    row("r3miss_apiKey", "{\"apiKey\":\"S3CR3T\"}", S, nullptr);
    row("password_guard", "{\"password\":\"S3CR3T\"}", S, nullptr);
    // ── [r3-miss] JSON inside a string ─────────────────────────────────────────
    row("r3miss_json_in_string",
        "{\"params\":\"{\\\"resume_token\\\":\\\"S3CR3T\\\"}\",\"a\":12}", S, "\"a\":12");
    // ── [r3-miss] frames RapidJSON refuses ─────────────────────────────────────
    row("r3miss_missing_colon", "{\"resume_token\" \"S3CR3T\"}", S, nullptr);
    row("comment_in_frame_guard", "{/* c */ \"resume_token\":\"S3CR3T\"}", S, nullptr);
    row("r3miss_unparsable_escaped_key", "{\"\\u0074oken\" \"S3CR3T\"", S, nullptr);
    row("r3miss_unquoted_key", "{resume_token: \"S3CR3T\"}", S, nullptr);
    // ── controls: nothing secret, byte for byte ────────────────────────────────
    control("control_plain_handshake",
            "{\"type\":\"handshake\",\"protocol_version\":2,\"client_name\":\"ck3agent\",\"mode\":\"control\"}");
    control("control_word_token_as_a_value", "{\"note\":\"token\",\"x\":[1,2,{\"y\":null}]}");
    control("control_spacing_kept", "{ \"a\" : 1 ,\n \"b\" : \"two\" }");
    control("control_string_that_is_json_without_secret", "{\"p\":\"{\\\"id\\\":3}\"}");
    control("control_unparsable_without_secret", "{\"a\":1,,}");
    std::printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
