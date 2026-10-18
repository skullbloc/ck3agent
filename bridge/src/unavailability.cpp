// unavailability.cpp — see unavailability.h.
//
// ck-6kvq.10 (R3 as reversed) built the accounting; ck-ijfa replaced the GATE
// with the progenitor's 2026-09-13 ruling: anomalous is a WALL-CLOCK judgement,
// the covariate stays in GAME-DAYS, and the retired game-day `min_span` is not
// re-derived because no game-day number can express "the agent was late" while
// the clock rate is free. ck-2ngg then made X itself a RULE over the episode's
// DECLARED horizon rather than the constant 365, which is that rule's answer at
// the registered 20-game-year horizon and at no other.

#include "unavailability.h"

namespace bridge {

void UnavailabilityLedger::configure(int64_t anomaly_wall_s, int64_t exclusion_days,
                                     bool exclusion_armed) {
    std::lock_guard<std::mutex> lk(mu_);
    if (anomaly_wall_s >= 0) anomaly_wall_s_ = anomaly_wall_s;
    // ck-2ngg: the armed bit is applied UNCONDITIONALLY, and the threshold only
    // when it is armed. A caller that disarms must not be able to leave a stale
    // X standing behind the disarm, and an unarmed episode's X is 0 — not a
    // threshold of zero, which `exclusion_armed_` is there to say.
    exclusion_armed_ = exclusion_armed;
    exclusion_days_  = (exclusion_armed && exclusion_days >= 0) ? exclusion_days : 0;
}

int64_t UnavailabilityLedger::anomaly_wall_seconds() const {
    std::lock_guard<std::mutex> lk(mu_);
    return anomaly_wall_s_;
}

int64_t UnavailabilityLedger::exclusion_days() const {
    std::lock_guard<std::mutex> lk(mu_);
    return exclusion_days_;
}

bool UnavailabilityLedger::exclusion_armed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return exclusion_armed_;
}

// THE GATE, in one place. UNKNOWN IS NOT "LONG": a gap with no wall stamp is
// never anomalous, because the only way to call it long would be to convert its
// game-days at an assumed rate, and an assumed rate is the defect ck-ijfa was
// filed for.
bool UnavailabilityLedger::is_anomalous_locked(bool wall_measured,
                                               double wall_seconds) const {
    if (!wall_measured) return false;
    return wall_seconds > static_cast<double>(anomaly_wall_s_);
}

// The wall cursor moves on ANY stamped call, including one whose game day is
// refused as a regression: wall time passed whether or not the world's clock
// did, and the OPEN gap is judged against this cursor.
void UnavailabilityLedger::note_wall_locked(uint64_t wall_ms) {
    if (wall_ms == 0) return;                       // "not stamped"
    if (!have_wall_) { current_wall_ms_ = wall_ms; have_wall_ = true; return; }
    if (wall_ms < current_wall_ms_) return;         // non-monotonic: ignore
    current_wall_ms_ = wall_ms;
}

void UnavailabilityLedger::note_day(int64_t day, uint64_t wall_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    note_wall_locked(wall_ms);
    if (!have_day_) { current_day_ = first_day_ = day; have_day_ = true; return; }
    if (day < current_day_) { ++day_regressions_; return; }
    current_day_ = day;
}

int64_t  UnavailabilityLedger::current_day() const {
    std::lock_guard<std::mutex> lk(mu_); return current_day_;
}
int64_t  UnavailabilityLedger::first_day() const {
    std::lock_guard<std::mutex> lk(mu_); return first_day_;
}
// WHAT THE EPISODE ACTUALLY OBSERVED, in game-days — never what it declared.
// 0 before the first day reaches the ledger, which is honest: nothing has been
// observed yet. See the header for why this exists beside the declared horizon.
int64_t  UnavailabilityLedger::observed_game_days() const {
    std::lock_guard<std::mutex> lk(mu_);
    return have_day_ ? (current_day_ - first_day_) : 0;
}
bool     UnavailabilityLedger::has_day() const {
    std::lock_guard<std::mutex> lk(mu_); return have_day_;
}
uint64_t UnavailabilityLedger::day_regressions() const {
    std::lock_guard<std::mutex> lk(mu_); return day_regressions_;
}

