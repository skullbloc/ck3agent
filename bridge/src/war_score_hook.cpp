// war_score_hook.cpp — Post-hook on WAR_SCORE_UPDATE to cache war score breakdown.
//
// Architecture: WAR_SCORE_UPDATE is called per-war during the war update cycle.
// We install an inline hook that calls the original function first (trampoline),
// then computes the per-component score breakdown and caches it keyed by war_id.
// Four components (imprisonment, battle, occupation, landless armies lost) call
// the game's sub-functions; TickingScore is derived from raw memory (ck-nr51.3).
// The IPC handler reads the cache under a shared_mutex read lock.
//
// The sub-functions are only safe to call from within the war update tick context,
// which is why we use this post-hook pattern rather than calling them on demand.
//
// Safety: sub-functions (via FUN_066b4f40) iterate the war's participant arrays
// at war+0x28 (atk) and war+0x58 (def) and dereference each entry to read its
// +0x08 char_id field. A dangling slot containing sentinel 0x1 crashes the
// iteration. We pre-validate every participant pointer before invoking sub-
// functions; the sigsetjmp block below is a last-resort belt-and-suspenders
// that should never fire during normal gameplay.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "world_epoch.h"   // ck-hxh7

#include <shared_mutex>
#include <unordered_map>

namespace bridge {

// =============================================================================
// War Score Sub-function Types
// =============================================================================

// Per-side functions: int fn(void* war_ptr, int side_index, void* tooltip_ctx)
//   side: 0=attacker, 1=defender.  tooltip: nullptr for no tooltip.
using fn_war_score_sided = int(*)(void*, int, void*);

// Net-score functions: int fn(void* war_ptr, void* tooltip_ctx)
//   No side parameter — function computes both sides internally and returns net score.
//   CRITICAL: Do NOT pass side_index — it would be misinterpreted as tooltip_ctx pointer.
using fn_war_score_net = int(*)(void*, void*);

static fn_war_score_sided g_fn_imprisonment = nullptr;  // 3-param: per-side
static fn_war_score_net   g_fn_battle       = nullptr;  // 2-param: net score
static fn_war_score_sided g_fn_occupation   = nullptr;  // 3-param: per-side
static fn_war_score_net   g_fn_landless     = nullptr;  // 2-param: net score (attacker only)
// g_fn_ticking removed (ck-nr51.3): TickingScore is now derived from raw memory
// reads — see compute_ticking_score_breakdown() below.

// =============================================================================
// Cached Breakdown (struct defined in bridge.h)
// =============================================================================

static std::shared_mutex g_war_score_cache_mutex;
static std::unordered_map<uint32_t, WarScoreBreakdown> g_war_score_cache CK3_EARLY_INIT;

// Why the hook last declined to refresh a war's breakdown (ck-xwlg). A stale
// cache entry with a recorded reject reason is the tell that pre-validation —
// not the game — is starving the cache; surfacing the reason from the query
// side turns "war_score stuck at 0" from a mystery into a pointer at the
// failing validator.
static std::unordered_map<uint32_t, WarScoreRejectReason> g_war_score_reject CK3_EARLY_INIT;

// ck-hxh7: both maps are keyed by WAR ID, and war ids are reused across saves.
// Their lifetime is the loaded WORLD, not the process — nothing ever erased from
// g_war_score_cache (not even on war end), so a reused war id in a newly loaded
// save served the previous world's score until it happened to be re-scored.
// Purely derived data: a spurious clear costs one recompute.
static void clear_war_score_cache() {
    std::unique_lock<std::shared_mutex> lock(g_war_score_cache_mutex);
    g_war_score_cache.clear();
    g_war_score_reject.clear();
}

[[maybe_unused]] static const WorldResetRegistrar g_war_score_world_scoped CK3_EARLY_INIT([] {
    register_world_reset("war_score_cache", &clear_war_score_cache);
});

static void record_reject(uint32_t war_id, WarScoreRejectReason why) {
    std::unique_lock<std::shared_mutex> lock(g_war_score_cache_mutex);
    g_war_score_reject[war_id] = why;
}

const char* war_score_reject_name(WarScoreRejectReason r) {
    switch (r) {
        case WarScoreRejectReason::FnUnresolved:     return "fn_unresolved";
        case WarScoreRejectReason::CbSubobj:         return "cb_subobj";
        case WarScoreRejectReason::BattleList:       return "battle_list";
        case WarScoreRejectReason::AtkParticipants:  return "atk_participants";
        case WarScoreRejectReason::DefParticipants:  return "def_participants";
        case WarScoreRejectReason::OccupationChain:  return "occupation_chain";
        case WarScoreRejectReason::LandlessRealm:    return "landless_realm";
        default:                                     return "none";
    }
}

// Trampoline to original WAR_SCORE_UPDATE
static void* g_orig_war_score_update = nullptr;

// =============================================================================
// Per-war pre-validation
// =============================================================================
//
// Every gate below is "primary defense" — sigsetjmp around the sub-function
// calls remains as belt-and-suspenders, but pre-validation must keep that
// recovery path silent in normal play. If you see "UNEXPECTED SIGSEGV" in the
// bridge log, extend this validator before doing anything else.
//
// What each sub-function touches (from re_war_score_subfunctions.md +
// re_war_score_cache.md decomp evidence):
//
//   ImprisonmentScore  — iterates participants via FUN_066b4f40, dereferences
//                        each entry to read +0x08 (char_id). Sentinel slot ⇒
//                        deref *(0x1 + 8) = *0x9 ⇒ SIGSEGV. Defended by
//                        side_participants_valid().
//   OccupationScore    — same FUN_066b4f40 iteration, plus walks the
//                        CharacterDB → realm → held-titles → LandedTitleDB
//                        chain for each participant. Defended by
//                        occupation_chain_valid() (ck-nr51.1): validates the
//                        DB singletons + shadow-resolves each participant's
//                        char_id to confirm the chain won't crash mid-walk.
//                        The function has no inline occupied-counties
//                        container — all county data is external (DB-lookup).
//   BattleScore        — iterates BATTLE_LIST_PTR (war+0x238) for BATTLE_COUNT
//                        (war+0x244) entries. Sentinel/garbage ptr ⇒ SIGSEGV.
//                        Defended by battle_list_valid().
//   TickingScore       — direct memory read (ck-nr51.3). reads war+0x40/0x48
//                        (atk score + enabled) and war+0x70/0x78 (def score +
//                        enabled). No function call; computed inline by
//                        compute_ticking_score_breakdown(). Per-side floor
//                        to whole-percentage matches FUN_066b5460 magic-div
//                        math; output is in fp/1000 to match other sub-
//                        functions in the breakdown sum.
//   LandlessScore      — resolves the CB primary attacker (war+0x228) via
//                        CharacterDB, walks the attacker's title_realm_data
//                        (char+0x1B8), and iterates the MAA_REGIMENT_REFS
//                        CPdxArray at title_realm+0x278, looking up each
//                        regiment id in OWNER_LINK_DB. Defended by
//                        landless_realm_valid() (ck-nr51.2): validates the
//                        OWNER_LINK_DB singleton, the resolved attacker's
//                        title_realm_data, and the MAA refs array. Same
//                        shadow-resolve pattern as occupation_chain_valid.
//
// All four remaining sub-functions also dereference the CB sub-object embedded
// at war+0x98 (CCasusBelli::vftable). cb_subobj_valid() guards that. (Direct-
// read TickingScore does not touch the CB sub-object.)

// Validate one CWarSide's participant array. Each slot must point to a mapped
// CWarParticipant struct; sentinel 0x1 (dangling/freed participant) would
// crash FUN_066b4f40 (called by ImprisonmentScore + OccupationScore).
static bool side_participants_valid(uintptr_t side_ptr) {
    uintptr_t data = safe_read<uintptr_t>(side_ptr + 0x08);   // ATK/DEF_PARTICIPANT
    uint32_t  cap  = safe_read<uint32_t>(side_ptr + 0x10);    // capacity
    int32_t   cnt  = safe_read<int32_t>(side_ptr + 0x14);     // count

    if (cnt < 0 || cnt > 256)                return false;    // sanity: count bound
    if (static_cast<uint32_t>(cnt) > cap)    return false;    // sanity: count <= cap
    if (cnt == 0)                            return true;     // empty array is fine
    if (!is_mapped_range(data, cnt * 8))     return false;    // array data mapped

    for (int32_t i = 0; i < cnt; ++i) {
        uintptr_t p = safe_read<uintptr_t>(data + i * 8);
        if (p < 0x10000 || !is_mapped(p)) return false;
        // Participant vtable sanity: +0x00 must be mapped (deref'd as vtable by
        // game code; non-mapped vtable means freed/dangling participant).
        if (!is_mapped(safe_read<uintptr_t>(p))) return false;
    }
    return true;
}

// Validate the embedded CCasusBelli sub-object. Sub-functions read the CB
// vtable (war+0x98) and CB_TYPE_REF (war+0xA0). A war whose CB sub-object is
// half-constructed has a sentinel/null vtable pointer; calling sub-functions
// dereferences the vtable and crashes.
static bool cb_subobj_valid(uintptr_t wp) {
    uintptr_t cb_vt = safe_read<uintptr_t>(wp + ck3::war_obj::CB_SUBOBJ);
    if (cb_vt < 0x10000 || !is_mapped(cb_vt))      return false;
    uintptr_t cb_type = safe_read<uintptr_t>(wp + ck3::war_obj::CB_TYPE_REF);
    if (cb_type < 0x10000 || !is_mapped(cb_type))  return false;
    return true;
}

// Validate the battle list array. BattleScore iterates BATTLE_LIST_PTR for
// BATTLE_COUNT entries; a sentinel ptr or absurd count blows up the loop.
// Empty list (count=0) is normal for new wars and is allowed.
static bool battle_list_valid(uintptr_t wp) {
    int32_t count = safe_read<int32_t>(wp + ck3::war_obj::BATTLE_COUNT);
    if (count < 0 || count > 4096) return false;
    if (count == 0) return true;
    uintptr_t list = safe_read<uintptr_t>(wp + ck3::war_obj::BATTLE_LIST_PTR);
    if (list < 0x10000 || !is_mapped(list)) return false;
    return true;
}

// Validate the dependency chain OccupationScore walks (ck-nr51.1).
//
// OccupationScore (FUN_066d95a0) decomp shows it has NO inline occupied-counties
// container on CWar. Instead, for each participant it:
//   1. Reads char_id at participant+0x08
//   2. Resolves the character via _g_CharacterDB_root (falls back to
//      _g_NullCharacter_sentinel if root is null OR the slot is unoccupied OR
//      the gen-id at char+0x18 doesn't match)
//   3. Calls maybe_get_realm_step(character) → reads character's realm chain
//   4. Calls FUN_06879500(character) → walks character's held-titles list,
//      looking up each title in _g_LandedTitleDB (similar slot-table pattern)
//   5. For each title, calls FUN_066dab20 → reads the title's holder_char_id
//      and re-resolves through CharacterDB
//
// The function tolerates a *null* DB root (uses NullCharacter sentinel) but
// does NOT validate that a non-null root is mapped. A sentinel non-null value
// (0x1 from a freed-during-load race, or stack-garbage during a hot reload)
// reaches '*(uint *)(root + 0x2c)' as the first read inside the if-branch and
// segfaults. Same trap applies to LandedTitleDB inside FUN_06879500.
//
// Sources: decompilation of FUN_066d95a0, FUN_06879500, FUN_066dab20 against
// CK3 binary BuildID e61ff264... Cross-validated against ck3_20260404 core
// dump: 11 wars, all participants resolved cleanly (slot_count=65536 chars,
// 32768 titles; per-character vtable=0x55f76d9507c8 mapped).
static bool occupation_chain_valid(uintptr_t wp) {
    // Slot-table layout shared by all TPdxRefDatabase<T,8> singletons (see
    // ck3::war_db for the canonical definition; CharacterDB / LandedTitleDB
    // use the same shape: +0x20 = slot-array ptr, +0x2C = slot count,
    // 16-byte stride, slot[+0x08] = T*).
    constexpr size_t DB_SLOT_TABLE_OFF   = 0x20;
    constexpr size_t DB_SLOT_COUNT_OFF   = 0x2C;
    constexpr size_t DB_SLOT_STRIDE      = 0x10;
    constexpr size_t DB_SLOT_OBJ_OFF     = 0x08;
    constexpr uint32_t DB_SLOT_INDEX_MASK = 0xFFFFFF;  // low 24 bits of T_id

    // Both DBs allow null-root (function uses NullSentinel fallback). If non-
    // null they must be mapped + have a sane slot table + bounded slot count.
    // The empirical caps are 64K (chars) and 32K (titles) in 867 AD; pick
    // 16M as a generous catastrophic-corruption bound that still rejects
    // garbage (e.g. uninitialised stack reading as ~0xFFFFFFFF).
    constexpr uint32_t CHAR_DB_MAX_SLOTS  = 16'000'000;
    constexpr uint32_t TITLE_DB_MAX_SLOTS = 16'000'000;

    auto db_valid = [&](uintptr_t global_off, uint32_t max_slots) -> bool {
        uintptr_t db = safe_read_offset<uintptr_t>(global_off);
        if (db == 0) return true;            // null root → NullSentinel fallback path
        if (!is_mapped(db)) return false;
        uintptr_t slot_table = safe_read<uintptr_t>(db + DB_SLOT_TABLE_OFF);
        if (slot_table < 0x10000 || !is_mapped(slot_table)) return false;
        uint32_t slot_count = safe_read<uint32_t>(db + DB_SLOT_COUNT_OFF);
        if (slot_count > max_slots) return false;
        return true;
    };

    if (!db_valid(ck3::globals::CHARACTER_DB, CHAR_DB_MAX_SLOTS)) return false;
    if (!db_valid(ck3::globals::LANDED_TITLE_DB, TITLE_DB_MAX_SLOTS)) return false;

    // Per-participant deep validation: shadow the CharacterDB lookup the
    // function performs and verify each participant resolves to either
    //   (a) the NullCharacter fallback path (slot missing or gen-id mismatch),
    //       which is by-design safe inside FUN_066d95a0, or
    //   (b) a CCharacter with a mapped vtable.
    //
    // side_participants_valid already validated each participant ptr + its
    // vtable; this catches the next-layer trap where the participant is
    // well-formed but its CharacterDB slot was freed mid-tick and re-allocated
    // to a half-constructed character (mapped slot, garbage vtable).
    uintptr_t char_db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (char_db != 0) {
        uintptr_t slot_table = safe_read<uintptr_t>(char_db + DB_SLOT_TABLE_OFF);
        uint32_t slot_count  = safe_read<uint32_t>(char_db + DB_SLOT_COUNT_OFF);

        for (size_t side_off : { ck3::war_obj::ATK_SIDE_VTABLE,
                                 ck3::war_obj::DEF_SIDE_VTABLE }) {
            uintptr_t side_ptr = wp + side_off;
            uintptr_t data = safe_read<uintptr_t>(side_ptr + 0x08);
            int32_t   cnt  = safe_read<int32_t>(side_ptr + 0x14);
            if (cnt <= 0) continue;
            // side_participants_valid already bounded cnt and mapped data.

            for (int32_t i = 0; i < cnt; ++i) {
                uintptr_t p = safe_read<uintptr_t>(data + i * 8);
                if (p < 0x10000) return false;
                uint32_t char_id = safe_read<uint32_t>(p + ck3::war_participant::CHAR_ID);
                uint32_t slot_idx = char_id & DB_SLOT_INDEX_MASK;
                if (slot_idx >= slot_count) continue;  // NullChar fallback path — safe
                uintptr_t char_ptr = safe_read<uintptr_t>(
                    slot_table + DB_SLOT_OBJ_OFF + (uintptr_t)slot_idx * DB_SLOT_STRIDE);
                if (char_ptr == 0) continue;           // NullChar fallback path — safe
                if (!is_mapped(char_ptr)) return false;
                // Generation-id mismatch → NullChar fallback path (also safe).
                uint32_t gen_id = safe_read<uint32_t>(char_ptr + ck3::character::ID);
                if (gen_id != char_id) continue;
                // Resolved character; FUN_066d95a0 will deref char_ptr->vtable
                // (implicit in maybe_get_realm_step / FUN_06879500). Validate it.
                uintptr_t vt = safe_read<uintptr_t>(char_ptr);
                if (vt < 0x10000 || !is_mapped(vt)) return false;
            }
        }
    }
    return true;
}

// Validate the dependency chain LandlessScore walks (ck-nr51.2).
//
// LandlessScore (FUN_066d9080, called via thunk FUN_066d9440) decompilation:
//   1. Early-return 0 for side==1 (function only applies to attacker).
//   2. Resolve attacker char_id from war+0x228 (PRIMARY_ATK_REF) via
//      CharacterDB; fall back to NullCharacter sentinel on miss/mismatch.
//   3. Test "is landless" predicate: reads char+0x1B8 (title_realm_data) and,
//      if non-null, reads title_realm_data+0x1F8 (CAPITAL_BARONY_ID).
//      Landless iff CAPITAL_BARONY_ID == 0xFFFFFFFF.
//   4. Check CB flag: *(byte*)(*(war+0xA0) + 0x1702) & 4 — already guarded
//      by cb_subobj_valid().
//   5. Get the MAA-refs CPdxArray via fn(char): returns &(title_realm+0x278)
//      if title_realm is non-null, else returns a global empty-container
//      sentinel (&_DAT_0c4d6b38, mapped image data — safe by construction).
//   6. Iterate the CPdxArray<uint32> (data ptr at +0x00, count at +0x0C). For
//      each regiment id, look it up in OWNER_LINK_DB (raw 0x0bed76c8). The
//      function falls back to OWNER_LINK_NULL when the DB is null/miss; the
//      resolved object's +0x14 "is_zero" predicate gates the penalty.
//   7. Return -1000 (fp/1000 = -1%) iff landless AND CB has landless flag
//      AND iteration reached the end without the predicate triggering.
//
// Crash surface vs. the current pre-validators:
//   - CB sub-object (war+0xA0) reads at +0x1702 — already covered by
//     cb_subobj_valid().
//   - CharacterDB structural sanity (slot table, slot count) — already
//     covered by occupation_chain_valid()'s db_valid().
//   - GAPS this validator closes (left exposed by ck-nr51.1):
//       (a) OWNER_LINK_DB structural sanity (independent global singleton —
//           not validated by occupation_chain_valid).
//       (b) The CB primary-attacker char's CharacterDB slot may not be in
//           the participant array (sub-functions read CB attacker refs, not
//           participant refs), so the per-participant shadow-resolve in
//           occupation_chain_valid does not cover it.
//       (c) Resolved attacker's title_realm_data ptr — sentinel/freed value
//           crashes the +0x1F8 and +0x278 reads.
//       (d) MAA_REGIMENT_REFS CPdxArray sanity — count bound + data ptr
//           mapping; a garbage count or unmapped data ptr crashes the loop.
//       (e) OWNER_LINK_DB slot table contents — sentinel 0x1 in a slot
//           (analogous to the participant trap) crashes the +0x10 verify
//           read inside the DB resolve.
//
// Sources: decompilation of FUN_066d9080 (LandlessScore_impl) and the helper
// thunks fn_0x0674f880 (is_landless predicate) + fn_0x06778540 (MAA-refs
// accessor) + fn_0x066826e0 (per-link predicate); cross-validated against
// ck3_20260404 core dump (867 AD), 11 active wars accept; six synthetic
// corruption injections (OWNER_LINK_DB unmapped, OWNER_LINK_DB.slot_count
// overflow, title_realm sentinel 0x1, MAA count overflow, MAA data ptr
// sentinel, attacker char_ptr unmapped) all reject.
static bool landless_realm_valid(uintptr_t wp) {
    // Same slot-table layout shared by all TPdxRefDatabase<T,8> singletons.
    constexpr size_t DB_SLOT_TABLE_OFF   = 0x20;
    constexpr size_t DB_SLOT_COUNT_OFF   = 0x2C;
    constexpr size_t DB_SLOT_STRIDE      = 0x10;
    constexpr size_t DB_SLOT_OBJ_OFF     = 0x08;
    constexpr uint32_t DB_SLOT_INDEX_MASK = 0xFFFFFF;
    // OWNER_LINK_DB observed at 1024 slots in 867 AD; 16M matches the
    // catastrophic-corruption bound chosen for the other DBs.
    constexpr uint32_t OWNER_LINK_MAX_SLOTS = 16'000'000;
    // MAA refs per ruler observed at 1-6 in 867 AD across all 11 wars; the
    // theoretical max is bounded by holding count (typically << 50). 4096
    // is the same bound used for the battle list — generous-but-rejects-garbage.
    constexpr int32_t MAA_REFS_MAX_COUNT = 4096;
    // title_realm+0x278 = MAA_REGIMENT_REFS CPdxArray base (data at +0x00,
    // count at +0x0C — matches ck3::title_realm::MAA_REGIMENT_REFS). The
    // is_landless predicate's read at title_realm+0x1F8 (CAPITAL_BARONY_ID)
    // is implicitly covered by the title_realm mapping check below.
    constexpr size_t TITLE_REALM_MAA_REFS = 0x278;
    constexpr size_t CPDXARRAY_COUNT_OFF  = 0x0C;

    // (a) OWNER_LINK_DB null OR mapped + sane slot table + bounded slot count.
    {
        uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::OWNER_LINK_DB);
        if (db != 0) {
            if (!is_mapped(db)) return false;
            uintptr_t slot_table = safe_read<uintptr_t>(db + DB_SLOT_TABLE_OFF);
            if (slot_table < 0x10000 || !is_mapped(slot_table)) return false;
            uint32_t slot_count = safe_read<uint32_t>(db + DB_SLOT_COUNT_OFF);
            if (slot_count > OWNER_LINK_MAX_SLOTS) return false;
        }
        // db == 0 → function uses OWNER_LINK_NULL sentinel — safe fallback path.
    }

