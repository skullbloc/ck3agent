// faction_demand.h — which event is a faction's demand to its target, which of
// its options refuses, and how an option's `name` reads when it is a dynamic text.
//
// A faction presses its demand by sending its target a letter event (common/factions/*.txt,
// each type's `demand = { ... trigger_event = <event> }`); an AI-run target answers it inside
// AI_PICK_EVENT_OPTION like any event (re/findings/re_faction_demand.md). Eleven such letters
// ship (faction_demand.0001 .0101 .0221 .1000 .1001 .1101 .2001 .2011 .9998,
// tgp_faction_events.0101 .0201). Each has one option named FACTION_DEMAND_*REFUSE (refusing
// starts the faction's war) and one or more named FACTION_DEMAND_*ACCEPT*. The populist
// letter's refuse option has a dynamic name (`name = { text = { first_valid = { triggered_desc
// = {...} desc = FACTION_DEMAND_POPULIST_REFUSE } } }`), which the engine keeps as a composite
// description whose last alternative is the untriggered fallback: name_key() reads that one.
//
// Header-only and engine-free: test_faction_demand.cpp checks the classification on every
// shipped letter's option keys and the name walk on the populist letter's layout as the 867
// core dump holds it. Every doubt answers "not a demand" / "" and the hook keeps the AI's pick.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bridge::faction_demand {

inline constexpr std::string_view kPrefix = "FACTION_DEMAND_";
inline constexpr std::string_view kClassicAccept = "FACTION_DEMAND_ACCEPT";

inline bool ends_with(std::string_view s, std::string_view t) {
    return s.size() >= t.size() && s.substr(s.size() - t.size()) == t;
}
inline bool starts_with(std::string_view s, std::string_view t) {
    return s.size() >= t.size() && s.substr(0, t.size()) == t;
}

//: The option that refuses a demand: FACTION_DEMAND_REFUSE, FACTION_DEMAND_PEASANT_REFUSE,
//: FACTION_DEMAND_POPULIST_REFUSE (and its _ADMIN variant, the populist name's other branch).
inline bool is_refuse_key(std::string_view k) {
    return starts_with(k, kPrefix) && (ends_with(k, "_REFUSE") || ends_with(k, "_REFUSE_ADMIN"));
}
//: An option that accepts it: FACTION_DEMAND_ACCEPT, FACTION_DEMAND_PEASANT_ACCEPT,
//: FACTION_DEMAND_POPULIST_ACCEPT_CONVERT / _CONVERT_STATE_FAITH / _GRANT_INDEPENDENCE,
//: FACTION_DEMAND_NOMADIC_ACCEPT_GRANT_INDEPENDENCE. Not the follow-up notices
//: (FACTION_DEMAND_ACCEPTED_OPT) nor the co-rule counter-offer (…_CORULE_ACCEPT, sent to the
//: faction's leader with a _REJECT option, never a _REFUSE one).
inline bool is_accept_key(std::string_view k) {
    if (!starts_with(k, kPrefix) || ends_with(k, "_OPT")) return false;
    return k.find("_ACCEPT") != std::string_view::npos && k.find("_ACCEPTED") == std::string_view::npos;
}

struct Options {
    int refuse = -1;               // the refuse option's index, -1 = none
    std::vector<int> accept;       // the accept options, file order
    bool is_demand() const { return refuse >= 0 && !accept.empty(); }
};

//: The options of one event (keys in file order, "" = unread). A demand has exactly one
//: refuse option and at least one accept option; anything else is not a demand.
inline Options classify(const std::vector<std::string>& keys) {
    Options o;
    int refuses = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (is_refuse_key(keys[i])) { ++refuses; o.refuse = static_cast<int>(i); }
        else if (is_accept_key(keys[i])) o.accept.push_back(static_cast<int>(i));
    }
    if (refuses != 1) o.refuse = -1;
    if (o.refuse < 0) o.accept.clear();
    return o;
}

enum class Want : int { kAI = 0, kAccept = 1, kRefuse = 2 };

//: The option a standing answer wants, -1 = keep the AI's pick. Refuse: the refuse option.
//: Accept: the AI's own pick when it already accepts (the populist letter has three ways to
//: accept and the AI weighs them), else FACTION_DEMAND_ACCEPT when the letter has it, else
//: its first accept option.
inline int choose(Want w, const Options& o, const std::vector<std::string>& keys, int64_t ai_pick) {
    if (!o.is_demand() || w == Want::kAI) return -1;
    if (w == Want::kRefuse) return o.refuse;
    for (int a : o.accept) if (a == ai_pick) return a;
    for (int a : o.accept)
        if (static_cast<size_t>(a) < keys.size() && keys[static_cast<size_t>(a)] == kClassicAccept) return a;
    return o.accept.front();
}