void UnavailabilityLedger::note_session(const std::string& session_id, int64_t day,
                                        uint64_t wall_ms) {
    if (session_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    note_wall_locked(wall_ms);
    if (!have_day_) { current_day_ = first_day_ = day; have_day_ = true; }
    else if (day > current_day_) current_day_ = day;

    auto it = recs_.find(session_id);
    if (it != recs_.end()) {
        // A RE-REGISTRATION IS AN ATTACH, NOT A RESET. bead .5's resume rebinds
        // the same session_id; zeroing `last_activity_day` here would erase the
        // very gap the resume just ended — the covariate would read 0 for
        // exactly the failure it exists to measure.
        it->second.attached = true;
        it->second.ended    = false;
        return;
    }
    Rec r;
    r.first_day         = day;
    r.last_activity_day = day;   // silence is measured from the moment it connected
    if (wall_ms != 0) {
        r.wall_stamped          = true;
        r.first_wall_ms         = wall_ms;
        r.last_activity_wall_ms = wall_ms;
    }
    recs_[session_id]   = r;
}

void UnavailabilityLedger::note_attached(const std::string& session_id, bool attached) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = recs_.find(session_id);
    if (it == recs_.end()) return;
    it->second.attached = attached;
}

void UnavailabilityLedger::note_session_ended(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = recs_.find(session_id);
    if (it == recs_.end()) return;
    it->second.ended    = true;
    it->second.attached = false;
}

// ck-j6lk: ERASE, not mark. The record is removed from recs_, so the session cannot
// appear in snapshot(), cannot carry an open span, and cannot be over threshold —
// because it was never a participant and has no silence to account for.
//
// Erasing and marking-ended differ in exactly the way shakedown 3 showed: the marked
// record stayed in the report with 364 silent days against an X of 18. A row that
// only checks the session was destroyed cannot tell these apart, which is how my own
// acceptance passed while the episode still sealed invalid — it graded the registry
// and the ledger is a different observer.
void UnavailabilityLedger::forget_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    recs_.erase(session_id);
}

// AN ANOMALOUS GAP ALWAYS GETS IN. When the list is full it evicts the OLDEST
// NON-ANOMALOUS gap; only when every retained gap is already anomalous does the
// record truncate. Ordinary turn-taking must not be able to crowd the covariate
// out of its own itemisation.
void UnavailabilityLedger::push_span_locked(Rec& r, const UnavailSpan& s) const {
    if (r.spans.size() < kMaxRetainedSpans) { r.spans.push_back(s); return; }
    if (s.anomalous) {
        for (auto it = r.spans.begin(); it != r.spans.end(); ++it) {
            if (!it->anomalous) {
                r.spans.erase(it);
                r.spans.push_back(s);
                r.spans_truncated = true;   // something WAS dropped: say so
                return;
            }
        }
    }
    r.spans_truncated = true;   // totals and counts stay exact; itemisation lost
}

