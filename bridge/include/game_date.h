// game_date.h — THE single source of truth for CK3 raw-date -> Y.M.D rendering.
//
// ck-05mg. Every date the bridge emits used to be rendered with twelve 30-day
// months at nine separate copy-pasted sites:
//
//     year = total_days / 365; doy = total_days % 365;
//     month = doy / 30 + 1;    day = doy % 30 + 1;      // WRONG
//
// Twelve 30-day months is 360, not 365, so days-of-year 360..364 rendered as
// MONTH 13 (a date CK3 cannot produce — three of the 99 samples in the E0
// paper series carried 1066.13.01 / 1083.13.05 / 1084.13.01) and the rest of
// the year drifted from the real calendar by up to four days.
//
// ─── THE ENGINE'S REAL CALENDAR ────────────────────────────────────────────
// CK3 uses a fixed 365-day year with REAL (never-leap) month lengths:
//
//     31 28 31 30 31 30 31 31 30 31 30 31   (= 365)
//
// Four independent sources, all agreeing (HIGH confidence):
//
//  1. GROUND TRUTH, raw memory vs the save CK3 itself wrote at that instant.
//     ~/ck3_dumps/william_1066_baseline.core, *(GAME_STATE_ROOT)+GAME_DATE
//     = 53144904 -> total_days 389371 (exact, remainder 0 sub-day ticks).
//       old rendering : 1066.10.12
//       this header   : 1066.10.09
//       save meta_date: 1066.10.9   <- CK3 is the authority on its own calendar
//       save filename : Duke_William_II_of_Normandy_1066_10_09.ck3
//
//  2. The game binary's own constant tables (file offset == base-relative
//     offset; these sit in the read-only LOAD segment, identity-mapped):
//       0x084dd26a  u8[12]  1f 1c 1f 1e 1f 1e 1f 1f 1e 1f 1e 1f  (month lengths)
//       0x08d1cb3f  u8[12]  (same table, second copy)
//       0x08e93be0  i32[12] 0,31,59,90,120,151,181,212,243,273,304,334
//                           (cumulative days before each month — MONTH_CUM_DAYS)
//     Ghidra image VA = these + 0x100000. Mirrored in re/offsets/ck3_offsets.h.
//
//  3. NO leap-year variant exists anywhere in the binary: neither the leap
//     month-length table {31,29,31,...} nor the leap cumulative table
//     {0,31,60,91,...} appears at any offset. There is no Feb-29 path.
//
//  4. DATE_EPOCH = 43800000 = 5000 * 365 * 24 — an epoch that is only exact
//     if a year is exactly 365 days. And the shipped game data agrees: across
//     every date literal in game/history/, max day per month is
//     31/28/31/30/31/30/31/31/30/31/30/31 (the ten 2.29/2.30 literals are
//     authoring artifacts in converted Japanese/steppe history, against 70
//     literals at 2.28).
//
// ─── UNITS ─────────────────────────────────────────────────────────────────
// The raw int32 at *(GAME_STATE_ROOT)+GAME_DATE counts SUB-DAY TICKS, 24 per
// day, since DATE_EPOCH. total_days is exact; only the Y.M.D rendering was
// ever wrong, so the "1 on_tick == 1 game day" result is unaffected.
//
// Header-only and free of engine reads on purpose: bridge/test/test_game_date.cpp
// and scripts/test_game_calendar.py compile against it directly.

#ifndef CK3_BRIDGE_GAME_DATE_H
#define CK3_BRIDGE_GAME_DATE_H

#include <cstdint>
#include <cstdio>
#include <cstddef>

#include "ck3_offsets.h"

