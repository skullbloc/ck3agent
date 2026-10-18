// test/acceptance_board.cpp — ck-6kvq.12 (R4) SHARED-BOARD acceptance, HOST lane.
//
// An ACCEPTANCE, not a unit test, and the difference is deliberate:
//
//   * test_board.cpp asserts. The first violated invariant aborts the process
//     and every later check goes unrun — right for a regression gate, useless
//     as evidence.
//   * This binary REPORTS. Every check runs, each prints PASS/FAIL with a name,
//     and `--json` emits a machine-readable summary that
//     scripts/acceptance_board.py compares — including against a build with the
//     attribution fix PATCHED OUT, which must FAIL. An acceptance that cannot
//     fail is not evidence.
//
// It links the REAL bridge/src/board.cpp AND the REAL bridge/src/session.cpp,
// so the attribution it checks is minted from the same ownership table the
// dispatch choke point consults, and the observe-mode refusal it checks is the
// same `action_is_mutation` / `validate_actor_ownership` pair that refuses a
// real observe session on the wire. It does NOT need CK3: everything here is
// bookkeeping, which is pure host code. What it therefore does NOT cover is the
// dispatch envelope and the MCP surface — that is the python driver's arm-gate
// lane, and (for the wire) the live lane of scripts/acceptance_board.py.
//
// Phases:
//   A1 attribution — six sessions post; every record carries the REGISTRY's
//                    session_id and character_id, the clock owner's date/tick,
//                    and the conductor's flush_id
//   A2 forgery     — a control session posting as another session is IGNORED,
//                    not believed; this is the phase the negative control breaks
//   A3 order       — interleaved posts form ONE total order, seq monotonic,
//                    read() a resumable cursor over it
//   A4 quotas      — every bound is a TYPED refusal; nothing truncated, nothing evicted
//   A5 episode     — a new episode starts at seq 0 and drops the old board
//   A6 observe     — an observe session READS and structurally cannot POST
//   A7 arm gate    — the board exists in arm b and in no other arm

#include "bridge.h"          // MemRange, for the stub globals below
#include "board.h"
#include "episode_policy.h"
#include "session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <vector>

// Stub globals referenced by session.cpp when compiled standalone (the same set
// test_session_registry and acceptance_n6_ownership provide). ck-81su added
// world_identity.cpp / world_epoch.cpp to this target's link (the board's
// carrier is judged by ck-9fc3's rule), which is where the memory-range globals
// come from.
namespace bridge {
FILE* g_logfile = nullptr;
// ck-6kvq.5: session.cpp now takes the RESUME decision with ck-9fc3's
// judge_same_save() — ONE identity rule, not a second copy — so linking it
// pulls in world_identity.cpp and world_epoch.cpp, whose guarded reads need
// these. Same stub set as acceptance_n6_ownership.cpp, test_client_slot.cpp,
// acceptance_save_identity.cpp, acceptance_world_reset.cpp and
// acceptance_session_resume.cpp; the CMake target takes SESSION_HOST_SOURCES.
uintptr_t             g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex     g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}

using namespace bridge;

