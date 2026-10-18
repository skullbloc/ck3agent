// claim_event_stamp_probe.cpp — DOES AN UNREADABLE DATE SAY SO? (ck-jij9)
//
// The claim-set events were emitted three ways and none carried the game tick:
// release_character and set_controlled_character were raw fprintf with no stamp at
// all, CONTROLLED_HANDOFF went through blog() with a monotonic wall-clock stamp and
// no tick. The only tick-bearing line in the log is "BUILD-B: DIAG tick=" every 50
// ticks — so a vacancy was locatable only to a 50-tick interval, and ep-02's
// vacancies ARE 50 ticks. The measurement floor equalled the phenomenon.
//
// WHAT THIS PROBE IS FOR. The tick is the bridge's own counter and cannot be absent.
// The DATE is a read of game memory and can, and the rule this bead adopted is that a
// line which cannot read the date SAYS SO — never a zero, never a stale value, never
// a plausible-looking date nobody measured. That is the same rule as ck-6xi6's
// `heir_ai_enabled=unobserved` and ck-7yrp's `Unknown`.
//
// THE ABSENT CASE IS THE DRIVABLE ONE, and it is the one that matters. With no game
// image mapped, safe_read_offset returns 0, so this exercises exactly the path a real
// bridge takes before a save is loaded — which is when a wrong answer would be most
// plausible and least noticed.

#include "claim_event_log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <vector>

// The library globals safe_read_offset reads. LEFT EMPTY ON PURPOSE: that is what
// "no game state" means, and it is the state under test.
namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex g_ranges_mutex;
}  // namespace bridge

static int g_failures = 0;
static void row(const char* name, bool ok, const std::string& detail) {
    printf("%-58s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

int main() {
    char buf[64];

    // ── AN UNREADABLE DATE IS NAMED, NOT ZEROED ──────────────────────────────
    bridge::claim_event_stamp(buf, sizeof buf, 1234);
    const std::string s(buf);
    row("an_unreadable_date_says_UNREAD",
        s.find("game_date=unread") != std::string::npos,
        "got: " + s + " — absence is its own state, as for heir_ai_enabled and "
        "ReleaseSubjectState::Unknown");
    row("and_does_NOT_print_a_zero_date",
        s.find("game_date=0") == std::string::npos
        && s.find("0000.") == std::string::npos
        && s.find("game_date=1.1.1") == std::string::npos,
        "a zero or epoch date would be indistinguishable from a measured one, "
        "which is the whole failure this rule exists to stop");

    // ── THE TICK IS ALWAYS THERE, because it is ours and not a memory read ───
    row("the_TICK_is_carried_even_with_no_game_state",
        s.find("tick=1234") != std::string::npos,
        "the bridge's own counter has no absent state, so a line with no date must "
        "still locate itself in tick space — which is what the 50-tick floor "
        "denied");

    // ── DIFFERENT TICKS PRODUCE DIFFERENT OUTPUT ─────────────────────────────
    // Without this the row above would pass on a function that hardcoded its
    // output, which is the constant-satisfies-the-condition trap.
    char other[64];
    bridge::claim_event_stamp(other, sizeof other, 7300);
    row("the_tick_is_the_ARGUMENT_and_not_a_constant",
        std::string(other).find("tick=7300") != std::string::npos
        && std::string(other) != s,
        "a hardcoded line would satisfy the row above; two ticks must give two "
        "answers");

    // ── ASCII ONLY, so the blind-rebuild witness can key on whole tokens ─────
    // ck-cwal's log literal carried a section sign, which TRUNCATES `strings`: the
    // full literal read 0 while its ASCII prefix read 1, and the bead's own witness
    // requirement was unmeetable as written. Every byte here must be plain ASCII.
    bool ascii = true;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7E) ascii = false;
    row("every_byte_is_printable_ASCII", ascii,
        "a non-ASCII byte truncates `strings` output and makes the literal useless "
        "as a witness — measured on ck-cwal, where it cost the bead its witness");

    // ── AND IT DOES NOT OVERRUN A SHORT BUFFER ───────────────────────────────
    char tiny[8];
    memset(tiny, 0x7F, sizeof tiny);
    bridge::claim_event_stamp(tiny, sizeof tiny, 999999);
    row("a_short_buffer_is_TRUNCATED_and_terminated",
        tiny[sizeof(tiny) - 1] == '\0' && strlen(tiny) < sizeof(tiny),
        "snprintf bounds it; a log helper that overran would corrupt the very "
        "artefact it exists to produce");

    // ── AND NOW THE WITH-DATE BRANCH, DRIVEN ─────────────────────────────────
    // A MUTANT SURVIVED HERE. Hardcoding the tick in the with-date snprintf went
    // undetected, because every row above runs with no game image and therefore only
    // ever exercises the `unread` branch. Half the function had no executed witness,
    // and the row named "the tick is the ARGUMENT and not a constant" was true of the
    // half it could reach and silent about the other.
    //
    // safe_read uses process_vm_readv on OUR OWN pid, so a fake image is just
    // memory: plant a pointer where GAME_STATE_ROOT resolves, and a raw date at
    // +GAME_DATE inside what it points to.
    {
        static uint8_t state[ck3::game_state::GAME_DATE + 64];
        static uintptr_t gs_slot;
        memset(state, 0, sizeof state);
        const int32_t raw = ck3::game_date::ymd_to_raw(1066, 9, 15);
        memcpy(state + ck3::game_state::GAME_DATE, &raw, sizeof raw);
        gs_slot = reinterpret_cast<uintptr_t>(state);
        bridge::g_base_addr = reinterpret_cast<uintptr_t>(&gs_slot)
                            - ck3::globals::GAME_STATE_ROOT;

        char dated[64];
        bridge::claim_event_stamp(dated, sizeof dated, 5150);
        const std::string d(dated);
        row("a_READABLE_date_is_formatted_not_reported_unread",
            d.find("game_date=1066.09.15") != std::string::npos,
            "got: " + d + " — the with-date branch, driven against a planted image "
            "because process_vm_readv reads our own pid");
        row("the_tick_is_the_ARGUMENT_in_the_WITH_DATE_branch_too",
            d.find("tick=5150") != std::string::npos,
            "the mutant that hardcoded the tick here SURVIVED, because every other "
            "row runs with no image and never reaches this line");
        bool ascii2 = true;
        for (unsigned char c : d) if (c < 0x20 || c > 0x7E) ascii2 = false;
        row("the_formatted_date_is_ASCII_too", ascii2,
            "the calendar's own output must not break the witness either");
        bridge::g_base_addr = 0;   //: leave the world as we found it
    }

    printf("\n%s (%d failing rows)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
