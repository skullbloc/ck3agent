// line_watch.h — the PLAYED LINE, followed by the bridge itself, per tick
// (METERLINE, orchestrator ruling on follow-up 2; owner ruling 2026-11-11).
//
// THE GAP THIS CLOSES. The growth meter samples every 30 game-days; the heir
// handoff (`bridge_init.cpp` `check_controlled_handoff`) re-reads a control
// session's primary title EVERY TICK. A king who gains a higher title and dies
// inside one sample interval is followed by the bridge to the higher title's
// heir and, from a 30-day-old cache, by the meter to the lower title's. So the
// meter does not re-derive the decision: it NAMES the character the line starts
// with once (an additive `played_line: 1` parameter on the existing
// `query_character` read — no new action, no new tool) and the bridge follows
// that line per tick with the SAME decision the handoff uses, seat-independent
// and session-independent (an E0 episode has neither). Every read with the
// parameter returns the watch's state (`emit` in query_handlers.cpp).
//
// ONE DECISION. `successor` below is what `check_controlled_handoff` calls too:
// the dead character's cached primary title's current holder
// (`heir_from_holder`, played_line.h), else the legacy fallback the caller
// supplies (`resolve_controlled_successor`). While he lives, his primary is
// refreshed per tick from `pick_primary` (the caller's `highest_tier_held_title`).
// While no heir resolves, the watch is `successor_pending` and retries every
// tick, as the handoff does.
//
// THE LINE ENDS ONLY ON AN OBSERVED DECISION (owner ruling 2026-11-12: an ended
// played line is the ultimate failure, R = 0, measured — and it must rest on
// the bridge's observed decision, never on a default). `ended` below is true
// only when THREE readings agree that the engine finished the succession and
// gave the line's title to nobody: (1) the dead character's cached primary
// title LOOKED UP and its holder reads the engine's "none" sentinel (-1,
// HOLDER_CHAR_ID; a title that did not look up reads 0 and is NOT this);
// (2) the legacy successor scan found nobody; (3) the dead character himself
// reads as holding NO title any more (the engine's IsRuler, TITLE_REALM_DATA
// null, or a title array of count 0): his titles have left him, so the transfer
// is done, not in flight. Any reading missing (title unreadable, his title
// state unreadable, the primary never read, the title still naming him) stays
// `successor_pending` with its reason. An ended line is TERMINAL: a later holder
// of the title is somebody else's line, not this one's heir. Evidence grade:
// MEDIUM (the transfer-done premise rests on the handoff's own observation that
// a dead ruler's held-title array is emptied when his titles transfer,
// bridge_init.cpp g_controlled_primary_title_cache; no archived episode has an
// ended line, so only a live episode can confirm the engine's order).
//
// Pure state machine + a mutex-guarded registry. No engine read here: the
// caller (bridge_init.cpp `check_line_watches`) does the reads, OUTSIDE the
// registry lock (a sigsetjmp recovery must never leave the lock held).
#pragma once

#include "played_line.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace played_line {

// The successor decision, shared by the session handoff and the line watch.
// `fallback()` is only called when the cached title names no heir.
template <class Fallback>
inline uint32_t successor(uint32_t raw_holder_of_cached, uint32_t dead_id, Fallback fallback) {
    const uint32_t heir = heir_from_holder(raw_holder_of_cached, dead_id);
    return heir ? heir : fallback();
}

enum class WatchStatus { Following, SuccessorPending, LineEnded };
enum class PendingReason { None, NoPrimaryTitle, PrimaryTitleUnheld, HolderIsTheDead };

inline const char* status_name(WatchStatus s) {
    switch (s) {
        case WatchStatus::Following: return "following";
        case WatchStatus::LineEnded: return "line_ended";
        default: return "successor_pending";
    }
}

// The engine's "nobody holds this title" as HOLDER_CHAR_ID carries it (-1), as
// distinct from 0 (`title_raw_holder`: the title did not look up).
inline bool holder_is_none_sentinel(uint32_t raw_holder) {
    return raw_holder != 0 && (raw_holder & kIdMask) == kIdMask;
}

// THE END DECISION (see the header comment). `raw_holder_of_cached` is the
// cached primary's raw holder (0: not looked up), `successor_found` whether
// the shared successor decision named anybody, `dead_titles_held` the dead
// character's held-title count as read now (< 0: unreadable).
inline bool line_ended(uint32_t cached_title, uint32_t raw_holder_of_cached,
                       bool successor_found, int dead_titles_held) {
    return cached_title != 0 && holder_is_none_sentinel(raw_holder_of_cached)
        && !successor_found && dead_titles_held == 0;
}
inline const char* pending_name(PendingReason r) {
    switch (r) {
        case PendingReason::NoPrimaryTitle: return "no_primary_title";
        case PendingReason::PrimaryTitleUnheld: return "primary_title_unheld";
        case PendingReason::HolderIsTheDead: return "holder_is_the_dead";
        default: return "";
    }
}

