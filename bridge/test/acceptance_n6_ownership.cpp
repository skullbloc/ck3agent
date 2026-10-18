// test/acceptance_n6_ownership.cpp — ck-6kvq.11 (R2) N=6 ownership acceptance,
// HOST lane.
//
// This is an ACCEPTANCE, not a unit test, and the difference is deliberate:
//
//   * test_session_registry.cpp asserts.  The first violated invariant aborts
//     the process and every later check goes unrun, which is right for a
//     regression gate and useless for an acceptance report.
//   * This binary REPORTS.  Every check runs, each one prints PASS/FAIL with a
//     name, and `--json` emits a machine-readable summary the python driver
//     (scripts/acceptance_n6_ownership.py) compares ACROSS FRESH PROCESSES.
//     That cross-process comparison is the one determinism claim a single
//     in-process test structurally cannot make: a process that ran once cannot
//     show that a different process, with a different heap layout and different
//     string hashes, elects the same global primary and publishes the same
//     roster bytes.
//
// It links the REAL bridge/src/session.cpp — the same registry, the same
// rebuild_legacy_mirrors, the same validate_actor_ownership the dispatch choke
// point calls — with only the handful of globals that file references stubbed,
// exactly as test_session_registry does.  It does NOT need CK3: everything
// under test here is ownership bookkeeping, which is pure host code.  What it
// therefore does NOT cover is stated in the driver's --lane live half.
//
// Phases (see scripts/acceptance_n6_ownership.py for the driver):
//   P1  claim      — 6 control sessions claim 6 distinct characters; observer refused
//   P2  cross      — all 30 ordered intruder pairs refused typed owned_by_other
//   P3  determinism— forced map reorder + rehandshakes; primary pinned, roster byte-stable
//   P4  manifest   — designation overrides claim order and follows a succession
//   P5  cap        — overflow refused typed watchlist_full; no silent divergence
//   P6  registry   — the ownership table is readable, ordered and byte-stable

#include "bridge.h"     // ck-6kvq.5: MemRange, for the stub globals below
#include "session.h"

#include <algorithm>
#include <cstdio>
#include <shared_mutex>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Stub globals referenced by session.cpp when compiled standalone (identical to
// the set test_session_registry provides).
namespace bridge {
// ck-6kvq.5: session.cpp now links world_identity.cpp (ck-9fc3's judge_same_save
// — one identity rule, not a second copy), which reaches world_epoch.cpp's
// guarded reads. These are the globals those engine-free TUs reference when the
// game is not there.
FILE* g_logfile = nullptr;
uintptr_t              g_base_addr = 0;
std::vector<MemRange>  g_valid_ranges;
std::shared_mutex      g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}

using namespace bridge;

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
namespace {

struct Check {
    std::string phase;
    std::string name;
    bool ok;
    std::string detail;
};

std::vector<Check> g_checks;
bool g_quiet = false;

bool check(const char* phase, const char* name, bool ok,
           const std::string& detail = std::string()) {
    g_checks.push_back({phase, name, ok, detail});
    if (!g_quiet) {
        printf("  [%-4s] %-46s %s%s%s\n", phase, name, ok ? "PASS" : "FAIL",
               detail.empty() ? "" : "  — ", detail.c_str());
    }
    return ok;
}

// The roster as a canonical byte string.  This is the artefact the driver
// compares across processes: "byte-stable roster" is a claim about these exact
// bytes, not about set equality, because a roster that reorders between
// rehandshakes reorders every downstream snapshot with it.
std::string roster_bytes() {
    std::lock_guard<std::mutex> lk(g_controlled_mutex);
    std::string s;
    for (size_t i = 0; i < g_controlled_chars.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(g_controlled_chars[i]);
    }
    return s;
}

void reset_registry() {
    g_session_registry.set_manifest_primary(0);
    for (const std::string& sid : g_session_registry.ids())
        g_session_registry.destroy(sid);
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

// The 1066 shape: one king plus five earls, which is exactly the N the epic
// asks for (design §R, arm A: N=6 on the English side).
const uint32_t kKing = 6001;
const uint32_t kEarls[5] = {6002, 6003, 6004, 6005, 6006};

}  // namespace

// ---------------------------------------------------------------------------
// P1 — six control sessions each claim a distinct character
// ---------------------------------------------------------------------------
static void phase_claim(std::vector<Session*>& ctrl, Session*& obs) {
    reset_registry();

    ctrl.clear();
    ctrl.push_back(g_session_registry.create("agent-king", Session::Mode::CONTROL));
    for (int i = 0; i < 5; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "agent-earl-%d", i);
        ctrl.push_back(g_session_registry.create(name, Session::Mode::CONTROL));
    }
    obs = g_session_registry.create("observer-meter", Session::Mode::OBSERVE);

