// monthly_currency.h — a character's monthly prestige and piety change, read from
// the game's own per-character cache (re/findings/re_monthly_prestige.md). Engine-free: the caller
// reads the four int64 fields of core_data (*(char+0x1a8)) and this file says what the game adds.
//
// The writer (FUN_06782190, per-character recompute job):
//   core+0x350 = MonthlyPiety(char, 0, &core+0x358)    - core+0x358
//   core+0x340 = MonthlyPrestige(char, 0, &core+0x348) - core+0x348
// The applier (the monthly pass FUN_068102a0):
//   FUN_0675eb30: prestige += core+0x340 (when non-zero); prestige += core+0x348 (when non-zero)
//   FUN_0675e9a0: piety    += core+0x350 (when non-zero); piety += core+0x358 WHEN core+0x348 != 0
// The last condition is the game's own test (objdump 0x665ea30: `cmpq $0x0,0x348(%rax)` before
// `push 0x358(%rax)`): the piety tail is added only when the PRESTIGE tail is non-zero. This file
// reproduces it rather than "fixing" it, because the number reported must be what the game adds.
//
// Not run live: the live checklist in a research note
#pragma once
#include <cstdint>

namespace bridge::monthly_currency {

struct Cache {             // the four raw int64 fixed-point (/100000) fields
    int64_t prestige_head; // core+0x340
    int64_t prestige_tail; // core+0x348
    int64_t piety_head;    // core+0x350
    int64_t piety_tail;    // core+0x358
};

struct Change {            // what the next monthly pass adds, fixed-point /100000
    int64_t prestige;
    int64_t piety;
};

constexpr Change change(const Cache& c) {
    return Change{c.prestige_head + c.prestige_tail,
                  c.piety_head + (c.prestige_tail != 0 ? c.piety_tail : 0)};
}

}  // namespace bridge::monthly_currency
