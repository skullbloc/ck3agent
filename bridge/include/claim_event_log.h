// claim_event_log.h — EVERY CLAIM-SET EVENT, STAMPED (ck-jij9)
//
// THE DEFECT. The claim-set events were emitted three different ways:
//
//   release_character:      raw fprintf, no stamp, no tick
//   set_controlled_character: raw fprintf, no stamp, no tick
//   CONTROLLED_HANDOFF:     blog(), monotonic stamp, still no tick
//
// and the only tick-bearing line in the log is "BUILD-B: DIAG tick=" every 50
// ticks. So a vacancy is locatable from the log only to a 50-tick marker interval —
// and ep-02's vacancies ARE 50 ticks. The measurement floor equalled the phenomenon,
// which means the log could not corroborate a vacancy at all.
//
// THE LEDGER REMAINS THE PRIMARY EXACT SOURCE (bridge_record.tick_executed, ck-324r,
// now actually reaching the observer after ck-h4pn). This makes the log a full
// CORROBORATOR rather than a second-best substitute: two independent artefacts that
// can disagree, which is the whole point of a corroborator.
//
// ABSENCE IS ITS OWN STATE. If the game state is not loaded the date cannot be read,
// and this prints `game_date=unread` — never a zero, never a stale value, never a
// plausible-looking date nobody measured. That is the same rule as ck-6xi6's
// `heir_ai_enabled=unobserved` and ck-7yrp's `Unknown`: a value that was not observed
// must not be formatted like one that was.
//
// AND THE LITERALS ARE ASCII. ck-cwal's log line contains a section sign, which
// TRUNCATES `strings` output and made the bead's own witness unusable — the full
// literal read 0 while its ASCII prefix read 1. Every literal here is ASCII so the
// blind-rebuild witness list can key on the whole token.
//
// NO SECOND CALENDAR. The date comes from ck3::game_date::format_raw, the one
// sanctioned formatter — pipeline.cpp's static current_game_date() calls the same
// function. scripts/test_game_calendar.py section D fails the build if a calendar is
// open-coded outside game_date.h, and that rule is why this reads the raw int32 and
// hands it straight to the formatter rather than doing any arithmetic of its own.
#pragma once

#include "bridge.h"
#include "game_date.h"
#include "ck3_offsets.h"
//: so one include gives a caller BOTH halves of the stamp — the monotonic
//: wall-clock prefix the rest of the log already carries, and the game tick +
//: date this bead adds. Two includes for one stamp invites one being forgotten.
#include "log_stamp.h"

#include <cstdint>
#include <cstdio>

namespace bridge {

//: The claim-set event stamp: the current tick and the current game date, or a named
//: absence for the date. Writes at most `n` bytes including the terminator.
//:
//: TICK IS ALWAYS AVAILABLE — it is the bridge's own counter, not a read of game
//: memory — so it has no absent state. The DATE is a memory read and does.
inline void claim_event_stamp(char* out, size_t n, uint64_t tick) {
    const uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) {
        //: WHOLE LITERAL for the absent case, so `strings` finds it and a reader can
        //: tell "the state was not loaded" from "the date happened to be 0".
        snprintf(out, n, "tick=%llu game_date=unread",
                 static_cast<unsigned long long>(tick));
        return;
    }
    const int32_t raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    char datebuf[16];
    ck3::game_date::format_raw(raw, datebuf, sizeof datebuf);
    snprintf(out, n, "tick=%llu game_date=%s",
             static_cast<unsigned long long>(tick), datebuf);
}

}  // namespace bridge
