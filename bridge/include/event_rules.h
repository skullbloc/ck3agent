// event_rules.h — a seat's standing answers to its own ruler's events (route E2).
//
// The game's AI answers an AI character's event inside CoreEventQueue, in the same
// call: AI_PICK_EVENT_OPTION returns an option index and the engine executes it at
// once (re/findings/re_events_purse.md §1). There is nothing queued to hold, so a
// seat's answer has to be set IN ADVANCE: a table, per seated character, event key
// -> rule. The hook (factions.cpp on_ai_pick_event_option) calls the game's picker
// first, then asks decide() below whether a rule changes the pick, and records every
// pick of a watched character whether a rule applied or not.
//
// Header-only and engine-free: test_event_rules.cpp checks every rule kind and every
// fall-back on a host with no game. Every doubt answers the AI's own pick.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bridge::event_rules {

enum class Kind : uint8_t {
    kOption = 1,   // always the named option
    kDecline,      // the other option of a two-option event (not the named one; named = first by default)
    kPayIf,        // the named option when the currency covers the amount, else the other option
};
enum class Currency : uint8_t { kGold = 0, kPrestige, kPiety };

//: Limits: a seat table is small and bounded (a hook reads it under a mutex).
inline constexpr size_t kMaxSeated = 8;          // characters with a table
inline constexpr size_t kMaxRulesPerChar = 32;   // rules per character
inline constexpr size_t kPickRingCap = 64;       // recorded picks kept (all seated characters)
inline constexpr int kMaxOptions = 32;           // an event with more options is not read
inline constexpr int64_t kFp = 100000;           // the game's fixed point (gold, prestige, piety)
inline constexpr int64_t kMaxAmount = 1000000;   // whole units

//: The kill switch: this environment variable set to "0" disables the table
//: entirely (no rule is set, the hook never consults it, nothing is recorded).
inline constexpr const char* kKillSwitchEnv = "CK3_BRIDGE_EVENT_RULES";
inline bool kill_switch_off(const char* env_value) {
    return env_value && env_value[0] == '0' && env_value[1] == '\0';
}

struct Rule {
    Kind kind = Kind::kOption;
    std::string option_key;      // the option's `name = KEY` loc key ("" = by index)
    int option_index = -1;       // used when option_key is empty; -1 = the first option (0)
    Currency currency = Currency::kGold;
    int64_t amount_fp = 0;       // pay_if: the amount, fixed point
    // What to answer when the rule's option cannot be taken (not in the event, not
    // offered by the game's own check): "" = the game's AI picks (the default, said as such in the
    // record), else a named second option, applied only if the game offers it.
    std::string else_key;
};

//: An event key as the game's files spell it: [a-z0-9_.] (court_chaplain_task.0301),
//: a letter first, at most 96 characters.
inline bool event_key_ok(std::string_view k) {
    if (k.empty() || k.size() > 96 || !(k[0] >= 'a' && k[0] <= 'z')) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    return true;
}
//: An option key: the loc key spelling ([A-Za-z0-9_.]), at most 64.
inline bool option_key_ok(std::string_view k) {
    if (k.empty() || k.size() > 64) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'))
            return false;
    return true;
}

inline const char* kind_name(Kind k) {
    switch (k) {
    case Kind::kOption: return "option";
    case Kind::kDecline: return "decline";
    case Kind::kPayIf: return "pay_if";
    }
    return "?";
}
inline bool kind_by_name(std::string_view n, Kind* out) {
    if (n == "option") { *out = Kind::kOption; return true; }
    if (n == "decline") { *out = Kind::kDecline; return true; }
    if (n == "pay_if") { *out = Kind::kPayIf; return true; }
    return false;
}
inline const char* currency_name(Currency c) {
    switch (c) {
    case Currency::kGold: return "gold";
    case Currency::kPrestige: return "prestige";
    case Currency::kPiety: return "piety";
    }
    return "?";
}
inline bool currency_by_name(std::string_view n, Currency* out) {
    if (n == "gold") { *out = Currency::kGold; return true; }
    if (n == "prestige") { *out = Currency::kPrestige; return true; }
    if (n == "piety") { *out = Currency::kPiety; return true; }
    return false;
}

//: What the hook can read of each option at the pick.
struct OptionView {
    std::string key;             // "" when the loc key did not read
};

