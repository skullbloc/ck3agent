// test_vassal_roster.cpp — lane l6 (cleanup 2026.11). NO GAME. The direct-vassal
// read the bridge's realm queries call (vassal_roster.h), driven over a fake memory
// laid out as dumps ivar867 / 1097 show a ruler's contracts: the ruler's
// title_realm VASSAL_CONTRACT_ARRAY holds contract refs; each contract carries its
// key at REF_KEY, its SUBJECT at +0x20 and its LIEGE at +0x28.
//
// THE CLAIM: the read lists exactly the subjects of the contracts whose liege is
// the ruler — never a contract of another liege, a ref the DB does not hold, a dead
// or stale character, or the ruler itself — and a fixture laid out the old way
// (liege at +0x20) lists nobody, so the test tells the two layouts apart.

#include "vassal_roster.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using bridge::vassal_roster::direct_vassal_ids;
namespace sc = ::subject_contract;

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what,
           detail.empty() ? "" : " — ", detail.c_str());
}

struct FakeMem {
    std::map<uintptr_t, uint64_t> words;      // 8-byte cells; u32 reads the low half
    std::set<uintptr_t> regions;              // mapped object bases
    std::map<uint32_t, uintptr_t> contracts;  // DB slot -> contract object
    std::map<uintptr_t, uint32_t> live_chars; // character object -> id (live DB entries)

    uint32_t  u32(uintptr_t a) const { auto it = words.find(a); return it == words.end() ? 0 : uint32_t(it->second); }
    uintptr_t ptr(uintptr_t a) const { auto it = words.find(a); return it == words.end() ? 0 : uintptr_t(it->second); }
    bool      mapped(uintptr_t a) const {
        for (uintptr_t b : regions) if (a >= b && a < b + 0x1000) return true;
        return false;
    }
    uintptr_t contract(uint32_t ref) const {
        auto it = contracts.find(ref & 0xFFFFFF);
        if (it == contracts.end()) return 0;
        return (u32(it->second + sc::REF_KEY) & 0xFFFFFF) == (ref & 0xFFFFFF) ? it->second : 0;
    }
    uint32_t char_id(uintptr_t ch) const {
        auto it = live_chars.find(ch);
        return it == live_chars.end() ? 0 : it->second;
    }
};

static uintptr_t g_next = 0x10000;
static uintptr_t alloc(FakeMem& m) { uintptr_t b = g_next; g_next += 0x1000; m.regions.insert(b); return b; }
static uintptr_t character(FakeMem& m, uint32_t id) { uintptr_t c = alloc(m); m.live_chars[c] = id; return c; }

// A contract `ref` between liege and subject, the parties at the given offsets.
static void contract(FakeMem& m, uint32_t ref, uintptr_t liege, uintptr_t subject,
                     size_t liege_off = sc::LIEGE, size_t subject_off = sc::SUBJECT) {
    uintptr_t c = alloc(m);
    m.words[c + sc::REF_KEY] = ref;
    m.words[c + liege_off] = liege;
    m.words[c + subject_off] = subject;
    m.contracts[ref] = c;
}

static uintptr_t realm_with(FakeMem& m, const std::vector<uint32_t>& refs) {
    uintptr_t tr = alloc(m), data = alloc(m);
    for (size_t i = 0; i < refs.size(); i++) m.words[data + i * 4] = refs[i];
    m.words[tr + ck3::title_realm::VASSAL_CONTRACT_ARRAY] = data;
    m.words[tr + ck3::title_realm::VASSAL_CONTRACT_ARRAY + 0x0C] = refs.size();
    return tr;
}

static std::string ids(const std::vector<uint32_t>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + std::to_string(v[i]);
    return s + "]";
}

int main() {
    printf("test_vassal_roster\n");
    check(sc::SUBJECT == 0x20 && sc::LIEGE == 0x28 && sc::REF_KEY == 0x08,
          "the contract layout the dumps show: subject +0x20, liege +0x28, key +0x08");

    FakeMem m;
    const uint32_t KING = 10465, OTHER = 999;
    uintptr_t king = character(m, KING), other = character(m, OTHER);
    uintptr_t v1 = character(m, 12829), v2 = character(m, 13463), v3 = character(m, 9699);
    uintptr_t dead = alloc(m);                 // a character the DB no longer holds live
    contract(m, 2300, king, v1);
    contract(m, 2427, king, v2);
    contract(m, 2718, king, v3);
    contract(m, 2719, king, v3);               // a second contract with the same subject
    contract(m, 2926, other, character(m, 777));  // another liege's contract in the array
    contract(m, 3054, king, dead);
    contract(m, 3055, king, king);             // the ruler as its own subject
    uintptr_t tr = realm_with(m, {2300, 2427, 2718, 2719, 2926, 3054, 3055, 4000 /* not in the DB */, 0});

    auto got = direct_vassal_ids(m, tr, KING);
    check(got == std::vector<uint32_t>{12829, 13463, 9699},
          "the king's vassals are the subjects of his contracts, once each", ids(got));
    check(direct_vassal_ids(m, tr, KING | 0x5000000) == got,
          "the ruler id is compared on its 24-bit slot");
    check(direct_vassal_ids(m, tr, OTHER) == std::vector<uint32_t>{777},
          "a contract counts for its own liege only");
    check(direct_vassal_ids(m, 0, KING).empty(), "no title_realm: no vassals");
    check(direct_vassal_ids(m, realm_with(m, {}), KING).empty(), "no contracts: no vassals");

    // contract_with: the contract set_obligation_level writes, keyed on the subject.
    using bridge::vassal_roster::contract_with;
    check(contract_with(m, tr, KING, 13463) == m.contracts[2427],
          "contract_with finds the king's contract with his vassal");
    check(contract_with(m, tr, KING, 9699) == m.contracts[2718],
          "contract_with: the first of two contracts with one subject");
    check(contract_with(m, tr, KING, 777) == 0, "contract_with: another liege's vassal has none");
    check(contract_with(m, tr, OTHER, 12829) == 0, "contract_with: not the liege, no contract");
    // Control (witness #2): a contract keyed where the pre-fix walk looked it up
    // (+0x10) is not found, so the test tells the two lookups apart.
    {
        FakeMem k;
        uintptr_t kk = character(k, KING), kv = character(k, 12829);
        contract(k, 2300, kk, kv);
        uintptr_t c = k.contracts[2300];
        k.words[c + sc::REF_KEY] = 0;          // its key moved off REF_KEY
        k.words[c + 0x10] = 2300;
        check(contract_with(k, realm_with(k, {2300}), KING, 12829) == 0,
              "control: a contract keyed at +0x10 instead of REF_KEY is not found");
    }

    uintptr_t huge = realm_with(m, {2300});
    m.words[huge + ck3::title_realm::VASSAL_CONTRACT_ARRAY + 0x0C] =
        bridge::vassal_roster::MAX_CONTRACTS + 1;
    check(direct_vassal_ids(m, huge, KING).empty(), "a count past the bound is not walked");

    // Negative control: the same contracts laid out the pre-fix way (liege +0x20,
    // subject +0x28) must give nobody: the read keys on the layout the dumps show.
    FakeMem old;
    uintptr_t oking = character(old, KING), ov = character(old, 12829);
    contract(old, 2300, oking, ov, /*liege_off=*/0x20, /*subject_off=*/0x28);
    check(direct_vassal_ids(old, realm_with(old, {2300}), KING).empty(),
          "control: a contract with the liege at +0x20 lists no vassal of the king");

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
