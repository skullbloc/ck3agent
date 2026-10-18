// Regression test for ck-05mg: the bridge must render dates with CK3's real
// calendar (31/28/31/30/31/30/31/31/30/31/30/31, never leap), not twelve
// 30-day months.
//
// Build: CMake target `test_game_date` (BUILD_BRIDGE_TESTS=ON).
// Run:   ./test_game_date   (exits non-zero on failure)
//
// The pre-fix formula produced month 13 for days-of-year 360..364 and drifted
// by up to four days the rest of the year. GROUND TRUTH pinned below: the raw
// game-date word read out of ~/ck3_dumps/william_1066_baseline.core is
// 53144904, and the save CK3 itself wrote at that same instant carries
// meta_date=1066.10.9 (filename Duke_William_II_of_Normandy_1066_10_09.ck3).
// The old formula said 1066.10.12.

#include <cstdio>
#include <cstring>

#include "game_date.h"

using namespace ck3::game_date;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d): %s\n", #cond, __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

// The engine's own month-length table, byte-identical to the u8[12] constant at
// base-relative 0x084dd26a (and its copy at 0x08d1cb3f) in the CK3 binary.
static const int kEngineMonthLengths[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

static void test_month_table_matches_engine() {
    int sum = 0;
    for (int m = 1; m <= 12; ++m) {
        CHECK(days_in_month(m) == kEngineMonthLengths[m-1], "month length differs from engine table");
        sum += days_in_month(m);
    }
    CHECK(sum == 365, "month lengths must sum to exactly 365 (twelve 30-day months = 360 was the bug)");
    CHECK(MONTH_CUM_DAYS[12] == DAYS_PER_YEAR, "cumulative sentinel must be 365");
    // No leap variant exists in the binary; assert we did not sneak one in.
    CHECK(days_in_month(2) == 28, "February is 28 days — CK3 has no leap years");
}

// THE acceptance pair: raw memory vs the save CK3 wrote at the same instant.
static void test_ground_truth_william_1066() {
    const int32_t raw = 53144904;  // *(GAME_STATE_ROOT)+GAME_DATE in the core dump

    CHECK(raw_to_total_days(raw) == 389371, "total_days must be exact (24 sub-day ticks/day)");

    char buf[16];
    format_raw(raw, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "1066.10.09") == 0, "must equal the save's own meta_date 1066.10.9");

    int y = 0, m = 0, d = 0;
    raw_to_ymd(raw, &y, &m, &d);
    CHECK(y == 1066 && m == 10 && d == 9, "y/m/d decode must match meta_date");

    // And it must NOT be the old 30-day-month answer.
    CHECK(std::strcmp(buf, "1066.10.12") != 0, "regressed to the twelve-30-day-month formula");
}

// Walk every day of a long span: no month 13, no out-of-range day, exact
// round-trip. 1066..1086 is the paper's horizon; go wider to be safe.
static void test_full_sweep_no_phantom_month() {
    const int32_t first = ymd_to_total_days(800, 1, 1);
    const int32_t last  = ymd_to_total_days(1300, 1, 1);
    int32_t seen_doy360_plus = 0;

    for (int32_t td = first; td < last; ++td) {
        int y = 0, m = 0, d = 0;
        total_days_to_ymd(td, &y, &m, &d);

        if (m < 1 || m > 12) {
            std::fprintf(stderr, "FAIL: total_days %d rendered month %d\n", td, m);
            failures++;
            return;  // one report is enough
        }
        if (d < 1 || d > days_in_month(m)) {
            std::fprintf(stderr, "FAIL: total_days %d rendered %04d.%02d.%02d (day out of range)\n", td, y, m, d);
            failures++;
            return;
        }
        if (ymd_to_total_days(y, m, d) != td) {
            std::fprintf(stderr, "FAIL: round-trip broke at total_days %d (%04d.%02d.%02d)\n", td, y, m, d);
            failures++;
            return;
        }
        // The five days a year the old formula turned into month 13.
        int doy = td - ymd_to_total_days(y, 1, 1);
        if (doy >= 360) seen_doy360_plus++;
    }

    // 500 years x 5 days — proves the sweep actually visited the poisoned range.
    CHECK(seen_doy360_plus == 500 * 5, "sweep must cover days-of-year 360..364");
}

