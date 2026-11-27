// test/test_council_holder.cpp — one holder read for a council seat.
//
// THE HAZARD: query_council (fixed 20260619) takes the display field +0x30 when populated, else the seated
// councillor +0x38; appoint_councillor read +0x30 alone. On a realm whose +0x30 is blank on every seat
// (Murchad's: holder_source "seated" on each) appoint read a FILLED seat as vacant and refused
// vacant_seat_fill_unsupported, and its readback after a swap could not see the new holder.
// Two halves:
//   decision — council_holder::resolve on the cases;
//   wiring   — both handlers call it, and appoint reads no holder field on its own (comment-stripped sources).
// Usage: test_council_holder <bridge-source-dir>

#include "council_holder.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_fails = 0, g_checks = 0;
static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static std::string strip_comments(const std::string& src) {
    std::string out;
    for (size_t i = 0; i < src.size();) {
        if (src.compare(i, 2, "//") == 0) { while (i < src.size() && src[i] != '\n') i++; continue; }
        if (src.compare(i, 2, "/*") == 0) {
            size_t e = src.find("*/", i + 2);
            i = (e == std::string::npos) ? src.size() : e + 2;
            continue;
        }
        if (src[i] == '"') {
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

static size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
    return n;
}

static std::string body_of(const std::string& src, const std::string& head) {
    size_t a = src.find(head);
    if (a == std::string::npos) return "";
    size_t b = src.find("\nbool do_", a + head.size());
    return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

int main(int argc, char** argv) {
    using namespace council_holder;
    printf("test_council_holder\n\ndecision\n");
    Source s = Source::None;
    check(resolve(36303u, 36303u, &s) == 36303u && s == Source::Display, "both fields: the display field answers");
    check(resolve(0xFFFFFFu, 36303u, &s) == 36303u && s == Source::Seated,
          "display blank (0xFFFFFF), seated set: the seated councillor (Murchad's realm; appoint read vacant)");
    check(resolve(0u, 36303u, &s) == 36303u && s == Source::Seated, "display 0: the seated councillor");
    check(resolve(0xFF000000u | 0xFFFFFFu, 0x01000000u | 36303u) == 36303u, "the high byte is masked off");
    check(resolve(0xFFFFFFu, 0xFFFFFFu, &s) == 0xFFFFFFu && s == Source::None, "neither: vacant (0xFFFFFF)");
    check(resolve(0u, 0u) == 0xFFFFFFu && !is_id(resolve(0u, 0u)), "zeros: vacant, never id 0");
    check(std::string(source_name(Source::Seated)) == "seated" && std::string(source_name(Source::Display)) == "display",
          "query_council's holder_source words are unchanged");

    printf("\nwiring\n");
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string q = strip_comments(read_file(dir + "/src/query_handlers.cpp"));
    const std::string a = strip_comments(read_file(dir + "/src/action_handlers.cpp"));
    check(!q.empty() && !a.empty(), "the two product sources were read");
    const std::string ap = body_of(a, "bool do_appoint_councillor(");
    check(!ap.empty(), "do_appoint_councillor found");
    check(count(q, "council_holder::resolve(") >= 1, "query_council resolves the holder with council_holder::resolve");
    check(count(ap, "council_holder::resolve(") == 1, "appoint_councillor resolves it with the same function");
    check(ap.find("COUNCIL_POS_COUNCILLOR_ID") != std::string::npos,
          "appoint reads the seated councillor field (+0x38)");
    check(ap.find("POS_HOLDER = 0x30") == std::string::npos && ap.find("+ POS_HOLDER)") == std::string::npos,
          "appoint no longer reads +0x30 alone");
    check(ap.find("seat_holder()") != std::string::npos && count(ap, "seat_holder()") >= 2,
          "the readback after the swap uses the same read");
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
