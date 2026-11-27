// test_army_orders.cpp — the switches, the hold table and the hold
// hook's drop decision (bridge/include/army_orders.h), engine-free.
//
// The ids model the 1097 dump's unit 285212676 (army 285212676, owner-link id
// 0x11000004): an AI CMoveUnitCommand carries src 2 at +0x20 and the unit id at +0x24.
#include <cstdio>
#include <cstdlib>
#include "army_orders.h"

namespace ao = bridge::army_orders;

static int g_fail = 0, g_rows = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        ++g_rows;                                                           \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

int main() {
    const int32_t DAY = ao::kTicksPerDay;
    const int32_t NOW = 37000000;          // a game date raw (1066.09.15-ish)
    const uint32_t UNIT = 0x11000004, ARMY = 0x11000004, OTHER = 0x01000007;

    // 1. the switches: exactly "1" is on.
    ROW("1a switch unset is off", !ao::switch_on(nullptr));
    ROW("1b switch empty is off", !ao::switch_on(""));
    ROW("1c switch 0 is off", !ao::switch_on("0"));
    ROW("1d switch yes / 11 / '1 ' are off", !ao::switch_on("yes") && !ao::switch_on("11") && !ao::switch_on("1 "));
    ROW("1e switch 1 is on", ao::switch_on("1"));

    // 2. the ops.
    ROW("2a op names round-trip", ao::op_by_name("disband") == ao::Op::kDisband &&
                                      ao::op_by_name("move") == ao::Op::kMove &&
                                      ao::op_by_name("hold") == ao::Op::kHold &&
                                      ao::op_by_name("release") == ao::Op::kRelease &&
                                      ao::op_by_name("holds") == ao::Op::kHolds &&
                                      std::string_view(ao::op_name(ao::Op::kMove)) == "move");
    ROW("2a' raid is an op that writes the game", ao::op_by_name("raid") == ao::Op::kRaid &&
                                                       std::string_view(ao::op_name(ao::Op::kRaid)) == "raid" &&
                                                       ao::op_writes_game(ao::Op::kRaid) &&
                                                       !ao::op_needs_hold(ao::Op::kRaid));
    ROW("2a'' raid toggles only toward the state asked", ao::raid_needs_toggle(1, false) &&
                                                          !ao::raid_needs_toggle(1, true) &&
                                                          ao::raid_needs_toggle(0, true) &&
                                                          !ao::raid_needs_toggle(0, false));
    ROW("2b an unknown op is none", ao::op_by_name("siege") == ao::Op::kNone && ao::op_by_name("") == ao::Op::kNone);
    ROW("2c only disband and move write the game",
        ao::op_writes_game(ao::Op::kDisband) && ao::op_writes_game(ao::Op::kMove) &&
            !ao::op_writes_game(ao::Op::kHold) && !ao::op_writes_game(ao::Op::kHolds));
    ROW("2d hold, release and holds need the hook", ao::op_needs_hold(ao::Op::kHold) &&
                                                        ao::op_needs_hold(ao::Op::kRelease) &&
                                                        ao::op_needs_hold(ao::Op::kHolds) &&
                                                        !ao::op_needs_hold(ao::Op::kDisband));
    ROW("2e hold days 1..365", ao::hold_days_ok(1) && ao::hold_days_ok(365) && !ao::hold_days_ok(0) &&
                                   !ao::hold_days_ok(366) && !ao::hold_days_ok(-3));

    // 3. an empty table passes everything (the fast path).
    {
        static ao::HoldTable t;
        ROW("3 empty table: no_holds", ao::decide(t, true, ao::kSourceAi, UNIT, NOW) == ao::Pass::kNoHolds);
    }
    // 4. a held unit: the AI's move is dropped; the UI's, an unchecked one, another
    //    command class and another unit pass.
    {
        static ao::HoldTable t;
        int s = t.set(UNIT, ARMY, 31506, NOW + 30 * DAY);
        int slot = -1;
        ROW("4a set returns a slot", s >= 0 && t.count.load() == 1);
        ROW("4b AI move of the held unit is dropped",
            ao::decide(t, true, ao::kSourceAi, UNIT, NOW, &slot) == ao::Pass::kDrop && slot == s);
        ROW("4c the UI's move (src 1) passes", ao::decide(t, true, 1, UNIT, NOW) == ao::Pass::kNotAiSource);
        ROW("4d an unchecked move (src 3) passes", ao::decide(t, true, 3, UNIT, NOW) == ao::Pass::kNotAiSource);
        ROW("4e another command class passes", ao::decide(t, false, ao::kSourceAi, UNIT, NOW) == ao::Pass::kNotMoveCommand);
        ROW("4f another unit passes", ao::decide(t, true, ao::kSourceAi, OTHER, NOW) == ao::Pass::kNotHeld);
        ROW("4g unit 0 never matches", ao::decide(t, true, ao::kSourceAi, 0, NOW) == ao::Pass::kNotHeld);
        // 5. expiry: at the until date the hold has ended (the AI owns the army again).
        ROW("5a one tick before the end it holds", ao::decide(t, true, ao::kSourceAi, UNIT, NOW + 30 * DAY - 1) == ao::Pass::kDrop);
        ROW("5b at the end it passes", ao::decide(t, true, ao::kSourceAi, UNIT, NOW + 30 * DAY) == ao::Pass::kExpired);
        ROW("5c find_unit agrees on expiry", t.find_unit(UNIT, NOW) == s && t.find_unit(UNIT, NOW + 30 * DAY) == -1);
        ROW("5d an unread date (0) does not expire it", ao::decide(t, true, ao::kSourceAi, UNIT, 0) == ao::Pass::kDrop);
        // 6. renew keeps one slot; release by the 24-bit id empties it.
        int s2 = t.set(UNIT, ARMY, 31506, NOW + 60 * DAY);
        ROW("6a renew keeps the slot and the count", s2 == s && t.count.load() == 1 &&
                                                       ao::decide(t, true, ao::kSourceAi, UNIT, NOW + 45 * DAY) == ao::Pass::kDrop);
        ROW("6b find_army by the 24-bit id", t.find_army(ARMY & 0xFFFFFF) == s);
        ROW("6c release by the 24-bit id", t.release_army(ARMY & 0xFFFFFF) && t.count.load() == 0 &&
                                               ao::decide(t, true, ao::kSourceAi, UNIT, NOW) == ao::Pass::kNoHolds);
        ROW("6d release of a free army is false", !t.release_army(ARMY));
    }
    // 7. the table is bounded: the 17th distinct unit is refused, none is evicted.
    {
        static ao::HoldTable t;
        bool all = true;
        for (int i = 0; i < ao::kMaxHolds; ++i) all = all && t.set(0x100 + i, 0x100 + i, 1, NOW + DAY) >= 0;
        ROW("7a sixteen holds fit", all && t.count.load() == ao::kMaxHolds);
        ROW("7b the seventeenth is refused", t.set(0x999, 0x999, 1, NOW + DAY) == -1 && t.count.load() == ao::kMaxHolds);
        ROW("7c the first is still held", ao::decide(t, true, ao::kSourceAi, 0x100, NOW) == ao::Pass::kDrop);
        // 8. prune clears what has ended, and only that.
        t.set(0x100, 0x100, 1, NOW + 10 * DAY);
        int pruned = t.prune(NOW + 2 * DAY);
        ROW("8a prune clears the fifteen that ended", pruned == ao::kMaxHolds - 1 && t.count.load() == 1);
        ROW("8b the renewed one survives", ao::decide(t, true, ao::kSourceAi, 0x100, NOW + 2 * DAY) == ao::Pass::kDrop);
        ROW("8c prune with an unread date clears nothing", t.prune(0) == 0 && t.count.load() == 1);
    }
    // 9. what a move may claim after it ran.
    {
        const uint32_t TARGET = 5285, HERE = 5253;
        ROW("9a path ends at the target: confirmed",
            ao::move_after(true, 5, TARGET, HERE, TARGET) == ao::MoveAfter::kConfirmed);
        ROW("9b path ends elsewhere: other_destination",
            ao::move_after(true, 5, 5231, HERE, TARGET) == ao::MoveAfter::kOtherDestination);
        ROW("9c no path, not there: no_path", ao::move_after(true, 0, 0, HERE, TARGET) == ao::MoveAfter::kNoPath);
        ROW("9d no path, already there", ao::move_after(true, 0, 0, TARGET, TARGET) == ao::MoveAfter::kAlreadyThere);
        ROW("9e a faulted read claims nothing", ao::move_after(false, 5, TARGET, HERE, TARGET) == ao::MoveAfter::kUnread);
        // In a test run the check said no_path and the send found 13 steps: a check claims no path
        ROW("9f a check never reports a path verdict: the unit's own path is not the order's",
            std::string_view(ao::move_after_reply(true, ao::MoveAfter::kNoPath)) == "not_sent" &&
            std::string_view(ao::move_after_reply(true, ao::MoveAfter::kConfirmed)) == "not_sent" &&
            std::string_view(ao::move_path_key(true)) == "current_path_steps");
        ROW("9g CONTROL a sent move keeps its verdict and its path_steps",
            std::string_view(ao::move_after_reply(false, ao::MoveAfter::kNoPath)) == "no_path" &&
            std::string_view(ao::move_after_reply(false, ao::MoveAfter::kConfirmed)) == "path_ends_at_target" &&
            std::string_view(ao::move_path_key(false)) == "path_steps");
    }
    std::printf("%d rows, %d failed\n", g_rows, g_fail);
    return g_fail ? 1 : 0;
}
