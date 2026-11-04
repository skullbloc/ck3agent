// test_pause_alarm.cpp — L20-A: the game-day pause alarm's rule (pause_alarm.h),
// driven the way the drain drives it: a date that advances by whole days, a
// check after every pass, and the pause the caller would write.
#include <cstdio>
#include <cstring>
#include "pause_alarm.h"
#include "game_date.h"

namespace pa = bridge::pause_alarm;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static int32_t day(int y, int m, int d) { return ck3::game_date::ymd_to_total_days(y, m, d); }

// Run the clock from `start` in steps of `step` days per pass until the alarm
// fires (the drain's take_due) or `max_passes` pass; returns the paused date.
static int32_t run(int32_t start, int step, int max_passes, int* passes_out) {
    int32_t today = start;
    for (int i = 0; i < max_passes; ++i) {
        today += step;                         // the daily tick(s) of this pass
        if (pa::take_due(today)) { if (passes_out) *passes_out = i + 1; return today; }
    }
    if (passes_out) *passes_out = max_passes;
    return -1;
}

int main() {
    int32_t t = 0;
    // parse: the engine calendar, nothing else
    CHECK(pa::parse_date("867.2.5", &t) && t == day(867, 2, 5));
    CHECK(pa::parse_date("0867.02.05", &t) && t == day(867, 2, 5));
    CHECK(pa::parse_date("867.12.31", &t) && t == day(867, 12, 31));
    CHECK(!pa::parse_date("867.2.29", &t));      // no leap day on this calendar
    CHECK(!pa::parse_date("867.13.1", &t) && !pa::parse_date("867.0.1", &t));
    CHECK(!pa::parse_date("867.1.0", &t) && !pa::parse_date("867.4.31", &t));
    CHECK(!pa::parse_date("", &t) && !pa::parse_date(nullptr, &t));
    CHECK(!pa::parse_date("867.1", &t) && !pa::parse_date("867.1.1x", &t) && !pa::parse_date("-1.1.1", &t));

    // a target at or before today is refused and arms nothing
    pa::reset_for_test();
    const int32_t now = day(867, 1, 26);
    CHECK(pa::arm(now, now) == pa::Arm::NotInFuture);
    CHECK(pa::arm(now - 1, now) == pa::Arm::NotInFuture);
    CHECK(!pa::armed() && pa::snapshot().state == pa::State::None);

    // smoke17's ask: 10 days from 0867.01.26 — one day per pass fires AT the target
    CHECK(pa::arm(now + 10, now) == pa::Arm::Armed && pa::armed());
    int passes = 0;
    int32_t at = run(now, 1, 1000, &passes);
    CHECK(at == now + 10 && passes == 10);
    pa::Snapshot s = pa::snapshot();
    CHECK(s.state == pa::State::Fired && s.paused_at_day == now + 10 && s.target_day == now + 10);
    CHECK(s.armed_at_day == now && s.arms == 1 && s.fires == 1);
    CHECK(!pa::armed() && !pa::take_due(now + 11));   // exactly one fire per arming

    // a pass that jumps several days fires at the first date >= target, and says so
    CHECK(pa::arm(now + 10, now) == pa::Arm::Armed);
    at = run(now, 3, 1000, &passes);
    CHECK(at == now + 12 && passes == 4);
    CHECK(pa::snapshot().paused_at_day == now + 12 && pa::snapshot().target_day == now + 10);

    // passes with no tick between them (the idle 5 ms passes) do not fire early
    CHECK(pa::arm(now + 2, now) == pa::Arm::Armed);
    for (int i = 0; i < 50; ++i) CHECK(!pa::take_due(now));
    CHECK(pa::take_due(now + 2));

    // clear: a pause or a plain unpause disarms an armed alarm, and names itself
    CHECK(pa::arm(now + 5, now) == pa::Arm::Armed);
    pa::clear("pause");
    s = pa::snapshot();
    CHECK(s.state == pa::State::Cleared && strcmp(s.cleared_by, "pause") == 0 && !pa::armed());
    CHECK(!pa::take_due(now + 99));                    // a cleared alarm never fires
    // ...and clearing a fired alarm changes nothing (the alarm's own pause)
    CHECK(pa::arm(now + 1, now) == pa::Arm::Armed && pa::take_due(now + 1));
    pa::clear("pause");
    CHECK(pa::snapshot().state == pa::State::Fired);
    // a refused re-arm leaves the previous alarm as it was
    CHECK(pa::arm(now + 4, now) == pa::Arm::Armed);
    CHECK(pa::arm(now, now) == pa::Arm::NotInFuture);
    CHECK(pa::armed() && pa::snapshot().target_day == now + 4);
    pa::clear("unpause");
    CHECK(strcmp(pa::snapshot().cleared_by, "unpause") == 0);

    CHECK(strcmp(pa::state_name(pa::State::Fired), "fired") == 0);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
