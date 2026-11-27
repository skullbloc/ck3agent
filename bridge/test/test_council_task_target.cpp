// test_council_task_target.cpp — the county task's target province and its progress
// word (bridge/include/council_task_target.h; re/findings/re_fabricate_claim.md).
//
// THE HAZARD. do_query_fabricate_targets and do_set_council_task built the target as
// BUILD_TARGET_SCOPE({8, <county TITLE id>}); type 8 is a PROVINCE, so the game judged
// (and set) the county that holds the province numbered like the title. The bridge now
// hands the game a province only after county_province_verified() agrees.
//
// Rows from the 1097 dump where noted; the province cache there holds
// 13,270 entries (the 867 dump the same count).
#include "council_task_target.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } } while (0)

using namespace bridge::council_task_target;

static ProvinceRoundTrip good() {
    ProvinceRoundTrip t;
    // c_leinster (title 508): capital barony b_wexford 509, province 37 (both dumps)
    t.county_id = 0x01000000u | 508; // generation bits set: the low 24 bits are compared
    t.barony_id = 509;
    t.province = 0x7f00'0000'1000;
    t.province_id = 37;
    t.cache_count = 13270;
    t.cache_entry = 0x7f00'0000'1000;
    t.province_barony = 509;
    t.barony_parent = 508;
    return t;
}

int main() {
    CHECK(county_province_verified(good()), "every link agrees: the province is the county's");

    { auto t = good(); t.cache_entry = 0x7f00'0000'2000;
      CHECK(!county_province_verified(t), "the cache's entry for the id is another province"); }
    { auto t = good(); t.province_id = 13270;
      CHECK(!county_province_verified(t), "a province id at the cache count is out of range"); }
    { auto t = good(); t.province_id = 0;
      CHECK(!county_province_verified(t), "province id 0 is the cache's no-target slot"); }
    { auto t = good(); t.province = 0; t.cache_entry = 0;
      CHECK(!county_province_verified(t), "a barony with no province"); }
    { auto t = good(); t.province_barony = 510;
      CHECK(!county_province_verified(t), "the province names another barony"); }
    { auto t = good(); t.barony_parent = 507;
      CHECK(!county_province_verified(t), "the barony's de jure parent is another county"); }
    { auto t = good(); t.barony_id = 0xFFFFFF; t.province_barony = 0xFFFFFF;
      CHECK(!county_province_verified(t), "the none barony id never verifies"); }
    { auto t = good(); t.cache_count = 0;
      CHECK(!county_province_verified(t), "an unread cache count verifies nothing"); }

    // THE HAZARD'S SHAPE: the title id itself used as the province id. Both dumps:
    // province 508 is b_dorostorum (barony 1607) of c_dorostorum (1605), not c_leinster;
    // province 469 (title 469 = c_athlone) is c_duklja's. The round trip the old call
    // never made refuses it.
    { auto t = good(); t.province_id = 508; t.barony_id = 1607; t.province_barony = 1607; t.barony_parent = 1605;
      CHECK(!county_province_verified(t), "a title id read as a province names another county (the old call made no such check and used it)"); }

    double pct = -1;
    // dump 1097: task_fabricate_claim +0x20 values 5,200,000 / 5,408,000 / 8,100,000
    CHECK(progress_percent(1, 5200000, &pct) && pct == 52.0, "5,200,000 is 52 %");
    CHECK(progress_percent(1, 5408000, &pct) && pct == 54.08, "5,408,000 is 54.08 %");
    CHECK(progress_percent(1, 0, &pct) && pct == 0.0, "a task just set reads 0 %");
    CHECK(progress_percent(1, kProgressFull, &pct) && pct == 100.0, "the clamp's top is 100 %");
    CHECK(!progress_percent(1, kProgressFull + 1, &pct), "above the clamp is a misread, refused");
    CHECK(!progress_percent(1, -1, &pct), "below 0 is a misread, refused");
    CHECK(!progress_percent(0, 5200000, &pct), "an infinite task (kind 0) has no percentage");
    CHECK(!progress_percent(2, 5200000, &pct), "a value task (kind 2) keeps no percentage there");

    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("test_council_task_target: all checks passed\n");
    return 0;
}
