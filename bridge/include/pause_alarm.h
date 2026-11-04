// pause_alarm.h — the game-day pause alarm (L20-A, smoke17 new item 1).
//
// `session(action="wait", game_days=10)` returned `game_days_advanced 25` after
// one 15 s call: the server could only read the date between its own sleeps, so
// the stop was a polling artefact of the first probe (15 s at ~1.7 game-days
// per second). The alarm moves the stop onto the game thread: the session arms
// a target date with `unpause_game {"pause_at":"Y.M.D"}`; the drain, which runs
// on the session thread between engine commands (every ~5 ms while idle, after
// every command pass, the daily tick included: re/findings/
// re_frame_update_hook.md §1-§4), pauses the game at the FIRST pass whose game
// date is at or past the target, and records the date it paused at.
//
// Semantics, exactly:
//   * arm: refused when the target is not strictly after the game date read at
//     arming (`pause_at_not_in_future`): a target already reached would pause
//     before any day ran, and a past one is a caller's arithmetic error.
//   * fire: at the first drain pass where today >= target. One CDailyTickCommand
//     advances the date one day; the main thread dispatches at most one per
//     frame, so at speeds 1-3 (the bridge caps its own speed writes at 3) the
//     pass after the tick that reaches the target fires and `paused_at` ==
//     target. When the engine has queued more than one tick before a pass (a
//     slow frame at speed 4-5, or one already dispatched when the pause lands),
//     the date can pass the target before the pause takes hold: `paused_at`
//     (the date at the firing pass) and the date read after the pause say so;
//     nothing rounds them back to the target.
//   * clear: any pause written through clock_write disarms an armed alarm
//     (`cleared_by:"pause"`), and an unpause without `pause_at` disarms it
//     (`cleared_by:"unpause"`): an alarm outlives nothing but its own wait. A
//     pause the ENGINE makes (an event popup) is not a clock_write: the alarm
//     stays armed and fires once the clock runs on to the target.
//
// PURE: days are ints (game_date.h's total days), the caller reads the date.
// Thread-safe (one mutex); `armed()` is a lock-free fast path for the drain.
// Host-tested by bridge/test/test_pause_alarm.cpp.
#ifndef CK3_BRIDGE_PAUSE_ALARM_H
#define CK3_BRIDGE_PAUSE_ALARM_H

#include <cstdint>

namespace bridge {
namespace pause_alarm {

enum class State { None, Armed, Fired, Cleared };

enum class Arm { Armed, NotInFuture };

struct Snapshot {
    State state = State::None;
    int32_t target_day = 0;     // valid unless state == None
    int32_t armed_at_day = 0;   // the game date read when armed
    int32_t paused_at_day = 0;  // valid when state == Fired
    const char* cleared_by = "";  // "pause" | "unpause" when state == Cleared
    uint64_t arms = 0;          // process-lifetime counts
    uint64_t fires = 0;
};

// Arm for `target_day`, `now_day` being the game date read just before.
Arm arm(int32_t target_day, int32_t now_day);

// Lock-free: is an alarm armed? (the drain's fast path)
bool armed();

// The drain's check: if armed and now_day >= target, disarm, record Fired at
// now_day and return true (the caller then pauses). Exactly one caller sees
// true per arming.
bool take_due(int32_t now_day);

// Disarm an armed alarm (no-op otherwise); `why` must be a string literal.
void clear(const char* why);

Snapshot snapshot();

const char* state_name(State s);

// Parse "Y.M.D" (1-2 digit month/day, the engine calendar: real month lengths,
// no leap day) to game_date total days. False on anything else.
bool parse_date(const char* s, int32_t* total_days);

// Test seam: back to State::None with zero counts.
void reset_for_test();

}  // namespace pause_alarm
}  // namespace bridge

#endif  // CK3_BRIDGE_PAUSE_ALARM_H