    bool all_claimed = g_session_registry.claim_character(ctrl[0], kKing, nullptr);
    for (int i = 0; i < 5; ++i)
        all_claimed &= g_session_registry.claim_character(ctrl[i + 1], kEarls[i], nullptr);
    check("P1", "six control sessions claim six characters", all_claimed);

    // Distinct: six sessions, six owners, no character owned twice.
    bool distinct = g_session_registry.owner_of(kKing) == ctrl[0];
    for (int i = 0; i < 5; ++i)
        distinct &= g_session_registry.owner_of(kEarls[i]) == ctrl[i + 1];
    check("P1", "each character owned by its own session", distinct);

    check("P1", "watchlist holds exactly six", g_controlled_chars.size() == 6,
          "roster=" + roster_bytes());
    check("P1", "registry claim count == watchlist size",
          g_session_registry.controlled_claim_count() == g_controlled_chars.size());

    // The observer is refused a claim IN THE REGISTRY, not merely at the
    // dispatch choke points — that is what makes "ownership == control
    // watchlist" true by construction rather than by convention.
    {
        std::string why;
        bool refused = !g_session_registry.claim_character(obs, 6999, &why);
        check("P1", "observe session refused a claim", refused && why == "observe_mode",
              "reason=" + why);
        check("P1", "observe session's refused character is unowned",
              g_session_registry.owner_of(6999) == nullptr);
    }

    std::string detail;
    check("P1", "no ownership/watchlist divergence",
          !g_session_registry.ownership_divergence(&detail), detail);
}