    // (b) Resolve CB primary attacker via CharacterDB. Same shadow-resolve
    // pattern as occupation_chain_valid; null-fallback cases (db null, slot
    // out of range, slot zero, gen-id mismatch) are all safe inside the
    // game function.
    uintptr_t char_db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (char_db == 0) return true;  // NullChar fallback — safe

    uintptr_t slot_table = safe_read<uintptr_t>(char_db + DB_SLOT_TABLE_OFF);
    uint32_t  slot_count = safe_read<uint32_t>(char_db + DB_SLOT_COUNT_OFF);

    uint32_t atk_char_id = safe_read<uint32_t>(wp + ck3::war_obj::PRIMARY_ATK_REF);
    uint32_t slot_idx    = atk_char_id & DB_SLOT_INDEX_MASK;
    if (slot_idx >= slot_count) return true;  // NullChar fallback — safe

    uintptr_t char_ptr = safe_read<uintptr_t>(
        slot_table + DB_SLOT_OBJ_OFF + (uintptr_t)slot_idx * DB_SLOT_STRIDE);
    if (char_ptr == 0) return true;            // NullChar fallback — safe
    if (!is_mapped(char_ptr)) return false;

    uint32_t gen_id = safe_read<uint32_t>(char_ptr + ck3::character::ID);
    if (gen_id != atk_char_id) return true;    // NullChar fallback — safe

