// test_event_rules.cpp — a seat's standing answers to its own
// ruler's events (bridge/include/event_rules.h), engine-free.
//
// The event every row models is the claim letter court_chaplain_task.0301
// (game/events/councillor_task_events/court_chaplain_task_events.txt:93-171, 1.18.3.1):
// two options, `court_chaplain_task.0301.a` (pay COST, get the claim) and
// `court_chaplain_task.0301.b` (decline). The AI's own pick is the picker's return.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "event_rules.h"

namespace er = bridge::event_rules;

static int g_fail = 0, g_rows = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        ++g_rows;                                                           \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

static std::vector<er::OptionView> letter() {
    return {{"court_chaplain_task.0301.a"}, {"court_chaplain_task.0301.b"}};
}

int main() {
    const auto L = letter();
    const int64_t FP = er::kFp;

    // 1. decline, the AI would pay (picked 0): the hook returns 1.
    {
        er::Rule r; r.kind = er::Kind::kDecline;
        auto t = er::target_of(r, 2, L, true, 84 * FP);
        auto d = er::decide(t, 0, true);
        ROW("1 decline returns the other option", d.returned == 1 && d.overridden && d.why == er::Why::kApplied);
    }
    // 2. decline named by key: the option NOT named.
    {
        er::Rule r; r.kind = er::Kind::kDecline; r.option_key = "court_chaplain_task.0301.a";
        auto d = er::decide(er::target_of(r, 2, L, true, 0), 0, true);
        ROW("2 decline by key", d.returned == 1);
    }
    // 3. pay_if gold covers: gold 120 >= 96 -> option a.
    {
        er::Rule r; r.kind = er::Kind::kPayIf; r.amount_fp = 96 * FP;
        auto t = er::target_of(r, 2, L, true, 120 * FP);
        auto d = er::decide(t, 1, true);
        ROW("3 pay_if covered pays", t.covered && d.returned == 0 && d.overridden);
    }
    // 4. pay_if not covered: gold 40 < 96 -> option b although the AI paid (20:10 odds).
    {
        er::Rule r; r.kind = er::Kind::kPayIf; r.amount_fp = 96 * FP;
        auto t = er::target_of(r, 2, L, true, 40 * FP);
        auto d = er::decide(t, 0, true);
        ROW("4 pay_if uncovered declines", !t.covered && d.returned == 1 && d.overridden);
    }
    // 5. pay_if exact amount covers (>=).
    {
        er::Rule r; r.kind = er::Kind::kPayIf; r.amount_fp = 96 * FP;
        auto t = er::target_of(r, 2, L, true, 96 * FP);
        ROW("5 pay_if exact covers", t.covered && t.index == 0);
    }
    // 6. pay_if with the currency unread: the AI's pick, why currency_unread.
    {
        er::Rule r; r.kind = er::Kind::kPayIf; r.amount_fp = 96 * FP;
        auto d = er::decide(er::target_of(r, 2, L, false, 0), 0, true);
        ROW("6 pay_if unread currency keeps the AI", d.returned == 0 && !d.overridden && d.why == er::Why::kCurrencyUnread);
    }
    // 7. option by key, the AI already picked it: same_as_ai, not overridden.
    {
        er::Rule r; r.option_key = "court_chaplain_task.0301.b";
        auto d = er::decide(er::target_of(r, 2, L, true, 0), 1, true);
        ROW("7 same as the AI", d.returned == 1 && !d.overridden && d.why == er::Why::kSameAsAi);
    }
    // 8. a key not in the event: the AI's pick.
    {
        er::Rule r; r.option_key = "FACTION_DEMAND_REFUSE";
        auto d = er::decide(er::target_of(r, 2, L, true, 0), 0, true);
        ROW("8 key not in the event", d.returned == 0 && d.why == er::Why::kOptionNotInEvent);
    }
    // 9. a key rule when the keys did not read: the AI's pick, options_unread.
    {
        er::Rule r; r.option_key = "court_chaplain_task.0301.b";
        auto d = er::decide(er::target_of(r, 2, {}, true, 0), 0, true);
        ROW("9 keys unread", d.returned == 0 && d.why == er::Why::kOptionsUnread);
    }
    // 10. an index past the options.
    {
        er::Rule r; r.option_index = 5;
        auto d = er::decide(er::target_of(r, 2, L, true, 0), 0, true);
        ROW("10 index out of range", d.returned == 0 && d.why == er::Why::kIndexOutOfRange);
    }
    // 11. decline on a three-option event: the AI's pick.
    {
        er::Rule r; r.kind = er::Kind::kDecline;
        std::vector<er::OptionView> three = {{"a"}, {"b"}, {"c"}};
        auto d = er::decide(er::target_of(r, 3, three, true, 0), 2, true);
        ROW("11 decline needs two options", d.returned == 2 && d.why == er::Why::kNeedsTwoOptions);
    }
    // 12. the game's availability check refuses the rule's option: the AI's pick.
    {
        er::Rule r; r.kind = er::Kind::kDecline;
        auto d = er::decide(er::target_of(r, 2, L, true, 0), 0, false);
        ROW("12 unavailable option keeps the AI", d.returned == 0 && d.why == er::Why::kOptionNotAvailable);
    }
    // 13. the picker returned -1 (nothing available): kept -1 whatever the rule.
    {
        er::Rule r; r.kind = er::Kind::kDecline;
        auto d = er::decide(er::target_of(r, 2, L, true, 0), -1, true);
        ROW("13 ai none kept", d.returned == -1 && d.why == er::Why::kAiPickedNone);
    }
    // 14. an option count past the bound (an unread or corrupt event).
    {
        er::Rule r;
        auto d = er::decide(er::target_of(r, 99, {}, true, 0), 0, true);
        ROW("14 option count bound", d.returned == 0 && d.why == er::Why::kOptionsUnread);
    }
    // 15. key checks: the event file spelling and the loc-key spelling.
    ROW("15a event key ok", er::event_key_ok("court_chaplain_task.0301"));
    ROW("15b event key refuses braces", !er::event_key_ok("a = { x }"));
    ROW("15c event key refuses empty", !er::event_key_ok(""));
    ROW("15d option key ok", er::option_key_ok("FACTION_DEMAND_REFUSE") && er::option_key_ok("court_chaplain_task.0301.a"));
    ROW("15e option key refuses space", !er::option_key_ok("a b"));
    // 16. the kill switch reads only "0" as off.
    ROW("16a kill switch 0", er::kill_switch_off("0"));
    ROW("16b kill switch unset", !er::kill_switch_off(nullptr) && !er::kill_switch_off("1") && !er::kill_switch_off(""));
    // 17. names round-trip.
    {
        er::Kind k; er::Currency c;
        ROW("17 names", er::kind_by_name("pay_if", &k) && k == er::Kind::kPayIf && er::currency_by_name("piety", &c) &&
                            c == er::Currency::kPiety && !er::kind_by_name("ai", &k));
    }
    // 18. the rule's text, as the record carries it.
    {
        er::Rule r; r.kind = er::Kind::kPayIf; r.amount_fp = 96 * FP;
        ROW("18 rule text", er::rule_text(r) == "pay_if gold>=96 option=0");
    }
    // 19. the rule named ACCEPT_CONVERT,
    //     the game did not offer it, the AI picked REFUSE (index 3).
    {
        const std::vector<er::OptionView> pop = {{"FACTION_DEMAND_POPULIST_ACCEPT_CONVERT"},
                                                 {"FACTION_DEMAND_POPULIST_ACCEPT_CONVERT_STATE_FAITH"},
                                                 {"FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE"},
                                                 {"FACTION_DEMAND_POPULIST_REFUSE"}};
        er::Rule r; r.option_key = "FACTION_DEMAND_POPULIST_ACCEPT_CONVERT";
        auto t = er::target_of(r, 4, pop, true, 0);
        auto d = er::decide(t, 3, /*available=*/false);
        auto f = er::with_else(d, 3, -1, false);
        ROW("19a not offered, no else: the AI's pick, not the rule's", f.d.returned == 3 && !f.rule_decided &&
                                                                        !f.fell_back_else && f.d.why == er::Why::kOptionNotAvailable);
        r.else_key = "FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE";
        const int ei = er::index_of(pop, r.else_key);
        auto g = er::with_else(d, 3, ei, true);
        ROW("19b not offered, else offered: the else option", ei == 2 && g.d.returned == 2 && g.fell_back_else &&
                                                              g.d.overridden && !g.rule_decided);
        auto h = er::with_else(d, 3, ei, false);
        ROW("19c else not offered either: the AI's pick", h.d.returned == 3 && !h.fell_back_else);
        auto ok = er::with_else(er::decide(t, 3, true), 3, ei, true);
        ROW("19d the rule's option offered: the rule decides", ok.rule_decided && ok.d.returned == 0 && !ok.fell_back_else);
        auto none = er::with_else(er::decide(t, -1, false), -1, ei, true);
        ROW("19e the AI picked none: nothing is applied", none.d.returned == -1 && !none.fell_back_else);
        ROW("19f rule text names the else", er::rule_text(r) ==
            "option option=FACTION_DEMAND_POPULIST_ACCEPT_CONVERT else=FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE");
    }
    // 20. pay_if on the three-option claim letter court_chaplain_task.0302 (.a county claim, .b duchy claim,
    // court_chaplain_task.0301.b decline): the else option is taken when the gold does not cover the amount
    {
        const std::vector<er::OptionView> c3 = {{"court_chaplain_task.0302.a"}, {"court_chaplain_task.0302.b"},
                                                {"court_chaplain_task.0301.b"}};
        er::Rule r; r.kind = er::Kind::kPayIf; r.amount_fp = 91 * FP; r.option_key = "court_chaplain_task.0302.a";
        r.else_key = "court_chaplain_task.0301.b";
        auto low = er::target_of(r, 3, c3, true, 4885 * FP / 100);
        auto dl = er::decide(low, 0, true);
        ROW("20a 0302 pay_if at 48.85 gold: the decline", !low.covered && dl.returned == 2 && dl.overridden);
        auto hi = er::target_of(r, 3, c3, true, 120 * FP);
        ROW("20b 0302 pay_if at 120 gold: the county claim", hi.covered && er::decide(hi, 2, true).returned == 0);
        er::Rule n = r; n.else_key.clear();
        ROW("20c three options and no else: needs_two_options",
            er::target_of(n, 3, c3, true, 0).why == er::Why::kNeedsTwoOptions);
        er::Rule bad = r; bad.else_key = "court_chaplain_task.0302.c";
        ROW("20d an else not in the event", er::target_of(bad, 3, c3, true, 0).why == er::Why::kOptionNotInEvent);
        ROW("20e keys unread", er::target_of(r, 3, {}, true, 0).why == er::Why::kOptionsUnread);
        er::Rule two; two.kind = er::Kind::kPayIf; two.amount_fp = 96 * FP; two.else_key = "court_chaplain_task.0301.a";
        ROW("20f two options: still the other option", er::target_of(two, 2, L, true, 40 * FP).index == 1);
    }
    // 21. the dead ruler's rules carried to his heir (1072.07.16: Murchad's 0301 and 0302 rules)
    {
        er::Rule a; a.kind = er::Kind::kPayIf; a.amount_fp = 91 * FP; a.option_key = "court_chaplain_task.0301.a";
        er::Rule b = a; b.option_key = "court_chaplain_task.0302.a"; b.else_key = "court_chaplain_task.0301.b";
        er::Rule own; own.kind = er::Kind::kOption; own.option_key = "court_chaplain_task.0301.b";
        const std::vector<std::pair<std::string, er::Rule>> from = {{"court_chaplain_task.0301", a},
                                                                     {"court_chaplain_task.0302", b}};
        std::vector<std::pair<std::string, er::Rule>> to;
        ROW("21a an empty heir table takes both, in order", er::carry_rules(from, to) == 2 && to.size() == 2 &&
            to[0].first == "court_chaplain_task.0301" && to[1].second.else_key == "court_chaplain_task.0301.b");
        std::vector<std::pair<std::string, er::Rule>> to2 = {{"court_chaplain_task.0301", own}};
        ROW("21b the heir's own rule for the same event is kept; the other is added",
            er::carry_rules(from, to2) == 1 && to2.size() == 2 && to2[0].second.kind == er::Kind::kOption &&
            to2[1].first == "court_chaplain_task.0302");
        ROW("21c carried twice adds nothing", er::carry_rules(from, to2) == 0 && to2.size() == 2);
        std::vector<std::pair<std::string, er::Rule>> full;
        for (size_t i = 0; i < er::kMaxRulesPerChar; ++i) full.emplace_back("e" + std::to_string(i), own);
        ROW("21d a full table takes none", er::carry_rules(from, full) == 0 && full.size() == er::kMaxRulesPerChar);
        std::vector<std::pair<std::string, er::Rule>> none;
        ROW("21e CONTROL nothing to carry", er::carry_rules({}, none) == 0 && none.empty());
    }
    std::printf("%d rows, %d failed\n", g_rows, g_fail);
    return g_fail ? 1 : 0;
}