// ── an option's name, as the engine holds it ─────────────────────────────────────
// option + OPTION_DESC_HOLDER -> holder; *holder = CJominiEventOption::CEventOptionDesc;
// desc + OPTION_DESC_FIRST_ENTRY -> the first CDynamicDescription entry. A plain `name = KEY`
// entry (CDynamicDescription::CSingleDesc) holds the key as a std::string at +0x30. A dynamic
// name (`text = { first_valid = ... }`) is a composite entry: its alternatives are a pointer
// array at +0x08 with the count at +0x14 (the composite's destructor, image FUN_07504e60 /
// FUN_07503520, destroys exactly those), and +0x30 holds no key string.
inline constexpr size_t kEntryKey = 0x30;        // CSingleDesc: std::string {ptr, len}
inline constexpr size_t kChildren = 0x08;        // composite: entry* array
inline constexpr size_t kChildCount = 0x14;      // composite: int32 count
inline constexpr int kMaxChildren = 16;
inline constexpr int kMaxDepth = 3;
inline constexpr size_t kMaxKeyLen = 64;

//: A loc key's spelling ([A-Za-z0-9_.], 1..64): anything else at +0x30 is not a key.
inline bool key_chars_ok(std::string_view k) {
    if (k.empty() || k.size() > kMaxKeyLen) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'))
            return false;
    return true;
}

//: The std::string layout of the build the bridge runs in (ADR 0001 P5; bridge.h
//: game_string_view): libstdc++ {data +0, length +8}; MSVC {inline buffer or data +0,
//: size +0x10, capacity +0x18}, the characters inline while capacity <= 15. On Windows
//: the option-name chain's offsets hold on Windows, only the key's layout differs
//: (ck3_offsets_win.h struct_layout::OPTION_ENTRY_KEY_MSVC: 6889 options of a 1.18.3.1 core).
enum class StrAbi { kLibstdcxx, kMsvc };
#if defined(_WIN32)
inline constexpr StrAbi kGameStrAbi = StrAbi::kMsvc;
#else
inline constexpr StrAbi kGameStrAbi = StrAbi::kLibstdcxx;
#endif

//: Where the characters of the string at `a` are, and how many, for `abi`. The libstdc++
//: branch reads exactly the two words name_key read before the Windows port.
template <typename M>
bool string_at(const M& m, uint64_t a, StrAbi abi, uint64_t* data, uint64_t* len) {
    if (abi == StrAbi::kLibstdcxx) return m.u64(a, data) && m.u64(a + 8, len);
    uint64_t cap = 0;
    if (!m.u64(a + 0x10, len) || !m.u64(a + 0x18, &cap) || cap < *len) return false;
    if (cap <= 15) {
        *data = a;
        return true;
    }
    return m.u64(a, data);
}

//: The key of a name entry: a single entry's own key; a composite's LAST alternative's key
//: (first_valid's untriggered fallback, the last `desc =` in the file), at most kMaxDepth
//: levels down. `M` reads the game's memory: bool u64(addr, uint64_t*), bool i32(addr,
//: int32_t*), bool str(addr, len, std::string*) — each false when the range is not mapped.
template <typename M>
std::string name_key(const M& m, uint64_t entry, int depth = 0, StrAbi abi = kGameStrAbi) {
    if (entry < 0x10000 || depth > kMaxDepth) return std::string();
    uint64_t data = 0, len = 0;
    if (string_at(m, entry + kEntryKey, abi, &data, &len) && len > 0 && len <= kMaxKeyLen &&
        data >= 0x10000) {
        std::string s;
        if (m.str(data, static_cast<size_t>(len), &s) && key_chars_ok(s)) return s;
    }
    int32_t n = 0;
    uint64_t arr = 0, last = 0;
    if (!m.i32(entry + kChildCount, &n) || n < 1 || n > kMaxChildren) return std::string();
    if (!m.u64(entry + kChildren, &arr) || arr < 0x10000) return std::string();
    if (!m.u64(arr + static_cast<uint64_t>(n - 1) * 8, &last)) return std::string();
    return name_key(m, last, depth + 1, abi);
}

}  // namespace bridge::faction_demand
