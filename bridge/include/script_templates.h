// script_templates.h — the script action layer's FIXED templates.
//
// The bridge carries a write out as one of the game's own script effects, run
// through the engine's `effect` console handler (console_handlers::CMD_EFFECT,
// re/findings/re_console_effect.md), each guarded by the game's own check. The
// script text is composed HERE, from an enumerated set of templates, and from
// nothing else: a caller names a template and passes typed parameters (ids the
// bridge turns into script references itself, and keys that must be plain
// script identifiers). There is no parameter that carries script text, so the
// verb cannot be used as an arbitrary-effect channel.
//
// Header-only and engine-free: test_script_templates.cpp checks every template
// and every refusal on a host with no game.
//
// THE REFERENCES. `character:<id>` in script resolves HISTORY ids, not the
// runtime ids the bridge reads (a probe: "Referencing non-existent character
// in script link character:9460"; error.log prints both ids). A ruler is
// therefore addressed through his primary title, `title:<key>.holder`, the key
// read natively and the title's holder checked to be that character before the
// script is composed (do_script_action). A character with no title has no
// reference: the template is refused (`target_not_script_addressable`), never
// composed against someone else.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bridge::script_tpl {

enum class Template : uint8_t {
    kNone = 0,
    kDeclareWar,     // can_declare_war guard -> start_war
    kGift,           // run_interaction gift_interaction (the interaction's own validity)
    kSway,           // run_interaction sway_interaction
    kCouncilTask,    // exists = cp:<seat> guard -> set_council_task
    kSetPolicy,      // a standing policy variable on, and the policy event armed
    kClearPolicy,    // a standing policy variable off
    // The incoming-proposal queue (ck3agent/proposal_mod/,
    //). Each enters the SEAT's own scope
    // through his primary title, so it cannot reach an offer to anyone else.
    kProposalSeat,   // the seat marker on (400-day timed variable) or off
    kProposalPolicy, // the seat's answer for one class
    kProposalList,   // the seat's open offers, between <token>_BEGIN and <token>_END
    kProposalAnswer, // one offer answered through the mod's one apply effect
    kEndWar,         // A war the actor leads against the target ends (end_war)
    kGoldReserve,    // (route P3): the actor's AI reserved-gold maximum, gold moved into it
    kActivity,       // The activities the actor's AI may host (gates), and one put to its AI
    kClaimPriceFacts,// The actor's claim-war price facts (legitimacy flag, age, faith's gender rule)
    kCbPrices,       // The game's own price of declaring each named casus belli on a defender (read only)
};

struct TemplateName { const char* name; Template t; };

//: The enumerated set, by the name a caller sends. Nothing else is accepted.
inline constexpr TemplateName kTemplates[] = {
    {"declare_war", Template::kDeclareWar},
    {"gift", Template::kGift},
    {"sway", Template::kSway},
    {"council_task", Template::kCouncilTask},
    {"set_policy", Template::kSetPolicy},
    {"clear_policy", Template::kClearPolicy},
    {"proposal_seat", Template::kProposalSeat},
    {"proposal_policy", Template::kProposalPolicy},
    {"proposal_list", Template::kProposalList},
    {"proposal_answer", Template::kProposalAnswer},
    {"end_war", Template::kEndWar},
    {"gold_reserve", Template::kGoldReserve},
    {"activity", Template::kActivity},
    {"claim_price_facts", Template::kClaimPriceFacts},
    {"cb_prices", Template::kCbPrices},
};

//: The terms of end_war, NAMED FROM THE ACTOR'S SIDE. The other
//: developer's mod once carried out "make the enemy surrender" as the ruler's
//: own surrender, so no word that can be read from either side is accepted
//: (surrender, victory, enforce_demands are refused, ck3agent/server
//: redesign.END_WAR_AMBIGUOUS). "" in Slots::peace_terms is the read-only form.
inline constexpr const char* kPeaceTerms[] = {"we_win", "white_peace", "we_yield"};
inline bool is_peace_terms(std::string_view t) {
    for (const char* x : kPeaceTerms)
        if (t == x) return true;
    return false;
}
//: The guards' thresholds (the peace window's rules, simplified): we_win needs
//: our side's war score at kWeWinScore, white_peace the enemy's below
//: kWhitePeaceEnemyBelow; we_yield is always open.
inline constexpr int kWeWinScore = 100;
inline constexpr int kWhitePeaceEnemyBelow = 25;
//: The war-score bands end_war reports (our side's score is >= each band it
//: logs): SCORE_GE_<n>, a negative n spelled M<n> (SCORE_GE_M25).
inline constexpr int kScoreBands[] = {100, 50, 25, 0, -25, -50, -100};

//: The reserve's bounds (whole gold) and the buckets gold is
//: moved from, in order. The effects are the game's own (binary script docs,
//: 1.18.3.1): `set_reserved_gold_maximum = X` ("the maximum (and also desired
//: value) for the 'reserved' gold AI budget ... saved up, even before the war
//: chest budget. It will not correct the current gold in that budget") and
//: `move_budget_gold = { gold = X from = Z to = Y }` ("will not move more than is
//: available in the source budget or what can fit in the target budget"), the
//: categories budget_war_chest, budget_reserved, budget_short_term, budget_long_term.
inline constexpr long long kReserveMax = 100000;
struct BudgetSource { const char* trigger; const char* category; };
inline constexpr BudgetSource kReserveSources[] = {
    {"short_term_gold", "budget_short_term"},
    {"long_term_gold", "budget_long_term"},
    {"war_chest_gold", "budget_war_chest"},
};

//: The activities a seat may put to its ruler's AI. An AI ruler hosts
//: an activity from the game's own monthly pass (AI.ToggleHostActivities): it
//: scores each activity type it may host (its is_shown / can_start, then
//: ai_will_do at least ACTIVITY_SCORE_THRESHOLD 20), takes the best, and hosts it
//: with a chance of that score in 100 (image 0x04da9f40, 0x04db1cd0). The game's
//: own effect `ai_attempt_to_host_activity = <type>` ("Force the AI to re-evaluate
//: the given activity type and see if they want to host it. This overrides the
//: ai_check_interval and activity cooldown") puts the type into the ruler's
//: forced set (image 0x04ec0960; it refuses a character the game's AI does not
//: run). Each gate below is the activity file's own AI condition, word for word
//: (common/activities/activity_types/<type>.txt is_shown / can_start, 1.18.3.1);
//: the gold condition (short_term_gold against the type's base price) is not here:
//: its price is a script value rooted at `root`, which in a console effect is not
//: the ruler (re_console_effect.md), so the caller prices it from its own reads.
struct ActivityType { const char* key; const char* word; const char* axis; bool build_focus; };
inline constexpr ActivityType kActivityTypes[] = {
    {"activity_feast", "FEAST", "ai_sociability", true},
    {"activity_hunt", "HUNT", "ai_boldness", true},
    {"activity_pilgrimage", "PILGRIMAGE", "ai_zeal", false},
};
//: (declarations refused ten months while the ruler was in a coronation the reply called "another
//: activity type"): every activity type of 1.18.3.1 (common/activities/activity_types/), marked IN_T_<index> when the
//: ruler's involved_activity is of that type, so the refusal names it.
inline constexpr const char* kInvolvedActivityTypes[] = {
    "activity_camp_party", "activity_chariot_race", "activity_coronation", "activity_debate", "activity_feast",
    "activity_festival", "activity_funeral", "activity_gruesome_festival", "activity_roaming", "activity_hunt",
    "activity_imperial_examination", "activity_survey", "activity_local_examination", "activity_journey",
    "activity_pilgrimage", "activity_playdate", "activity_tournament", "activity_tour", "activity_adult_education",
    "activity_wedding", "activity_witch_ritual",
};
inline const ActivityType* activity_type(std::string_view k) {
    for (const auto& a : kActivityTypes)
        if (k == a.key) return &a;
    return nullptr;
}
//: The gates every type shares (is_shown's AI branch and can_start), each mark
//: written when the condition FAILS. A count is "throttled" by one AI value per
//: type (feast ai_sociability, hunt ai_boldness, pilgrimage ai_zeal >= 100).
struct ActivityGate { const char* word; const char* trigger; };
inline constexpr ActivityGate kActivityGates[] = {
    {"NOT_AI", "is_ai = no"},
    {"TIER_BARONY", "highest_held_title_tier <= tier_barony"},
    {"EARLY", "years_from_game_start < 3"},
    {"AT_WAR", "is_at_war = yes"},
    {"BUILD_FOCUS", "ai_should_focus_on_building_in_their_capital = yes"},
    {"CONQUEROR", "has_variable = conqueror"},
    {"UNCROWNED", "has_realm_law = uncrowned"},
    {"NOT_AVAILABLE_ADULT", "is_available_adult = no"},
    {"IN_ACTIVITY", "exists = involved_activity"},
    {"COUNT_TIER", "highest_held_title_tier = tier_county"},
};

