// test/test_engine_id.cpp — the whole id a game function compares (include/engine_id.h).
//
// THE HAZARD: the game's command and effect functions resolve a character (or a mercenary
// company) by slot and then compare the WHOLE stored id with the one they were given. A
// character born during the game carries a generation byte in bits 24-31 (0x01006dd0 for slot
// 0x6dd0), so an id handed over as its low 24 bits resolves to the null sentinel: every hire by
// such a ruler was refused (CanExecute=0), and the men-at-arms recruit, the raid toggle, the
// council seat swap and the war creation took the same masked id.
// Three halves:
//   decision — engine_id::full_or on the cases;
//   census   — no bridge source stores or passes a value masked with & 0xFFFFFF into a command
//              buffer or a command constructor, and the census fires on masked statements (a control);
//   wiring   — each of the five calls passes the whole id (comment-stripped sources).
// Usage: test_engine_id <bridge-source-dir>

#include "engine_id.h"

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

static int g_fails = 0, g_checks = 0;
static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static std::string strip_comments(const std::string& src) {
    std::string out;
    for (size_t i = 0; i < src.size();) {
        if (src.compare(i, 2, "//") == 0) { while (i < src.size() && src[i] != '\n') i++; continue; }
        if (src.compare(i, 2, "/*") == 0) {
            size_t e = src.find("*/", i + 2);
            i = (e == std::string::npos) ? src.size() : e + 2;
            continue;
        }
        if (src[i] == '"') {
            out += src[i++];
            while (i < src.size() && src[i] != '"') { if (src[i] == '\\') out += src[i++]; out += src[i++]; }
            if (i < src.size()) out += src[i++];
            continue;
        }
        out += src[i++];
    }
    return out;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string body_of(const std::string& src, const std::string& head) {
    size_t a = src.find(head);
    if (a == std::string::npos) return "";
    size_t b = src.find("\n}\n", a + head.size());
    return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// Statements (up to ';') that hand a value masked to 24 bits to a command: a constructor call
// on a command buffer, or a store into one. `& 0xFFFFFF` not followed by another hex digit.
static std::vector<std::string> masked_into_command(const std::string& code) {
    static const std::regex mask(R"(&\s*0x[fF]{6}(?![0-9a-fA-F]))");
    static const std::regex sink(R"((\bctor\s*\(\s*cmd\b)|(reinterpret_cast<\s*u?int32_t\s*\*\s*>\s*\(\s*cmd\s*\+[^)]*\)\s*=))");
    std::vector<std::string> hits;
    size_t start = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        if (code[i] != ';' && code[i] != '{' && code[i] != '}') continue;
        std::string stmt = code.substr(start, i - start);
        start = i + 1;
        if (std::regex_search(stmt, sink) && std::regex_search(stmt, mask)) hits.push_back(stmt);
    }
    return hits;
}

int main(int argc, char** argv) {
    using namespace engine_id;
    printf("test_engine_id\n\ndecision\n");
    check(full_or(0x6dd0u, 0x01006dd0u) == 0x01006dd0u, "low 24 bits asked, generation 1 stored: the whole id");
    check(full_or(0x01006dd0u, 0x01006dd0u) == 0x01006dd0u, "the whole id asked: unchanged");
    check(full_or(36303u, 36303u) == 36303u, "a start-save character (no generation byte): unchanged");
    check(full_or(0x6dd0u, 0x01006dd1u) == 0x6dd0u, "another slot stored: the asked id, nothing made up");
    check(full_or(0x6dd0u, kNone) == 0x6dd0u && full_or(0x6dd0u, 0u) == 0x6dd0u,
          "nothing resolved (sentinel or 0): the asked id");
    check(full_or(kNone, 0x01006dd0u) == kNone, "the none sentinel stays the sentinel");
    check(slot(0x01006dd0u) == 0x6dd0u, "slot() is the low 24 bits");

    const std::string dir = argc > 1 ? argv[1] : ".";

    printf("\ncensus\n");
    // Control: the census must flag statements of the masked shape the handlers once had.
    const std::string masked_lines = strip_comments(
        "    ctor(cmd, actor_id & 0xFFFFFF, company_id & 0xFFFFFF, contract_len);\n"
        "    *reinterpret_cast<uint32_t*>(cmd + 0x28) = actor_id & 0xFFFFFF;\n"
        "    ctor(cmd, actor_id & 0xFFFFFF, actor_id & 0xFFFFFF,\n"
        "         reinterpret_cast<void*>(maa_type_obj), 0xFFFFFFFF, 1);\n");
    check(masked_into_command(masked_lines).size() == 3, "control: the census flags the three masked statements");
    const std::string clean_lines = strip_comments(
        "    ctor(cmd, actor_id, company_full, contract_len);\n"
        "    *reinterpret_cast<uint32_t*>(cmd + 0x24) = 0xFFFFFFFF;\n"
        "    r.value_uint(company_id & 0xFFFFFF);\n");
    check(masked_into_command(clean_lines).empty(),
          "control: a whole id, the 0xFFFFFFFF sentinel and a masked reply value are not flagged");

    std::vector<std::string> files;
    if (DIR* d = opendir((dir + "/src").c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".cpp") == 0 || n.compare(n.size() - 4, 4, ".inc") == 0))
                files.push_back(n);
        }
        closedir(d);
    }
    check(files.size() > 20, "the bridge sources were listed");
    size_t hits = 0;
    for (const auto& n : files) {
        for (const auto& h : masked_into_command(strip_comments(read_file(dir + "/src/" + n)))) {
            printf("       masked id into a command in %s: %.160s\n", n.c_str(), h.c_str());
            ++hits;
        }
    }
    check(hits == 0, "no source stores or passes a 24-bit-masked value into a command buffer");

    printf("\nwiring\n");
    const std::string a = strip_comments(read_file(dir + "/src/action_handlers.cpp"));
    const std::string ao = strip_comments(read_file(dir + "/src/army_orders.cpp"));
    const std::string idp = strip_comments(read_file(dir + "/src/intent_dispatch.cpp"));
    const std::string dr = strip_comments(read_file(dir + "/src/dispatch_router.cpp"));
    check(!a.empty() && !ao.empty() && !idp.empty() && !dr.empty(), "the four product sources were read");

    const std::string hire = body_of(a, "bool do_hire_mercenary(");
    check(has(hire, "ctor(cmd, actor_id, company_full, contract_len)"),
          "hire: the hirer's and the company's whole ids reach the command");
    check(has(hire, "engine_id::full_or(company_id") && has(hire, "ENTRY_KEY_REF"),
          "hire: the company's whole id is read off its database entry");
    check(has(hire, "= actor_id;") && has(hire, "= company_full;"), "hire: the Windows stores take the same ids");
    check(has(hire, "uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + 0x18);"),
          "hire: actor_id is the actor's whole id (+0x18)");

    const std::string maa = body_of(a, "bool do_maa_recruit(");
    check(has(maa, "ctor(cmd, actor_id, actor_id,"), "maa_recruit: the owner goes in whole");
    check(has(maa, "(cmd + 0x28) = actor_id;"), "maa_recruit: the Windows store takes the same id");

    const std::string ap = body_of(a, "bool do_appoint_councillor(");
    check(has(ap, "engine_char_id(character_id)") && has(ap, "engine_char_id(holder)"),
          "appoint: both councillors' whole ids are resolved");
    check(has(ap, "swap_leaf(reinterpret_cast<void*>(council), actor, character_full, holder_full)"),
          "appoint: the seat swap gets the whole ids");

    const std::string raid = body_of(ao, "bool do_army_raid(");
    check(has(raid, "ctor(cmd, ao::kSourceAi, actor_full, a.full_id)"), "raid: the ordering character goes in whole");

    const std::string wcc = body_of(idp, "bool execute_war_create_core_intent(");
    check(has(wcc, "game_state, atk_full, def_full, claimant_full,"), "war creation: attacker, defender, claimant whole");
    check(has(wcc, "atk_full = char_get_id(attacker)") && has(wcc, "def_full = char_get_id(defender)") &&
          has(wcc, "claimant_full = engine_char_id(claimant)"),
          "war creation: the whole ids are read off the resolved characters");

    check(has(dr, "uint32_t engine_char_id(uint32_t char_id)") && has(dr, "engine_id::full_or(char_id, char_get_id(ch))"),
          "engine_char_id reads the whole id off the character the slot holds");

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