// One full year, spelled out: each month starts the day after the previous ends.
static void test_year_boundaries() {
    const int32_t jan1 = ymd_to_total_days(1066, 1, 1);
    int expect_doy = 0;
    for (int m = 1; m <= 12; ++m) {
        int y = 0, mm = 0, d = 0;
        total_days_to_ymd(jan1 + expect_doy, &y, &mm, &d);
        CHECK(y == 1066 && mm == m && d == 1, "1st of each month must land correctly");

        total_days_to_ymd(jan1 + expect_doy + days_in_month(m) - 1, &y, &mm, &d);
        CHECK(y == 1066 && mm == m && d == days_in_month(m), "last of each month must land correctly");

        expect_doy += days_in_month(m);
    }
    CHECK(expect_doy == 365, "twelve months must be 365 days");

    // Dec 31 -> Jan 1 next year, no phantom 13th month in between.
    int y = 0, m = 0, d = 0;
    total_days_to_ymd(ymd_to_total_days(1066, 12, 31) + 1, &y, &m, &d);
    CHECK(y == 1067 && m == 1 && d == 1, "Dec 31 + 1 day must be Jan 1 of the next year");
}

// Sub-day ticks: 24 raw units per day, and the day must not advance early.
static void test_sub_day_ticks() {
    const int32_t raw_midnight = ymd_to_raw(1066, 10, 9);
    CHECK(raw_midnight == 53144904, "ymd_to_raw must invert the ground-truth read");

    char a[16], b[16], c[16];
    format_raw(raw_midnight, a, sizeof(a));
    format_raw(raw_midnight + 23, b, sizeof(b));
    format_raw(raw_midnight + 24, c, sizeof(c));
    CHECK(std::strcmp(a, "1066.10.09") == 0, "midnight");
    CHECK(std::strcmp(b, "1066.10.09") == 0, "+23 ticks is still the same day");
    CHECK(std::strcmp(c, "1066.10.10") == 0, "+24 ticks is the next day");
}

// The epoch itself, and pre-epoch raws (floor, not truncate-toward-zero).
static void test_epoch_and_negative() {
    char buf[16];
    format_raw(ck3::game_state::DATE_EPOCH, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "0000.01.01") == 0, "DATE_EPOCH is year 0, Jan 1");

    CHECK(raw_to_total_days(ck3::game_state::DATE_EPOCH - 1) == -1,
          "one tick before the epoch is the previous day, not day 0");
    format_raw(ck3::game_state::DATE_EPOCH - 24, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "-001.12.31") == 0 || std::strcmp(buf, "00-1.12.31") == 0,
          "pre-epoch decodes to the last day of year -1");
}

// raw_to_month_index must advance exactly 12 per year and exactly once per
// month — it is the gate for emit_monthly_telemetry.
static void test_month_index() {
    int32_t prev = raw_to_month_index(ymd_to_raw(1066, 1, 1));
    CHECK(prev == 1066 * 12, "month index is year*12 + (month-1)");

    int rollovers = 0;
    const int32_t first = ymd_to_total_days(1066, 1, 1);
    for (int32_t td = first + 1; td <= first + 365 * 3; ++td) {  // through 1069.01.01
        int32_t idx = raw_to_month_index(td * TICKS_PER_DAY + ck3::game_state::DATE_EPOCH);
        if (idx != prev) {
            CHECK(idx == prev + 1, "month index must step by exactly 1");
            rollovers++;
            prev = idx;
        }
    }
    CHECK(rollovers == 36, "three game years must produce exactly 36 month rollovers, not 39");
}

// The concrete defects observed in the E0 paper series (ep-...-e0-paper-9eae1e0e):
// three of 99 samples carried an impossible month. Whatever total_days those
// samples really were, no total_days may now render as month 13.
static void test_e0_series_dates_are_impossible_now() {
    const char* impossible[] = {"1066.13.01", "1083.13.05", "1084.13.01"};
    const int32_t first = ymd_to_total_days(1060, 1, 1);
    const int32_t last  = ymd_to_total_days(1090, 1, 1);
    for (int32_t td = first; td < last; ++td) {
        char buf[16];
        format_total_days(td, buf, sizeof(buf));
        for (const char* bad : impossible) {
            if (std::strcmp(buf, bad) == 0) {
                std::fprintf(stderr, "FAIL: still renders the impossible date %s\n", bad);
                failures++;
                return;
            }
        }
    }
}

int main() {
    test_month_table_matches_engine();
    test_ground_truth_william_1066();
    test_full_sweep_no_phantom_month();
    test_year_boundaries();
    test_sub_day_ticks();
    test_epoch_and_negative();
    test_month_index();
    test_e0_series_dates_are_impossible_now();

    if (failures) {
        std::fprintf(stderr, "\ntest_game_date: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_game_date: all checks passed\n");
    return 0;
}
