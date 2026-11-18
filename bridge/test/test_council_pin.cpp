// test_council_pin.cpp —: the per-seat council-task pin
// (bridge/include/council_pin.h, bridge/src/council_suppress_hook.cpp).
//
// Fixture: the 867 dump ck3_20260625_152901_ivar867 (BuildID e61ff264...), read
// 2026-11-14 with re/coredump/core_reader.py:
//   character 14859: CCharacter 0x7f8f0df16210, u32 at +0x18 = 0x3a0b (14859);
//     its CAIStrategy 0x7f8f2fe041e0 (*(*(char+0x1a8)+0x278)) holds at +0x18
//     the CCharacter POINTER 0x7f8f0df16210, whose low 24 bits are 15819280.
//   its council positions 0x101c (task_foreign_affairs) and 0x101e
//     (task_organize_levies): u32 id at +0x10, owner 14859 at +0x3c.
// The per-seat selector FUN_04dc9fc0 receives the CCharacter and the position;
// the pre-COUNCILRE hook sat one level up on FUN_04dca2a0, whose argument is the
// CAIStrategy, and read the id at its +0x18.
//
// Built twice. Plain: every row must pass. -DCOUNCIL_PIN_DISPATCHER_ARG_CONTROL:
// the pre-fix read (the id at the CAIStrategy's +0x18); it exits 0 only when
// EXACTLY rows 1 and 7a fail and every other row passes (not WILL_FAIL, which
// any nonzero exit would satisfy): the lock never matched a ruler, and nothing
// else about the pin depends on that read.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "council_pin.h"
#include "ck3_offsets.h"

namespace cp = bridge::council_pin;

#include <set>
#include <string>

static int g_checks = 0, g_fails = 0;
static std::set<std::string> g_failed_rows;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("FAIL: %s\n", what);
        const char* colon = std::strchr(what, ':');
        g_failed_rows.insert(std::string(what, colon ? colon - what : std::strlen(what)));
    }
}

template <typename T> static void put(unsigned char* b, size_t off, T v) { std::memcpy(b + off, &v, sizeof v); }

