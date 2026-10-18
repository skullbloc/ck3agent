// budget_ledger.cpp — R6 decision-budget governor, enforcing half (ck-6kvq.13).
//
// Pure: no engine reads, no offsets, no globals. See budget_ledger.h for the
// rules this implements and why they live in the bridge rather than the agent.

#include "budget_ledger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bridge {

// ---------------------------------------------------------------------------
// Small JSON helpers (the pipeline's, reproduced so this TU stays dependency-
// free — it is compiled standalone by test_budget_ledger).
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (uc < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static std::string jstr(const std::string& s) { return "\"" + json_escape(s) + "\""; }

static std::string jnum(int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return std::string(buf);
}

static std::string year_start_date(int32_t year) {
    // Clamped so the formatted width is bounded (and so a nonsense year read
    // cannot produce a date string no parser expects).
    if (year < 0) year = 0;
    if (year > 9998) year = 9998;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d.01.01", static_cast<int>(year));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
void BudgetLedger::configure(int64_t ceiling_tokens, bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    ceiling_ = ceiling_tokens > 0 ? ceiling_tokens : 0;
    enabled_ = enabled;
    armed_.store(enabled_ && ceiling_ > 0, std::memory_order_relaxed);
}

int64_t BudgetLedger::ceiling() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ceiling_;
}

bool BudgetLedger::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------
void BudgetLedger::observe_clock(int32_t year, const std::string& date) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!date.empty()) game_date_ = date;
    if (!year_seen_) {
        year_ = year;
        year_seen_ = true;
        return;
    }
    if (year < year_) {
        // MONOTONIC. A year that goes backwards cannot refill an allowance that
        // has already been spent; it is counted so the divergence is in the
        // data rather than swallowed.
        ++clock_regressions_;
        return;
    }
    year_ = year;
}

int32_t     BudgetLedger::game_year() const { std::lock_guard<std::mutex> lk(mu_); return year_; }
std::string BudgetLedger::game_date() const { std::lock_guard<std::mutex> lk(mu_); return game_date_; }

std::string BudgetLedger::resets_at() const {
    std::lock_guard<std::mutex> lk(mu_);
    return year_start_date(year_ + 1);
}

int64_t BudgetLedger::clock_regressions() const {
    std::lock_guard<std::mutex> lk(mu_);
    return clock_regressions_;
}

// ---------------------------------------------------------------------------
// Charging
// ---------------------------------------------------------------------------
BudgetLedger::Entry& BudgetLedger::entry_locked(const std::string& session_id) {
    return ledger_[Key(session_id, year_)];
}

BudgetVerdict BudgetLedger::verdict_locked(const std::string& session_id,
                                           const Entry& e, bool allowed,
                                           const char* reason) const {
    BudgetVerdict v;
    v.allowed    = allowed;
    v.reason     = reason ? reason : "";
    v.session_id = session_id;
    v.game_date  = game_date_;
    v.resets_at  = year_start_date(year_ + 1);
    v.game_year  = year_;
    v.spend      = e.usage.billed();
    v.ceiling    = ceiling_;
    return v;
}

BudgetVerdict BudgetLedger::report(const std::string& session_id,
                                   const std::string& message_id,
                                   const BudgetUsage& usage,
                                   int64_t rounds) {
    std::lock_guard<std::mutex> lk(mu_);

    BudgetUsage charge = usage;
    bool new_message = message_id.empty();

    if (!message_id.empty()) {
        SeenMessages& sm = seen_[session_id];
        auto it = sm.charged.find(message_id);
        if (it == sm.charged.end()) {
            new_message = true;
            sm.charged.emplace(message_id, usage);
            sm.order.push_back(message_id);
            while (sm.order.size() > kMaxTrackedMessages) {
                sm.charged.erase(sm.order.front());
                sm.order.pop_front();
            }
        } else {
            // Charge the positive per-field delta only; clamp at zero so a
            // shrinking rewrite cannot refund an agent.
            BudgetUsage& prev = it->second;
            charge.output_tokens         = std::max<int64_t>(0, usage.output_tokens         - prev.output_tokens);
            charge.uncached_input_tokens = std::max<int64_t>(0, usage.uncached_input_tokens - prev.uncached_input_tokens);
            charge.cache_read_tokens     = std::max<int64_t>(0, usage.cache_read_tokens     - prev.cache_read_tokens);
            charge.cache_write_tokens    = std::max<int64_t>(0, usage.cache_write_tokens    - prev.cache_write_tokens);
            prev.output_tokens         = std::max(prev.output_tokens,         usage.output_tokens);
            prev.uncached_input_tokens = std::max(prev.uncached_input_tokens, usage.uncached_input_tokens);
            prev.cache_read_tokens     = std::max(prev.cache_read_tokens,     usage.cache_read_tokens);
            prev.cache_write_tokens    = std::max(prev.cache_write_tokens,    usage.cache_write_tokens);
        }
    }

    Entry& e = entry_locked(session_id);
    const bool was_exhausted = e.exhausted;

    e.usage += charge;
    e.rounds += rounds;
    if (new_message) ++e.messages;
    if (!charge.is_zero()) {
        if (e.first_charge_date.empty()) e.first_charge_date = game_date_;
        e.last_charge_date = game_date_;
    }

    // THE LINE. Spending EXACTLY the ceiling spends it: the ceiling is an
    // allowance, and an agent that has used all of it has none left.
    if (enabled_ && ceiling_ > 0 && e.usage.billed() >= ceiling_) e.exhausted = true;

    BudgetVerdict v = verdict_locked(session_id, e, !e.exhausted,
                                     e.exhausted ? "budget_exhausted" : nullptr);
    v.crossed = e.exhausted && !was_exhausted;
    return v;
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------
bool BudgetLedger::is_exhausted(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || ceiling_ <= 0) return false;
    auto it = ledger_.find(Key(session_id, year_));
    return it != ledger_.end() && it->second.exhausted;
}

