// test_script_templates.cpp — the script action layer's templates
// (script_templates.h). NO GAME.
//
// THE CLAIMS:
//  1. each template composes the exact script the live probe ran (the mod
//     probe: start_war guarded by can_declare_war, run_interaction
//     gift/sway with execute_threshold accept, set_council_task on cp:<seat>),
//     with the bridge's own token in both guard branches;
//  2. no slot can carry script: every key must be a plain identifier, so a key
//     with a space, a brace, an `=` or a quote is refused and nothing is composed;
//  3. the read-back parsers find the token's marker in debug.log text and the
//     console effect's own errors in error.log text (the probe's real lines).
//
// Built twice: plain (must pass) and -DSCRIPT_TPL_RAW_CONTROL, registered
// WILL_FAIL: the probe's channel, which took
// the script text from its caller. Under it every "refused" check of part 2
// fails: a caller's text reaches the engine.

#include "script_templates.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace bridge::script_tpl;

#ifdef SCRIPT_TPL_RAW_CONTROL
// The probe's verb: whatever the caller sent is the script.
static Composed compose_raw(Template, const Slots& s) {
    Composed c; c.ok = true;
    c.script = s.actor_title + " " + s.cb + " " + s.task + " " + s.token;
    return c;
}
#define compose compose_raw
#endif

static int g_fail = 0;
#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (!(cond)) { std::printf("FAIL: %s\n", what); ++g_fail; }            \
        else std::printf("ok: %s\n", what);                                    \
    } while (0)

static Slots base() {
    Slots s;
    s.actor_title = "c_leinster";
    s.target_title = "d_meath";
    s.token = "SACT_7";
    return s;
}