    // (c) Validate the resolved attacker's title_realm_data ptr — the chain
    // root the function dereferences for both is_landless and MAA-refs.
    uintptr_t title_realm = safe_read<uintptr_t>(
        char_ptr + ck3::character::TITLE_REALM_DATA);
    if (title_realm == 0) return true;   // unlanded — function returns 0 early
    if (!is_mapped(title_realm)) return false;

    // (d) MAA_REGIMENT_REFS CPdxArray sanity. The +0x1F8 read for the
    // is_landless predicate is implicitly covered by is_mapped(title_realm)
    // (struct is ≥0x500 bytes for any populated title_realm).
    uintptr_t maa_arr   = title_realm + TITLE_REALM_MAA_REFS;
    int32_t   maa_count = safe_read<int32_t>(maa_arr + CPDXARRAY_COUNT_OFF);
    if (maa_count < 0 || maa_count > MAA_REFS_MAX_COUNT) return false;
    if (maa_count == 0) return true;

    uintptr_t maa_data = safe_read<uintptr_t>(maa_arr);
    if (maa_data < 0x10000 || !is_mapped(maa_data)) return false;
    // Ensure the last element is also in mapped memory (rejects truncated map).
    if (!is_mapped(maa_data + (uintptr_t)(maa_count - 1) * 4)) return false;

