// clock_rate.cpp — see clock_rate.h (ck-ijfa).

#include "clock_rate.h"

namespace bridge {

void ClockRateMeter::note(const TickBookkeeping& bk, int64_t game_day,
                          uint64_t wall_ms, int speed, bool paused) {
    // BEFORE THE LOCK (ck-zffq). The call is unconditional now, so the cost in
    // the modes that record nothing must be a branch and not a mutex.
    if (!bk.note_clock_rate) return;
    std::lock_guard<std::mutex> lk(mu_);

    // RE-ANCHOR ON A DISCONTINUITY.  A world reload moves the game day
    // backwards; so does loading a different save.  Averaging across that would
    // report a rate the run never ran at — and a NEGATIVE one whenever the jump
    // back is large.  The wall clock going backwards is the same problem from
    // the other side (it should not, being monotonic, but a caller that passes
    // the wrong clock must not produce a plausible wrong answer).
    if (have_ && (game_day < last_day_ || wall_ms < last_ms_)) {
        ++reanchors_;
        have_ = false;
    }

    if (!have_) {
        have_      = true;
        first_day_ = last_day_ = game_day;
        first_ms_  = last_ms_  = wall_ms;
        samples_   = 0;
        paused_    = 0;
        speed_min_ = speed_max_ = 0;
    } else {
        last_day_ = game_day;
        last_ms_  = wall_ms;
    }

    ++samples_;
    if (paused) ++paused_;
    if (speed > 0) {
        if (speed_min_ == 0 || speed < speed_min_) speed_min_ = speed;
        if (speed > speed_max_) speed_max_ = speed;
    }
}

ClockRateSample ClockRateMeter::get() const {
    std::lock_guard<std::mutex> lk(mu_);
    ClockRateSample s;
    s.samples        = samples_;
    s.paused_samples = paused_;
    s.speed_min      = speed_min_;
    s.speed_max      = speed_max_;
    s.reanchors      = reanchors_;
    if (!have_) return s;

    s.first_day     = first_day_;
    s.last_day      = last_day_;
    s.first_wall_ms = first_ms_;
    s.last_wall_ms  = last_ms_;
    s.game_days     = last_day_ - first_day_;
    s.wall_seconds  = static_cast<double>(last_ms_ - first_ms_) / 1000.0;

    // NOT MEASURED is a real answer.  A window too short to mean anything, or
    // one in which the world never moved, publishes nothing at all rather than a
    // number a reader would go on to convert a threshold with.
    if (s.wall_seconds < kMinWallSeconds) return s;
    if (s.game_days <= 0) return s;

    s.measured = true;
    s.game_days_per_wall_second = static_cast<double>(s.game_days) / s.wall_seconds;
    s.game_years_per_wall_hour  =
        s.game_days_per_wall_second * 3600.0 / kGameDaysPerGameYear;
    return s;
}

void ClockRateMeter::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    have_      = false;
    first_day_ = last_day_ = 0;
    first_ms_  = last_ms_  = 0;
    samples_   = 0;
    paused_    = 0;
    speed_min_ = speed_max_ = 0;
    reanchors_ = 0;
}

ClockRateMeter& clock_rate() {
    static ClockRateMeter m;
    return m;
}

}  // namespace bridge
