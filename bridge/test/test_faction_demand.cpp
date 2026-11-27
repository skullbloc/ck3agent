// test_faction_demand.cpp — the faction-demand letters, their refuse option, and
// the name walk that reads a dynamic option name (bridge/include/faction_demand.h), engine-free.
//
// Keys are the shipped letters' option names in file order (game/events/factions/
// faction_demands.txt and tgp_faction_events, read on the test host's install). The memory rows are
// the 867 core dump's bytes for faction_demand.1001's fourth option (the 867 start dump:
// entry 0x7f8ff8ad42e0 -> composite 0x7f8ff8adc1f0 -> CSingleDesc 0x7f8ff8adc690).
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "faction_demand.h"

namespace fd = bridge::faction_demand;

static int g_fail = 0, g_rows = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        ++g_rows;                                                           \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

using K = std::vector<std::string>;

// A sparse fake of the game's memory: 8-byte words and strings at addresses.
struct Mem {
    std::map<uint64_t, uint64_t> w;
    std::map<uint64_t, std::string> s;
    bool u64(uint64_t a, uint64_t* o) const {
        auto it = w.find(a);
        if (it == w.end()) return false;
        *o = it->second;
        return true;
    }
    bool i32(uint64_t a, int32_t* o) const {
        // the low or high half of an 8-byte word
        auto it = w.find(a & ~7ull);
        if (it == w.end()) return false;
        uint64_t v = it->second;
        *o = static_cast<int32_t>((a & 4) ? (v >> 32) : (v & 0xFFFFFFFFu));
        return true;
    }
    bool str(uint64_t a, size_t n, std::string* o) const {
        auto it = s.find(a);
        if (it == s.end() || it->second.size() < n) return false;
        *o = it->second.substr(0, n);
        return true;
    }
};

static Mem populist_refuse_name() {
    Mem m;
    const uint64_t E = 0x7f8ff8ad42e0, A1 = 0x7f8ff8ad7490, FV = 0x7f8ff8adc1f0, A2 = 0x7f8ff8ace440;
    const uint64_t S0 = 0x7f8ff8adc220, S1 = 0x7f8ff8adc690;
    // the composite entry (vtable image 0xbf00ac8): +0x08 children, +0x10/+0x14 = 1/1,
    // +0x30 an image pointer with a word of 0x426f after it (no key string)
    m.w[E] = 0x561b55ad4ac8; m.w[E + 0x08] = A1; m.w[E + 0x10] = 0x0000000100000001ull;
    m.w[E + 0x30] = 0x561b55593680; m.w[E + 0x38] = 0x426f;
    m.w[A1] = FV;
    // first_valid (vtable image 0xbf00bb8): two alternatives; +0x30 an image pointer, length 0
    m.w[FV] = 0x561b55ad4bb8; m.w[FV + 0x08] = A2; m.w[FV + 0x10] = 0x0000000200000002ull;
    m.w[FV + 0x30] = 0x561b55ad4b28; m.w[FV + 0x38] = 0;
    m.w[A2] = S0; m.w[A2 + 8] = S1;
    // the two CSingleDesc alternatives: the triggered ADMIN text, then the fallback
    m.w[S0 + 0x30] = 0x7f8ff8aa0000; m.w[S0 + 0x38] = 36; m.s[0x7f8ff8aa0000] = "FACTION_DEMAND_POPULIST_REFUSE_ADMIN";
    m.w[S1 + 0x30] = 0x7f8ff8aa1000; m.w[S1 + 0x38] = 30; m.s[0x7f8ff8aa1000] = "FACTION_DEMAND_POPULIST_REFUSE";
    return m;
}