    // (e) Per-MaA-ref shadow validation in OWNER_LINK_DB — catch sentinel-0x1
    // slot entries the game function would crash on at slot+0x10. Cases that
    // fall through to OWNER_LINK_NULL inside the function (slot out of range,
    // slot zero, id-verifier mismatch) are accepted here because the fallback
    // is safe.
    uintptr_t owner_db = safe_read_offset<uintptr_t>(ck3::globals::OWNER_LINK_DB);
    if (owner_db != 0) {
        uintptr_t owner_st = safe_read<uintptr_t>(owner_db + DB_SLOT_TABLE_OFF);
        uint32_t  owner_sc = safe_read<uint32_t>(owner_db + DB_SLOT_COUNT_OFF);
        for (int32_t i = 0; i < maa_count; ++i) {
            uint32_t oid = safe_read<uint32_t>(maa_data + (uintptr_t)i * 4);
            uint32_t oidx = oid & DB_SLOT_INDEX_MASK;
            if (oidx >= owner_sc) continue;     // NullSentinel fallback — safe
            uintptr_t op = safe_read<uintptr_t>(
                owner_st + DB_SLOT_OBJ_OFF + (uintptr_t)oidx * DB_SLOT_STRIDE);
            if (op == 0) continue;              // NullSentinel fallback — safe
            if (op < 0x10000 || !is_mapped(op)) return false;
            // Verifier-id mismatch → NullSentinel fallback (also safe).
        }
    }
    return true;
}

