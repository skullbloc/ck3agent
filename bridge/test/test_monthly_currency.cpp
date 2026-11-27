// test_monthly_currency.cpp — the monthly prestige/piety change from the game's
// cache (bridge/include/monthly_currency.h), engine-free.
//
// Rows 1-5 use raw int64 values read from core_data+0x340/+0x348/+0x350/+0x358 of five rulers in the
// 1097 core dump (BuildID e61ff264). Rows 6-7 pin the game's piety-tail quirk (the piety tail is added only when the
// prestige tail is non-zero; objdump 0x665ea30).
#include <cstdio>
#include "monthly_currency.h"

namespace mc = bridge::monthly_currency;

static int g_fail = 0, g_rows = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        ++g_rows;                                                           \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

int main() {
    // 1097 dump, char 73081: [492934, 0, 235000, 0] -> +4.92934 prestige, +2.35 piety a month
    constexpr mc::Change a = mc::change({492934, 0, 235000, 0});
    ROW("1 73081 prestige head only", a.prestige == 492934 && a.piety == 235000);
    // char 38161: [327808, 0, 336000, 0]
    constexpr mc::Change b = mc::change({327808, 0, 336000, 0});
    ROW("2 38161", b.prestige == 327808 && b.piety == 336000);
    // char 35671: [227160, 0, 215250, 0]
    constexpr mc::Change c = mc::change({227160, 0, 215250, 0});
    ROW("3 35671", c.prestige == 227160 && c.piety == 215250);
    // char 34351: a ruler with a prestige tail: [13914, 37800, 451000, 0] -> 0.51714
    constexpr mc::Change d = mc::change({13914, 37800, 451000, 0});
    ROW("4 34351 head + tail", d.prestige == 51714 && d.piety == 451000);
    // char 16785181: [107773, 13860, 70000, 0]
    constexpr mc::Change e = mc::change({107773, 13860, 70000, 0});
    ROW("5 16785181 head + tail", e.prestige == 121633 && e.piety == 70000);
    // the quirk: a piety tail with no prestige tail is NOT added by the game
    constexpr mc::Change f = mc::change({0, 0, 100000, 50000});
    ROW("6 piety tail ignored when prestige tail is zero", f.piety == 100000 && f.prestige == 0);
    constexpr mc::Change g = mc::change({0, -1, 100000, 50000});
    ROW("7 piety tail added when prestige tail is non-zero", g.piety == 150000 && g.prestige == -1);
    // negative heads pass through signed
    constexpr mc::Change h = mc::change({-250000, 0, -15000, 0});
    ROW("8 negative change", h.prestige == -250000 && h.piety == -15000);

    std::printf("%d rows, %d failed\n", g_rows, g_fail);
    return g_fail ? 1 : 0;
}