// ---------------------------------------------------------------------------
// P2 — cross-ownership actions are refused with a TYPED reason
// ---------------------------------------------------------------------------
static void phase_cross(std::vector<Session*>& ctrl, Session* obs) {
    const uint32_t chars[6] = {kKing, kEarls[0], kEarls[1], kEarls[2], kEarls[3], kEarls[4]};

    // (a) Registry level: every one of the 30 ordered (owner, intruder) pairs.
    int pairs = 0, refused_typed = 0;
    std::string first_bad;
    for (int owner = 0; owner < 6; ++owner) {
        for (int intruder = 0; intruder < 6; ++intruder) {
            if (owner == intruder) continue;
            ++pairs;
            std::string why;
            bool refused = !g_session_registry.claim_character(ctrl[intruder],
                                                               chars[owner], &why);
            if (refused && why == "owned_by_other") ++refused_typed;
            else if (first_bad.empty())
                first_bad = "pair(" + std::to_string(chars[owner]) + "<-" +
                            std::to_string(intruder) + ") reason=" + why;
        }
    }
    check("P2", "all 30 cross-session claims refused owned_by_other",
          pairs == 30 && refused_typed == 30,
          refused_typed == 30 ? std::to_string(pairs) + " pairs" : first_bad);

    // (b) Dispatch choke point: the same 30 pairs through the function every
    //     mutating handler actually calls.
    int acts = 0, acts_refused = 0;
    std::string first_act_bad;
    for (int owner = 0; owner < 6; ++owner) {
        for (int intruder = 0; intruder < 6; ++intruder) {
            if (owner == intruder) continue;
            ++acts;
            ClientContext ctx;
            ctx.session = ctrl[intruder];
            ctx.action_type = "send_interaction";
            ClientContextGuard guard(&ctx);
            std::string why;
            if (!validate_actor_ownership(chars[owner], "send_interaction", &why) &&
                why == "owned_by_other")
                ++acts_refused;
            else if (first_act_bad.empty())
                first_act_bad = "actor=" + std::to_string(chars[owner]) +
                                " reason=" + why;
        }
    }
    check("P2", "all 30 cross-session ACTIONS refused owned_by_other",
          acts == 30 && acts_refused == 30,
          acts_refused == 30 ? std::to_string(acts) + " actions" : first_act_bad);

    // (c) The owner is allowed on its own actor — the refusals above are a gate,
    //     not a wall.
    int allowed = 0;
    for (int i = 0; i < 6; ++i) {
        ClientContext ctx;
        ctx.session = ctrl[i];
        ctx.action_type = "send_interaction";
        ClientContextGuard guard(&ctx);
        std::string why;
        if (validate_actor_ownership(chars[i], "send_interaction", &why)) ++allowed;
    }
    check("P2", "each owner may act on its own character", allowed == 6,
          std::to_string(allowed) + "/6");

    // (d) Read-only actions bypass ownership — the observer lane must be able to
    //     read the whole board while owning nothing.
    {
        ClientContext ctx;
        ctx.session = ctrl[1];
        ctx.action_type = "query_character";
        ClientContextGuard guard(&ctx);
        std::string why;
        check("P2", "read-only action bypasses ownership",
              validate_actor_ownership(kKing, "query_character", &why));
    }

    // (e) An observe session is refused a MUTATION but not a read.
    {
        ClientContext ctx;
        ctx.session = obs;
        ctx.action_type = "send_interaction";
        ClientContextGuard guard(&ctx);
        std::string why;
        bool refused = !validate_actor_ownership(kKing, "send_interaction", &why);
        check("P2", "observe session refused a mutation",
              refused && why == "observe_mode", "reason=" + why);

        ctx.action_type = "query_character";
        std::string why2;
        check("P2", "observe session still allowed a read",
              validate_actor_ownership(kKing, "query_character", &why2));
    }

    // (f) An unclaimed actor is refused with its own reason — distinct from
    //     owned_by_other, so a handler can tell "someone else's" from "nobody's".
    {
        ClientContext ctx;
        ctx.session = ctrl[0];
        ctx.action_type = "send_interaction";
        ClientContextGuard guard(&ctx);
        std::string why;
        bool refused = !validate_actor_ownership(9999, "send_interaction", &why);
        check("P2", "unclaimed actor refused actor_not_claimed",
              refused && why == "actor_not_claimed", "reason=" + why);
    }

    // Nothing above mutated ownership.
    check("P2", "roster unchanged by the refusal sweep",
          g_controlled_chars.size() == 6, "roster=" + roster_bytes());
}

