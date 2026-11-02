// intent_queue.cpp — IntentQueue's members (L13 FF2: split out of
// intent_dispatch.cpp). Engine-free: the queue is a mutex-protected vector and
// a completed ring, so bridge/test/test_intent_lookup.cpp links this TU and
// drives the real lookup across a claim, a finish and a store. L13 FF3: no
// pointer into intents_ leaves this file (claim/finish copy under the lock).

#include "bridge.h"
#include "intent_queue.h"
#include "session.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Local logging macro -- g_logfile only (stderr is unsafe in pressure-vessel)
#define IQLOG(fmt, ...) do { \
    if (bridge::g_logfile) { fprintf(bridge::g_logfile, "[CK3-Bridge][IntentQ] " fmt, ##__VA_ARGS__); fflush(bridge::g_logfile); } \
} while(0)

namespace bridge {

// =============================================================================
// IntentQueue Implementation
// =============================================================================

void IntentQueue::enqueue(Intent intent) {
    intent.consumed = false;
    intent.executed = false;
    intent.rejected = false;
    memset(intent.rejection_reason, 0, sizeof(intent.rejection_reason));

    // M1: stamp the current session on the intent so the game-thread drain can
    // reinstall the owning context.
    if (intent.session_id.empty()) {
        bridge::Session* s = bridge::current_session();
        if (s) intent.session_id = s->session_id;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    intent.seq = next_seq_++;
    intent.in_flight = false;
    intents_.push_back(std::move(intent));
    pending_count_atomic_.fetch_add(1, std::memory_order_relaxed);
    IQLOG("Enqueued: %s actor=%u target=%u request_id=%s session_id=%s\n",
          intents_.back().interaction_name.c_str(),
          intents_.back().actor_id & 0xFFFFFF,
          intents_.back().target_id & 0xFFFFFF,
          intents_.back().request_id.c_str(),
          intents_.back().session_id.c_str());
}

bool IntentQueue::claim(uint64_t seq, Intent* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& intent : intents_) {
        if (intent.seq != seq) continue;
        if (intent.consumed) return false;
        intent.consumed = true;
        intent.in_flight = true;
        if (out) *out = intent;
        return true;
    }
    return false;
}

void IntentQueue::finish(const Intent& done) {
    Intent rec = done;
    rec.in_flight = false;
    rec.consumed = true;
    if (!rec.executed && !rec.rejected) {
        rec.rejected = true;
        snprintf(rec.rejection_reason, sizeof(rec.rejection_reason), "executor_returned_no_verdict");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& intent : intents_) {
        if (intent.seq != rec.seq) continue;
        intent = rec;
        return;
    }
    // Cleared while it executed: keep the verdict.
    store_completed_locked(rec);
}

std::vector<Intent> IntentQueue::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Intent> result;
    for (const auto& intent : intents_) {
        if (!intent.consumed) {
            result.push_back(intent);
        }
    }
    return result;
}

std::vector<Intent> IntentQueue::pending_writes() {
    std::vector<Intent> result;
    for (auto& intent : snapshot())
        if (!intent_is_read(intent)) result.push_back(std::move(intent));
    return result;
}

size_t IntentQueue::pending_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& intent : intents_) {
        if (!intent.consumed && !intent_is_read(intent)) count++;
    }
    return count;
}

void IntentQueue::tick_maintenance(uint64_t current_tick,
                                    std::vector<Intent>& completed_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!intents_.empty() &&
        current_tick % 100 == 0) {
        IQLOG("tick_maintenance: tick=%lu pending=%zu\n", (unsigned long)current_tick, intents_.size());
        for (auto& i : intents_) {
            IQLOG("  intent: %s actor=%u age=%lu ttl=%u consumed=%d\n",
                  i.interaction_name.c_str(), i.actor_id & 0xFFFFFF,
                  (unsigned long)(current_tick - i.queued_tick), i.ttl_ticks, i.consumed ? 1 : 0);
        }
    }

    // Collect completed + expired intents
    for (auto& intent : intents_) {
        // L13 FF3: maintenance runs on the drain's thread after the drain
        // returned, so an intent still in flight here is one whose execution
        // the crash recovery jumped out of: its verdict is that crash.
        if (intent.in_flight) {
            intent.in_flight = false;
            intent.executed = false;
            intent.rejected = true;
            snprintf(intent.rejection_reason, sizeof(intent.rejection_reason),
                     "game_thread_crash_during_drain");
        }
        if (intent.consumed) {
            completed_out.push_back(intent);
            continue;
        }
        // Check TTL expiry
        if (current_tick > intent.queued_tick + intent.ttl_ticks) {
            intent.consumed = true;
            intent.rejected = true;
            snprintf(intent.rejection_reason, sizeof(intent.rejection_reason),
                     "ttl_expired");
            completed_out.push_back(intent);
            IQLOG("Intent expired: %s actor=%u (queued=%lu ttl=%u current=%lu)\n",
                  intent.interaction_name.c_str(),
                  intent.actor_id & 0xFFFFFF,
                  (unsigned long)intent.queued_tick,
                  intent.ttl_ticks,
                  (unsigned long)current_tick);
        }
    }

    // L13 FF2 (witness R3): the move to the completed ring happens HERE, under
    // the same lock as the erase, so a reader (lookup_dispatch) sees every
    // consumed intent in exactly one of the two places, never in neither.
    for (const auto& c : completed_out) store_completed_locked(c);
    // Remove all consumed intents from the vector
    intents_.erase(
        std::remove_if(intents_.begin(), intents_.end(),
                        [](const Intent& i) { return i.consumed; }),
        intents_.end());
    // Update atomic counter to reflect remaining unconsumed intents
    size_t unconsumed = 0;
    for (const auto& i : intents_) { if (!i.consumed) unconsumed++; }
    pending_count_atomic_.store(unconsumed, std::memory_order_relaxed);
}

