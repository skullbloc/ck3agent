// siege_view.h — what a siege still needs, and what holds it.
//
// The bridge once read a siege's "progress" at CSiege+0x1b0 (0..20, labelled
// "won" at 20). That field is not the siege's: a CSiege is 0x190 bytes (the destroy path
// FUN_0643ac30 memsets 400 bytes) and the sieges sit in one pool, so +0x1b0 is the NEXT
// pooled siege's +0x20, its siege-event phase length in days (20 with intact walls, 18 / 14
// after a small / large breach; 1097 dump: 41 of 41 sieges with a pooled neighbour read the
// neighbour's +0x20/+0x28, the other 23 read 0). A seat read 20 for months and white-peaced
// three wars on it. re/findings/re_siege_progress_struct.md §10.
//
// The game's own siege, decompiled (image VAs; header offsets = VA - 0x100000):
//   needed   FUN_064363f0 = BASE_DEFENDER_MORALE + DEFENDER_MORALE_PER_FORT_LEVEL x fort level
//            x min(1, garrison now / garrison max)            (fort level = province+0x430)
//   progress CSiege+0x110 (int64, /100000); the daily tick adds the day's progress to it
//   per day  FUN_06436800 = (1 + assault + men-at-arms siege value + 0.01 per 200 men above
//            the garrison + disease) x modifiers x 0.7 per missing siege tier (FUN_06439b30:
//            fort level at or above 4 / 6 / 11 / 16 / 31 needs siege tier 1 / 2 / 3 / 4 / 5),
//            never below MIN_DAILY_MORALE_LOSS
//   blocked  FUN_064365d0 / FUN_064384f0: no progress while the men of the armies that can
//            besiege (on the holding, not moving, not in battle) are fewer than the garrison
//            or there is no such army; the tick writes that verdict to CSiege+0x18
//   falls    when progress reaches needed (siege events add to it; on_siege_completion)
//
// Engine-free so a host test reaches the verdict (bridge/test/test_siege_view.cpp); the
// reads are in siege_read.cpp.
#pragma once

#include <cstdint>