// ---------------------------------------------------------------------------
// P6 — the registry is READABLE, and it reads the same way twice
//
// SessionRegistry::sessions_json() shipped in M1 with no caller: the ownership
// table was invisible to every client, so "get_controlled_roster agrees with the
// registry" was a claim nothing could check — which is a fair part of why the
// N>1 defects survived three weeks unnoticed. get_bridge_state now publishes it
// (dispatch_router.cpp do_get_bridge_state), so it has to be deterministic too,
// or a client diffing two snapshots would see the session list "change" on every
// rehash.
// ---------------------------------------------------------------------------
static void phase_registry(std::vector<Session*>& ctrl, Session* obs) {
    (void)obs;
    const std::string snap0 = g_session_registry.sessions_json();

    size_t entries = 0;
    for (size_t p = snap0.find("\"session_id\""); p != std::string::npos;
         p = snap0.find("\"session_id\"", p + 1))
        ++entries;
    check("P6", "registry snapshot lists every session", entries == 7,
          std::to_string(entries) + " entries (6 control + 1 observe)");

    check("P6", "snapshot is a JSON array",
          !snap0.empty() && snap0.front() == '[' && snap0.back() == ']');
    check("P6", "snapshot exposes claim_seq (why THIS primary was elected)",
          snap0.find("\"claim_seq\"") != std::string::npos);

    // The head of the array is the claim-order winner, i.e. the session that
    // holds the global primary. That is the property that makes the array
    // auditable rather than decorative.
    const std::string want = "\"primary_character_id\":" + std::to_string(kKing);
    const size_t head_primary = snap0.find("\"primary_character_id\":");
    check("P6", "the first entry holds the global primary",
          head_primary != std::string::npos &&
              snap0.compare(head_primary, want.size(), want) == 0,
          "primary=" + std::to_string(g_controlled_primary.load()));

    // Byte-stability under the same churn that used to move the primary.
    bool stable = true;
    for (int round = 0; round < 8 && stable; ++round) {
        std::vector<Session*> filler;
        for (int i = 0; i < 8; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "snapfill-%d-%d", round, i);
            filler.push_back(g_session_registry.create(name, Session::Mode::OBSERVE));
        }
        for (Session* f : filler) g_session_registry.destroy(f->session_id);
        stable = g_session_registry.sessions_json() == snap0;
    }
    check("P6", "registry snapshot is BYTE-stable across map churn", stable);

    // Every claimed character in the registry appears in the derived watchlist,
    // and nothing else does — the live lane's "roster agrees with the registry"
    // check, done here where it can be proven exhaustively.
    bool agree = g_controlled_chars.size() == 6;
    for (uint32_t cid : g_controlled_chars) {
        const std::string needle = std::to_string(cid);
        bool found = false;
        g_session_registry.for_each([&](const Session& s) {
            std::lock_guard<std::mutex> slock(s.mutex);
            if (s.claimed_character_ids.count(cid)) found = true;
        });
        agree &= found;
    }
    check("P6", "every watchlist entry is owned by some session in the registry",
          agree, "roster=" + roster_bytes());
    check("P6", "registry claim count matches the watchlist exactly",
          g_session_registry.controlled_claim_count() == g_controlled_chars.size(),
          std::to_string(g_session_registry.controlled_claim_count()) + " vs " +
              std::to_string(g_controlled_chars.size()));

    // client_name is taken VERBATIM from a client's handshake frame, and every
    // session's name now rides in every get_bridge_state reply. A name carrying
    // a quote, a backslash, a newline or a raw control byte must not be able to
    // corrupt a response that every other client — the observer meter lane
    // included — has to parse.
    {
        Session* nasty = g_session_registry.create(
            std::string("evil\" \\ \n\t") + std::string(1, '\x01') + "name",
            Session::Mode::OBSERVE);
        const std::string snap = g_session_registry.sessions_json();
        bool escaped = snap.find("\\\"") != std::string::npos &&
                       snap.find("\\n") != std::string::npos &&
                       snap.find("\\u0001") != std::string::npos;
        // No raw control byte and no unescaped newline survived into the wire.
        bool no_raw_control = true;
        for (unsigned char c : snap) {
            if (c < 0x20) { no_raw_control = false; break; }
        }
        check("P6", "a hostile client_name is JSON-escaped, not spliced raw",
              escaped && no_raw_control);
        g_session_registry.destroy(nasty->session_id);
    }

    // ...and an oversized name is truncated rather than echoed in full to every
    // client on every state read.
    {
        Session* big = g_session_registry.create(std::string(4096, 'x'),
                                                 Session::Mode::OBSERVE);
        const std::string snap = g_session_registry.sessions_json();
        check("P6", "an oversized client_name is truncated at the cap",
              snap.find(std::string(MAX_CLIENT_NAME + 1, 'x')) == std::string::npos &&
                  snap.find(std::string(MAX_CLIENT_NAME, 'x')) != std::string::npos,
              "cap=" + std::to_string(MAX_CLIENT_NAME));
        g_session_registry.destroy(big->session_id);
    }

    check("P6", "the snapshot is unchanged once the odd sessions are gone",
          g_session_registry.sessions_json() == snap0);
    (void)ctrl;
}