//: ("your capital has a building slot to fill" named no slot; the seat built
//: at Athlone, which did not clear it): the game's ai_should_focus_on_building_in_their_capital
//: (common/scripted_triggers/00_ai_value_triggers.txt) reads capital_county.title_province, the capital
//: county's seat, and holds when ONE of these does. Each is written, word for word, when it holds; and
//: BF_SEAT_IS_CAPITAL when that province is the ruler's capital_province (the realm's capital barony).
struct BuildFocusBranch { const char* word; const char* trigger; };
inline constexpr BuildFocusBranch kBuildFocusBranches[] = {
    {"BF_FREE_SLOT", "scope:ck3a_bfp = { free_building_slots > 0 }"},
    {"BF_LEDGER", "culture = { has_innovation = innovation_ledger } scope:ck3a_bfp = { combined_building_level <= 4 }"},
    {"BF_BALIFFS", "culture = { has_innovation = innovation_baliffs } scope:ck3a_bfp = { combined_building_level <= 6 }"},
    {"BF_GUILDS", "culture = { has_innovation = innovation_guilds } scope:ck3a_bfp = { combined_building_level <= 9 }"},
    {"BF_CRANES", "culture = { has_innovation = innovation_cranes } scope:ck3a_bfp = { combined_building_level <= 12 }"},
    {"BF_SEAT_IS_CAPITAL", "exists = capital_province capital_province = { this = scope:ck3a_bfp }"},
};

//: (a test run: the declare gate read 234.38 where the war cost
//: 312.5): claim_cb's legitimacy factor is the attacker's legitimacy FLAG
//: (common/casus_belli_types/00_claim.txt "LEGITIMACY AFFECTS COST": two multiply
//: blocks, one of each list), read here with the game's own trigger instead of
//: inferred from the legitimacy value and a culture era no read gives.
struct LegitimacyFlag { const char* flag; const char* word; };
inline constexpr LegitimacyFlag kLegitimacyUp[] = {
    {"massively_increased_claim_cb_cost", "LEGIT_X2_5"},
    {"very_increased_claim_cb_cost", "LEGIT_X2_0"},
    {"increased_claim_cb_cost", "LEGIT_X1_5"},
};
inline constexpr LegitimacyFlag kLegitimacyDown[] = {
    {"reduced_claim_cb_cost", "LEGIT_X0_5"},
    {"very_reduced_claim_cb_cost", "LEGIT_X0_25"},
    {"extra_reduced_claim_cb_cost", "LEGIT_X0_15"},
};

inline Template template_by_name(std::string_view n) {
    for (const auto& t : kTemplates)
        if (n == t.name) return t.t;
    return Template::kNone;
}

//: The standing policies (the mod, ck3agent/policy_mod/). Each is one character
//: variable the mod's event reads; THE names live here and in the mod's events
//: file, and test_policy_mod.py checks the two agree.
struct PolicyName { const char* policy; const char* variable; };
inline constexpr PolicyName kPolicies[] = {
    {"join_liege_wars", "ck3agent_policy_join_liege_wars"},
    {"stay_out_of_liege_factions", "ck3agent_policy_no_liege_factions"},
};
inline constexpr const char* kPolicyArmedVar = "ck3agent_policy_armed";
//: The line the game writes to debug.log when it loads the mod enabled (the
//: probe's mod: "modprobe|mod/modprobe.mod|Enabled").
//: scripts/policy_mod.py ENABLED_LINE is the same string (test_policy_mod.py).
inline constexpr const char* kPolicyModEnabledLine =
    "ck3agent_policies|mod/ck3agent_policies.mod|Enabled";
inline constexpr const char* kPolicyEvent = "ck3agent_policy.1";

// ── the incoming-proposal queue ──────────────────────────────────
// THE NAMES live here once for the bridge; their mirrors are checked:
// scripts/proposal_overrides.py FIRST_SET (classes, kinds, families),
// scripts/proposal_mod.py (codes, words, the enabled line) and the mod's own
// files (test_proposal_mod.py reads all three).

struct ProposalClass { const char* name; const char* family; };
//: The classes a seat's answer policy is set on. call_to_war and peace hold
//: only if live rows show the offer is held a tick or more; a class
//: that cannot be held is removed by deleting its row here and its constant in
//: proposal_overrides.py.
inline constexpr ProposalClass kProposalClasses[] = {
    {"marriage", "resend"},
    {"alliance", "resend"},
    {"call_to_war", "join_war"},
    {"peace", "peace"},
    {"vassalage", "resend"},
};
struct ProposalKind { const char* kind; const char* cls; };
inline constexpr ProposalKind kProposalKinds[] = {
    {"arrange_marriage_interaction", "marriage"},
    {"negotiate_alliance_interaction", "alliance"},
    {"call_ally_interaction", "call_to_war"},
    {"end_war_attacker_victory_interaction", "peace"},
    {"end_war_attacker_white_peace_interaction", "peace"},
    {"end_war_attacker_defeat_interaction", "peace"},
    {"offer_vassalization_interaction", "vassalage"},
};
//: The mod's codes, by index (0 is no word). answer: the policy values and the
//: answers; by: who answered; void: why an offer could no longer be answered.
inline constexpr const char* kProposalAnswers[] = {"", "accept", "decline", "ask_me", "native"};
inline constexpr const char* kProposalBy[] = {"", "seat", "rule", "boundary", "lapse", "void", "classifier"};
inline constexpr const char* kProposalVoid[] = {"", "void_sender_dead", "void_subject_dead", "void_war_ended",
                                                "void_recipient_dead", "invalid_now"};
//: The outcomes of proposal_answer the mod writes (CK3AGENT_PROPOSAL_OUTCOME|<o>|<reason>).
inline constexpr const char* kProposalOutcomes[] = {"applied_accept", "applied_decline", "handed_back", "refused"};
//: The refusal reasons an outcome line may carry (beside the void reasons).
//: void_reason_unread: a void offer whose story kept no reason (voided before the
//: reason was kept); an answer to any other void offer names its void reason.
inline constexpr const char* kProposalOutcomeReasons[] = {"not_found", "already_resolved", "native_not_supported",
                                                          "void_sender_dead", "void_subject_dead", "void_war_ended",
                                                          "void_recipient_dead", "invalid_now", "void_reason_unread"};
inline constexpr const char* kProposalModEnabledLine =
    "ck3agent_proposals|mod/ck3agent_proposals.mod|Enabled";
inline constexpr const char* kProposalSeatVar = "ck3agent_proposal_seat";
inline constexpr int kProposalSeatDays = 400;
inline constexpr const char* kProposalAnswerVarPrefix = "ck3agent_proposal_answer_";
inline constexpr const char* kProposalStory = "ck3agent_proposal";
inline constexpr const char* kProposalWordPrefix = "CK3AGENT_PROPOSAL_";
//: The words query_script_log's proposal_lines mode returns.
inline constexpr const char* kProposalLogWords[] = {"CK3AGENT_PROPOSAL_HELD", "CK3AGENT_PROPOSAL_RESOLVED",
                                                    "CK3AGENT_PROPOSAL_VOID"};
//: proposal_seat's refusal when `on` is absent or not a JSON boolean.
inline constexpr const char* kProposalArgOn = "bad_on";

inline const char* proposal_family(std::string_view cls) {
    for (const auto& c : kProposalClasses)
        if (cls == c.name) return c.family;
    return nullptr;
}
template <size_t N>
inline int code_of(const char* const (&table)[N], std::string_view w) {
    if (w.empty()) return -1;
    for (size_t i = 1; i < N; ++i)
        if (w == table[i]) return static_cast<int>(i);
    return -1;
}
template <size_t N>
inline const char* word_of(const char* const (&table)[N], long long code) {
    return (code >= 1 && static_cast<size_t>(code) < N) ? table[code] : nullptr;
}
//: The argument checks, one place: the code to send, or -1 with WHY set.
inline int proposal_policy_answer(std::string_view a) { return code_of(kProposalAnswers, a); }
//: proposal_answer takes accept, decline or native (ask_me is a policy, not an answer).
inline int proposal_answer_code(std::string_view a) {
    const int c = code_of(kProposalAnswers, a);
    return (c == 1 || c == 2 || c == 4) ? c : -1;
}
//: A template call answers as the seat, at the boundary, or as the classifier;
//: rule, lapse and void are the game's own (the event, the story).
inline int proposal_by_code(std::string_view b) {
    const int c = code_of(kProposalBy, b);
    return (c == 1 || c == 3 || c == 6) ? c : -1;
}
inline bool proposal_id_ok(long long id) { return id >= 1 && id <= 100000000LL; }

//: A word query_script_log may count in debug.log: letters, digits and _|./-
//: (the policy mod's CK3AGENT_POLICY_* lines, the mod-enabled line).
inline bool is_count_word(std::string_view w) {
    if (w.empty() || w.size() > 96) return false;
    for (char c : w)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '|' || c == '.' || c == '/' || c == '-'))
            return false;
    return true;
}

inline const char* policy_variable(std::string_view p) {
    for (const auto& x : kPolicies)
        if (p == x.policy) return x.variable;
    return nullptr;
}