// =============================================================================
// TickingScore: direct memory read (ck-nr51.3)
// =============================================================================
//
// Replaces the FUN_066da7b0 function call — drops one of the five sub-functions
// entirely as a SIGSEGV source and stops paying the per-tick function-call cost.
//
// Function-chain decomp (verified against R20/R24 trace exports and
// re/findings/re_war_score_cache.md):
//
//   FUN_066b5460(side_ptr):
//     if (*(byte*)(side+0x28) != 0)
//         return ((*(int64*)(side+0x20)) / 100000) * 100000;   // fp/100k, sub-% truncated
//     return 0;
//
//   FUN_066da6e0(war, side, tt):
//     return (int)FUN_066b5460(war + (side==0 ? 0x20 : 0x50));
//
//   FUN_066da7b0(war, tt):
//     return FUN_066da6e0(war, 0, tt) - FUN_066da6e0(war, 1, tt);
//
// The raw int64 accumulators live INLINE in the CWar struct (not at side+0x20
// dereferenced — the side IS embedded at war+0x20/+0x50, so side+0x20 is
// war+0x40 for atk and war+0x70 for def). Both accumulators are fp/100000
// (100000 = 1%).
//
// Unit conversion (fp/100000 → fp/1000): each side is first floored to whole-
// percentage precision (matches FUN_066b5460's magic-div * 100000 pattern),
// then multiplied by 1000 instead of 100000. The breakdown sum below clamps
// in fp/1000 (default ±100000 = ±100%) and divides by 1000 to get the final
// percentage, so the ticking term must be in the same fp/1000 unit as the
// other sub-function returns.
//
// Verified empirically across 11 wars (ck3_20260404_093841.core, 867 AD,
// Tuscany start) and 30 wars (ck3_20260421_195552.core, 1097 AD, multi-realm
// observer save). All enabled-byte combinations and signs covered;
// per-side outputs match `(raw / 100000) * 1000` exactly.
//
// Crash-safety: safe_read<T> uses process_vm_readv — never SIGSEGVs on
// unmapped memory (returns T{}). This path no longer needs to live inside
// the sigsetjmp region.
static int compute_ticking_score_breakdown(uintptr_t wp) {
    int64_t atk_raw = 0, def_raw = 0;
    uint8_t atk_enabled = safe_read<uint8_t>(wp + ck3::war_obj::ATK_TICKING_ENABLED);
    uint8_t def_enabled = safe_read<uint8_t>(wp + ck3::war_obj::DEF_TICKING_ENABLED);
    if (atk_enabled) atk_raw = safe_read<int64_t>(wp + ck3::war_obj::ATK_TICKING_SCORE);
    if (def_enabled) def_raw = safe_read<int64_t>(wp + ck3::war_obj::DEF_TICKING_SCORE);
    int64_t atk_in_1000 = (atk_raw / 100000) * 1000;
    int64_t def_in_1000 = (def_raw / 100000) * 1000;
    return static_cast<int>(atk_in_1000 - def_in_1000);
}