namespace ck3 {
namespace game_date {

// The calendar constants themselves live in re/offsets/ck3_offsets.h — that
// header is the contract between RE work and bridge code, and the tables there
// carry the evidence + the engine's own .rodata anchors. Re-exported here so
// callers say ck3::game_date::MONTH_CUM_DAYS, with no second copy to drift.

// Sub-day ticks per game day (confirmed via Ghidra: / 0x18 and / 6 >> 2).
constexpr int32_t TICKS_PER_DAY = ck3::game_state::TICKS_PER_DAY;

// A CK3 year is exactly this many days. Never 366 — see source (3) above.
constexpr int32_t DAYS_PER_YEAR = ck3::game_state::DAYS_PER_YEAR;

constexpr int MONTHS_PER_YEAR = 12;

// Days elapsed before the 1st of month N (1-based N indexes [N-1]); the
// trailing 365 is the year sentinel. Byte-identical to the engine's own i32
// table at base-relative ck3::game_state::MONTH_CUM_DAYS_I32.
inline constexpr const int32_t (&MONTH_CUM_DAYS)[MONTHS_PER_YEAR + 1] =
    ck3::game_state::MONTH_CUM_DAYS;

// Length of month N (1-based).
inline constexpr int32_t days_in_month(int month) {
    return MONTH_CUM_DAYS[month] - MONTH_CUM_DAYS[month - 1];
}

// Raw sub-day-tick date -> absolute days since the epoch (year 0, Jan 1).
// Floors, so pre-epoch raws land on the right day instead of truncating
// toward zero.
inline int32_t raw_to_total_days(int32_t raw) {
    int32_t adjusted = raw - ck3::game_state::DATE_EPOCH;
    int32_t days = adjusted / TICKS_PER_DAY;
    if (adjusted % TICKS_PER_DAY < 0) days--;
    return days;
}

// Absolute day count -> calendar year/month/day. month is 1..12, ALWAYS —
// there is no path through this function that yields 13.
inline void total_days_to_ymd(int32_t total_days, int* out_year, int* out_month, int* out_day) {
    int year = total_days / DAYS_PER_YEAR;
    int doy  = total_days % DAYS_PER_YEAR;
    if (doy < 0) { year--; doy += DAYS_PER_YEAR; }

    int m = 0;
    while (m < MONTHS_PER_YEAR - 1 && doy >= MONTH_CUM_DAYS[m + 1]) m++;

    if (out_year)  *out_year  = year;
    if (out_month) *out_month = m + 1;
    if (out_day)   *out_day   = doy - MONTH_CUM_DAYS[m] + 1;
}

inline void raw_to_ymd(int32_t raw, int* out_year, int* out_month, int* out_day) {
    total_days_to_ymd(raw_to_total_days(raw), out_year, out_month, out_day);
}

// Inverse, for tests and for date arithmetic that must round-trip.
inline int32_t ymd_to_total_days(int year, int month, int day) {
    return year * DAYS_PER_YEAR + MONTH_CUM_DAYS[month - 1] + (day - 1);
}

inline int32_t ymd_to_raw(int year, int month, int day) {
    return ymd_to_total_days(year, month, day) * TICKS_PER_DAY + ck3::game_state::DATE_EPOCH;
}

// Render a raw date as the bridge's canonical "%04d.%02d.%02d".
inline void format_raw(int32_t raw, char* buf, size_t bufsz) {
    int y = 0, m = 0, d = 0;
    raw_to_ymd(raw, &y, &m, &d);
    snprintf(buf, bufsz, "%04d.%02d.%02d", y, m, d);
}

inline void format_total_days(int32_t total_days, char* buf, size_t bufsz) {
    int y = 0, m = 0, d = 0;
    total_days_to_ymd(total_days, &y, &m, &d);
    snprintf(buf, bufsz, "%04d.%02d.%02d", y, m, d);
}

// Calendar year of a raw date (cheaper than rendering when only the year is
// wanted, and consistent with it).
inline int raw_to_year(int32_t raw) {
    int y = 0;
    total_days_to_ymd(raw_to_total_days(raw), &y, nullptr, nullptr);
    return y;
}

// Monotonic absolute month index (year*12 + month-1) — used to fire
// once-per-game-month work exactly once.
inline int32_t raw_to_month_index(int32_t raw) {
    int y = 0, m = 0;
    total_days_to_ymd(raw_to_total_days(raw), &y, &m, nullptr);
    return static_cast<int32_t>(y) * MONTHS_PER_YEAR + (m - 1);
}

}  // namespace game_date
}  // namespace ck3

#endif  // CK3_BRIDGE_GAME_DATE_H
