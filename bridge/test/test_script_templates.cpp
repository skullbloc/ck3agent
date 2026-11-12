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
        CHECK(sizeof(kTemplates) / sizeof(kTemplates[0]) == 6, "the enumerated set is six templates");
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