// ---------------------------------------------------------------------------
// P3 — the global primary is deterministic; the roster is byte-stable
// ---------------------------------------------------------------------------
static void phase_determinism(std::vector<Session*>& ctrl) {
    const std::string roster0 = roster_bytes();

    auto control_order = []() {
        std::vector<uint32_t> order;
        g_session_registry.for_each([&](const Session& s) {
            if (s.is_control() && s.primary_character_id != 0)
                order.push_back(s.primary_character_id);
        });
        return order;
    };

    // Churn the map the way a busy episode does, and do it in a way that does
    // NOT depend on the bucket count still growing: filler create/destroy stops
    // relocating anything once the map has settled, so each round ALSO
    // rehandshakes one rotating earl — destroy the session, mint a new one, let
    // it reclaim. A new session id hashes to a new bucket, so the live sessions
    // genuinely move, which is exactly what a reconnect does in production.
    //
    // The king is never the one rehandshaken here, so he keeps the lowest
    // claim_seq throughout and MUST remain the global primary. We then require
    // proof that the hazard was actually constructed — without it this phase
    // could pass on incidental hash order and mean nothing.
    std::vector<std::vector<uint32_t>> observed{control_order()};
    bool primary_pinned = g_controlled_primary.load() == kKing;
    bool roster_stable = true;
    bool rehandshakes_ok = true;
    for (int round = 0; round < 24; ++round) {
        std::vector<Session*> filler;
        for (int i = 0; i < 8; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "filler-%d-%d", round, i);
            filler.push_back(g_session_registry.create(name, Session::Mode::OBSERVE));
        }
        for (Session* f : filler) g_session_registry.destroy(f->session_id);

        const int which = 1 + (round % 5);  // an earl, never the king
        const uint32_t cid = kEarls[which - 1];
        rehandshakes_ok &= g_session_registry.destroy(ctrl[which]->session_id);
        char name[48];
        snprintf(name, sizeof(name), "agent-earl-%d-r%d", which - 1, round);
        ctrl[which] = g_session_registry.create(name, Session::Mode::CONTROL);
        rehandshakes_ok &= g_session_registry.claim_character(ctrl[which], cid, nullptr);

        observed.push_back(control_order());
        primary_pinned &= g_controlled_primary.load() == kKing;
        roster_stable &= roster_bytes() == roster0;
    }
    check("P3", "24 rotating earl rehandshakes all succeeded", rehandshakes_ok);

    // The rotation above left the earls in an arbitrary claim order, so
    // re-establish a KNOWN one: rehandshake all five in index order, earl 0
    // first.  The checks below then compare against an expectation written
    // down here, rather than against a re-derivation of the very rule under
    // test — which would only prove the implementation agrees with itself.
    // The reorder evidence was already collected, so this costs nothing.
    for (int i = 1; i <= 5; ++i) {
        rehandshakes_ok &= g_session_registry.destroy(ctrl[i]->session_id);
        char name[48];
        snprintf(name, sizeof(name), "agent-earl-%d-settled", i - 1);
        ctrl[i] = g_session_registry.create(name, Session::Mode::CONTROL);
        rehandshakes_ok &= g_session_registry.claim_character(ctrl[i], kEarls[i - 1],
                                                              nullptr);
    }
    observed.push_back(control_order());
    check("P3", "claim order re-established: king, then earls 0..4",
          rehandshakes_ok && g_controlled_primary.load() == kKing &&
              roster_bytes() == roster0,
          "primary=" + std::to_string(g_controlled_primary.load()) +
              " roster=" + roster_bytes());

    bool distinct_orders = false, wrong_front = false;
    for (const auto& o : observed) {
        if (o != observed.front()) distinct_orders = true;
        if (!o.empty() && o.front() != kKing) wrong_front = true;
    }
    check("P3", "the session map really did reorder", distinct_orders,
          std::to_string(observed.size()) + " observations");
    check("P3", "a non-first-claimant led at least one map order", wrong_front,
          "the OLD hash-order rule would have published the wrong primary");
    check("P3", "global primary pinned to the first claimant through the churn",
          primary_pinned, "primary=" + std::to_string(g_controlled_primary.load()));
    check("P3", "roster BYTE-stable through the churn", roster_stable,
          "roster=" + roster_bytes());

    // Rehandshake of a NON-primary session: primary must not move, roster bytes
    // must come back identical.
    bool nonprimary_ok = true, nonprimary_bytes = true;
    for (int round = 0; round < 3; ++round) {
        nonprimary_ok &= g_session_registry.destroy(ctrl[3]->session_id);
        nonprimary_ok &= g_controlled_primary.load() == kKing;
        ctrl[3] = g_session_registry.create("agent-earl-2-again", Session::Mode::CONTROL);
        nonprimary_ok &= g_session_registry.claim_character(ctrl[3], kEarls[2], nullptr);
        nonprimary_ok &= g_controlled_primary.load() == kKing;
        nonprimary_bytes &= roster_bytes() == roster0;
    }
    check("P3", "rehandshake of a non-primary keeps the primary", nonprimary_ok);
    check("P3", "rehandshake of a non-primary keeps the roster bytes",
          nonprimary_bytes, "roster=" + roster_bytes());

    // Rehandshake of the PRIMARY session: the handoff is by claim order — the
    // next-earliest claimant — and is the same answer every time, not whoever
    // happens to hash first.
    {
        bool ok = g_session_registry.destroy(ctrl[0]->session_id);
        ok &= g_controlled_primary.load() == kEarls[0];
        check("P3", "primary session drop hands off by claim order", ok,
              "primary=" + std::to_string(g_controlled_primary.load()));

        ctrl[0] = g_session_registry.create("agent-king-again", Session::Mode::CONTROL);
        bool ok2 = g_session_registry.claim_character(ctrl[0], kKing, nullptr);
        // The king is now the NEWEST claimant, so claim order alone leaves the
        // primary where it is.  (The manifest is what moves it back — P4.)
        ok2 &= g_controlled_primary.load() == kEarls[0];
        check("P3", "reconnecting king does not reclaim the primary by luck", ok2,
              "primary=" + std::to_string(g_controlled_primary.load()));
        check("P3", "roster bytes restored after the primary rehandshake",
              roster_bytes() == roster0, "roster=" + roster_bytes());
    }

    std::string detail;
    check("P3", "no divergence across the whole reorder sweep",
          !g_session_registry.ownership_divergence(&detail), detail);
}