int main() {
    // ---- fixtures (values from the dump; the buffers are host memory) ----
    static unsigned char ruler[0x200] = {};
    put<uint32_t>(ruler, 0x18, 0x3a0b);                     // CCharacter+0x18 = id 14859
    static unsigned char ai_strategy[0x40] = {};
    put<uint64_t>(ai_strategy, 0x18, 0x7f8f0df16210ull);    // CAIStrategy+0x18 = the CCharacter*
    static unsigned char pos_a[0x48] = {}, pos_b[0x48] = {};
    put<uint32_t>(pos_a, 0x10, 0x101c); put<uint32_t>(pos_a, 0x3c, 14859);
    put<uint32_t>(pos_b, 0x10, 0x101e); put<uint32_t>(pos_b, 0x3c, 14859);

#ifdef COUNCIL_PIN_DISPATCHER_ARG_CONTROL
    // The pre-fix read: council_suppress_hook's on_ai_council_tasks took
    // *(u32*)(arg + character::ID) & 0xFFFFFF with arg = the CAIStrategy.
    uint32_t v = 0; std::memcpy(&v, ai_strategy + ck3::character::ID, sizeof v);
    const uint32_t ruler_id = v & 0xFFFFFF;
#else
    const uint32_t ruler_id = cp::ruler_id_at(ruler);
#endif

    // 1. The ruler id the hook keys on is the character's id.
    check(ruler_id == 14859, "1: the hook reads ruler id 14859");
    // 2. The position id is read where FUN_04dca2a0 matches it.
    check(cp::position_id_at(pos_a) == 0x101c && cp::position_id_at(pos_b) == 0x101e,
          "2: position ids 0x101c / 0x101e read at +0x10");
    // 3. The mirrors of ck3_offsets.h.
    check(cp::kRulerIdOffset == ck3::character::ID, "3a: kRulerIdOffset == character::ID");
    check(cp::kPositionIdOffset == ck3::globals::COUNCIL_POS_KEY, "3b: kPositionIdOffset == COUNCIL_POS_KEY");
    check(ck3::funcs::AI_COUNCIL_TASK_SELECT_FOR_POSITION == 0x04cc9fc0, "3c: hook target is FUN_04dc9fc0 (header offset)");

    // 4. The pin set: one seat of one ruler.
    {
        cp::PinSet s;
        check(s.empty(), "4a: starts empty (no pins = native everywhere)");
        check(s.add(14859, 0x101c), "4b: add (14859, 0x101c)");
        check(s.has(14859, 0x101c), "4c: that seat is pinned");
        check(!s.has(14859, 0x101e), "4d: the ruler's other seat is NOT pinned");
        check(!s.has(14860, 0x101c), "4e: another ruler with the same position id is NOT pinned");
        check(s.has(14859 | 0x01000000u, 0x101c), "4f: the ruler id is compared on its 24 bits");
        check(!s.add(0, 0x101c) && !s.add(0xFFFFFF, 0x101c) && !s.add(14859, 0) &&
              !s.add(14859, 0xFFFFFFFFu), "4g: null / sentinel ids are refused");
        check(s.add(14859, 0x101e) && s.add(9999, 0x2000) && s.size() == 3, "4h: three pins");
        check(s.remove_ruler(14859) == 2 && s.size() == 1 && s.has(9999, 0x2000),
              "4i: releasing a ruler drops only its pins");
    }

    // 5. The pass verdict: only pinned + readable + seated + alive skips.
    check(cp::decide(false, true, true, true, false) == cp::Pass::kNativeNoPins, "5a: no pins -> native");
    check(cp::decide(true, false, true, true, false) == cp::Pass::kNativeBadArgs, "5b: unreadable -> native (fail open)");
    check(cp::decide(true, true, false, true, false) == cp::Pass::kNativeNotPinned, "5c: not pinned -> native");
    check(cp::decide(true, true, true, false, false) == cp::Pass::kNativeNotSeated, "5d: not seated -> native");
    check(cp::decide(true, true, true, true, true) == cp::Pass::kNativeRulerDead, "5e: dead ruler -> native");
    check(cp::decide(true, true, true, false, true) == cp::Pass::kNativeRulerDead, "5f: dead + unseated -> native (released)");
    check(cp::decide(true, true, true, true, false) == cp::Pass::kSkipPinned, "5g: pinned seat of a seated ruler -> skip");
    {
        int skipping = 0;
        for (int m = 0; m < 32; ++m)
            skipping += cp::skips(cp::decide(m & 1, m & 2, m & 4, m & 8, m & 16)) ? 1 : 0;
        check(skipping == 1, "5h: exactly one of the 32 input rows skips the native pass");
    }

    // 6. The arm verdict and its reply words.
    check(cp::arm_verdict(false, true, true) == cp::Arm::kNoPosition, "6a: no position");
    check(cp::arm_verdict(true, false, true) == cp::Arm::kNotApplied, "6b: read-back differs -> not_applied");
    check(cp::arm_verdict(true, true, false) == cp::Arm::kNotSeated, "6c: not a controlled character -> not_seated (default off)");
    check(cp::arm_verdict(true, true, true) == cp::Arm::kPinned, "6d: pinned");
    check(!std::strcmp(cp::arm_name(cp::Arm::kPinned), "pinned") &&
          !std::strcmp(cp::arm_name(cp::Arm::kNotSeated), "not_seated") &&
          !std::strcmp(cp::arm_name(cp::Arm::kNotApplied), "not_applied") &&
          !std::strcmp(cp::arm_name(cp::Arm::kNoPosition), "no_position") &&
          !std::strcmp(cp::arm_name(cp::Arm::kRefused), "refused"), "6e: reply words");

    // 7. The hook's decision on the fixture, as on_council_select makes it: the
    //    seat 14859 pinned is held, his other seat runs native.
    {
        cp::PinSet s;
        s.add(14859, 0x101c);
        const bool seated = true, dead = false;
        const cp::Pass a = cp::decide(!s.empty(), true, s.has(ruler_id, cp::position_id_at(pos_a)), seated, dead);
        const cp::Pass b = cp::decide(!s.empty(), true, s.has(ruler_id, cp::position_id_at(pos_b)), seated, dead);
        check(cp::skips(a), "7a: the pinned seat 0x101c of 14859 is held");
        check(!cp::skips(b), "7b: 14859's seat 0x101e keeps its AI");
    }

    printf("%d checks, %d failed\n", g_checks, g_fails);
#ifdef COUNCIL_PIN_DISPATCHER_ARG_CONTROL
    const std::set<std::string> expected = {"1", "7a"};
    const bool as_expected = g_failed_rows == expected;
    printf("control: %s (the pre-fix read must fail exactly rows 1 and 7a)\n",
           as_expected ? "failed exactly the expected rows" : "DID NOT fail exactly rows 1 and 7a");
    return as_expected ? 0 : 1;
#else
    return g_fails ? 1 : 0;
#endif
}
