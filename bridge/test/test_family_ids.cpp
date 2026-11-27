// test_family_ids.cpp — a missing parent is no parent (family_ids.h).
//
// Row 1 is the recorded case (a Carinthia test run, inspect character 31010 and 33202 at 1066.09.15): Berthold
// 30331 father 28063, Markward father 25223, Kuno father 28100, no mother read for any (raw 0xFFFFFFFF).
// Row 2 is the control: the old mask makes the three "siblings".
#include <cstdio>
#include "family_ids.h"

namespace fi = bridge::family_ids;

static int g_fail = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

int main() {
    const uint32_t none = 0xFFFFFFFFu;
    const uint32_t bf = fi::parent_id(28063), bm = fi::parent_id(none);
    ROW("1 a missing parent reads 0", bm == 0 && fi::parent_id(0) == 0);
    ROW("1 a present parent keeps its id", bf == 28063);
    ROW("1 Markward is not Berthold's sibling",
        !fi::siblings(bf, bm, fi::parent_id(25223), fi::parent_id(none)));
    ROW("1 Kuno is not Berthold's sibling", !fi::siblings(bf, bm, fi::parent_id(28100), fi::parent_id(none)));
    // 2 control: the old read (raw & 0xFFFFFF) on the same slots calls both siblings
    const uint32_t old_m = none & 0xFFFFFFu;
    ROW("2 control: the old mask makes a missing mother a shared one", old_m != 0 && old_m == (none & 0xFFFFFFu));
    // 3 real siblings: same father; half siblings by the mother; the high byte of a slot is not the id
    ROW("3 same father", fi::siblings(fi::parent_id(28063), 0, fi::parent_id(28063), 0));
    ROW("3 same mother only", fi::siblings(fi::parent_id(1), fi::parent_id(77), fi::parent_id(2), fi::parent_id(77)));
    ROW("3 a slot's high byte is masked", fi::parent_id(0x05000000u | 28063u) == 28063);
    ROW("3 no parents read: never siblings", !fi::siblings(0, 0, 0, 0));
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
