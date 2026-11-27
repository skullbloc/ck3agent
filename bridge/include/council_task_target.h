// council_task_target.h — the two decisions a county council task needs from memory
// (re/findings/re_fabricate_claim.md). Header-only, no game reads: the
// handlers read the fields and these functions decide, so the decisions are tested
// without a game (bridge/test/test_council_task_target.cpp).
//
// 1. The TARGET of a county task (task_fabricate_claim, task_conversion, ...) is a
//    PROVINCE, not a title. BUILD_TARGET_SCOPE({8, id}) returns the province scope
//    cache entry `id` (*(*(GAME_STATE_ROOT)+0xA0)+0x118, array +0x28, count +0x34:
//    FUN_06061760 + FUN_0632cc20, decompile); the task's scope builder FUN_05cb7700
//    reads the county as title(province+0x318).DE_JURE_PARENT. The save reader
//    FUN_05ccaec0 turns the legacy `province=<id>` token into the same tag-8 target.
//    The 1097 dump: the stored target of 694/694 develop_county
//    tasks is a province of a county the ruler holds when read as a province, 0/694
//    when read as a title. A county title id passed as that id names whatever county
//    holds the province with the same number.
//    county_province_verified() is the round trip the bridge requires before it
//    hands a province id to the game for a county: the province is the cache's entry
//    for its id, its barony is the barony it came from, and that barony's de jure
//    parent is the county asked for.
//
// 2. The PROGRESS of a percentage task lives on the CActiveCouncilTask at +0x20
//    (int64, fixed point x100000; 10,000,000 = 100 %). The daily step FUN_05cc8880
//    adds the task's `progress` script value / 30 and clamps to [0, 10,000,000]; the
//    manager FUN_05cc7c40 finishes the task at >= 9,999,999. Only task types with
//    progress kind 1 (task_progress_percentage, CCouncilTaskType+0x44) carry it.
#pragma once
#include <cstdint>

namespace bridge::council_task_target {

// The values the round trip compares, each read by the caller.
struct ProvinceRoundTrip {
    uint32_t county_id = 0;          // the county title asked for (low 24 bits used)
    uint32_t barony_id = 0;          // the barony the province was reached from
    uintptr_t province = 0;          // barony + title_obj::PROVINCE_PTR
    uint32_t province_id = 0;        // province + province::ID
    int32_t cache_count = 0;         // province cache count (+0x34)
    uintptr_t cache_entry = 0;       // province cache array[province_id] (0 when out of range)
    uint32_t province_barony = 0;    // province + province::BARONY_TITLE_ID
    uint32_t barony_parent = 0;      // barony + title_obj::DE_JURE_PARENT
};

// True only when every link agrees; a 0 / out-of-range anywhere is false.
inline bool county_province_verified(const ProvinceRoundTrip& t) {
    constexpr uint32_t M = 0xFFFFFF;
    if (!t.province || !t.province_id) return false;
    if (t.cache_count <= 0 || t.province_id >= static_cast<uint32_t>(t.cache_count)) return false;
    if (t.cache_entry != t.province) return false;
    if ((t.barony_id & M) == 0 || (t.barony_id & M) == M) return false;
    if ((t.province_barony & M) != (t.barony_id & M)) return false;
    if ((t.county_id & M) == 0 || (t.county_id & M) == M) return false;
    return (t.barony_parent & M) == (t.county_id & M);
}

// The progress word's full scale (FUN_05cc8880 clamp, FUN_05cc7c40 finish test).
constexpr int64_t kProgressFull = 10000000;
// CCouncilTaskType+0x44: 0 infinite, 1 percentage, 2 value (dump 1097: every
// task_progress_percentage task of 00_*_tasks.txt reads 1).
constexpr int32_t kProgressPercentage = 1;

// The task's progress in percent, or false when the type has none or the word is
// outside the range the game keeps it in (a misread is refused, never clamped).
inline bool progress_percent(int32_t progress_kind, int64_t raw, double* pct) {
    if (progress_kind != kProgressPercentage) return false;
    if (raw < 0 || raw > kProgressFull) return false;
    if (pct) *pct = static_cast<double>(raw) / 100000.0;
    return true;
}

}  // namespace bridge::council_task_target