struct Watch {
    uint32_t start_id = 0;            // the character the line was armed on (the key)
    uint32_t line_id = 0;             // who the line is now
    uint32_t previous_id = 0;         // the character it last passed from (0: never passed)
    uint32_t carrying_title_id = 0;   // the title that carried the last pass (0: the fallback)
    uint32_t primary_title_id = 0;    // the line character's primary, as last read alive
    uint32_t successions = 0;         // passes since armed
    WatchStatus status = WatchStatus::Following;
    PendingReason reason = PendingReason::None;
    std::string armed_date;           // game date the watch was armed
    std::string since_date;           // game date of the last pass (armed_date before any)
    std::string ended_date;           // game date the line was observed ended ("" while not)
};

// ONE TICK for one watch. The caller skips the tick when the line character
// does not look up (the handoff's `continue`), so `alive` is a reading.
//   primary_now      pick_primary over his held titles NOW (0: none readable)
//   holder_raw_of    title id -> raw HOLDER_CHAR_ID (0 when it does not look up)
//   fallback         dead id -> the legacy successor scan (0: none)
//   titles_held_of   dead id -> his held-title count NOW (< 0: unreadable);
//                    asked only when nobody succeeds him
// Returns true when the watch changed. An ended watch never changes again.
template <class HolderRawOf, class Fallback, class TitlesHeldOf>
inline bool tick(Watch& w, bool alive, uint32_t primary_now, HolderRawOf holder_raw_of,
                 Fallback fallback, TitlesHeldOf titles_held_of, const std::string& date) {
    if (w.status == WatchStatus::LineEnded) return false;
    if (alive) {
        bool changed = w.status != WatchStatus::Following;
        if (primary_now && primary_now != w.primary_title_id) {
            w.primary_title_id = primary_now;
            changed = true;
        }
        w.status = WatchStatus::Following;
        w.reason = PendingReason::None;
        return changed;
    }
    const uint32_t dead = w.line_id;
    const uint32_t cached = w.primary_title_id;
    const uint32_t raw = cached ? holder_raw_of(cached) : 0;
    bool via_fallback = false;
    const uint32_t heir = successor(raw, dead, [&] { via_fallback = true; return fallback(dead); });
    if (heir) {
        w.previous_id = dead;
        w.line_id = heir;
        w.carrying_title_id = via_fallback ? 0 : cached;
        // The heir holds the carried title; his own primary is re-read on the
        // next tick he is read alive.
        if (via_fallback) w.primary_title_id = 0;
        w.successions += 1;
        w.since_date = date;
        w.status = WatchStatus::Following;
        w.reason = PendingReason::None;
        return true;
    }
    if (line_ended(cached, raw, false, titles_held_of(dead))) {
        w.status = WatchStatus::LineEnded;
        w.reason = PendingReason::None;
        w.ended_date = date;
        return true;
    }
    const PendingReason why = !cached ? PendingReason::NoPrimaryTitle
        : ((raw & kIdMask) == dead ? PendingReason::HolderIsTheDead
                                   : PendingReason::PrimaryTitleUnheld);
    const bool changed = w.status != WatchStatus::SuccessorPending || w.reason != why;
    w.status = WatchStatus::SuccessorPending;
    w.reason = why;
    return changed;
}

// The armed watches. Bounded: a watch costs a held-title scan per tick, and the
// meter arms one per episode.
constexpr std::size_t kMaxWatches = 8;

class WatchRegistry {
public:
    // Arm a watch on `start` (idempotent). False when the registry is full and
    // `start` is not already armed; `*out` then is untouched.
    bool arm(uint32_t start, const std::string& date, Watch* out, bool* armed_now) {
        std::lock_guard<std::mutex> lk(m_);
        start &= kIdMask;
        auto it = w_.find(start);
        if (it == w_.end()) {
            if (w_.size() >= kMaxWatches) return false;
            Watch w;
            w.start_id = w.line_id = start;
            w.armed_date = w.since_date = date;
            it = w_.emplace(start, w).first;
            if (armed_now) *armed_now = true;
        } else if (armed_now) {
            *armed_now = false;
        }
        if (out) *out = it->second;
        return true;
    }
    // Read one watch without arming it. False when `start` is not armed.
    bool find(uint32_t start, Watch* out) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = w_.find(start & kIdMask);
        if (it == w_.end()) return false;
        if (out) *out = it->second;
        return true;
    }
    std::vector<Watch> snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<Watch> v;
        for (const auto& kv : w_) v.push_back(kv.second);
        return v;
    }
    // Write back a ticked copy, if the watch is still armed (a world reset may
    // have cleared it between the snapshot and now).
    void store(const Watch& w) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = w_.find(w.start_id);
        if (it != w_.end()) it->second = w;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        w_.clear();
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return w_.size();
    }

private:
    mutable std::mutex m_;
    std::map<uint32_t, Watch> w_;
};

inline WatchRegistry& watches() {
    static WatchRegistry r;
    return r;
}

}  // namespace played_line
