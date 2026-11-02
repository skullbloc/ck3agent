// test_intent_lookup.cpp — L13 FF2 (witness R3): a reader never finds an intent
// in NEITHER the live queue nor the completed ring.
//
// Links the REAL IntentQueue (src/intent_queue.cpp). Before FF2,
// query_dispatch_result asked the completed ring, then the live queue for an
// UNCONSUMED intent, each under its own lock, and the drain moved a consumed
// intent to the ring in a separate step after tick_maintenance had erased it:
// an intent the drain had consumed but not yet stored answered not_found,
// which the server reads as a refusal — for a write that ran.
//
// Rows:
//   1  pending -> claimed (drain executing) -> finished -> maintained, step by
//      step: Pending, Running, Completed (with the drain's verdict);
//   2  a reader hammering lookup_dispatch while another thread claims, finishes
//      and maintains 2000 intents never sees NotFound for an id it enqueued;
//   3  control: an id never enqueued is NotFound (the row-2 assertion is not
//      vacuous);
//   4  L13 FF3 (witness X3): one thread enqueues 1000 intents while TWO
//      drainers claim each, write console_output BEFORE consumed (on their
//      copy) and finish it, while a reader runs lookup_dispatch and snapshot:
//      every intent is executed exactly once (a claim is atomic: no double
//      send), every verdict carries the drain's console_output, and a started
//      intent is never answered Pending (FF4, witness m7). Built with -fsanitize=thread (and
//      separately address): a reported race or use-after-free is a red
//      (ctest rows test_intent_lookup_thread / _address; SKIPPED where the
//      toolchain has no sanitizer runtime; evidence in
//      docs/cleanup-2026.11/evidence/l13/sanitizers.md);
//   5  retire on a claimed intent returns false (the drain's verdict stands);
//   6  (m10) a crash rejects the in-flight intent by seq, not a duplicate
//      request_id queued ahead of it;
//   7  (m11) a crash puts g_current_client_context back to the drain's entry
//      value;
//   8  L14 (surface-eval finding 3): a slot READ (the interaction check and the
//      scheme check, shaped as their handlers build them) is found by its asker
//      and leaves no row in the completed ring inspect(pending) lists; a WRITE
//      leaves exactly one; 200 reads do not push a write's verdict out;
//   9  L14: a drained call's record carries the actor the call names under
//      params (the wire shape), not 0 (drain_write_verdict.h drain_actor_id).
//  10  L14 FB m-6: pending_writes (query_intents' pending[] and pending_count)
//      lists a queued write and none of the queued checks.
//  11  L14 FB2 N-10: pending_count() counts the same writes (not the checks).
// Exit 0 pass, 1 fail.
#include "bridge.h"
#include "intent_queue.h"
#include "drain_write_verdict.h"
#include "session.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace bridge {
FILE* g_logfile = nullptr;
std::atomic<uint64_t> g_tick_id{0};
thread_local ClientContext* g_current_client_context = nullptr;
}  // namespace bridge

using namespace bridge;