bool IntentQueue::cancel_by_request_id(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = intents_.begin(); it != intents_.end(); ++it) {
        if (!it->consumed && it->request_id == request_id) {
            IQLOG("Intent cancelled: %s actor=%u request_id=%s\n",
                  it->interaction_name.c_str(), it->actor_id & 0xFFFFFF, request_id.c_str());
            intents_.erase(it);
            if (pending_count_atomic_.load(std::memory_order_relaxed) > 0)
                pending_count_atomic_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

bool IntentQueue::retire_pending_by_request_id(const std::string& request_id,
                                               const char* reason) {
    if (request_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& it : intents_) {
        if (!it.consumed && it.request_id == request_id) {
            it.consumed = true;
            it.rejected = true;
            it.executed = false;
            snprintf(it.rejection_reason, sizeof(it.rejection_reason), "%s",
                     reason ? reason : "retired");
            if (pending_count_atomic_.load(std::memory_order_relaxed) > 0)
                pending_count_atomic_.fetch_sub(1, std::memory_order_relaxed);
            IQLOG("Intent retired: %s actor=%u request_id=%s reason=%s\n",
                  it.interaction_name.c_str(), it.actor_id & 0xFFFFFF,
                  request_id.c_str(), it.rejection_reason);
            return true;
        }
    }
    return false;
}

void IntentQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    intents_.clear();
    pending_count_atomic_.store(0, std::memory_order_relaxed);
    IQLOG("All intents cleared\n");
}

void IntentQueue::store_completed(const Intent& intent) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_completed_locked(intent);
}

void IntentQueue::store_completed_locked(const Intent& intent) {
    // L14 (finding 3): a read's verdict is its asker's only; it never takes a
    // slot in, or is listed from, the writes' ring.
    std::vector<Intent>& ring = intent_is_read(intent) ? reads_ : completed_;
    const size_t cap = intent_is_read(intent) ? MAX_READS : MAX_COMPLETED;
    if (ring.size() >= cap) {
        ring.erase(ring.begin());
    }
    ring.push_back(intent);
}

// Newest-first search of both rings (writes, then reads) for one request id.
static bool find_in_rings(const std::vector<Intent>& writes, const std::vector<Intent>& reads,
                          const std::string& request_id, Intent* out) {
    for (const std::vector<Intent>* ring : {&writes, &reads}) {
        for (auto it = ring->rbegin(); it != ring->rend(); ++it) {
            if (it->request_id == request_id) {
                if (out) *out = *it;
                return true;
            }
        }
    }
    return false;
}

std::vector<Intent> IntentQueue::completed_snapshot(size_t max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = std::min(max_count, completed_.size());
    return std::vector<Intent>(completed_.end() - static_cast<ptrdiff_t>(n),
                               completed_.end());
}

bool IntentQueue::find_completed_by_request_id(const std::string& request_id,
                                                Intent* out) {
    if (request_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // Walk newest-to-oldest so duplicates surface the most recent result.
    return find_in_rings(completed_, reads_, request_id, out);
}

DispatchLookup IntentQueue::lookup_dispatch(const std::string& request_id, Intent* out) {
    if (request_id.empty()) return DispatchLookup::NotFound;
    std::lock_guard<std::mutex> lock(mutex_);
    bool consumed = false, pending = false, completed = false;
    for (const auto& intent : intents_) {
        if (intent.request_id != request_id) continue;
        // Claimed and not finished: the drain is executing its own copy.
        if (intent.in_flight) { consumed = true; break; }
        // Finished, not yet moved to the ring: its verdict (every field written
        // under this lock by finish()).
        if (intent.consumed) { completed = true; if (out) *out = intent; break; }
        pending = true;
        if (out) *out = intent;
        break;
    }
    if (!consumed && !pending && !completed)
        completed = find_in_rings(completed_, reads_, request_id, out);
    return dispatch_lookup(completed, consumed, pending);
}

bool IntentQueue::mark_rejected_in_flight(uint64_t seq, const char* reason) {
    if (seq == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& intent : intents_) {
        if (intent.seq != seq || !intent.in_flight) continue;
        intent.executed = false;
        intent.rejected = true;
        snprintf(intent.rejection_reason, sizeof(intent.rejection_reason), "%s", reason);
        intent.consumed = true;
        intent.in_flight = false;
        return true;
    }
    return false;
}

thread_local uint64_t g_inflight_intent_seq = 0;

uint64_t recover_drain_crash(IntentQueue& q, ClientContext* saved_ctx, const char* reason) {
    g_current_client_context = saved_ctx;
    const uint64_t seq = g_inflight_intent_seq;
    g_inflight_intent_seq = 0;
    if (seq != 0 && q.mark_rejected_in_flight(seq, reason)) return seq;
    return 0;
}

} // namespace bridge