//: Why a rule did or did not change the pick. The words are the record's `why`.
enum class Why : uint8_t {
    kApplied = 0,         // the rule's option was returned
    kSameAsAi,            // the rule's option is the AI's own pick (returned, nothing changed)
    kAiPickedNone,        // the game's picker returned -1: kept
    kOptionNotInEvent,    // the named key is not one of this event's options
    kIndexOutOfRange,     // the named index is not one of this event's options
    kNeedsTwoOptions,     // decline / pay_if's else need exactly two options
    kCurrencyUnread,      // pay_if: the ruler's currency did not read
    kOptionNotAvailable,  // the game's own availability check refused the option
    kOptionsUnread,       // the event's option list did not read
};
inline const char* why_name(Why w) {
    switch (w) {
    case Why::kApplied: return "applied";
    case Why::kSameAsAi: return "same_as_ai";
    case Why::kAiPickedNone: return "ai_picked_none";
    case Why::kOptionNotInEvent: return "option_not_in_event";
    case Why::kIndexOutOfRange: return "option_index_out_of_range";
    case Why::kNeedsTwoOptions: return "needs_two_options";
    case Why::kCurrencyUnread: return "currency_unread";
    case Why::kOptionNotAvailable: return "option_not_available";
    case Why::kOptionsUnread: return "options_unread";
    }
    return "?";
}

struct Target {
    int index = -1;              // the option the rule wants, -1 = none (fall back)
    Why why = Why::kApplied;     // when index < 0: why not
    bool covered = false;        // pay_if: did the currency cover the amount
};

//: Step 1 (no engine call): which option the rule wants for this event. `n` is the
//: event's option count, `opts` the keys read (opts.size() == n, or empty when they
//: did not read). `have` is false when the currency did not read.
inline Target target_of(const Rule& r, int n, const std::vector<OptionView>& opts, bool have, int64_t value_fp) {
    Target t;
    if (n <= 0 || n > kMaxOptions) { t.why = Why::kOptionsUnread; return t; }
    int named = -1;
    if (!r.option_key.empty()) {
        if (static_cast<int>(opts.size()) != n) { t.why = Why::kOptionsUnread; return t; }
        for (int i = 0; i < n; ++i)
            if (opts[static_cast<size_t>(i)].key == r.option_key) { named = i; break; }
        if (named < 0) { t.why = Why::kOptionNotInEvent; return t; }
    } else {
        named = r.option_index < 0 ? 0 : r.option_index;
        if (named >= n) { t.why = Why::kIndexOutOfRange; return t; }
    }
    switch (r.kind) {
    case Kind::kOption:
        t.index = named;
        return t;
    case Kind::kDecline:
        if (n != 2) { t.why = Why::kNeedsTwoOptions; return t; }
        t.index = 1 - named;
        return t;
    case Kind::kPayIf: {
        // (1078.11.06: the 3-option claim letter court_chaplain_task.0302 could take no gold rule and
        // the AI paid 90 at 48.85 gold): with three or more options the rule names the one taken when the currency
        // does not cover the amount (else_key); with two it is the other option, as before
        int other = -1;
        if (n == 2) {
            other = 1 - named;
        } else {
            if (r.else_key.empty()) { t.why = Why::kNeedsTwoOptions; return t; }
            if (static_cast<int>(opts.size()) != n) { t.why = Why::kOptionsUnread; return t; }
            for (int i = 0; i < n; ++i)
                if (opts[static_cast<size_t>(i)].key == r.else_key) { other = i; break; }
            if (other < 0 || other == named) { t.why = Why::kOptionNotInEvent; return t; }
        }
        if (!have) { t.why = Why::kCurrencyUnread; return t; }
        t.covered = value_fp >= r.amount_fp;
        t.index = t.covered ? named : other;
        return t;
    }
    }
    t.why = Why::kOptionsUnread;
    return t;
}

//: Step 2: the index the hook returns. `ai_pick` is the game's picker's own return
//: (-1 = none); `available` the game's availability check of the target option.
struct Decision {
    int64_t returned = -1;       // what the hook returns to the engine
    Why why = Why::kApplied;
    bool overridden = false;     // returned != ai_pick
};
inline Decision decide(const Target& t, int64_t ai_pick, bool available) {
    Decision d;
    d.returned = ai_pick;
    if (ai_pick < 0) { d.why = Why::kAiPickedNone; return d; }
    if (t.index < 0) { d.why = t.why; return d; }
    if (!available) { d.why = Why::kOptionNotAvailable; return d; }
    d.returned = t.index;
    d.overridden = (t.index != ai_pick);
    d.why = d.overridden ? Why::kApplied : Why::kSameAsAi;
    return d;
}