// ---------------------------------------------------------------------------
// P4 — the episode manifest designates the global primary
// ---------------------------------------------------------------------------
static void phase_manifest(std::vector<Session*>& ctrl) {
    g_session_registry.set_manifest_primary(kKing);
    check("P4", "manifest designation overrides claim order",
          g_session_registry.manifest_primary() == kKing &&
              g_controlled_primary.load() == kKing,
          "primary=" + std::to_string(g_controlled_primary.load()));

    bool held = true;
    for (int i = 0; i < 32; ++i) {
        Session* f = g_session_registry.create("churn", Session::Mode::OBSERVE);
        g_session_registry.destroy(f->session_id);
        held &= g_controlled_primary.load() == kKing;
    }
    held &= g_session_registry.destroy(ctrl[2]->session_id);
    held &= g_controlled_primary.load() == kKing;
    ctrl[2] = g_session_registry.create("earl-1-again", Session::Mode::CONTROL);
    held &= g_session_registry.claim_character(ctrl[2], kEarls[1], nullptr);
    held &= g_controlled_primary.load() == kKing;
    check("P4", "designation survives churn and other rehandshakes", held);

    // Succession, in the order check_controlled_handoff actually does it: claim
    // the heir FIRST, then release the corpse — so the session never holds
    // nothing and never surrenders its claim-order slot.
    const uint32_t kHeir = 6100;
    bool succ = g_session_registry.claim_character(ctrl[0], kHeir, nullptr);
    succ &= g_session_registry.release_character(ctrl[0], kKing);
    succ &= g_session_registry.owner_of(kKing) == nullptr;
    succ &= g_controlled_primary.load() == kHeir;
    check("P4", "designation follows the SESSION through a succession", succ,
          "primary=" + std::to_string(g_controlled_primary.load()));
    check("P4", "designation is bound to the surviving session",
          g_session_registry.manifest_primary_session() == ctrl[0]->session_id);

    g_session_registry.set_manifest_primary(0);
    check("P4", "clearing the designation falls back to claim order",
          g_controlled_primary.load() == kEarls[0],
          "primary=" + std::to_string(g_controlled_primary.load()));
}