// =============================================================================
// Post-Hook: called instead of WAR_SCORE_UPDATE
// =============================================================================

// WAR_SCORE_UPDATE signature: void FUN_066b6020(void* war_ptr)
// We call the original first, then compute and cache the breakdown.

// The imprisonment component, per side. Linux: int score in eax of a 16-byte
// return. MSVC returns the 16 bytes through a hidden slot in rcx
// (ck3_offsets_win.h funcs::WAR_SCORE_IMPRISONMENT); the score is the slot's +0.
static int war_score_imprisonment(void* war_ptr, int side) {
#ifdef _WIN32
    struct { int32_t score; uint32_t pad; uint64_t flags; } r = {};
    reinterpret_cast<void* (*)(void*, void*, uint8_t, void*)>(g_fn_imprisonment)(
        &r, war_ptr, static_cast<uint8_t>(side), nullptr);
    return r.score;
#else
    return g_fn_imprisonment(war_ptr, side, nullptr);
#endif
}

static void on_war_score_update(void* war_ptr) {
    // Call original first — game state is updated
    auto orig = reinterpret_cast<void(*)(void*)>(g_orig_war_score_update);
    if (orig) orig(war_ptr);

    // CK3 uses low sentinel values (e.g. 0x1) for invalid/unresolved pointers
    if (!war_ptr || reinterpret_cast<uintptr_t>(war_ptr) < 0x10000) return;

    uintptr_t wp = reinterpret_cast<uintptr_t>(war_ptr);

    // Validate this is a real CWar (not NullWar sentinel, not garbage).
    // Both subclasses carry a fixed vtable address; a war with any other vtable
    // value is either freed memory or a different object type entirely.
    uintptr_t vtable = safe_read<uintptr_t>(wp + ck3::war_obj::VTABLE);
    uintptr_t cwar_vt = g_base_addr + ck3::war_obj::CWAR_VTABLE;
    if (vtable != cwar_vt) return;

    uint32_t war_id = safe_read<uint32_t>(wp + ck3::war_obj::WAR_ID);
    if (war_id == 0xFFFFFFFF) return;   // uninitialized sentinel

    // Skip ended wars
    uint8_t is_ended = safe_read<uint8_t>(wp + ck3::war_obj::IS_ENDED);
    if (is_ended != 0) return;

    // All sub-functions must be resolved (TickingScore is direct-read — no fn ptr)
    if (!g_fn_imprisonment || !g_fn_battle || !g_fn_occupation || !g_fn_landless) {
        record_reject(war_id, WarScoreRejectReason::FnUnresolved); return;
    }

    // CB sub-object must be valid — sub-functions deref the CB vtable (war+0x98)
    // and CB_TYPE_REF (war+0xA0) for flag checks and score min/max bounds.
    if (!cb_subobj_valid(wp)) {
        record_reject(war_id, WarScoreRejectReason::CbSubobj); return;
    }

    // Battle list must be sane — BattleScore iterates BATTLE_LIST_PTR/COUNT.
    if (!battle_list_valid(wp)) {
        record_reject(war_id, WarScoreRejectReason::BattleList); return;
    }

    // Participant arrays must contain valid pointers — the original key fix.
    // FUN_066b4f40 (called transitively by ImprisonmentScore and OccupationScore)
    // iterates these and dereferences each entry to read char_id at +0x08.
    // A sentinel 0x1 slot causes *(0x1 + 8) = *0x9 = SIGSEGV.
    if (!side_participants_valid(wp + ck3::war_obj::ATK_SIDE_VTABLE)) {
        record_reject(war_id, WarScoreRejectReason::AtkParticipants); return;
    }
    if (!side_participants_valid(wp + ck3::war_obj::DEF_SIDE_VTABLE)) {
        record_reject(war_id, WarScoreRejectReason::DefParticipants); return;
    }

    // OccupationScore-specific deep chain validation (ck-nr51.1): CharacterDB
    // and LandedTitleDB globals must be mapped+sane, and each participant
    // must resolve through CharacterDB to either a NullSentinel-safe fallback
    // path or a CCharacter with a mapped vtable. See occupation_chain_valid()
    // for the full decompilation-grounded analysis.
    if (!occupation_chain_valid(wp)) {
        record_reject(war_id, WarScoreRejectReason::OccupationChain); return;
    }

    // LandlessScore-specific deep chain validation (ck-nr51.2): OWNER_LINK_DB
    // global must be mapped+sane; the CB primary-attacker char must resolve
    // safely through CharacterDB; the resolved attacker's title_realm_data
    // chain (used for is_landless predicate + MAA-refs iteration) must be
    // mapped, and every MAA regiment id must resolve through OWNER_LINK_DB
    // to either a NullSentinel-safe fallback or a mapped object. See
    // landless_realm_valid() for the full decompilation-grounded analysis.
    if (!landless_realm_valid(wp)) {
        record_reject(war_id, WarScoreRejectReason::LandlessRealm); return;
    }

    WarScoreBreakdown bd{};

    // TickingScore: direct memory read (ck-nr51.3). Lives OUTSIDE the sigsetjmp
    // region — safe_read uses process_vm_readv and cannot SIGSEGV.
    bd.ticking_atk      = compute_ticking_score_breakdown(wp);
    bd.ticking_def      = 0;  // net score already in ticking_atk

    // SIGSEGV protection around sub-function calls
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        // Per-side functions (3-param: war_ptr, side, tooltip)
        bd.imprisonment_atk = war_score_imprisonment(war_ptr, 0);
        bd.imprisonment_def = war_score_imprisonment(war_ptr, 1);
        bd.occupation_atk   = g_fn_occupation(war_ptr, 0, nullptr);
        bd.occupation_def   = g_fn_occupation(war_ptr, 1, nullptr);

        // Net-score functions (2-param: war_ptr, tooltip)
        // These compute both sides internally and return a single net value.
        // Do NOT pass side_index — it would be interpreted as a tooltip pointer
        // and SIGSEGV when side=1 (dereference of addr 0x1).
        bd.battle_atk       = g_fn_battle(war_ptr, nullptr);
        bd.battle_def       = 0;  // net score already in battle_atk
        bd.landless_atk     = g_fn_landless(war_ptr, nullptr);
        bd.landless_def     = 0;  // landless penalty is attacker-only

        // Sum and clamp like the game does (values are fixed-point / 1000)
        int raw_sum = bd.imprisonment_atk + bd.imprisonment_def
                    + bd.battle_atk + bd.battle_def
                    + bd.occupation_atk + bd.occupation_def
                    + bd.ticking_atk + bd.ticking_def
                    + bd.landless_atk + bd.landless_def;

        // Get min/max from CB type: *(war+0xA0) -> cb_type, +0x1738/+0x173C
        uintptr_t cb_type = safe_read<uintptr_t>(wp + ck3::war_obj::CB_TYPE_REF);
        int min_score = -100000;  // default -100 * 1000
        int max_score = 100000;   // default +100 * 1000
        if (cb_type > 0x10000 && is_mapped(cb_type)) {
            min_score = safe_read<int32_t>(cb_type + 0x1738);
            max_score = safe_read<int32_t>(cb_type + 0x173C);
            if (min_score == 0 && max_score == 0) {
                min_score = -100000;
                max_score = 100000;
            }
        }

        int clamped = std::max(min_score, std::min(max_score, raw_sum));
        bd.total = clamped / 1000;  // convert to percentage points

        bd.cached_tick = g_tick_id.load(std::memory_order_relaxed);

        // Store in cache (and clear any recorded reject — this war is healthy)
        {
            std::unique_lock<std::shared_mutex> lock(g_war_score_cache_mutex);
            g_war_score_cache[war_id] = bd;
            g_war_score_reject.erase(war_id);
        }
    } else {
        // SIGSEGV recovery — belt-and-suspenders; pre-validation above should
        // have caught any war state that would crash sub-functions. Hitting
        // this branch in production means the validation missed something
        // (log aggressively so we can extend the validator).
        if (g_logfile) {
            fprintf(g_logfile,
                "[CK3-Bridge] war_score_hook: UNEXPECTED SIGSEGV on war %u — "
                "pre-validation passed but sub-function still crashed. "
                "Investigate and extend validation.\n", war_id);
            fflush(g_logfile);
        }
        bridge_strict_check_abort("war_score_hook");
    }
    g_in_protected_region = false;
}

