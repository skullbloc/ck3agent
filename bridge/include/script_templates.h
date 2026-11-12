// script_templates.h — the script action layer's FIXED templates (lane v4act).
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
//: probe's mod: "modprobe|mod/modprobe.mod|Enabled", lane modprobe e3a).
//: scripts/policy_mod.py ENABLED_LINE is the same string (test_policy_mod.py).
inline constexpr const char* kPolicyModEnabledLine =
    "ck3agent_policies|mod/ck3agent_policies.mod|Enabled";
inline constexpr const char* kPolicyEvent = "ck3agent_policy.1";

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
    std::string token;             // SACT_<n>
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
    const std::string ran = "debug_log = \"" + ran_marker(s.token) + "\"";
    const std::string refused = "debug_log = \"" + refused_marker(s.token) + "\"";
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

}  // namespace bridge::script_tpl
