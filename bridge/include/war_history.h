// war_history.h — the record of wars that have ENDED (bead ck-sd4d).
//
// ── Why this exists ────────────────────────────────────────────────────────
//
// Every war enumerator in the bridge filters on war_obj::IS_ENDED, and the
// obvious fix — "add an include_ended flag" — cannot work. WAR_END_CORE
// (re/offsets/ck3_offsets.h, funcs::WAR_END_CORE, decompile-verified) sets
// IS_ENDED at its top and then, before returning, runs the CWar destructor,
// memsets all 0x300 bytes of the object to zero and returns its WAR_DB slot to
// the free list. Setting the flag and wiping the object happen inside ONE
// call, so no client — polling at 90 days, at a day, or at a millisecond —
// can ever observe an ended war. Empirically: 0 of the 87 wars in the 1097 AD
// core dump (a 230-game-year campaign) carry IS_ENDED.
//
// A war's OUTCOME and END DATE are therefore not readable state. They are an
// EVENT, and the only place to catch it is inside the engine call that raises
// it. The bridge hooks WAR_END_CORE, copies the war out before the original
// runs, and appends {record, outcome, end date} here. This header is the pure,
// engine-free half so the ledger and — more importantly — the war-IDENTITY
// rule can be unit-tested with no CK3 process.
//
// ── The identity rule ──────────────────────────────────────────────────────
//
// A war is identified by war_id, or by cb_type together with its belligerents.
// NEVER by the title it targets. The scripted 1066 start ships two wars that
// both target k_england — the Norwegian Invasion (29767 -> 30853) and the
// Norman Conquest (31808 -> 30853) — so a target-title match silently follows
// whichever one happens to be in the set. The paper's meter lane hit exactly
// that trap. `match()` below refuses a title-only query with a typed reason
// instead of answering it; see test_war_history.cpp.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

// The 3rd argument of funcs::WAR_END_CORE. See ck3::war_obj::OUTCOME_* in
// re/offsets/ck3_offsets.h for the four-way cross-validation of the mapping.
enum class WarOutcome : int {
    AttackerVictory = 0,
    DefenderVictory = 1,
    WhitePeace      = 2,
    Invalidated     = 3,
    Unknown         = -1,
};

// Stable wire names. Chosen to match the engine's own trigger vocabulary
// ("victory/defeat/white_peace/invalidated") while naming the SIDE explicitly,
// because "victory" alone is ambiguous to a reader of the meter series.
const char* war_outcome_name(int raw_outcome);

// Inverse of war_outcome_name, for clients and tests. Returns Unknown for
// anything unrecognised. Also accepts the engine's bare "victory"/"defeat".
WarOutcome war_outcome_from_name(const char* name);

// One ended war, copied out of the CWar object immediately before the engine
// destroyed it. Dates are carried as an exact day number since the engine
// epoch (ck3::game_date::raw_to_total_days) so this header stays free of the
// calendar and of the offsets header; rendering to "YYYY.MM.DD" happens at the
// emitting edge.
struct WarEndRecord {
    uint32_t war_id          = 0;
    int      outcome         = static_cast<int>(WarOutcome::Unknown);

    // End: the game clock read inside the hook, i.e. the instant the engine
    // terminated the war. This is the only source of an end date that exists.
    int32_t  end_raw_date    = 0;
    int64_t  end_total_days  = 0;
    bool     end_date_known  = false;   // false if GAME_STATE_ROOT was unreadable

    // Start: from war_obj::START_DATE_RAW, which the engine does keep.
    int32_t  start_raw_date  = 0;
    int64_t  start_total_days = 0;
    bool     start_date_known = false;

    uint32_t attacker_id     = 0;       // war_obj::PRIMARY_ATK_REF, 24-bit
    uint32_t defender_id     = 0;       // war_obj::PRIMARY_DEF_REF, 24-bit
    uint32_t claimant_id     = 0xFFFFFFFF;
    std::string cb_type;                // CB type script key, e.g. "norman_conquest_cb"
    std::string war_name;

    std::vector<uint32_t> target_titles;
    std::vector<uint32_t> atk_participants;
    std::vector<uint32_t> def_participants;

    uint64_t tick            = 0;       // bridge tick at termination
    uint64_t timestamp_ms    = 0;       // wall clock, for log correlation

    // The engine field re/offsets called END_DATE_PACKED. Emitted raw so the
    // refutation stays visible instead of being re-guessed. Always 0 so far.
    uint64_t end_date_field_raw = 0;
};

// A query against the ledger. Every set field must match (AND).
struct WarHistoryFilter {
    bool     has_war_id      = false;
    uint32_t war_id          = 0;
    bool     has_cb_type     = false;
    std::string cb_type;
    bool     has_attacker    = false;
    uint32_t attacker_id     = 0;
    bool     has_defender    = false;
    uint32_t defender_id     = 0;
    // Target title is an OUTPUT of this module, and may be used to narrow a
    // query that is ALREADY identified by cb_type/belligerents. It can never
    // identify a war on its own — see `filter_is_identifying`.
    bool     has_target_title = false;
    uint32_t target_title    = 0;

    uint64_t since_tick      = 0;
    int      limit           = 64;
};

// Does this filter pin a war by something that cannot be shared by two
// concurrent wars? True for war_id, or for cb_type plus at least one
// belligerent. False for an empty filter (a plain "list everything" browse,
// which is fine) — use `filter_is_title_only` to catch the dangerous case.
bool filter_is_identifying(const WarHistoryFilter& f);

// The trap this bead exists to close: a caller that names ONLY a target title
// (with or without extra non-identifying hints) and expects one war back.
bool filter_is_title_only(const WarHistoryFilter& f);

bool war_matches(const WarEndRecord& rec, const WarHistoryFilter& f);

// The ledger. Bounded ring — an episode is 20 game-years and the bound only
// has to cover the wars an observer might ask about after the fact; the paper
// asks about one. Oldest records are dropped first.
class WarHistory {
public:
    static constexpr size_t kMaxRecords = 512;

    void record(const WarEndRecord& rec);

    // Newest first, capped by f.limit.
    std::vector<WarEndRecord> query(const WarHistoryFilter& f) const;

    void clear();
    size_t size() const;
    uint64_t total_recorded() const;   // includes records dropped by the bound

    // ck-yifp. A WAR_END_CORE entry the hook saw but could NOT copy out — the
    // CWar pointer was unmapped, carried another vtable, or had a freed slot's
    // id. Counted separately because otherwise such a termination is
    // indistinguishable from one that never happened: the ledger is short a
    // row either way, and "no row" would read as "no end". A consumer that
    // finds no row for a war it watched leave the active set can now ask
    // whether this bridge dropped anything at all.
    //
    // PROCESS-scoped, like total_recorded() and for the same reason: it is a
    // statement about whether the hook is healthy, not about the loaded world,
    // so clear() leaves it alone.
    void note_uncaptured();
    uint64_t total_uncaptured() const;

private:
    mutable std::mutex mu_;
    std::vector<WarEndRecord> records_;   // oldest first
    uint64_t total_ = 0;
    uint64_t uncaptured_ = 0;
};

// The process-wide ledger the hook writes and the query handler reads.
WarHistory& war_history();

}  // namespace bridge
