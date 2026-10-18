// exclusion_rule.h — X IS A RULE, NOT A CONSTANT (ck-2ngg).
//
// Design of record: pre-registration §7.1. Predecessors: ck-6kvq.10 (the
// covariate), ck-ijfa (the wall-clock anomaly gate + the both-units rule),
// ck-uq5i (the same ruled-meaning-vs-coded-constant shape, one bead earlier).
//
// ─── THE DEFECT THIS EXISTS TO END ─────────────────────────────────────────
// `unavailability.h` stored `exclusion_days_ = 365`. 365 is not a threshold —
// it is the RULE "X is 5% of the episode's horizon, unsteered" EVALUATED AT ONE
// HORIZON: the registered 20-game-year run, 7300 game-days x 0.05 = 365.0
// exactly. At any other horizon the constant is simply wrong, and wrong in the
// PERMISSIVE direction, which is why nothing ever showed a symptom:
//
//   horizon                              game-days   rule    constant
//   1066.09.15 -> 1086.09.15 (20 y)           7300     365         365   ==
//   1066.09.15 -> 1071.01.01 (smoke)          1568      78         365   4.7x
//
// A smoke episode judged at 365 cannot be excluded at all: its entire horizon
// is 1568 game-days, so a session would have to be unsteered for 23% of the run
// before the constant noticed, where the rule notices at 5%. The number agreed
// with its meaning in exactly one configuration — ck-uq5i's shape exactly.
//
// ─── THE RULE ──────────────────────────────────────────────────────────────
//   X = round(FRACTION x (horizon_end - horizon_start), in GAME-DAYS)
//
// FRACTION is the single pre-registered constant: 0.05, overridable for a pilot
// via CK3_UNAVAIL_EXCLUSION_FRACTION and PUBLISHED beside every value it
// produced. The HORIZON is DECLARED by the harness at episode start — the
// bridge never guesses one, never substitutes a default, and never derives one
// from what it has happened to observe so far.
//
// ROUNDING: round half away from zero (llround) on the product. The span is
// non-negative, so that is plain round-half-up. Stated because 78.4 and 78.55
// are one rounding rule apart and the difference is a threshold.
//
// ─── NO HORIZON => UNARMED, TYPED, NEVER 365 ───────────────────────────────
// An episode that declares no horizon has no X. The bridge REFUSES to arm the
// exclusion rather than falling back to the constant, because falling back is
// precisely the defect: it would judge an unknown-length episode against a
// 20-year number. `armed == false` carries a typed reason, nothing is flagged
// over threshold, and the seal marks `unavailability_over_x` NOT EVALUABLE. An
// unevaluated criterion must never read as a passed one.
//
// ─── THE CALENDAR IS THE ENGINE'S ──────────────────────────────────────────
// The span is computed through ck3::game_date (ck-05mg) — REAL month lengths,
// 31/28/31/30/... — because that is the calendar the ledger's own day numbers
// come from (`(raw_date - DATE_EPOCH)/24`). It is NOT the uniform-30-day
// `_ordinal()` in scripts/meter_lane.py, which is documented there as CADENCE
// ARITHMETIC ONLY and is never written to a row. The two disagree by 3 days on
// any horizon starting 15 September, and mixing them is the same class of
// units error as the one ck-ijfa was filed for. See test_unavailability's
// exclusion lane, which asserts both numbers side by side.
//
// PURE. Header-only, no engine reads, no globals, no allocation beyond the
// strings it is handed. Host-tested (bridge/test/test_unavailability.cpp,
// bridge/test/acceptance_exclusion_fraction.cpp).

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "episode_policy.h"  // the RULE's constants: fraction, unarmed names
#include "game_date.h"      // ck-05mg: THE calendar. Never open-code one (section D).

namespace bridge {


// The rule, resolved for ONE episode. Everything a reader needs to re-derive
// the number offline is in here, which is the ck-ijfa property applied to X:
// the value, the fraction it came from, and the horizon it was measured over.
struct ExclusionRule {
    bool        armed = false;
    std::string unarmed_reason = kUnarmedNoHorizon;   // "" when armed

    double      fraction = kDefaultExclusionFraction;

    bool        horizon_declared = false;
    std::string horizon_start;        // VERBATIM as declared, even if unparseable
    std::string horizon_end;
    int64_t     horizon_game_days = 0;

