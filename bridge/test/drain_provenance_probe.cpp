// ck-dup0 — can the drain line tell a ticketed hand-off from a raw push?
//
// The fixture drives BOTH paths, because the defect was that they were
// indistinguishable: a probe that only exercised one would have been green
// against the ambiguity it exists to end.
#include "drain_provenance.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace bridge { FILE* g_logfile = nullptr; }

// Replace every digit with '#', so a comparison of the results can only turn
// on wording. Ticket numbers and session serials are per-envelope; the PATH is
// the claim under test.
static void blind_digits(char* out, size_t n, const char* in) {
    size_t i = 0;
    for (; in[i] && i + 1 < n; ++i)
        out[i] = (in[i] >= '0' && in[i] <= '9') ? '#' : in[i];
    out[i] = '\0';
}

static int failures = 0;
static void row(const char* n, bool ok, const char* d) {
    printf("%-50s %s  %s\n", n, ok ? "PASS" : "FAIL", d);
    if (!ok) failures++;
}

int main() {
    char b[256];

    // THE FLUSH HAND-OFF: buffered and ticketed at submit, routed here by
    // action_routes_to_drain at flush time.
    bridge::format_drain_origin(b, sizeof b, 40, "sess_0008");
    row("a_ticketed_handoff_names_its_ticket",
        std::strstr(b, "ticket=tk_") && std::strstr(b, "sess_0008"), b);

    // THE READER RAW PUSH: the client's own envelope, no conductor ticket.
    bridge::format_drain_origin(b, sizeof b, 0, "sess_0008");
    row("a_raw_push_says_ticket_0_AND_WHY",
        std::strstr(b, "ticket=0") &&
        std::strstr(b, "reader raw push, no conductor ticket"), b);

    // THE TWO ARE DISTINGUISHABLE — the whole point. Same action, same rid,
    // same sizes; only this field differs.
    //
    // ck-qirf: asserting merely that the two strings DIFFER does not carry this
    // bead. Restore the defect by rendering every envelope as a ticketed
    // hand-off (`if (pipeline_ticket)` -> `if (true)`) and the raw push comes
    // out "ticket=tk_00000000" against the hand-off's "ticket=tk_00000040":
    // still two different strings, so a strcmp row stays GREEN while the
    // property it is named for is gone. They differed by the ticket NUMBER,
    // which is per-envelope, and not by the PATH, which is what is claimed.
    //
    // So blind the comparison to every digit first. What survives is the
    // path wording, and only a line that labels its path can differ once the
    // numbers are gone.
    char t[256], r[256];
    bridge::format_drain_origin(t, sizeof t, 40, "sess_0008");
    bridge::format_drain_origin(r, sizeof r, 0,  "sess_0008");
    char tb[256], rb[256];
    blind_digits(tb, sizeof tb, t);
    blind_digits(rb, sizeof rb, r);
    row("the_two_paths_differ_BY_PATH_not_by_ticket_number",
        std::strcmp(tb, rb) != 0,
        tb);

    // An empty session says so rather than leaving a blank that reads as
    // attribution nobody checked.
    bridge::format_drain_origin(b, sizeof b, 40, "");
    row("an_empty_session_says_UNATTRIBUTED",
        std::strstr(b, "UNATTRIBUTED (no session_id on the queued envelope)"), b);
    bridge::format_drain_origin(b, sizeof b, 0, nullptr);
    row("a_null_session_says_UNATTRIBUTED_too",
        std::strstr(b, "UNATTRIBUTED"), b);

    // UNCONDITIONAL COUNTS — the half that survives an untraced run.
    bridge::format_drain_provenance(b, sizeof b);
    row("a_clean_run_states_both_counts_as_zero",
        std::strcmp(b, "drain_from_flush=0 drain_from_raw_push=0") == 0, b);

    bridge::note_drain_provenance(40);
    bridge::note_drain_provenance(0);
    bridge::note_drain_provenance(0);
    bridge::format_drain_provenance(b, sizeof b);
    row("the_counts_separate_the_two_paths",
        std::strcmp(b, "drain_from_flush=1 drain_from_raw_push=2") == 0, b);

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
