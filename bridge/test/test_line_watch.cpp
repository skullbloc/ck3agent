// test_line_watch.cpp — the played line followed per tick (METERLINE;
// bridge/include/line_watch.h). Engine-free: titles and holders are a map.
//
// The case the watch exists for (witness follow-up 2): the king's primary is
// k_france; he gains e_hre and dies between two 30-day meter samples. A meter
// re-deriving the heir from its last sample would follow k_france's heir (son
// A); the watch, ticking daily with the handoff's own decision, follows the
// holder of e_hre (the elected emperor).
#include "line_watch.h"

#include <cstdio>
#include <map>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } } while (0)

namespace {
constexpr uint32_t K = 12015, SON_A = 14445, CADET = 15319, EMPEROR = 16000, HEIR = 14445;
constexpr uint32_t K_FRANCE = 7052, E_HRE = 105;

struct World {
    std::map<uint32_t, uint32_t> holder;   // title -> raw holder
    uint32_t fallback = 0;
    int fallback_calls = 0;
    int dead_titles = -1;                  // the dead man's titles NOW (-1: unreadable)
    bool tick(played_line::Watch& w, bool alive, uint32_t primary_now, const char* date) {
        return played_line::tick(
            w, alive, primary_now,
            [this](uint32_t t) { auto it = holder.find(t); return it == holder.end() ? 0u : it->second; },
            [this](uint32_t) { fallback_calls++; return fallback; },
            [this](uint32_t) { return dead_titles; }, date);
    }
};

played_line::Watch armed(uint32_t start) {
    played_line::Watch w;
    w.start_id = w.line_id = start;
    w.armed_date = w.since_date = "0867.01.03";
    return w;
}
}  // namespace

