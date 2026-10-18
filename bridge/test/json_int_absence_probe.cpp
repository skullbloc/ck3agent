// ck-vf92 — what does json_get_int ACTUALLY return for an absent key?
//
// This probe exists because the first diagnosis of ck-bl15's root cause said
// json_get_int returns 0 for an absent field, making "omitted" and "zero"
// indistinguishable at the guard. That is a reasonable reading and it is FALSE
// for this implementation: every failure path returns -1. Absence was already
// refused by `if (cid < 0)`; only an explicit 0 slipped through.
//
// The distinction decides the fix. If absence read as 0, the fix would need a
// json_has_key presence guard. Because absence reads as -1, such a guard could
// never fire for a case `< 0` did not already catch — a check that cannot fail
// for the reason it names. So the fix is `<= 0`, and json_has_key is used only
// to make the refusal REASON accurate.
//
// Links the REAL json_helpers.cpp, so a change to the helper reaches this test.
// Exit 0 pass / 1 FAIL.
#include <cstdio>
namespace bridge {
int  json_get_int(const char* json, const char* key);
bool json_has_key(const char* json, const char* key);
}
int main() {
    struct { const char* name; const char* json; bool absent_case;
             bool must_be_refused_by_fix; } cases[] = {
        {"absent entirely",        "{\"enabled\":true}",                              true,  true},
        {"absent, params present", "{\"params\":{\"enabled\":true}}",                 true,  true},
        // armb1's four faults, verbatim from the sealed bundle:
        {"explicit 0 in params",   "{\"params\":{\"character_id\":0,\"enabled\":true}}", false, true},
        {"explicit 0 top-level",   "{\"character_id\":0}",                            false, true},
        {"string \"0\"",           "{\"character_id\":\"0\"}",                          false, true},
        {"explicit -1",            "{\"character_id\":-1}",                           false, true},
        {"null value",             "{\"character_id\":null}",                         false, true},
        {"malformed json",         "{\"character_id\":",                              false, true},
        // The one row that must survive: a real character must still be accepted.
        {"legit id",               "{\"params\":{\"character_id\":47607}}",           false, false},
    };
    int failures = 0;
    printf("%-24s %8s %9s  %-10s %s\n", "case", "get_int", "has_key", "old `<0`", "new `<=0`");
    for (auto& c : cases) {
        int v = bridge::json_get_int(c.json, "character_id");
        bool h = bridge::json_has_key(c.json, "character_id");
        printf("%-24s %8d %9s  %-10s %s\n", c.name, v, h ? "true" : "false",
               v < 0 ? "refused" : "ACCEPTED", v <= 0 ? "refused" : "accepted");
        // THE LOAD-BEARING CLAIM: absence must read as a NEGATIVE, not as 0.
        // If this ever flips, the fix below is wrong and a presence guard is
        // required — so assert it rather than leaving it as a printed table.
        if (c.absent_case && !(v < 0)) {
            printf("  FAIL: '%s' did not read as negative (got %d) -- a presence\n"
                   "        guard WOULD now be required; re-open ck-vf92\n", c.name, v);
            failures++;
        }
        if (c.must_be_refused_by_fix && !(v <= 0)) {
            printf("  FAIL: '%s' survives the `<= 0` guard (got %d)\n", c.name, v);
            failures++;
        }
    }
    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
