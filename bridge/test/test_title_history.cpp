// The title-history entry's date and type (title_history.h), rows from a seated game's 1096 save.
#include "title_history.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint32_t raw(int y, int m0, int d0) { return (uint32_t(y) << 16) | (uint32_t(m0) << 8) | uint32_t(d0); }

int main() {
    // 1: the raw fields as the save holds them (zero-based month/day) read as the war history's dates
    auto u = title_history::decode(raw(1075, 0, 11));
    CHECK(u.y == 1075 && u.m == 1 && u.d == 12);          // Ulster, war_history 1075.01.12
    auto o = title_history::decode(raw(1073, 8, 7));
    CHECK(o.y == 1073 && o.m == 9 && o.d == 8);           // Oriel 1073.09.08
    auto dub = title_history::decode(raw(1091, 7, 13));
    CHECK(dub.m == 8 && dub.d == 14);                     // Dublin, inside a seated game's step 1091.07.28-08.20
    // 2: a cutoff in reply dates packs the same way (the filter keeps the entry of that day)
    CHECK(title_history::encode(1075, 1, 12) == raw(1075, 0, 11));
    CHECK(title_history::encode(1091, 0, 0) == raw(1091, 0, 0));
    // 3: the types read beside their causes
    CHECK(std::strcmp(title_history::type_name(0), "conquest") == 0);
    CHECK(std::strcmp(title_history::type_name(2), "conquest_claim") == 0);
    CHECK(std::strcmp(title_history::type_name(4), "inheritance") == 0);
    CHECK(std::strcmp(title_history::type_name(10), "revoked") == 0);
    CHECK(std::strcmp(title_history::type_name(5), "unknown") == 0);
    std::printf("test_title_history: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