int main() {
    // ── 1. the templates ────────────────────────────────────────────────────
    {
        Slots s = base();
        s.cb = "claim_cb";
        s.war_title = "d_meath";
        Composed c = compose(Template::kDeclareWar, s);
        CHECK(c.ok, "declare_war composes");
        CHECK(c.script ==
              "title:c_leinster.holder = { if = { limit = { can_declare_war = { defender = "
              "title:d_meath.holder casus_belli = claim_cb target_titles = { title:d_meath } } } "
              "start_war = { cb = claim_cb target = title:d_meath.holder target_title = title:d_meath } "
              "info_log = \"SACT_7_RAN\" } else = { "
              "if = { limit = { has_raised_armies = yes } info_log = \"SACT_7_WHY_RAISED_ARMIES\" } "
              "if = { limit = { is_in_debt = yes } info_log = \"SACT_7_WHY_IN_DEBT\" } "
              "if = { limit = { exists = involved_activity } info_log = \"SACT_7_WHY_IN_ACTIVITY\" } "
              "if = { limit = { is_imprisoned = yes } info_log = \"SACT_7_WHY_IMPRISONED\" } "
              "if = { limit = { has_trait = incapable } info_log = \"SACT_7_WHY_INCAPABLE\" } "
              "if = { limit = { has_character_flag = sworn_peace } info_log = \"SACT_7_WHY_SWORN_PEACE\" } "
              "if = { limit = { is_at_war_with = title:d_meath.holder } info_log = \"SACT_7_WHY_AT_WAR_WITH\" } "
              "if = { limit = { is_allied_in_war = title:d_meath.holder } info_log = \"SACT_7_WHY_ALLIED_IN_WAR\" } "
              "if = { limit = { has_truce = title:d_meath.holder } info_log = \"SACT_7_WHY_TRUCE\" } "
              "if = { limit = { NOT = { can_attack_in_hierarchy = title:d_meath.holder } } "
              "info_log = \"SACT_7_WHY_HIERARCHY\" } "
              "if = { limit = { NOT = { has_any_display_cb_on = title:d_meath.holder } } "
              "info_log = \"SACT_7_WHY_NO_DISPLAY_CB\" } "
              "if = { limit = { prestige < 0 } info_log = \"SACT_7_WHY_PRESTIGE_BELOW_0\" } "
              // The rest of the interaction's script conditions (the defender's side among them)
              "if = { limit = { culture = { has_cultural_parameter = cannot_attack_allies } is_allied_to = "
              "title:d_meath.holder } info_log = \"SACT_7_WHY_ALLIED_NO_ATTACK\" } "
              "if = { limit = { has_variable_list = subjugation_offer_under_consideration is_target_in_variable_list = "
              "{ name = subjugation_offer_under_consideration target = title:d_meath.holder } } "
              "info_log = \"SACT_7_WHY_SUBJUGATION_OFFER\" } "
              "if = { limit = { liege ?= { is_at_war_with = title:d_meath.holder } } info_log = \"SACT_7_WHY_LIEGE_AT_WAR\" } "
              "if = { limit = { is_confederation_member = yes title:d_meath.holder = { is_member_of_confederation = "
              "title:c_leinster.holder.confederation } } info_log = \"SACT_7_WHY_CONFEDERATION\" } "
              "if = { limit = { title:d_meath.holder = { government_has_flag = government_is_herder } } "
              "info_log = \"SACT_7_WHY_TARGET_HERDER\" } "
              "if = { limit = { title:d_meath.holder = { is_tributary_of_suzerain_or_above = title:c_leinster.holder } } "
              "info_log = \"SACT_7_WHY_TARGET_TRIBUTARY\" } "
              "if = { limit = { title:d_meath.holder = { has_strong_hook = title:c_leinster.holder } } "
              "info_log = \"SACT_7_WHY_TARGET_STRONG_HOOK\" } "
              "if = { limit = { title:d_meath.holder = { is_imprisoned = yes imprisoner = title:c_leinster.holder } } "
              "info_log = \"SACT_7_WHY_TARGET_YOUR_PRISONER\" } "
              "if = { limit = { title:d_meath.holder = { is_playable_character = no } } "
              "info_log = \"SACT_7_WHY_TARGET_NOT_PLAYABLE\" } "
              // The casus belli's part, probed by the game
              "info_log = \"SACT_7_PROBED\" if = { limit = { NOT = { has_cb_on = { target = title:d_meath.holder casus_belli = claim_cb } } } "
              "info_log = \"SACT_7_WHY_NO_CB_ON\" } "
              "if = { limit = { can_declare_war = { defender = title:d_meath.holder casus_belli = claim_cb } } "
              "info_log = \"SACT_7_PROBE_BARE_OK\" } "
              "if = { limit = { can_declare_war = { defender = title:d_meath.holder casus_belli = claim_cb "
              "target_titles = { title:d_meath } } } info_log = \"SACT_7_PROBE_TITLE_OK\" }" +
              // The casus belli's own price in the refused branch (claim_cb with its title)
              price_scopes("title:d_meath.holder", "", "title:d_meath") +
              price_block(kCbPrices[cb_price_index("claim_cb")], "SACT_7", "COST", true) +
              " info_log = \"SACT_7_REFUSED\" } }",
              "declare_war: the probe's guarded start_war, token in both branches; the refused "
              "branch asks the game each condition of declare_war_interaction's is_valid; its CB probes; the "
              "defender's side and the rest");
        {
            // Live (a live check, 1069.11.27): an extra brace in a probe closed the else block early, so
            // REFUSED ran after RAN and an applied war read as refused. The braces balance, and RAN and
            // REFUSED each sit inside their own branch: the else block is the text after "} else = {" up
            // to its matching brace, and it holds REFUSED and every probe.
            int depth = 0, low = 0;
            for (char ch : c.script) { depth += (ch == '{') - (ch == '}'); low = depth < low ? depth : low; }
            const size_t e = c.script.find("} else = {");
            int d = 0; size_t end = std::string::npos;
            for (size_t i = e + 9; i < c.script.size(); ++i) {
                if (c.script[i] == '{') ++d;
                if (c.script[i] == '}' && --d == 0) { end = i; break; }
            }
            const std::string els = (e != std::string::npos && end != std::string::npos) ? c.script.substr(e, end - e) : "";
            CHECK(depth == 0 && low == 0 && els.find("SACT_7_REFUSED") != std::string::npos &&
                      els.find("SACT_7_PROBE_TITLE_OK") != std::string::npos &&
                      c.script.substr(0, e).find("SACT_7_REFUSED") == std::string::npos &&
                      c.script.find("SACT_7_REFUSED", end) == std::string::npos,
                  "the declare script's braces balance; REFUSED and the probes are inside the else block only");
        }
        CHECK(token_marks("SACT_7_WHY_RAISED_ARMIES\nSACT_7_REFUSED\nSACT_7_WHY_IN_DEBT", "SACT_7") ==
                  std::vector<std::string>({"WHY_RAISED_ARMIES", "WHY_IN_DEBT"}),
              "the refused branch's readings come back as the token's marks");
        s.claimant_title = "k_ireland";
        c = compose(Template::kDeclareWar, s);
        CHECK(c.ok && c.script.find("claimant = title:k_ireland.holder target_titles") != std::string::npos &&
              c.script.find("target = title:d_meath.holder claimant = title:k_ireland.holder target_title") !=
                  std::string::npos,
              "declare_war: a claimant goes to the guard and to start_war");
        s.claimant_title.clear(); s.war_title.clear();
        c = compose(Template::kDeclareWar, s);
        CHECK(c.ok && c.script.find("target_title") == std::string::npos,
              "declare_war: a CB without a title names none");
    }
    {
        Composed g = compose(Template::kGift, base());
        CHECK(g.ok && g.script ==
              "run_interaction = { interaction = gift_interaction actor = title:c_leinster.holder "
              "recipient = title:d_meath.holder execute_threshold = accept } info_log = \"SACT_7_RAN\"",
              "gift: run_interaction with execute_threshold accept");
        Composed w = compose(Template::kSway, base());
        CHECK(w.ok && w.script.find("interaction = sway_interaction") != std::string::npos,
              "sway: run_interaction sway_interaction");
    }
    {
        Slots s = base();
        s.seat = "councillor_spymaster";
        s.task = "task_find_secrets";
        s.task_target_char = "d_meath";
        Composed c = compose(Template::kCouncilTask, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { if = { limit = { exists = cp:councillor_spymaster } "
              "cp:councillor_spymaster = { set_council_task = { task_type = task_find_secrets "
              "target = title:d_meath.holder } } info_log = \"SACT_7_RAN\" } else = { "
              "info_log = \"SACT_7_REFUSED\" } }",
              "council_task: the probe's set_council_task on cp:<seat>, guarded by exists");
        s.task_target_char.clear();
        s.task_target_county = "c_meath";
        c = compose(Template::kCouncilTask, s);
        CHECK(c.ok && c.script.find("target = title:c_meath.title_province") != std::string::npos,
              "council_task: a county target is its province");
        s.task_target_char = "d_meath";
        CHECK(!compose(Template::kCouncilTask, s).ok, "council_task: two targets refused");
        s.task_target_char.clear(); s.task_target_county = "d_meath";
        CHECK(!compose(Template::kCouncilTask, s).ok, "council_task: a county target must be c_*");
        s.task_target_county.clear(); s.seat = "spymaster";
        CHECK(!compose(Template::kCouncilTask, s).ok, "council_task: a seat must be councillor_*");
    }
    {
        Slots s = base();
        s.policy = "join_liege_wars";
        Composed c = compose(Template::kSetPolicy, s);
        CHECK(c.ok && c.script.find("set_variable = { name = ck3agent_policy_join_liege_wars value = 1 }") != std::string::npos &&
              c.script.find("trigger_event = ck3agent_policy.1") != std::string::npos &&
              c.script.find("NOT = { has_variable = ck3agent_policy_armed }") != std::string::npos,
              "set_policy: the variable, then the event armed once");
        c = compose(Template::kClearPolicy, s);
        CHECK(c.ok && c.script.find("remove_variable = ck3agent_policy_join_liege_wars") != std::string::npos,
              "clear_policy: the variable removed");
        s.policy = "stay_out_of_liege_factions";
        CHECK(compose(Template::kSetPolicy, s).ok, "set_policy: the second policy");
        s.policy = "anything_else";
        CHECK(!compose(Template::kSetPolicy, s).ok && compose(Template::kSetPolicy, s).why == "unknown_policy",
              "set_policy: an unknown policy refused");
    }

    {
        // end_war. The war the two LEAD on opposite sides; the
        // reading first (LEADS, the open terms, our score band), then the
        // guarded end_war, the side resolved from which side the actor leads.
        const std::string between =
            "OR = { AND = { primary_attacker = title:c_leinster.holder primary_defender = title:d_meath.holder } "
            "AND = { primary_attacker = title:d_meath.holder primary_defender = title:c_leinster.holder } }";
        const std::string we_win =
            "OR = { AND = { primary_attacker = title:c_leinster.holder attacker_war_score >= 100 } "
            "AND = { primary_defender = title:c_leinster.holder defender_war_score >= 100 } }";
        const std::string white =
            "OR = { AND = { primary_attacker = title:d_meath.holder attacker_war_score < 25 } "
            "AND = { primary_defender = title:d_meath.holder defender_war_score < 25 } }";
        Slots s = base();
        s.peace_terms = "we_win";
        Composed c = compose(Template::kEndWar, s);
        CHECK(c.ok && c.script.rfind("title:c_leinster.holder = { if = { limit = { any_character_war = { " +
                                     between + " } } random_character_war = { limit = { " + between +
                                     " } info_log = \"SACT_7_LEADS\"", 0) == 0,
              "end_war: inside the war both lead, LEADS first");
        CHECK(c.ok && c.script.find("if = { limit = { " + we_win + " } info_log = \"SACT_7_OPEN_WE_WIN\" }") !=
                          std::string::npos &&
                  c.script.find("if = { limit = { " + white + " } info_log = \"SACT_7_OPEN_WHITE_PEACE\" }") !=
                      std::string::npos &&
                  c.script.find("info_log = \"SACT_7_OPEN_WE_YIELD\"") != std::string::npos,
              "end_war: the open terms are listed (we_win at our 100, white peace under their 25, we_yield always)");
        CHECK(c.ok && c.script.find("info_log = \"SACT_7_SCORE_GE_M25\"") != std::string::npos &&
                  c.script.find("info_log = \"SACT_7_SCORE_GE_100\"") != std::string::npos,
              "end_war: our score band is listed");
        CHECK(c.ok && c.script.find("if = { limit = { any_character_war = { " + between + " " + we_win +
                                    " } } random_character_war = { limit = { " + between + " " + we_win +
                                    " } if = { limit = { primary_attacker = title:c_leinster.holder } "
                                    "end_war = attacker } else = { end_war = defender } } "
                                    "info_log = \"SACT_7_RAN\" } else = { info_log = \"SACT_7_REFUSED\" }") !=
                          std::string::npos,
              "end_war we_win: guarded by our score at 100, OUR side wins");
        CHECK(c.ok && c.script.find("} else = { info_log = \"SACT_7_NO_WAR\" info_log = \"SACT_7_REFUSED\" } }") !=
                          std::string::npos,
              "end_war: no war led against them is NO_WAR, refused");
        s.peace_terms = "we_yield";
        c = compose(Template::kEndWar, s);
        CHECK(c.ok && c.script.find("always = yes } } random_character_war = { limit = { " + between +
                                    " always = yes } if = { limit = { primary_attacker = title:c_leinster.holder } "
                                    "end_war = defender } else = { end_war = attacker } }") != std::string::npos,
              "end_war we_yield: always open, the ENEMY's side wins");
        s.peace_terms = "white_peace";
        c = compose(Template::kEndWar, s);
        CHECK(c.ok && c.script.find(" " + white + " } end_war = white_peace }") != std::string::npos,
              "end_war white_peace: guarded by the enemy under 25");
        s.peace_terms.clear();
        c = compose(Template::kEndWar, s);
        CHECK(c.ok && c.script.find("end_war") == std::string::npos &&
                  c.script.find("info_log = \"SACT_7_RAN\" } else") != std::string::npos,
              "end_war read only: the listing and no end_war");
        for (const char* t : {"surrender", "victory", "enforce_demands", "attacker", "WE_WIN", "we_win }"}) {
            s.peace_terms = t;
            Composed r = compose(Template::kEndWar, s);
            std::string what = std::string("end_war: terms '") + t + "' refused bad_peace_terms";
            CHECK(!r.ok && r.why == "bad_peace_terms", what.c_str());
        }
        s.peace_terms = "we_win"; s.target_title = "d_meath } add_gold = 1 {";
        CHECK(!compose(Template::kEndWar, s).ok, "end_war: a bad enemy key refused");
        // A rebel leader who holds no title (a 1092.01.06 game), at our 100
        Slots L = base();
        L.target_title.clear();
        L.enemy_landless = true;
        L.peace_terms = "we_win";
        Composed lc = compose(Template::kEndWar, L);
        const std::string lb =
            "OR = { AND = { primary_attacker = title:c_leinster.holder primary_defender = { is_landed = no } } "
            "AND = { primary_attacker = { is_landed = no } primary_defender = title:c_leinster.holder } }";
        CHECK(lc.ok && lc.script.find("any_character_war = { " + lb + " }") != std::string::npos,
              "end_war landless: the war is matched by a leader who holds no title");
        CHECK(lc.ok && lc.script.find("if = { limit = { any_character_war = { count >= 2 " + lb +
                                      " } } info_log = \"SACT_7_AMBIGUOUS\" info_log = \"SACT_7_REFUSED\" } else = {") !=
                          std::string::npos,
              "end_war landless: two such wars are AMBIGUOUS and nothing ends");
        CHECK(lc.ok && lc.script.find("end_war = attacker } else = { end_war = defender }") != std::string::npos &&
                  lc.script.find("title:d_meath") == std::string::npos,
              "end_war landless we_win: our side wins, no enemy title named");
        L.target_title = "d_meath";
        CHECK(!compose(Template::kEndWar, L).ok, "end_war landless with an enemy title too: refused");
        Slots T = base();
        T.peace_terms = "we_win";
        CHECK(compose(Template::kEndWar, T).script.find("AMBIGUOUS") == std::string::npos,
              "end_war with a titled enemy: unchanged (no count)");
    }

    // ── 2. no slot carries script ───────────────────────────────────────────
    {
        const char* bad[] = {"claim_cb } add_gold = 1000 {", "claim_cb add_gold", "x=1", "\"q\"",
                             "Claim_cb", "1abc", "", "a{", "a.b", "title:k_x",
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
        for (const char* b : bad) {
            Slots s = base(); s.cb = b; s.war_title = "d_meath";
            std::string what = std::string("declare_war: cb '") + b + "' refused";
            CHECK(!compose(Template::kDeclareWar, s).ok, what.c_str());
            Slots t = base(); t.actor_title = b;
            CHECK(!compose(Template::kGift, t).ok, "gift: a bad actor key refused");
            Slots u = base(); u.seat = "councillor_spymaster"; u.task = b;
            CHECK(!compose(Template::kCouncilTask, u).ok, "council_task: a bad task key refused");
        }
        Slots s = base(); s.token = "SACT_1\" } add_gold = 1 {";
        CHECK(!compose(Template::kGift, s).ok, "a token that is not SACT_<digits> refused");
        CHECK(template_by_name("effect") == Template::kNone && template_by_name("run") == Template::kNone &&
              template_by_name("raw") == Template::kNone,
              "no template passes raw script");
        CHECK(sizeof(kTemplates) / sizeof(kTemplates[0]) == 15, "the enumerated set is fifteen templates (activity, claim_price_facts, cb_prices)");
    }

    // ── 5. every line of ours is INFO ──────────────────────────
    // The game drops every message of a level once 100,000 of that level were
    // logged in the session (ck3::nlog::LEVEL_MSG_CAP); DEBUG filled at
    // 1088.01.27 in a recorded game and every guard read `unseen` after it. No
    // template may write a DEBUG line: each composes, writes kLogEffect, and
    // carries no debug_log.
    {
        CHECK(std::string(kLogEffect) == "info_log", "the log effect is info_log");
        for (const auto& tn : kTemplates) {
            Slots s = base();
            s.cb = "claim_cb"; s.war_title = "d_meath";
            s.seat = "councillor_spymaster"; s.task = "task_find_secrets";
            s.policy = "join_liege_wars"; s.proposal_on = true;
            s.proposal_class = "alliance";
            s.proposal_answer = tn.t == Template::kProposalPolicy ? proposal_policy_answer("decline")
                                                                   : proposal_answer_code("accept");
            s.proposal_by = proposal_by_code("seat"); s.proposal_id = 12;
            s.peace_terms = "white_peace"; s.reserve_gold = 200;
            s.price_cbs = {"vassalization_cb"};
            const Composed c = compose(tn.t, s);
            const std::string what = std::string(tn.name) + ": composes, writes info_log, no debug_log";
            CHECK(c.ok && c.script.find("info_log = \"") != std::string::npos &&
                      c.script.find("debug_log") == std::string::npos, what.c_str());
        }
    }

    // ── 6. the gold reserve, the actor's own scope only ──
    {
        Slots s = base();
        s.reserve_gold = 200; s.reserve_move = true;
        Composed c = compose(Template::kGoldReserve, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { set_reserved_gold_maximum = 200 "
              "if = { limit = { short_term_gold > 0 } move_budget_gold = { gold = short_term_gold from = budget_short_term to = budget_reserved } } "
              "if = { limit = { long_term_gold > 0 } move_budget_gold = { gold = long_term_gold from = budget_long_term to = budget_reserved } } "
              "if = { limit = { war_chest_gold > 0 } move_budget_gold = { gold = war_chest_gold from = budget_war_chest to = budget_reserved } } "
              "info_log = \"SACT_7_RAN\" }",
              "gold_reserve: the maximum, then the gold held moved in (short term, long term, war chest)");
        s.reserve_move = false;
        c = compose(Template::kGoldReserve, s);
        CHECK(c.ok && c.script == "title:c_leinster.holder = { set_reserved_gold_maximum = 200 info_log = \"SACT_7_RAN\" }",
              "gold_reserve without move: the maximum only");
        s.reserve_gold = 0;
        c = compose(Template::kGoldReserve, s);
        CHECK(c.ok && c.script.find("set_reserved_gold_maximum = 0 if = { limit = { reserved_gold > 0 } move_budget_gold = "
                                    "{ gold = reserved_gold from = budget_reserved to = budget_short_term } }") != std::string::npos,
              "gold_reserve 0: the reserve released back to short term");
        for (long long bad : {-1LL, -200LL, kReserveMax + 1}) {
            s.reserve_gold = bad;
            CHECK(!compose(Template::kGoldReserve, s).ok && compose(Template::kGoldReserve, s).why == "bad_reserve_amount",
                  "gold_reserve: an amount out of 0..kReserveMax refused");
        }
        s.reserve_gold = 200; s.actor_title = "x_script_846";
        CHECK(!compose(Template::kGoldReserve, s).ok, "gold_reserve: a key script cannot resolve refused");
    }

    // ── 7. activities (the listing, and one type put to the AI) ──
    {
        Slots s = base();
        Composed c = compose(Template::kActivity, s);
        CHECK(c.ok && c.script.rfind("title:c_leinster.holder = { save_scope_as = ck3a_me "
                                     "if = { limit = { is_ai = no } info_log = \"SACT_7_NOT_AI\" } ", 0) == 0,
              "activity: the actor's own scope, the gates first");
        CHECK(c.script.find("if = { limit = { years_from_game_start < 3 } info_log = \"SACT_7_EARLY\" }") !=
                  std::string::npos &&
              c.script.find("if = { limit = { ai_should_focus_on_building_in_their_capital = yes } "
                            "info_log = \"SACT_7_BUILD_FOCUS\" }") != std::string::npos,
              "activity: the files' own AI conditions, word for word");
        CHECK(c.script.find("if = { limit = { exists = capital_county } capital_county.title_province = "
                            "{ save_scope_as = ck3a_bfp } if = { limit = { scope:ck3a_bfp = { free_building_slots > 0 } } "
                            "info_log = \"SACT_7_BF_FREE_SLOT\" }") != std::string::npos &&
              c.script.find("if = { limit = { culture = { has_innovation = innovation_ledger } scope:ck3a_bfp = "
                            "{ combined_building_level <= 4 } } info_log = \"SACT_7_BF_LEDGER\" }") != std::string::npos &&
              c.script.find("info_log = \"SACT_7_BF_SEAT_IS_CAPITAL\"") != std::string::npos,
              "activity: each branch of the build-focus trigger, at the capital county's seat");
        CHECK(c.script.find("if = { limit = { can_host_activity = activity_feast } info_log = \"SACT_7_FEAST_CAN_HOST\" }") !=
                  std::string::npos &&
              c.script.find("if = { limit = { ai_boldness < 100 } info_log = \"SACT_7_HUNT_AXIS_BELOW_100\" }") !=
                  std::string::npos &&
              c.script.find("is_activity_type_on_cooldown = activity_pilgrimage") != std::string::npos,
              "activity: each type's game verdict, cooldown and the count's throttle value");
        CHECK(c.script.find("ai_attempt_to_host_activity") == std::string::npos &&
                  c.script.size() >= 25 && c.script.substr(c.script.size() - 25) == "info_log = \"SACT_7_RAN\" }",
              "activity: the listing writes nothing to the game");
        s.activity = "activity_hunt"; s.activity_host = true;
        c = compose(Template::kActivity, s);
        CHECK(c.ok && c.script.find("if = { limit = { can_host_activity = activity_hunt } "
                                    "ai_attempt_to_host_activity = activity_hunt info_log = \"SACT_7_RAN\" } else = { "
                                    "info_log = \"SACT_7_REFUSED\" } }") != std::string::npos,
              "activity host: the game's own effect, guarded by its own can_host_activity");
        s.activity = "activity_tournament";
        CHECK(!compose(Template::kActivity, s).ok && compose(Template::kActivity, s).why == "bad_activity",
              "activity: a type outside the set refused");
        s.activity = "activity_feast } add_gold = 1000 {";
        CHECK(!compose(Template::kActivity, s).ok, "activity: a type carrying script refused");
        s.activity = ""; s.activity_host = true;
        CHECK(!compose(Template::kActivity, s).ok && compose(Template::kActivity, s).why == "activity_required",
              "activity: host with no type refused");
        s.activity_host = false; s.actor_title = "x_script_846";
        CHECK(!compose(Template::kActivity, s).ok, "activity: a key script cannot resolve refused");
        // A guest leaves by the game's own "Go home" effects; a host is refused by the guard
        s = base(); s.activity_leave = true;
        c = compose(Template::kActivity, s);
        CHECK(c.ok && c.script.find("if = { limit = { exists = involved_activity NOT = { involved_activity.activity_host = "
                                    "scope:ck3a_me } } remove_from_activity = involved_activity return_home = yes "
                                    "info_log = \"SACT_7_RAN\" } else = { info_log = \"SACT_7_REFUSED\" } }") !=
                  std::string::npos && c.script.find("ai_attempt_to_host_activity") == std::string::npos,
              "activity leave: remove_from_activity + return_home, guarded: in one, not its host");
        CHECK(c.script.find("info_log = \"SACT_7_IN_ACTIVITY\"") < c.script.find("remove_from_activity"),
              "activity leave: the listing (which activity) is written before the leave");
        s.activity_host = true; s.activity = "activity_feast";
        CHECK(!compose(Template::kActivity, s).ok && compose(Template::kActivity, s).why == "leave_takes_no_activity",
              "activity leave: leave with host or a type refused");
    }
    // ── 8. (W3a): the claim-war price facts, read only ──────────
    {
        Slots s = base();
        Composed c = compose(Template::kClaimPriceFacts, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { info_log = \"SACT_7_READ\" "
              "if = { limit = { has_legitimacy_flag = massively_increased_claim_cb_cost } info_log = \"SACT_7_LEGIT_X2_5\" } "
              "else_if = { limit = { has_legitimacy_flag = very_increased_claim_cb_cost } info_log = \"SACT_7_LEGIT_X2_0\" } "
              "else_if = { limit = { has_legitimacy_flag = increased_claim_cb_cost } info_log = \"SACT_7_LEGIT_X1_5\" } "
              "if = { limit = { has_legitimacy_flag = reduced_claim_cb_cost } info_log = \"SACT_7_LEGIT_X0_5\" } "
              "else_if = { limit = { has_legitimacy_flag = very_reduced_claim_cb_cost } info_log = \"SACT_7_LEGIT_X0_25\" } "
              "else_if = { limit = { has_legitimacy_flag = extra_reduced_claim_cb_cost } info_log = \"SACT_7_LEGIT_X0_15\" } "
              "if = { limit = { is_adult = no } info_log = \"SACT_7_CHILD\" } "
              "if = { limit = { is_male = yes } info_log = \"SACT_7_MALE\" } "
              "if = { limit = { faith = { has_doctrine = doctrine_gender_male_dominated } } info_log = \"SACT_7_FAITH_MALE_DOMINATED\" } "
              "else_if = { limit = { faith = { has_doctrine = doctrine_gender_female_dominated } } info_log = \"SACT_7_FAITH_FEMALE_DOMINATED\" } "
              "info_log = \"SACT_7_RAN\" }",
              "claim_price_facts: 00_claim.txt's two legitimacy chains, age, sex, the faith's gender doctrine");
        s.actor_title = "x_script_846";
        CHECK(!compose(Template::kClaimPriceFacts, s).ok, "claim_price_facts: a key script cannot resolve refused");
    }

    // ── 9. the game's price of a declaration, read only ─────────
    {
        Slots s = base();
        s.price_cbs = {"vassalization_cb", "claim_cb"};
        Composed c = compose(Template::kCbPrices, s);
        const int vi = cb_price_index("vassalization_cb"), ci = cb_price_index("claim_cb");
        CHECK(vi >= 0 && ci >= 0 && kCbPrices[ci].needs_title && !kCbPrices[vi].needs_title,
              "cb_prices: vassalization_cb reads no title, claim_cb does");
        const std::string vw = "SACT_7_C0", cw = "SACT_7_C1";   // by position in the request
        CHECK(c.ok && c.script.rfind("title:c_leinster.holder = { save_scope_as = attacker title:d_meath.holder = "
                                     "{ save_scope_as = defender } save_scope_as = claimant info_log = \"SACT_7_READ\"", 0) == 0,
              "cb_prices: the actor's scope; attacker, defender, claimant (the actor) saved; READ");
        CHECK(c.script.find(std::string("set_variable = { name = ck3a_cbp value = ") + kCbPrices[vi].prestige + " }") !=
                  std::string::npos && c.script.find(vw + "P_I17") != std::string::npos &&
                  c.script.find(vw + "P_F0") != std::string::npos && c.script.find(vw + "P_SET") != std::string::npos &&
                  c.script.find(vw + "Q_SET") != std::string::npos,
              "cb_prices: the file's prestige value into the variable, 18 whole and 10 fraction bits, SET");
        CHECK(c.script.find(cw + "_NOTITLE") != std::string::npos && c.script.find(cw + "P_SET") == std::string::npos,
              "cb_prices: claim_cb with no title is NOTITLE, never priced without its list");
        CHECK(c.script.find("remove_variable = ck3a_cbp") != std::string::npos &&
                  c.script.find("WHY_") == std::string::npos && c.script.find("start_war") == std::string::npos,
              "cb_prices: the variable removed; no refusal reading, no war");
        s.war_title = "c_ossory";
        c = compose(Template::kCbPrices, s);
        CHECK(c.ok && c.script.find("title:c_ossory = { add_to_temporary_list = target_titles }") != std::string::npos &&
                  c.script.find(cw + "P_SET") != std::string::npos,
              "cb_prices: with a title, the list target_titles and claim_cb priced");
        s.price_cbs = {"vassalization_cb", "add_gold"};
        CHECK(!compose(Template::kCbPrices, s).ok && compose(Template::kCbPrices, s).why == "bad_cb_key",
              "cb_prices: a key that is not a row refused");
        s.price_cbs = {};
        CHECK(!compose(Template::kCbPrices, s).ok, "cb_prices: no casus belli refused");
        // the declaration's refused branch prices its own casus belli, against prestige and piety
        Slots d = base(); d.cb = "vassalization_cb";
        c = compose(Template::kDeclareWar, d);
        const size_t els = c.script.find("} else = {");
        CHECK(c.ok && els != std::string::npos &&
                  c.script.find("if = { limit = { prestige < var:ck3a_cbp } info_log = \"SACT_7_WHY_PRESTIGE_BELOW_COST\" }", els) !=
                      std::string::npos &&
                  c.script.find("SACT_7_COSTP_SET", els) != std::string::npos &&
                  c.script.find("ck3a_cbp") > els,
              "declare_war: the refused branch reads the price and whether prestige is below it");
        d.cb = "claim_cb";
        CHECK(compose(Template::kDeclareWar, d).script.find("COSTP") == std::string::npos,
              "declare_war: claim_cb with no title is not priced (its value reads target_titles)");
        d.cb = "artifact_war";
        CHECK(compose(Template::kDeclareWar, d).script.find("ck3a_cbp") == std::string::npos,
              "declare_war: a casus belli with no row is not priced");
    }

    // ── 4. the incoming-proposal queue ─────────────────────────
    {
        Slots s = base();
        s.proposal_on = true;
        Composed c = compose(Template::kProposalSeat, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { set_variable = { name = ck3agent_proposal_seat value = 1 days = 400 } "
              "if = { limit = { has_variable = ck3agent_proposal_seat } info_log = \"SACT_7_RAN\" } else = { "
              "info_log = \"SACT_7_REFUSED\" } }",
              "proposal_seat on: the 400-day marker, read back by the guard");
        s.proposal_on = false;
        c = compose(Template::kProposalSeat, s);
        CHECK(c.ok && c.script.find("remove_variable = ck3agent_proposal_seat") != std::string::npos &&
              c.script.find("has_variable = ck3agent_proposal_seat } info_log = \"SACT_7_REFUSED\"") != std::string::npos,
              "proposal_seat off: removed; still there is REFUSED");
    }
    {
        Slots s = base();
        s.proposal_class = "alliance";
        s.proposal_answer = proposal_policy_answer("decline");
        Composed c = compose(Template::kProposalPolicy, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { set_variable = { name = ck3agent_proposal_answer_alliance value = 2 } "
              "if = { limit = { has_variable = ck3agent_proposal_answer_alliance "
              "var:ck3agent_proposal_answer_alliance = 2 } info_log = \"SACT_7_RAN\" } else = { "
              "info_log = \"SACT_7_REFUSED\" } }",
              "proposal_policy: the class variable set to the answer's code and read back");
        for (const char* a : {"accept", "decline", "ask_me", "native"}) {
            s.proposal_answer = proposal_policy_answer(a);
            CHECK(compose(Template::kProposalPolicy, s).ok, "proposal_policy: each of the four values");
        }
        s.proposal_answer = proposal_policy_answer("maybe");
        CHECK(compose(Template::kProposalPolicy, s).why == "bad_answer", "proposal_policy: an unknown answer refused");
        s.proposal_answer = 1;
        for (const char* b : {"wars", "alliance } add_gold = 1 {", "", "Marriage", "call_ally_interaction"}) {
            s.proposal_class = b;
            CHECK(compose(Template::kProposalPolicy, s).why == "bad_class", "proposal_policy: an unknown class refused");
        }
    }
    {
        Slots s = base();
        Composed c = compose(Template::kProposalList, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { info_log = \"SACT_7_BEGIN\" every_owned_story = { limit = { "
              "story_type = ck3agent_proposal ck3agent_proposal_open_trigger = yes } "
              "ck3agent_proposal_list_item_effect = yes } info_log = \"SACT_7_END\" info_log = \"SACT_7_RAN\" }",
              "proposal_list: the seat's OWN open stories, between BEGIN and END");
    }
    {
        Slots s = base();
        s.proposal_id = 12;
        s.proposal_answer = proposal_answer_code("accept");
        s.proposal_by = proposal_by_code("seat");
        Composed c = compose(Template::kProposalAnswer, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { info_log = \"SACT_7_BEGIN\" every_owned_story = { limit = { "
              "story_type = ck3agent_proposal var:ck3agent_proposal_id = 12 } save_scope_as = ck3a_found "
              "ck3agent_proposal_answer_effect = { ANSWER = 1 BY = 1 } } if = { limit = { NOT = { exists = "
              "scope:ck3a_found } } info_log = \"CK3AGENT_PROPOSAL_OUTCOME|refused|not_found\" } "
              "info_log = \"SACT_7_END\" info_log = \"SACT_7_RAN\" }",
              "proposal_answer: found by id inside the seat's own stories, the mod's one apply effect");
        CHECK(proposal_answer_code("ask_me") == -1 && proposal_answer_code("native") == 4 &&
              proposal_answer_code("decline") == 2, "proposal_answer: accept, decline, native only");
        CHECK(proposal_by_code("rule") == -1 && proposal_by_code("lapse") == -1 && proposal_by_code("void") == -1 &&
              proposal_by_code("boundary") == 3 && proposal_by_code("classifier") == 6 && proposal_by_code("") == -1,
              "proposal_answer: by is seat, boundary or classifier (the rule/lapse/void are the game's)");
        Slots b = s; b.proposal_id = 0;
        CHECK(compose(Template::kProposalAnswer, b).why == "bad_proposal_id", "proposal_answer: id 0 refused");
        b.proposal_id = -3;
        CHECK(compose(Template::kProposalAnswer, b).why == "bad_proposal_id", "proposal_answer: a negative id refused");
        b = s; b.proposal_answer = proposal_answer_code("ask_me");
        CHECK(compose(Template::kProposalAnswer, b).why == "bad_answer", "proposal_answer: ask_me is not an answer");
        b = s; b.proposal_by = proposal_by_code("rule");
        CHECK(compose(Template::kProposalAnswer, b).why == "bad_by", "proposal_answer: by=rule refused");
    }
    {
        // proposal_stage is deleted (seen live: run_interaction ...
        // send_threshold = decline runs no on_send, so a staged offer is never held);
        // its name is unknown in every build, debug verbs or not.
        CHECK(template_by_name("proposal_stage") == Template::kNone, "proposal_stage: deleted, not nameable");
        CHECK(template_by_name("proposal_answer") == Template::kProposalAnswer &&
              template_by_name("proposal_list") == Template::kProposalList, "the four seat templates are named");
    }
    {
        // the mod's lines, as the game writes them (the policy mod's real prefix)
        const std::string P = "[16:03:01][D][jomini_effect_impl.cpp:450]: file: common/scripted_effects/"
                              "zz_ck3agent_proposal_kinds.txt line: 9 (effect): ";
        ProposalLogLine l;
        CHECK(parse_proposal_log_line(P + "CK3AGENT_PROPOSAL_HELD|3|negotiate_alliance_interaction|alliance|"
                                          "12,604|9870|1066.3.14", &l) &&
              l.word == "CK3AGENT_PROPOSAL_HELD" && l.fields.size() == 5 && l.fields[0] == "3" &&
              l.fields[3] == "12604" && l.fields[4] == "9870" && l.date == "1066.3.14",
              "line: HELD, a grouped id read, the date");
        CHECK(parse_proposal_log_line(P + "CK3AGENT_PROPOSAL_RESOLVED|3|2|2|negotiate_alliance_interaction|alliance|"
                                          "1066.3.15", &l) &&
              l.fields[1] == "decline" && l.fields[2] == "rule" &&
              l.fields[3] == "class_policy:alliance=decline",
              "line: RESOLVED by rule names its rule");
        CHECK(parse_proposal_log_line(P + "CK3AGENT_PROPOSAL_RESOLVED|4|1|1|arrange_marriage_interaction|marriage|"
                                          "1066.3.15", &l) &&
              l.fields[2] == "seat" && l.fields[3].empty(), "line: RESOLVED by the seat has no rule");
        CHECK(parse_proposal_log_line(P + "CK3AGENT_PROPOSAL_VOID|5|2|arrange_marriage_interaction|marriage|1066.4.1",
                                      &l) && l.fields[1] == "void_subject_dead",
              "line: VOID with its reason");
        CHECK(parse_proposal_log_line(P + "CK3AGENT_PROPOSAL_RESOLVED|4|9|1|k|c|1066.1.1", &l) &&
              l.fields[1] == "unknown:9", "line: an unknown code is kept, never dropped");
        CHECK(parse_proposal_log_line(P + "CK3AGENT_PROPOSAL_HELD|x|k|c|a|b|d", &l) && l.fields[0] == "unread:x" &&
              l.date.empty(), "line: an unprintable number is unread, never a number");
        CHECK(!parse_proposal_log_line(P + "CK3AGENT_POLICY_TICK", &l), "line: other words are not ours");
        CHECK(loc_int("12.00") == 12 && loc_int("1,234") == 1234 && loc_int("12.5") == -1 && loc_int("") == -1 &&
              loc_int("-3") == -1, "loc_int: lenient on grouping and a zero fraction, nothing else");

        const std::string win =
            P + "SACT_9_BEGIN\n" +
            P + "CK3AGENT_PROPOSAL_ITEM|3|arrange_marriage_interaction|marriage|9870|12604|2\n" +
            P + "CK3AGENT_PROPOSAL_ITEM_SECONDARY_ACTOR|9901\n" +
            P + "CK3AGENT_PROPOSAL_ITEM_SECONDARY_RECIPIENT|12700\n" +
            P + "CK3AGENT_PROPOSAL_ITEM_OPTION|matrilineal\n" +
            P + "CK3AGENT_PROPOSAL_ITEM|4|negotiate_alliance_interaction|alliance|7000|12604|0\n" +
            P + "SACT_9_END\n" +
            P + "CK3AGENT_PROPOSAL_ITEM|99|x|y|1|2|3\n";
        ProposalWindow w = proposal_window(win, "SACT_9");
        auto items = proposal_items(w.lines);
        CHECK(w.complete && items.size() == 2, "list: two items between BEGIN and END, none after");
        CHECK(items.size() == 2 && items[0].id == 3 && items[0].secondary_actor_id == 9901 &&
              items[0].secondary_recipient_id == 12700 && items[0].options.size() == 1 &&
              items[0].age_days_min == 20 && items[1].secondary_actor_id == -1 && items[1].options.empty(),
              "list: each item carries its own secondaries and options");
        ProposalWindow half = proposal_window(P + "SACT_9_BEGIN\n" + P +
                                              "CK3AGENT_PROPOSAL_ITEM|3|k|marriage|1|2|0\n", "SACT_9");
        CHECK(half.begun && !half.complete, "list: no END seen is not complete (a refusal upstream)");
        CHECK(!proposal_window(win, "SACT_90").begun, "list: another token's window is not this one");

        ProposalWindow a = proposal_window(P + "SACT_2_BEGIN\n" + P +
                                           "CK3AGENT_PROPOSAL_OUTCOME|refused|void_sender_dead\n" + P +
                                           "SACT_2_END\n", "SACT_2");
        ProposalOutcome o = proposal_outcome(a.lines);
        CHECK(o.outcome == "refused" && o.reason == "void_sender_dead", "answer: the refusal and its reason");
        a = proposal_window(P + "SACT_2_BEGIN\n" + P + "CK3AGENT_PROPOSAL_OUTCOME|applied_accept|none\n" + P +
                            "SACT_2_END\n", "SACT_2");
        o = proposal_outcome(a.lines);
        CHECK(o.outcome == "applied_accept" && o.reason.empty(), "answer: applied, no reason");
    }

    // ── 3. read-back parsers on the probe's real log lines ───────────────────
    {
        const std::string dbg =
            "[20:13:51][D][jomini_effect_impl.cpp:450]: file: effect console command line: 1: SACT_5_REFUSED\n"
            "[20:13:51][D][jomini_effect_impl.cpp:450]: file: effect console command line: 1: SACT_51_RAN\n";
        CHECK(marker_in(dbg, "SACT_5") == Marker::kRefused, "marker: SACT_5 refused");
        CHECK(marker_in(dbg, "SACT_51") == Marker::kRan, "marker: SACT_51 ran, not confused with SACT_5");
        CHECK(marker_in(dbg, "SACT_1") == Marker::kUnseen, "marker: an absent token is unseen");
    }
    {
        // end_war's reading, the other marks of the token.
        const std::string dbg =
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: SACT_8_LEADS\n"
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: SACT_8_OPEN_WHITE_PEACE\n"
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: SACT_88_OPEN_WE_WIN\n"
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: SACT_8_OPEN_WE_YIELD\n"
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: SACT_8_SCORE_GE_M25\n"
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: SACT_8_SCORE_GE_M25\n"
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: XSACT_8_FAKE\n"
            "[10:00:01][D][x.cpp:1]: file: effect console command line: 1: SACT_8_REFUSED\n";
        auto m = token_marks(dbg, "SACT_8");
        CHECK(m.size() == 4 && m[0] == "LEADS" && m[1] == "OPEN_WHITE_PEACE" && m[2] == "OPEN_WE_YIELD" &&
                  m[3] == "SCORE_GE_M25",
              "marks: the token's own, once each, in order; not SACT_88's, not RAN/REFUSED");
        CHECK(token_marks(dbg, "SACT_9").empty(), "marks: none for an absent token");
    }
    {
        // error.log of the probe's declare_war_interaction, verbatim
        // but the two control characters around the icon.
        const std::string err =
            "[20:11:07][E][jomini_script_system.cpp:303]: Script system error!\n"
            "  Error: run_interaction effect [ Interaction cannot be sent. Reason:\n"
            "warning_icon! X No target selected!  ]\n"
            "  Script location: file: effect console command line: 1\n"
            "\n"
            "[20:11:08][E][jomini_script_system.cpp:303]: Script system error!\n"
            "  Error: some event's own error\n"
            "  Script location: file: events/some_events.txt line: 12\n"
            "[20:11:09][E][holdinggraphics.cpp:148]: no parameter 'Tier' for audio event\n";
        auto e = console_effect_errors(err);
        CHECK(e.size() == 1, "errors: one console-effect block, the event's skipped");
        CHECK(!e.empty() && e[0].find("run_interaction effect [ Interaction cannot be sent. Reason:") == 0 &&
              e[0].find("No target selected") != std::string::npos,
              "errors: the game's own reason, continuation joined");
    }

    {
        // In a 1078.11.27 game Domnall of Oriel's primary title was the duchy the
        // game created mid-run, x_script_846; title:x_script_846.holder was null and
        // can_declare_war answered "defender was null".
        CHECK(!is_script_title_key("x_script_846") && !is_script_title_key("x_x_12"),
              "a dynamic title's key is not a script link");
        CHECK(is_script_title_key("c_oriel") && is_script_title_key("d_meath") &&
              is_script_title_key("k_ireland") && is_script_title_key("e_britannia") &&
              is_script_title_key("b_athlone") && is_script_title_key("h_china"),
              "a de-jure title's key is");
        CHECK(!is_script_title_key("claim_cb") && !is_script_title_key("c_") && !is_script_title_key(""),
              "a key with no tier prefix (or nothing after it) is not");
        Slots s = base();
        s.cb = "claim_cb";
        s.target_title = "x_script_846";
        CHECK(!compose(Template::kDeclareWar, s).ok, "declare_war never names a dynamic title's holder");
        s.target_title = "c_oriel";
        s.war_title = "x_script_846";
        CHECK(!compose(Template::kDeclareWar, s).ok, "nor fights for a dynamic title by key");
        s.war_title = "c_oriel";
        CHECK(compose(Template::kDeclareWar, s).ok, "the same war through his county composes");
        Slots g = base(); g.target_title = "x_script_846";
        CHECK(!compose(Template::kGift, g).ok && !compose(Template::kEndWar, g).ok,
              "no template addresses a dynamic title");
    }

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
