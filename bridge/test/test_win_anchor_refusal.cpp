// win_anchor_refusal.h — the typed `not_on_windows` refusal for a write whose
// engine entry has no Windows anchor. Built twice from this one source:
//   Linux  (re/offsets/ck3_offsets.h): no constant reads unmapped, so NO row refuses;
//   Windows (the generated win_offsets/ck3_offsets.h, under wine): a row refuses exactly
//          when its constant is the generator's poison, and the reply names that row.
// The value test itself (rva_unmapped) is checked on both against the generator's own
// encoding (CK3_WIN_UNMAPPED_N, CK3_WIN_RECIPE) spelled out here, so a change to the
// encoding on one side reds.
#include "win_anchor_refusal.h"

#include <cstdio>
#include <initializer_list>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    namespace wa = bridge::win_anchor;
    // the generator's encodings (gen_win_offsets.py: UNMAPPED | i << 40; recipe 0x7FE... | i)
    for (unsigned long long i : {0ull, 1ull, 77ull, 1152ull, 4095ull}) {
        CHECK(wa::rva_unmapped(static_cast<uintptr_t>(0x7FF0000000000000ull | (i << 40))));
        CHECK(!wa::rva_unmapped(static_cast<uintptr_t>(0x7FE0000000000000ull | i)));   // a recipe is callable
    }
    CHECK(!wa::rva_unmapped(0x0bee39f8));   // a real Linux RVA
    CHECK(!wa::rva_unmapped(0x00df5340));   // a real Windows RVA (CMD_EFFECT)
    CHECK(wa::missing_for(nullptr) == nullptr);
    CHECK(wa::missing_for("query_character") == nullptr);   // not a listed write
    CHECK(wa::missing_for("script_action") == nullptr);     // its entry (CMD_EFFECT) is anchored on both

    int refused = 0;
    for (const auto& n : wa::kNeeds) {
        const auto* m = wa::missing_for(n.action);
        if (wa::rva_unmapped(n.rva)) {
            CHECK(m != nullptr);
            if (m) CHECK(wa::rva_unmapped(m->rva) && std::strcmp(m->action, n.action) == 0);
        }
        if (m) ++refused;
#if defined(_WIN32)
        std::printf("%-22s %-36s %s\n", n.action, n.anchor, wa::rva_unmapped(n.rva) ? "not_on_windows" : "anchored");
#else
        CHECK(m == nullptr);                 // Linux: nothing is ever refused by this table
#endif
    }
    std::printf("%d rows, %d refused on this build\n", static_cast<int>(sizeof(wa::kNeeds) / sizeof(wa::kNeeds[0])),
                refused);

    // set_ai_veto kinds (legacy, faction) by their command vtables
    CHECK(wa::veto_kind_missing(nullptr) == nullptr);
    CHECK(wa::veto_kind_missing("war") == nullptr);    // interaction-matched: not in the table
    CHECK(wa::veto_kind_missing("law") == nullptr);
    int kinds_refused = 0;
    for (const auto& n : wa::kVetoKindNeeds) {
        const auto* m = wa::veto_kind_missing(n.what);
        if (wa::rva_unmapped(n.rva)) CHECK(m != nullptr && wa::rva_unmapped(m->rva));
        if (wa::rva_unmapped(n.rva)) ++kinds_refused;
#if defined(_WIN32)
        std::printf("veto kind %-8s %-42s %s\n", n.what, n.anchor, wa::rva_unmapped(n.rva) ? "not_on_windows" : "anchored");
#else
        CHECK(m == nullptr);                 // Linux: no veto kind is ever refused by this table
#endif
    }
    std::printf("%d veto-kind rows, %d unmapped on this build\n",
                static_cast<int>(sizeof(wa::kVetoKindNeeds) / sizeof(wa::kVetoKindNeeds[0])), kinds_refused);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