int main() {
    using played_line::WatchStatus;
    using played_line::PendingReason;

    {   // the k_france -> e_hre -> death window
        World wd;
        played_line::Watch w = armed(K);
        wd.holder = {{K_FRANCE, K}};
        wd.tick(w, true, K_FRANCE, "0872.11.08");            // the meter's sample: primary k_france
        CHECK(w.primary_title_id == K_FRANCE, "alive: primary refreshed");
        wd.holder[E_HRE] = K;
        wd.tick(w, true, E_HRE, "0872.11.20");               // gains e_hre (no meter sample)
        CHECK(w.primary_title_id == E_HRE, "the per-tick refresh sees the gained empire");
        wd.holder[K_FRANCE] = SON_A;                          // partition: k_france to son A
        wd.holder[E_HRE] = EMPEROR;                           // elective: e_hre to the emperor
        const bool changed = wd.tick(w, false, 0, "0872.11.30");
        CHECK(changed, "a death that resolves changes the watch");
        CHECK(w.line_id == EMPEROR, "the line follows the holder of the primary held AT DEATH (e_hre)");
        CHECK(w.line_id != SON_A, "not the heir of the 30-day-old primary (k_france)");
        CHECK(w.previous_id == K && w.carrying_title_id == E_HRE && w.successions == 1, "the pass is recorded");
        CHECK(w.since_date == "0872.11.30" && w.status == WatchStatus::Following, "dated, following");
        CHECK(wd.fallback_calls == 0, "the fallback is not consulted when the title names an heir");
    }
    {   // partition: the primary heir's realm; a cadet's county does not carry the line
        World wd;
        played_line::Watch w = armed(K);
        wd.holder = {{E_HRE, K}};
        wd.tick(w, true, E_HRE, "0870.01.01");
        wd.holder[E_HRE] = HEIR;
        wd.holder[7157] = CADET;
        wd.tick(w, false, 0, "0870.02.01");
        CHECK(w.line_id == HEIR && w.carrying_title_id == E_HRE, "partition: the primary title's heir");
        wd.tick(w, true, E_HRE, "0870.02.02");
        CHECK(w.status == WatchStatus::Following && w.primary_title_id == E_HRE, "the heir is followed on");
    }
    {   // the bridge's own retry: still the dead man, then unheld, then an heir
        World wd;
        played_line::Watch w = armed(K);
        wd.holder = {{E_HRE, K}};
        wd.tick(w, true, E_HRE, "0870.01.01");
        wd.tick(w, false, 0, "0870.01.02");
        CHECK(w.status == WatchStatus::SuccessorPending && w.reason == PendingReason::HolderIsTheDead,
              "the title still names the dead: pending");
        CHECK(w.line_id == K, "pending keeps the dead character");
        wd.holder[E_HRE] = 0xFFFFFFFFu;
        wd.tick(w, false, 0, "0870.01.03");
        CHECK(w.reason == PendingReason::PrimaryTitleUnheld, "an unheld title: pending, never ended");
        wd.holder[E_HRE] = 0x1000000u | HEIR;                  // a masked holder
        wd.tick(w, false, 0, "0870.03.01");
        CHECK(w.line_id == HEIR && w.status == WatchStatus::Following, "retried until an heir resolves");
        CHECK(w.since_date == "0870.03.01", "dated at the tick it resolved");
    }
    {   // THE END (owner ruling 2026-11-12): only on three agreeing readings
        World wd;
        played_line::Watch w = armed(K);
        wd.holder = {{K_FRANCE, K}};
        wd.tick(w, true, K_FRANCE, "0870.01.01");
        wd.holder[K_FRANCE] = 0xFFFFFFFFu;                    // the title looked up: nobody holds it
        wd.dead_titles = -1;                                  // his own title state unreadable
        wd.tick(w, false, 0, "0870.01.02");
        CHECK(w.status == WatchStatus::SuccessorPending, "unreadable title state: pending, not ended");
        wd.dead_titles = 2;                                   // he still holds titles: transfer in flight
        wd.tick(w, false, 0, "0870.01.03");
        CHECK(w.status == WatchStatus::SuccessorPending, "titles still on the dead: pending");
        wd.dead_titles = 0;                                   // his titles have left him, to nobody
        const bool changed = wd.tick(w, false, 0, "0870.01.04");
        CHECK(changed && w.status == WatchStatus::LineEnded, "unheld + no successor + holds nothing: ended");
        CHECK(w.ended_date == "0870.01.04" && w.line_id == K, "dated; the line's last character kept");
        CHECK(std::string(played_line::status_name(w.status)) == "line_ended", "the emitted spelling");
        wd.holder[K_FRANCE] = CADET;                          // somebody later takes the title
        CHECK(!wd.tick(w, false, 0, "0871.01.01") && w.status == WatchStatus::LineEnded && w.line_id == K,
              "terminal: a later holder is not this line's heir");
    }
    {   // never ended without each reading
        World wd;
        played_line::Watch w = armed(K);
        wd.holder = {{K_FRANCE, K}};
        wd.tick(w, true, K_FRANCE, "0870.01.01");
        wd.dead_titles = 0;
        wd.holder[K_FRANCE] = 0;                              // the title did not look up (0, not -1)
        wd.tick(w, false, 0, "0870.01.02");
        CHECK(w.status == WatchStatus::SuccessorPending, "a title that did not look up: pending");
        wd.holder[K_FRANCE] = K;                              // still names the dead
        wd.tick(w, false, 0, "0870.01.03");
        CHECK(w.status == WatchStatus::SuccessorPending && w.reason == PendingReason::HolderIsTheDead,
              "the title still names him: pending");
        played_line::Watch w2 = armed(K);                     // died before any primary was read
        wd.tick(w2, false, 0, "0870.01.03");
        CHECK(w2.status == WatchStatus::SuccessorPending && w2.reason == PendingReason::NoPrimaryTitle,
              "no primary read: pending, never ended");
        wd.holder[K_FRANCE] = 0xFFFFFFFFu;
        wd.fallback = CADET;                                  // the legacy scan names somebody
        played_line::Watch w3 = armed(K);
        wd.tick(w3, true, K_FRANCE, "0870.01.01");
        wd.tick(w3, false, 0, "0870.01.04");
        CHECK(w3.status == WatchStatus::Following && w3.line_id == CADET, "a successor: never ended");
        CHECK(!played_line::line_ended(K_FRANCE, 0xFFFFFFFFu, true, 0), "the decision: a successor");
        CHECK(played_line::line_ended(K_FRANCE, 0x00FFFFFFu, false, 0), "the masked sentinel is none too");
        CHECK(!played_line::line_ended(0, 0xFFFFFFFFu, false, 0), "the decision: no cached primary");
    }
    {   // the legacy fallback, as the handoff applies it
        World wd;
        played_line::Watch w = armed(K);
        wd.holder = {{K_FRANCE, K}};
        wd.tick(w, true, K_FRANCE, "0870.01.01");
        wd.holder[K_FRANCE] = 0;
        wd.fallback = CADET;
        wd.tick(w, false, 0, "0870.01.02");
        CHECK(wd.fallback_calls == 1 && w.line_id == CADET, "no heir on the title: the fallback's successor");
        CHECK(w.carrying_title_id == 0 && w.primary_title_id == 0, "a fallback pass names no carrying title");
    }
    {   // died before any primary was read
        World wd;
        played_line::Watch w = armed(K);
        wd.tick(w, false, 0, "0867.01.04");
        CHECK(w.status == WatchStatus::SuccessorPending && w.reason == PendingReason::NoPrimaryTitle,
              "no primary read: pending, named");
        CHECK(wd.fallback_calls == 1, "the fallback is still asked, as the handoff asks it");
    }
    {   // successor(): the one decision both callers use
        int calls = 0;
        CHECK(played_line::successor(14445, 12015, [&] { calls++; return 9u; }) == 14445 && calls == 0,
              "the cached title's holder wins; no fallback");
        CHECK(played_line::successor(12015, 12015, [&] { calls++; return 9u; }) == 9 && calls == 1,
              "the dead man still holding it: the fallback");
        CHECK(played_line::successor(0, 12015, [&] { calls++; return 0u; }) == 0 && calls == 2,
              "nothing: 0 (retry)");
    }
    {   // the registry
        played_line::WatchRegistry reg;
        played_line::Watch out;
        bool now = false;
        CHECK(reg.arm(K, "0867.01.03", &out, &now) && now && out.line_id == K, "arm");
        CHECK(reg.arm(K, "0867.02.03", &out, &now) && !now && out.armed_date == "0867.01.03",
              "re-arming is a read: the first arm's date stands");
        out.line_id = HEIR;
        reg.store(out);
        CHECK(reg.arm(K, "x", &out, &now) && out.line_id == HEIR, "a ticked copy is stored back");
        for (uint32_t i = 1; i < played_line::kMaxWatches; ++i) reg.arm(100 + i, "d", nullptr, nullptr);
        CHECK(!reg.arm(999, "d", &out, &now), "bounded: a full registry refuses a new watch");
        reg.clear();
        reg.store(out);
        CHECK(reg.size() == 0, "a store after a world reset does not resurrect a watch");
        CHECK(played_line::watches().size() == played_line::watches().size(), "the global registry exists");
    }

    std::printf("test_line_watch: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