// =============================================================================
// Initialization
// =============================================================================

void init_war_score_hook() {
    // Resolve sub-function pointers (per-side = 3-param, net = 2-param)
    g_fn_imprisonment = reinterpret_cast<fn_war_score_sided>(
        bridge::game_fn(ck3::funcs::WAR_SCORE_IMPRISONMENT));
    g_fn_battle = reinterpret_cast<fn_war_score_net>(
        bridge::game_fn(ck3::funcs::WAR_SCORE_BATTLE));
    g_fn_occupation = reinterpret_cast<fn_war_score_sided>(
        bridge::game_fn(ck3::funcs::WAR_SCORE_OCCUPATION));
    // ck-nr51.3: TickingScore is direct-read — no function pointer to resolve.
    g_fn_landless = reinterpret_cast<fn_war_score_net>(
        bridge::game_fn(ck3::funcs::WAR_SCORE_LANDLESS_THUNK));

    // Install inline hook on WAR_SCORE_UPDATE
    uintptr_t target = bridge::game_fn(ck3::funcs::WAR_SCORE_UPDATE);
    g_orig_war_score_update = install_hook(target, reinterpret_cast<void*>(on_war_score_update));

    if (g_orig_war_score_update) {
        ADLOG("war_score_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_war_score_update);
    } else {
        ADLOG("war_score_hook: FAILED to install at 0x%lx\n", target);
    }
}

// =============================================================================
// Cache Query API (called from IPC thread)
// =============================================================================

bool query_war_score_breakdown(uint32_t war_id, WarScoreBreakdown* out) {
    std::shared_lock<std::shared_mutex> lock(g_war_score_cache_mutex);
    auto it = g_war_score_cache.find(war_id);
    if (it == g_war_score_cache.end()) return false;
    *out = it->second;
    return true;
}

// Freshness-aware variant (ck-xwlg). One bridge tick ≈ one game day;
// WAR_SCORE_UPDATE fires per war-update cycle, so a healthy war's breakdown
// is at most ~a month old. An entry aging past WAR_SCORE_FRESH_TICKS means
// pre-validation has been rejecting every refresh since — the entry is a
// fossil (typically the all-zero day-one breakdown) and reporting it as
// "computed" truth is exactly the bug that pinned war_score at 0 on-air
// while battles raged. Stale entries are reported as a miss; *reject_out
// (when non-null) receives the last recorded reject reason so the caller
// can surface WHICH validator is starving the cache.
bool query_war_score_breakdown_fresh(uint32_t war_id, WarScoreBreakdown* out,
                                     WarScoreRejectReason* reject_out) {
    std::shared_lock<std::shared_mutex> lock(g_war_score_cache_mutex);
    if (reject_out) {
        auto rj = g_war_score_reject.find(war_id);
        *reject_out = (rj == g_war_score_reject.end())
                          ? WarScoreRejectReason::None : rj->second;
    }
    auto it = g_war_score_cache.find(war_id);
    if (it == g_war_score_cache.end()) return false;
    uint64_t now = g_tick_id.load(std::memory_order_relaxed);
    if (now > it->second.cached_tick &&
        now - it->second.cached_tick > WAR_SCORE_FRESH_TICKS) return false;
    *out = it->second;
    return true;
}

bool query_all_war_score_breakdowns(std::unordered_map<uint32_t, WarScoreBreakdown>* out) {
    std::shared_lock<std::shared_mutex> lock(g_war_score_cache_mutex);
    *out = g_war_score_cache;
    return !g_war_score_cache.empty();
}

} // namespace bridge
