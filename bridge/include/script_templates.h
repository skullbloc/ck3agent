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
// runtime ids the bridge reads (probe e12b: "Referencing non-existent character
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
    // docs/design/incoming-proposals.md). Each enters the SEAT's own scope
    // through his primary title, so it cannot reach an offer to anyone else.
    kProposalSeat,   // the seat marker on (400-day timed variable) or off
    kProposalPolicy, // the seat's answer for one class
    kProposalList,   // the seat's open offers, between <token>_BEGIN and <token>_END
    kProposalAnswer, // one offer answered through the mod's one apply effect
    kEndWar,         // lane peace: a war the actor leads against the target ends (end_war)
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
};

//: lane peace: the terms of end_war, NAMED FROM THE ACTOR'S SIDE. The other
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
//: probe's mod: "modprobe|mod/modprobe.mod|Enabled", lane modprobe e3a).
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
    std::string token;             // SACT_<n>
    // The proposal templates
    bool proposal_on = false;      // proposal_seat
    std::string proposal_class;    // proposal_policy
    int proposal_answer = 0;       // proposal_policy (1-4) / proposal_answer (1, 2, 4)
    int proposal_by = 0;           // proposal_answer (1, 3, 6)
    long long proposal_id = 0;     // proposal_answer
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
//: re-pilot's channel died at 1088.01.27 when it filled); INFO is near-unused by
//: the game (2 lines in the re-pilot's 21 years), so our lines get their own
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

inline Composed compose(Template t, const Slots& s) {
    if (!token_ok(s.token)) return refuse("bad_token");
    const std::string ran = log_line(ran_marker(s.token));
    const std::string refused = log_line(refused_marker(s.token));
    auto need = [](const std::string& k) { return is_ident(k); };
    Composed c;
    switch (t) {
    case Template::kDeclareWar: {
        if (!need(s.actor_title) || !need(s.target_title)) return refuse("bad_character_ref");
        if (!need(s.cb)) return refuse("bad_cb_key");
        if (!s.claimant_title.empty() && !need(s.claimant_title)) return refuse("bad_claimant_ref");
        if (!s.war_title.empty() && !need(s.war_title)) return refuse("bad_title_key");
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
        c.script = A + " = { if = { limit = { can_declare_war = { defender = " + D +
                   " casus_belli = " + s.cb + guard_extra + " } } start_war = { cb = " + s.cb +
                   " target = " + D + war_extra + " } " + ran + " } else = { " + refused + " } }";
        break;
    }
    case Template::kGift:
    case Template::kSway: {
        if (!need(s.actor_title) || !need(s.target_title)) return refuse("bad_character_ref");
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
        if (!need(s.actor_title)) return refuse("bad_character_ref");
        if (!need(s.seat) || s.seat.rfind("councillor_", 0) != 0) return refuse("bad_seat_key");
        if (!need(s.task)) return refuse("bad_task_key");
        if (!s.task_target_char.empty() && !s.task_target_county.empty())
            return refuse("two_task_targets");
        std::string target;
        if (!s.task_target_char.empty()) {
            if (!need(s.task_target_char)) return refuse("bad_character_ref");
            target = " target = " + holder_ref(s.task_target_char);
        } else if (!s.task_target_county.empty()) {
            if (!need(s.task_target_county) || s.task_target_county.rfind("c_", 0) != 0)
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
        if (!need(s.actor_title)) return refuse("bad_character_ref");
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
        if (!need(s.actor_title)) return refuse("bad_character_ref");
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
        if (!need(s.actor_title)) return refuse("bad_character_ref");
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
        if (!need(s.actor_title)) return refuse("bad_character_ref");
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
        if (!need(s.actor_title) || !need(s.target_title)) return refuse("bad_character_ref");
        if (!s.peace_terms.empty() && !is_peace_terms(s.peace_terms)) return refuse("bad_peace_terms");
        const std::string A = holder_ref(s.actor_title), E = holder_ref(s.target_title);
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
        }
        c.script = A + " = { if = { limit = { any_character_war = { " + between +
                   " } } random_character_war = { limit = { " + between + " } " + listing + " } " + act +
                   " } else = { " + mark("NO_WAR") + " " + refused + " } }";
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

//: lane peace: every other mark the token's script wrote (<token>_<WORD>,
//: WORD of [A-Z0-9_], not RAN / REFUSED), in first-seen order, each once.
//: end_war writes its reading this way (LEADS, OPEN_WE_WIN, SCORE_GE_50, ...).
inline std::vector<std::string> token_marks(std::string_view text, const std::string& token,
                                            size_t max_out = 32) {
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
//: ... No target selected", probe e12c). Blocks from anything else (the game's
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