namespace {
int g_failures = 0;
void row(const char* name, bool ok, const std::string& detail) {
    std::printf("%-58s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}
const char* nm(DispatchLookup d) {
    switch (d) {
        case DispatchLookup::Completed: return "completed";
        case DispatchLookup::Running:   return "running";
        case DispatchLookup::Pending:   return "pending";
        case DispatchLookup::NotFound:  return "not_found";
    }
    return "?";
}
Intent make(const std::string& rid, uint32_t actor) {
    Intent i{};
    i.type = IntentType::INTERACTION;
    i.actor_id = actor;
    i.request_id = rid;
    i.session_id = "s";
    i.ttl_ticks = 1000000;
    return i;
}
}  // namespace

int main() {
    // Row 1: the three states, in the order the drain produces them.
    {
        IntentQueue q;
        q.enqueue(make("si_row1", 7));
        Intent out{};
        const DispatchLookup a = q.lookup_dispatch("si_row1", &out);
        const std::vector<Intent> snap = q.snapshot();
        Intent work{};
        const bool claimed = !snap.empty() && q.claim(snap[0].seq, &work);   // the drain
        const DispatchLookup b = q.lookup_dispatch("si_row1", &out);
        work.executed = true; work.consumed = true;
        if (claimed) q.finish(work);
        std::vector<Intent> done;
        q.tick_maintenance(1, done);
        Intent fin{};
        const DispatchLookup c = q.lookup_dispatch("si_row1", &fin);
        row("pending_then_running_then_completed",
            a == DispatchLookup::Pending && b == DispatchLookup::Running &&
                c == DispatchLookup::Completed && fin.executed && !fin.rejected,
            std::string(nm(a)) + " -> " + nm(b) + " -> " + nm(c));
    }
    // Row 2: a concurrent reader across consume + maintenance.
    {
        IntentQueue q;
        constexpr int N = 2000;
        std::atomic<int> enqueued{-1};
        std::atomic<bool> stop{false};
        std::atomic<int> not_found{0}, reads{0}, evicted{0};
        std::thread reader([&] {
            while (!stop.load()) {
                const int hi = enqueued.load();
                for (int k = (hi > 40 ? hi - 40 : 0); k <= hi; ++k) {
                    Intent o{};
                    const bool nf =
                        q.lookup_dispatch("si_" + std::to_string(k), &o) == DispatchLookup::NotFound;
                    // Id k leaves the ring only once id k + MAX_COMPLETED is
                    // stored, which follows its enqueue: an `enqueued` read AFTER
                    // the lookup that is still short of it proves k was in the
                    // ring (or the queue) when looked up. A slower reader can see
                    // a real eviction; that one is not counted.
                    const int after = enqueued.load();
                    if (nf && after - k < static_cast<int>(IntentQueue::MAX_COMPLETED)) {
                        not_found.fetch_add(1);
                    } else if (nf) {
                        evicted.fetch_add(1);
                    }
                    reads.fetch_add(1);
                }
            }
        });
        for (int k = 0; k < N; ++k) {
            q.enqueue(make("si_" + std::to_string(k), 100 + static_cast<uint32_t>(k)));
            enqueued.store(k);
            for (const Intent& p : q.snapshot()) {
                Intent w{};
                if (!q.claim(p.seq, &w)) continue;
                w.executed = true; w.consumed = true;
                q.finish(w);
            }
            std::vector<Intent> done;
            q.tick_maintenance(static_cast<uint64_t>(k), done);
        }
        stop = true;
        reader.join();
        row("a_reader_never_sees_not_found_across_consume_and_store",
            not_found.load() == 0 && reads.load() > 0,
            std::to_string(reads.load()) + " reads, " + std::to_string(not_found.load()) +
                " not_found (" + std::to_string(evicted.load()) + " evicted past the ring, not counted)");
    }
    // Row 3: the control.
    {
        IntentQueue q;
        q.enqueue(make("si_known", 9));
        Intent o{};
        row("control_an_unknown_id_is_not_found",
            q.lookup_dispatch("si_never", &o) == DispatchLookup::NotFound, "");
    }
    // Row 4: enqueue x drain (claim, write, finish, maintain) x readers. Two
    // drainers claim concurrently (production has one; the property under test
    // is that a claim is atomic, so two must never both win one intent), and
    // the reader checks that an intent a drainer has started is never answered
    // Pending (it is Running until finished).
    {
        IntentQueue q;
        constexpr int N = 1000;
        std::atomic<bool> producing{true}, stop{false};
        std::vector<std::atomic<int>> executed(N), started(N);
        for (int k = 0; k < N; ++k) { executed[k] = 0; started[k] = 0; }
        std::atomic<int> reads{0}, pending_while_running{0};
        std::thread producer([&] {
            for (int k = 0; k < N; ++k)
                q.enqueue(make("x3_" + std::to_string(k), 5000 + static_cast<uint32_t>(k)));
            producing = false;
        });
        std::thread reader([&] {
            while (!stop.load()) {
                for (int k = 0; k < N; k += 7) {
                    const bool was_started = started[k].load(std::memory_order_acquire) != 0;
                    Intent o{};
                    if (q.lookup_dispatch("x3_" + std::to_string(k), &o) == DispatchLookup::Pending &&
                        was_started)
                        pending_while_running.fetch_add(1);
                    reads.fetch_add(1);
                }
                (void)q.snapshot();
                (void)q.completed_snapshot(16);
            }
        });
        // Maintenance runs once, after both drainers stop: tick_maintenance
        // reads an intent still in flight as one a crash jumped out of (in the
        // bridge it runs on the drain's own thread, after the drain), so it
        // must not run while the other drainer holds a claim.
        std::vector<Intent> verdicts;
        auto drainer = [&]() {
            while (producing.load() || q.pending_count() > 0) {
                for (const Intent& p : q.snapshot()) {
                    Intent w{};
                    if (!q.claim(p.seq, &w)) continue;
                    const int k = std::stoi(w.request_id.substr(3));
                    started[k].store(1, std::memory_order_release);
                    executed[k].fetch_add(1);
                    w.console_output = std::string(64, 'a' + (k % 26)) + w.request_id;  // before consumed
                    w.executed = true;
                    w.consumed = true;
                    q.finish(w);
                }
            }
        };
        std::thread d1(drainer);
        drainer();
        d1.join();
        q.tick_maintenance(1, verdicts);
        stop = true;
        producer.join();
        reader.join();
        int once = 0, twice = 0, bad_output = 0;
        for (int k = 0; k < N; ++k) { once += executed[k] == 1; twice += executed[k] > 1; }
        for (const Intent& v : verdicts) {
            const std::string want = std::string(64, 'a' + (std::stoi(v.request_id.substr(3)) % 26)) + v.request_id;
            if (!v.executed || v.console_output != want) ++bad_output;
        }
        row("x3_every_intent_executed_once_under_concurrent_enqueue_and_reads",
            once == N && twice == 0 && static_cast<int>(verdicts.size()) == N && bad_output == 0 &&
                pending_while_running.load() == 0,
            std::to_string(once) + " once, " + std::to_string(twice) + " more than once, " +
                std::to_string(verdicts.size()) + " verdicts, " + std::to_string(bad_output) +
                " wrong verdicts, " + std::to_string(pending_while_running.load()) +
                " answered pending while running, " + std::to_string(reads.load()) + " reads");
    }
    // Row 5: a claimed intent cannot be retired.
    {
        IntentQueue q;
        q.enqueue(make("si_row5", 11));
        Intent w{};
        const bool claimed = q.claim(q.snapshot().at(0).seq, &w);
        const bool retired = q.retire_pending_by_request_id("si_row5", "intent_timeout_cancelled_not_executed");
        w.executed = true; w.consumed = true;
        q.finish(w);
        std::vector<Intent> done;
        q.tick_maintenance(1, done);
        row("x3_a_claimed_intent_is_not_retired",
            claimed && !retired && done.size() == 1 && done[0].executed && !done[0].rejected,
            std::string("claimed=") + (claimed ? "1" : "0") + " retired=" + (retired ? "1" : "0"));
    }
    // Row 6 (witness m10): a crash rejects the intent IN FLIGHT, by seq, never
    // a duplicate request_id queued ahead of it.
    {
        IntentQueue q;
        q.enqueue(make("dup_rid", 21));        // ahead, never claimed
        q.enqueue(make("dup_rid", 22));        // the one the drain runs
        const std::vector<Intent> snap = q.snapshot();
        Intent w{};
        const bool claimed = snap.size() == 2 && q.claim(snap[1].seq, &w);
        g_inflight_intent_seq = w.seq;
        g_current_client_context = nullptr;
        const uint64_t marked = recover_drain_crash(q, nullptr, "game_thread_crash_during_drain");
        const std::vector<Intent> pending = q.snapshot();
        std::vector<Intent> done;
        q.tick_maintenance(1, done);
        row("m10_a_crash_rejects_the_in_flight_intent_not_a_duplicate_ahead",
            claimed && marked == w.seq && pending.size() == 1 && pending[0].actor_id == 21 &&
                done.size() == 1 && done[0].actor_id == 22 && done[0].rejected &&
                g_inflight_intent_seq == 0,
            "marked seq " + std::to_string(marked) + ", still pending: " +
                std::to_string(pending.size()) + (pending.empty() ? "" : " (actor " +
                std::to_string(pending[0].actor_id) + ")"));
    }
    // Row 7 (witness m11): after a crash the client context is the drain's
    // entry value, not the dead per-intent guard's object.
    {
        IntentQueue q;
        ClientContext entry{}, dead{};
        g_current_client_context = &dead;       // what the skipped guard left behind
        g_inflight_intent_seq = 0;
        const uint64_t marked = recover_drain_crash(q, &entry, "x");
        row("m11_a_crash_restores_the_client_context",
            g_current_client_context == &entry && marked == 0, "");
        g_current_client_context = nullptr;
    }
    // Row 8 (L14, finding 3): reads are delivered, never listed.
    {
        IntentQueue q;
        auto drain_one = [&](Intent in) {
            q.enqueue(std::move(in));
            const std::vector<Intent> snap = q.snapshot();
            Intent work{};
            if (snap.empty() || !q.claim(snap.back().seq, &work)) return false;
            work.executed = true;
            work.console_output = "{\"valid_count\":0}";
            q.finish(work);
            std::vector<Intent> done;
            q.tick_maintenance(1, done);
            return true;
        };
        Intent ci = make("ci_9238_4_1", 9238);                  // query_interaction_check
        ci.console_command = "check:1,2";
        Intent sc = make("schc_9238_4_37106", 9238);            // scheme_action action=check
        sc.type = IntentType::SCHEME;
        sc.interaction_name = "befriend";
        sc.console_command = "check:20:";
        Intent bare = make("ci_9238_4_2", 9238);                // a single-target check
        bare.console_command = "check";
        Intent w = make("si_9238_4_3", 9238);                   // a send (a write)
        w.interaction_name = "gift_interaction";
        const bool drained = drain_one(ci) && drain_one(sc) && drain_one(bare) && drain_one(w);
        Intent got_sc{}, got_ci{};
        const bool asker_finds = q.find_completed_by_request_id("schc_9238_4_37106", &got_sc) &&
                                 q.find_completed_by_request_id("ci_9238_4_1", &got_ci) &&
                                 got_sc.console_output == "{\"valid_count\":0}";
        Intent lk{};
        const bool lookup_finds =
            q.lookup_dispatch("schc_9238_4_37106", &lk) == DispatchLookup::Completed;
        const std::vector<Intent> listed = q.completed_snapshot(IntentQueue::MAX_COMPLETED);
        const bool only_the_write = listed.size() == 1 && listed[0].request_id == "si_9238_4_3";
        for (int k = 0; k < 200; ++k) {
            Intent r = make("schc_flood_" + std::to_string(k), 9238);
            r.type = IntentType::SCHEME;
            r.console_command = "check:20:";
            drain_one(r);
        }
        Intent still{};
        const bool write_kept = q.find_completed_by_request_id("si_9238_4_3", &still) &&
                                q.completed_snapshot(IntentQueue::MAX_COMPLETED).size() == 1;
        row("l14_a_slot_read_leaves_no_ring_row_a_write_leaves_one",
            drained && asker_finds && lookup_finds && only_the_write && write_kept &&
                intent_is_read(sc) && intent_is_read(ci) && intent_is_read(bare) &&
                !intent_is_read(w),
            "listed " + std::to_string(listed.size()) + " row(s)" +
                (listed.empty() ? "" : ", first " + listed[0].request_id));
    }
    // Row 9 (L14, finding 3): the record's actor is the one the call names.
    {
        auto actor_of = [](const char* msg) {
            rapidjson::Document d;
            d.Parse(msg);
            return drain_actor_id(d);
        };
        const uint32_t steer = actor_of(
            R"({"action":"set_ai_personality","params":{"character_id":14147,"boldness":10},"request_id":"r1"})");
        const uint32_t top = actor_of(R"({"action":"x","actor_id":9238})");
        const uint32_t both = actor_of(
            R"({"action":"x","params":{"actor_id":7,"character_id":8}})");
        const uint32_t none = actor_of(R"({"action":"x","params":{"title_id":5}})");
        Intent rec{};
        const bool built = build_drain_write_verdict(true, "set_ai_personality", "r1", steer,
                                                     R"({"success":true})", &rec);
        row("l14_a_drained_write_records_the_actor_under_params",
            steer == 14147 && top == 9238 && both == 7 && none == 0 && built &&
                rec.actor_id == 14147,
            "steer " + std::to_string(steer) + ", top " + std::to_string(top) + ", both " +
                std::to_string(both) + ", none " + std::to_string(none));
    }
    // Row 10 (L14 FB m-6): queued checks are not pending writes.
    {
        IntentQueue q;
        Intent ci = make("ci_9238_5_1", 9238);
        ci.console_command = "check:1,2";
        Intent sc = make("schc_9238_5_2", 9238);
        sc.type = IntentType::SCHEME;
        sc.console_command = "check:20:";
        Intent w = make("si_9238_5_3", 9238);
        w.interaction_name = "gift_interaction";
        q.enqueue(ci);
        q.enqueue(sc);
        q.enqueue(w);
        const std::vector<Intent> pw = q.pending_writes();
        row("l14_fb_m6_query_intents_lists_no_queued_check",
            q.snapshot().size() == 3 && pw.size() == 1 && pw[0].request_id == "si_9238_5_3",
            "pending_writes " + std::to_string(pw.size()) + " of " + std::to_string(q.snapshot().size()));
    }
    // Row 11 (L14 FB2 N-10): pending_count() is the same count as
    // pending_writes(): send_interaction's pending_count and status'
    // pending_intents report it, query_intents reports pending_writes().size().
    {
        IntentQueue q;
        Intent ci = make("ci_9238_6_1", 9238);
        ci.console_command = "check:1,2";
        Intent sc = make("schc_9238_6_2", 9238);
        sc.type = IntentType::SCHEME;
        sc.console_command = "check:20:";
        q.enqueue(ci);
        q.enqueue(sc);
        q.enqueue(make("si_9238_6_3", 9238));
        q.enqueue(make("si_9238_6_4", 9238));
        const size_t pc = q.pending_count();
        const size_t pw = q.pending_writes().size();
        row("l14_fb2_n10_pending_count_counts_writes_only",
            pc == 2 && pw == 2 && q.pending_count_fast() == 4,
            "pending_count " + std::to_string(pc) + ", pending_writes " + std::to_string(pw) +
                ", fast " + std::to_string(q.pending_count_fast()));
    }
    std::printf("\n%s (%d failing row%s)\n", g_failures ? "FAIL" : "PASS", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
