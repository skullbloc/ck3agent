// ticket_table.cpp — R1b: the async ticket layer (ck-6kvq.9).
// See ticket_table.h. Deliberately dependency-free so it host-tests with no
// game: <cstdio>/<deque>/<mutex>/<string> and nothing from the engine.

#include "ticket_table.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bridge {

const char* ticket_state_name(TicketState s) {
    switch (s) {
        case TicketState::Pending: return "pending";
        case TicketState::Running: return "running";
        case TicketState::Done:    return "done";
        case TicketState::Failed:  return "failed";
        case TicketState::Expired: return "expired";
    }
    return "unknown";
}

std::string ticket_id_to_string(uint64_t id) {
    char buf[32];
    snprintf(buf, sizeof(buf), "tk_%08llu", (unsigned long long)id);
    return std::string(buf);
}

uint64_t ticket_id_from_string(const char* s) {
    if (!s || !s[0]) return 0;
    const char* p = s;
    if (strncmp(p, "tk_", 3) == 0) p += 3;
    // Reject anything that is not a pure decimal run: a typo must be an
    // unknown_ticket refusal, not a silent 0.
    for (const char* q = p; *q; ++q) {
        if (*q < '0' || *q > '9') return 0;
    }
    if (!*p) return 0;
    return strtoull(p, nullptr, 10);
}

// ============================================================================
TicketTable::TicketTable(const TicketTableConfig& cfg) : cfg_(cfg) {
    if (cfg_.capacity == 0) cfg_.capacity = 1;
}

void TicketTable::reindex_locked() {
    by_id_.clear();
    for (size_t i = 0; i < ring_.size(); ++i) by_id_[ring_[i].id] = i;
}

Ticket* TicketTable::find_locked(uint64_t id) {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return nullptr;
    if (it->second >= ring_.size()) return nullptr;
    return &ring_[it->second];
}

void TicketTable::open(uint64_t id, const std::string& session_id,
                       const std::string& request_id, const std::string& action,
                       Polarity polarity, uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    Ticket t;
    t.id         = id;
    t.session_id = session_id;
    t.request_id = request_id;
    t.action     = action;
    t.polarity   = polarity;
    t.state      = TicketState::Pending;
    t.created_ms = now_ms;
    t.updated_ms = now_ms;

    while (ring_.size() >= cfg_.capacity) {
        const Ticket& victim = ring_.front();
        // An uncollected result evicted by capacity is the intent ring's known
        // incident. Count it so the condition is loud rather than silent.
        if (victim.state == TicketState::Done || victim.state == TicketState::Running ||
            victim.state == TicketState::Pending) {
            ++evicted_;
        }
        auto rit = by_rid_.find(victim.request_id);
        if (rit != by_rid_.end() && rit->second == victim.id) by_rid_.erase(rit);
        ring_.pop_front();
    }
    ring_.push_back(t);
    reindex_locked();
    if (!request_id.empty()) by_rid_[request_id] = id;
}

bool TicketTable::mark_running(uint64_t id, uint64_t flush_id, uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    Ticket* t = find_locked(id);
    if (!t) return false;
    if (t->state != TicketState::Pending) return false;  // never walk a terminal state back
    t->state      = TicketState::Running;
    t->flush_id   = flush_id;
    t->updated_ms = now_ms;
    return true;
}

void TicketTable::complete(uint64_t id, const std::string& result_json, uint64_t flush_id,
                           uint32_t character_id, uint64_t tick_executed,
                           const std::string& game_date, uint64_t now_ms,
                           const std::string& attribution_reason) {
    std::lock_guard<std::mutex> lk(mu_);
    Ticket* t = find_locked(id);
    if (!t) return;
    if (t->state == TicketState::Done || t->state == TicketState::Failed) return;
    t->state               = TicketState::Done;
    t->result_json         = result_json;
    t->flush_id            = flush_id;
    t->character_id        = character_id;
    t->tick_executed       = tick_executed;
    t->game_date_executed  = game_date;
    t->attribution_reason  = attribution_reason;
    t->updated_ms          = now_ms;
}

void TicketTable::fail(uint64_t id, const std::string& result_json, uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    Ticket* t = find_locked(id);
    if (!t) return;
    if (t->state == TicketState::Done || t->state == TicketState::Failed) return;
    t->state       = TicketState::Failed;
    t->result_json = result_json;
    t->updated_ms  = now_ms;
}

bool TicketTable::find(uint64_t id, Ticket* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_id_.find(id);
    if (it == by_id_.end() || it->second >= ring_.size()) return false;
    if (out) *out = ring_[it->second];
    return true;
}

bool TicketTable::find_by_request_id(const std::string& request_id, Ticket* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (request_id.empty()) return false;
    auto rit = by_rid_.find(request_id);
    if (rit == by_rid_.end()) return false;
    auto it = by_id_.find(rit->second);
    if (it == by_id_.end() || it->second >= ring_.size()) return false;
    if (out) *out = ring_[it->second];
    return true;
}

size_t TicketTable::expire_older_than(uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cfg_.ttl_ms == 0) return 0;
    size_t n = 0;
    for (Ticket& t : ring_) {
        if (t.state == TicketState::Expired) continue;
        if (now_ms < t.created_ms) continue;
        if (now_ms - t.created_ms < cfg_.ttl_ms) continue;
        t.state      = TicketState::Expired;
        t.updated_ms = now_ms;
        ++n;
    }
    return n;
}

size_t TicketTable::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ring_.size();
}

std::vector<Ticket> TicketTable::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return std::vector<Ticket>(ring_.begin(), ring_.end());
}

uint64_t TicketTable::evicted_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return evicted_;
}

}  // namespace bridge
