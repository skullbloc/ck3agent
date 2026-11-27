// test_armies_around.cpp — the engine-free part of the armies-around reads
// Per armies_around.h: the county id list a caller sends, which units are rows, their order. NO GAME.
// Built and run by ctest; not proven live.
#include <cstdio>
#include <string>
#include <vector>
#include "armies_around.h"

namespace aa = bridge::armies_around;

static int g_fail = 0;
#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (!(cond)) { std::printf("FAIL: %s\n", what); ++g_fail; }            \
    } while (0)

int main() {
    std::vector<uint32_t> ids;
    CHECK(aa::parse_ids("123,456", &ids) && ids.size() == 2 && ids[0] == 123 && ids[1] == 456, "two ids");
    CHECK(aa::parse_ids("7", &ids) && ids.size() == 1 && ids[0] == 7, "one id");
    ids = {9};
    CHECK(!aa::parse_ids("", &ids) && ids.size() == 1 && ids[0] == 9, "empty refused, out untouched");
    CHECK(!aa::parse_ids("1,,2", &ids), "an empty item refused");
    CHECK(!aa::parse_ids("1,", &ids), "a trailing comma refused");
    CHECK(!aa::parse_ids("-1", &ids), "a sign refused");
    CHECK(!aa::parse_ids("1 ,2", &ids), "a space refused");
    CHECK(!aa::parse_ids("0", &ids), "zero refused");
    CHECK(!aa::parse_ids("16777216", &ids), "over 24 bits refused");
    CHECK(aa::parse_ids("16777215", &ids), "24 bits taken");
    std::string many;
    for (int i = 1; i <= aa::kCountiesMax; ++i) many += (i > 1 ? "," : "") + std::to_string(i);
    CHECK(aa::parse_ids(many, &ids) && static_cast<int>(ids.size()) == aa::kCountiesMax, "the cap taken");
    CHECK(!aa::parse_ids(many + ",99", &ids), "one over the cap refused");

    CHECK(aa::listed({5, 0x1234}, 0x7F001234u), "a county id compared on its 24 bits");
    CHECK(!aa::listed({5}, 6), "an unlisted county");

    CHECK(aa::foreign(0x02000010u, 0x11u), "another ruler's unit is a row");
    CHECK(!aa::foreign(0x7F000011u, 0x11u), "the army's own owner (24-bit match) is not");
    CHECK(!aa::foreign(0, 0x11u) && !aa::foreign(0xFFFFFFFFu, 0x11u), "an unread owner is not a row");

    std::vector<aa::Row> rows = {{3, 1, 100, 9}, {1, 2, -1, 9}, {2, 3, 900, 9}, {4, 4, 100, 9}};
    aa::order(&rows);
    CHECK(rows[0].army_id == 2 && rows[1].army_id == 3 && rows[2].army_id == 4 && rows[3].army_id == 1,
          "largest first, ties by army id, an unread size last");

    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("test_armies_around: all passed\n");
    return 0;
}
