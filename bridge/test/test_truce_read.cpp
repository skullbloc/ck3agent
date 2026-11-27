// test_truce_read.cpp — a character's one-way truce on another
// (bridge/include/truce_read.h), engine-free.
//
// The rows model a seated game's truce: Domnall 36124 white-peaced his war on 14492 on
// 1092.05.15 (add_truce_white_peace_effect: scope:attacker add_truce_one_way character =
// defender), so the pair's relation (ids 14492 low, 36124 high) holds a truce in
// 36124's slot (side B, +0x58) until 1097.05.15. The game refused his declarations of
// 1094-1096 with has_truce; the bridge read `truce false`.
#include <cstdio>
#include <cstdint>
#include "truce_read.h"

namespace tr = bridge::truce;

static int g_fail = 0, g_rows = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        ++g_rows;                                                           \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

// days since year 0 as the game counts them (365-day years): enough for ordering here
static int32_t d(int y, int m, int day) {
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    return y * 365 + cum[m - 1] + (day - 1);
}

int main() {
    const int32_t SENT = -1;          // the game's no-truce date (a global the caller reads)
    const int32_t until = d(1097, 5, 15);
    const int32_t today = d(1094, 5, 15);
    // 14492 (low, side A: no truce) and 36124 (high, side B: the white peace's truce)
    auto atk = tr::slot_of(14492, 36124, 36124, SENT, 3, until, 2, today, SENT);
    ROW("attacker's slot (side B) holds the truce", atk.matched && atk.active && atk.expiry == until);
    auto def = tr::slot_of(14492, 36124, 14492, SENT, 3, until, 2, today, SENT);
    ROW("the defender's own slot holds none: a truce is one way", def.matched && !def.active);
    // the same pair stored the other way round (the attacker the low id)
    auto low = tr::slot_of(36124, 99999, 36124, until, 0, SENT, 3, today, SENT);
    ROW("attacker as the low id reads side A", low.active && low.expiry == until && low.result == 0);
    // expiry: the game clears a slot whose date is <= today (FUN_06368b70)
    auto on_day = tr::slot_of(14492, 36124, 36124, SENT, 3, today, 2, today, SENT);
    ROW("a truce ending today has ended", on_day.matched && !on_day.active);
    auto unread_clock = tr::slot_of(14492, 36124, 36124, SENT, 3, until, 2, 0, SENT);
    ROW("an unread clock does not drop a set slot", unread_clock.active);
    auto none_code = tr::slot_of(14492, 36124, 36124, SENT, 3, until, 3, today, SENT);
    ROW("result 3 is no truce", !none_code.active);
    auto stranger = tr::slot_of(14492, 36124, 77777, SENT, 3, until, 2, today, SENT);
    ROW("a relation without the character is not matched", !stranger.matched && !stranger.active);
    auto high_bits = tr::slot_of(14492u | 0x01000000u, 36124u | 0x02000000u, 36124, SENT, 3, until, 2, today, SENT);
    ROW("ids compare on 24 bits", high_bits.active);
    std::printf("%d/%d rows\n", g_rows - g_fail, g_rows);
    return g_fail ? 1 : 0;
}
