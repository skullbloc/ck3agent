// test/test_claim_rule.cpp — lane integ11: when a claim may carry a claim war.
//
// THE EVIDENCE (docs/cleanup-2026.11/evidence/v4integ11, live run i11s3, the
// peace lane's row 4): script_action declare_war claim_cb by 35156 on his OWN
// UNPRESSED claim (c_berestye 3401; query_claims: pressed false, block_reason
// claim_not_pressed): the game's can_declare_war RAN and the war started.
// Two halves:
//   decision — claim_rule::claim_usable on the cases;
//   wiring   — every read that culls a claim row and the native write path call
//              it, and no own-claim walker skips an unpressed claim any more
//              (comment-stripped sources).
// Usage: test_claim_rule <bridge-source-dir>

#include "claim_rule.h"

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

int main(int argc, char** argv) {
    printf("test_claim_rule (integ11)\n\ndecision\n");
    using claim_rule::claim_usable;
    check(claim_usable(true, false, true), "the actor's own UNPRESSED claim carries a claim war (live i11s3)");
    check(claim_usable(true, true, true), "the actor's own pressed claim carries one");
    check(claim_usable(true, true, false), "another claimant's PRESSED claim carries one");
    check(!claim_usable(true, false, false), "another claimant's unpressed claim does not");
    check(!claim_usable(false, true, true) && !claim_usable(false, false, true), "no claim, no claim war");

    printf("\nwiring\n");
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string q = strip_comments(read_file(dir + "/src/query_handlers.cpp"));
    const std::string w = strip_comments(read_file(dir + "/src/intent_dispatch.cpp"));
    check(!q.empty() && !w.empty(), "the two product sources were read");
    check(count(q, "claim_rule::claim_usable(") >= 5,
          "query_handlers calls claim_usable at the two own-claim walkers, query_claims, "
          "the valid war targets and the title claimants");
    check(count(q, "PRESSED) == 0) continue;") == 0,
          "no own-claim walker skips an unpressed claim");
    check(q.find("(pressed == 0) ? \"claim_not_pressed\"") == std::string::npos &&
          q.find("(!pressed) ? \"claim_not_pressed\"") == std::string::npos,
          "no read names claim_not_pressed from the pressed flag alone");
    check(w.find("claim_rule::claim_usable(has_claim, pressed_claim, eff_claimant == atk_id)") != std::string::npos &&
          w.find("if (!pressed_claim) return reject(\"claim_not_pressed\");") == std::string::npos,
          "the native declare_war path refuses claim_not_pressed only by claim_usable");

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
