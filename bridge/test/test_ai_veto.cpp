// test_ai_veto.cpp — the veto table, the hook's decision and the attempt ring
// (bridge/include/ai_veto.h), engine-free.
//
// Ids from a test run: the count 29459 created d_ryazan into debt within a year;
// 33963 (d_chudia) is the control with no rule.
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>
#include "ai_veto.h"

namespace av = bridge::ai_veto;

static int g_fail = 0, g_rows = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        ++g_rows;                                                           \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

int main() {
    const uint32_t RYAZAN = 29459, CHUDIA = 33963, GEN = 0x5A000000u;
    const int CT = static_cast<int>(av::What::kCreateTitle), LAW = static_cast<int>(av::What::kLaw);

    // 1. names and the kill switch
    ROW("1a what names round-trip", av::what_by_name("create_title") == CT && av::what_by_name("law") == LAW &&
                                    av::what_by_name("mercenary") == 2 && av::what_by_name("construction") == 3);
    ROW("1b an unknown what is none", av::what_by_name("siege") == av::kNone && av::what_by_name("") == av::kNone);
    ROW("1c kill switch: only exactly 0 is off", av::kill_switch_off("0") && !av::kill_switch_off(nullptr) &&
                                                 !av::kill_switch_off("") && !av::kill_switch_off("00") &&
                                                 !av::kill_switch_off("1"));

    av::Table t;
    int slot = 7;
    // 2. empty table: everything passes
    ROW("2a empty table passes", av::decide(t, CT, 7, RYAZAN, &slot) == av::Verdict::kPass && slot == -1);

    // 3. a watched ruler with no rule: recorded, passed
    ROW("3a watch takes a slot", t.watch(RYAZAN) == 0 && t.count.load() == 1);
    ROW("3b watching twice is the same slot", t.watch(RYAZAN) == 0 && t.count.load() == 1);
    ROW("3c no rule: record, not drop", av::decide(t, CT, 7, RYAZAN, &slot) == av::Verdict::kRecord && slot == 0);

    // 4. the rule
    ROW("4a set_rule on a watched ruler", t.set_rule(RYAZAN, CT, true));
    ROW("4b his AI create_title post (flags 7) drops", av::decide(t, CT, 7, RYAZAN, &slot) == av::Verdict::kDrop);
    ROW("4c the full id's generation byte is ignored", av::decide(t, CT, 7, RYAZAN | GEN, &slot) == av::Verdict::kDrop);
    ROW("4d another what of his is recorded only", av::decide(t, LAW, 7, RYAZAN, &slot) == av::Verdict::kRecord);
    ROW("4e flags 0xe (the bridge's own law post) passes", av::decide(t, CT, 0xe, RYAZAN, &slot) == av::Verdict::kPass);
    ROW("4f a command of none of the four passes", av::decide(t, av::kNone, 7, RYAZAN, &slot) == av::Verdict::kPass);
    ROW("4g the control ruler (not watched) passes", av::decide(t, CT, 7, CHUDIA, &slot) == av::Verdict::kPass);
    ROW("4h set_rule on an unwatched ruler is refused", !t.set_rule(CHUDIA, CT, true));
    ROW("4i set_rule with a bad what is refused", !t.set_rule(RYAZAN, av::kWhatCount, true) && !t.set_rule(RYAZAN, -1, true));

    // 5. clear
    ROW("5a clearing the rule", t.set_rule(RYAZAN, CT, false));
    ROW("5b after clear: recorded, passed", av::decide(t, CT, 7, RYAZAN, &slot) == av::Verdict::kRecord);
    ROW("5c unwatch frees the slot", t.unwatch(RYAZAN) && t.count.load() == 0 && t.find(RYAZAN) == -1);
    ROW("5d unwatched: passes", av::decide(t, CT, 7, RYAZAN, &slot) == av::Verdict::kPass);
    t.set_rule(RYAZAN, CT, true);
    ROW("5e a rule cannot outlive unwatch", av::decide(t, CT, 7, RYAZAN, &slot) == av::Verdict::kPass);
    ROW("5f a re-watched slot starts with no rule and zero counts",
        t.watch(RYAZAN) >= 0 && av::decide(t, CT, 7, RYAZAN, &slot) == av::Verdict::kRecord &&
        t.slot[slot].dropped[CT].load() == 0);

    // 6. the table is bounded
    av::Table full;
    for (uint32_t i = 1; i <= av::kMaxRulers; ++i) full.watch(1000 + i);
    ROW("6a a 17th ruler is refused", full.watch(5000) == -1 && full.count.load() == av::kMaxRulers);
    ROW("6b id 0 is never watched", full.watch(0) == -1 && t.watch(0) == -1);

    // 7. the ring
    av::Ring ring;
    ROW("7a empty ring", ring.last_seq() == 0 && ring.first_seq() == 1);
    av::Record r; r.ruler = RYAZAN; r.what = CT; r.dropped = true; r.object_id = 200;
    const uint64_t s1 = ring.push(r);
    av::Record got;
    ROW("7b push then get", s1 == 1 && ring.get(1, &got) && got.object_id == 200 && got.dropped && got.priced == 0);
    got.priced = 1; got.cost_gold_fp = 125LL * 100000; av::copy_text(got.key, sizeof(got.key), "d_ryazan");
    ROW("7c the drain writes the price back", ring.put_priced(got) && ring.get(1, &got) && got.priced == 1 &&
                                             std::string_view(got.key) == "d_ryazan");
    for (int i = 0; i < av::kRingCap; ++i) ring.push(r);
    ROW("7d an overwritten seq is gone", !ring.get(1, &got) && ring.last_seq() == 1 + av::kRingCap &&
                                         ring.first_seq() == 2);
    got.seq = 1; got.priced = 1;
    ROW("7e a stale price is not written over a newer record", !ring.put_priced(got) && ring.get(2, &got) && got.priced == 0);
    ROW("7f seq 0 is never a record", !ring.get(0, &got));
    char small[4];
    av::copy_text(small, sizeof(small), "d_meath");
    ROW("7g copy_text truncates and terminates", std::string_view(small) == "d_m");

    // 8. concurrent pushes keep every seq
    av::Ring cring;
    std::vector<std::thread> th;
    for (int k = 0; k < 4; ++k) th.emplace_back([&] { for (int i = 0; i < 25; ++i) cring.push(r); });
    for (auto& x : th) x.join();
    bool all = cring.last_seq() == 100;
    for (uint64_t s = 1; s <= 100; ++s) all = all && cring.get(s, &got) && got.seq == s;
    ROW("8a four threads, 100 pushes, 100 distinct seqs held", all);

    // 9. the two new kinds
    const int ALLY = static_cast<int>(av::What::kAllyCall), PERK = static_cast<int>(av::What::kPerk);
    ROW("9a ally_call and perk names round-trip", av::what_by_name("ally_call") == ALLY &&
                                                  av::what_by_name("perk") == PERK && av::kWhatCount >= 6);   // both kinds are in the table
    ROW("9b an ally call's ruler is the interaction's actor at cmd+0x2f8; the others at +0x20",
        av::ruler_offset(ALLY) == 0x2f8 && av::ruler_offset(PERK) == 0x20 && av::ruler_offset(CT) == 0x20);

    // 10. who posted (re_interaction_command.md §3): the dispatch call's return address.
    // AI_INTERACTION_CORE posts from image 0x4dc49f9 (header = image - 0x100000); the base is the
    // game's load address, the bridge's .so somewhere above it.
    const uintptr_t GAME = 0x400000, GAME_END = GAME + 0xC800000, SELF = 0x7f12a0000000ULL, SELF_END = SELF + 0x3f5000;
    const uintptr_t AI_RA = GAME + (0x4dc49f9 - 0x100000) + 5, BR_RA = SELF + 0x1a2b3c, LIB_RA = 0x7f12b0001000ULL;
    ROW("10a an AI site returns into the game image",
        av::classify_origin(AI_RA, GAME, GAME_END, SELF, SELF_END) == av::Origin::kGame);
    ROW("10b the bridge's call returns into libck3_bridge.so",
        av::classify_origin(BR_RA, GAME, GAME_END, SELF, SELF_END) == av::Origin::kBridge);
    ROW("10c a third library is other",
        av::classify_origin(LIB_RA, GAME, GAME_END, SELF, SELF_END) == av::Origin::kOther);
    ROW("10d an unread self range never reads as the bridge, an unread image never as the game",
        av::classify_origin(BR_RA, GAME, GAME_END, 0, 0) == av::Origin::kOther &&
        av::classify_origin(AI_RA, 0, 0, SELF, SELF_END) == av::Origin::kOther);
    ROW("10e the image end is outside it", av::classify_origin(GAME_END, GAME, GAME_END, SELF, SELF_END) != av::Origin::kGame);
    ROW("10f the marker agrees: game without it, bridge with it; other never",
        av::origin_agrees(av::Origin::kGame, false) && av::origin_agrees(av::Origin::kBridge, true) &&
        !av::origin_agrees(av::Origin::kGame, true) && !av::origin_agrees(av::Origin::kBridge, false) &&
        !av::origin_agrees(av::Origin::kOther, false) && !av::origin_agrees(av::Origin::kOther, true));
    ROW("10g only a post both tests call the game's is the AI's",
        !av::bridge_own(av::Origin::kGame, false) && av::bridge_own(av::Origin::kGame, true) &&
        av::bridge_own(av::Origin::kBridge, false) && av::bridge_own(av::Origin::kOther, false));

    // 11. the veto and the bridge's own send
    av::Table t2;
    const uint32_t MURCHAD = 31506, CONTROL = 33963;
    t2.watch(MURCHAD); t2.set_rule(MURCHAD, ALLY, true); t2.watch(CONTROL);
    ROW("11a the AI's ally call for a vetoed ruler drops", av::decide(t2, ALLY, 7, MURCHAD, &slot, false) == av::Verdict::kDrop);
    ROW("11b the bridge's own call for him is recorded, not dropped",
        av::decide(t2, ALLY, 7, MURCHAD, &slot, true) == av::Verdict::kRecord && slot >= 0);
    ROW("11c the control's AI call is recorded, passed", av::decide(t2, ALLY, 7, CONTROL, &slot, false) == av::Verdict::kRecord);
    ROW("11d a perk veto drops only his AI's perk post",
        t2.set_rule(MURCHAD, PERK, true) && av::decide(t2, PERK, 7, MURCHAD, &slot) == av::Verdict::kDrop &&
        av::decide(t2, PERK, 7, CONTROL, &slot) == av::Verdict::kRecord && av::decide(t2, LAW, 7, MURCHAD, &slot) == av::Verdict::kRecord);

    // 12. a retrying AI: one record a window per object
    const int32_t D0 = 1000000, W = 30 * 24;
    ROW("12a the first drop is recorded", av::record_drop(0, 0, 0x5000, D0, W));
    ROW("12b the same object within the window is counted only", !av::record_drop(0x5000, D0, 0x5000, D0 + W - 1, W));
    ROW("12c after the window it is recorded again", av::record_drop(0x5000, D0, 0x5000, D0 + W, W));
    ROW("12d another object is recorded at once", av::record_drop(0x5000, D0, 0x6000, D0 + 1, W));
    ROW("12e an unread date is recorded; a date that went back is recorded",
        av::record_drop(0x5000, D0, 0x5000, 0, W) && av::record_drop(0x5000, D0, 0x5000, D0 - 5, W));

    // 13. the interaction census
    av::Census cs;
    cs.add(0xA1, MURCHAD, D0, 0x3cc49fe); cs.add(0xA1, MURCHAD, D0 + 24, 0x3cc49fe); cs.add(0xA2, MURCHAD, D0, 1);
    cs.add(0xA1, CONTROL, D0, 1);
    av::CensusEntry ce[8];
    const int nce = cs.copy(ce, 8);
    ROW("13a counted by (ruler, interaction)", nce == 3 && ce[0].count == 2 && ce[0].first_date == D0 &&
                                               ce[0].last_date == D0 + 24 && ce[1].count == 1 && ce[2].ruler == CONTROL);
    av::Census full_cs;
    for (int i = 0; i < av::kCensusCap; ++i) full_cs.add(0x100 + i, 1, D0, 0);
    ROW("13b a full census counts the overflow, never grows", !full_cs.add(0x9999, 1, D0, 0) && full_cs.overflow() == 1 &&
                                                              full_cs.add(0x100, 1, D0, 0));
    // 13c-e: in another session, 10 rulers filled 96 pairs in five game-years; the cap is 512 and the pairs past it are counted
    ROW("13c the cap holds 10 rulers x 50 interaction types", av::kCensusCap >= 500);
    full_cs.add(0x9999, 1, D0 + 1, 0); full_cs.add(0x9998, 2, D0, 0); full_cs.add(0x9998, 2, D0 + 1, 0);
    ROW("13d the pairs not shown are counted once each, per ruler and in all",
        full_cs.overflow() == 4 && full_cs.pairs_not_shown() == 2 && full_cs.pairs_not_shown_for(1) == 1 &&
        full_cs.pairs_not_shown_for(2) == 1 && full_cs.pairs_not_shown_for(0) == 2 && full_cs.pairs_not_shown_exact());
    for (int i = 0; i < av::kOverflowKeys; ++i) full_cs.add(0x20000 + i, 3, D0, 0);
    ROW("13e past the key table the count is a floor, said so",
        full_cs.pairs_not_shown() == av::kOverflowKeys && !full_cs.pairs_not_shown_exact());
    ROW("13f a census with room says nothing is cut", cs.overflow() == 0 && cs.pairs_not_shown() == 0 &&
                                                      cs.pairs_not_shown_exact());

    // 14. the decision kind (CExecuteDecisionCommand; Domnall 36124's AI took
    //     convert_to_local_culture_decision for 1,000 prestige in both run-eight arms)
    const int DEC = static_cast<int>(av::What::kDecision);
    const uint32_t DOMNALL = 36124;
    ROW("14a decision is a kind with its own name", DEC == 6 && av::kWhatCount >= 7 &&
                                                   av::what_by_name("decision") == DEC &&
                                                   std::string_view(av::what_name(DEC)) == "decision");
    ROW("14b its ruler is the command's +0x20, its decision type +0x28", av::ruler_offset(DEC) == 0x20 &&
                                                                        av::kDecisionTypeOffset == 0x28);
    av::Table t3;
    t3.watch(DOMNALL); t3.watch(CONTROL);
    ROW("14c no rule: his AI's decision is recorded, passed", av::decide(t3, DEC, 7, DOMNALL, &slot) == av::Verdict::kRecord);
    ROW("14d the rule with no keys drops every decision of his AI",
        t3.set_decision_keys(DOMNALL, nullptr, 0) && t3.set_rule(DOMNALL, DEC, true) &&
        av::decision_key_matches(t3.slot[t3.find(DOMNALL)], "stress_loss_drunkard_decision", 29) == av::KeyMatch::kYes &&
        av::decide(t3, DEC, 7, DOMNALL, &slot, false, true) == av::Verdict::kDrop);
    ROW("14e the seat's own take_decision (the marker) is recorded, never dropped",
        av::decide(t3, DEC, 7, DOMNALL, &slot, true, true) == av::Verdict::kRecord);
    ROW("14f the player's decision window (flags 0xe) passes", av::decide(t3, DEC, 0xe, DOMNALL, &slot) == av::Verdict::kPass);
    ROW("14g the control's AI decision is recorded only", av::decide(t3, DEC, 7, CONTROL, &slot) == av::Verdict::kRecord);
    // keys
    const char* only[] = {"convert_to_local_culture_decision"};
    ROW("14h a keyed rule: the named decision falls under it",
        t3.set_decision_keys(DOMNALL, only, 1) &&
        av::decision_key_matches(t3.slot[t3.find(DOMNALL)], "convert_to_local_culture_decision", 33) == av::KeyMatch::kYes);
    ROW("14i a keyed rule: another decision does not (recorded, passed)",
        av::decision_key_matches(t3.slot[t3.find(DOMNALL)], "stress_loss_drunkard_decision", 29) == av::KeyMatch::kNo &&
        av::decide(t3, DEC, 7, DOMNALL, &slot, false, false) == av::Verdict::kRecord);
    ROW("14j a keyed rule: an unread key passes",
        av::decision_key_matches(t3.slot[t3.find(DOMNALL)], nullptr, 0) == av::KeyMatch::kNo);
    ROW("14k a prefix is not the key", av::decision_key_matches(t3.slot[t3.find(DOMNALL)], "convert_to_local_culture", 24) ==
                                       av::KeyMatch::kNo);
    {
        av::Slot& sl = t3.slot[t3.find(DOMNALL)];
        sl.keys_ver.fetch_add(1);   // a writer mid-way
        ROW("14l a list being rewritten passes the post",
            av::decision_key_matches(sl, "convert_to_local_culture_decision", 33) == av::KeyMatch::kNo);
        sl.keys_ver.fetch_add(1);
    }
    const char* nine[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
    const char* empty_key[] = {""};
    ROW("14m too many keys, an empty key, an unwatched ruler: refused",
        !t3.set_decision_keys(DOMNALL, nine, 9) && !t3.set_decision_keys(DOMNALL, empty_key, 1) &&
        !t3.set_decision_keys(29459, only, 1));
    ROW("14n kinds are independent: the decision rule leaves his laws recorded only",
        av::decide(t3, LAW, 7, DOMNALL, &slot) == av::Verdict::kRecord);
    // the origin rule for decisions
    ROW("14o a decision's origin is the marker's; the game image is the only agreeing address",
        av::decision_origin(av::Origin::kGame, true) == av::Origin::kBridge &&
        av::decision_origin(av::Origin::kGame, false) == av::Origin::kGame &&
        av::decision_agrees(av::Origin::kGame) && !av::decision_agrees(av::Origin::kBridge) &&
        !av::decision_agrees(av::Origin::kOther));
    // the comma list set_ai_veto takes
    char ks[av::kMaxDecisionKeys][av::kDecisionKeyCap] = {};
    ROW("14p split: two keys, spaces ignored",
        av::split_decision_keys("convert_to_local_culture_decision, stress_loss_drunkard_decision", ks) == 2 &&
        std::string_view(ks[0]) == "convert_to_local_culture_decision" &&
        std::string_view(ks[1]) == "stress_loss_drunkard_decision");
    char ks2[av::kMaxDecisionKeys][av::kDecisionKeyCap] = {};
    ROW("14q split: empty is none; a trailing comma, an upper-case letter, nine keys are refused",
        av::split_decision_keys("", ks2) == 0 && av::split_decision_keys("a,", ks2) == -1 &&
        av::split_decision_keys("Convert", ks2) == -1 && av::split_decision_keys("a,b,c,d,e,f,g,h,i", ks2) == -1 &&
        av::split_decision_keys("a,,b", ks2) == -1);
    ROW("14r clearing the rule: his AI's decision is recorded, passed",
        t3.set_rule(DOMNALL, DEC, false) && av::decide(t3, DEC, 7, DOMNALL, &slot, false, true) == av::Verdict::kRecord);

    // 15. vassal_offer — another ruler's AI offers vassalage TO the watched ruler (a seated game: Murchad
    //     became Munster's vassal 1075.04.02 with no war; offer_vassalization has no reply delay, so his AI
    //     answered inside Munster's send). The watched ruler is the command's RECIPIENT (cmd+0x2fc).
    const int VO = static_cast<int>(av::What::kVassalOffer);
    ROW("15a vassal_offer is a kind with its own name", VO == 7 && av::kWhatCount >= 8 &&
                                                       av::what_by_name("vassal_offer") == VO &&
                                                       std::string_view(av::what_name(VO)) == "vassal_offer");
    ROW("15b its watched ruler is the RECIPIENT (cmd+0x2fc), not the actor (cmd+0x2f8)",
        av::ruler_offset(VO) == 0x2fc && av::kInteractionRecipientOffset == 0x2fc &&
        av::ruler_offset(static_cast<int>(av::What::kAllyCall)) == 0x2f8);
    av::Table t4;
    t4.watch(MURCHAD); t4.watch(CONTROL);
    ROW("15c no rule: an offer to him is recorded, passed (the witness of how he became a vassal)",
        av::decide(t4, VO, 7, MURCHAD, &slot) == av::Verdict::kRecord);
    ROW("15d the rule drops an AI's offer to him", t4.set_rule(MURCHAD, VO, true) &&
                                                  av::decide(t4, VO, 7, MURCHAD, &slot) == av::Verdict::kDrop);
    ROW("15e an offer to the control is recorded only", av::decide(t4, VO, 7, CONTROL, &slot) == av::Verdict::kRecord);
    ROW("15f the rule leaves his ally calls alone",
        av::decide(t4, static_cast<int>(av::What::kAllyCall), 7, MURCHAD, &slot) == av::Verdict::kRecord);

    // 16. the handoff carries the dead ruler's rules to his heir at once (1069.09.27: the AI
    //     passed crown_authority_1 for Brian on his first day, before the toolkit set the vetoes again).
    const uint32_t BRIAN = 36303;
    av::Table t5;
    t5.watch(MURCHAD);
    const char* k5[] = {"convert_to_local_culture_decision"};
    t5.set_rule(MURCHAD, LAW, true); t5.set_rule(MURCHAD, DEC, true); t5.set_decision_keys(MURCHAD, k5, 1);
    ROW("16a CONTROL before the carry the heir's law post passes",
        av::decide(t5, LAW, 7, BRIAN, &slot) == av::Verdict::kPass);
    const uint32_t m5 = t5.carry(MURCHAD, BRIAN);
    ROW("16b the carry returns the mask and the heir's law post is dropped",
        m5 == ((1u << LAW) | (1u << DEC)) && av::decide(t5, LAW, 7, BRIAN, &slot) == av::Verdict::kDrop);
    const int b5 = t5.find(BRIAN);
    ROW("16c the decision keys go with it",
        b5 >= 0 && t5.slot[b5].key_count.load() == 1 &&
        std::string_view(t5.slot[b5].keys[0]) == "convert_to_local_culture_decision");
    ROW("16d an unwatched or ruleless predecessor carries nothing",
        t5.carry(4242, BRIAN + 1) == 0 && t5.find(BRIAN + 1) < 0 && t5.carry(BRIAN, BRIAN) == 0);

    // 17. the watched ruler's own interaction is an ally_call or (hire_laamp_mercenaries_interaction) a
    //     mercenary hire, under the mercenary veto; any other interaction, or a type not resolved, is none.
    const uint64_t ALLY_T = 0x7f0000001000ull, LAAMP_T = 0x7f0000002000ull, OTHER_T = 0x7f0000003000ull;
    const int MERC = static_cast<int>(av::What::kMercenary), ALLYC = static_cast<int>(av::What::kAllyCall);
    ROW("17a the adventurer's hire is a mercenary hire",
        av::actor_interaction_what(LAAMP_T, ALLY_T, LAAMP_T) == MERC && av::actor_interaction_what(ALLY_T, ALLY_T, LAAMP_T) == ALLYC);
    ROW("17b CONTROL another interaction, or an unresolved type, is none",
        av::actor_interaction_what(OTHER_T, ALLY_T, LAAMP_T) == av::kNone && av::actor_interaction_what(LAAMP_T, ALLY_T, 0) == av::kNone &&
        av::actor_interaction_what(0, 0, 0) == av::kNone);
    av::Table t6;
    t6.watch(MURCHAD);
    t6.set_rule(MURCHAD, MERC, true);
    ROW("17c a vetoed mercenary kind drops the adventurer's hire as it drops the command",
        av::decide(t6, av::actor_interaction_what(LAAMP_T, ALLY_T, LAAMP_T), 7, MURCHAD, &slot) == av::Verdict::kDrop);

    // 18. men_at_arms, the ninth kind (the AI bought a mangonel regiment for Murchad, about 66 gold)
    const int MAA = static_cast<int>(av::What::kMenAtArms);
    ROW("18a men_at_arms round-trips and is the ninth kind",
        MAA == 8 && av::kWhatCount >= 9 && av::what_by_name("men_at_arms") == MAA &&
        std::string_view(av::what_name(MAA)) == "men_at_arms");
    ROW("18b a new regiment's ruler is its owning character at cmd+0x28 (CCreateMAARegimentCommand's ctor store)",
        av::ruler_offset(MAA) == 0x28 && av::kMaaCreateTypeOffset == 0x2c);
    ROW("18c CONTROL the other kinds keep their ruler offsets",
        av::ruler_offset(CT) == 0x20 && av::ruler_offset(MERC) == 0x20 && av::ruler_offset(ALLYC) == av::kInteractionActorOffset);
    ROW("18d more men of a regiment: the command names the regiment (+0x24), the owner is CRegiment+0x124",
        av::kMaaAddRegimentOffset == 0x24 && av::kMaaAddTypeOffset == 0x28 && av::kRegimentOwnerCharOffset == 0x124);
    av::Table t7;
    t7.watch(MURCHAD);
    t7.set_rule(MURCHAD, MAA, true);
    ROW("18e a men_at_arms veto drops his AI's purchase, and only that kind",
        av::decide(t7, MAA, 7, MURCHAD, &slot) == av::Verdict::kDrop &&
        av::decide(t7, MERC, 7, MURCHAD, &slot) == av::Verdict::kRecord);
    ROW("18f the ninth bit carries to the heir with the others",
        (t7.carry(MURCHAD, BRIAN) & (1u << MAA)) != 0 && av::decide(t7, MAA, 7, BRIAN, &slot) == av::Verdict::kDrop);

    // 19. war, the tenth kind: the watched ruler's own AI posting declare_war_interaction
    //     (ai_war_post_decl 0x04e55c10: special slot 0xd, BuildInteractionCmd with the ruler as actor, dispatch 7)
    const int WAR = static_cast<int>(av::What::kWar);
    const uint64_t WAR_T = 0x7f0000004000ull;
    ROW("19a war round-trips and is the tenth kind",
        WAR == 9 && av::kWhatCount >= 10 && av::what_by_name("war") == WAR &&
        std::string_view(av::what_name(WAR)) == "war");
    ROW("19b his AI's declare_war_interaction is a war; its ruler is the interaction's actor",
        av::actor_interaction_what(WAR_T, ALLY_T, LAAMP_T, WAR_T) == WAR &&
        av::ruler_offset(WAR) == av::kInteractionActorOffset);
    ROW("19c CONTROL unresolved, or another interaction, is not a war; the other interactions keep their kinds",
        av::actor_interaction_what(WAR_T, ALLY_T, LAAMP_T, 0) == av::kNone &&
        av::actor_interaction_what(OTHER_T, ALLY_T, LAAMP_T, WAR_T) == av::kNone &&
        av::actor_interaction_what(ALLY_T, ALLY_T, LAAMP_T, WAR_T) == ALLYC &&
        av::actor_interaction_what(LAAMP_T, ALLY_T, LAAMP_T, WAR_T) == MERC);
    av::Table t8;
    t8.watch(MURCHAD);
    t8.set_rule(MURCHAD, WAR, true);
    ROW("19d a war veto drops his AI's declaration, and only that kind",
        av::decide(t8, WAR, 7, MURCHAD, &slot) == av::Verdict::kDrop &&
        av::decide(t8, ALLYC, 7, MURCHAD, &slot) == av::Verdict::kRecord &&
        av::decide(t8, MAA, 7, MURCHAD, &slot) == av::Verdict::kRecord);
    ROW("19e the seat's own declaration is never dropped: a bridge post (the marker) is recorded",
        av::decide(t8, WAR, 7, MURCHAD, &slot, av::bridge_own(av::Origin::kGame, true)) == av::Verdict::kRecord &&
        av::decide(t8, WAR, 7, MURCHAD, &slot, av::bridge_own(av::Origin::kGame, false)) == av::Verdict::kDrop);
    ROW("19f CONTROL another ruler's declaration passes; the player's flags (0xe) pass",
        av::decide(t8, WAR, 7, BRIAN, &slot) == av::Verdict::kPass &&
        av::decide(t8, WAR, 0xe, MURCHAD, &slot) == av::Verdict::kPass);
    ROW("19g the tenth bit carries to the heir",
        (t8.carry(MURCHAD, BRIAN) & (1u << WAR)) != 0 && av::decide(t8, WAR, 7, BRIAN, &slot) == av::Verdict::kDrop);
    // An unclassified class's post is counted by vtable for a watched ruler (the word at +0x20), never for another
    ROW("24a an unclassified post names the watched ruler at +0x20 (24-bit), and no one for an unwatched or -1 word",
        av::unclassified_ruler(0x2000000u | MURCHAD, true) == MURCHAD && av::unclassified_ruler(MURCHAD, false) == 0 &&
        av::unclassified_ruler(0xFFFFFFFFu, true) == 0 && av::kCmdRulerOffset == 0x20);
    av::Census uc;
    uc.add(0x0bc62c40, MURCHAD, D0, 0x10);
    uc.add(0x0bc62c40, MURCHAD, D0 + 1, 0x11);
    uc.add(0x0bbf6f80, MURCHAD, D0 + 2, 0x12);
    av::CensusEntry ue[4];
    const int un = uc.copy(ue, 4);
    ROW("24b the census keys a vtable per ruler: two classes, the repeated one counted twice with its last date",
        un == 2 && ue[0].itype == 0x0bc62c40 && ue[0].count == 2 && ue[0].last_date == D0 + 1 && ue[1].count == 1);

    // 21. court_type, the eleventh kind: the AI's CSetCourtTypeSettingCommand (500 prestige), posted by
    //     its court pass FUN_04dc0770 with flags 7, the ruler at +0x20 (ctor FUN_067d42d0); 12015 on
    //     0867.03.23, the same day as crown_authority_1 (792), prestige 986 -> -305
    const int CTY = static_cast<int>(av::What::kCourtType);
    ROW("21a court_type round-trips and is the eleventh kind",
        CTY == 10 && av::kWhatCount >= 11 && av::what_by_name("court_type") == CTY &&
        std::string_view(av::what_name(CTY)) == "court_type");
    ROW("21b its ruler is the command's own +0x20 (no interaction, no regiment)",
        av::ruler_offset(CTY) == av::kCmdRulerOffset && av::kCmdRulerOffset == 0x20);
    av::Table t9;
    t9.watch(MURCHAD);
    t9.set_rule(MURCHAD, CTY, true);
    ROW("21c a court_type veto drops his AI's change, and only that kind",
        av::decide(t9, CTY, 7, MURCHAD, &slot) == av::Verdict::kDrop &&
        av::decide(t9, LAW, 7, MURCHAD, &slot) == av::Verdict::kRecord &&
        av::decide(t9, WAR, 7, MURCHAD, &slot) == av::Verdict::kRecord);
    ROW("21d CONTROL the bridge's own post and the player's flags pass; another ruler passes",
        av::decide(t9, CTY, 7, MURCHAD, &slot, av::bridge_own(av::Origin::kBridge, true)) == av::Verdict::kRecord &&
        av::decide(t9, CTY, 0xe, MURCHAD, &slot) == av::Verdict::kPass &&
        av::decide(t9, CTY, 7, BRIAN, &slot) == av::Verdict::kPass);
    ROW("21e the eleventh bit carries to the heir",
        (t9.carry(MURCHAD, BRIAN) & (1u << CTY)) != 0 && av::decide(t9, CTY, 7, BRIAN, &slot) == av::Verdict::kDrop);
    ROW("21f the bits still fit the mask word", av::kWhatCount <= 32);

    // 22. faction, the twelfth kind: the AI's CCreateFactionCommand (FUN_04ddea80) and CJoinFactionCommand
    //     (FUN_04dde690), flags 7, the ruler at +0x24 (ctors FUN_05f99b80, FUN_05f99440); and a claimant faction's
    //     interaction sent by the ruler (FUN_04ddf6d0). The seat's own faction calls dispatch nothing (factions.cpp).
    const int FAC = static_cast<int>(av::What::kFaction);
    const uint32_t GOTTFRIED = 11030;
    ROW("22a faction round-trips and is the twelfth kind",
        FAC == 11 && av::kWhatCount >= 12 && av::what_by_name("faction") == FAC &&
        std::string_view(av::what_name(FAC)) == "faction");
    ROW("22b its ruler is the command's +0x24 (the +0x20 word is a type index or a faction id); the target +0x28",
        av::ruler_offset(FAC) == 0x24 && av::kFactionCmdActorOffset == 0x24 && av::kFactionCmdObjectOffset == 0x20 &&
        av::kFactionCreateTargetOffset == 0x28);
    ROW("22c CONTROL the other kinds keep their ruler offsets",
        av::ruler_offset(CTY) == 0x20 && av::ruler_offset(WAR) == av::kInteractionActorOffset && av::ruler_offset(MAA) == 0x28);
    const uint64_t CLAIM_T = 0x7f0000005000ull, CER_T = 0x7f0000006000ull;
    const uint64_t fi[av::kFactionInteractionCount] = {CLAIM_T, CER_T, 0, 0};
    ROW("22d a faction interaction is one of the resolved keys; another, or 0, is not",
        av::faction_interaction(CLAIM_T, fi, 4) && av::faction_interaction(CER_T, fi, 4) &&
        !av::faction_interaction(OTHER_T, fi, 4) && !av::faction_interaction(0, fi, 4) &&
        !av::faction_interaction(CLAIM_T, fi, 0) && av::actor_interaction_what(CLAIM_T, ALLY_T, LAAMP_T, WAR_T) == av::kNone);
    ROW("22e the four keys name the claimant interactions of common/factions",
        std::string_view(av::kFactionInteractionKeys[0]) == "create_claimant_faction_against_interaction" &&
        std::string_view(av::kFactionInteractionKeys[1]) == "create_ceremonial_claimant_faction_against_interaction");
    av::Table t10;
    t10.watch(GOTTFRIED);
    ROW("22f no rule: his AI's join is recorded, passed", av::decide(t10, FAC, 7, GOTTFRIED, &slot) == av::Verdict::kRecord);
    t10.set_rule(GOTTFRIED, FAC, true);
    ROW("22g a faction veto drops his AI's post, and only that kind",
        av::decide(t10, FAC, 7, GOTTFRIED | GEN, &slot) == av::Verdict::kDrop &&
        av::decide(t10, WAR, 7, GOTTFRIED, &slot) == av::Verdict::kRecord &&
        av::decide(t10, CTY, 7, GOTTFRIED, &slot) == av::Verdict::kRecord);
    ROW("22h CONTROL a bridge post (the marker), the player's flags and another ruler pass",
        av::decide(t10, FAC, 7, GOTTFRIED, &slot, av::bridge_own(av::Origin::kGame, true)) == av::Verdict::kRecord &&
        av::decide(t10, FAC, 0xe, GOTTFRIED, &slot) == av::Verdict::kPass &&
        av::decide(t10, FAC, 7, BRIAN, &slot) == av::Verdict::kPass);
    ROW("22i the twelfth bit carries to the heir",
        (t10.carry(GOTTFRIED, BRIAN) & (1u << FAC)) != 0 && av::decide(t10, FAC, 7, BRIAN, &slot) == av::Verdict::kDrop);
    ROW("22j a record says create, join or interaction", av::kFactionCreate == 0 && av::kFactionJoin == 1 &&
                                                         av::kFactionInteraction == 2);

    // 23. legacy, the thirteenth kind: the dynasty head's AI buying a dynasty legacy perk with renown.
    //     CAddDynastyPerkCommand (ctor FUN_067ca890: the character +0x20, the CDynastyLegacyPerk* +0x28), posted with
    //     flags 7 by the AI's legacy pass FUN_04dbc7e0 (only when the ruler is his dynasty's head); the player's
    //     legacy window FUN_046515f0 posts it with flags 0xe; the bridge has no call that posts it. Price
    //     (FUN_05eaab70): PERK_COST_MULTIPLIER x the dynasty's legacies + PERK_COST_BASE, renown (250 + 500 n).
    //     West Francia 867, a seated run: Charles's AI bought two (867.04, 885.10), renown 1,578 -> 240.
    const int LEG = static_cast<int>(av::What::kLegacy);
    const uint32_t CHARLES = 12015;
    ROW("23a legacy round-trips and is the thirteenth kind",
        LEG == 12 && av::kWhatCount >= 13 && av::what_by_name("legacy") == LEG &&   // call_in is the 14th
        std::string_view(av::what_name(LEG)) == "legacy");
    ROW("23b CONTROL perk is still the lifestyle kind, a different name", av::what_by_name("perk") == PERK &&
        PERK != LEG && av::what_by_name("dynasty_perk") == av::kNone);
    ROW("23c its ruler is the command's own +0x20", av::ruler_offset(LEG) == av::kCmdRulerOffset);
    ROW("23d the price: 250 + 500 for each legacy held before this one",
        av::legacy_price(250, 500, 3, false) == 1750 && av::legacy_price(250, 500, 3, true) == 1250 &&
        av::legacy_price(250, 500, 0, false) == 250);
    ROW("23e an unread or impossible count is no price",
        av::legacy_price(250, 500, 0, true) < 0 && av::legacy_price(250, 100, 0, true) < 0 &&
        av::legacy_price(250, 500, -1, false) < 0 &&
        av::legacy_price(250, 500, 200, false) < 0 && av::legacy_price(0, 500, 1, false) < 0 &&
        av::legacy_price(250, -5, 1, false) < 0);
    av::Table t11;
    t11.watch(CHARLES);
    ROW("23f no rule: his AI's legacy is recorded, passed", av::decide(t11, LEG, 7, CHARLES, &slot) == av::Verdict::kRecord);
    t11.set_rule(CHARLES, LEG, true);
    ROW("23g a legacy veto drops his AI's post, and only that kind",
        av::decide(t11, LEG, 7, CHARLES | GEN, &slot) == av::Verdict::kDrop &&
        av::decide(t11, PERK, 7, CHARLES, &slot) == av::Verdict::kRecord &&
        av::decide(t11, FAC, 7, CHARLES, &slot) == av::Verdict::kRecord);
    ROW("23h CONTROL a bridge post (the marker), the player's window (0xe) and another ruler pass",
        av::decide(t11, LEG, 7, CHARLES, &slot, av::bridge_own(av::Origin::kGame, true)) == av::Verdict::kRecord &&
        av::decide(t11, LEG, 0xe, CHARLES, &slot) == av::Verdict::kPass &&
        av::decide(t11, LEG, 7, BRIAN, &slot) == av::Verdict::kPass);
    ROW("23i the thirteenth bit carries to the heir and fits the mask word",
        (t11.carry(CHARLES, BRIAN) & (1u << LEG)) != 0 && av::decide(t11, LEG, 7, BRIAN, &slot) == av::Verdict::kDrop &&
        av::kWhatCount <= 32);

    // 24. the create_title veto spares one title (k_sweden 2416 for Bjorn 12330)
    {
        av::Table t3;
        const uint32_t BJORN = 12330, K_SWEDEN = 2416, D_UPPLAND = 2417;
        int s3 = -1;
        ROW("24a no slot: nothing spared", !t3.set_spare_title(BJORN, K_SWEDEN));
        t3.watch(BJORN);
        t3.set_rule(BJORN, CT, true);
        ROW("24b none spared: every title's post drops",
            !av::title_spared(t3.slot[t3.find(BJORN)], K_SWEDEN) &&
            av::decide(t3, CT, 7, BJORN, &s3, false, true) == av::Verdict::kDrop);
        ROW("24c set the spared title", t3.set_spare_title(BJORN, K_SWEDEN));
        const av::Slot& sl = t3.slot[t3.find(BJORN)];
        ROW("24d the spared title matches (a full id's high byte ignored), another does not",
            av::title_spared(sl, K_SWEDEN) && av::title_spared(sl, K_SWEDEN | 0x7F000000ull) &&
            !av::title_spared(sl, D_UPPLAND) && !av::title_spared(sl, 0));
        ROW("24e the hook's answer for the spared title: recorded, not dropped",
            av::decide(t3, CT, 7, BJORN, &s3, false, !av::title_spared(sl, K_SWEDEN)) == av::Verdict::kRecord &&
            av::decide(t3, CT, 7, BJORN, &s3, false, !av::title_spared(sl, D_UPPLAND)) == av::Verdict::kDrop);
        const uint32_t HEIR = 12331;
        t3.carry(BJORN, HEIR);
        ROW("24f the heir's slot carries the spared title", av::title_spared(t3.slot[t3.find(HEIR)], K_SWEDEN));
        ROW("24g zero forgets it", t3.set_spare_title(BJORN, 0) && !av::title_spared(sl, K_SWEDEN));
    }
    // 25. call_in — another ruler's AI calls the watched ruler INTO a war (ally 36358's calls to
    //     Murchad 31762, accepted by his AI, 1076-79 and 1082-85). The watched ruler is the RECIPIENT; the three
    //     call interactions sharing special_interaction = call_ally_interaction all count.
    {
        const int CI = static_cast<int>(av::What::kCallIn), AC = static_cast<int>(av::What::kAllyCall);
        const uint32_t MUR = 31762, ALLY = 36358;
        const uint64_t T_CALL = 0x7f0000001000ull, T_HOUSE = 0x7f0000002000ull, T_DYN = 0x7f0000003000ull,
                       T_VO = 0x7f0000004000ull, T_OTHER = 0x7f0000005000ull;
        const uint64_t calls[av::kCallInCount] = {T_CALL, T_HOUSE, T_DYN};
        ROW("25a call_in is the fourteenth kind, its own name", CI == 13 && av::kWhatCount >= 14 &&
            av::what_by_name("call_in") == CI && std::string_view(av::what_name(CI)) == "call_in" &&
            av::kWhatCount <= 32);
        ROW("25b its watched ruler is the RECIPIENT (cmd+0x2fc); ally_call's is the actor",
            av::ruler_offset(CI) == 0x2fc && av::ruler_offset(AC) == 0x2f8);
        uint32_t which = 99;
        ROW("25c the three calls are call_in (which names the key), an offer is vassal_offer, others none",
            av::recipient_interaction_what(T_CALL, T_VO, calls, av::kCallInCount, &which) == CI && which == 0 &&
            av::recipient_interaction_what(T_DYN, T_VO, calls, av::kCallInCount, &which) == CI && which == 2 &&
            av::recipient_interaction_what(T_HOUSE, T_VO, calls, av::kCallInCount) == CI &&
            av::recipient_interaction_what(T_VO, T_VO, calls, av::kCallInCount) == static_cast<int>(av::What::kVassalOffer) &&
            av::recipient_interaction_what(T_OTHER, T_VO, calls, av::kCallInCount) == av::kNone &&
            av::recipient_interaction_what(0, T_VO, calls, av::kCallInCount) == av::kNone);
        const uint64_t unresolved[av::kCallInCount] = {0, 0, 0};
        ROW("25d CONTROL types not resolved yet: nothing is a call_in",
            av::recipient_interaction_what(T_CALL, 0, unresolved, av::kCallInCount) == av::kNone);
        av::Table t6;
        int s6 = -1;
        t6.watch(MUR);
        ROW("25e no rule: a call to him is recorded, passed (the witness of the war he joined)",
            av::decide(t6, CI, 7, MUR, &s6) == av::Verdict::kRecord);
        ROW("25f the rule drops an AI's call to him, and leaves his own AI's ally calls alone",
            t6.set_rule(MUR, CI, true) && av::decide(t6, CI, 7, MUR, &s6) == av::Verdict::kDrop &&
            av::decide(t6, AC, 7, MUR, &s6) == av::Verdict::kRecord);
        ROW("25g CONTROL a call to an unwatched ruler passes; the player's 0xe passes",
            av::decide(t6, CI, 7, ALLY, &s6) == av::Verdict::kPass && av::decide(t6, CI, 0xe, MUR, &s6) == av::Verdict::kPass);
        ROW("25h the fourteenth bit carries to the heir",
            (t6.carry(MUR, 36303) & (1u << CI)) != 0 && av::decide(t6, CI, 7, 36303, &s6) == av::Verdict::kDrop);
    }

    // 26. the create_title veto spares the goal title's chain (d_leinster 3 drops at 191/191/169
    //     gold, the duchy k_ireland's tier rule needed)
    {
        av::Table t7;
        const uint32_t MUR = 31762, K_IRE = 461, D_LEI = 470, D_ULS = 480, D_MEATH = 490;
        int s7 = -1;
        t7.watch(MUR);
        t7.set_rule(MUR, CT, true);
        const uint32_t ids[] = {K_IRE, D_LEI, D_ULS};
        ROW("26a three spared titles set", t7.set_spare_titles(MUR, ids, 3));
        const av::Slot& sl = t7.slot[t7.find(MUR)];
        ROW("26b each is spared, another is not; the first is the goal",
            av::title_spared(sl, K_IRE) && av::title_spared(sl, D_LEI | 0x01000000ull) && av::title_spared(sl, D_ULS) &&
            !av::title_spared(sl, D_MEATH) && !av::title_spared(sl, 0) && av::first_spare(sl) == K_IRE);
        ROW("26c the hook's answer: a chain title recorded, another dropped",
            av::decide(t7, CT, 7, MUR, &s7, false, !av::title_spared(sl, D_LEI)) == av::Verdict::kRecord &&
            av::decide(t7, CT, 7, MUR, &s7, false, !av::title_spared(sl, D_MEATH)) == av::Verdict::kDrop);
        const uint32_t nine[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        ROW("26d over the cap is refused and leaves the list", !t7.set_spare_titles(MUR, nine, 9) &&
            av::title_spared(sl, D_ULS));
        const uint32_t one[] = {K_IRE};
        ROW("26e a shorter list forgets the rest (the chain met)", t7.set_spare_titles(MUR, one, 1) &&
            av::title_spared(sl, K_IRE) && !av::title_spared(sl, D_LEI) && !av::title_spared(sl, D_ULS));
        t7.set_spare_titles(MUR, ids, 3);
        t7.carry(MUR, 36303);
        ROW("26f the heir's slot carries the whole list", av::title_spared(t7.slot[t7.find(36303)], D_ULS) &&
            av::first_spare(t7.slot[t7.find(36303)]) == K_IRE);
        uint32_t out[av::kMaxSpareTitles] = {};
        ROW("26g the id list parses; a bad one is refused",
            av::split_title_ids("461, 470,480", out) == 3 && out[0] == 461 && out[2] == 480 &&
            av::split_title_ids("", out) == 0 && av::split_title_ids("461,,470", out) < 0 &&
            av::split_title_ids("461,x", out) < 0 && av::split_title_ids("0", out) < 0 &&
            av::split_title_ids("16777216", out) < 0 && av::split_title_ids("1,2,3,4,5,6,7,8,9", out) < 0);
        ROW("26h CONTROL the one-title setter still works", t7.set_spare_title(MUR, D_MEATH) &&
            av::title_spared(sl, D_MEATH) && !av::title_spared(sl, K_IRE));
    }

    // 27. the decision veto's gold rule (Murchad 31762; take_vow_of_poverty_decision has no cost
    // block, pay_homage_decision a gold one)
    {
        av::Table t6;
        const uint32_t MURCHAD = 31762, BRIAN_HEIR = 36303;
        const int DEC = static_cast<int>(av::What::kDecision);
        const uint32_t none[av::kCostSlots] = {}, gold[av::kCostSlots] = {1}, prestige[av::kCostSlots] = {0, 1};
        ROW("27a the cost read: a gold entry in the primary block, prestige only, none (then the fallback's)",
            av::decision_costs_gold(gold, 0) && !av::decision_costs_gold(prestige, 1) &&
            !av::decision_costs_gold(none, 0) && av::decision_costs_gold(none, 2));
        ROW("27b no slot: no gold rule", !t6.set_decision_gold(MURCHAD, true));
        t6.watch(MURCHAD);
        const char* vow = "take_vow_of_poverty_decision";
        const char* k1[] = {vow};
        t6.set_decision_keys(MURCHAD, k1, 1);
        t6.set_decision_gold(MURCHAD, true);
        t6.set_rule(MURCHAD, DEC, true);
        const av::Slot& s6 = t6.slot[t6.find(MURCHAD)];
        const char* stress = "stress_loss_drunkard_decision";
        const char* homage = "pay_homage_decision";
        ROW("27c keys and gold: the named key, a gold decision; not a free unnamed one",
            av::decision_key_matches(s6, vow, std::strlen(vow), false) == av::KeyMatch::kYes &&
            av::decision_key_matches(s6, homage, std::strlen(homage), true) == av::KeyMatch::kYes &&
            av::decision_key_matches(s6, stress, std::strlen(stress), false) == av::KeyMatch::kNo);
        t6.set_decision_keys(MURCHAD, k1, 0);
        ROW("27d gold and no keys: only the gold ones (not every decision)",
            av::decision_key_matches(s6, homage, std::strlen(homage), true) == av::KeyMatch::kYes &&
            av::decision_key_matches(s6, stress, std::strlen(stress), false) == av::KeyMatch::kNo);
        t6.set_decision_gold(MURCHAD, false);
        ROW("27e CONTROL no gold rule and no keys: every decision, as before",
            av::decision_key_matches(s6, stress, std::strlen(stress), false) == av::KeyMatch::kYes);
        t6.set_decision_keys(MURCHAD, k1, 1);
        ROW("27f CONTROL no gold rule, keys: a gold decision not named passes",
            av::decision_key_matches(s6, homage, std::strlen(homage), true) == av::KeyMatch::kNo);
        t6.set_decision_gold(MURCHAD, true);
        t6.carry(MURCHAD, BRIAN_HEIR);
        ROW("27g the heir's slot carries the gold rule",
            t6.slot[t6.find(BRIAN_HEIR)].gold_cost.load() == 1 &&
            av::decision_key_matches(t6.slot[t6.find(BRIAN_HEIR)], homage, std::strlen(homage), true) == av::KeyMatch::kYes);
    }
    // 28. activity — the AI's HostActivities pass starts an activity for the watched ruler
    //     (1075.10-1076.02: a pilgrimage, about -155 gold, under no veto). CStartActivityCommand: the host at cmd+0x28.
    {
        av::Table t8;
        const int ACT = static_cast<int>(av::What::kActivity);
        const uint32_t BRIAN = 36303, HEIR = 29159;
        int s8 = -1;
        ROW("28a activity is the fifteenth kind, its own name, in the mask's width",
            ACT == 14 && av::kWhatCount == 15 && av::what_by_name("activity") == ACT &&
            std::string_view(av::what_name(ACT)) == "activity" && av::kWhatCount <= 32);
        ROW("28b its watched ruler is the plan's host (cmd+0x20 plan, +0x8 host); the type at cmd+0x20",
            av::ruler_offset(ACT) == 0x28 && av::kActivityTypeOffset == 0x20);
        t8.watch(BRIAN);
        ROW("28c no rule: recorded and passed (the purse prices it)", av::decide(t8, ACT, 7, BRIAN, &s8) == av::Verdict::kRecord);
        ROW("28d the rule drops his AI's start; the player's 0xe passes",
            t8.set_rule(BRIAN, ACT, true) && av::decide(t8, ACT, 7, BRIAN, &s8) == av::Verdict::kDrop &&
            av::decide(t8, ACT, 0xe, BRIAN, &s8) == av::Verdict::kPass);
        const av::Slot& sl = t8.slot[t8.find(BRIAN)];
        const char* pil = "activity_pilgrimage";
        const char* feast = "activity_feast";
        ROW("28e no spared type: none spared (an unread key neither)",
            !av::activity_spared(sl, pil, std::strlen(pil)) && !av::activity_spared(sl, nullptr, 0));
        const char* k1[] = {feast};
        ROW("28f a spared type (the seat's host_activity) passes, another does not",
            t8.set_activity_spares(BRIAN, k1, 1) && av::activity_spared(sl, feast, std::strlen(feast)) &&
            !av::activity_spared(sl, pil, std::strlen(pil)));
        const char* five[] = {feast, pil, "activity_hunt", "activity_tour", "activity_wedding"};
        ROW("28g over the cap is refused and leaves the list", !t8.set_activity_spares(BRIAN, five, 5) &&
            av::activity_spared(sl, feast, std::strlen(feast)));
        ROW("28h the heir's slot carries the rule and the spared types",
            (t8.carry(BRIAN, HEIR) & (1u << ACT)) != 0 && av::decide(t8, ACT, 7, HEIR, &s8) == av::Verdict::kDrop &&
            av::activity_spared(t8.slot[t8.find(HEIR)], feast, std::strlen(feast)));
        const char* self[] = {sl.act_keys[0]};
        ROW("28i a list set from the slot's own keys keeps them (the copy is taken first)",
            t8.set_activity_spares(BRIAN, self, 1) && av::activity_spared(sl, feast, std::strlen(feast)));
        ROW("28j zero forgets them", t8.set_activity_spares(BRIAN, k1, 0) && !av::activity_spared(sl, feast, std::strlen(feast)));
    }
    std::printf("%d rows, %d failed\n", g_rows, g_fail);
    return g_fail ? 1 : 0;
}
