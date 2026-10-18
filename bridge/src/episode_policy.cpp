// episode_policy.cpp — see episode_policy.h (ck-6kvq.10).

#include "episode_policy.h"

#include "exclusion_rule.h"   // ck-2ngg: X is a RULE over the declared horizon

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bridge {

const char* episode_mode_name(EpisodeMode m) {
    switch (m) {
        case EpisodeMode::Experiment: return "experiment";
        case EpisodeMode::FreePlay:   return "free_play";
    }
    return "free_play";
}

namespace {

// Parse a non-negative integer env var; leave `out` alone on anything else, so a
// typo degrades to the documented default rather than to 0 (a 0 min-span would
// silently make every episode invalid — see the header).
void env_i64(const char* name, int64_t& out) {
    const char* v = getenv(name);
    if (!v || !*v) return;
    char* end = nullptr;
    long long parsed = strtoll(v, &end, 10);
    if (end && *end == '\0' && parsed >= 0) out = static_cast<int64_t>(parsed);
}

// Parse a fraction env var. Anything that is not a finite number in (0, 1] is
// REFUSED and the published default stands — a "5" meant as "5%" must not
// silently re-scale X by 100x. `refused_out` receives the raw text ONLY when it
// is refused, so the bridge publishes the refusal instead of swallowing it.
void env_fraction(const char* name, double& out, std::string& refused_out) {
    const char* v = getenv(name);
    if (!v || !*v) return;
    char* end = nullptr;
    const double parsed = strtod(v, &end);
    if (end && *end == '\0' && exclusion_fraction_is_usable(parsed)) {
        out = parsed;
        return;
    }
    refused_out = v;
}

// A declared endpoint of the horizon. Empty/unset stays empty, which is a
// DIFFERENT answer from "declared and unparseable" — see exclusion_rule.h.
std::string env_str(const char* name) {
    const char* v = getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

}  // namespace

void EpisodePolicyHolder::set(const EpisodePolicy& p) {
    std::lock_guard<std::mutex> lk(mu_);
    p_ = p;
    experiment_.store(p.mode == EpisodeMode::Experiment, std::memory_order_relaxed);
}

void EpisodePolicyHolder::set_episode_id(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    p_.episode_id = id;
}

EpisodePolicy EpisodePolicyHolder::get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return p_;
}

bool EpisodePolicyHolder::is_experiment() const {
    return experiment_.load(std::memory_order_relaxed);
}

void EpisodePolicyHolder::init_from_env() {
    EpisodePolicy p;
    const char* m = getenv("CK3_EPISODE_MODE");
    p.mode = (m && strcmp(m, "experiment") == 0) ? EpisodeMode::Experiment
                                                 : EpisodeMode::FreePlay;
    const char* id = getenv("CK3_EPISODE_ID");
    if (id && *id) p.episode_id = id;

    // ck-6kvq.12 (R4 / G5): CK3_EPISODE_ARM, lower-cased. Kept verbatim
    // otherwise — an unrecognised arm is not "b", and the board is therefore
    // absent, which is the conservative failure direction (see the header).
    const char* arm = getenv("CK3_EPISODE_ARM");
    if (arm && *arm) {
        p.arm = arm;
        for (char& ch : p.arm)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }

    // ck-ijfa: CK3_UNAVAIL_MIN_SPAN_DAYS is RETIRED with the game-day gate it
    // configured. The gate is now in WALL-CLOCK and reads this instead.
    env_i64("CK3_UNAVAIL_ANOMALY_WALL_S", p.unavail_anomaly_wall_s);

    // ── ck-2ngg: X IS COMPUTED, NEVER READ ─────────────────────────────────
    // The only knob is the FRACTION. The horizon is DECLARED by the harness at
    // episode start, and with no horizon the exclusion is UNARMED and typed —
    // the bridge does not fall back to 365, because falling back to the rule's
    // answer at somebody else's horizon is the whole defect.
    env_fraction("CK3_UNAVAIL_EXCLUSION_FRACTION", p.unavail_exclusion_fraction,
                 p.refused_exclusion_fraction_env);

    // ── ck-on4u: §9.4's REGISTERED GAME SPEED ──────────────────────────────
    // ONE DECLARATION, THREE READERS. This is the bridge's read; the other two
    // are scripts/registered_launch-side establishment and
    // scripts/episode_validity.py's comparison. A refusal keeps the raw text
    // and lets the pre-registered default stand, because a knob that is
    // silently ignored has told the operator nothing.
    {
        const GameSpeedDeclaration g = resolve_game_speed_from_env();
        p.game_speed              = g.speed;
        p.game_speed_declared     = g.declared;
        p.refused_game_speed_env  = g.refused;
        p.game_speed_present      = g.present;
    }

    const ExclusionRule rule = resolve_exclusion_rule(
        env_str("CK3_EPISODE_HORIZON_START"),
        env_str("CK3_EPISODE_HORIZON_END"),
        p.unavail_exclusion_fraction);

    p.horizon_start_game_date = rule.horizon_start;
    p.horizon_end_game_date   = rule.horizon_end;
    p.horizon_declared        = rule.horizon_declared;
    p.horizon_game_days       = rule.horizon_game_days;
    p.unavail_exclusion_fraction        = rule.fraction;
    p.unavail_exclusion_armed           = rule.armed;
    p.unavail_exclusion_unarmed_reason  = rule.unarmed_reason;
    p.unavail_exclusion_days            = rule.exclusion_days;

    // THE RETIRED OVERRIDE, PUBLISHED AS IGNORED. A retired knob that silently
    // does nothing is a trap: an operator who sets CK3_UNAVAIL_X_DAYS and sees
    // no effect and no complaint has been told nothing at all.
    p.ignored_x_days_env = env_str("CK3_UNAVAIL_X_DAYS");

    set(p);
}

EpisodePolicyHolder& episode_policy() {
    static EpisodePolicyHolder h;
    return h;
}

bool episode_is_experiment() { return episode_policy().is_experiment(); }
bool episode_pins_claims()   { return episode_policy().is_experiment(); }

}  // namespace bridge