namespace siege_view {

// NSiege, game/common/defines/00_defines.txt (vanilla); the code reads them from its define
// globals (FUN_064363f0 _DAT_0c3abbf8 / _DAT_0c3abc10, FUN_06436800 _DAT_0c3abc28 /
// _DAT_0c3abc40 / _DAT_0c3abc58 / _DAT_0c3abc70, FUN_06439b30 _DAT_0c3abcb8 / _DAT_0c3abce0).
inline constexpr double kBaseDefenderMorale = 100.0;   // BASE_DEFENDER_MORALE
inline constexpr double kMoralePerFortLevel = 75.0;    // DEFENDER_MORALE_PER_FORT_LEVEL
inline constexpr double kBaseDaily = 1.0;              // BASE_DAILY_MORALE_LOSS
inline constexpr double kMinDaily = 0.5;               // MIN_DAILY_MORALE_LOSS
inline constexpr double kPerMenStack = 0.01;           // DAILY_MORALE_LOSS_FROM_RELATIVE_ATTACKER_MEN
inline constexpr int    kMenStack = 200;               // RELATIVE_ATTACKER_MEN_STACK_SIZE
inline constexpr double kTierImpact = 0.7;             // SIEGE_FORT_THRESHOLD_IMPACT
inline constexpr int    kFortThresholds[5] = {4, 6, 11, 16, 31};   // SIEGE_FORT_THRESHOLDS

// What siege_read.cpp read off one siege. -1 = not read.
struct Reads {
    bool   ticked = false;        // the game has evaluated it (phase length > 0): not on a siege opened this day, nor on the day a save loads (a live siege test: 3/3)
    bool   can_progress = false;  // CSiege+0x18, the game's own daily verdict
    double progress = 0;          // CSiege+0x110 / 100000
    int    fort_level = -1;       // the besieged province's +0x430
    int    garrison = -1;         // the holding's garrison regiment, men now
    int    men = -1;              // men of the armies on the holding that can besiege
    int    men_in_battle = 0;     // men on the holding in a battle
    int    men_moving = 0;        // men on the holding with a path (marching on)
    int    siege_tier = 0;        // best siege tier among the armies that can besiege (0 = no siege weapons)
    int    breach = 0;            // walls: 0 intact, 1 small breach, 2 large breach (CSiege+0x118)
    double phase_days = 0;        // days between siege events (CSiege+0x20 / 100000)
    int    day_in_phase = 0;      // CSiege+0x17c
    bool   assault = false;       // CSiege+0x18c
};

// The progress a holding of this fort level needs with a full garrison (a thinner garrison needs less).
inline double needed_max(int fort_level) {
    return kBaseDefenderMorale + kMoralePerFortLevel * (fort_level > 0 ? fort_level : 0);
}

// The siege tier at which this fort level no longer slows the siege (FUN_06439b30).
inline int tier_needed(int fort_level) {
    int n = 0;
    for (int t : kFortThresholds) if (fort_level >= t) ++n;
    return n;
}

// The slowdown factor: 0.7 for every threshold the army's best siege tier does not reach.
inline double fort_speed(int fort_level, int siege_tier) {
    double m = 1.0;
    for (int i = 0; i < 5; ++i) {
        if (fort_level < kFortThresholds[i]) break;
        if (siege_tier <= i) m *= kTierImpact;
    }
    return m;
}

// The day's progress at the base rate: the base, the men above the garrison, the fort slowdown,
// the floor. Not counted: men-at-arms siege values, modifiers, disease, an assault (each adds).
inline double daily_at_base(int men, int garrison, int fort_level, int siege_tier) {
    int extra = (men > 0 && garrison >= 0 && men > garrison) ? (men - garrison) / kMenStack : 0;
    double d = (kBaseDaily + kPerMenStack * extra) * fort_speed(fort_level, siege_tier);
    return d < kMinDaily ? kMinDaily : d;
}

struct Verdict {
    const char* status = "unread";   // starting | progressing | blocked | unread
    const char* blocker = nullptr;   // garrison_larger | in_battle | moving | no_besieger | not_read
    double needed_max = -1;          // < 0: the fort level was not read
    double pct = -1;                 // progress / needed_max x 100, capped at 100
    int    tier_needed = -1;
    double speed = -1;               // fort_speed
    int    eta_days = -1;            // at the base rate; -1 when blocked or not read
    int    next_event_days = -1;     // days to the next siege event
};

inline Verdict judge(const Reads& s) {
    Verdict v;
    if (s.fort_level >= 0) {
        v.needed_max = needed_max(s.fort_level);
        v.tier_needed = tier_needed(s.fort_level);
        v.speed = fort_speed(s.fort_level, s.siege_tier);
        double p = s.progress / v.needed_max * 100.0;
        v.pct = p < 0 ? 0 : (p > 100 ? 100 : p);
    }
    if (!s.ticked) { v.status = "starting"; return v; }
    if (s.phase_days > 0) {
        int left = static_cast<int>(s.phase_days + 0.999) - s.day_in_phase;
        v.next_event_days = left < 0 ? 0 : left;
    }
    if (!s.can_progress) {
        v.status = "blocked";
        if (s.men > 0 && s.garrison >= 0 && s.men < s.garrison) v.blocker = "garrison_larger";
        else if (s.men_in_battle > 0) v.blocker = "in_battle";
        else if (s.men_moving > 0) v.blocker = "moving";
        else if (s.men == 0) v.blocker = "no_besieger";
        else v.blocker = "not_read";
        return v;
    }
    v.status = "progressing";
    if (v.needed_max > 0 && s.men >= 0 && s.garrison >= 0) {
        double left = v.needed_max - s.progress;
        double d = daily_at_base(s.men, s.garrison, s.fort_level, s.siege_tier);
        v.eta_days = left <= 0 ? 0 : static_cast<int>(left / d + 0.999);
    }
    return v;
}

}  // namespace siege_view
