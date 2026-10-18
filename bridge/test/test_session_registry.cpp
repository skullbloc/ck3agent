// test/test_session_registry.cpp — unit tests for the M1 session registry.
//
// This is a standalone test: it compiles session.cpp in isolation and provides
// the few global symbols that session.cpp's rebuild_legacy_mirrors references.

#include "bridge.h"     // ck-6kvq.5: MemRange, for the stub globals below
#include "session.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
#include <shared_mutex>
#include <iostream>

// Stub globals referenced by bridge.h / session.cpp when compiled standalone.
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

int main() {
    Session* s1 = g_session_registry.create("test-agent-1", Session::Mode::CONTROL);
    assert(s1); (void)s1;
    Session* s2 = g_session_registry.create("test-agent-2", Session::Mode::OBSERVE);
    assert(s2); (void)s2;
    Session* s3 = g_session_registry.create("test-agent-3", Session::Mode::CONTROL);
    assert(s3); (void)s3;

    // Basic claim.
    std::string reason;
    assert(g_session_registry.claim_character(s1, 1001, &reason));
    assert(s1->primary_character_id == 1001);
    assert(g_controlled_primary.load() == 1001);

    // Cross-session claim must fail.
    assert(!g_session_registry.claim_character(s3, 1001, &reason));
    assert(reason == "owned_by_other");

    // Idempotent re-claim by owner succeeds.
    assert(g_session_registry.claim_character(s1, 1001, &reason));

    // s3 can claim a different character.
    assert(g_session_registry.claim_character(s3, 2002, &reason));
    assert(g_controlled_primary.load() == 1001);  // first control session keeps global primary

    // Owner lookup.
    assert(g_session_registry.owner_of(1001) == s1);
    assert(g_session_registry.owner_of(2002) == s3);
    assert(g_session_registry.owner_of(9999) == nullptr);

    // Release by non-owner fails.
    assert(!g_session_registry.release_character(s3, 1001));

    // Release by owner succeeds and updates global mirrors.
    assert(g_session_registry.release_character(s1, 1001));
    assert(g_session_registry.owner_of(1001) == nullptr);
    assert(g_controlled_primary.load() == 2002);  // remaining control session becomes primary

    // Destroying a session releases its claims.
    assert(g_session_registry.destroy(s3->session_id));
    assert(g_session_registry.owner_of(2002) == nullptr);
    assert(g_controlled_primary.load() == 0);
    assert(!g_agent_mode.load());

    // Observe-mode session can exist but holds no control state.
    assert(g_session_registry.find(s2->session_id) == s2);
    assert(g_controlled_primary.load() == 0);

    // JSON snapshot sanity check.
    Session* s4 = g_session_registry.create("json-test", Session::Mode::CONTROL);
    g_session_registry.claim_character(s4, 3003, nullptr);
    std::string json = g_session_registry.sessions_json();
    assert(json.find("\"session_id\":\"") != std::string::npos);
    assert(json.find("3003") != std::string::npos);
    assert(json.find("\"mode\":\"control\"") != std::string::npos);

    // Context assertion: without a ClientContext, dispatch context is refused.
    {
        std::string reason;
        assert(!require_dispatch_context("query_character", &reason));
        assert(reason == "no_client_context");
    }

    // Context assertion: with a ClientContext installed, dispatch context passes.
    {
        ClientContext ctx;
        ctx.session = s4;
        ctx.action_type = "query_character";
        ClientContextGuard guard(&ctx);
        std::string reason;
        assert(require_dispatch_context("query_character", &reason));
        assert(reason.empty());
    }

    // Cross-session actor refusal via validate_actor_ownership.
    {
        Session* s5 = g_session_registry.create("owner", Session::Mode::CONTROL);
        Session* s6 = g_session_registry.create("intruder", Session::Mode::CONTROL);
        assert(g_session_registry.claim_character(s5, 4004, nullptr));
        assert(g_session_registry.claim_character(s6, 5005, nullptr));

        ClientContext ctx;
        ctx.session = s6;  // acting as the intruder
        ctx.action_type = "send_interaction";
        ClientContextGuard guard(&ctx);

        std::string reason;
        assert(!validate_actor_ownership(4004, "send_interaction", &reason));
        assert(reason == "owned_by_other");

        // Owner acting on their own character is allowed.
        ctx.session = s5;
        reason.clear();
        assert(validate_actor_ownership(4004, "send_interaction", &reason));

        // Read-only actions bypass ownership checks.
        ctx.session = s6;
        reason.clear();
        assert(validate_actor_ownership(4004, "query_character", &reason));

        g_session_registry.destroy(s5->session_id);
        g_session_registry.destroy(s6->session_id);
    }

    // Observe-mode sessions cannot mutate.
    {
        Session* s7 = g_session_registry.create("watcher", Session::Mode::OBSERVE);
        ClientContext ctx;
        ctx.session = s7;
        ctx.action_type = "send_interaction";
        ClientContextGuard guard(&ctx);
        std::string reason;
        assert(!validate_actor_ownership(1001, "send_interaction", &reason));
        assert(reason == "observe_mode");
        g_session_registry.destroy(s7->session_id);
    }

    // ========================================================================
    // ck-6kvq.11 — the global primary must be DETERMINISTIC at N>1.
    //
    // Defect (design §1.4 defect 2): rebuild_legacy_mirrors() used to take "the
    // first control session with a nonzero primary" while walking sessions_, an
    // unordered_map — i.e. HASH order decided the global primary, and
    // g_controlled_primary is load-bearing for the dead-ruler handoff, monthly
    // telemetry, personality locks, and every handler that defaults an actor.
    //
    // These cases deliberately construct a session map whose iteration order
    // differs from claim order, and assert that it did, so the test cannot pass
    // by incidental hash luck.
    // ========================================================================
    {
        // Start from a clean registry.
        for (const std::string& sid : g_session_registry.ids())
            g_session_registry.destroy(sid);
        assert(g_controlled_primary.load() == 0);
        g_session_registry.set_manifest_primary(0);

        // N=6 control sessions plus one observer, claiming in a known order.
        // Characters are named after the shape of the 1066 episode: the first
        // claim is the English king, the rest are earls.
        const uint32_t kKing = 6001;
        const uint32_t kEarls[5] = {6002, 6003, 6004, 6005, 6006};

        std::vector<Session*> ctrl;
        ctrl.push_back(g_session_registry.create("agent-king", Session::Mode::CONTROL));
        for (int i = 0; i < 5; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "agent-earl-%d", i);
            ctrl.push_back(g_session_registry.create(name, Session::Mode::CONTROL));
        }
        Session* obs = g_session_registry.create("observer", Session::Mode::OBSERVE);

        assert(g_session_registry.claim_character(ctrl[0], kKing, nullptr));
        for (int i = 0; i < 5; ++i)
            assert(g_session_registry.claim_character(ctrl[i + 1], kEarls[i], nullptr));

        // An observe session must not be able to take a claim at all — if it
        // could, the ownership table and the control watchlist would differ.
        {
            std::string why;
            assert(!g_session_registry.claim_character(obs, 6999, &why));
            assert(why == "observe_mode");
        }

        auto control_iteration_order = [&]() {
            std::vector<uint32_t> order;
            g_session_registry.for_each([&](const Session& s) {
                if (s.is_control() && s.primary_character_id != 0)
                    order.push_back(s.primary_character_id);
            });
            return order;
        };

        // --- Forced reordering of the session map. ---
        // Creating and destroying throwaway sessions grows the bucket count and
        // relocates every live session — which is what a busy episode does
        // anyway (every rehandshake is an erase plus an insert). We record the
        // control-session iteration order after each round and require:
        //   (a) at least two DISTINCT orders were observed, so the map really
        //       was reordered and this is not an incidental-hash-order pass;
        //   (b) at least one observed order does NOT start with the first
        //       claimant, so the old "first session in sessions_ wins" rule
        //       would demonstrably have published the wrong character;
        //   (c) g_controlled_primary was the first claimant at every single
        //       observation.
        std::vector<std::vector<uint32_t>> observed;
        observed.push_back(control_iteration_order());
        assert(g_controlled_primary.load() == kKing);
        assert(g_controlled_chars.size() == 6);

        for (int round = 0; round < 24; ++round) {
            std::vector<Session*> filler;
            for (int i = 0; i < 8; ++i) {
                char name[32];
                snprintf(name, sizeof(name), "filler-%d-%d", round, i);
                filler.push_back(g_session_registry.create(name, Session::Mode::OBSERVE));
            }
            for (Session* f : filler) g_session_registry.destroy(f->session_id);

            observed.push_back(control_iteration_order());
            // (c) — the primary never moved, at any point in the churn.
            assert(g_controlled_primary.load() == kKing);
        }

        bool distinct_orders = false;
        bool wrong_front_seen = false;
        for (const auto& o : observed) {
            assert(o.size() == 6);
            if (o != observed.front()) distinct_orders = true;
            if (o.front() != kKing) wrong_front_seen = true;
        }
        assert(distinct_orders &&
               "the session map never reordered — the reorder hazard was not "
               "constructed, so this test would pass on incidental hash order");
        assert(wrong_front_seen &&
               "no observed map order put a non-first-claimant first — the OLD "
               "hash-order rule was never actually exercised");
        std::cout << "  forced reorder: " << observed.size()
                  << " map orders observed, first-claimant-first in only some of "
                     "them; global primary pinned at "
                  << g_controlled_primary.load() << " throughout\n";
        // Claim order decides, not hash order.
        assert(g_controlled_primary.load() == kKing);
        {
            std::string detail;
            assert(!g_session_registry.ownership_divergence(&detail));
        }

        // --- Rehandshake of a NON-primary session must not move the primary. ---
        for (int round = 0; round < 3; ++round) {
            std::string dead = ctrl[3]->session_id;
            assert(g_session_registry.destroy(dead));
            assert(g_controlled_primary.load() == kKing);
            ctrl[3] = g_session_registry.create("agent-earl-2-again",
                                                Session::Mode::CONTROL);
            assert(g_session_registry.claim_character(ctrl[3], kEarls[2], nullptr));
            assert(g_controlled_primary.load() == kKing);
        }

        // --- Rehandshake of the PRIMARY session hands off by claim order. ---
        // The king's session drops; the next-earliest claimant (earl 0) takes
        // the global primary. Deterministic, and not whoever hashes first.
        {
            std::string king_sid = ctrl[0]->session_id;
            assert(g_session_registry.destroy(king_sid));
            assert(g_controlled_primary.load() == kEarls[0]);

            // The king reconnects: he is now the newest claimant, so claim order
            // alone leaves the primary where it is.
            ctrl[0] = g_session_registry.create("agent-king-again",
                                                Session::Mode::CONTROL);
            assert(g_session_registry.claim_character(ctrl[0], kKing, nullptr));
            assert(g_controlled_primary.load() == kEarls[0]);
        }

        // --- The episode manifest designation overrides claim order. ---
        // This is why the manifest exists: the 1066 episode wants the English
        // king as the global primary regardless of who connected first.
        g_session_registry.set_manifest_primary(kKing);
        assert(g_session_registry.manifest_primary() == kKing);
        assert(g_controlled_primary.load() == kKing);

        // ...and it stays put across rehashes and rehandshakes of other sessions.
        for (int i = 0; i < 32; ++i) {
            Session* f = g_session_registry.create("churn", Session::Mode::OBSERVE);
            g_session_registry.destroy(f->session_id);
        }
        assert(g_controlled_primary.load() == kKing);
        assert(g_session_registry.destroy(ctrl[2]->session_id));
        assert(g_controlled_primary.load() == kKing);
        ctrl[2] = g_session_registry.create("earl-1-again", Session::Mode::CONTROL);
        assert(g_session_registry.claim_character(ctrl[2], kEarls[1], nullptr));
        assert(g_controlled_primary.load() == kKing);

        // --- Succession: the designation follows the SESSION, not the corpse. ---
        // check_controlled_handoff claims the heir, then releases the dead king.
        const uint32_t kHeir = 6100;
        assert(g_session_registry.claim_character(ctrl[0], kHeir, nullptr));
        assert(g_session_registry.release_character(ctrl[0], kKing));
        assert(g_session_registry.owner_of(kKing) == nullptr);
        assert(g_controlled_primary.load() == kHeir);
        assert(g_session_registry.manifest_primary_session() == ctrl[0]->session_id);

        // Clearing the designation falls back to claim order (earl 0 claimed
        // before the reconnected king's session did).
        g_session_registry.set_manifest_primary(0);
        assert(g_controlled_primary.load() == kEarls[0]);

        // No divergence anywhere in the above.
        {
            std::string detail;
            assert(!g_session_registry.ownership_divergence(&detail));
        }

        // ====================================================================
        // ck-6kvq.11 — watchlist/ownership divergence is an ERROR.
        //
        // Defect (design §1.5): MAX_WATCHLIST bounded g_controlled_chars and,
        // when full, SKIPPED the add while the registry claim SUCCEEDED, so the
        // two structures silently disagreed. The registry now enforces the cap
        // itself, so the derived watchlist is exactly the ownership table.
        // ====================================================================
        assert(g_session_registry.controlled_claim_count() == 6);
        assert(g_controlled_chars.size() == 6);

        // Fill to the cap (6 held + 4 more = 10).
        Session* filler_sess = g_session_registry.create("filler-control",
                                                         Session::Mode::CONTROL);
        for (uint32_t c = 7001; c <= 7004; ++c)
            assert(g_session_registry.claim_character(filler_sess, c, nullptr));
        assert(g_session_registry.controlled_claim_count() == MAX_CONTROLLED_WATCHLIST);
        assert(g_controlled_chars.size() == MAX_CONTROLLED_WATCHLIST);

        // The claim that would overflow is REFUSED with a typed reason — it does
        // not succeed-and-skip.
        {
            std::string why;
            assert(!g_session_registry.claim_character(filler_sess, 7005, &why));
            assert(why == "watchlist_full");
            assert(g_session_registry.owner_of(7005) == nullptr);
        }

        // Registry and watchlist still agree exactly, and the primary is still
        // in the watchlist.
        {
            std::string detail;
            assert(!g_session_registry.ownership_divergence(&detail));
            assert(g_session_registry.controlled_claim_count() ==
                   g_controlled_chars.size());
            uint32_t p = g_controlled_primary.load();
            assert(p != 0);
            assert(std::find(g_controlled_chars.begin(), g_controlled_chars.end(), p)
                   != g_controlled_chars.end());
        }

        // Releasing frees a slot, and the previously refused claim now works.
        assert(g_session_registry.release_character(filler_sess, 7004));
        assert(g_session_registry.claim_character(filler_sess, 7005, nullptr));
        assert(g_session_registry.owner_of(7005) == filler_sess);
        {
            std::string detail;
            assert(!g_session_registry.ownership_divergence(&detail));
        }

        // Injected divergence is DETECTED rather than ignored: corrupt the
        // mirror behind the registry's back and make it rebuild.
        {
            {
                std::lock_guard<std::mutex> lk(g_controlled_mutex);
                g_controlled_chars.clear();
            }
            // A no-op rebuild (idempotent re-claim) recomputes from ownership_,
            // so the roster repairs itself and no divergence is latched.
            assert(g_session_registry.claim_character(filler_sess, 7005, nullptr));
            std::string detail;
            assert(!g_session_registry.ownership_divergence(&detail));
            assert(g_controlled_chars.size() == MAX_CONTROLLED_WATCHLIST);
        }

        // Tear down.
        for (const std::string& sid : g_session_registry.ids())
            g_session_registry.destroy(sid);
        assert(g_controlled_primary.load() == 0);
        assert(g_controlled_chars.empty());
        assert(!g_agent_mode.load());
    }

    // ========================================================================
    // ck-6kvq.5 — DETACH / RESUME.  The bead's acceptance criteria, in the file
    // it names.  The FULL treatment (the identity rule, the unavailability
    // covariate, the abort ladder's detach report, and a negative control that
    // must fail) is bridge/test/acceptance_session_resume.cpp; this is the
    // unit-test floor, so the five claims below are checked on every build.
    // ========================================================================
    {
        for (const std::string& sid : g_session_registry.ids())
            g_session_registry.destroy(sid);

        Session* r1 = g_session_registry.create("resume-1", Session::Mode::CONTROL);
        const std::string rsid  = r1->session_id;
        const std::string token = r1->resume_token;
        assert(!token.empty());                       // SERVER-minted
        assert(g_session_registry.claim_character(r1, 8001, nullptr));
        assert(g_session_registry.claim_character(r1, 8002, nullptr));
        assert(r1->set_primary(8002));
        g_session_registry.attach_to_slot(r1, /*slot*/2, /*gen*/4, /*now*/1000);
        const uint64_t seq = r1->claim_seq;

        // 1. DESTROY-ON-DROP NO LONGER OCCURS, and 2. the claims and the
        //    ownership entry are retained.
        assert(g_session_registry.detach_from_slot(rsid, 2, 4, 2000));
        assert(g_session_registry.find(rsid) == r1);
        assert(r1->state == Session::State::DETACHED);
        assert(r1->claimed_character_ids.size() == 2);
        assert(g_session_registry.owner_of(8001) == r1);
        assert(r1->claim_seq == seq);                 // global primary cannot move

        // 3. RESUME BY TOKEN rebinds and restores the primary.
        auto ok = g_session_registry.resume_by_token(token, WorldIdentity{},
                                                     /*epoch*/0, 6, 1, 3000);
        assert(ok.ok && ok.session == r1);
        assert(r1->state == Session::State::ATTACHED);
        assert(r1->primary_character_id == 8002);
        assert(r1->claimed_character_ids.size() == 2);

        // 4. A STALE SLOT GENERATION CANNOT CLOSE A LIVE SESSION.
        assert(!g_session_registry.detach_from_slot(rsid, 2, 4, 4000));
        assert(r1->state == Session::State::ATTACHED);
        assert(g_session_registry.owner_of(8001) == r1);

        // 5. max_detach_s EXPIRY IS REPORTED, NOT SILENTLY RELEASED.
        assert(g_session_registry.detach_from_slot(rsid, 6, 1, 5000));
        assert(g_session_registry.sweep_detached(5000 + 100, 1000).empty());
        auto lost = g_session_registry.sweep_detached(5000 + 1000, 1000);
        assert(lost.size() == 1 && lost[0] == rsid);
        assert(r1->state == Session::State::LOST);
        assert(g_session_registry.sweep_detached(5000 + 9000, 1000).empty());  // once
        assert(r1->claimed_character_ids.size() == 2);      // RELEASES NOTHING
        assert(g_session_registry.owner_of(8001) == r1);

        for (const std::string& sid : g_session_registry.ids())
            g_session_registry.destroy(sid);
        assert(g_controlled_primary.load() == 0);
    }

    std::cout << "test_session_registry: OK (all checks passed)\n";
    return 0;
}
