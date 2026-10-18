// war_history.cpp — pure half of the ended-war ledger (ck-sd4d).
//
// No engine reads, no globals from the game, no offsets header: everything
// here is testable by bridge/test/test_war_history.cpp with no CK3 process.
// The engine-facing half (the WAR_END_CORE hook and the IPC handler) lives in
// war_history_hook.cpp.

#include "war_history.h"

#include <algorithm>
#include <cstring>

namespace bridge {

const char* war_outcome_name(int raw_outcome) {
    switch (raw_outcome) {
        case static_cast<int>(WarOutcome::AttackerVictory): return "attacker_victory";
        case static_cast<int>(WarOutcome::DefenderVictory): return "defender_victory";
        case static_cast<int>(WarOutcome::WhitePeace):      return "white_peace";
        case static_cast<int>(WarOutcome::Invalidated):     return "invalidated";
        default:                                            return "unknown";
    }
}

WarOutcome war_outcome_from_name(const char* name) {
    if (!name) return WarOutcome::Unknown;
    if (strcmp(name, "attacker_victory") == 0 || strcmp(name, "victory") == 0)
        return WarOutcome::AttackerVictory;
    if (strcmp(name, "defender_victory") == 0 || strcmp(name, "defeat") == 0)
        return WarOutcome::DefenderVictory;
    if (strcmp(name, "white_peace") == 0)  return WarOutcome::WhitePeace;
    if (strcmp(name, "invalidated") == 0)  return WarOutcome::Invalidated;
    return WarOutcome::Unknown;
}

bool filter_is_identifying(const WarHistoryFilter& f) {
    if (f.has_war_id) return true;
    // A CB type alone is not enough (two county_conquest_cb wars can run at
    // once); a CB type plus a named belligerent is, because one character can
    // be the primary attacker of at most one war per CB at a time and the
    // pairing is what distinguishes the two scripted 1066 k_england wars.
    if (f.has_cb_type && (f.has_attacker || f.has_defender)) return true;
    if (f.has_attacker && f.has_defender) return true;
    return false;
}

bool filter_is_title_only(const WarHistoryFilter& f) {
    if (!f.has_target_title) return false;
    return !filter_is_identifying(f);
}

bool war_matches(const WarEndRecord& rec, const WarHistoryFilter& f) {
    if (f.has_war_id && rec.war_id != f.war_id) return false;
    if (f.has_cb_type && rec.cb_type != f.cb_type) return false;
    if (f.has_attacker && rec.attacker_id != f.attacker_id) return false;
    if (f.has_defender && rec.defender_id != f.defender_id) return false;
    if (f.has_target_title) {
        if (std::find(rec.target_titles.begin(), rec.target_titles.end(),
                      f.target_title) == rec.target_titles.end()) {
            return false;
        }
    }
    if (f.since_tick && rec.tick < f.since_tick) return false;
    return true;
}

void WarHistory::record(const WarEndRecord& rec) {
    std::lock_guard<std::mutex> lock(mu_);
    // A war ends exactly once — WAR_END_CORE returns early when IS_ENDED is
    // already set — but a defensive de-dup keeps a double-hooked build (or a
    // reused war id after a save load without a world reset) from writing two
    // rows for one war.
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->war_id == rec.war_id && it->tick == rec.tick) return;
    }
    records_.push_back(rec);
    ++total_;
    if (records_.size() > kMaxRecords) {
        records_.erase(records_.begin(),
                       records_.begin() + (records_.size() - kMaxRecords));
    }
}

std::vector<WarEndRecord> WarHistory::query(const WarHistoryFilter& f) const {
    std::vector<WarEndRecord> out;
    int limit = (f.limit <= 0) ? 64 : f.limit;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (static_cast<int>(out.size()) >= limit) break;
        if (war_matches(*it, f)) out.push_back(*it);
    }
    return out;
}

void WarHistory::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    records_.clear();
    // total_ is deliberately NOT reset: it counts terminations the hook
    // observed in this process, which is the number that tells an operator
    // whether the hook is alive at all.
}

size_t WarHistory::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return records_.size();
}

uint64_t WarHistory::total_recorded() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_;
}

void WarHistory::note_uncaptured() {
    std::lock_guard<std::mutex> lock(mu_);
    ++uncaptured_;
}

uint64_t WarHistory::total_uncaptured() const {
    std::lock_guard<std::mutex> lock(mu_);
    return uncaptured_;
}

WarHistory& war_history() {
    static WarHistory g_war_history;
    return g_war_history;
}

}  // namespace bridge
