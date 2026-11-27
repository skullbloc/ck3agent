// test/test_claim_rule.cpp — when a claim may carry a claim war.
//
// THE EVIDENCE (a live run of the
// peace checks, row 4): script_action declare_war claim_cb by 35156 on his OWN
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
#include <map>
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
    printf("test_claim_rule\n\ndecision\n");
    using claim_rule::claim_usable;
    check(claim_usable(true, false, true), "the actor's own UNPRESSED claim carries a claim war (as read live)");
    check(claim_usable(true, true, true), "the actor's own pressed claim carries one");
    check(claim_usable(true, true, false), "another claimant's PRESSED claim carries one");
    check(!claim_usable(true, false, false), "another claimant's unpressed claim does not");
    check(!claim_usable(false, true, true) && !claim_usable(false, false, true), "no claim, no claim war");

    printf("\nThe realm a claimed title lies in (a recorded run, 1075.09.15)\n");
    {
        // Conchobar 29585 holds c_athlone under 68530 (independent); Donnchad 37528 is
        // independent. 900 is a vassal of 29585; 37528's own vassal 500; a loop 1<->2.
        std::map<uint32_t, uint32_t> liege = {{29585, 68530}, {68530, 0}, {900, 29585},
                                              {37528, 0}, {500, 37528}, {1, 2}, {2, 1},
                                              {700, 9999}};
        auto liege_of = [&](uint32_t h) { auto it = liege.find(h); return it == liege.end() ? 0u : it->second; };
        auto lookup = [&](uint32_t id) { return liege.count(id) ? id : 0u; };
        using claim_rule::title_in_defender_realm;
        using claim_rule::claim_war_defender;
        check(title_in_defender_realm(29585u, 29585u, 68530u, liege_of, lookup),
              "a title held by the defender's VASSAL is in his realm (was claim_not_pressed)");
        check(title_in_defender_realm(900u, 900u, 68530u, liege_of, lookup),
              "a sub-vassal's title is in the top liege's realm");
        check(title_in_defender_realm(68530u, 68530u, 68530u, liege_of, lookup),
              "the defender's own title is in his realm (the old rule)");
        check(!title_in_defender_realm(68530u, 68530u, 29585u, liege_of, lookup),
              "the liege's title is not in his vassal's realm");
        check(!title_in_defender_realm(1u, 1u, 3u, liege_of, lookup) &&
              !title_in_defender_realm(700u, 700u, 68530u, liege_of, lookup),
              "a liege loop or an unresolvable liege ends without a match");
        std::set<uint32_t> none;
        check(claim_war_defender(29585u, 29585u, 37528u, none, liege_of, lookup) == 68530u,
              "an independent claimant declares on the vassal holder's top liege");
        check(claim_war_defender(68530u, 68530u, 37528u, none, liege_of, lookup) == 68530u,
              "an independent holder is the defender himself");
        check(claim_war_defender(900u, 900u, 37528u, none, liege_of, lookup) == 68530u,
              "a sub-vassal holder: the top of the chain");
        check(claim_war_defender(500u, 500u, 37528u, none, liege_of, lookup) == 0u &&
              claim_war_defender(37528u, 37528u, 37528u, none, liege_of, lookup) == 0u,
              "the claimant's own vassal or himself: no war (0)");
        std::set<uint32_t> under68530 = {68530};
        check(claim_war_defender(900u, 900u, 42u, under68530, liege_of, lookup) == 29585u,
              "a fellow vassal of 68530: the realms meet below 68530, the defender is 29585");
        check(claim_war_defender(1u, 1u, 37528u, none, liege_of, lookup) == 0u &&
              claim_war_defender(700u, 700u, 37528u, none, liege_of, lookup) == 0u,
              "a loop or an unresolvable liege: 0, never a guess");
    }

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

    check(count(q, "claim_holder_in_realm(holder, defender_id)") == 2,
          "both own-claim walkers match a title anywhere in the defender's realm ");
    check(q.find("if (holder == defender_id)") == std::string::npos,
          "no own-claim walker matches the holder alone ");
    check(count(q, "claim_war_defender_for(c, holder_char_id)") == 1 &&
          count(q, "claim_war_defender_for(ac, holder_id)") == 1,
          "query_claims and the valid war targets name the realm's defender ");
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