void UnavailabilityLedger::note_activity(const std::string& session_id, int64_t day,
                                         uint64_t wall_ms) {
    if (session_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    note_wall_locked(wall_ms);

    // Unknown id => an observer, an anonymous socket, or a session that was
    // never registered as CONTROL. Observer traffic never counts (§2.7).
    auto it = recs_.find(session_id);
    if (it == recs_.end()) return;

    if (!have_day_) { current_day_ = first_day_ = day; have_day_ = true; }
    else if (day > current_day_) current_day_ = day;

    Rec& r = it->second;
    ++r.activity_count;

    // A gap is only closed FORWARD. Two calls on the same game day (the common
    // case — an agent does several things per day) close nothing and cost
    // nothing.
    if (day > r.last_activity_day) {
        const int64_t gap = day - r.last_activity_day;

        // THE SAME GAP IN WALL-CLOCK, which is what the gate reads.
        const bool wall_ok = r.wall_stamped && wall_ms != 0 &&
                             wall_ms >= r.last_activity_wall_ms;
        const double gap_wall_s =
            wall_ok ? static_cast<double>(wall_ms - r.last_activity_wall_ms) / 1000.0
                    : 0.0;

        // RAW totals: every forward gap, anomalous or not, both units.
        r.silent_closed += gap;
        if (wall_ok) r.silent_wall_closed += gap_wall_s;
        ++r.gap_count_closed;
        if (!wall_ok) ++r.unjudged_closed;
        // ck-48lf: the extrema, here where EVERY gap passes, and before
        // push_span_locked can decline to retain this one.
        if (gap > r.max_gap_days_closed) r.max_gap_days_closed = gap;
        if (wall_ok && gap_wall_s > r.max_gap_wall_closed)
            r.max_gap_wall_closed = gap_wall_s;

        UnavailSpan sp;
        sp.start_day     = r.last_activity_day;
        sp.end_day       = day;
        sp.days          = gap;
        sp.open          = false;
        sp.wall_measured = wall_ok;
        if (wall_ok) {
            sp.start_wall_ms = r.last_activity_wall_ms;
            sp.end_wall_ms   = wall_ms;
            sp.wall_seconds  = gap_wall_s;
        }
        sp.anomalous = is_anomalous_locked(wall_ok, gap_wall_s);

        // THE COVARIATE: anomalous gaps only, and it is the GAME-DAYS they
        // SPANNED — what the absence cost the experiment, not how late the agent
        // was.
        if (sp.anomalous) {
            r.unavail_closed      += gap;
            r.unavail_wall_closed += gap_wall_s;
            ++r.span_count_closed;
        }
        push_span_locked(r, sp);

        r.last_activity_day = day;
        if (wall_ms != 0) {
            r.wall_stamped          = true;
            r.last_activity_wall_ms = wall_ms;
            if (r.first_wall_ms == 0) r.first_wall_ms = wall_ms;
        }
    } else if (wall_ms != 0) {
        // Same game day, later wall clock — the agent is active, so the wall
        // cursor for its NEXT gap must move even though no game-day gap closed.
        // Without this a fast world would make every gap look shorter in wall
        // time than it was.
        r.wall_stamped = true;
        if (wall_ms >= r.last_activity_wall_ms) r.last_activity_wall_ms = wall_ms;
        if (r.first_wall_ms == 0) r.first_wall_ms = wall_ms;
    }
    // Activity dated at or before the cursor (same day, or a reload) never
    // rewinds it — a rewind would re-open a gap that has already been counted.
}

UnavailSession UnavailabilityLedger::render_locked(const std::string& id, const Rec& r) const {
    UnavailSession u;
    u.session_id        = id;
    u.attached          = r.attached;
    u.ended             = r.ended;
    u.first_day         = r.first_day;
    u.last_activity_day = r.last_activity_day;
    u.activity_count    = r.activity_count;

    u.silent_game_days    = r.silent_closed;
    u.silent_wall_seconds = r.silent_wall_closed;
    u.gap_count           = r.gap_count_closed;

    u.unavailable_game_days    = r.unavail_closed;
    u.unavailable_wall_seconds = r.unavail_wall_closed;
    u.span_count               = r.span_count_closed;
    u.unjudged_gap_count       = r.unjudged_closed;

    u.spans                 = r.spans;
    u.spans_truncated       = r.spans_truncated;
    // ck-48lf: the closed extrema; the OPEN gap joins below, on the same path
    // the open totals take, so the maximum cannot miss a session that never
    // came back -- the case the covariate most needs.
    u.max_span_wall_seconds = r.max_gap_wall_closed;
    u.max_span_game_days    = r.max_gap_days_closed;

    u.wall_measured         = r.wall_stamped;
    u.first_wall_ms         = r.first_wall_ms;
    u.last_activity_wall_ms = r.last_activity_wall_ms;

    // THE OPEN GAP, computed here and stored nowhere. This is what keeps the
    // sum honest across a close: the running gap is never added to a counter, so
    // when it finally closes in note_activity it is counted exactly once.
    //
    // IT IS JUDGED THE SAME WAY. An agent that has been silent for more than the
    // gate RIGHT NOW is unavailable right now — a covariate that only recognised
    // an absence once the agent came back would read zero for a session that
    // never returns, which is the case it most needs to catch.
    const int64_t open_gap = current_day_ - r.last_activity_day;
    const bool open_wall_ok = r.wall_stamped && have_wall_ &&
                              current_wall_ms_ >= r.last_activity_wall_ms;
    const double open_wall_s =
        open_wall_ok
            ? static_cast<double>(current_wall_ms_ - r.last_activity_wall_ms) / 1000.0
            : 0.0;
    const bool open_anomalous = is_anomalous_locked(open_wall_ok, open_wall_s);

    if (open_gap > 0) {
        u.silent_game_days += open_gap;
        if (open_wall_ok) u.silent_wall_seconds += open_wall_s;
        ++u.gap_count;
        if (!open_wall_ok) ++u.unjudged_gap_count;

        if (open_anomalous) {
            u.unavailable_game_days    += open_gap;
            u.unavailable_wall_seconds += open_wall_s;
            ++u.span_count;
        }
        // ck-48lf: the open gap is a gap. MAX, not +=, and outside the
        // anomalous branch because the extremum is over EVERY gap.
        if (open_gap > u.max_span_game_days) u.max_span_game_days = open_gap;
        if (open_wall_ok && open_wall_s > u.max_span_wall_seconds)
            u.max_span_wall_seconds = open_wall_s;
        if (u.spans.size() < kMaxRetainedSpans) {
            UnavailSpan s;
            s.start_day     = r.last_activity_day;
            s.end_day       = current_day_;
            s.days          = open_gap;
            s.open          = true;
            s.wall_measured = open_wall_ok;
            if (open_wall_ok) {
                s.start_wall_ms = r.last_activity_wall_ms;
                s.end_wall_ms   = current_wall_ms_;
                s.wall_seconds  = open_wall_s;
            }
            s.anomalous = open_anomalous;
            u.spans.push_back(s);
        } else {
            u.spans_truncated = true;
        }
    }

    // NO X, NO VERDICT (ck-2ngg). An unarmed episode declared no horizon, so
    // there is nothing to be over. Reporting `false` here is correct and is NOT
    // "this session passed" — `exclusion_armed()` is the field that says which
    // of the two a false means, and the seal marks the criterion NOT EVALUABLE.
    u.over_threshold = exclusion_armed_ &&
                       (u.unavailable_game_days > exclusion_days_);
    return u;
}

std::vector<UnavailSession> UnavailabilityLedger::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<UnavailSession> out;
    out.reserve(recs_.size());
    for (const auto& kv : recs_) out.push_back(render_locked(kv.first, kv.second));
    return out;   // std::map => ids ascending, a stable wire order
}

bool UnavailabilityLedger::episode_invalid() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : recs_) {
        if (render_locked(kv.first, kv.second).over_threshold) return true;
    }
    return false;
}

void UnavailabilityLedger::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    recs_.clear();
    current_day_     = 0;
    first_day_       = 0;
    have_day_        = false;
    day_regressions_ = 0;
    current_wall_ms_ = 0;
    have_wall_       = false;
}

UnavailabilityLedger& unavailability() {
    static UnavailabilityLedger l;
    return l;
}

}  // namespace bridge