namespace {

struct Check {
    std::string phase, name, detail;
    bool ok;
};

std::vector<Check> g_checks;
bool g_quiet = false;

bool check(const char* phase, const char* name, bool ok,
           const std::string& detail = std::string()) {
    g_checks.push_back({phase, name, detail, ok});
    if (!g_quiet) {
        printf("  [%-3s] %-58s %s%s%s\n", phase, name, ok ? "PASS" : "FAIL",
               detail.empty() ? "" : "  — ", detail.c_str());
    }
    return ok;
}

std::string json_escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Six distinct claimable 1066 characters — the N=6 SHAPE, which is all this
// acceptance needs. NOT an English roster: see ck-7bzi. These are the ids the
// last live N=6 run claimed, and five of the six are foreign (32624 is the King
// of Scotland, 37067 the King of France). The English roster is derived from
// the save by scripts/english_roster.py and lives nowhere else.
const uint32_t kChars[6] = {30853, 32624, 37067, 33715, 31012, 30744};

void reset_registry() {
    g_session_registry.set_manifest_primary(0);
    for (const std::string& sid : g_session_registry.ids())
        g_session_registry.destroy(sid);
}

// THE PRODUCTION MINT, reproduced from dispatch_router.cpp's
// mint_board_attribution(): session_id and character_id off the installed
// session (i.e. the ownership table), date and tick from the clock owner,
// flush_id from the conductor. Reproduced rather than linked because the real
// one lives in an engine-coupled translation unit; what it must NOT do — read
// any of these from the caller's frame — is what the whole file is about.
BoardAttribution mint_for(Session* s, const char* game_date, uint64_t tick,
                          uint64_t flush_id) {
    BoardAttribution who;
    if (s) {
        who.session_id = s->session_id;
        std::lock_guard<std::mutex> lk(s->mutex);
        who.character_id = s->primary_character_id;
    }
    who.game_date = game_date;
    who.tick = tick;
    who.flush_id = flush_id;
    return who;
}

BoardConfig episode_config() {
    BoardConfig c;
    c.enabled = true;
    c.max_post_bytes = 256;
    c.max_posts_per_session_per_flush = 2;
    c.max_posts_per_episode = 64;
    c.require_flush_scope = true;   // an experiment episode
    return c;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

// ---------------------------------------------------------------------------
// A1 — attribution is minted from the ownership table and the clock owner
// ---------------------------------------------------------------------------
static void phase_attribution(EpisodeBoard& b, std::vector<Session*>& ctrl) {
    reset_registry();
    ctrl.clear();
    for (int i = 0; i < 6; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "agent-%d", i);
        Session* s = g_session_registry.create(name, Session::Mode::CONTROL);
        bool claimed = g_session_registry.claim_character(s, kChars[i], nullptr);
        if (!claimed) check("A1", "six sessions claim six characters", false,
                            "claim refused");
        ctrl.push_back(s);
    }
    check("A1", "six control sessions claim six characters",
          g_session_registry.controlled_claim_count() == 6);

    b.configure(episode_config());
    b.begin_episode("ep-20260912T000000Z-b-acceptance");

    bool all_ok = true, ids_ok = true, chars_ok = true, clock_ok = true;
    for (int i = 0; i < 6; ++i) {
        BoardPostResult r = b.post_text("muster " + std::to_string(i),
                                        mint_for(ctrl[i], "1066.10.14",
                                                 100 + i, 7));
        all_ok   &= r.ok;
        ids_ok   &= (r.post.who.session_id == ctrl[i]->session_id);
        chars_ok &= (r.post.who.character_id == kChars[i]);
        clock_ok &= (r.post.who.game_date == "1066.10.14" &&
                     r.post.who.tick == static_cast<uint64_t>(100 + i) &&
                     r.post.who.flush_id == 7);
    }
    check("A1", "every post is accepted", all_ok);
    check("A1", "session_id on every post is the REGISTRY's", ids_ok);
    check("A1", "character_id on every post is the CLAIMED character", chars_ok);
    check("A1", "game_date, tick and flush_id come from the clock owner", clock_ok);

    // Every post is attributed to a session that actually owns its character —
    // the property the paper's mediator analysis rests on.
    bool owned = true;
    for (const BoardPost& p : b.read(0, 0).posts) {
        Session* owner = g_session_registry.owner_of(p.who.character_id);
        owned &= (owner != nullptr && owner->session_id == p.who.session_id);
    }
    check("A1", "every post's author owns the character it speaks for", owned);
}

// ---------------------------------------------------------------------------
// A2 — a control session CANNOT forge another session's authorship
// ---------------------------------------------------------------------------
static void phase_forgery(EpisodeBoard& b, std::vector<Session*>& ctrl) {
    b.begin_episode("ep-forgery");

    Session* impostor = ctrl[1];
    Session* victim   = ctrl[0];

    // Every field of the attribution spine, claimed in the frame, both framed
    // and flat, so neither framing is a hole.
    std::string framed =
        "{\"action\":\"board_post\",\"params\":{\"text\":\"the king orders a retreat\","
        "\"session_id\":" + quoted(victim->session_id) + ",\"character_id\":30853,"
        "\"game_date\":\"1200.01.01\",\"tick\":999999,\"flush_id\":424242}}";
    std::string flat =
        "{\"action\":\"board_post\",\"text\":\"flat forgery\","
        "\"session_id\":" + quoted(victim->session_id) + ",\"character_id\":30853,"
        "\"game_date\":\"1200.01.01\",\"tick\":999999,\"flush_id\":424242}";

    const BoardAttribution minted = mint_for(impostor, "1066.10.20", 200, 9);

    BoardPostResult r1 = b.post(framed.c_str(), minted);
    check("A2", "a forged FRAMED post is accepted, not refused", r1.ok,
          r1.ok ? "" : "reason=" + r1.reason);
    check("A2", "its session_id is the impostor's own, not the victim's",
          r1.post.who.session_id == impostor->session_id,
          "recorded=" + r1.post.who.session_id);
    check("A2", "its character_id is the impostor's own (32624, not 30853)",
          r1.post.who.character_id == kChars[1],
          "recorded=" + std::to_string(r1.post.who.character_id));
    check("A2", "its game_date is the clock owner's, not the claimed 1200.01.01",
          r1.post.who.game_date == "1066.10.20", "recorded=" + r1.post.who.game_date);
    check("A2", "its tick and flush_id are the bridge's",
          r1.post.who.tick == 200 && r1.post.who.flush_id == 9);
    check("A2", "the TEXT is the only thing taken from the frame",
          r1.post.text == "the king orders a retreat");

    BoardPostResult r2 = b.post(flat.c_str(), minted);
    check("A2", "a forged FLAT post is attributed the same way",
          r2.ok && r2.post.who.session_id == impostor->session_id &&
          r2.post.who.character_id == kChars[1]);

    // The property stated as the board sees it: no post on the board is
    // attributed to a session other than the one that made it.
    bool none_forged = true;
    for (const BoardPost& p : b.read(0, 0).posts)
        none_forged &= (p.who.session_id == impostor->session_id);
    check("A2", "NO post on the board carries a forged author", none_forged);

    // ...and the victim's identity appears nowhere in the record.
    check("A2", "the victim's session_id appears nowhere in board.jsonl",
          !contains(b.export_jsonl(), "\"session_id\":" + quoted(victim->session_id)));
}

// ---------------------------------------------------------------------------
// A3 — one total order, monotonic seq, a resumable cursor
// ---------------------------------------------------------------------------
static void phase_order(EpisodeBoard& b, std::vector<Session*>& ctrl) {
    b.begin_episode("ep-order");
    check("A3", "a fresh board is at seq 0", b.next_seq() == 0);

    // Three flushes, six sessions, interleaved — the shape arm B actually runs.
    uint64_t expect = 0;
    bool seq_ok = true;
    for (uint64_t flush = 1; flush <= 3; ++flush) {
        for (int i = 0; i < 6; ++i) {
            BoardPostResult r = b.post_text(
                "f" + std::to_string(flush) + "-s" + std::to_string(i),
                mint_for(ctrl[i], "1066.10.14", 300 + flush, flush));
            seq_ok &= (r.ok && r.post.seq == expect);
            ++expect;
        }
    }
    check("A3", "18 interleaved posts take 18 consecutive seqs", seq_ok,
          "next_seq=" + std::to_string(b.next_seq()));

    const BoardReadResult all = b.read(0, 0);
    bool order_ok = all.posts.size() == 18;
    for (size_t i = 0; i < all.posts.size(); ++i)
        order_ok &= (all.posts[i].seq == i);
    check("A3", "read() returns them in one total order", order_ok);

    // Flush grouping survives the ordering: all six of a flush are adjacent,
    // which is what makes reply latency countable in flushes.
    bool grouped = true;
    for (size_t i = 0; i < all.posts.size(); ++i)
        grouped &= (all.posts[i].who.flush_id == (i / 6) + 1);
    check("A3", "posts are grouped by the flush that executed them", grouped);

    // A resumable cursor: repeated reads see every post exactly once.
    uint64_t cursor = 0;
    size_t seen = 0, rounds = 0;
    bool dupes = false;
    std::vector<bool> hit(18, false);
    while (rounds++ < 50) {
        BoardReadResult page = b.read(cursor, 5);
        if (page.posts.empty()) break;
        for (const BoardPost& p : page.posts) {
            if (p.seq < hit.size()) {
                if (hit[p.seq]) dupes = true;
                hit[p.seq] = true;
            }
            ++seen;
        }
        cursor = page.next_seq;
    }
    check("A3", "paging with since_seq sees every post EXACTLY once",
          seen == 18 && !dupes, "seen=" + std::to_string(seen));
    check("A3", "a caught-up reader gets an empty page, not a replay",
          b.read(cursor, 0).posts.empty() && cursor == 18);
}

// ---------------------------------------------------------------------------
// A4 — bounds refuse, typed; nothing truncated, nothing evicted
// ---------------------------------------------------------------------------
static void phase_quotas(EpisodeBoard& b, std::vector<Session*>& ctrl) {
    BoardConfig c = episode_config();
    c.max_post_bytes = 16;
    c.max_posts_per_session_per_flush = 2;
    c.max_posts_per_episode = 5;
    b.configure(c);
    b.begin_episode("ep-quota");

    const std::string big(17, 'x');
    BoardPostResult r = b.post_text(big, mint_for(ctrl[0], "1066.10.14", 1, 1));
    check("A4", "an oversized post is refused board_quota_exceeded",
          !r.ok && r.reason == board_reason::kQuotaExceeded, "reason=" + r.reason);
    check("A4", "the refusal names max_post_bytes and carries limit/observed",
          r.quota == board_quota::kPostBytes && r.limit == 16 && r.observed == 17);
    check("A4", "NOTHING WAS TRUNCATED — the post is absent, not shortened",
          b.total_posts() == 0);

    b.post_text("a", mint_for(ctrl[0], "1066.10.14", 1, 1));
    b.post_text("b", mint_for(ctrl[0], "1066.10.14", 1, 1));
    BoardPostResult third = b.post_text("c", mint_for(ctrl[0], "1066.10.14", 1, 1));
    check("A4", "a third post in one flush is refused max_posts_per_session_per_flush",
          !third.ok && third.quota == board_quota::kPostsPerFlush);
    check("A4", "the quota is PER SESSION — a second session posts freely",
          b.post_text("d", mint_for(ctrl[1], "1066.10.14", 1, 1)).ok);
    check("A4", "the quota resets at the next flush",
          b.post_text("e", mint_for(ctrl[0], "1066.10.15", 2, 2)).ok);

    b.post_text("f", mint_for(ctrl[2], "1066.10.15", 2, 2));
    BoardPostResult over = b.post_text("g", mint_for(ctrl[3], "1066.10.15", 2, 2));
    check("A4", "the episode ceiling refuses max_posts_per_episode",
          !over.ok && over.quota == board_quota::kPostsPerEpisode);
    check("A4", "NOTHING WAS EVICTED — append-only survives the ceiling",
          b.total_posts() == 5);
    check("A4", "every refusal is counted for the record",
          b.total_refusals() == 3, "refusals=" + std::to_string(b.total_refusals()));

    // The surviving form of "a post outside a window is refused" (§3.2 as
    // corrected 2026-09-12): an experiment episode refuses an UNANCHORED post.
    b.begin_episode("ep-flushscope");
    BoardPostResult un = b.post_text("unanchored", mint_for(ctrl[0], "1066.10.14", 1, 0));
    check("A4", "an experiment episode refuses a post with no flush behind it",
          !un.ok && un.reason == board_reason::kOutsideFlush, "reason=" + un.reason);
}

// ---------------------------------------------------------------------------
// A5 — per-episode: a new episode starts at seq 0
// ---------------------------------------------------------------------------
static void phase_episode(EpisodeBoard& b, std::vector<Session*>& ctrl) {
    b.configure(episode_config());
    b.begin_episode("ep-first");
    for (int i = 0; i < 4; ++i)
        b.post_text("first-" + std::to_string(i),
                    mint_for(ctrl[i], "1066.10.14", 1, 1));
    check("A5", "episode one has four posts", b.total_posts() == 4);
    check("A5", "its board.jsonl is stamped with episode one's id",
          contains(b.export_jsonl(), "\"episode_id\":\"ep-first\""));

    b.begin_episode("ep-second");
    check("A5", "A NEW EPISODE STARTS AT SEQ 0", b.next_seq() == 0);
    check("A5", "a new episode carries none of the old posts", b.total_posts() == 0);
    check("A5", "its export is empty, not episode one's", b.export_jsonl().empty());
    BoardPostResult r = b.post_text("second", mint_for(ctrl[0], "1067.01.01", 1, 1));
    check("A5", "the first post of the new episode is seq 0",
          r.ok && r.post.seq == 0 && r.post.post_id == "bp_00000000");
    check("A5", "and it is stamped with episode two's id",
          contains(b.export_jsonl(), "\"episode_id\":\"ep-second\"") &&
          !contains(b.export_jsonl(), "ep-first"));
    check("A5", "the per-flush quota does not leak across episodes",
          b.post_text("second-2", mint_for(ctrl[0], "1067.01.01", 1, 1)).ok);
}

// ---------------------------------------------------------------------------
// A6 — an observe session READS and structurally cannot POST
// ---------------------------------------------------------------------------
static void phase_observe(EpisodeBoard& b, std::vector<Session*>& ctrl) {
    Session* obs = g_session_registry.create("observer-meter", Session::Mode::OBSERVE);

    // The classification the dispatch choke point uses, asserted on the REAL
    // session.cpp tables rather than restated here.
    check("A6", "board_read is classified READ-ONLY", action_is_readonly("board_read"));
    check("A6", "board_post is classified a MUTATION", action_is_mutation("board_post"));

    // Read: an observe session's read is never refused by ownership validation.
    std::string reason;
    check("A6", "an observe session's board_read passes ownership validation",
          validate_actor_ownership(0, "board_read", &reason), "reason=" + reason);

    // Post: refused, typed, for the same reason a real observe session is
    // refused on the wire — observe sessions are read-only.
    reason.clear();
    ClientContext ctx;
    ctx.session = obs;
    {
        ClientContextGuard guard(&ctx);
        bool refused = !validate_actor_ownership(kChars[0], "board_post", &reason);
        check("A6", "an observe session's board_post is refused observe_mode",
              refused && reason == "observe_mode", "reason=" + reason);
    }

    // And it never gets an identity to post with: an observe session claims
    // nothing, so the mint produces no session_id and the board refuses it
    // outright — two independent gates, neither relying on the other.
    BoardAttribution anon = mint_for(nullptr, "1066.10.14", 1, 1);
    BoardPostResult r = b.post_text("observer speaking", anon);
    check("A6", "an unattributable post is refused board_no_session",
          !r.ok && r.reason == board_reason::kNoSession, "reason=" + r.reason);

    // The observer DOES see everything the control sessions wrote.
    b.begin_episode("ep-observe");
    for (int i = 0; i < 3; ++i)
        b.post_text("m" + std::to_string(i), mint_for(ctrl[i], "1066.10.14", 1, 1));
    const BoardReadResult seen = b.read(0, 0);
    check("A6", "the observer reads every post the controllers made",
          seen.posts.size() == 3 && seen.next_seq == 3);
    check("A6", "and the volume block it meters is populated",
          contains(b.stats_json(), "\"posts\":3") &&
          contains(b.stats_json(), "\"by_session\":{"));

    g_session_registry.destroy(obs->session_id);
}

// ---------------------------------------------------------------------------
// A7 — the arm gate
// ---------------------------------------------------------------------------
static void phase_arm_gate() {
    setenv("CK3_EPISODE_MODE", "experiment", 1);
    setenv("CK3_EPISODE_ID", "ep-arm", 1);
    unsetenv("CK3_BOARD_ENABLED");

    struct Case { const char* arm; bool want; const char* what; };
    const Case cases[] = {
        {nullptr, false, "free play (no arm declared) has NO board"},
        {"e0",    false, "arm E0 has NO board"},
        {"a",     false, "arm A has NO board (G5 option A2: ABSENT)"},
        {"b",     true,  "arm B has the board"},
        {"c",     false, "arm C has NO board"},
    };
    bool all = true;
    for (const Case& c : cases) {
        if (c.arm) setenv("CK3_EPISODE_ARM", c.arm, 1); else unsetenv("CK3_EPISODE_ARM");
        episode_policy().init_from_env();
        board_init_from_env();
        all &= check("A7", c.what, board().enabled() == c.want);
    }
    check("A7", "the arm gate answers correctly in every arm", all);
    check("A7", "exactly two verbs are arm-gated",
          is_board_action("board_post") && is_board_action("board_read") &&
          !is_board_action("board_admin") && !is_board_action("query_realm"));

    // THE GATE AS THE DISPATCH ROUTER SEES IT. board_verb_registration() is the
    // single function both dispatch_action() and is_registered_action()
    // delegate to, so what is checked here is the rule those two sites apply —
    // not a restatement of it. 0 means "a board verb that is NOT REGISTERED",
    // which is what makes `board_post` in arm A indistinguishable from a typo.
    setenv("CK3_EPISODE_ARM", "a", 1);
    unsetenv("CK3_BOARD_ADMIN_TOKEN");
    episode_policy().init_from_env();
    board_init_from_env();
    check("A7", "arm A: board_post is NOT REGISTERED (unknown_action)",
          board_verb_registration("board_post") == 0);
    check("A7", "arm A: board_read is NOT REGISTERED", 
          board_verb_registration("board_read") == 0);
    check("A7", "arm A: board_admin is NOT REGISTERED",
          board_verb_registration("board_admin") == 0);
    check("A7", "a non-board verb is left to the registry (-1)",
          board_verb_registration("query_realm") == -1 &&
          board_verb_registration("declare_war") == -1);

    setenv("CK3_EPISODE_ARM", "b", 1);
    episode_policy().init_from_env();
    board_init_from_env();
    check("A7", "arm B: both board verbs ARE registered",
          board_verb_registration("board_post") == 1 &&
          board_verb_registration("board_read") == 1);
    check("A7", "arm B: board_admin stays unregistered with no host token",
          board_verb_registration("board_admin") == 0);
    setenv("CK3_BOARD_ADMIN_TOKEN", "t0ken", 1);
    check("A7", "arm B + a host token: board_admin is registered",
          board_verb_registration("board_admin") == 1);
    unsetenv("CK3_BOARD_ADMIN_TOKEN");
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) { json = true; g_quiet = true; }
    }
    if (!json) printf("\nck-6kvq.12 — shared-board acceptance (host lane)\n\n");

    EpisodeBoard b;
    std::vector<Session*> ctrl;
    phase_attribution(b, ctrl);
    phase_forgery(b, ctrl);
    phase_order(b, ctrl);
    phase_quotas(b, ctrl);
    phase_episode(b, ctrl);
    phase_observe(b, ctrl);
    phase_arm_gate();

    size_t failed = 0;
    for (const Check& c : g_checks) if (!c.ok) ++failed;

    if (json) {
        printf("{\"acceptance\":\"board\",\"total\":%zu,\"failed\":%zu,\"checks\":[",
               g_checks.size(), failed);
        for (size_t i = 0; i < g_checks.size(); ++i) {
            const Check& c = g_checks[i];
            printf("%s{\"phase\":\"%s\",\"name\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
                   i ? "," : "", c.phase.c_str(), json_escape(c.name).c_str(),
                   c.ok ? "true" : "false", json_escape(c.detail).c_str());
        }
        printf("]}\n");
    } else {
        printf("\n%zu checks: %zu pass, %zu fail\n", g_checks.size(),
               g_checks.size() - failed, failed);
        printf("acceptance_board[host]: %s\n", failed ? "FAIL" : "PASS");
    }
    return failed ? 1 : 0;
}
