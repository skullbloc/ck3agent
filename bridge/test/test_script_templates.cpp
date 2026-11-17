// test_script_templates.cpp — lane v4act: the script action layer's templates
// (script_templates.h). NO GAME.
//
// THE CLAIMS:
//  1. each template composes the exact script the live probe ran (lane
//     modprobe e12c / e3a: start_war guarded by can_declare_war, run_interaction
//     gift/sway with execute_threshold accept, set_council_task on cp:<seat>),
//     with the bridge's own token in both guard branches;
//  2. no slot can carry script: every key must be a plain identifier, so a key
//     with a space, a brace, an `=` or a quote is refused and nothing is composed;
//  3. the read-back parsers find the token's marker in debug.log text and the
//     console effect's own errors in error.log text (the probe's real lines).
//
// Built twice: plain (must pass) and -DSCRIPT_TPL_RAW_CONTROL, registered
// WILL_FAIL: the probe's channel (lane modprobe do_console_script), which took
// the script text from its caller. Under it every "refused" check of part 2
// fails: a caller's text reaches the engine.

#include "script_templates.h"

#include <cstdio>
#include <string>

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
              "debug_log = \"SACT_7_RAN\" } else = { debug_log = \"SACT_7_REFUSED\" } }",
              "declare_war: the probe's guarded start_war, token in both branches");
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
              "recipient = title:d_meath.holder execute_threshold = accept } debug_log = \"SACT_7_RAN\"",
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
              "target = title:d_meath.holder } } debug_log = \"SACT_7_RAN\" } else = { "
              "debug_log = \"SACT_7_REFUSED\" } }",
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
        // lane peace: end_war. The war the two LEAD on opposite sides; the
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
                                     " } debug_log = \"SACT_7_LEADS\"", 0) == 0,
              "end_war: inside the war both lead, LEADS first");
        CHECK(c.ok && c.script.find("if = { limit = { " + we_win + " } debug_log = \"SACT_7_OPEN_WE_WIN\" }") !=
                          std::string::npos &&
                  c.script.find("if = { limit = { " + white + " } debug_log = \"SACT_7_OPEN_WHITE_PEACE\" }") !=
                      std::string::npos &&
                  c.script.find("debug_log = \"SACT_7_OPEN_WE_YIELD\"") != std::string::npos,
              "end_war: the open terms are listed (we_win at our 100, white peace under their 25, we_yield always)");
        CHECK(c.ok && c.script.find("debug_log = \"SACT_7_SCORE_GE_M25\"") != std::string::npos &&
                  c.script.find("debug_log = \"SACT_7_SCORE_GE_100\"") != std::string::npos,
              "end_war: our score band is listed");
        CHECK(c.ok && c.script.find("if = { limit = { any_character_war = { " + between + " " + we_win +
                                    " } } random_character_war = { limit = { " + between + " " + we_win +
                                    " } if = { limit = { primary_attacker = title:c_leinster.holder } "
                                    "end_war = attacker } else = { end_war = defender } } "
                                    "debug_log = \"SACT_7_RAN\" } else = { debug_log = \"SACT_7_REFUSED\" }") !=
                          std::string::npos,
              "end_war we_win: guarded by our score at 100, OUR side wins");
        CHECK(c.ok && c.script.find("} else = { debug_log = \"SACT_7_NO_WAR\" debug_log = \"SACT_7_REFUSED\" } }") !=
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
                  c.script.find("debug_log = \"SACT_7_RAN\" } else") != std::string::npos,
              "end_war read only: the listing and no end_war");
        for (const char* t : {"surrender", "victory", "enforce_demands", "attacker", "WE_WIN", "we_win }"}) {
            s.peace_terms = t;
            Composed r = compose(Template::kEndWar, s);
            std::string what = std::string("end_war: terms '") + t + "' refused bad_peace_terms";
            CHECK(!r.ok && r.why == "bad_peace_terms", what.c_str());
        }
        s.peace_terms = "we_win"; s.target_title = "d_meath } add_gold = 1 {";
        CHECK(!compose(Template::kEndWar, s).ok, "end_war: a bad enemy key refused");
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
        CHECK(sizeof(kTemplates) / sizeof(kTemplates[0]) == 11, "the enumerated set is eleven templates");
    }

    // ── 4. the incoming-proposal queue (lane QMOD) ─────────────────────────
    {
        Slots s = base();
        s.proposal_on = true;
        Composed c = compose(Template::kProposalSeat, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { set_variable = { name = ck3agent_proposal_seat value = 1 days = 400 } "
              "if = { limit = { has_variable = ck3agent_proposal_seat } debug_log = \"SACT_7_RAN\" } else = { "
              "debug_log = \"SACT_7_REFUSED\" } }",
              "proposal_seat on: the 400-day marker, read back by the guard");
        s.proposal_on = false;
        c = compose(Template::kProposalSeat, s);
        CHECK(c.ok && c.script.find("remove_variable = ck3agent_proposal_seat") != std::string::npos &&
              c.script.find("has_variable = ck3agent_proposal_seat } debug_log = \"SACT_7_REFUSED\"") != std::string::npos,
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
              "var:ck3agent_proposal_answer_alliance = 2 } debug_log = \"SACT_7_RAN\" } else = { "
              "debug_log = \"SACT_7_REFUSED\" } }",
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
              "title:c_leinster.holder = { debug_log = \"SACT_7_BEGIN\" every_owned_story = { limit = { "
              "story_type = ck3agent_proposal ck3agent_proposal_open_trigger = yes } "
              "ck3agent_proposal_list_item_effect = yes } debug_log = \"SACT_7_END\" debug_log = \"SACT_7_RAN\" }",
              "proposal_list: the seat's OWN open stories, between BEGIN and END");
    }
    {
        Slots s = base();
        s.proposal_id = 12;
        s.proposal_answer = proposal_answer_code("accept");
        s.proposal_by = proposal_by_code("seat");
        Composed c = compose(Template::kProposalAnswer, s);
        CHECK(c.ok && c.script ==
              "title:c_leinster.holder = { debug_log = \"SACT_7_BEGIN\" every_owned_story = { limit = { "
              "story_type = ck3agent_proposal var:ck3agent_proposal_id = 12 } save_scope_as = ck3a_found "
              "ck3agent_proposal_answer_effect = { ANSWER = 1 BY = 1 } } if = { limit = { NOT = { exists = "
              "scope:ck3a_found } } debug_log = \"CK3AGENT_PROPOSAL_OUTCOME|refused|not_found\" } "
              "debug_log = \"SACT_7_END\" debug_log = \"SACT_7_RAN\" }",
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
        // lane QFIX: proposal_stage is deleted (live i11p6: run_interaction ...
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
        // lane peace: end_war's reading, the other marks of the token.
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
        // error.log of the probe's declare_war_interaction (e12c mp_e2), verbatim
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

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