    // round(fraction * horizon_game_days). 0 when unarmed — and `armed` is what
    // a consumer must test, never this field being non-zero.
    int64_t     exclusion_days = 0;
};

// "YYYY.M.D" / "YYYY.MM.DD" -> y/m/d. Strict: three dot-separated non-negative
// integers, a month in 1..12 and a day in range FOR THAT MONTH on the engine's
// calendar. Returns false on anything else — including 1066.02.30, which CK3
// cannot produce and which would otherwise silently become 2 March.
inline bool parse_game_date(const std::string& s, int* out_y, int* out_m, int* out_d) {
    if (s.empty()) return false;
    int part[3] = {0, 0, 0};
    size_t i = 0;
    for (int p = 0; p < 3; ++p) {
        if (p > 0) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
        size_t digits = 0;
        long long v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (digits > 9) return false;                 // absurd, refuse
            v = v * 10 + (s[i] - '0');
            ++i; ++digits;
        }
        if (digits == 0) return false;
        part[p] = static_cast<int>(v);
    }
    if (i != s.size()) return false;                      // trailing junk
    const int y = part[0], m = part[1], d = part[2];
    if (m < 1 || m > ck3::game_date::MONTHS_PER_YEAR) return false;
    if (d < 1 || d > ck3::game_date::days_in_month(m))   return false;
    if (y < 0) return false;
    if (out_y) *out_y = y;
    if (out_m) *out_m = m;
    if (out_d) *out_d = d;
    return true;
}

// Is this a game date at all? The predicate form, for a caller that only wants
// the verdict.
inline bool is_game_date(const std::string& s) {
    return parse_game_date(s, nullptr, nullptr, nullptr);
}

// Game-days between two declared dates, on the ENGINE's calendar. false when
// either date is not one.
inline bool horizon_span_game_days(const std::string& start, const std::string& end,
                                   int64_t* out_days) {
    int sy = 0, sm = 0, sd = 0, ey = 0, em = 0, ed = 0;
    if (!parse_game_date(start, &sy, &sm, &sd)) return false;
    if (!parse_game_date(end,   &ey, &em, &ed)) return false;
    const int64_t a = ck3::game_date::ymd_to_total_days(sy, sm, sd);
    const int64_t b = ck3::game_date::ymd_to_total_days(ey, em, ed);
    if (out_days) *out_days = b - a;
    return true;
}

// A fraction is usable iff it is finite and in (0, 1]. Anything else — a typo,
// a negative, a 5 meant as "5%" — falls back to the published default rather
// than silently re-scaling X by 100x. The caller publishes which it got.
inline bool exclusion_fraction_is_usable(double f) {
    return std::isfinite(f) && f > 0.0 && f <= 1.0;
}

// THE RULE. Pure. `start`/`end` are the DECLARED horizon; empty means "not
// declared", which is a different answer from "declared and unparseable".
inline ExclusionRule resolve_exclusion_rule(const std::string& start,
                                            const std::string& end,
                                            double fraction = kDefaultExclusionFraction) {
    ExclusionRule r;
    r.fraction = exclusion_fraction_is_usable(fraction) ? fraction
                                                        : kDefaultExclusionFraction;
    r.horizon_start = start;
    r.horizon_end   = end;
    r.horizon_declared = !start.empty() && !end.empty();

    if (!r.horizon_declared) {
        r.armed = false;
        r.unarmed_reason = kUnarmedNoHorizon;
        return r;
    }

    int64_t span = 0;
    if (!horizon_span_game_days(start, end, &span) || span <= 0) {
        // Declared, but it is not a horizon: an unparseable endpoint, or one
        // that does not move forward. NOT "no horizon" — somebody meant to
        // declare one and got it wrong, and that is a different repair.
        r.armed = false;
        r.unarmed_reason = kUnarmedBadHorizon;
        r.horizon_game_days = span > 0 ? span : 0;
        return r;
    }

    r.horizon_game_days = span;
    r.exclusion_days = static_cast<int64_t>(
        std::llround(r.fraction * static_cast<double>(span)));
    r.armed = true;
    r.unarmed_reason.clear();
    return r;
}

}  // namespace bridge