int main() {
    // 1. every shipped letter is a demand with one refuse option
    const std::vector<std::pair<const char*, K>> letters = {
        {"faction_demand.0001", {"FACTION_DEMAND_ACCEPT", "FACTION_DEMAND_BRIBE_CO_EMPEROR", "FACTION_DEMAND_REFUSE"}},
        {"faction_demand.1001", {"FACTION_DEMAND_POPULIST_ACCEPT_CONVERT", "FACTION_DEMAND_POPULIST_ACCEPT_CONVERT_STATE_FAITH",
                                 "FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE", "FACTION_DEMAND_POPULIST_REFUSE"}},
        {"faction_demand.1101", {"FACTION_DEMAND_PEASANT_ACCEPT", "FACTION_DEMAND_PEASANT_REFUSE"}},
        {"faction_demand.2011", {"FACTION_DEMAND_ACCEPT", "FACTION_DEMAND_REFUSE"}},
        {"faction_demand.9998", {"FACTION_DEMAND_NOMADIC_ACCEPT_GRANT_INDEPENDENCE", "FACTION_DEMAND_POPULIST_REFUSE"}},
    };
    for (const auto& [ev, keys] : letters) {
        auto o = fd::classify(keys);
        std::string n = std::string("1 ") + ev + " is a demand, refuse = its last option";
        ROW(n.c_str(), o.is_demand() && o.refuse == static_cast<int>(keys.size()) - 1);
    }
    {
        auto o = fd::classify(letters[1].second);
        ROW("1b the populist letter has three accept options", o.accept == std::vector<int>({0, 1, 2}));
    }
    // 2. not demands: notices, the counter-offer, the populist vassal letter, a claim letter
    const K notice = {"FACTION_DEMAND_ACCEPTED_OPT"};
    const K corule = {"FACTION_DEMAND_COUNTER_OFFER_CORULE_ACCEPT", "FACTION_DEMAND_COUNTER_OFFER_CORULE_REJECT"};
    const K vassal = {"", "FACTION_DEMAND_POPULIST_VASSAL_JOIN_WAR", "FACTION_DEMAND_POPULIST_VASSAL_TAKE_NO_SIDE"};
    const K claim = {"court_chaplain_task.0301.a", "court_chaplain_task.0301.b"};
    const K two = {"FACTION_DEMAND_ACCEPT", "FACTION_DEMAND_REFUSE", "FACTION_DEMAND_PEASANT_REFUSE"};
    ROW("2a a notice (ACCEPTED_OPT) is not a demand", !fd::classify(notice).is_demand());
    ROW("2b the co-rule counter-offer (ACCEPT/REJECT) is not a demand", !fd::classify(corule).is_demand());
    ROW("2c the populist vassal's letter is not a demand", !fd::classify(vassal).is_demand());
    ROW("2d the claim letter is not a demand", !fd::classify(claim).is_demand());
    ROW("2e two refuse options: not read as a demand", !fd::classify(two).is_demand());
    // 3. the populist letter as the bridge first read it (a recorded game, 1079.11.13):
    //    the dynamic refuse name read "", so no refuse option and no demand: a standing answer
    //    could not refuse it.
    {
        K old = {"FACTION_DEMAND_POPULIST_ACCEPT_CONVERT", "FACTION_DEMAND_POPULIST_ACCEPT_CONVERT_STATE_FAITH",
                 "FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE", ""};
        auto o = fd::classify(old);
        ROW("3 the recorded read (refuse unread) is not a demand", !o.is_demand() && fd::choose(fd::Want::kRefuse, o, old, 2) == -1);
    }
    // 4. choose
    {
        const K& pop = letters[1].second;
        auto o = fd::classify(pop);
        ROW("4a refuse -> the refuse option whatever the AI picked", fd::choose(fd::Want::kRefuse, o, pop, 2) == 3);
        ROW("4b accept, the AI accepts -> its own way of accepting", fd::choose(fd::Want::kAccept, o, pop, 2) == 2);
        ROW("4c accept, the AI refuses, no classic key -> the first accept", fd::choose(fd::Want::kAccept, o, pop, 3) == 0);
        ROW("4d ai -> keep the AI's pick (-1)", fd::choose(fd::Want::kAI, o, pop, 3) == -1);
        const K& ind = letters[0].second;
        auto oi = fd::classify(ind);
        ROW("4e accept, the AI refuses -> FACTION_DEMAND_ACCEPT (not the co-emperor bribe)",
            fd::choose(fd::Want::kAccept, oi, ind, 2) == 0);
        const K& pea = letters[2].second;
        ROW("4f peasant refuse -> 1", fd::choose(fd::Want::kRefuse, fd::classify(pea), pea, 0) == 1);
    }
    // 5. the name walk on the populist refuse option's layout (867 core dump)
    {
        Mem m = populist_refuse_name();
        const fd::StrAbi L = fd::StrAbi::kLibstdcxx;
        ROW("5a composite -> the fallback FACTION_DEMAND_POPULIST_REFUSE",
            fd::name_key(m, 0x7f8ff8ad42e0, 0, L) == "FACTION_DEMAND_POPULIST_REFUSE");
        ROW("5b a single entry reads its own key", fd::name_key(m, 0x7f8ff8adc220, 0, L) == "FACTION_DEMAND_POPULIST_REFUSE_ADMIN");
        Mem bad = m;
        bad.w[0x7f8ff8ad42e0 + 0x10] = 0x0000004000000040ull;   // 64 children: not a name
        ROW("5c an absurd child count reads nothing", fd::name_key(bad, 0x7f8ff8ad42e0, 0, L).empty());
        Mem cut = m;
        cut.w.erase(0x7f8ff8ace440 + 8);                         // the fallback's slot unmapped
        ROW("5d an unmapped alternative reads nothing", fd::name_key(cut, 0x7f8ff8ad42e0, 0, L).empty());
        Mem junk = m;
        junk.w[0x7f8ff8adc690 + 0x38] = 5; junk.s[0x7f8ff8aa1000] = "AB CD";   // not a key's spelling
        ROW("5e a string that is not a key reads nothing", fd::name_key(junk, 0x7f8ff8ad42e0, 0, L).empty());
        Mem loop = m;
        loop.w[0x7f8ff8ace440 + 8] = 0x7f8ff8ad42e0;            // a cycle back to the top
        ROW("5f a cycle stops at the depth limit", fd::name_key(loop, 0x7f8ff8ad42e0, 0, L).empty());
    }
    // 6. the same walk over the MSVC string layout (ck3.exe 1.18.3.1 under Proton,
    // the mid1 core: physician_epidemic_events.1000's first option, entry 0xa34d28b0, its key a
    // heap string: data +0x30, size +0x40 = 32, capacity +0x48 = 0x2f; +0x38 holds 0x40)
    {
        const fd::StrAbi W = fd::StrAbi::kMsvc, L = fd::StrAbi::kLibstdcxx;
        Mem w;
        const uint64_t E = 0xa34d28b0;
        w.w[E + 0x08] = 0; w.w[E + 0x10] = 0;
        w.w[E + 0x30] = 0x59abe2c0; w.w[E + 0x38] = 0x40; w.w[E + 0x40] = 32; w.w[E + 0x48] = 0x2f;
        w.s[0x59abe2c0] = "physician_epidemic_events.1000.a";
        ROW("6a MSVC heap key", fd::name_key(w, E, 0, W) == "physician_epidemic_events.1000.a");
        ROW("6b control: the same bytes read as libstdc++ give no key", fd::name_key(w, E, 0, L).empty());
        // an inline key (capacity 15): the characters are at entry+0x30 itself
        Mem in;
        const uint64_t I = 0x5000000;
        in.w[I + 0x40] = 9; in.w[I + 0x48] = 15; in.s[I + 0x30] = "faith.1.a";
        ROW("6c MSVC inline key (capacity 15)", fd::name_key(in, I, 0, W) == "faith.1.a");
        Mem big = in;
        big.w[I + 0x40] = 16;                                    // size above capacity: not a string
        ROW("6d size above capacity reads nothing", fd::name_key(big, I, 0, W).empty());
        // a composite over MSVC alternatives: the last one's key (172 such options in the core)
        Mem c = w;
        const uint64_t C = 0xa0000000, ARR = 0xa0001000, S0 = 0xa0002000;
        c.w[C + 0x08] = ARR; c.w[C + 0x10] = 0x0000000100000001ull; c.w[C + 0x30] = 0; c.w[C + 0x40] = 0;
        c.w[C + 0x48] = 0;
        c.w[ARR] = S0;
        c.w[S0 + 0x30] = 0x59abf000; c.w[S0 + 0x40] = 34; c.w[S0 + 0x48] = 0x2f;
        c.s[0x59abf000] = "ep3_contract_event.0045.a_pillaged";
        ROW("6e MSVC composite -> its alternative's key", fd::name_key(c, C, 0, W) == "ep3_contract_event.0045.a_pillaged");
        // the build's own layout is the default argument
#if defined(_WIN32)
        ROW("6f this build reads MSVC by default", fd::kGameStrAbi == W);
#else
        ROW("6f this build reads libstdc++ by default", fd::kGameStrAbi == L &&
            fd::name_key(populist_refuse_name(), 0x7f8ff8ad42e0) == "FACTION_DEMAND_POPULIST_REFUSE");
#endif
    }
    std::printf("%d rows, %d failed\n", g_rows, g_fail);
    return g_fail ? 1 : 0;
}