BudgetVerdict BudgetLedger::evaluate(const std::string& session_id,
                                     const std::string& action,
                                     bool is_mutation) {
    std::lock_guard<std::mutex> lk(mu_);

    auto it = ledger_.find(Key(session_id, year_));
    static const Entry kEmpty;
    const Entry& e = (it == ledger_.end()) ? kEmpty : it->second;

    if (!enabled_ || ceiling_ <= 0 || !is_mutation || it == ledger_.end() ||
        !it->second.exhausted) {
        return verdict_locked(session_id, e, true, nullptr);
    }

    // Refused. Record it: a refusal the episode record never hears about is
    // indistinguishable from an agent that simply chose not to act.
    Entry& live = it->second;
    ++live.refusals;
    ++refusals_total_;
    ++refusals_by_session_[session_id];

    BudgetRefusal r;
    r.seq        = ++refusal_seq_;
    r.session_id = session_id;
    r.game_date  = game_date_;
    r.game_year  = year_;
    r.action     = action;
    r.spend      = live.usage.billed();
    r.ceiling    = ceiling_;
    refusal_ring_.push_back(r);
    while (refusal_ring_.size() > kMaxRefusalRing) refusal_ring_.pop_front();

    return verdict_locked(session_id, live, false, "budget_exhausted");
}

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------
std::vector<BudgetRow> BudgetLedger::rows() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<BudgetRow> out;
    out.reserve(ledger_.size());
    for (const auto& kv : ledger_) {
        BudgetRow row;
        row.session_id        = kv.first.first;
        row.game_year         = kv.first.second;
        row.usage             = kv.second.usage;
        row.rounds            = kv.second.rounds;
        row.messages          = kv.second.messages;
        row.refusals          = kv.second.refusals;
        row.exhausted         = kv.second.exhausted;
        row.first_charge_date = kv.second.first_charge_date;
        row.last_charge_date  = kv.second.last_charge_date;
        out.push_back(row);
    }
    return out;
}

std::vector<BudgetRefusal> BudgetLedger::recent_refusals() const {
    std::lock_guard<std::mutex> lk(mu_);
    return std::vector<BudgetRefusal>(refusal_ring_.begin(), refusal_ring_.end());
}

int64_t BudgetLedger::refusal_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return refusals_total_;
}

int64_t BudgetLedger::refusal_count_for(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = refusals_by_session_.find(session_id);
    return it == refusals_by_session_.end() ? 0 : it->second;
}

