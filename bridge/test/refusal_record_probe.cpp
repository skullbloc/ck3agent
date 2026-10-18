// ck-475j — does a refusal show up without anyone grepping for it?
#include "refusal_record.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace bridge { FILE* g_logfile = nullptr; }

static int failures = 0;
static void row(const char* n, bool ok, const char* d) {
    printf("%-44s %s  %s\n", n, ok ? "PASS" : "FAIL", d);
    if (!ok) failures++;
}

int main() {
    char buf[256];

    // A clean run must SAY "none", not omit the field. An absent field is
    // indistinguishable from an older binary that never had one.
    bridge::format_refusals(buf, sizeof buf);
    row("clean_run_says_none_explicitly", std::strcmp(buf, "refusals=none") == 0, buf);

    // The detector: one refusal, and it is visible without a string search.
    bridge::record_refusal(bridge::RefusalReason::kCharacterIdInvalid);
    bridge::format_refusals(buf, sizeof buf);
    row("a_single_refusal_becomes_visible",
        std::strstr(buf, "character_id_invalid=1") != nullptr, buf);

    // Only NON-ZERO reasons render, so the line stays short and any entry on it
    // means something happened.
    row("zero_reasons_are_not_rendered",
        std::strstr(buf, "ai_controller_unbound") == nullptr,
        "untriggered reasons stay off the line");

    for (int i = 0; i < 46; ++i)
        bridge::record_refusal(bridge::RefusalReason::kCharacterIdInvalid);
    bridge::record_refusal(bridge::RefusalReason::kAiControllerUnmapped);
    bridge::format_refusals(buf, sizeof buf);
    row("counts_accumulate_and_separate",
        std::strstr(buf, "character_id_invalid=47") != nullptr &&
        std::strstr(buf, "ai_controller_unmapped=1") != nullptr, buf);

    // The whole point: 47 silently-absorbed caller bugs are a NUMBER on the
    // heartbeat, not a string someone has to know to look for.
    row("a_silently_absorbed_bug_is_a_climbing_number",
        bridge::refusal_count(bridge::RefusalReason::kCharacterIdInvalid) == 47,
        "readable without grepping the log");

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