// ---------------------------------------------------------------------------
// P5 — the watchlist cap refuses, it does not silently skip
// ---------------------------------------------------------------------------
static void phase_cap() {
    check("P5", "six claims held before filling", g_controlled_chars.size() == 6,
          "roster=" + roster_bytes());

    Session* filler = g_session_registry.create("filler-control", Session::Mode::CONTROL);
    bool filled = true;
    for (uint32_t c = 7001; c <= 7004; ++c)
        filled &= g_session_registry.claim_character(filler, c, nullptr);
    check("P5", "watchlist fills to the cap",
          filled && g_controlled_chars.size() == MAX_CONTROLLED_WATCHLIST,
          "roster=" + roster_bytes());

    {
        std::string why;
        bool refused = !g_session_registry.claim_character(filler, 7005, &why);
        check("P5", "the overflowing claim is REFUSED watchlist_full",
              refused && why == "watchlist_full", "reason=" + why);
        check("P5", "the refused character is not owned",
              g_session_registry.owner_of(7005) == nullptr);
    }

    {
        std::string detail;
        bool agree = !g_session_registry.ownership_divergence(&detail) &&
                     g_session_registry.controlled_claim_count() == g_controlled_chars.size();
        check("P5", "registry and watchlist agree at the cap", agree, detail);
        uint32_t p = g_controlled_primary.load();
        check("P5", "the global primary is inside the watchlist",
              p != 0 && std::find(g_controlled_chars.begin(), g_controlled_chars.end(),
                                  p) != g_controlled_chars.end(),
              "primary=" + std::to_string(p));
    }

    bool freed = g_session_registry.release_character(filler, 7004);
    freed &= g_session_registry.claim_character(filler, 7005, nullptr);
    freed &= g_session_registry.owner_of(7005) == filler;
    check("P5", "releasing frees a slot and the refused claim then works", freed);

    std::string detail;
    check("P5", "no divergence after the cap sweep",
          !g_session_registry.ownership_divergence(&detail), detail);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    bool want_json = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) { want_json = true; g_quiet = true; }
        else if (strcmp(argv[i], "--quiet") == 0) g_quiet = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: acceptance_n6_ownership [--json] [--quiet]\n");
            return 0;
        }
    }

    if (!g_quiet)
        printf("ck-6kvq.11 — N=6 ownership acceptance (HOST lane)\n\n");

    std::vector<Session*> ctrl;
    Session* obs = nullptr;
    phase_claim(ctrl, obs);
    phase_cross(ctrl, obs);
    phase_registry(ctrl, obs);
    phase_determinism(ctrl);
    phase_manifest(ctrl);
    phase_cap();

    // The determinism artefacts the driver compares across processes.  They are
    // taken AFTER the full sweep on purpose: a primary that is stable only
    // before any churn is not the property we are claiming.
    const uint32_t final_primary = g_controlled_primary.load();
    const std::string final_roster = roster_bytes();

    // Tear down, and check the teardown too — a registry that leaks a mirror on
    // the last release leaves the next episode's first claim non-deterministic.
    reset_registry();
    check("P5", "teardown clears every mirror",
          g_controlled_primary.load() == 0 && g_controlled_chars.empty() &&
              !g_agent_mode.load());

    size_t failed = 0;
    for (const Check& c : g_checks) if (!c.ok) ++failed;

    if (want_json) {
        printf("{\"acceptance\":\"n6_ownership\",\"lane\":\"host\",");
        printf("\"total\":%zu,\"failed\":%zu,", g_checks.size(), failed);
        printf("\"final_primary\":%u,", final_primary);
        printf("\"final_roster\":\"%s\",", json_escape(final_roster).c_str());
        printf("\"max_watchlist\":%zu,", MAX_CONTROLLED_WATCHLIST);
        printf("\"checks\":[");
        for (size_t i = 0; i < g_checks.size(); ++i) {
            const Check& c = g_checks[i];
            printf("%s{\"phase\":\"%s\",\"name\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
                   i ? "," : "", c.phase.c_str(), json_escape(c.name).c_str(),
                   c.ok ? "true" : "false", json_escape(c.detail).c_str());
        }
        printf("]}\n");
    } else {
        printf("\n%zu checks, %zu failed — final primary %u, roster [%s]\n",
               g_checks.size(), failed, final_primary, final_roster.c_str());
        printf("acceptance_n6_ownership: %s\n", failed ? "FAIL" : "PASS");
    }
    return failed ? 1 : 0;
}