std::string BudgetLedger::export_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string s;
    s += "{";
    s += "\"source\":\"bridge\"";
    s += ",\"enabled\":"; s += (enabled_ ? "true" : "false");
    s += ",\"per_agent_per_year\":"; s += jnum(ceiling_);
    s += ",\"game_year\":"; s += jnum(year_);
    s += ",\"game_date\":"; s += jstr(game_date_);
    s += ",\"resets_at\":"; s += jstr(year_start_date(year_ + 1));
    s += ",\"clock_regressions\":"; s += jnum(clock_regressions_);

    s += ",\"spent\":[";
    bool first = true;
    for (const auto& kv : ledger_) {
        if (!first) s += ",";
        first = false;
        const Entry& e = kv.second;
        // FIELD NAMES ARE THE BUNDLE'S, NOT THIS CLASS'S. `decisions` and
        // `cached_input_tokens` are what run_summary.json's budget.spent[] rows
        // already call these (scripts/schemas/run_summary.schema.json, added by
        // this bead's supervisor half); a second vocabulary for the same numbers
        // is a reconciliation bug waiting to happen. The four additive fields
        // this ENFORCING half needs — billed_tokens, refusals and the two charge
        // dates — are specified in scripts/schemas/budget_ledger.schema.json.
        s += "{\"session_id\":"; s += jstr(kv.first.first);
        s += ",\"game_year\":";  s += jnum(kv.first.second);
        s += ",\"output_tokens\":";         s += jnum(e.usage.output_tokens);
        s += ",\"uncached_input_tokens\":"; s += jnum(e.usage.uncached_input_tokens);
        // What the ceiling compares against, written out rather than left to be
        // re-derived — a consumer that re-derived it by adding cache would be
        // violating D4 and nobody would see it.
        s += ",\"billed_tokens\":";         s += jnum(e.usage.billed());
        // Recorded, never equalised (D4).
        s += ",\"cached_input_tokens\":";   s += jnum(e.usage.cache_read_tokens);
        s += ",\"cache_write_tokens\":";    s += jnum(e.usage.cache_write_tokens);
        s += ",\"decisions\":"; s += jnum(e.rounds);
        s += ",\"messages\":";  s += jnum(e.messages);
        s += ",\"refusals\":";  s += jnum(e.refusals);
        s += ",\"exhausted\":"; s += (e.exhausted ? "true" : "false");
        s += ",\"first_charge_date\":"; s += jstr(e.first_charge_date);
        s += ",\"last_charge_date\":";  s += jstr(e.last_charge_date);
        s += "}";
    }
    s += "]";

    s += ",\"refusals\":{\"total\":"; s += jnum(refusals_total_);
    s += ",\"by_session\":{";
    first = true;
    for (const auto& kv : refusals_by_session_) {
        if (!first) s += ",";
        first = false;
        s += jstr(kv.first); s += ":"; s += jnum(kv.second);
    }
    s += "},\"recent\":[";
    first = true;
    for (const BudgetRefusal& r : refusal_ring_) {
        if (!first) s += ",";
        first = false;
        s += "{\"seq\":";          s += jnum(static_cast<int64_t>(r.seq));
        s += ",\"session_id\":";   s += jstr(r.session_id);
        s += ",\"game_date\":";    s += jstr(r.game_date);
        s += ",\"game_year\":";    s += jnum(r.game_year);
        s += ",\"action\":";       s += jstr(r.action);
        s += ",\"spend\":";        s += jnum(r.spend);
        s += ",\"ceiling\":";      s += jnum(r.ceiling);
        s += ",\"reason\":\"budget_exhausted\"}";
    }
    s += "]}";
    s += "}";
    return s;
}

void BudgetLedger::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    ledger_.clear();
    seen_.clear();
    refusal_ring_.clear();
    refusals_by_session_.clear();
    refusals_total_ = 0;
    refusal_seq_ = 0;
    ceiling_ = 0;
    enabled_ = true;
    armed_.store(false, std::memory_order_relaxed);
    year_ = 0;
    year_seen_ = false;
    game_date_.clear();
    clock_regressions_ = 0;
}

BudgetLedger& budget_ledger() {
    static BudgetLedger g;
    return g;
}

// ---------------------------------------------------------------------------
// Where the gate applies
// ---------------------------------------------------------------------------
bool budget_gate_applies(const char* action_type,
                         bool is_mutation,
                         bool is_management_verb) {
    if (!action_type || !action_type[0]) return false;
    if (!is_mutation) return false;            // reads always continue (§5.3)
    if (is_management_verb) return false;      // tempo/observer/diagnostic verbs

    // The explicit carve-outs. `action_is_mutation` is simply `!readonly`, so
    // without these a spent session could not report the usage that would let
    // it out, nor collect a ticket for work the flush already ran.
    static const char* kAlways[] = {
        "report_usage", "set_budget_config", "get_budget_state",
        "check", "cancel", "handshake",
        // R4's board: reading the board is how a spent agent stays in the arm.
        "board_read",
        // Session-activity verbs (§2.7's record; turn_done itself is retired).
        "turn_done", "end_turn",
    };
    for (const char* a : kAlways) {
        if (std::strcmp(action_type, a) == 0) return false;
    }
    return true;
}

}  // namespace bridge