//: A plain script identifier: lower-case letter first, then [a-z0-9_], at most
//: 96 characters. Keys (a CB name, a council task, a seat, a title key) must be
//: one, so a key can never close a block or open another one.
inline bool is_ident(std::string_view s) {
    if (s.empty() || s.size() > 96) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

//: A title key script can resolve. A title the
//: game creates mid-run (a dynamic title: `x_script_846`, Domnall of Oriel's
//: primary duchy) has a key, but `title:x_script_846` is not a script link, so
//: `title:x_script_846.holder` was null and can_declare_war answered "defender was
//: null". A key script resolves is a de-jure title's: its tier prefix (b_ c_ d_
//: k_ e_ h_) and an identifier; anything else is never put into a reference.
inline bool is_script_title_key(std::string_view k) {
    if (!is_ident(k) || k.size() < 3 || k[1] != '_') return false;
    switch (k[0]) {
    case 'b': case 'c': case 'd': case 'k': case 'e': case 'h': return true;
    default: return false;
    }
}

//: The script reference of a character addressed through a title he holds.
inline std::string holder_ref(std::string_view title_key) {
    return "title:" + std::string(title_key) + ".holder";
}
inline std::string title_ref(std::string_view title_key) {
    return "title:" + std::string(title_key);
}
inline std::string province_ref(std::string_view county_key) {
    return "title:" + std::string(county_key) + ".title_province";
}

//: The slots a template is filled from. Every *_key is checked with is_ident;
//: refs are built by the functions above from checked keys, never taken from a
//: caller. `token` is the bridge's own (SACT_<n>), written to debug.log by the
//: guard's two branches so the outcome can be read back.
struct Slots {
    std::string actor_title;       // the actor's primary title key
    std::string target_title;      // the recipient / defender's primary title key
    std::string claimant_title;    // declare_war: the claimant's primary title key ("" = none)
    std::string war_title;         // declare_war: the CB's target title key ("" = none)
    std::string cb;                // declare_war: the casus belli key
    std::string seat;              // council_task: the council position key (councillor_*)
    std::string task;              // council_task: the task type key
    std::string task_target_char;  // council_task: a character target's primary title key
    std::string task_target_county;// council_task: a county target's key (-> its province)
    std::string policy;            // set_policy / clear_policy
    std::string peace_terms;       // end_war: we_win | white_peace | we_yield, "" = read only
    bool enemy_landless = false;   // end_war: the enemy leader holds no title (a rebel):
                                   // the war is the ONE the actor leads against a landless leader
    std::string token;             // SACT_<n>
    // The proposal templates
    bool proposal_on = false;      // proposal_seat
    std::string proposal_class;    // proposal_policy
    int proposal_answer = 0;       // proposal_policy (1-4) / proposal_answer (1, 2, 4)
    int proposal_by = 0;           // proposal_answer (1, 3, 6)
    long long proposal_id = 0;     // proposal_answer
    // gold_reserve
    long long reserve_gold = -1;   // the reserved maximum, whole gold (0 = none: the reserve is released)
    bool reserve_move = false;     // also move the gold held now into the reserve
    // Activity
    std::string activity;          // an activity type of kActivityTypes; "" = the listing only
    bool activity_host = false;    // put `activity` to the actor's AI (ai_attempt_to_host_activity)
    bool activity_leave = false;   // Take the actor out of the activity he is a GUEST in
    // cb_prices
    std::vector<std::string> price_cbs;  // casus belli keys of kCbPrices (war_title / claimant_title as declare_war)
};

struct Composed {
    bool ok = false;
    std::string script;
    std::string why;               // the refusal code when !ok
};

inline Composed refuse(const char* why) {
    Composed c; c.why = why; return c;
}

//: The debug.log lines the guard writes: <token>_RAN / <token>_REFUSED.
inline std::string ran_marker(const std::string& token) { return token + "_RAN"; }
//: The script effect every line of ours is written with. The game
//: drops every log message of a level once 100,000 of that level have been
//: logged in the session (a per-level counter, ck3::nlog::LEVEL_MSG_COUNTERS;
//: re/findings/re_log_level_cap.md). DEBUG is the game's busiest level (the
//: a recorded game's channel died at 1088.01.27 when it filled); INFO is near-unused by
//: the game (2 lines in a recorded game's 21 years), so our lines get their own
//: budget there. scripts/policy_mod.py and scripts/proposal_overrides.py write
//: the same effect (test_policy_mod.py / test_proposal_mod.py check it).
inline constexpr const char* kLogEffect = "info_log";
inline std::string log_line(const std::string& text) { return std::string(kLogEffect) + " = \"" + text + "\""; }
inline std::string refused_marker(const std::string& token) { return token + "_REFUSED"; }

//: The bridge's own token: SACT_<n>.
inline constexpr const char* kTokenPrefix = "SACT_";

inline bool token_ok(std::string_view t) {
    if (t.size() < 6 || t.size() > 32 || t.substr(0, 5) != kTokenPrefix) return false;
    for (char c : t.substr(5))
        if (!(c >= '0' && c <= '9')) return false;
    return true;
}

//: A declaration's price (a 1066 game: a vassalization declaration estimated 50 prestige took 125)
//: is its casus belli's own `cost` block, a script value of the war's scopes
//: (scope:attacker, scope:defender, scope:claimant, the list target_titles). The bridge's family estimate
//: (query_handlers.cpp cb_cost_for: 50 for any casus belli it does not know) is not the game's price:
//: vassalization_cb is 125 a county of the defender's realm (00_vassalization.txt). Each row is the
//: casus belli's prestige and piety value, word for word from common/casus_belli_types/*.txt (1.18.3.1;
//: comments removed, whitespace folded), evaluated by the game in the cb_prices template and in
//: declare_war's refused branch. test_declaration_price_and_strength.py checks every row against the install's files. The set: every
//: casus belli the evidence's cbs / plan_war rows offered (claim_cb, county_conquest_cb,
//: county_struggle_cb, ducal_conquest_cb, fp2_border_raid, individual_county_de_jure_cb,
//: make_tributary_cb, vassalization_cb) and their siblings (duchy conquest and de jure, tribal
//: subjugation, the three religious wars). needs_title: the value reads target_titles or the claimant.
struct CbPrice { const char* cb; const char* prestige; const char* piety; bool needs_title; };
inline constexpr CbPrice kCbPrices[] = {
    {"county_struggle_cb", R"cb({ value = 0 if = { limit = { scope:defender.faith != scope:attacker.faith } value = struggle_cb_phase_modifier_cost } multiply = common_cb_prestige_cost_multiplier_sans_struggles })cb", R"cb({ value = 0 if = { limit = { scope:defender.faith = scope:attacker.faith } value = struggle_cb_phase_modifier_cost } multiply = common_cb_piety_cost_multiplier_sans_struggles })cb", true},
    {"fp2_border_raid", R"cb({ value = minor_prestige_value if = { limit = { scope:defender.faith != scope:attacker.faith } add = { value = minor_prestige_value desc = CB_COST_DIFFERENT_FAITH } } multiply = common_cb_prestige_cost_multiplier })cb", R"cb({ value = 0 if = { limit = { scope:defender.faith = scope:attacker.faith } add = { value = minor_piety_value desc = CB_SAME_FAITH_COST } } multiply = common_cb_piety_cost_multiplier })cb", false},
    {"claim_cb", R"cb({ value = 0 add = claim_cb_prestige_cost multiply = common_cb_prestige_cost_multiplier if = { limit = { scope:attacker = { has_character_flag = temp_no_claim_war_cost } } multiply = { value = 0 desc = "free_cost" } } multiply = { value = { value = 0 scope:attacker = { if = { limit = { has_legitimacy_flag = reduced_claim_cb_cost } add = 0.5 } else_if = { limit = { has_legitimacy_flag = very_reduced_claim_cb_cost } add = 0.25 } else_if = { limit = { has_legitimacy_flag = extra_reduced_claim_cb_cost } add = 0.15 } else = { add = 1 } } } desc = legitimacy_high_modifier_tt } multiply = { value = { value = 0 scope:attacker = { if = { limit = { has_legitimacy_flag = massively_increased_claim_cb_cost } add = 2.5 } else_if = { limit = { has_legitimacy_flag = very_increased_claim_cb_cost } add = 2 } else_if = { limit = { has_legitimacy_flag = increased_claim_cb_cost } add = 1.5 } else = { add = 1 } } } desc = legitimacy_low_modifier_tt } })cb", R"cb({ value = 0 add = common_cb_impious_piety_cost })cb", true},
    {"county_conquest_cb", R"cb({ value = 0 if = { limit = { scope:defender.faith != scope:attacker.faith } add = { value = 50 desc = CB_COST_DIFFERENT_FAITH } add = { value = 5 multiply = scope:attacker.realm_size desc = CB_REALM_SIZE_COST max = 100 } } multiply = common_cb_prestige_cost_multiplier })cb", R"cb({ value = 0 if = { limit = { scope:defender.faith = scope:attacker.faith } add = { value = 25 desc = CB_SAME_FAITH_COST } add = { value = 5 multiply = scope:attacker.realm_size desc = CB_REALM_SIZE_COST max = 75 } } multiply = common_cb_piety_cost_multiplier })cb", false},
    {"duchy_conquest_cb", R"cb({ value = 0 if = { limit = { scope:defender.faith != scope:attacker.faith } add = { value = 150 desc = CB_COST_DIFFERENT_FAITH } add = { value = 10 multiply = scope:attacker.realm_size desc = CB_REALM_SIZE_COST max = 800 } } multiply = common_cb_prestige_cost_multiplier if = { limit = { scope:attacker.house = { OR = { has_house_modifier = fp2_struggle_hostility_house_culture_modifier has_house_modifier = fp2_struggle_hostility_house_combined_modifier } } any_in_global_list = { variable = fp2_struggle_ending_culture_list this = scope:attacker.culture } any_in_global_list = { variable = fp2_struggle_ending_culture_list this = scope:defender.culture } } divide = { value = 2 desc = CB_COST_FP2_STRUGGLE_HOSTILITY_CULTURE_DISCOUNT } } if = { limit = { has_global_variable = fp2_struggle_compromise_ending any_in_global_list = { variable = fp2_struggle_ending_culture_list this = scope:attacker.culture } any_in_global_list = { variable = fp2_struggle_ending_culture_list this = scope:defender.culture } } multiply = { value = 2 desc = CB_COST_FP2_STRUGGLE_COMPROMISE_MULTIPLIER } } })cb", R"cb({ add = common_cb_impious_piety_cost if = { limit = { scope:defender.faith = scope:attacker.faith } add = { value = 100 desc = CB_SAME_FAITH_COST } add = { value = 10 multiply = scope:attacker.realm_size desc = CB_REALM_SIZE_COST max = 800 } } multiply = common_cb_piety_cost_multiplier })cb", false},
    {"ducal_conquest_cb", R"cb({ value = 0 if = { limit = { scope:attacker.primary_title.tier <= tier_county } add = { value = 50 desc = CB_BASE_COST } } else = { add = { value = 100 desc = CB_BASE_COST } } multiply = common_cb_prestige_cost_multiplier })cb", R"cb({ value = 0 add = common_cb_impious_piety_cost })cb", false},
    {"individual_county_de_jure_cb", R"cb({ add = { value = 100 desc = CB_CLAIM_COST_TARGET_TITLES } multiply = common_cb_prestige_cost_multiplier })cb", R"cb({ value = 0 add = common_cb_impious_piety_cost })cb", false},
    {"individual_duchy_de_jure_cb", R"cb({ value = 0 add = { every_in_list = { list = target_titles every_in_de_jure_hierarchy = { limit = { tier = tier_county OR = { holder.top_liege = scope:defender holder.liege = scope:defender holder.liege = scope:defender.liege } } add = 100 } } desc = CB_CLAIM_COST_TARGET_TITLES } multiply = common_cb_prestige_cost_multiplier })cb", R"cb({ value = 0 add = common_cb_impious_piety_cost })cb", true},
    {"vassalization_cb", R"cb({ value = 0 add = { desc = CB_SIZE_OF_TARGET_REALM_COST scope:defender = { every_sub_realm_county = { add = { value = 125 } } } } if = { limit = { scope:defender.primary_title = { tier = tier_kingdom } } multiply = { value = 2 desc = CB_VASSALIZING_KING } } multiply = common_cb_prestige_cost_multiplier })cb", R"cb({ value = 0 add = common_cb_impious_piety_cost })cb", false},
    {"make_tributary_cb", R"cb({ value = { if = { limit = { OR = { scope:attacker = { government_has_flag = government_is_mandala } scope:attacker = { ai_should_get_conqueror_bonuses = yes } } } multiply = 0 } else = { scope:defender = { add = { add = realm_size multiply = 25 if = { limit = { primary_title.tier = tier_county } max = 100 } else_if = { limit = { primary_title.tier = tier_duchy } max = 200 } else = { max = 300 } desc = CB_SIZE_OF_TARGET_REALM_COST } } } } multiply = common_cb_prestige_cost_multiplier })cb", R"cb({ value = { if = { limit = { scope:attacker = { NOT = { government_has_flag = government_is_mandala } } } multiply = 0 } else = { scope:defender = { value = { add = realm_size multiply = 75 if = { limit = { primary_title.tier = tier_county } max = 200 } else_if = { limit = { primary_title.tier = tier_duchy } max = 350 } else = { max = 500 } if = { limit = { scope:attacker = { is_story_godking_trigger = yes } } multiply = 0.25 } desc = CB_SIZE_OF_TARGET_REALM_COST } } } } multiply = common_cb_piety_cost_multiplier })cb", false},
    {"tribal_subjugation_cb", R"cb(tribal_subjugation_cb_prestige_cost)cb", R"cb({ value = 0 if = { limit = { scope:attacker.faith.religious_head = scope:attacker scope:defender.faith = scope:attacker.faith } add = { value = 1000 desc = CB_BASE_COST } } add = common_cb_impious_piety_cost multiply = common_cb_piety_cost_multiplier if = { limit = { scope:attacker = { has_title = title:d_sunni struggle:persian_struggle ?= { has_struggle_phase_parameter = fp3_caliph_gets_cheaper_subjugation_cb } } } multiply = { value = fp3_caliph_gets_cheaper_subjugation_cb_actual_value desc = CB_STRUGGLE_PHASE_DISCOUNT } } })cb", false},
    {"minor_religious_war", R"cb()cb", R"cb({ add = { value = 100 desc = CB_BASE_COST } multiply = common_cb_piety_cost_multiplier multiply = holy_war_cb_piety_cost_multiplier if = { limit = { scope:attacker = { any_active_accolade = { accolade_parameter = acclaimed_knight_piety_from_battle } } } multiply = accolade_piety_reduction_value min = 10 } if = { limit = { scope:attacker = { any_active_accolade = { accolade_parameter = acclaimed_knight_piety_from_battle_high } } } multiply = accolade_piety_reduction_value_high min = 10 } })cb", false},
    {"religious_war", R"cb()cb", R"cb({ add = { value = 200 desc = CB_BASE_COST } multiply = common_cb_piety_cost_multiplier multiply = holy_war_cb_piety_cost_multiplier if = { limit = { scope:attacker.house = { OR = { has_house_modifier = fp2_struggle_hostility_house_faith_modifier has_house_modifier = fp2_struggle_hostility_house_combined_modifier } } any_in_global_list = { variable = fp2_struggle_ending_faith_list this = scope:attacker.faith } any_in_global_list = { variable = fp2_struggle_ending_faith_list this = scope:defender.faith } } divide = { value = 2 desc = CB_COST_FP2_STRUGGLE_HOSTILITY_FAITH_DISCOUNT } } if = { limit = { has_global_variable = fp2_struggle_compromise_ending any_in_global_list = { variable = fp2_struggle_ending_faith_list this = scope:attacker.faith } any_in_global_list = { variable = fp2_struggle_ending_faith_list this = scope:defender.faith } } multiply = { value = 2 desc = CB_COST_FP2_STRUGGLE_COMPROMISE_MULTIPLIER } } if = { limit = { scope:attacker = { any_active_accolade = { accolade_parameter = acclaimed_knight_piety_from_battle } } } multiply = accolade_piety_reduction_value min = 25 } if = { limit = { scope:attacker = { any_active_accolade = { accolade_parameter = acclaimed_knight_piety_from_battle_high } } } multiply = accolade_piety_reduction_value_high min = 25 } })cb", false},
    {"major_religious_war", R"cb()cb", R"cb({ add = { value = 750 desc = CB_BASE_COST } multiply = common_cb_piety_cost_multiplier multiply = holy_war_cb_piety_cost_multiplier if = { limit = { scope:attacker = { any_active_accolade = { accolade_parameter = acclaimed_knight_piety_from_battle } } } multiply = accolade_piety_reduction_value min = 90 } if = { limit = { scope:attacker = { any_active_accolade = { accolade_parameter = acclaimed_knight_piety_from_battle_high } } } multiply = accolade_piety_reduction_value_high min = 90 } })cb", false},
};
inline int cb_price_index(std::string_view k) {
    for (size_t i = 0; i < sizeof kCbPrices / sizeof kCbPrices[0]; ++i)
        if (k == kCbPrices[i].cb) return static_cast<int>(i);
    return -1;
}
//: The variable a price is computed into (on the actor, removed before the script ends) and its
//: encoding as marks: <word>_NEG, <word>_OVER (2^18 or more: not encoded), <word>_I<k> for each bit k of
//: the whole part (17..0), <word>_F<k> for each bit of the fraction times 1024 (9..0), then <word>_SET.
inline constexpr const char* kPriceVar = "ck3a_cbp";
inline constexpr int kPriceIntBits = 18;
inline constexpr int kPriceFracBits = 10;
inline std::string price_marks(const std::string& token, const std::string& word) {
    const std::string v = std::string("var:") + kPriceVar, n = std::string("name = ") + kPriceVar;
    auto mark = [&](const std::string& m) { return log_line(token + "_" + word + "_" + m); };
    std::string o = " if = { limit = { " + v + " < 0 } " + mark("NEG") + " change_variable = { " + n +
                    " multiply = -1 } }";
    o += " if = { limit = { " + v + " >= " + std::to_string(1L << kPriceIntBits) + " } " + mark("OVER") +
         " set_variable = { " + n + " value = 0 } }";
    for (int k = kPriceIntBits - 1; k >= 0; --k) {
        const std::string b = std::to_string(1L << k);
        o += " if = { limit = { " + v + " >= " + b + " } " + mark("I" + std::to_string(k)) +
             " change_variable = { " + n + " subtract = " + b + " } }";
    }
    o += " change_variable = { " + n + " multiply = " + std::to_string(1L << kPriceFracBits) + " }";
    for (int k = kPriceFracBits - 1; k >= 0; --k) {
        const std::string b = std::to_string(1L << k);
        o += " if = { limit = { " + v + " >= " + b + " } " + mark("F" + std::to_string(k)) +
             " change_variable = { " + n + " subtract = " + b + " } }";
    }
    return o + " " + mark("SET");
}
//: The scopes a cost block reads, saved in the actor's scope: attacker (the actor), defender, claimant
//: (the claimant, else the actor) and the list target_titles (the war's title, when given).
inline std::string price_scopes(const std::string& D, const std::string& C, const std::string& title) {
    std::string o = " save_scope_as = attacker " + D + " = { save_scope_as = defender }";
    o += C.empty() ? " save_scope_as = claimant" : " " + C + " = { save_scope_as = claimant }";
    if (!title.empty()) o += " " + title + " = { add_to_temporary_list = target_titles }";
    return o;
}
//: One casus belli's price into marks <word>P_* (prestige) and <word>Q_* (piety); with `why`, the
//: refused branch's reading first: <token>_WHY_PRESTIGE_BELOW_COST / _WHY_PIETY_BELOW_COST.
inline std::string price_block(const CbPrice& p, const std::string& token, const std::string& word, bool why) {
    std::string o;
    const char* cur[2][3] = {{p.prestige, "P", "prestige"}, {p.piety, "Q", "piety"}};
    for (auto& c : cur) {
        if (!c[0] || !c[0][0]) continue;
        o += std::string(" set_variable = { name = ") + kPriceVar + " value = " + c[0] + " }";
        if (why)
            o += std::string(" if = { limit = { ") + c[2] + " < var:" + kPriceVar + " } " +
                 log_line(token + "_WHY_" + (c[1][0] == 'P' ? "PRESTIGE" : "PIETY") + "_BELOW_COST") + " }";
        o += price_marks(token, word + c[1]);
    }
    return o + std::string(" remove_variable = ") + kPriceVar;
}

//: Ten claim declarations refused by
//: can_declare_war "without a word" while the casus-belli read passed. The refused
//: branch now asks the game, in the same script, each actor/defender condition of
//: declare_war_interaction's is_valid_showing_failures_only (common/character_
//: interactions/00_war.txt), each written as <token>_WHY_<WORD> when it FAILS. The
//: triggers are the file's own, word for word; `D` is the defender's reference. A
//: mark is the game's reading; no mark for a condition means it held (or that the
//: game's can_declare_war refused on something this list does not hold).
//: The rest of the interaction's script
//: conditions, so a refusal with no mark is the engine's own part (re_declare_refusal.md):
//: the defender's side (`D = { ... }`, `A` the actor's reference), the culture-gated
//: alliance, a subjugation offer, a confederation, the liege's war.
struct WhyCheck { const char* word; const char* trigger; };
inline constexpr WhyCheck kDeclareWhy[] = {
    {"RAISED_ARMIES", "has_raised_armies = yes"},
    {"IN_DEBT", "is_in_debt = yes"},
    {"IN_ACTIVITY", "exists = involved_activity"},
    {"IMPRISONED", "is_imprisoned = yes"},
    {"INCAPABLE", "has_trait = incapable"},
    {"SWORN_PEACE", "has_character_flag = sworn_peace"},
    {"AT_WAR_WITH", "is_at_war_with = D"},
    {"ALLIED_IN_WAR", "is_allied_in_war = D"},
    {"TRUCE", "has_truce = D"},
    {"HIERARCHY", "NOT = { can_attack_in_hierarchy = D }"},
    {"NO_DISPLAY_CB", "NOT = { has_any_display_cb_on = D }"},
    {"PRESTIGE_BELOW_0", "prestige < 0"},
    //
    {"ALLIED_NO_ATTACK", "culture = { has_cultural_parameter = cannot_attack_allies } is_allied_to = D"},
    {"SUBJUGATION_OFFER", "has_variable_list = subjugation_offer_under_consideration is_target_in_variable_list = { name = subjugation_offer_under_consideration target = D }"},
    {"LIEGE_AT_WAR", "liege ?= { is_at_war_with = D }"},
    {"CONFEDERATION", "is_confederation_member = yes D = { is_member_of_confederation = A.confederation }"},
    {"TARGET_HERDER", "D = { government_has_flag = government_is_herder }"},
    {"TARGET_TRIBUTARY", "D = { is_tributary_of_suzerain_or_above = A }"},
    {"TARGET_STRONG_HOOK", "D = { has_strong_hook = A }"},
    {"TARGET_YOUR_PRISONER", "D = { is_imprisoned = yes imprisoner = A }"},
    {"TARGET_NOT_PLAYABLE", "D = { is_playable_character = no }"},
};

//: A trigger with its references filled: the word `D` (the defender) and `A` (the actor, also
//: as the head of a link such as `A.confederation`), whole words only.
inline std::string why_trigger(const char* trigger, const std::string& A, const std::string& D) {
    std::string out, w;
    auto flush = [&]() {
        if (w == "D") out += D;
        else if (w == "A") out += A;
        else if (w.size() > 2 && w[0] == 'A' && w[1] == '.') out += A + w.substr(1);
        else out += w;
        w.clear();
    };
    for (const char* p = trigger; *p; ++p) {
        if (*p == ' ') { flush(); out += ' '; }
        else w += *p;
    }
    flush();
    return out;
}

inline std::string declare_why_listing(const std::string& token, const std::string& D, const std::string& A) {
    std::string out;
    for (const auto& w : kDeclareWhy)
        out += " if = { limit = { " + why_trigger(w.trigger, A, D) + " } " + log_line(token + "_WHY_" + w.word) + " }";
    return out;
}

inline Composed compose(Template t, const Slots& s) {
    if (!token_ok(s.token)) return refuse("bad_token");
    const std::string ran = log_line(ran_marker(s.token));
    const std::string refused = log_line(refused_marker(s.token));
    auto need = [](const std::string& k) { return is_ident(k); };
    // Every title slot becomes a title: reference (holder_ref / title_ref /
    // province_ref), so each must be a key script resolves.
    auto ref = [](const std::string& k) { return is_script_title_key(k); };
    Composed c;
    switch (t) {
    case Template::kDeclareWar: {
        if (!ref(s.actor_title) || !ref(s.target_title)) return refuse("bad_character_ref");
        if (!need(s.cb)) return refuse("bad_cb_key");
        if (!s.claimant_title.empty() && !ref(s.claimant_title)) return refuse("bad_claimant_ref");
        if (!s.war_title.empty() && !ref(s.war_title)) return refuse("bad_title_key");
        const std::string A = holder_ref(s.actor_title), D = holder_ref(s.target_title);
        std::string guard_extra, war_extra;
        if (!s.claimant_title.empty()) {
            guard_extra += " claimant = " + holder_ref(s.claimant_title);
            war_extra += " claimant = " + holder_ref(s.claimant_title);
        }
        if (!s.war_title.empty()) {
            guard_extra += " target_titles = { " + title_ref(s.war_title) + " }";
            war_extra += " target_title = " + title_ref(s.war_title);
        }
        // (1087-1096: 36 refusals with none of the interaction's conditions failing):
        // the game's own probes of the casus belli's part, so a refusal says which part said no:
        // has_cb_on (the casus belli against him at all), and can_declare_war without the claimant
        // and without the title (each passing narrows it to the argument left out).
        std::string probes = " " + log_line(s.token + "_PROBED") + " if = { limit = { NOT = { has_cb_on = { target = " + D + " casus_belli = " + s.cb +
                             " } } } " + log_line(s.token + "_WHY_NO_CB_ON") + " }";
        probes += " if = { limit = { can_declare_war = { defender = " + D + " casus_belli = " + s.cb +
                  " } } " + log_line(s.token + "_PROBE_BARE_OK") + " }";
        if (!s.war_title.empty())
            probes += " if = { limit = { can_declare_war = { defender = " + D + " casus_belli = " + s.cb +
                      " target_titles = { " + title_ref(s.war_title) + " } } } " +
                      log_line(s.token + "_PROBE_TITLE_OK") + " }";
        // (1080.07: two vassalization declarations refused at 50.07 prestige, "which
        // no read names"; the price was 125 a county): the casus belli's own price, evaluated by the game in
        // the refused branch, against your prestige and piety (WHY_PRESTIGE_BELOW_COST) and as marks COST*
        const int pi = cb_price_index(s.cb);
        if (pi >= 0 && (!kCbPrices[pi].needs_title || !s.war_title.empty()))
            probes += price_scopes(D, s.claimant_title.empty() ? "" : holder_ref(s.claimant_title),
                                   s.war_title.empty() ? "" : title_ref(s.war_title)) +
                      price_block(kCbPrices[pi], s.token, "COST", true);
        c.script = A + " = { if = { limit = { can_declare_war = { defender = " + D +
                   " casus_belli = " + s.cb + guard_extra + " } } start_war = { cb = " + s.cb +
                   " target = " + D + war_extra + " } " + ran + " } else = {" +
                   declare_why_listing(s.token, D, A) + probes + " " + refused + " } }";
        break;
    }
    case Template::kGift:
    case Template::kSway: {
        if (!ref(s.actor_title) || !ref(s.target_title)) return refuse("bad_character_ref");
        const char* itype = (t == Template::kGift) ? "gift_interaction" : "sway_interaction";
        // run_interaction checks the interaction's own validity and logs the
        // game's reason ("Interaction cannot be sent. Reason: ...") to error.log;
        // `accept` runs it only when the recipient's AI accepts. The marker says
        // the script ran, not that the interaction did: the read-back decides.
        c.script = std::string("run_interaction = { interaction = ") + itype + " actor = " +
                   holder_ref(s.actor_title) + " recipient = " + holder_ref(s.target_title) +
                   " execute_threshold = accept } " + ran;
        break;
    }
    case Template::kCouncilTask: {
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        if (!need(s.seat) || s.seat.rfind("councillor_", 0) != 0) return refuse("bad_seat_key");
        if (!need(s.task)) return refuse("bad_task_key");
        if (!s.task_target_char.empty() && !s.task_target_county.empty())
            return refuse("two_task_targets");
        std::string target;
        if (!s.task_target_char.empty()) {
            if (!ref(s.task_target_char)) return refuse("bad_character_ref");
            target = " target = " + holder_ref(s.task_target_char);
        } else if (!s.task_target_county.empty()) {
            if (!ref(s.task_target_county) || s.task_target_county.rfind("c_", 0) != 0)
                return refuse("bad_county_key");
            target = " target = " + province_ref(s.task_target_county);
        }
        const std::string seat = "cp:" + s.seat;
        c.script = holder_ref(s.actor_title) + " = { if = { limit = { exists = " + seat + " } " +
                   seat + " = { set_council_task = { task_type = " + s.task + target + " } } " +
                   ran + " } else = { " + refused + " } }";
        break;
    }
    case Template::kSetPolicy:
    case Template::kClearPolicy: {
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        const char* var = policy_variable(s.policy);
        if (!var) return refuse("unknown_policy");
        const std::string A = holder_ref(s.actor_title);
        if (t == Template::kSetPolicy) {
            // The variable first, then the mod's event, armed once per character:
            // the event re-arms itself every 3 days while any policy is set and
            // clears kPolicyArmedVar when none is (ck3agent/policy_mod/).
            c.script = A + " = { set_variable = { name = " + var + " value = 1 } if = { limit = { NOT = { has_variable = " +
                       kPolicyArmedVar + " } } set_variable = { name = " + kPolicyArmedVar +
                       " value = 1 } trigger_event = " + kPolicyEvent + " } if = { limit = { has_variable = " +
                       var + " } " + ran + " } else = { " + refused + " } }";
        } else {
            c.script = A + " = { remove_variable = " + var + " if = { limit = { has_variable = " + var +
                       " } " + refused + " } else = { " + ran + " } }";
        }
        break;
    }
    case Template::kProposalSeat: {
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        const std::string A = holder_ref(s.actor_title), v = kProposalSeatVar;
        // The marker is the only thing that makes the mod capture an offer to this
        // character; nothing else sets it (test_proposal_mod.py polarity row).
        if (s.proposal_on)
            c.script = A + " = { set_variable = { name = " + v + " value = 1 days = " +
                       std::to_string(kProposalSeatDays) + " } if = { limit = { has_variable = " + v +
                       " } " + ran + " } else = { " + refused + " } }";
        else
            c.script = A + " = { remove_variable = " + v + " if = { limit = { has_variable = " + v + " } " +
                       refused + " } else = { " + ran + " } }";
        break;
    }
    case Template::kProposalPolicy: {
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        if (!proposal_family(s.proposal_class)) return refuse("bad_class");
        if (!word_of(kProposalAnswers, s.proposal_answer)) return refuse("bad_answer");
        const std::string var = std::string(kProposalAnswerVarPrefix) + s.proposal_class;
        const std::string val = std::to_string(s.proposal_answer);
        c.script = holder_ref(s.actor_title) + " = { set_variable = { name = " + var + " value = " + val +
                   " } if = { limit = { has_variable = " + var + " var:" + var + " = " + val + " } " + ran +
                   " } else = { " + refused + " } }";
        break;
    }
    case Template::kProposalList:
    case Template::kProposalAnswer: {
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        const std::string begin = log_line(s.token + "_BEGIN");
        const std::string end = log_line(s.token + "_END");
        if (t == Template::kProposalList) {
            c.script = holder_ref(s.actor_title) + " = { " + begin + " every_owned_story = { limit = { story_type = " +
                       kProposalStory + " ck3agent_proposal_open_trigger = yes } "
                       "ck3agent_proposal_list_item_effect = yes } " + end + " " + ran + " }";
            break;
        }
        if (!proposal_id_ok(s.proposal_id)) return refuse("bad_proposal_id");
        if (s.proposal_answer != 1 && s.proposal_answer != 2 && s.proposal_answer != 4) return refuse("bad_answer");
        if (s.proposal_by != 1 && s.proposal_by != 3 && s.proposal_by != 6) return refuse("bad_by");
        c.script = holder_ref(s.actor_title) + " = { " + begin + " every_owned_story = { limit = { story_type = " +
                   kProposalStory + " var:ck3agent_proposal_id = " + std::to_string(s.proposal_id) +
                   " } save_scope_as = ck3a_found ck3agent_proposal_answer_effect = { ANSWER = " +
                   std::to_string(s.proposal_answer) + " BY = " + std::to_string(s.proposal_by) +
                   " } } if = { limit = { NOT = { exists = scope:ck3a_found } } " +
                   log_line(std::string(kProposalWordPrefix) + "OUTCOME|refused|not_found") + " } " + end + " " + ran + " }";
        break;
    }
    case Template::kEndWar: {
        // Inside the war scope: the war the actor and the target LEAD on
        // opposite sides. The listing (which terms are open, our score band)
        // is written to debug.log FIRST, so the reading stands beside the write
        // whatever the guard decides; then the guarded end_war. end_war runs
        // the casus belli's own outcome (attacker = the attacker wins); the
        // enemy is not asked.
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        if (s.enemy_landless ? !s.target_title.empty() : !ref(s.target_title)) return refuse("bad_character_ref");
        if (!s.peace_terms.empty() && !is_peace_terms(s.peace_terms)) return refuse("bad_peace_terms");
        // (1092.01.06: a populist rebel at +100 could not be named by a title):
        // a landless enemy leader is matched as `{ is_landed = no }`, and the war must be the only
        // one the actor leads against such a leader (AMBIGUOUS otherwise: nothing is ended)
        const std::string A = holder_ref(s.actor_title);
        const std::string E = s.enemy_landless ? std::string("{ is_landed = no }") : holder_ref(s.target_title);
        auto mark = [&](const std::string& m) { return log_line(s.token + "_" + m); };
        const std::string between = "OR = { AND = { primary_attacker = " + A + " primary_defender = " + E +
                                    " } AND = { primary_attacker = " + E + " primary_defender = " + A + " } }";
        auto side_score = [&](const std::string& who, const char* op, int v) {
            const std::string n = std::to_string(v);
            return "OR = { AND = { primary_attacker = " + who + " attacker_war_score " + op + " " + n +
                   " } AND = { primary_defender = " + who + " defender_war_score " + op + " " + n + " } }";
        };
        const std::string open_win = side_score(A, ">=", kWeWinScore);
        const std::string open_white = side_score(E, "<", kWhitePeaceEnemyBelow);
        std::string listing = mark("LEADS") + " if = { limit = { " + open_win + " } " + mark("OPEN_WE_WIN") +
                              " } if = { limit = { " + open_white + " } " + mark("OPEN_WHITE_PEACE") + " } " +
                              mark("OPEN_WE_YIELD");
        for (int v : kScoreBands) {
            const std::string band = v < 0 ? "M" + std::to_string(-v) : std::to_string(v);
            listing += " if = { limit = { " + side_score(A, ">=", v) + " } " + mark("SCORE_GE_" + band) + " }";
        }
        std::string act;
        if (s.peace_terms.empty()) {
            act = ran;                                   // read only: the listing is the answer
        } else {
            std::string open, end;
            if (s.peace_terms == "we_win") {
                open = open_win;
                end = "if = { limit = { primary_attacker = " + A + " } end_war = attacker } else = { end_war = defender }";
            } else if (s.peace_terms == "white_peace") {
                open = open_white;
                end = "end_war = white_peace";
            } else {                                     // we_yield: always open
                open = "always = yes";
                end = "if = { limit = { primary_attacker = " + A + " } end_war = defender } else = { end_war = attacker }";
            }
            act = "if = { limit = { any_character_war = { " + between + " " + open +
                  " } } random_character_war = { limit = { " + between + " " + open + " } " + end + " } " +
                  ran + " } else = { " + refused + " }";
            if (s.enemy_landless)
                act = "if = { limit = { any_character_war = { count >= 2 " + between + " } } " + mark("AMBIGUOUS") +
                      " " + refused + " } else = { " + act + " }";
        }
        c.script = A + " = { if = { limit = { any_character_war = { " + between +
                   " } } random_character_war = { limit = { " + between + " } " + listing + " } " + act +
                   " } else = { " + mark("NO_WAR") + " " + refused + " } }";
        break;
    }
    case Template::kGoldReserve: {
        // The actor's own scope only. N = 0 releases the reserve: its gold back to
        // short term (the cap alone "will not correct the current gold").
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        if (s.reserve_gold < 0 || s.reserve_gold > kReserveMax) return refuse("bad_reserve_amount");
        const std::string n = std::to_string(s.reserve_gold);
        std::string body = "set_reserved_gold_maximum = " + n;
        if (s.reserve_gold == 0) {
            body += " if = { limit = { reserved_gold > 0 } move_budget_gold = { gold = reserved_gold "
                    "from = budget_reserved to = budget_short_term } }";
        } else if (s.reserve_move) {
            for (const auto& src : kReserveSources)
                body += std::string(" if = { limit = { ") + src.trigger + " > 0 } move_budget_gold = { gold = " +
                        src.trigger + " from = " + src.category + " to = budget_reserved } }";
        }
        c.script = holder_ref(s.actor_title) + " = { " + body + " " + ran + " }";
        break;
    }
    case Template::kActivity: {
        // The actor's own scope only. The listing (each failing gate, each type's
        // verdict by the game's can_host_activity, its cooldown and the count's
        // AI-value throttle, the activity he is in) is written FIRST, so it stands
        // beside the write whatever the guard decides; then, for a host call, the
        // type is put to his AI, guarded by can_host_activity.
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        const ActivityType* want = nullptr;
        if (!s.activity.empty()) {
            want = activity_type(s.activity);
            if (!want) return refuse("bad_activity");
        }
        if (s.activity_host && !want) return refuse("activity_required");
        if (s.activity_leave && (s.activity_host || want)) return refuse("leave_takes_no_activity");
        auto mark = [&](const std::string& m) { return log_line(s.token + "_" + m); };
        std::string body = "save_scope_as = ck3a_me";
        for (const auto& g : kActivityGates)
            body += std::string(" if = { limit = { ") + g.trigger + " } " + mark(g.word) + " }";
        // Which branch of the build-focus condition holds, at which province
        body += " if = { limit = { exists = capital_county } capital_county.title_province = { save_scope_as = ck3a_bfp }";
        for (const auto& b : kBuildFocusBranches)
            body += std::string(" if = { limit = { ") + b.trigger + " } " + mark(b.word) + " }";
        body += " }";
        for (const auto& a : kActivityTypes) {
            const std::string w = a.word;
            body += std::string(" if = { limit = { can_host_activity = ") + a.key + " } " + mark(w + "_CAN_HOST") + " }";
            body += std::string(" if = { limit = { is_activity_type_on_cooldown = ") + a.key + " } " +
                    mark(w + "_COOLDOWN") + " }";
            body += std::string(" if = { limit = { ") + a.axis + " < 100 } " + mark(w + "_AXIS_BELOW_100") + " }";
        }
        body += " if = { limit = { exists = involved_activity } involved_activity = {";
        for (const auto& a : kActivityTypes)
            body += std::string(" if = { limit = { has_activity_type = ") + a.key + " } " +
                    mark(std::string("IN_") + a.word) + " }";
        for (size_t i = 0; i < sizeof kInvolvedActivityTypes / sizeof kInvolvedActivityTypes[0]; ++i)
            body += std::string(" if = { limit = { has_activity_type = ") + kInvolvedActivityTypes[i] + " } " +
                    mark("IN_T_" + std::to_string(i)) + " }";
        body += " if = { limit = { activity_host = scope:ck3a_me } " + mark("HOSTING") + " } } }";
        std::string act;
        if (s.activity_host)
            act = std::string("if = { limit = { can_host_activity = ") + want->key + " } ai_attempt_to_host_activity = " +
                  want->key + " " + ran + " } else = { " + refused + " }";
        else if (s.activity_leave)
            // A recorded game refused declarations involved_activity for nine turns while the ruler
            // was a guest at a coronation. This is the game's own way home for a guest, word for word
            // (events/activities/activity_system_events.txt activity_system.0400 "Go home":
            // remove_from_activity = involved_activity, return_home = yes). A host is refused: the
            // game ends a host's activity with invalidate_activity (prison_on_actions.txt), not this.
            act = "if = { limit = { exists = involved_activity NOT = { involved_activity.activity_host = scope:ck3a_me } } "
                  "remove_from_activity = involved_activity return_home = yes " + ran + " } else = { " + refused + " }";
        else
            act = ran;
        c.script = holder_ref(s.actor_title) + " = { " + body + " " + act + " }";
        break;
    }
    case Template::kClaimPriceFacts: {
        // The actor's own scope only, read only: the claim_cb price's legitimacy
        // flag (the two lists, each an if / else_if chain as 00_claim.txt), whether
        // he is an adult (CB_COST_ATTACKER_IS_CHILD x1.5), his sex and his faith's
        // gender doctrine (x0.75 / x1.25 against a ruler of his faith and the other sex).
        if (!ref(s.actor_title)) return refuse("bad_character_ref");
        auto mark = [&](const std::string& m) { return log_line(s.token + "_" + m); };
        auto chain = [&](const auto& list) {
            std::string out;
            bool first = true;
            for (const auto& f : list) {
                out += std::string(first ? " if" : " else_if") + " = { limit = { has_legitimacy_flag = " + f.flag +
                       " } " + mark(f.word) + " }";
                first = false;
            }
            return out;
        };
        std::string body = mark("READ") + chain(kLegitimacyUp) + chain(kLegitimacyDown) +
                           " if = { limit = { is_adult = no } " + mark("CHILD") + " }" +
                           " if = { limit = { is_male = yes } " + mark("MALE") + " }" +
                           " if = { limit = { faith = { has_doctrine = doctrine_gender_male_dominated } } " +
                           mark("FAITH_MALE_DOMINATED") + " } else_if = { limit = { faith = { has_doctrine = "
                           "doctrine_gender_female_dominated } } " + mark("FAITH_FEMALE_DOMINATED") + " }";
        c.script = holder_ref(s.actor_title) + " = { " + body + " " + ran + " }";
        break;
    }
    case Template::kCbPrices: {
        // Read only: the price of declaring each named casus belli on the defender, the game's own cost
        // blocks evaluated in the war's scopes (kCbPrices), marked C<j> by the key's position j in the request;
        // a casus belli whose value reads target_titles with no title given writes <token>_C<j>_NOTITLE.
        // Every key must be a row of kCbPrices.
        if (!ref(s.actor_title) || !ref(s.target_title)) return refuse("bad_character_ref");
        if (!s.claimant_title.empty() && !ref(s.claimant_title)) return refuse("bad_claimant_ref");
        if (!s.war_title.empty() && !ref(s.war_title)) return refuse("bad_title_key");
        if (s.price_cbs.empty() || s.price_cbs.size() > 6) return refuse("bad_cb_list");
        std::string body = price_scopes(holder_ref(s.target_title),
                                        s.claimant_title.empty() ? "" : holder_ref(s.claimant_title),
                                        s.war_title.empty() ? "" : title_ref(s.war_title)) +
                           " " + log_line(s.token + "_READ");
        for (size_t j = 0; j < s.price_cbs.size(); ++j) {
            const int i = cb_price_index(s.price_cbs[j]);
            if (i < 0) return refuse("bad_cb_key");
            const std::string w = "C" + std::to_string(j);   // the key's position in the request
            if (kCbPrices[i].needs_title && s.war_title.empty())
                body += " " + log_line(s.token + "_" + w + "_NOTITLE");
            else
                body += price_block(kCbPrices[i], s.token, w, false);
        }
        c.script = holder_ref(s.actor_title) + " = {" + body + " " + ran + " }";
        break;
    }
    default:
        return refuse("unknown_template");
    }
    c.ok = true;
    return c;
}

// ── reading the outcome back from the game's own logs ───────────────────────

enum class Marker : uint8_t { kUnseen = 0, kRan, kRefused };

//: Which of the token's two markers the debug.log text holds (the last wins).
inline Marker marker_in(std::string_view text, const std::string& token) {
    const std::string r = ran_marker(token), f = refused_marker(token);
    size_t pr = std::string::npos, pf = std::string::npos, p = 0;
    // Matched as a whole word, so SACT_1_RAN is not found inside SACT_11_RAN.
    auto find_word = [&](const std::string& w) {
        size_t last = std::string::npos;
        for (p = text.find(w); p != std::string::npos; p = text.find(w, p + 1)) {
            const size_t e = p + w.size();
            const bool start_ok = p == 0 || !(std::isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_');
            const bool end_ok = e >= text.size() || !(std::isalnum((unsigned char)text[e]) || text[e] == '_');
            if (start_ok && end_ok) last = p;
        }
        return last;
    };
    pr = find_word(r);
    pf = find_word(f);
    if (pr == std::string::npos && pf == std::string::npos) return Marker::kUnseen;
    if (pf == std::string::npos) return Marker::kRan;
    if (pr == std::string::npos) return Marker::kRefused;
    return pr > pf ? Marker::kRan : Marker::kRefused;
}

//: Every other mark the token's script wrote (<token>_<WORD>,
//: WORD of [A-Z0-9_], not RAN / REFUSED), in first-seen order, each once.
//: end_war writes its reading this way (LEADS, OPEN_WE_WIN, SCORE_GE_50, ...).
inline std::vector<std::string> token_marks(std::string_view text, const std::string& token,
                                            size_t max_out = 1024) {
    std::vector<std::string> out;
    const std::string pre = token + "_";
    for (size_t p = text.find(pre); p != std::string_view::npos; p = text.find(pre, p + 1)) {
        if (p > 0 && (std::isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_')) continue;
        size_t e = p + pre.size();
        while (e < text.size() && ((text[e] >= 'A' && text[e] <= 'Z') || (text[e] >= '0' && text[e] <= '9') ||
                                   text[e] == '_'))
            ++e;
        if (e < text.size() && (std::isalnum((unsigned char)text[e]))) continue;  // a lower-case tail: not ours
        std::string w(text.substr(p + pre.size(), e - p - pre.size()));
        if (w.empty() || w == "RAN" || w == "REFUSED") continue;
        bool seen = false;
        for (const auto& x : out) seen = seen || x == w;
        if (!seen && out.size() < max_out) out.push_back(w);
    }
    return out;
}

inline const char* marker_name(Marker m) {
    switch (m) {
    case Marker::kRan: return "ran";
    case Marker::kRefused: return "refused";
    default: return "unseen";
    }
}

//: The script-system errors in error.log text that belong to a console effect:
//: each "Script system error!" block whose "Script location: file: effect
//: console command" line closes it. Returns each block's "Error:" text (with its
//: continuation lines, joined by a space), in order. This is the game's own
//: reason for a refused run_interaction ("Interaction cannot be sent. Reason:
//: ... No target selected", a probe). Blocks from anything else (the game's
//: own scripts, a mod's event) are not ours and are skipped.
inline std::vector<std::string> console_effect_errors(std::string_view text, size_t max_out = 8) {
    std::vector<std::string> out;
    std::vector<std::string_view> lines;
    for (size_t s = 0; s <= text.size();) {
        size_t e = text.find('\n', s);
        if (e == std::string_view::npos) e = text.size();
        std::string_view l = text.substr(s, e - s);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        lines.push_back(l);
        s = e + 1;
        if (e == text.size()) break;
    }
    auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
        return v;
    };
    std::string cur;
    bool in_error = false;
    for (std::string_view l : lines) {
        std::string_view t = trim(l);
        if (t.find("Script system error!") != std::string_view::npos) {
            cur.clear(); in_error = false;
            continue;
        }
        if (t.rfind("Error:", 0) == 0) {
            cur = std::string(trim(t.substr(6)));
            in_error = true;
            continue;
        }
        if (t.rfind("Script location:", 0) == 0) {
            if (in_error && t.find("effect console command") != std::string_view::npos &&
                out.size() < max_out)
                out.push_back(cur);
            cur.clear(); in_error = false;
            continue;
        }
        if (in_error && !t.empty() && t.front() != '[') {
            if (!cur.empty()) cur += ' ';
            cur += std::string(t);
        } else if (!t.empty() && t.front() == '[') {
            in_error = false; cur.clear();
        }
    }
    return out;
}

// ── the proposal queue's lines ──────────────────────────────────
// The mod writes `CK3AGENT_PROPOSAL_<WORD>|f1|f2|...` (the effects file's
// header names every word and field); the game prefixes its own
// "[time][D][file:line]: file: ... line: N (ctx): ". Numbers are printed by
// localization data functions (ScriptValue|0, Character.GetID), so a field is
// read leniently for grouping commas and a ".000" tail, and anything else is
// unread (-1), never a guessed number.

inline std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    for (size_t s = 0; s < text.size();) {
        size_t e = text.find('\n', s);
        if (e == std::string_view::npos) e = text.size();
        std::string_view l = text.substr(s, e - s);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        out.push_back(l);
        s = e + 1;
    }
    return out;
}

//: A number the game printed: digits with optional ',' / ' ' grouping and an
//: optional all-zero fraction. -1 when it is anything else.
inline long long loc_int(std::string_view f) {
    std::string d;
    size_t i = 0;
    while (i < f.size() && f[i] == ' ') ++i;
    for (; i < f.size(); ++i) {
        const char ch = f[i];
        if (ch >= '0' && ch <= '9') d += ch;
        else if (ch == ',' || ch == ' ') continue;
        else break;
    }
    if (i < f.size()) {
        if (f[i] != '.') return -1;
        for (++i; i < f.size(); ++i)
            if (f[i] != '0' && f[i] != ' ') return -1;
    }
    if (d.empty() || d.size() > 15) return -1;
    return std::stoll(d);
}

//: The `CK3AGENT_PROPOSAL_...` text of a debug.log line, split on '|'; empty
//: when the line holds none.
inline std::vector<std::string> proposal_parts(std::string_view line) {
    std::vector<std::string> parts;
    const size_t p = line.find(kProposalWordPrefix);
    if (p == std::string_view::npos) return parts;
    std::string_view t = line.substr(p);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
    size_t s = 0;
    for (;;) {
        const size_t e = t.find('|', s);
        parts.emplace_back(t.substr(s, e == std::string_view::npos ? std::string_view::npos : e - s));
        if (e == std::string_view::npos) break;
        s = e + 1;
    }
    return parts;
}

//: A date the mod printed as y.m.d (three loc_int fields): "Y.M.D", or "".
inline std::string proposal_date(std::string_view f) {
    long long v[3];
    size_t s = 0;
    for (int k = 0; k < 3; ++k) {
        size_t e = f.find('.', s);
        if (k < 2 && e == std::string_view::npos) return {};
        std::string_view piece = f.substr(s, (k < 2) ? e - s : std::string_view::npos);
        v[k] = loc_int(piece);
        if (v[k] < 0) return {};
        s = e + 1;
    }
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
}

//: One HELD / RESOLVED / VOID line as the contract's {word, fields[], date}.
//:   HELD      fields: id, kind, class, actor_id, recipient_id
//:   RESOLVED  fields: id, answer, by, rule, kind, class
//:             (rule = "class_policy:<class>=<answer>" when by is rule, else "")
//:   VOID      fields: id, reason, kind, class
//: A code the bridge does not know is kept as "unknown:<raw>", never dropped.
struct ProposalLogLine {
    std::string word;
    std::vector<std::string> fields;
    std::string date;
};
inline bool parse_proposal_log_line(std::string_view line, ProposalLogLine* out) {
    const auto p = proposal_parts(line);
    if (p.empty()) return false;
    auto num = [](const std::string& f) {
        const long long v = loc_int(f);
        return v < 0 ? "unread:" + f : std::to_string(v);
    };
    auto code = [](auto& table, const std::string& f) -> std::string {
        const char* w = word_of(table, loc_int(f));
        return w ? std::string(w) : "unknown:" + f;
    };
    ProposalLogLine r;
    r.word = p[0];
    if (r.word == "CK3AGENT_PROPOSAL_HELD" && p.size() == 7) {
        r.fields = {num(p[1]), p[2], p[3], num(p[4]), num(p[5])};
        r.date = proposal_date(p[6]);
    } else if (r.word == "CK3AGENT_PROPOSAL_RESOLVED" && p.size() == 7) {
        const std::string ans = code(kProposalAnswers, p[2]), by = code(kProposalBy, p[3]);
        r.fields = {num(p[1]), ans, by, by == "rule" ? "class_policy:" + p[5] + "=" + ans : "", p[4], p[5]};
        r.date = proposal_date(p[6]);
    } else if (r.word == "CK3AGENT_PROPOSAL_VOID" && p.size() == 6) {
        r.fields = {num(p[1]), code(kProposalVoid, p[2]), p[3], p[4]};
        r.date = proposal_date(p[5]);
    } else {
        return false;
    }
    *out = std::move(r);
    return true;
}

//: The lines a template call wrote between <token>_BEGIN and <token>_END
//: (whole-word markers; the last BEGIN wins). complete = both were seen.
struct ProposalWindow { bool begun = false, complete = false; std::vector<std::string> lines; };
inline ProposalWindow proposal_window(std::string_view text, const std::string& token) {
    const std::string b = token + "_BEGIN", e = token + "_END";
    auto ends_with_word = [](std::string_view l, const std::string& w) {
        size_t p = l.rfind(w);
        if (p == std::string_view::npos) return false;
        const size_t q = p + w.size();
        const bool start_ok = p == 0 || !(std::isalnum((unsigned char)l[p - 1]) || l[p - 1] == '_');
        const bool end_ok = q >= l.size() || !(std::isalnum((unsigned char)l[q]) || l[q] == '_');
        return start_ok && end_ok;
    };
    ProposalWindow w;
    for (std::string_view l : split_lines(text)) {
        if (ends_with_word(l, b)) { w = ProposalWindow{}; w.begun = true; continue; }
        if (!w.begun || w.complete) continue;
        if (ends_with_word(l, e)) { w.complete = true; continue; }
        if (l.find(kProposalWordPrefix) != std::string_view::npos) w.lines.emplace_back(l);
    }
    return w;
}

//: One item of proposal_list's reply. -1 = absent (secondaries) or unread.
struct ProposalItem {
    long long id = -1, actor_id = -1, recipient_id = -1, secondary_actor_id = -1, secondary_recipient_id = -1;
    long long age_days_min = -1;
    std::string kind, cls, war_name;
    bool has_war = false;
    std::vector<std::string> options;
};
//: The ITEM lines of a window and the lines that follow each (secondaries, the
//: war's name, the options). Lines before the first ITEM are not an item's.
inline std::vector<ProposalItem> proposal_items(const std::vector<std::string>& lines) {
    std::vector<ProposalItem> out;
    for (const auto& l : lines) {
        const auto p = proposal_parts(l);
        if (p.empty()) continue;
        if (p[0] == "CK3AGENT_PROPOSAL_ITEM" && p.size() == 7) {
            ProposalItem it;
            it.id = loc_int(p[1]);
            it.kind = p[2];
            it.cls = p[3];
            it.actor_id = loc_int(p[4]);
            it.recipient_id = loc_int(p[5]);
            const long long steps = loc_int(p[6]);
            it.age_days_min = steps < 0 ? -1 : steps * 10;
            out.push_back(std::move(it));
            continue;
        }
        if (out.empty() || p.size() != 2) continue;
        ProposalItem& it = out.back();
        if (p[0] == "CK3AGENT_PROPOSAL_ITEM_SECONDARY_ACTOR") it.secondary_actor_id = loc_int(p[1]);
        else if (p[0] == "CK3AGENT_PROPOSAL_ITEM_SECONDARY_RECIPIENT") it.secondary_recipient_id = loc_int(p[1]);
        else if (p[0] == "CK3AGENT_PROPOSAL_ITEM_WAR") { it.war_name = p[1]; it.has_war = true; }
        else if (p[0] == "CK3AGENT_PROPOSAL_ITEM_OPTION" && is_ident(p[1])) it.options.push_back(p[1]);
    }
    return out;
}

//: proposal_answer's outcome: the last OUTCOME line of the window.
struct ProposalOutcome { std::string outcome, reason; };
inline ProposalOutcome proposal_outcome(const std::vector<std::string>& lines) {
    ProposalOutcome o;
    for (const auto& l : lines) {
        const auto p = proposal_parts(l);
        if (p.size() == 3 && p[0] == "CK3AGENT_PROPOSAL_OUTCOME") {
            o.outcome = p[1];
            o.reason = p[2] == "none" ? "" : p[2];
        }
    }
    return o;
}

}  // namespace bridge::script_tpl