//: The index of option `key` in the event (-1: not one of its options).
inline int index_of(const std::vector<OptionView>& opts, const std::string& key) {
    if (key.empty()) return -1;
    for (size_t i = 0; i < opts.size(); ++i)
        if (opts[i].key == key) return static_cast<int>(i);
    return -1;
}

//: (1091.07.16: the rule named FACTION_DEMAND_POPULIST_ACCEPT_CONVERT, which the
//: game did not offer this ruler; the AI's REFUSE was applied and recorded `by: event_rule`): the
//: decision when the rule's own option could not be taken. `d` is decide()'s; when it kept the AI's
//: pick for a reason of the rule (not ai_picked_none), the rule's else option is returned if it is in
//: the event (else_index >= 0) and the game offers it. `fell_back_else` says so; otherwise the AI's
//: pick stands and the record names it the AI's.
struct Fallback {
    Decision d;
    bool rule_decided = false;   // the rule's own option (applied or same as the AI's)
    bool fell_back_else = false; // the rule's else option was returned
};
inline Fallback with_else(const Decision& d, int64_t ai_pick, int else_index, bool else_available) {
    Fallback f;
    f.d = d;
    if (d.why == Why::kApplied || d.why == Why::kSameAsAi) { f.rule_decided = true; return f; }
    if (d.why == Why::kAiPickedNone || ai_pick < 0) return f;
    if (else_index >= 0 && else_available) {
        f.d.returned = else_index;
        f.d.overridden = (else_index != ai_pick);
        f.fell_back_else = true;
    }
    return f;
}

//: One recorded pick (the ring the seat reads).
struct Pick {
    uint64_t seq = 0;
    int32_t date_raw = 0;
    uint32_t character_id = 0;
    std::string event;
    std::vector<std::string> options;  // keys in file order ("" unread)
    int64_t ai_pick = -1;
    int64_t applied = -1;
    bool had_rule = false;
    std::string rule;                  // "pay_if gold>=100 option=0" etc., "" when none
    Why why = Why::kApplied;
    bool covered = false;
    bool gold_read = false;
    int64_t gold_fp = 0, prestige_fp = 0, piety_fp = 0;
    bool by_faction_demand = false;    // the faction-demand answer decided it
    int64_t wanted = -1;               // The rule's option (index), -1 = none / not in the event
    bool fell_back_else = false;       // The rule's else option was applied
    std::string else_key;              // The rule's else option ("" = the AI)
};

inline std::string rule_text(const Rule& r) {
    std::string s = kind_name(r.kind);
    if (r.kind == Kind::kPayIf) {
        s += std::string(" ") + currency_name(r.currency) + ">=" + std::to_string(r.amount_fp / kFp);
    }
    s += " option=";
    s += r.option_key.empty() ? std::to_string(r.option_index < 0 ? 0 : r.option_index) : r.option_key;
    if (!r.else_key.empty()) s += " else=" + r.else_key;
    return s;
}

//: (1072.07.16: the claim-letter rules Murchad set died with him while his vetoes were carried; his
//: heir's AI then paid two letters, 162 gold): at the seat's handoff from a dead ruler to his heir, the heir's table takes
//: the dead ruler's rules, as the veto table's carry does: a rule of the heir's own for the same event is kept, the rest
//: are added in the dead ruler's order up to kMaxRulesPerChar. Returns how many were added. The dead ruler's table is
//: left as it was.
inline size_t carry_rules(const std::vector<std::pair<std::string, Rule>>& from,
                          std::vector<std::pair<std::string, Rule>>& to) {
    size_t added = 0;
    for (const auto& kv : from) {
        bool have = false;
        for (const auto& t : to) have = have || t.first == kv.first;
        if (have) continue;
        if (to.size() >= kMaxRulesPerChar) break;
        to.push_back(kv);
        ++added;
    }
    return added;
}

}  // namespace bridge::event_rules
