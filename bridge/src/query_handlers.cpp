// query_handlers.cpp — All do_query_* observation functions, build_char_profile,
// enumerate_interactions, enumerate_decisions, do_get_tick_info, do_get_ai_evaluations.
//
// Split from action_dispatch.cpp (mechanical refactor, no logic changes).

#include "scope_slot_policy.h"   // ck-zgh4: the scope latch
#include "instance_paths.h"  // ck-qn03
#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "game_date.h"
#include "speed_read.h"    // ck-k57a: a faulted speed read is an ABSENCE, not a 1
#include "pipeline.h"
#include "budget_ledger.h"

#include <cmath>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <map>
#include <mutex>
#include <cstdio>
#include <sys/stat.h>
#include <rapidjson/document.h>

namespace bridge {

void log_degraded_read(const char* action) {
    if (!g_logfile) return;
    const unsigned long long a = safe_read_last_fault();
    fprintf(g_logfile, "[CK3-Bridge] %s degraded: %d unreadable read(s), last addr=0x%llx", action,
            safe_read_fault_count(), a);
    if ((a >> 52) == 0x7FF)
        fprintf(g_logfile, " (poisoned RVA: poison_index=%llu field_offset=0x%llx)", (a >> 40) & 0xFFF,
                (a - g_base_addr) & 0xFFFFFFFFFFull);
    fprintf(g_logfile, "\n");
}

// Runtime flag arming the do_query_war_stance AI-mgr coordinator record walk.
// DEFAULT OFF — the record↔war-id match key (rec+0x18) is decompile-confirmed
// (FUN_04e60920) but not yet live-probed on a known-at-war record
// (re_query_war_stance.md §6). With it OFF the handler still returns occupation-
// derived stance from the war-score cache; the WALK only adds the true stance
// name once a live session confirms the match key. Toggle via dispatch_action
// ("set_war_stance_record_walk" {enabled:bool}) — pure read, no game-thread reentry.
std::atomic<bool> g_war_stance_record_walk_enabled{false};

// ============================================================================
// War-declarability honesty helpers (read-side only — ck war_validity_discovery)
//
// These mirror the steered-war WRITE gates (intent_dispatch.cpp:1358-1572) and
// the engine cull conditions into the DISCOVERY surfaces so the agent stops
// attempting wars the engine will cull to war_count 0. PURE READS — no engine
// evaluator, no Execute, no WCC. NOT a write gate (the reverted 4d14ce3 hang was
// an inline gate on the WRITE path; this only annotates query JSON).
//
// DefenderState captures the per-target cull conditions a steered claim_cb war
// is dropped on: imprisoned target (war_validity R4), non-independent target
// (intent_dispatch.cpp:1478 target_not_independent). Alliance/truce require the
// engine list-getters (mirrored separately where the actor pointer is in scope).
// ============================================================================
struct DefenderState {
    bool independent = true;     // get_liege_id_from_addr => 0xFFFFFF/0 == no liege
    uint32_t liege_id = 0;       // resolved liege char id when not independent
    bool imprisoned = false;     // core_data+0x288 IMPRISONER_LINK != null && mapped
};

// Pure-memory probe of the war-defender (title holder). holder_addr is a
// CCharacter* already resolved by the caller (lookup_character). All reads
// fail OPEN (independent=true, imprisoned=false) on unmapped chains so a freed
// or garbage pointer never blocks a legitimate war — same posture as the write
// path. Mirrors intent_dispatch.cpp:1478 (independence) and
// query_handlers.cpp:124-130 (imprisonment, the AUTHORITATIVE signal — NOT the
// IS_IMPRISONED predicate which L7 found is an independence check).
static DefenderState probe_defender_state(uintptr_t holder_addr) {
    DefenderState st;
    if (!holder_addr || !is_mapped(holder_addr)) return st;
    // Independence — authoritative liege resolution (get_liege_id_from_addr
    // normalizes the 0xFFFFFF "no-liege" sentinel; raw title_realm+0x448 is
    // unreliable and false-rejects independent rulers — L22).
    uint32_t dl = get_liege_id_from_addr(holder_addr) & 0xFFFFFF;
    if (dl != 0xFFFFFF && dl != 0) {
        st.independent = false;
        st.liege_id = dl;
    }
    // Imprisonment — core_data = *(holder+0x1A8); link = *(core_data+0x288).
    uintptr_t cd = safe_read<uintptr_t>(holder_addr + ck3::character::REALM_DATA);
    if (cd && is_mapped(cd)) {
        uintptr_t link = safe_read<uintptr_t>(cd + ck3::core_data::IMPRISONER_LINK);
        st.imprisoned = (link != 0 && is_mapped(link));
    }
    return st;
}

// Does `attacker_addr` hold a PRESSED claim on a title currently held by
// `defender_id`? Mirrors the write-path pressed-claim scan
// (intent_dispatch.cpp:1375-1393) in the discovery direction (no specific war
// title chosen, so test existence of ANY pressed claim against the defender).
// Pure reads; returns false on unmapped chains.
static bool attacker_has_pressed_claim_on(uintptr_t attacker_addr, uint32_t defender_id) {
    if (!attacker_addr || !is_mapped(attacker_addr)) return false;
    defender_id &= 0xFFFFFF;
    uintptr_t core_data = safe_read<uintptr_t>(attacker_addr + ck3::character::REALM_DATA);
    if (!core_data || !is_mapped(core_data)) return false;
    uintptr_t claims_pdx = core_data + ck3::core_data::CLAIMS_ARRAY;
    uintptr_t data = safe_read<uintptr_t>(claims_pdx + 0x00);
    int32_t cnt = safe_read<int32_t>(claims_pdx + 0x0C);
    if (cnt < 0 || cnt > 200) cnt = 0;
    if (!data || !is_mapped(data)) return false;
    for (int32_t j = 0; j < cnt; j++) {
        uintptr_t entry = data + j * ck3::claim_entry::ENTRY_SIZE;
        if (safe_read<uint8_t>(entry + ck3::claim_entry::PRESSED) == 0) continue;
        uint32_t tid = safe_read<uint32_t>(entry + ck3::claim_entry::TITLE_ID) & 0xFFFFFF;
        uintptr_t tobj = lookup_title_obj(tid);
        if (!tobj) continue;
        uint32_t holder = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
        if (holder == defender_id) return true;
    }
    return false;
}

// Resolve the pressed-claim TITLE the actor holds against `defender_id` (the
// title_id a claim_cb war would take). Returns the title id (0 if none) and, on
// success, the claimant char id = the actor (claim_cb's claimant is the holder of
// the pressed claim — the actor itself for an actor-pressed claim). This makes a
// claim_cb row declare-ready (cb_title_id + claimant_id) WITHOUT a second query.
// Self-contained walk of the actor's claims array; no de-jure walk. Pure safe_read.
static uint32_t resolve_pressed_claim_title(uintptr_t actor_addr, uint32_t defender_id) {
    if (!actor_addr || !is_mapped(actor_addr)) return 0;
    defender_id &= 0xFFFFFF;
    uintptr_t core_data = safe_read<uintptr_t>(actor_addr + ck3::character::REALM_DATA);
    if (!core_data || !is_mapped(core_data)) return 0;
    uintptr_t claims_pdx = core_data + ck3::core_data::CLAIMS_ARRAY;
    uintptr_t data = safe_read<uintptr_t>(claims_pdx + 0x00);
    int32_t cnt = safe_read<int32_t>(claims_pdx + 0x0C);
    if (cnt < 0 || cnt > 200) cnt = 0;
    if (!data || !is_mapped(data)) return 0;
    for (int32_t j = 0; j < cnt; j++) {
        uintptr_t entry = data + j * ck3::claim_entry::ENTRY_SIZE;
        if (safe_read<uint8_t>(entry + ck3::claim_entry::PRESSED) == 0) continue;
        uint32_t tid = safe_read<uint32_t>(entry + ck3::claim_entry::TITLE_ID) & 0xFFFFFF;
        uintptr_t tobj = lookup_title_obj(tid);
        if (!tobj) continue;
        uint32_t holder = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
        if (holder == defender_id) return tid;
    }
    return 0;
}

// AUTHORITATIVE bilateral alliance check (#119). Returns true iff `attacker` and
// `defender_id` are genuinely ALLIED, mirroring the engine's IsAlliedTo
// (0x6750c50) relation path EXACTLY: walk the attacker's sorted relation array
// (core_data+0x20, ck3::relation_container), find the CDiplomaticRelation for
// `defender_id`, and test alliance == (REASON_COUNT @ rel+0x94 != 0) &&
// (BROKEN_FLAG @ rel+0x189 == 0). HasAllianceInRelation (engine 0x6369900) is
// literally `*(int*)(rel+0x94) != 0`; IsAlliedTo additionally rejects the
// broken-flag case. This is the SAME container query_realm's `alliances` array
// already enumerates (query_handlers.cpp ~7046).
//
// ROOT CAUSE FIXED (#119): the prior probe searched MAYBE_GET_ALLIANCE_LIST's
// +0x578 list (== diplomatic_data::WAR_LIST_PTR, the WAR-PARTICIPANT list) for
// the defender pointer, so it reported the attacker's WAR opponents as "allied".
// Core-dump validated (ck3_20260421_195552): genuine allies live ONLY in the
// relation array (+0x94), NEVER in +0x578/+0x7D0. See
// re/findings/re_alliance_false_positive_119.md.
//
// PURE safe_read memory walk — no engine call, no game-thread reentry → pause-safe,
// adds no unpause requirement. Linear scan (counts are small, <500). Fails CLOSED
// to "not allied" (false) on any unmapped chain so a garbage pointer never blocks
// a legitimate war — same fail-open posture as before.
static bool is_allied_via_relation(uintptr_t attacker_addr, uint32_t defender_id) {
    if (!attacker_addr || !is_mapped(attacker_addr)) return false;
    defender_id &= 0xFFFFFF;
    if (defender_id == 0) return false;
    uintptr_t core_data = safe_read<uintptr_t>(attacker_addr + ck3::character::REALM_DATA);
    if (!core_data || !is_mapped(core_data)) return false;
    uintptr_t rel_arr = safe_read<uintptr_t>(core_data + ck3::relation_container::ARRAY_PTR);
    int32_t rel_count = safe_read<int32_t>(core_data + ck3::relation_container::COUNT);
    if (rel_count < 0 || rel_count > 5000) rel_count = 0;
    if (!rel_arr || !is_mapped(rel_arr) || rel_count == 0) return false;
    for (int32_t i = 0; i < rel_count; i++) {
        uintptr_t entry = rel_arr + i * ck3::relation_container::ENTRY_STRIDE;
        uint32_t other_id = safe_read<uint32_t>(entry + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
        if (other_id != defender_id) continue;
        uintptr_t rel = safe_read<uintptr_t>(entry + ck3::relation_container::ENTRY_PTR);
        if (!rel || !is_mapped(rel)) return false;
        int32_t reasons = safe_read<int32_t>(rel + ck3::alliance::REASON_COUNT);
        if (reasons <= 0 || reasons > 20) return false;       // not allied / garbage
        uint8_t broken = safe_read<uint8_t>(rel + ck3::alliance::BROKEN_FLAG);
        return broken == 0;                                    // allied iff not broken
    }
    return false;  // no relation record => definitely not allied
}

// Alliance/truce target-state probe (read-side honesty). Mirrors the steered-
// write gates (intent_dispatch.cpp:1511-1572 target_in_alliance/target_in_truce)
// and the strict surface's inline probe (do_query_cb_validity_strict 7410-7444).
// attacker_char/defender_char are CCharacter* already resolved by the caller.
//
// #119 FIX: the ALLIANCE half now uses is_allied_via_relation (the authoritative
// relation-array +0x94 check, == engine IsAlliedTo) instead of the misnamed
// MAYBE_GET_ALLIANCE_LIST search that was matching WAR participants. The TRUCE
// half is unchanged — GET_TRUCE_LIST (char+0x1C0+0x6A0) IS the genuine truce
// list (HIGH, Ghidra-named "GetTruceList"), and the engine searches it the same
// way. PURE safe_read + one list-getter (no engine evaluator, no WCC) — wrapped
// in sigsetjmp + g_in_protected_region and FAILS OPEN (in_alliance=in_truce=false)
// on any unmapped chain or longjmp so a freed/garbage pointer NEVER blocks a war.
static void probe_alliance_truce(void* attacker_char, void* defender_char,
                                 bool& out_in_alliance, bool& out_in_truce) {
    out_in_alliance = false;
    out_in_truce = false;
    if (!attacker_char || is_sentinel(attacker_char)) return;
    if (!defender_char || is_sentinel(defender_char)) return;
    uint32_t defender_id = char_get_id(defender_char) & 0xFFFFFF;
    if (defender_id == 0) return;
    auto get_truce_list = reinterpret_cast<uintptr_t(*)(void*)>(bridge::game_fn(ck3::funcs::GET_TRUCE_LIST));
    auto simd_find      = reinterpret_cast<uintptr_t(*)(uintptr_t, uintptr_t, void*)>(bridge::game_fn(ck3::funcs::SIMD_FIND_LONG));
    const bool truce_funcs_mapped =
        is_mapped(reinterpret_cast<uintptr_t>(get_truce_list)) &&
        is_mapped(reinterpret_cast<uintptr_t>(simd_find));
    // volatile keeps the flags off registers across a possible longjmp recovery.
    volatile bool in_alliance_v = false, in_truce_v = false;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig == 0) {
        g_in_protected_region = true;
        // #119: authoritative relation-array alliance check (NOT the war list).
        in_alliance_v = is_allied_via_relation(
            reinterpret_cast<uintptr_t>(attacker_char), defender_id);
        // Truce: GET_TRUCE_LIST is correct — keep the engine list search.
        if (truce_funcs_mapped) {
            uintptr_t tlist = get_truce_list(attacker_char);
            if (tlist && is_mapped(tlist)) {
                uintptr_t b = safe_read<uintptr_t>(tlist + 0);
                int32_t n = safe_read<int32_t>(tlist + 0xC);
                if (b && is_mapped(b) && n > 0 && n < 1000) {
                    uintptr_t hit = simd_find(b, b + (size_t)n * 8, defender_char);
                    in_truce_v = (hit != 0 && hit != b + (size_t)n * 8);
                }
            }
        }
        g_in_protected_region = false;
    } else {
        g_in_protected_region = false;  // fail open
    }
    out_in_alliance = in_alliance_v;
    out_in_truce = in_truce_v;
}

// Truce-expiry probe (read-side, for the war-planner enumeration). Walks the
// actor's CDiplomaticRelation truce list (diplomatic_data+TRUCE_LIST_PTR) and
// returns the expiry date of the truce vs `target_id` if one is active. Mirrors
// the read in do_query_relations (the authoritative expiry source) — NOT the
// engine GET_TRUCE_LIST char-ptr list (which has no expiry). Pure safe_read,
// bounded; fails closed (returns false / no expiry) on any unmapped chain.
// out_end_date_str is "YYYY.MM.DD" (>= 32 bytes); out_end_date_raw is the raw date.
static bool probe_truce_expiry(void* actor_char, uint32_t target_id,
                               int32_t& out_end_date_raw, char* out_end_date_str,
                               size_t str_sz) {
    out_end_date_raw = 0;
    if (out_end_date_str && str_sz) out_end_date_str[0] = '\0';
    if (!actor_char || is_sentinel(actor_char)) return false;
    target_id &= 0xFFFFFF;
    if (target_id == 0) return false;
    uintptr_t c = reinterpret_cast<uintptr_t>(actor_char);
    uint32_t my_id = char_get_id(actor_char) & 0xFFFFFF;

    uintptr_t diplo = safe_read<uintptr_t>(c + ck3::character::DIPLOMATIC_DATA);
    if (!diplo || !is_mapped(diplo)) return false;
    uintptr_t truce_arr = safe_read<uintptr_t>(diplo + ck3::diplomatic_data::TRUCE_LIST_PTR);
    int32_t tc = safe_read<int32_t>(diplo + ck3::diplomatic_data::TRUCE_LIST_COUNT);
    if (tc < 0 || tc > 200) return false;
    if (!truce_arr || !is_mapped(truce_arr) || tc == 0) return false;

    int32_t current_date_raw = 0;
    {
        uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
        if (gs) current_date_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    }

    for (int32_t i = 0; i < tc; i++) {
        uintptr_t rel = safe_read<uintptr_t>(truce_arr + i * 8);
        if (!rel || !is_mapped(rel)) continue;
        uint32_t cid1 = safe_read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_LOW);
        uint32_t cid2 = safe_read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_HIGH);
        size_t expiry_off, reason_off;
        uint32_t other;
        if ((cid1 & 0xFFFFFF) == my_id) {
            other = cid2 & 0xFFFFFF;
            expiry_off = ck3::diplomatic_relation::TRUCE_EXPIRY_A;
            reason_off = ck3::diplomatic_relation::TRUCE_REASON_TYPE_A;
        } else if ((cid2 & 0xFFFFFF) == my_id) {
            other = cid1 & 0xFFFFFF;
            expiry_off = ck3::diplomatic_relation::TRUCE_EXPIRY_B;
            reason_off = ck3::diplomatic_relation::TRUCE_REASON_TYPE_B;
        } else {
            continue;
        }
        if (other != target_id) continue;
        int32_t expiry = safe_read<int32_t>(rel + expiry_off);
        int32_t reason = safe_read<int32_t>(rel + reason_off);
        if (reason == ck3::diplomatic_relation::TRUCE_REASON_INVALID) continue;
        if (current_date_raw > 0 && expiry > 0 && expiry < current_date_raw) continue;  // expired
        out_end_date_raw = expiry;
        if (out_end_date_str && str_sz) {
            ck3::game_date::format_raw(expiry, out_end_date_str, str_sz);  // ck-05mg
        }
        return true;
    }
    return false;
}

// Static CB declaration-cost table (re_warplanner_enumeration.md §4). CB cost is
// game DATA, not memory-resident (no CB_COST offset exists), so these are the
// stock CK3 base costs keyed by CB family. MEDIUM confidence — tier-scaled and
// rule-modifiable in the engine; the `affordable` flag derived from them is a
// HINT (the engine's own gate is authoritative at declare time), good enough for
// the agent to choose "declare now" vs "raid for prestige first".
struct CBCost { int prestige; int piety; };
static CBCost cb_cost_for(const char* cb_name) {
    if (!cb_name) return { 0, 0 };
    // Holy / religious wars cost piety.
    if (strstr(cb_name, "holy_war") || strstr(cb_name, "religious_war"))
        return { 0, 100 };
    // Claim / subjugation are free to declare (claim is "spent" by pressing it;
    // tribal_subjugation has no resource cost).
    if (strstr(cb_name, "claim_cb") || strstr(cb_name, "subjugation"))
        return { 0, 0 };
    // Conquest / de-jure family cost prestige (stock base; tier-scaled in engine).
    if (strstr(cb_name, "conquest") || strstr(cb_name, "de_jure"))
        return { 100, 0 };
    // Default: unknown win-line CB — assume modest prestige.
    return { 50, 0 };
}

// Is `actor_addr` already in an ACTIVE war against `target_id` on OPPOSING
// sides? The engine culls a redundant second war declaration between a pair
// already at war on opposing sides (Gate 0 / CB_ALREADY_AT_WAR — same condition
// do_query_war_validity surfaces as R5/S3). This is the read-side mirror: a pure
// participant-array walk lifted from do_get_active_wars (3220-3300) /
// do_query_war_validity. NO engine function call, NO game-thread reentry — only
// safe_read over the WAR_DB hash map, so it is pause-safe and adds no unpause
// requirement. Fails OPEN (returns false) on any unmapped chain so a stale war
// ref can never spuriously suppress a legitimate war. actor_addr is a
// CCharacter*; target_id is the defender char id (24-bit).
static bool actor_at_war_opposing(uintptr_t actor_addr, uint32_t target_id) {
    if (!actor_addr || !is_mapped(actor_addr)) return false;
    target_id &= 0xFFFFFF;
    uint32_t actor_id = char_get_id(reinterpret_cast<void*>(actor_addr)) & 0xFFFFFF;
    if (actor_id == 0 || target_id == 0) return false;
    uintptr_t ruler_data = safe_read<uintptr_t>(actor_addr + ck3::character::TITLE_REALM_DATA);
    if (!ruler_data || !is_mapped(ruler_data)) return false;
    uintptr_t war_array_base = ruler_data + ck3::title_realm::WAR_REF_ARRAY;
    uintptr_t war_ref_data = safe_read<uintptr_t>(war_array_base + 0x00);
    int32_t count = safe_read<int32_t>(war_array_base + 0x0C);
    if (count < 0 || count > 50) count = 0;
    if (!war_ref_data || !is_mapped(war_ref_data)) return false;
    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (!war_db || !is_mapped(war_db)) return false;
    uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
    if (!table || !is_mapped(table)) return false;

    // side_has: does the participant container at war_ptr+side_off contain want_id?
    auto side_has = [&](uintptr_t war_ptr, size_t side_off, uint32_t want_id) -> bool {
        uintptr_t arr = safe_read<uintptr_t>(war_ptr + side_off + 0x00);
        int32_t pc = safe_read<int32_t>(war_ptr + side_off + 0x0C);
        if (pc < 0 || pc > 100) pc = 0;
        if (!arr || !is_mapped(arr)) return false;
        for (int32_t j = 0; j < pc && j < 30; j++) {
            uintptr_t part_ptr = safe_read<uintptr_t>(arr + j * 8);
            if (!part_ptr || !is_mapped(part_ptr)) continue;
            uint32_t pid = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
            if (pid == want_id) return true;
        }
        return false;
    };

    for (int32_t i = 0; i < count; i++) {
        uint32_t war_ref = safe_read<uint32_t>(war_ref_data + i * 4);
        if (war_ref == 0 || war_ref == 0xFFFFFFFF) continue;
        uint32_t idx = war_ref & 0xFFFFFF;
        if (idx >= capacity) continue;
        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
        uintptr_t war_ptr = safe_read<uintptr_t>(entry);
        if (!war_ptr || !is_mapped(war_ptr)) continue;
        if ((safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID) & 0xFFFFFF) != idx) continue;
        if (safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED) != 0) continue;

        // Fast path: primary attacker/defender refs.
        uint32_t patk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
        uint32_t pdef = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
        if ((patk == actor_id && pdef == target_id) ||
            (pdef == actor_id && patk == target_id)) return true;

        // Full participant scan (co-belligerents): actor & target on opposing sides.
        bool actor_atk = side_has(war_ptr, ck3::war_obj::ATK_PARTICIPANT, actor_id);
        bool actor_def = side_has(war_ptr, ck3::war_obj::DEF_PARTICIPANT, actor_id);
        if (actor_atk && side_has(war_ptr, ck3::war_obj::DEF_PARTICIPANT, target_id)) return true;
        if (actor_def && side_has(war_ptr, ck3::war_obj::ATK_PARTICIPANT, target_id)) return true;
    }
    return false;
}

// Forward decls for kin helpers (defined later in TU near do_query_conquest_targets).
static uint32_t char_dynasty_id(uintptr_t char_addr);
static uint32_t char_house_id(uintptr_t char_addr);
static bool is_kin_of(uintptr_t target, uint32_t ref_house, uint32_t ref_dyn);
static bool family_array_contains(uintptr_t family_ptr, size_t arr_off, uint32_t cid);
// ck-6kvq.27: the authoritative top-liege walk (defined below, ~:2100) is
// needed by the character emitter, which sits above its definition.
static uint32_t get_top_realm_ruler_id(uintptr_t char_addr);

// lane-query item 3: ONE definition of `is_ai`, read by both the `character`
// profile and the `characters` listing (search_characters). They disagreed:
// the profile used core_data+AI_CONTROLLER (non-null), the listing read the
// byte at character+0x199 (IS_AI_FLAG), so an observer save reported
// is_ai:false for every independent ruler in the listing and is_ai:true for the
// same ids in the profile.
//
// Core-dump evidence (ck3_20260625_152901_ivar867, observer, PLAYER_CHAR_ID
// 0xFFFFFFFF; william_1066_baseline, player 31808):
//   * +0x199 is NOT an AI flag: in the observer dump it is 0 on 4,487 of 4,593
//     living rulers and 1 on 8,951 living non-rulers; in the William dump it is 0
//     for the human player AND for his AI rivals 30853/29767 alike.
//   * core_data+AI_CONTROLLER is non-null on every living ruler in both dumps,
//     the human player included (31808: controller present, AI_ENABLED=1), so on
//     its own it over-reports the player as AI.
// So: AI-controlled = has an AI controller AND is not the human player.
// PLAYER_CHAR_ID is 0xFFFFFFFF in observer mode, which matches no character.
static bool char_is_ai_controlled(uintptr_t c) {
    if (!c || !is_mapped(c)) return false;
    uintptr_t core = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
    if (!core || !is_mapped(core)) return false;
    if (safe_read<uintptr_t>(core + ck3::core_data::AI_CONTROLLER) == 0) return false;
    uint32_t player = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID) & 0xFFFFFF;
    uint32_t id = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;
    return player == 0xFFFFFF || player == 0 || id != player;
}

// Helper: build character basic profile into a ResponseBuilder
//
// ck-86ue.2: All derived-pointer hops are now guarded with is_mapped() in
// addition to the safe_read<T>() reads. safe_read returns 0 on unmapped
// addresses, so the guards never gated previously-valid pointers — they
// only stop subsequent reads from a derived pointer that is plausibly
// non-null but not actually backed by a mapped page (sentinel 0x1,
// freed-and-reused, half-constructed objects, etc.) from leaking
// garbage into the output JSON.
static void build_char_profile(ResponseBuilder& r, void* character) {
    uint64_t prof_tick = g_tick_id.load(std::memory_order_relaxed);
    bool verbose = (prof_tick % 100 == 0);  // Log details periodically
    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t id = safe_read<uint32_t>(c + ck3::character::ID);
    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] build_char_profile: char=%p id=%u\n", character, id & 0xFFFFFF); fflush(g_logfile); }

    r.key("id"); r.value_uint(id & 0xFFFFFF);

    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. names\n"); fflush(g_logfile); }
    emit_char_names(r, c);

    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. alive\n"); fflush(g_logfile); }
    // Wrap g_char_is_alive in sigsetjmp — game function may crash on corrupted object
    bool alive = true;
    if (g_char_is_alive) {
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig == 0) {
            alive = g_char_is_alive(reinterpret_cast<void*>(c));
        } else {
            alive = true;  // default to alive on crash
            if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] build_char_profile: g_char_is_alive crashed (sig=%d)\n", sig); fflush(g_logfile); }
        }
    }
    r.key("alive"); r.value_bool(alive);

    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. resources\n"); fflush(g_logfile); }
    uintptr_t realm_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
    if (realm_data && is_mapped(realm_data)) {
        r.key("gold");     r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::character::REALM_GOLD)) / 100000.0);
        r.key("prestige"); r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::character::REALM_PRESTIGE)) / 100000.0);
        r.key("piety");    r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::character::REALM_PIETY)) / 100000.0);
        // Stress: uint32 at core_data + 0x2F8 (HIGH confidence, Round 7)
        r.key("stress");   r.value_int(safe_read<int32_t>(realm_data + ck3::character_vitals::STRESS));
        // Health: int64 fixed-point at core_data + 0x2F0 (VPS MEMORY.md CONFIRMED)
        int64_t health_raw = safe_read<int64_t>(realm_data + ck3::character_vitals::HEALTH);
        r.key("health");   r.value_float(static_cast<double>(health_raw) / 100000.0);
        // Income: int64 fixed-point at core_data + 0x170 (R8V-1 CONFIRMED via FUN_0674b920)
        int64_t income_raw = safe_read<int64_t>(realm_data + ck3::core_data::INCOME_DISPLAY);
        r.key("monthly_income"); r.value_float(static_cast<double>(income_raw) / 100000.0);
        // Income breakdown (core_data+0x2B0..0x2D0, all int64 fixed-point / 100000)
        r.key("income_breakdown"); r.begin_object();
        r.key("total_monthly");  r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::core_data::MONTHLY_INCOME)) / 100000.0);
        r.key("gross_domain");   r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::core_data::GROSS_DOMAIN_INCOME)) / 100000.0);
        r.key("net_domain");     r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::core_data::NET_DOMAIN_INCOME)) / 100000.0);
        r.key("total_expenses"); r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::core_data::TOTAL_EXPENSES)) / 100000.0);
        r.key("net_gold");       r.value_float(static_cast<double>(safe_read<int64_t>(realm_data + ck3::core_data::NET_GOLD_INCOME)) / 100000.0);
        r.end_object();
    } else {
        r.key("gold");     r.value_float(0.0);
        r.key("prestige"); r.value_float(0.0);
        r.key("piety");    r.value_float(0.0);
        r.key("stress");   r.value_int(0);
        r.key("health");   r.value_float(0.0);
        r.key("monthly_income"); r.value_float(0.0);
        r.key("income_breakdown"); r.begin_object();
        r.key("total_monthly");  r.value_float(0.0);
        r.key("gross_domain");   r.value_float(0.0);
        r.key("net_domain");     r.value_float(0.0);
        r.key("total_expenses"); r.value_float(0.0);
        r.key("net_gold");       r.value_float(0.0);
        r.end_object();
    }
    // Dread: int64 fixed-point at title_realm + 0x350 (HIGH confidence, Round 7)
    uintptr_t title_realm_for_dread = resolve_title_realm(c);
    if (title_realm_for_dread) {
        int64_t dread_raw = safe_read<int64_t>(title_realm_for_dread + ck3::character_vitals::DREAD);
        r.key("dread"); r.value_float(static_cast<double>(dread_raw) / 100000.0);
    } else {
        r.key("dread"); r.value_float(0.0);
    }

    // Age: call GET_AGE(char*) -> int16 (years) — wrapped in sigsetjmp
    {
        auto fn_age = reinterpret_cast<int16_t(*)(void*)>(bridge::game_fn(ck3::funcs::GET_AGE));
        volatile int16_t age = 0;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig == 0) {
            age = fn_age(character);
        } else {
            if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] build_char_profile: fn_age crashed (sig=%d)\n", sig); fflush(g_logfile); }
        }
        r.key("age"); r.value_int(age);

        // Birth date index (raw int16 at char+0x6C)
        int16_t birth_date = safe_read<int16_t>(c + ck3::character::BIRTH_DATE_SHORT);
        r.key("birth_date"); r.value_int(birth_date);
    }

    // Imprisonment status.
    // L7 note: do NOT call ck3::funcs::IS_IMPRISONED (0x0664adf0) — that is
    // maybe_realm_predicate_inverted (independence predicate), NOT an
    // imprisonment check (flagged ~25% of landed chars as "imprisoned").
    // Authoritative signal (HIGH, verified live pid 680049 vs engine predicate
    // FUN_0674b2d0): is_imprisoned == (core_data+0x288 link pointer != null),
    // where core_data = *(char + REALM_DATA / 0x1A8). Prisoners can be unlanded
    // yet still have a valid char+0x1A8 core_data, so re-read c+0x1A8 here rather
    // than reusing the income-path realm_data guard. Defaults false on fault.
    {
        bool imprisoned = false;
        uintptr_t cd = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);  // core_data = *(char+0x1A8)
        if (cd && is_mapped(cd)) {
            uintptr_t link = safe_read<uintptr_t>(cd + ck3::core_data::IMPRISONER_LINK);
            imprisoned = (link != 0 && is_mapped(link));
        }
        r.key("is_imprisoned"); r.value_bool(imprisoned);
    }

    // Culture ID and Faith ID (direct on CCharacter, HIGH confidence Round 5e/5f)
    {
        uint32_t culture_id_raw = safe_read<uint32_t>(c + ck3::character::CULTURE_ID_DIRECT);
        if (culture_id_raw != 0 && culture_id_raw != 0xFFFFFFFF) {
            r.key("culture_id"); r.value_uint(culture_id_raw & 0xFFFFFF);
        } else {
            r.key("culture_id"); r.value_null();
        }
        uint32_t faith_id_raw = safe_read<uint32_t>(c + ck3::character::FAITH_ID_DIRECT);
        if (faith_id_raw != 0 && faith_id_raw != 0xFFFFFFFF) {
            r.key("faith_id"); r.value_uint(faith_id_raw & 0xFFFFFF);
        } else {
            r.key("faith_id"); r.value_null();
        }
    }

    // FIX #is_kin: numeric house/dynasty + kin flags relative to the controlled ruler.
    // Murder-lever guard: agent must SKIP act(scheme,start,murder) when is_kin==true
    // (kinslayer trait → catastrophic stress). house+0x150 is the reliable kin field.
    {
        uint32_t this_house = char_house_id(c);
        uint32_t this_dyn   = char_dynasty_id(c);
        if (this_house != 0) { r.key("house_id"); r.value_uint(this_house); }
        else                 { r.key("house_id"); r.value_null(); }
        if (this_dyn != 0)   { r.key("dynasty_id"); r.value_uint(this_dyn); }
        else                 { r.key("dynasty_id"); r.value_null(); }

        void* ruler = get_controlled_character();
        if (ruler && !is_sentinel(ruler)) {
            uintptr_t rc = reinterpret_cast<uintptr_t>(ruler);
            uint32_t self_id  = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;
            uint32_t ruler_id = safe_read<uint32_t>(rc + ck3::character::ID) & 0xFFFFFF;
            if (self_id == ruler_id) {
                r.key("is_kin");       r.value_bool(false);
                r.key("same_house");   r.value_bool(false);
                r.key("same_dynasty"); r.value_bool(false);
                r.key("relation");     r.value_string("self");
            } else {
                uint32_t ref_house = char_house_id(rc);
                uint32_t ref_dyn   = char_dynasty_id(rc);
                bool same_house   = (ref_house != 0 && this_house == ref_house);
                bool same_dynasty = (ref_dyn != 0 && this_dyn == ref_dyn);
                // Close-family link via family sub-structs (both directions). Catches
                // married-in kin (e.g. a father whose house is 0xFFFFFFFF).
                uintptr_t r_fam = safe_read<uintptr_t>(rc + ck3::character::FAMILY_DATA);
                uintptr_t t_fam = safe_read<uintptr_t>(c  + ck3::character::FAMILY_DATA);
                uint32_t r_father = r_fam ? (safe_read<uint32_t>(r_fam + ck3::family::FATHER_ID) & 0xFFFFFF) : 0;
                uint32_t r_mother = r_fam ? (safe_read<uint32_t>(r_fam + ck3::family::MOTHER_ID) & 0xFFFFFF) : 0;
                uint32_t t_father = t_fam ? (safe_read<uint32_t>(t_fam + ck3::family::FATHER_ID) & 0xFFFFFF) : 0;
                uint32_t t_mother = t_fam ? (safe_read<uint32_t>(t_fam + ck3::family::MOTHER_ID) & 0xFFFFFF) : 0;
                bool is_child  = (t_father == ruler_id || t_mother == ruler_id)
                              || family_array_contains(r_fam, ck3::family::CHILDREN_ARRAY, self_id);
                bool is_parent = (r_father == self_id || r_mother == self_id)
                              || family_array_contains(t_fam, ck3::family::CHILDREN_ARRAY, ruler_id);
                bool is_spouse = family_array_contains(r_fam, ck3::family::SPOUSE_ARRAY, self_id)
                              || family_array_contains(t_fam, ck3::family::SPOUSE_ARRAY, ruler_id);
                bool is_sibling = (r_father != 0 && (r_father == t_father)) ||
                                  (r_mother != 0 && (r_mother == t_mother));
                bool is_kin = same_house || same_dynasty || is_child || is_parent || is_spouse || is_sibling;
                const char* relation = is_child ? "child"
                                     : is_parent ? "parent"
                                     : is_spouse ? "spouse"
                                     : is_sibling ? "sibling"
                                     : same_house ? "same_house"
                                     : same_dynasty ? "same_dynasty"
                                     : "unrelated";
                r.key("is_kin");       r.value_bool(is_kin);
                r.key("same_house");   r.value_bool(same_house);
                r.key("same_dynasty"); r.value_bool(same_dynasty);
                r.key("relation");     r.value_string(relation);
            }
        }
        // else: no controlled ruler → omit kin/relation (honest)
    }

    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. skills\n"); fflush(g_logfile); }
    r.key("skills"); r.begin_object();
    r.key("diplomacy");   r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_DIPLOMACY));
    r.key("martial");     r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_MARTIAL));
    r.key("stewardship"); r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_STEWARDSHIP));
    r.key("intrigue");    r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_INTRIGUE));
    r.key("learning");    r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_LEARNING));
    r.key("prowess");     r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_PROWESS));
    r.end_object();

    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. traits\n"); fflush(g_logfile); }
    r.key("traits"); r.begin_array();
    {
        uintptr_t trait_array_ptr = safe_read<uintptr_t>(c + ck3::character::TRAIT_ARRAY);
        int32_t trait_count = safe_read<int32_t>(c + ck3::character::TRAIT_COUNT);
        uintptr_t tdb = safe_read_offset<uintptr_t>(ck3::globals::TRAIT_DB);
        if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. traits: ptr=%lx count=%d tdb=%lx\n",
                trait_array_ptr, trait_count, tdb); fflush(g_logfile); }

        if (trait_array_ptr && is_mapped(trait_array_ptr)
                && trait_count > 0 && trait_count < 200
                && tdb && is_mapped(tdb)) {
            int32_t max_trait_id = safe_read<int32_t>(tdb + ck3::trait_db::ARRAY_COUNT);
            uintptr_t trait_table = safe_read<uintptr_t>(tdb + ck3::trait_db::ARRAY_PTR);

            for (int32_t i = 0; i < trait_count; i++) {
                uint32_t trait_id = safe_read<uint32_t>(trait_array_ptr + i * 4);
                r.begin_object();
                r.key("id"); r.value_uint(trait_id);
                // Resolve trait name via db_object pattern (safe memory read, no function call)
                if (trait_table && is_mapped(trait_table) && trait_id < static_cast<uint32_t>(max_trait_id)) {
                    uintptr_t trait_entry = safe_read<uintptr_t>(trait_table + trait_id * 8);
                    if (trait_entry && is_mapped(trait_entry)) {
                        const char* tname = nullptr;
                        size_t tname_len = 0;
                        if (read_db_obj_name(trait_entry, tname, tname_len)) {
                            r.key("name"); r.value_string(tname, tname_len);
                        }
                        uint8_t category = safe_read<uint8_t>(trait_entry + ck3::trait_entry::CATEGORY);
                        r.key("category"); r.value_string(trait_category_name(category));

                        // Enhanced trait metadata
                        uint32_t flags = safe_read<uint32_t>(trait_entry + ck3::trait_entry::FLAGS);
                        r.key("flags"); r.value_uint(flags);
                        r.key("is_genetic"); r.value_bool((flags & ck3::trait_flags::GENETIC) != 0);
                        r.key("is_physical"); r.value_bool((flags & ck3::trait_flags::PHYSICAL) != 0);
                        r.key("is_education"); r.value_bool((flags & ck3::trait_flags::EDUCATION_TYPE) != 0);
                        r.key("is_incapacitating"); r.value_bool((flags & ck3::trait_flags::INCAPACITATING) != 0);

                        // Opinion modifiers
                        int32_t same_op = safe_read<int32_t>(trait_entry + ck3::trait_entry::SAME_OPINION);
                        int32_t opp_op = safe_read<int32_t>(trait_entry + ck3::trait_entry::OPPOSITE_OPINION);
                        if (same_op != 0) { r.key("same_opinion"); r.value_int(same_op); }
                        if (opp_op != 0) { r.key("opposite_opinion"); r.value_int(opp_op); }

                        // Inheritance
                        int32_t inherit = safe_read<int32_t>(trait_entry + ck3::trait_entry::INHERIT_CHANCE);
                        if (inherit > 0) { r.key("inherit_chance"); r.value_int(inherit); }

                        // Level tracks (commander traits)
                        uint32_t lt_count = safe_read<uint32_t>(trait_entry + ck3::trait_entry::LEVEL_TRACK_PACKED);
                        if (lt_count > 0 && lt_count < 100) {
                            r.key("level_track_count"); r.value_uint(lt_count);
                        }
                    }
                }
                r.end_object();
            }
        }
    }
    r.end_array();

    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. personality\n"); fflush(g_logfile); }
    // BUG-10 fix: IS_AI_FLAG at 0x199 was always 0. Use AI controller pointer
    // in core_data as a more reliable indicator (non-null = AI-controlled).
    // lane-query item 3: shared with search_characters (char_is_ai_controlled).
    bool is_ai = char_is_ai_controlled(c);
    r.key("is_ai"); r.value_bool(is_ai);
    r.key("personality"); r.begin_object();
    {
        static const char* p_axis_names[] = {
            "honor", "greed", "rationality", "energy", "boldness",
            "zeal", "vengefulness", "compassion", "sociability", "dominance"
        };
        uintptr_t core_data_p = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
        uintptr_t personality_ptr_p = (core_data_p && is_mapped(core_data_p))
            ? safe_read<uintptr_t>(core_data_p + ck3::core_data::PERSONALITY_PTR) : 0;
        // ck-86ue.2: replaced inline bounds check with is_mapped() — equivalent
        // low-bound rejection plus confirmation of /proc/self/maps coverage.
        if (personality_ptr_p && is_mapped(personality_ptr_p)) {
            // Check personality locks — report locked values (what AI actually sees)
            uint32_t p_cid = id & 0xFFFFFF;
            PersonalityLock* pl_ptr = nullptr;
            {
                std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
                auto it = g_personality_locks.find(p_cid);
                if (it != g_personality_locks.end()) pl_ptr = &it->second;
            }
            for (int pi = 0; pi < 10; pi++) {
                int8_t val;
                if (pl_ptr && pl_ptr->locked[pi]) {
                    val = pl_ptr->axes[pi];
                } else {
                    val = safe_read<int8_t>(personality_ptr_p + pi);
                }
                r.key(p_axis_names[pi]); r.value_int(static_cast<int>(val));
            }
        }
    }
    r.end_object();

    // Family data: spouse, children, father, mother (safe memory reads only)
    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. family\n"); fflush(g_logfile); }
    uintptr_t family_ptr = safe_read<uintptr_t>(c + ck3::character::FAMILY_DATA);
    // ck-86ue.2: gate family_ptr once at the head — every downstream reader
    // (spouses, parents, children) depends on it. Replaces ad-hoc safe_ptr()
    // on spouse_data and silent truthy checks on parent/child readers below.
    bool family_ok = family_ptr && is_mapped(family_ptr);

    // Spouses via family sub-struct + SPOUSE_ARRAY (R8V-1 canonical path)
    // CK3 supports multiple spouses/concubines — iterate entire array
    {
        uint32_t first_alive_spouse = 0;
        r.key("spouses");
        r.begin_array();
        if (family_ok) {
            uintptr_t spouse_arr = family_ptr + ck3::family::SPOUSE_ARRAY;
            int32_t spouse_count = safe_read<int32_t>(spouse_arr + 0x0C);
            uintptr_t spouse_data = safe_read<uintptr_t>(spouse_arr + 0x00);
            if (spouse_data && is_mapped(spouse_data) && spouse_count > 0 && spouse_count <= 10) {
                for (int i = 0; i < spouse_count; i++) {
                    uint32_t spouse_ref = safe_read<uint32_t>(spouse_data + i * sizeof(uint32_t));
                    uint32_t spouse_id = spouse_ref & 0xFFFFFF;
                    if (spouse_id == 0 || spouse_id == 0xFFFFFF) continue;

                    // Check if spouse is still alive (dead spouses persist in the array)
                    void* spouse_char = lookup_character(spouse_id);
                    bool alive = spouse_char && g_char_is_alive && g_char_is_alive(spouse_char);

                    r.begin_object();
                    r.key("id"); r.value_uint(spouse_id);
                    r.key("alive"); r.value_bool(alive);
                    r.end_object();

                    if (alive && first_alive_spouse == 0) {
                        first_alive_spouse = spouse_id;
                    }
                }
            }
        }
        r.end_array();

        // Backward compatibility: first alive spouse (or null)
        r.key("spouse_id");
        if (first_alive_spouse != 0) {
            r.value_uint(first_alive_spouse);
        } else {
            r.value_null();
        }
    }

    // Father/mother via family sub-struct (CONFIRMED, HIGH)
    if (family_ok) {
        uint32_t father = safe_read<uint32_t>(family_ptr + ck3::family::FATHER_ID);
        uint32_t mother = safe_read<uint32_t>(family_ptr + ck3::family::MOTHER_ID);
        r.key("father_id");
        if (father != 0xFFFFFFFF) r.value_uint(father & 0xFFFFFF); else r.value_null();
        r.key("mother_id");
        if (mother != 0xFFFFFFFF) r.value_uint(mother & 0xFFFFFF); else r.value_null();
    } else {
        r.key("father_id"); r.value_null();
        r.key("mother_id"); r.value_null();
    }

    // Children via family sub-struct + CHILDREN_ARRAY (R8V-1 canonical path)
    // PDX array of uint32 character IDs (4-byte stride)
    r.key("children"); r.begin_array();
    if (family_ok) {
        uintptr_t child_arr = family_ptr + ck3::family::CHILDREN_ARRAY;
        int32_t child_count = safe_read<int32_t>(child_arr + 0x0C);
        uintptr_t child_data = safe_read<uintptr_t>(child_arr + 0x00);
        if (child_count > 0 && child_data && is_mapped(child_data) && child_count < 50) {
            for (int i = 0; i < child_count; i++) {
                // Elements are uint32 char IDs, not pointers
                uint32_t cid = safe_read<uint32_t>(child_data + i * sizeof(uint32_t)) & 0xFFFFFF;
                if (cid != 0 && cid != 0xFFFFFF) {
                    r.value_uint(cid);
                }
            }
        }
    }
    r.end_array();

    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. realm\n"); fflush(g_logfile); }
    r.key("liege_id");  r.value_uint(get_liege_id_from_addr(c));
    // ck-6kvq.27. The INDEPENDENT ruler at the head of this character's liege
    // chain — an independent character walks to their OWN id (:2109), and 0
    // means the walk could not be made. `liege_id` alone is one hop and a
    // sub-vassal's one hop is not a side; english_side_v1 keys on the TOP.
    r.key("top_liege_id"); r.value_uint(get_top_realm_ruler_id(c));

    // Engine's canonical IsRuler: char+0x1B8 (TITLE_REALM_DATA) != 0
    uintptr_t title_data = resolve_title_realm(c);
    bool is_ruler = (title_data != 0);

    r.key("has_realm"); r.value_bool(is_ruler);
    // ck-86ue.2: title_data is already validated by resolve_title_realm()
    // (which carries is_mapped() from ck-86ue.1). Downstream DB-traversal
    // pointers below pick up the same is_mapped() pattern.
    if (title_data) {
        // domain_size = number of directly held counties (tier == 2 only)
        // Iterate held title array and filter by tier, same approach as do_query_realm().
        int32_t bp_title_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        uintptr_t bp_title_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
        int bp_domain_size = 0;
        if (bp_title_ptr && is_mapped(bp_title_ptr) && bp_title_count > 0 && bp_title_count < 500) {
            uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
            if (db && is_mapped(db)) {
                uintptr_t table = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
                uint32_t capacity = safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT);
                if (table && is_mapped(table) && capacity > 0) {
                    for (int32_t i = 0; i < bp_title_count; i++) {
                        uint32_t tid = safe_read<uint32_t>(bp_title_ptr + i * 4);
                        uint32_t masked = tid & 0xFFFFFF;
                        if (masked >= capacity) continue;
                        uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                           + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
                        if (!entry || !is_mapped(entry)) continue;
                        uintptr_t type_ptr = safe_read<uintptr_t>(entry + ck3::title_obj::TITLE_TYPE_PTR);
                        if (!type_ptr || !is_mapped(type_ptr)) continue;
                        int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
                        if (tier == 2) {
                            bp_domain_size++;
                        }
                    }
                }
            }
        }
        r.key("domain_size");       r.value_int(bp_domain_size);
        r.key("realm_size");        r.value_int(bp_title_count);
        // Read vassal count from the direct vassal array (PDX array at DIRECT_VASSAL_ARRAY).
        // Count is at offset 0x0C within the array header.
        {
            uintptr_t vassal_arr = title_data + ck3::title_realm::DIRECT_VASSAL_ARRAY;
            int32_t vassal_cnt = safe_read<int32_t>(vassal_arr + 0x0C);
            if (vassal_cnt < 0 || vassal_cnt > 500) vassal_cnt = 0;
            r.key("vassal_count"); r.value_int(vassal_cnt);
        }
        r.key("military_strength"); r.value_int(safe_read<int32_t>(title_data + ck3::title_realm::MIL_CURRENT));

        // Primary title = first element of held title array (NOT 0x1F8 which is capital barony)
        int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
        if (title_arr_ptr && is_mapped(title_arr_ptr) && title_arr_count > 0) {
            uint32_t primary_tid = safe_read<uint32_t>(title_arr_ptr);
            r.key("primary_title_id"); r.value_uint(primary_tid & 0xFFFFFF);
            // lookup_title_obj already carries is_mapped() guards (ck-86ue.1).
            uintptr_t tobj = lookup_title_obj(primary_tid);
            if (tobj) {
                auto display_name = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!display_name.empty()) {
                    r.key("primary_title_name"); r.value_string(display_name.c_str(), display_name.size());
                }
                uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
                if (type_ptr && is_mapped(type_ptr)) {
                    auto title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                    if (!title_key.empty()) {
                        r.key("primary_title_key"); r.value_string(title_key.c_str(), title_key.size());
                    }
                    int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
                    if (tier >= 1 && tier <= 5) {
                        r.key("ruler_tier"); r.value_int(tier);
                    }
                }
            }
        }
    } else {
        r.key("domain_size");       r.value_int(0);
        r.key("realm_size");        r.value_int(0);
        r.key("vassal_count");      r.value_int(0);
        r.key("military_strength"); r.value_int(0);
    }
    if (verbose && g_logfile) { fprintf(g_logfile, "[CK3-Bridge] .. done\n"); fflush(g_logfile); }
}

// query_character: detailed character profile
bool do_query_character(const char* params_json, std::string* out) {
    safe_read_clear();
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_character: character not found\n");
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    build_char_profile(r, character);
    if (safe_read_had_fault()) {
        r.key("degraded");    r.value_bool(true);
        r.key("fault_count"); r.value_int(safe_read_fault_count());
        log_degraded_read("query_character");
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
        char addr_buf[24];
        snprintf(addr_buf, sizeof(addr_buf), "0x%lx", safe_read_last_fault());
        r.key("fault_addr");  r.value_string(addr_buf);
#endif
    }
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// query_opinion: get opinion score between two characters
// UNSAFE: calls GET_OPINION_OF game function (must go through on_tick dispatch)
bool do_query_opinion(const char* params_json, std::string* out) {
    int actor_id = json_get_int(params_json, "actor_id");
    int target_id = json_get_int(params_json, "target_id");
    if (actor_id < 0 || target_id < 0) {
        if (out) *out = "{\"error\":\"missing actor_id or target_id\"}";
        return false;
    }

    void* actor = lookup_character(static_cast<uint32_t>(actor_id));
    void* target = lookup_character(static_cast<uint32_t>(target_id));
    if (!actor || is_sentinel(actor)) {
        if (out) *out = "{\"error\":\"actor not found\"}";
        return false;
    }
    if (!target || is_sentinel(target)) {
        if (out) *out = "{\"error\":\"target not found\"}";
        return false;
    }

    using fn_opinion = int(*)(void*, void*);
    static fn_opinion get_opinion = reinterpret_cast<fn_opinion>(
        bridge::game_fn(ck3::funcs::GET_OPINION_OF));
    int opinion = get_opinion(actor, target);
    if (opinion < -200 || opinion > 200) opinion = 0;  // sanity clamp

    ResponseBuilder r;
    r.begin_object();
    r.key("actor_id");  r.value_uint(static_cast<uint32_t>(actor_id));
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id));
    r.key("opinion");   r.value_int(opinion);

    // Also get reverse opinion (target's opinion of actor)
    int reverse_opinion = get_opinion(target, actor);
    if (reverse_opinion < -200 || reverse_opinion > 200) reverse_opinion = 0;
    r.key("reverse_opinion"); r.value_int(reverse_opinion);
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// query_opinion_breakdown: detailed opinion modifiers between two characters
// SAFE: pure memory reads, no game function calls (except GET_OPINION_OF for total)
bool do_query_opinion_breakdown(const char* params_json, std::string* out) {
    int actor_id = json_get_int(params_json, "actor_id");
    int target_id = json_get_int(params_json, "target_id");
    if (actor_id < 0 || target_id < 0) {
        if (out) *out = "{\"error\":\"missing actor_id or target_id\"}";
        return false;
    }

    void* actor = lookup_character(static_cast<uint32_t>(actor_id));
    void* target = lookup_character(static_cast<uint32_t>(target_id));
    if (!actor || is_sentinel(actor)) {
        if (out) *out = "{\"error\":\"actor not found\"}";
        return false;
    }
    if (!target || is_sentinel(target)) {
        if (out) *out = "{\"error\":\"target not found\"}";
        return false;
    }

    // Get total opinion via engine function
    using fn_opinion = int(*)(void*, void*);
    static fn_opinion get_opinion = reinterpret_cast<fn_opinion>(
        bridge::game_fn(ck3::funcs::GET_OPINION_OF));
    int total_opinion = get_opinion(actor, target);
    if (total_opinion < -200 || total_opinion > 200) total_opinion = 0;

    // Get current game date for date formatting
    int32_t current_date_raw = 0;
    {
        uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
        if (gs) current_date_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    }
    auto format_date = [](int32_t raw_date, char* buf, size_t bufsz) {
        if (raw_date <= 0) { snprintf(buf, bufsz, "none"); return; }
        ck3::game_date::format_raw(raw_date, buf, bufsz);  // ck-05mg
    };

    uintptr_t actor_ptr = reinterpret_cast<uintptr_t>(actor);
    uintptr_t target_ptr = reinterpret_cast<uintptr_t>(target);

    // Get target's char ID from target_char+0x18 (used as binary search key)
    uint32_t target_char_id = safe_read<uint32_t>(target_ptr + 0x18);

    // Get actor's core_data (= *(actor + 0x1A8))
    uintptr_t core_data = safe_read<uintptr_t>(actor_ptr + ck3::character::REALM_DATA);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_opinion_breakdown");
    r.key("actor_id");  r.value_uint(static_cast<uint32_t>(actor_id));
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id));
    r.key("total_opinion"); r.value_int(total_opinion);

    r.key("active_modifiers"); r.begin_array();
    int modifier_count = 0;

    if (core_data && (core_data && is_mapped(core_data))) {
        // Per-target opinion entries at core_data+0x08 (sorted array)
        // Layout: ptr at +0x08, count at +0x14, each entry stride 0x10
        uintptr_t opinion_arr = safe_read<uintptr_t>(core_data + 0x08);
        int32_t opinion_entry_count = safe_read<int32_t>(core_data + 0x14);
        if (opinion_entry_count < 0 || opinion_entry_count > 5000) opinion_entry_count = 0;

        // Binary search for the target character's entry
        uintptr_t pair_container = 0;
        if (opinion_arr && (opinion_arr && is_mapped(opinion_arr)) && opinion_entry_count > 0) {
            int lo = 0, hi = opinion_entry_count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                uintptr_t entry_base = opinion_arr + mid * 0x10;
                uint32_t entry_key = safe_read<uint32_t>(entry_base);
                if (entry_key == target_char_id) {
                    // Found — the per-pair container ptr is at entry_base+0x08
                    pair_container = safe_read<uintptr_t>(entry_base + 0x08);
                    break;
                } else if (entry_key < target_char_id) {
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
        }

        if (pair_container && (pair_container && is_mapped(pair_container))) {
            // CActiveOpinion* array at pair_container+0x08, count at +0x14
            uintptr_t ao_arr = safe_read<uintptr_t>(pair_container + 0x08);
            int32_t ao_count = safe_read<int32_t>(pair_container + 0x14);
            if (ao_count < 0 || ao_count > 500) ao_count = 0;

            if (ao_arr && (ao_arr && is_mapped(ao_arr)) && ao_count > 0) {
                for (int32_t i = 0; i < ao_count; i++) {
                    uintptr_t ao_ptr = safe_read<uintptr_t>(ao_arr + i * 8);
                    if (!ao_ptr || (!ao_ptr || !is_mapped(ao_ptr))) continue;

                    // Read CActiveOpinion fields
                    uintptr_t modifier_def = safe_read<uintptr_t>(ao_ptr + active_opinion::MODIFIER_PTR);
                    int32_t current_value = safe_read<int32_t>(ao_ptr + active_opinion::CURRENT_OPINION);
                    int32_t base_value = safe_read<int32_t>(ao_ptr + active_opinion::BASE_OPINION);
                    int32_t start_date = safe_read<int32_t>(ao_ptr + active_opinion::START_DATE);
                    int32_t end_date = safe_read<int32_t>(ao_ptr + active_opinion::END_DATE);

                    // Sanity check values
                    if (current_value < -500 || current_value > 500) continue;
                    if (base_value < -500 || base_value > 500) continue;

                    r.begin_object();
                    r.key("value"); r.value_int(current_value);
                    r.key("base_value"); r.value_int(base_value);

                    // Read modifier name from def+0x10 (std::string)
                    if (modifier_def && (modifier_def && is_mapped(modifier_def))) {
                        std::string mod_name = ad_read_std_string(modifier_def + opinion_modifier::NAME_KEY);
                        if (!mod_name.empty()) {
                            r.key("modifier"); r.value_string(mod_name.c_str(), mod_name.size());
                        } else {
                            r.key("modifier"); r.value_string("unknown");
                        }

                        // Read decay flag from the modifier definition
                        uint8_t is_decaying = safe_read<uint8_t>(modifier_def + opinion_modifier::IS_DECAYING);
                        r.key("decaying"); r.value_bool(is_decaying != 0);
                    } else {
                        r.key("modifier"); r.value_string("unknown");
                        r.key("decaying"); r.value_bool(false);
                    }

                    // Format dates
                    char date_buf[32];
                    if (start_date > 0) {
                        format_date(start_date, date_buf, sizeof(date_buf));
                        r.key("start_date"); r.value_string(date_buf);
                    } else {
                        r.key("start_date"); r.value_null();
                    }
                    if (end_date > 0) {
                        format_date(end_date, date_buf, sizeof(date_buf));
                        r.key("end_date"); r.value_string(date_buf);
                    } else {
                        r.key("end_date"); r.value_null();
                    }

                    r.end_object();
                    modifier_count++;
                }
            }

            // Scripted relations at pair_container+0x20, count at +0x2c, stride 0x28
            uintptr_t sr_arr = safe_read<uintptr_t>(pair_container + 0x20);
            int32_t sr_count = safe_read<int32_t>(pair_container + 0x2c);
            if (sr_count < 0 || sr_count > 200) sr_count = 0;

            if (sr_arr && (sr_arr && is_mapped(sr_arr)) && sr_count > 0) {
                for (int32_t i = 0; i < sr_count; i++) {
                    uintptr_t sr_base = sr_arr + i * 0x28;

                    // Scripted relation entries contribute opinion via their modifier ref
                    // Entry layout: +0x00=type_ptr, +0x08=modifier_ptr or opinion value
                    uintptr_t sr_type_ptr = safe_read<uintptr_t>(sr_base);
                    if (!sr_type_ptr || (!sr_type_ptr || !is_mapped(sr_type_ptr))) continue;

                    // Try reading a name from the type object
                    std::string rel_name = ad_read_std_string(sr_type_ptr + ck3::db_object::NAME_KEY);

                    // Read opinion contribution — typically at +0x20 in the entry
                    int32_t sr_opinion = safe_read<int32_t>(sr_base + 0x20);
                    if (sr_opinion < -500 || sr_opinion > 500 || sr_opinion == 0) continue;

                    r.begin_object();
                    r.key("value"); r.value_int(sr_opinion);
                    r.key("base_value"); r.value_int(sr_opinion);
                    if (!rel_name.empty()) {
                        r.key("modifier"); r.value_string(rel_name.c_str(), rel_name.size());
                    } else {
                        r.key("modifier"); r.value_string("scripted_relation");
                    }
                    r.key("decaying"); r.value_bool(false);
                    r.key("start_date"); r.value_null();
                    r.key("end_date"); r.value_null();
                    r.end_object();
                    modifier_count++;
                }
            }
        }
    }

    r.end_array();
    r.key("modifier_count"); r.value_int(modifier_count);
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// query_imprisonment_reasons: check if actor has imprisonment reasons against target
// Returns: list of opinion modifiers that grant imprisonment_reason (EOpinionFlag bit 0)
// Also checks hooks and strong hooks as alternative imprisonment paths.
// SAFE: pure memory reads, same pattern as query_opinion_breakdown
bool do_query_imprisonment_reasons(const char* params_json, std::string* out) {
    int actor_id = json_get_int(params_json, "actor_id");
    int target_id = json_get_int(params_json, "target_id");
    if (actor_id < 0 || target_id < 0) {
        if (out) *out = "{\"error\":\"missing actor_id or target_id\"}";
        return false;
    }

    void* actor = lookup_character(static_cast<uint32_t>(actor_id));
    void* target = lookup_character(static_cast<uint32_t>(target_id));
    if (!actor || is_sentinel(actor)) {
        if (out) *out = "{\"error\":\"actor not found\"}";
        return false;
    }
    if (!target || is_sentinel(target)) {
        if (out) *out = "{\"error\":\"target not found\"}";
        return false;
    }

    uintptr_t actor_ptr = reinterpret_cast<uintptr_t>(actor);
    uintptr_t target_ptr = reinterpret_cast<uintptr_t>(target);
    uint32_t target_char_id = safe_read<uint32_t>(target_ptr + 0x18);

    // Get actor's core_data (= *(actor + 0x1A8))
    uintptr_t core_data = safe_read<uintptr_t>(actor_ptr + ck3::character::REALM_DATA);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_imprisonment_reasons");
    r.key("actor_id");  r.value_uint(static_cast<uint32_t>(actor_id));
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id));

    bool has_any_reason = false;
    int reason_count = 0;

    r.key("reasons"); r.begin_array();

    if (core_data && (core_data && is_mapped(core_data))) {
        // Per-target opinion entries at core_data+0x08 (sorted array)
        uintptr_t opinion_arr = safe_read<uintptr_t>(core_data + 0x08);
        int32_t opinion_entry_count = safe_read<int32_t>(core_data + 0x14);
        if (opinion_entry_count < 0 || opinion_entry_count > 5000) opinion_entry_count = 0;

        // Binary search for the target character's entry
        uintptr_t pair_container = 0;
        if (opinion_arr && (opinion_arr && is_mapped(opinion_arr)) && opinion_entry_count > 0) {
            int lo = 0, hi = opinion_entry_count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                uintptr_t entry_base = opinion_arr + mid * 0x10;
                uint32_t entry_key = safe_read<uint32_t>(entry_base);
                if (entry_key == target_char_id) {
                    pair_container = safe_read<uintptr_t>(entry_base + 0x08);
                    break;
                } else if (entry_key < target_char_id) {
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
        }

        if (pair_container && (pair_container && is_mapped(pair_container))) {
            // CActiveOpinion* array at pair_container+0x08, count at +0x14
            uintptr_t ao_arr = safe_read<uintptr_t>(pair_container + 0x08);
            int32_t ao_count = safe_read<int32_t>(pair_container + 0x14);
            if (ao_count < 0 || ao_count > 500) ao_count = 0;

            if (ao_arr && (ao_arr && is_mapped(ao_arr)) && ao_count > 0) {
                for (int32_t i = 0; i < ao_count; i++) {
                    uintptr_t ao_ptr = safe_read<uintptr_t>(ao_arr + i * 8);
                    if (!ao_ptr || (!ao_ptr || !is_mapped(ao_ptr))) continue;

                    uintptr_t modifier_def = safe_read<uintptr_t>(ao_ptr + active_opinion::MODIFIER_PTR);
                    if (!modifier_def || (!modifier_def || !is_mapped(modifier_def))) continue;

                    // Read reason flags at modifier_def+0x48 (uint16 bitmask)
                    uint16_t reason_flags = safe_read<uint16_t>(modifier_def + opinion_modifier::REASON_FLAGS);

                    // Check all reason flag bits
                    if (reason_flags == 0) continue;

                    // Read modifier name
                    std::string mod_name = ad_read_std_string(modifier_def + opinion_modifier::NAME_KEY);

                    if (reason_flags & opinion_modifier::FLAG_IMPRISONMENT_REASON) {
                        r.begin_object();
                        r.key("type"); r.value_string("imprisonment_reason");
                        if (!mod_name.empty()) {
                            r.key("modifier"); r.value_string(mod_name.c_str(), mod_name.size());
                        }
                        int32_t current_value = safe_read<int32_t>(ao_ptr + active_opinion::CURRENT_OPINION);
                        r.key("opinion_value"); r.value_int(current_value);
                        r.end_object();
                        has_any_reason = true;
                        reason_count++;
                    }
                    if (reason_flags & opinion_modifier::FLAG_BANISH_REASON) {
                        r.begin_object();
                        r.key("type"); r.value_string("banish_reason");
                        if (!mod_name.empty()) {
                            r.key("modifier"); r.value_string(mod_name.c_str(), mod_name.size());
                        }
                        r.end_object();
                        reason_count++;
                    }
                    if (reason_flags & opinion_modifier::FLAG_EXECUTE_REASON) {
                        r.begin_object();
                        r.key("type"); r.value_string("execute_reason");
                        if (!mod_name.empty()) {
                            r.key("modifier"); r.value_string(mod_name.c_str(), mod_name.size());
                        }
                        r.end_object();
                        reason_count++;
                    }
                    if (reason_flags & opinion_modifier::FLAG_REVOKE_TITLE_REASON) {
                        r.begin_object();
                        r.key("type"); r.value_string("revoke_title_reason");
                        if (!mod_name.empty()) {
                            r.key("modifier"); r.value_string(mod_name.c_str(), mod_name.size());
                        }
                        r.end_object();
                        reason_count++;
                    }
                    if (reason_flags & opinion_modifier::FLAG_DIVORCE_REASON) {
                        r.begin_object();
                        r.key("type"); r.value_string("divorce_reason");
                        if (!mod_name.empty()) {
                            r.key("modifier"); r.value_string(mod_name.c_str(), mod_name.size());
                        }
                        r.end_object();
                        reason_count++;
                    }
                    if (reason_flags & opinion_modifier::FLAG_OBEDIENCE_REASON) {
                        r.begin_object();
                        r.key("type"); r.value_string("obedience_reason");
                        if (!mod_name.empty()) {
                            r.key("modifier"); r.value_string(mod_name.c_str(), mod_name.size());
                        }
                        r.end_object();
                        reason_count++;
                    }
                }
            }
        }
    }

    r.end_array(); // reasons

    r.key("has_imprisonment_reason"); r.value_bool(has_any_reason);
    r.key("reason_count"); r.value_int(reason_count);

    // Also check if actor is liege of target (required for imprisonment to work)
    bool actor_is_liege = false;
    {
        uintptr_t target_realm = safe_read<uintptr_t>(target_ptr + ck3::character::REALM_DATA);
        if (target_realm) {
            // Liege check: target's landed sub-struct at +0x1B8, then title_realm+0x1C0
            uintptr_t title_realm = safe_read<uintptr_t>(target_ptr + 0x1B8);
            if (title_realm && (title_realm && is_mapped(title_realm))) {
                uintptr_t liege_ptr = safe_read<uintptr_t>(title_realm + 0x1C0);
                if (liege_ptr && (liege_ptr && is_mapped(liege_ptr))) {
                    uint32_t liege_id = safe_read<uint32_t>(liege_ptr + 0x18);
                    uint32_t actor_char_id = safe_read<uint32_t>(actor_ptr + 0x18);
                    actor_is_liege = (liege_id == actor_char_id);
                }
            }
        }
    }
    r.key("actor_is_liege"); r.value_bool(actor_is_liege);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// Forward decl — true vassal-inclusive realm county count (defined below, after
// the get_top_realm_ruler_id liege walker it depends on).
static int count_realm_counties(uintptr_t ruler_addr, uint32_t ruler_id);

// query_realm: realm structure for a character
bool do_query_realm(const char* params_json, std::string* out) {
    // ck-86ue.2: All derived-pointer hops now guarded with is_mapped().
    // resolve_title_realm/lookup_title_obj/lookup_character carry their own
    // is_mapped() guards from ck-86ue.1; the body adds is_mapped() at every
    // intermediate hop (rd/core_data, title_data sub-arrays, vassal data).
    safe_read_clear();
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        return false;
    }

    uintptr_t c_ptr = reinterpret_cast<uintptr_t>(character);
    // char_get_realm uses unsafe safe_read<>; replace with safe_read + is_mapped.
    uintptr_t rd = safe_read<uintptr_t>(c_ptr + ck3::character::REALM_DATA);
    if (rd && !is_mapped(rd)) rd = 0;  // treat unmapped same as missing
    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_get_id(character) & 0xFFFFFF);

    // Engine's canonical IsRuler: char+0x1B8 (TITLE_REALM_DATA) != 0
    uintptr_t title_data_check = safe_read<uintptr_t>(c_ptr + ck3::character::TITLE_REALM_DATA);
    bool is_ruler = (title_data_check != 0);

    if (!is_ruler) {
        r.key("has_realm"); r.value_bool(false);
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    uintptr_t title_data = resolve_title_realm(c_ptr);

    r.key("has_realm");        r.value_bool(true);

    // Domain size: count held titles with tier == 2 (counties only = demesne)
    // Uses title_realm+0x1E0 (PDX array of held title IDs) with title DB lookup for tier filtering
    // NOTE: lookup_title_obj can fault if title DB pointers are stale. Since this runs
    // from the safe-query reader thread (no SIGSEGV recovery), we use a simple fallback:
    // try the tier-filtered count, but fall back to raw title count if any pointer is bad.
    int domain_size = 0;
    int held_title_count_raw = 0;
    if (title_data) {
        uintptr_t tdata = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
        int32_t tcnt = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        held_title_count_raw = (tcnt > 0 && tcnt < 500) ? tcnt : 0;
        if (tdata && is_mapped(tdata) && held_title_count_raw > 0) {
            uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
            if (db && is_mapped(db)) {
                uintptr_t table = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
                uint32_t capacity = safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT);
                if (table && is_mapped(table) && capacity > 0) {
                    for (int32_t i = 0; i < held_title_count_raw; i++) {
                        uint32_t tid = safe_read<uint32_t>(tdata + i * 4);
                        uint32_t masked = tid & 0xFFFFFF;
                        if (masked >= capacity) continue;
                        uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                           + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
                        if (!entry || !is_mapped(entry)) continue;
                        uintptr_t type_ptr = safe_read<uintptr_t>(entry + ck3::title_obj::TITLE_TYPE_PTR);
                        if (!type_ptr || !is_mapped(type_ptr)) continue;
                        int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
                        if (tier == 2) {
                            domain_size++;
                        }
                    }
                }
            }
        }
    }
    r.key("domain_size"); r.value_int(domain_size);

    // Domain limit: cached value at title_realm + 0x3D0
    int dl = title_data ? std::max(1, safe_read<int32_t>(title_data + ck3::title_realm::DOMAIN_LIMIT_CACHED)) : 0;
    r.key("domain_limit"); r.value_int(dl);

    // realm_size: DIRECTLY-held title count (all tiers). NOTE: this is NOT a
    // realm-growth proxy — it stays flat when a vassal conquers more land, since
    // the vassal's titles never enter THIS ruler's held-title array. For "total
    // land under authority" use realm_total_counties below (ck-realmsize).
    r.key("realm_size");       r.value_int(title_data ? safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT) : 0);
    r.key("held_title_count"); r.value_int(title_data ? safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT) : 0);
    // realm_total_counties: TRUE vassal-inclusive realm size — every county under
    // this ruler's authority (own demesne + every vassal's counties, recursively).
    // GROWS when ANY vassal grows. Use THIS as the realm-growth metric, not
    // realm_size. Computed via a top-liege enumeration (count_realm_counties).
    {
        uint32_t ruler_id = char_get_id(character) & 0xFFFFFF;
        r.key("realm_total_counties"); r.value_int(count_realm_counties(c_ptr, ruler_id));
    }
    r.key("military_strength");r.value_int(title_data ? safe_read<int32_t>(title_data + ck3::title_realm::MIL_CURRENT) : 0);

    // Primary title = first element of held title array (title_realm+0x1E0).
    // NOTE: title_realm+0x1F8 is the CAPITAL BARONY, not the primary (highest-tier) title.
    if (title_data) {
        int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
        uint32_t primary_tid = 0xFFFFFFFF;
        if (title_arr_ptr && is_mapped(title_arr_ptr) && title_arr_count > 0) {
            primary_tid = safe_read<uint32_t>(title_arr_ptr);  // first element = primary title
        }
        uint32_t masked_tid = primary_tid & 0xFFFFFF;

        // Also emit capital barony for reference
        uint32_t capital_barony_tid = safe_read<uint32_t>(title_data + ck3::title_realm::CAPITAL_BARONY_ID);
        if (capital_barony_tid != 0xFFFFFFFF) {
            r.key("capital_barony_id"); r.value_uint(capital_barony_tid & 0xFFFFFF);
        }

        if (primary_tid != 0xFFFFFFFF) {
            r.key("primary_title_id"); r.value_uint(masked_tid);

            // Look up title in DB to get name, tier, and display name
            // lookup_title_obj already carries is_mapped() guards (ck-86ue.1).
            uintptr_t title_obj = lookup_title_obj(primary_tid);
            if (title_obj) {
                // Title display name — custom/localized name at title+0x58 (std::string, GCC SSO)
                auto display_name = ad_read_std_string(title_obj + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!display_name.empty()) {
                    r.key("primary_title_name"); r.value_string(display_name.c_str(), display_name.size());
                }

                // Title key — from the title TYPE definition's NAME_KEY at +0x10
                uintptr_t type_ptr = safe_read<uintptr_t>(title_obj + ck3::title_obj::TITLE_TYPE_PTR);
                if (type_ptr && is_mapped(type_ptr)) {
                    auto title_name = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                    if (!title_name.empty()) {
                        r.key("primary_title_key"); r.value_string(title_name.c_str(), title_name.size());
                    }
                    int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
                    if (tier >= 1 && tier <= 5) {
                        r.key("title_tier"); r.value_int(tier);
                    }
                }
            }
        }

        // Also emit ruler tier from title_realm directly (0x1D4)
        int ruler_tier = safe_read<int32_t>(title_data + ck3::title_realm::TIER);
        if (ruler_tier >= 1 && ruler_tier <= 7) {
            r.key("ruler_tier"); r.value_int(ruler_tier);
        }
    }

    // Direct vassals from ruler_data + 0xa8 (PDX array: data_ptr at +0x00, count at +0x0C)
    r.key("vassals"); r.begin_array();
    int32_t direct_vassal_count = 0;
    if (title_data) {
        uintptr_t vassal_arr = title_data + ck3::title_realm::DIRECT_VASSAL_ARRAY;
        uintptr_t vassal_data_ptr = safe_read<uintptr_t>(vassal_arr);
        int32_t vassal_cnt = safe_read<int32_t>(vassal_arr + 0x0C);
        if (vassal_data_ptr && is_mapped(vassal_data_ptr) && vassal_cnt > 0 && vassal_cnt < 500) {
            for (int32_t i = 0; i < vassal_cnt; i++) {
                uint32_t vid = safe_read<uint32_t>(vassal_data_ptr + i * 4);
                void* vassal_char = lookup_character(vid);
                if (!vassal_char || is_sentinel(vassal_char)) continue;
                uintptr_t vc = reinterpret_cast<uintptr_t>(vassal_char);
                if (g_char_is_alive && !g_char_is_alive(vassal_char)) continue;
                r.begin_object();
                r.key("id"); r.value_uint(vid & 0xFFFFFF);
                // Read name from character
                std::string vname = ad_read_char_name(vc);
                if (!vname.empty()) {
                    r.key("name"); r.value_string(vname.c_str(), vname.size());
                }
                // Military strength for quick reference
                uintptr_t their_title_data = safe_read<uintptr_t>(vc + ck3::character::TITLE_REALM_DATA);
                if (their_title_data && is_mapped(their_title_data)) {
                    r.key("military_strength"); r.value_int(safe_read<int32_t>(their_title_data + ck3::title_realm::MIL_CURRENT));
                }
                r.end_object();
                direct_vassal_count++;
            }
        }
    }
    r.end_array();
    r.key("vassal_count"); r.value_int(direct_vassal_count);
    // Also emit the raw PDX array count for debugging
    r.key("vassal_count_raw"); r.value_int(title_data ? safe_read<int32_t>(title_data + ck3::title_realm::DIRECT_VASSAL_ARRAY + 0x0C) : 0);
    r.key("vassal_limit"); r.value_int(title_data ? safe_read<int32_t>(title_data + ck3::title_realm::VASSAL_LIMIT_CACHED) : 0);

    // War count from title_realm WAR_REF_ARRAY (PDX array: count at +0x0C)
    if (title_data) {
        uintptr_t war_arr = title_data + ck3::title_realm::WAR_REF_ARRAY;
        int32_t wc = safe_read<int32_t>(war_arr + 0x0C);
        r.key("war_count"); r.value_int(wc > 0 ? wc : 0);
    } else {
        r.key("war_count"); r.value_int(0);
    }

    // Income from core_data
    r.key("income"); r.begin_object();
    r.key("monthly");     r.value_float(static_cast<float>(safe_read<int64_t>(rd + ck3::core_data::MONTHLY_INCOME)) / 100000.0f);
    r.key("accumulated"); r.value_float(static_cast<float>(safe_read<int64_t>(rd + ck3::core_data::INCOME_ACCUMULATOR)) / 100000.0f);
    r.end_object();

    if (safe_read_had_fault()) {
        r.key("degraded");    r.value_bool(true);
        r.key("fault_count"); r.value_int(safe_read_fault_count());
        log_degraded_read("query_realm");
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
        char addr_buf[24];
        snprintf(addr_buf, sizeof(addr_buf), "0x%lx", safe_read_last_fault());
        r.key("fault_addr");  r.value_string(addr_buf);
#endif
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_succession: succession info for a character
// Round 3: heir list at *(*(char+0x1a8)+0x1b8), count at +0x1c4
// Round 6 P4: these are TITLE IDs, not character IDs. FUN_0674ae20
// (GetLineOfSuccession) looks up each title_id in the landed title DB.
// Max heirs capped by DAT_0c3a86c8.
// ─────────────────────────────────────────────────────────────────────────────
// Effective per-title succession rule (order/division/rank), resolved the CORRECT
// way per re_per_title_succession.md §0/§2/§3. The OLD code read raw bytes at
// title+0x380/+0x384 as if title+0x380 were an inline 6-byte SSuccessionRule — but
// title+0x380 is a CPdxVector<CTitleSuccessionLaw*> (data ptr @ +0x380, count @
// +0x38C), so those bytes were the LOW BYTES OF THE VECTOR DATA POINTER (garbage).
//
// Correct resolution (PURE READ — no engine call, every deref is_mapped-gated):
//   • title-scoped laws: walk the SUCC_LAW_VECTOR_DATA[] (count = SUCC_LAW_VECTOR_COUNT),
//     reading each law element's SSuccessionRule VALUE at (elem + 0xB08). Each title
//     law only sets the axes it defines; merge non-"unset" axes onto the output.
//   • 0-custom title (count==0): the effective rule is the HOLDER's REALM law —
//     walk title_realm_laws::ACTIVE_LAW_ARRAY (holder_title_realm + 0x200, count
//     +0x208), reading each CLaw*'s SSuccessionRule at (CLaw* + 0xB08), merging
//     succession-relevant axes (order/division/rank) where defined.
// This mirrors init_succession_rule_from_title's merge without calling it.
//
// 0x09=unset order, 0x02=unset division/rank are the engine's "axis not set"
// sentinels; we treat them as "do not override".
struct EffSuccRule {
    uint8_t order; uint8_t division; uint8_t rank;
    // query_succession's other axes (P5: it read title+0x381.. raw, the vector's pointer bytes)
    uint8_t traversal = 3; uint8_t gender = 5; uint8_t faith = 3;
};

static void merge_rule_value(EffSuccRule& acc, uintptr_t rule_base) {
    if (!rule_base || !is_mapped(rule_base)) return;
    uint8_t o = safe_read<uint8_t>(rule_base + ck3::title_obj::SUCC_RULE_ORDER);
    uint8_t d = safe_read<uint8_t>(rule_base + ck3::title_obj::SUCC_RULE_DIVISION);
    uint8_t k = safe_read<uint8_t>(rule_base + ck3::title_obj::SUCC_RULE_RANK);
    if (o != 9) acc.order = o;        // 9 = unset order
    if (d != 2) acc.division = d;     // 2 = unset division
    if (k != 2) acc.rank = k;         // 2 = unset rank
    uint8_t t = safe_read<uint8_t>(rule_base + ck3::title_obj::SUCC_RULE_TRAVERSAL);
    uint8_t g = safe_read<uint8_t>(rule_base + ck3::title_obj::SUCC_RULE_GENDER);
    uint8_t f = safe_read<uint8_t>(rule_base + ck3::title_obj::SUCC_RULE_FAITH_REQ);
    if (t != 3) acc.traversal = t;    // 3 = unset traversal
    if (g != 5) acc.gender = g;       // 5 = unset gender
    if (f != 3) acc.faith = f;        // 3 = unset faith requirement
}

// holder_title_realm: the *(char+0x1B8) title_realm of the title's holder, used
// for the realm-law fallback. Pass 0 to skip the fallback (still safe).
static EffSuccRule resolve_effective_succ_rule(uintptr_t tptr, uintptr_t holder_title_realm) {
    EffSuccRule acc = { 9 /*unset order*/, 2 /*unset div*/, 2 /*unset rank*/ };
    if (!tptr || !is_mapped(tptr)) return acc;

    int32_t law_count = safe_read<int32_t>(tptr + ck3::title_obj::SUCC_LAW_VECTOR_COUNT);
    uintptr_t law_arr = safe_read<uintptr_t>(tptr + ck3::title_obj::SUCC_LAW_VECTOR_DATA);
    if (law_count < 0 || law_count > 64) law_count = 0;  // sanity clamp
    if (law_arr && !is_mapped(law_arr)) { law_arr = 0; law_count = 0; }

    if (law_count > 0 && law_arr) {
        // title carries its own succession law(s): merge their value structs.
        for (int32_t i = 0; i < law_count; i++) {
            uintptr_t elem = safe_read<uintptr_t>(law_arr + i * sizeof(uintptr_t));
            if (!elem || elem < 0x10000 || !is_mapped(elem)) continue;
            merge_rule_value(acc, elem + ck3::title_obj::SUCC_LAW_RULE_AT_ELEM);
        }
        return acc;
    }

    // 0-custom title: effective rule = holder's REALM succession law.
    if (holder_title_realm && is_mapped(holder_title_realm)) {
        uintptr_t active_arr = safe_read<uintptr_t>(holder_title_realm + ck3::title_realm_laws::ACTIVE_LAW_ARRAY);
        int32_t active_cnt   = safe_read<int32_t>(holder_title_realm + ck3::title_realm_laws::ACTIVE_LAW_COUNT);
        if (active_cnt < 0 || active_cnt > 512) active_cnt = 0;
        if (active_arr && is_mapped(active_arr)) {
            for (int32_t i = 0; i < active_cnt; i++) {
                uintptr_t claw = safe_read<uintptr_t>(active_arr + i * sizeof(uintptr_t));
                if (!claw || claw < 0x10000 || !is_mapped(claw)) continue;
                merge_rule_value(acc, claw + ck3::realm_law_entry::SUCCESSION_RULES);
            }
        }
    }
    return acc;
}

// The effective rule of a title, its realm-law fallback taken from the title's own
// holder. Every succession reader goes through this (P5: seven handlers read the
// deprecated title+0x380.. bytes, which are the law vector's data pointer).
static EffSuccRule title_succ_rule(uintptr_t tptr) {
    uintptr_t holder_tr = 0;
    if (tptr && is_mapped(tptr)) {
        uint32_t h = safe_read<uint32_t>(tptr + ck3::title_obj::HOLDER_CHAR_ID);
        void* holder = (h && h != 0xFFFFFFFF) ? lookup_character(h) : nullptr;
        if (holder && !is_sentinel(holder)) holder_tr = resolve_title_realm(reinterpret_cast<uintptr_t>(holder));
    }
    return resolve_effective_succ_rule(tptr, holder_tr);
}

bool do_query_succession(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_succession: character not found\n");
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;
    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_id);

    // Get title_realm data (char+0x1b8), with fallback probing
    uintptr_t title_data = resolve_title_realm(c);
    if (!title_data) {
        r.key("succession_order"); r.value_string("none");
        r.key("gender_law"); r.value_string("none");
        r.key("title_division"); r.value_string("none");
        r.key("designated_heir_id"); r.value_int(-1);
        r.key("titles"); r.begin_array(); r.end_array();
        r.key("note"); r.value_string("Character is unlanded (no title_realm data)");
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    // Get held title array from title_realm+0x1E0 (same pattern as query_realm)
    uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (title_arr_count < 0 || title_arr_count > 200) title_arr_count = 0;

    // Helper: decode succession order byte to string
    auto decode_succ_order = [](uint8_t v) -> const char* {
        switch (v) {
            case 0: return "inheritance";
            case 1: return "election";
            case 2: return "appointment";
            case 8: return "noble_family";
            case 9: return "unset";
            default: return "unknown";
        }
    };
    auto decode_gender_law = [](uint8_t v) -> const char* {
        switch (v) {
            case 0: return "male_only";
            case 1: return "male_preference";
            case 2: return "equal";
            case 3: return "female_preference";
            case 4: return "female_only";
            case 5: return "unset";
            default: return "unknown";
        }
    };
    auto decode_division = [](uint8_t v) -> const char* {
        switch (v) {
            case 0: return "single_heir";
            case 1: return "partition";
            case 2: return "unset";
            default: return "unknown";
        }
    };
    auto decode_traversal = [](uint8_t v) -> const char* {
        switch (v) {
            case 0: return "children";
            case 3: return "unset";
            default: return "unknown";
        }
    };
    auto decode_rank = [](uint8_t v) -> const char* {
        switch (v) {
            case 0: return "oldest_first";
            case 1: return "youngest_first";
            case 2: return "unset";
            default: return "unknown";
        }
    };

    // Find the primary (highest-tier) title and read its succession rules
    int best_tier = -1;
    uintptr_t best_title_ptr = 0;
    uint32_t best_title_id = 0;

    for (int32_t i = 0; i < title_arr_count && title_arr_ptr; i++) {
        uint32_t tid = safe_read<uint32_t>(title_arr_ptr + i * sizeof(uint32_t));
        uintptr_t tptr = ad_db_lookup(ck3::globals::LANDED_TITLE_DB, tid);
        if (!tptr) continue;
        uintptr_t type_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::TITLE_TYPE_PTR);
        if (!type_ptr) continue;
        int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
        if (tier > best_tier) {
            best_tier = tier;
            best_title_ptr = tptr;
            best_title_id = tid;
        }
    }

    // Emit primary title succession rules
    if (best_title_ptr) {
        // The effective rule: the title's own succession laws, else the holder's
        // realm law (resolve_effective_succ_rule). The raw title+0x380.. bytes read
        // here before P5 were the law vector's data pointer (they differed per run).
        const EffSuccRule eff = resolve_effective_succ_rule(best_title_ptr, resolve_title_realm(c));
        uint8_t order     = eff.order;
        uint8_t traversal = eff.traversal;
        uint8_t gender    = eff.gender;
        uint8_t rank      = eff.rank;
        uint8_t division  = eff.division;
        uint8_t faith_req = eff.faith;

        std::string tname = ad_read_std_string(best_title_ptr + ck3::db_object::NAME_KEY);
        r.key("primary_title_id"); r.value_uint(best_title_id & 0xFFFFFF);
        if (!tname.empty()) {
            r.key("primary_title_key"); r.value_string(tname.c_str(), tname.size());
        }
        r.key("primary_title_tier"); r.value_int(best_tier);

        r.key("succession_order"); r.value_string(decode_succ_order(order));
        r.key("traversal_order"); r.value_string(decode_traversal(traversal));
        r.key("gender_law"); r.value_string(decode_gender_law(gender));
        r.key("rank_order"); r.value_string(decode_rank(rank));
        r.key("title_division"); r.value_string(decode_division(division));
        r.key("faith_requirement"); r.value_int(faith_req);

        // Succession order raw bytes for debugging
        r.key("succession_raw"); r.begin_array();
        r.value_int(order); r.value_int(traversal); r.value_int(gender);
        r.value_int(rank); r.value_int(division); r.value_int(faith_req);
        r.end_array();

        // Designated heir on primary title
        int32_t designated = safe_read<int32_t>(best_title_ptr + ck3::title_obj::DESIGNATED_HEIR_ID);
        r.key("designated_heir_id"); r.value_int(designated == -1 ? -1 : (designated & 0xFFFFFF));
        if (designated != -1 && designated != 0) {
            uintptr_t heir_ptr = reinterpret_cast<uintptr_t>(
                lookup_character(static_cast<uint32_t>(designated)));
            if (heir_ptr && !is_sentinel(reinterpret_cast<void*>(heir_ptr))) {
                std::string hname = ad_read_char_name(heir_ptr);
                if (!hname.empty()) {
                    r.key("designated_heir_name"); r.value_string(hname.c_str(), hname.size());
                }
            }
        }

        // Top-level primary heir: index 0 of the primary title's cached heir list.
        // Mirrors the per-title heir loop below (same offsets, same lookup_character +
        // ad_read_char_name, same sentinel/empty-array guards).
        {
            uintptr_t primary_heir_arr = safe_read<uintptr_t>(best_title_ptr + ck3::title_obj::SUCCESSION_HEIRS);
            int32_t primary_heir_count = safe_read<int32_t>(best_title_ptr + ck3::title_obj::SUCCESSION_HEIR_COUNT);
            if (primary_heir_count > 0 && primary_heir_arr && primary_heir_arr >= 0x10000) {
                uint32_t heir_ref = safe_read<uint32_t>(primary_heir_arr);
                uint32_t heir_cid = heir_ref & 0xFFFFFF;
                if (heir_cid != 0 && heir_cid != 0xFFFFFF) {
                    void* heir_char = lookup_character(heir_ref);
                    if (heir_char && !is_sentinel(heir_char)) {
                        r.key("primary_heir_id"); r.value_uint(heir_cid);
                        std::string primary_heir_name = ad_read_char_name(
                            reinterpret_cast<uintptr_t>(heir_char));
                        if (!primary_heir_name.empty()) {
                            r.key("primary_heir_name");
                            r.value_string(primary_heir_name.c_str(), primary_heir_name.size());
                        }
                    }
                }
            }

            // #97: succession-stability companion fields. primary_heir_id is read off
            // position-0 of the engine's CACHED heir array (SUCCESSION_HEIRS), which
            // re-orders EQUAL-PRIORITY candidates between recomputes (e.g. two adult
            // sons under single_heir + male_only): the id at slot 0 oscillates
            // round-to-round while the realm still passes WHOLE to one heir. These
            // derived fields let the agent assert on the stable invariant instead of
            // the volatile id. Computed from values already in hand (division read at
            // SUCC_DIVISION above, primary_heir_count here) — NO new memory reads, NO
            // engine calls.
            //   succession_stable      : top title passes intact to a single heir.
            //   primary_heir_confidence: high  = single_heir & exactly one heir (id trustworthy)
            //                            low   = single_heir & >1 heir (slot-0 id may oscillate)
            //                            n/a   = partition / unset (id identity not the right signal)
            bool realm_intact = (division == 0 /*single_heir*/) && (primary_heir_count >= 1);
            r.key("succession_stable"); r.value_bool(realm_intact);
            const char* heir_conf;
            if (division == 0 && primary_heir_count == 1) {
                heir_conf = "high";
            } else if (division == 0 && primary_heir_count > 1) {
                heir_conf = "low";
            } else {
                heir_conf = "n/a";
            }
            r.key("primary_heir_confidence"); r.value_string(heir_conf);
            if (heir_conf[0] == 'l') {  // "low"
                r.key("heir_note"); r.value_string(
                    "primary_heir_id may oscillate among equal-priority heirs; realm "
                    "still passes intact under single_heir -- assert on "
                    "succession_stable, not heir id");
            }
        }
    } else {
        r.key("succession_order"); r.value_string("none");
        r.key("gender_law"); r.value_string("none");
        r.key("title_division"); r.value_string("none");
        r.key("designated_heir_id"); r.value_int(-1);
    }

    // Per-title succession info for all held titles (with cached heir lists from R15)
    r.key("titles"); r.begin_array();
    for (int32_t i = 0; i < title_arr_count && title_arr_ptr; i++) {
        uint32_t tid = safe_read<uint32_t>(title_arr_ptr + i * sizeof(uint32_t));
        uintptr_t tptr = ad_db_lookup(ck3::globals::LANDED_TITLE_DB, tid);
        if (!tptr) continue;

        uintptr_t type_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::TITLE_TYPE_PTR);
        int tier = type_ptr ? safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER) : 0;

        // Skip baronies — too many and succession rules are inherited from county
        if (tier <= 1) continue;

        r.begin_object();
        r.key("title_id"); r.value_uint(tid & 0xFFFFFF);

        // Title key from type definition
        if (type_ptr) {
            std::string tname = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
            if (!tname.empty()) {
                r.key("title_key"); r.value_string(tname.c_str(), tname.size());
            }
        }
        // Display name from title object
        std::string display_name = ad_read_std_string(tptr + ck3::title_obj::TITLE_DISPLAY_NAME);
        if (!display_name.empty()) {
            r.key("title_name"); r.value_string(display_name.c_str(), display_name.size());
        }
        r.key("tier"); r.value_int(tier);

        uint8_t order     = title_succ_rule(tptr).order;
        uint8_t gender    = title_succ_rule(tptr).gender;
        uint8_t division  = title_succ_rule(tptr).division;

        r.key("succession_order"); r.value_string(decode_succ_order(order));
        r.key("gender_law"); r.value_string(decode_gender_law(gender));
        r.key("title_division"); r.value_string(decode_division(division));

        int32_t designated = safe_read<int32_t>(tptr + ck3::title_obj::DESIGNATED_HEIR_ID);
        if (designated != -1 && designated != 0) {
            r.key("designated_heir_id"); r.value_int(designated & 0xFFFFFF);
        }

        // Cached heir list (R15: title+0x290 = ptr to uint32[] heir refs, title+0x29C = count)
        // This is a PDX array — data pointer at +0x290, count at +0x29C
        uintptr_t heir_arr_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::SUCCESSION_HEIRS);
        int32_t heir_count = safe_read<int32_t>(tptr + ck3::title_obj::SUCCESSION_HEIR_COUNT);
        if (heir_count < 0) heir_count = 0;
        if (heir_count > 50) heir_count = 50;  // sanity cap
        r.key("heir_count"); r.value_int(heir_count);
        r.key("heirs"); r.begin_array();

        int max_heirs = (heir_count > 10) ? 10 : heir_count;
        if (!heir_arr_ptr || heir_arr_ptr < 0x10000) max_heirs = 0;
        for (int hi = 0; hi < max_heirs; ++hi) {
            uint32_t heir_ref = safe_read<uint32_t>(heir_arr_ptr
                                                + static_cast<uintptr_t>(hi) * 4);
            uint32_t heir_cid = heir_ref & 0xFFFFFF;
            if (heir_cid == 0 || heir_cid == 0xFFFFFF) continue;

            // Resolve heir character for name
            void* heir_char = lookup_character(heir_ref);
            if (!heir_char || is_sentinel(heir_char)) continue;

            uintptr_t hc = reinterpret_cast<uintptr_t>(heir_char);
            std::string heir_name = ad_read_char_name(hc);

            r.begin_object();
            r.key("position"); r.value_int(hi + 1);
            r.key("character_id"); r.value_uint(heir_cid);
            if (!heir_name.empty()) {
                r.key("name"); r.value_string(heir_name.c_str(), heir_name.size());
            }
            r.end_object();
        }

        r.end_array();
        r.end_object();
    }
    r.end_array();

    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// do_query_title_succession_map — the PARTITION MAP keyed on a CHARACTER.
//
// For a given character_id, returns one entry PER held title (tier > 1) with:
//   title_key, tier, projected_heir_id, projected_heir_name,
//   succession_law (single_heir | partition | elective | appointment | …),
//   splits_away (true when this title's projected heir DIFFERS from the
//                primary/highest-tier title's projected heir).
//
// This is the single-heir-vs-scatter view the agent needs at a glance: a realm
// passes WHOLE when every entry has splits_away=false; any splits_away=true entry
// is a title that breaks off to a cadet at the next succession.
//
// PURE READ. No engine call, no alloc, no name-cache drain, no RNG-scope or
// pause requirement. Every pointer read goes through safe_read<T> (crash-proof
// process_vm_readv) and every dereferenceable base is is_mapped()-gated before
// use. Heir projection comes from the engine's OWN materialized per-title cache
// (title+0x290 ordered TPdxRef[], count at +0x29C, mask 0xFFFFFF) — never the
// computing path (build_title_heir_list / ExecuteSuccession), which would
// allocate and touch succession scratch. See re_per_title_succession.md §4 and
// re_succession_preview_audit.md.
bool do_query_title_succession_map(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_title_succession_map: character not found\n");
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_title_succession_map");
    r.key("character_id"); r.value_uint(char_id);

    uintptr_t title_data = resolve_title_realm(c);
    if (!title_data || !is_mapped(title_data)) {
        r.key("titles"); r.begin_array(); r.end_array();
        r.key("primary_heir_id"); r.value_int(-1);
        r.key("any_splits"); r.value_bool(false);
        r.key("note"); r.value_string("Character is unlanded (no title_realm data)");
        r.key("status"); r.value_string("ok");
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (title_arr_count < 0 || title_arr_count > 200) title_arr_count = 0;
    if (title_arr_ptr && !is_mapped(title_arr_ptr)) { title_arr_ptr = 0; title_arr_count = 0; }

    auto decode_law = [](uint8_t order, uint8_t division) -> const char* {
        // order: 0=inheritance 1=election 2=appointment 8=noble_family 9=unset
        // division (only meaningful for inheritance): 0=single_heir 1=partition 2=unset
        switch (order) {
            case 1: return "elective";
            case 2: return "appointment";
            case 8: return "noble_family";
            case 0:
                switch (division) {
                    case 0: return "single_heir";
                    case 1: return "partition";
                    case 2: return "unset";
                    default: return "unknown";
                }
            case 9: return "unset";
            default: return "unknown";
        }
    };

    // Helper: read position-0 (next holder) of a title's CACHED heir array.
    // Returns 0 when the cache is empty/invalid. Pure read.
    auto projected_heir = [](uintptr_t tptr) -> uint32_t {
        if (!tptr || !is_mapped(tptr)) return 0;
        uintptr_t heir_arr = safe_read<uintptr_t>(tptr + ck3::title_obj::SUCCESSION_HEIRS);
        int32_t heir_count = safe_read<int32_t>(tptr + ck3::title_obj::SUCCESSION_HEIR_COUNT);
        if (heir_count <= 0 || !heir_arr || heir_arr < 0x10000 || !is_mapped(heir_arr)) return 0;
        uint32_t ref = safe_read<uint32_t>(heir_arr) & 0xFFFFFF;
        if (ref == 0 || ref == 0xFFFFFF) return 0;
        return ref;
    };

    // Pass 1: find the primary (highest-tier) held title and its projected heir.
    int best_tier = -1;
    uintptr_t best_title_ptr = 0;
    uint32_t best_title_id = 0;
    for (int32_t i = 0; i < title_arr_count && title_arr_ptr; i++) {
        uint32_t tid = safe_read<uint32_t>(title_arr_ptr + i * sizeof(uint32_t));
        uintptr_t tptr = ad_db_lookup(ck3::globals::LANDED_TITLE_DB, tid);
        if (!tptr || !is_mapped(tptr)) continue;
        uintptr_t type_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::TITLE_TYPE_PTR);
        if (!type_ptr || !is_mapped(type_ptr)) continue;
        int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
        if (tier > best_tier) {
            best_tier = tier;
            best_title_ptr = tptr;
            best_title_id = tid;
        }
    }

    uint32_t primary_heir_id = best_title_ptr ? projected_heir(best_title_ptr) : 0;
    r.key("primary_title_id"); r.value_int(best_title_ptr ? static_cast<int>(best_title_id & 0xFFFFFF) : -1);
    r.key("primary_title_tier"); r.value_int(best_tier);
    if (primary_heir_id) {
        r.key("primary_heir_id"); r.value_uint(primary_heir_id & 0xFFFFFF);
        void* ph = lookup_character(primary_heir_id);
        if (ph && !is_sentinel(ph)) {
            std::string n = ad_read_char_name(reinterpret_cast<uintptr_t>(ph));
            if (!n.empty()) { r.key("primary_heir_name"); r.value_string(n.c_str(), n.size()); }
        }
    } else {
        r.key("primary_heir_id"); r.value_int(-1);
    }

    // Pass 2: emit the partition map, one entry per held title (tier > 1).
    bool any_splits = false;
    r.key("titles"); r.begin_array();
    for (int32_t i = 0; i < title_arr_count && title_arr_ptr; i++) {
        uint32_t tid = safe_read<uint32_t>(title_arr_ptr + i * sizeof(uint32_t));
        uintptr_t tptr = ad_db_lookup(ck3::globals::LANDED_TITLE_DB, tid);
        if (!tptr || !is_mapped(tptr)) continue;

        uintptr_t type_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::TITLE_TYPE_PTR);
        int tier = (type_ptr && is_mapped(type_ptr))
                       ? safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER) : 0;
        if (tier <= 1) continue;  // baronies inherit from county; skip the noise

        r.begin_object();
        r.key("title_id"); r.value_uint(tid & 0xFFFFFF);
        if (type_ptr && is_mapped(type_ptr)) {
            std::string tkey = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
            if (!tkey.empty()) { r.key("title_key"); r.value_string(tkey.c_str(), tkey.size()); }
        }
        std::string display_name = ad_read_std_string(tptr + ck3::title_obj::TITLE_DISPLAY_NAME);
        if (!display_name.empty()) { r.key("title_name"); r.value_string(display_name.c_str(), display_name.size()); }
        r.key("tier"); r.value_int(tier);

        // CORRECTED (re_per_title_succession.md §0/§5): title+0x380 is a
        // CPdxVector<CTitleSuccessionLaw*>, NOT an inline SSuccessionRule. Reading
        // title+0x380/+0x384 directly returned the low bytes of the vector data
        // pointer (garbage), and for a 0-custom title (count==0) it read order=0/
        // division=0 → always reported "single_heir", ignoring the realm's actual
        // law (e.g. a confederate-partition realm looked single_heir on every
        // default title — the recurring run-killer). Resolve the effective rule by
        // merging title-scoped law value structs (elem+0xB08), falling back to the
        // holder's realm succession law. The holder of every title here is `c`, so
        // its title_realm (title_data) is the realm-law container.
        EffSuccRule eff = resolve_effective_succ_rule(tptr, title_data);
        uint8_t order    = eff.order;
        uint8_t division = eff.division;
        r.key("succession_law"); r.value_string(decode_law(order, division));
        r.key("title_division"); r.value_string(
            order >= 2 ? "n/a"   // non-inheritance order (elective/appointment/etc.): division not meaningful
                       : (division == 0 ? "single_heir" : (division == 1 ? "partition"
                                          : (division == 2 ? "unset" : "unknown"))));
        // rank distinguishes confederate (0) vs regular (1) partition.
        if (order < 2 && division == 1) {
            r.key("partition_rank"); r.value_string(
                eff.rank == 0 ? "confederate_partition"
                              : (eff.rank == 1 ? "regular_partition" : "unset"));
        }

        uint32_t heir_ref = projected_heir(tptr);
        uint32_t heir_cid = heir_ref & 0xFFFFFF;
        if (heir_cid != 0) {
            r.key("projected_heir_id"); r.value_uint(heir_cid);
            void* hc = lookup_character(heir_ref);
            if (hc && !is_sentinel(hc)) {
                std::string hn = ad_read_char_name(reinterpret_cast<uintptr_t>(hc));
                if (!hn.empty()) { r.key("projected_heir_name"); r.value_string(hn.c_str(), hn.size()); }
            }
        } else {
            r.key("projected_heir_id"); r.value_int(-1);
        }

        // splits_away: this title goes to a DIFFERENT heir than the primary title.
        // Only meaningful when both heirs are known and we have a primary to compare
        // against; an empty/unknown title heir or no-primary case is reported false.
        bool splits = (primary_heir_id != 0 && heir_cid != 0 && heir_cid != (primary_heir_id & 0xFFFFFF));
        if (splits) any_splits = true;
        r.key("splits_away"); r.value_bool(splits);

        r.end_object();
    }
    r.end_array();

    r.key("any_splits"); r.value_bool(any_splits);
    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// Helper: walk liege chain to find top-level INDEPENDENT ruler.
//
// FIX (#110 liege-walk, 2026-06-22): the old walk dereferenced the raw
// title_realm+0x448 LIEGE_PTR each hop and read +0x18 as the next liege id. That
// pointer is UNRELIABLE in this build (documented at get_liege_id_from_addr): for
// an INDEPENDENT ruler it is a non-null *mapped sentinel/economy* object whose id
// field reads 0xFFFFFF (and for a sub-vassal the walk silently stalls), so the old
// code returned 0xFFFFFF for independents and the holder's OWN id for sub-vassals —
// it NEVER reached the true independent top-liege. Core-dump validated against the
// 1097 save: 82/93 British county holders got the wrong top_liege (independent
// kings returned 16777215; sub-vassals of an independent king returned themselves).
//
// The fix iterates get_liege_id_from_addr() (PATH1 0x448-sentinel-reject + PATH2
// realm-hierarchy 0x1C0→0x28, the authoritative liege signal) hop by hop until a
// ruler with no liege is reached — that ruler IS the independent top-liege. A
// holder who is already independent returns its own id (get_liege_id_from_addr→0).
static uint32_t get_top_realm_ruler_id(uintptr_t char_addr) {
    if (!char_addr || !is_mapped(char_addr)) return 0;
    uint32_t current_id = safe_read<uint32_t>(char_addr + ck3::character::ID) & 0xFFFFFF;
    uintptr_t current = char_addr;
    std::set<uint32_t> seen;       // cycle guard (corrupt liege loop)
    for (int depth = 0; depth < 30; depth++) {  // safety limit
        uint32_t liege_id = get_liege_id_from_addr(current) & 0xFFFFFF;
        if (liege_id == 0 || liege_id == current_id) return current_id;  // no liege = independent top
        if (seen.count(liege_id)) return current_id;                     // cycle
        seen.insert(liege_id);
        void* next = lookup_character(liege_id);
        if (!next || is_sentinel(next)) return current_id;               // liege unresolvable
        current_id = liege_id;
        current = reinterpret_cast<uintptr_t>(next);
    }
    return current_id;
}

// Helper: count a single character's DIRECTLY-held tier-2 (county) titles.
// This is the "demesne county count" — pure safe_read walk of the held-title
// array (title_realm+0x1E0/+0x1EC) with a title-DB tier lookup, mirroring the
// domain_size loop in do_query_realm. Returns 0 on any unmapped chain.
static int count_demesne_counties(uintptr_t char_addr) {
    if (!char_addr || !is_mapped(char_addr)) return 0;
    uintptr_t td = safe_read<uintptr_t>(char_addr + ck3::character::TITLE_REALM_DATA);
    if (!td || !is_mapped(td)) return 0;
    uintptr_t tdata = safe_read<uintptr_t>(td + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t tcnt = safe_read<int32_t>(td + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (tcnt <= 0 || tcnt >= 500) return 0;
    if (!tdata || !is_mapped(tdata)) return 0;
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    if (!db || !is_mapped(db)) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT);
    if (!table || !is_mapped(table) || capacity == 0) return 0;
    int counties = 0;
    for (int32_t i = 0; i < tcnt; i++) {
        uint32_t tid = safe_read<uint32_t>(tdata + i * 4);
        uint32_t masked = tid & 0xFFFFFF;
        if (masked >= capacity) continue;
        uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                       + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
        if (!entry || !is_mapped(entry)) continue;
        uintptr_t type_ptr = safe_read<uintptr_t>(entry + ck3::title_obj::TITLE_TYPE_PTR);
        if (!type_ptr || !is_mapped(type_ptr)) continue;
        if (safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER) == 2)
            counties++;
    }
    return counties;
}

// Helper: TRUE total realm size = every county under a ruler's authority,
// recursively (own demesne + every direct/indirect vassal's held counties).
//
// ROOT CAUSE this fixes (ck-realmsize): `realm_size` had been emitted as
// title_realm+0x1EC (TITLE_ARRAY_COUNT) — the count of titles the ruler holds
// DIRECTLY (all tiers, incl. duchies/baronies). That number is flat when a
// vassal conquers more land (the vassal's titles never enter the ruler's own
// held-title array), so it is useless as a realm-growth proxy. Core-dump
// validated against ck3_20260625_152901_ivar867: Ivar's TITLE_ARRAY_COUNT=11
// but his realm is 5 counties (4 demesne + 1 vassal county).
//
// The engine's own recursive collector is COLLECT_REALM_TITLES (0x06779780),
// but it allocates + recurses through vassal-contract DB lookups and is not
// safe to call from the lock-free reader thread. Instead we replicate the
// "counties under authority" definition with a pure safe_read enumeration:
// for every landed AI character whose top-liege resolves to `ruler_id`, sum
// their directly-held county count; add the ruler's own demesne. top-liege is
// computed via get_top_realm_ruler_id (the authoritative PATH2 liege walk).
//
// Linear over the AI-character set (counts are bounded; each top-liege walk is
// depth-capped at 30). Fails toward the demesne-only count on any bad chain —
// never crashes, never blocks the query.
static int count_realm_counties(uintptr_t ruler_addr, uint32_t ruler_id) {
    ruler_id &= 0xFFFFFF;
    int total = count_demesne_counties(ruler_addr);   // own demesne
    enumerate_ai_characters([&](const CharacterInfo& info) -> bool {
        if (!info.alive || !info.has_realm) return true;
        uint32_t vid = info.id & 0xFFFFFF;
        if (vid == ruler_id) return true;              // self already counted
        uintptr_t vaddr = reinterpret_cast<uintptr_t>(info.raw_ptr);
        int vc = count_demesne_counties(vaddr);
        if (vc == 0) return true;                      // holds no counties — skip the liege walk
        if (get_top_realm_ruler_id(vaddr) == ruler_id)
            total += vc;
        return true;
    });
    return total;
}

// lane-query item 5: realm county counts for MANY rulers in one pass, for the
// `characters` listing. count_realm_counties (above) is right for one ruler but
// walks every character per call, so a 25-500 row listing would be quadratic;
// it also enumerates only AI-ENABLED characters (enumerate_ai_characters), so a
// vassal whose AI is off drops out of its liege's count.
//
// This walk reads CHARACTER_DB once (the same safe_read loop search_characters
// uses), takes every living landed character's DIRECTLY-held county count
// (count_demesne_counties) and credits it to that character AND to every liege
// above it (get_liege_id_from_addr, depth-capped, cycle-guarded). So for any
// ruler R, index[R] = counties held by R or by anyone in R's vassal tree:
//   * independent ruler: the same set count_realm_counties(R) counts
//     (= query(realm).realm_total_counties), plus any AI-disabled vassals;
//   * vassal: its own sub-realm (count_realm_counties gives demesne only).
// Pure reads, no engine calls. An unreadable chain just stops crediting upward.
static void build_realm_county_index(std::unordered_map<uint32_t, int>& index) {
    index.clear();
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (!db) return;
    uintptr_t tbl = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (!tbl || capacity == 0 || capacity > 500000) return;
    uintptr_t sentinel = safe_read_offset<uintptr_t>(ck3::globals::CHAR_DB_SENTINEL);
    for (uint32_t i = 0; i < capacity; i++) {
        uintptr_t c = safe_read<uintptr_t>(tbl + ck3::hash_map::ENTRY_HEADER_SIZE
                                           + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
        if (!c || c < 0x10000 || c > 0x7FFFFFFFFFFF || c == sentinel) continue;
        if (safe_read<uint8_t>(c + ck3::character::DEAD_FLAG) != 0) continue;
        uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
        if (!tr || !is_mapped(tr)) continue;
        int vc = count_demesne_counties(c);
        if (vc <= 0) continue;
        uint32_t cur_id = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;
        uintptr_t cur = c;
        uint32_t seen[32];
        int nseen = 0;
        for (int depth = 0; depth < 30 && cur_id != 0 && cur_id != 0xFFFFFF; depth++) {
            bool cycle = false;
            for (int k = 0; k < nseen; k++) if (seen[k] == cur_id) { cycle = true; break; }
            if (cycle) break;
            seen[nseen++] = cur_id;
            index[cur_id] += vc;
            uint32_t liege = get_liege_id_from_addr(cur) & 0xFFFFFF;
            if (liege == 0 || liege == 0xFFFFFF || liege == cur_id) break;
            void* next = lookup_character(liege);
            if (!next || is_sentinel(next)) break;
            cur = reinterpret_cast<uintptr_t>(next);
            cur_id = liege;
        }
    }
}

// =============================================================================
// do_query_dejure_progress — READ-ONLY de-jure-control meter (no engine calls)
//
// Answers the win-condition metric "how many of <title>'s de-jure counties does
// ruler R hold?" directly, replacing the hang-prone create_title-refusal probe
// (the create_title path freezes on the deferred I15 frozen-thread). Pure
// safe_read; never writes, never calls an engine function that can hang.
//
// REUSE (no offset duplication):
//   - title-by-key resolution: resolve_title_id_by_key (action_handlers.cpp) —
//     the same LANDED_TITLE_DB + NAME_KEY iteration create_title's title_key path
//     uses.
//   - de-jure county enumeration: collect_dejure_counties (intent_dispatch.cpp) —
//     the SUB_TITLE_ARRAY(0x258)/SUB_TITLE_COUNT(0x264) recursive walk the #131
//     create_title de-jure gate uses (total).
//   - per-county attribution: get_top_realm_ruler_id (this file) on each county's
//     HOLDER_CHAR_ID — identical top-realm-ruler walk count_realm_counties uses to
//     attribute a county to a ruler (held).
//
// Inputs:  character_id (the ruler), title_key (default "e_britannia") | title_id.
// Returns: {character_id, title_key, title_id, held, total, fraction}.
// Any per-county read failure (unmapped title / unresolvable holder) is skipped
// and counted as NOT-held; the loop never crashes and never blocks the query.
// =============================================================================
bool do_query_dejure_progress(const char* params_json, std::string* out) {
    safe_read_clear();

    uint32_t ruler_id = 0;
    {
        void* character = resolve_character(params_json);
        if (character && !is_sentinel(character))
            ruler_id = char_get_id(character) & 0xFFFFFF;
    }

    // Resolve the title: numeric title_id wins; else title_key (default e_britannia).
    uint32_t title_id = 0xFFFFFFFFu;
    std::string title_key;
    int title_id_raw = json_get_int(params_json, "title_id");
    if (title_id_raw > 0) {
        title_id = static_cast<uint32_t>(title_id_raw) & 0xFFFFFF;
    } else {
        const char* key = json_get_string(params_json, "title_key");
        if (!key || !key[0]) key = json_get_string(params_json, "title");
        title_key = (key && key[0]) ? key : "e_britannia";
        title_id  = resolve_title_id_by_key(title_key.c_str());
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(ruler_id);
    if (!title_key.empty()) { r.key("title_key"); r.value_string(title_key.c_str(), title_key.size()); }

    if (ruler_id == 0 || title_id == 0xFFFFFFFFu) {
        r.key("title_id");  r.value_int(-1);
        r.key("held");      r.value_int(0);
        r.key("total");     r.value_int(0);
        r.key("fraction");  r.value_double(0.0);
        r.key("error");     r.value_string(ruler_id == 0 ? "character_unresolved" : "title_key_not_found");
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    r.key("title_id"); r.value_uint(title_id);

    // O2 (mcp-redesign SPEC §7): when verbose=1, emit the per-county holder array so
    // the MCP layer can show "which N counties am I missing". Pure additive over the
    // same walk; default (no verbose) keeps the lean {held,total,fraction} shape.
    bool verbose = json_get_int(params_json, "verbose") != 0;

    // Resolve the top title's tier once — kingdoms/empires (tier>=4) WITH a real
    // de-jure tree must yield total>0; a transient 0 means we landed mid-tick while
    // the engine was clear-then-repopulating the children vector (de-jure drift /
    // title create/destroy). Retry the collect a couple times on a yield before
    // reporting 0 so an UNPAUSED caller can't observe the transient empty tree.
    // (re_dejure_meter_and_mint.md Fix B — defense-in-depth; Fix A is the gateway
    // routing this read PAUSED in ck3agent, which is the primary fix.)
    int top_tier = -1;
    {
        uintptr_t tobj = lookup_title_obj(title_id);
        if (tobj) {
            uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
            if (type_ptr && is_mapped(type_ptr))
                top_tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
        }
    }

    std::vector<uint32_t> counties, seen;
    collect_dejure_counties(title_id, counties, seen, 0);
    int total = static_cast<int>(counties.size());
    // SETTLE-UNTIL-STABLE (re_observe_meter_0_0.md §3A). A resolved tier>=4 title
    // with a 0-county tree is either genuinely titular (e.g. k_danelaw) or a
    // mid-repopulate transient: game_pause sets a flag but does NOT barrier an
    // in-flight tick, so an immediate read can race the engine's de-jure
    // clear-then-repopulate window (empirically ~1.8s) and catch a transient 0.
    // The old 2ms (1ms x2) retry always exhausted mid-repopulate.
    //
    // Poll until two CONSECUTIVE collects AGREE (stable across a re-read), not
    // "first non-zero," within a ~2s budget at ~50ms cadence. This spans the real
    // repopulate window and converges on the STABLE value (e.g. 9/93 once the tick
    // finishes). A genuinely titular kingdom converges on a STABLE 0 across two
    // reads and exits ON THE AGREEMENT CHECK almost immediately (no 2s stall) —
    // the agreement check makes over-budgeting harmless. Pure safe_read loop (no
    // engine calls → never the masked-SIGALRM hang); runs on the IPC thread, so it
    // never blocks the game thread.
    if (top_tier >= 4 /*kingdom or empire*/) {
        int prev = total;
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(2000);
        for (int stable = 0; stable < 2
                             && std::chrono::steady_clock::now() < deadline;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            counties.clear();
            seen.clear();
            collect_dejure_counties(title_id, counties, seen, 0);
            int t = static_cast<int>(counties.size());
            if (t == prev) { ++stable; }
            else           { stable = 0; prev = t; }
        }
        total = prev;
        // Re-collect at the settled value so the per-county held loop below sees
        // the SAME stable tree we converged on (prev tracks the count; refresh the
        // vector to match it for the holder enumeration).
        if (static_cast<int>(counties.size()) != total) {
            counties.clear(); seen.clear();
            collect_dejure_counties(title_id, counties, seen, 0);
            total = static_cast<int>(counties.size());
        }
    }
    int held  = 0;
    if (verbose) r.key("counties"), r.begin_array();
    for (uint32_t ctid : counties) {
        uintptr_t co = lookup_title_obj(ctid);
        uint32_t holder_id = 0;
        // ck-6kvq.27. The INDEPENDENT ruler at the head of this holder's liege
        // chain. The walk already happened here — `you_control` is exactly
        // "is that top liege the anchor ruler" — so emitting the id itself is
        // free, and it is the field the paper's english_side_v1 predicate keys
        // on. Without it a per-county record carries only a holder_id, and a
        // holder_id says nothing about which SIDE holds the county: over a
        // 20-year run most holders are characters who did not exist at episode
        // start (measured: 24 of 28 terminal holders in the first full E0), so
        // no start-of-episode table can classify them.
        uint32_t top_liege_id = 0;
        bool you_control = false;
        if (co) {
            uint32_t h = safe_read<uint32_t>(co + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
            if (h != 0 && h != 0xFFFFFF) {
                holder_id = h;
                void* hc = lookup_character(h);
                if (hc && !is_sentinel(hc)) {
                    top_liege_id = get_top_realm_ruler_id(reinterpret_cast<uintptr_t>(hc));
                    if (top_liege_id == ruler_id) {
                        you_control = true;
                        held++;
                    }
                }
            }
        }
        if (verbose) {
            r.begin_object();
            r.key("county_id");    r.value_uint(ctid);
            r.key("holder_id");    r.value_uint(holder_id);
            // 0 means "not resolvable", never "independent": an independent
            // holder's walk returns their OWN id (query_handlers.cpp:2109).
            r.key("top_liege_id"); r.value_uint(top_liege_id);
            r.key("you_control");  r.value_bool(you_control);
            r.end_object();
        }
    }
    if (verbose) r.end_array();

    double frac = (total > 0) ? static_cast<double>(held) / static_cast<double>(total) : 0.0;
    r.key("held");     r.value_int(held);
    r.key("total");    r.value_int(total);
    r.key("fraction"); r.value_double(frac);
    // Disambiguate the two zero-causes so the agent never conflates a genuine
    // titular kingdom (empty de-jure tree by design) with a mid-tick transient.
    // (re_dejure_meter_and_mint.md Fix B.)
    if (total == 0)
        r.key("note"), r.value_string("titular_or_transient_empty_tree");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// Monthly telemetry emitter (TASK B)
//
// Appends ONE compact JSONL line per game-MONTH for the controlled ruler to
// <telemetry>/gen_current.jsonl (per-instance since ck-qn03). Called from on_tick.
// Design constraints: cheap, crash-safe, never blocks the tick.
//  - Off the hot path: returns immediately unless the game MONTH changed since
//    the last emission (tracked in a file-static), so the body runs ~12x/game-yr.
//  - No engine function calls — pure safe_read of already-mapped structs (same
//    reads query_realm/query_character use); a stale pointer degrades to a 0/empty
//    field, never a fault.
//  - No-op when there is no controlled character (observer with nobody steered).
//  - All I/O wrapped; an fopen failure is swallowed (telemetry is best-effort).
// =============================================================================
void emit_monthly_telemetry(int32_t raw_date) {
    // --- Month-rollover gate (keeps this off the hot path) ---
    // raw_date is sub-day ticks (24/day) since DATE_EPOCH. Convert to an absolute
    // year*12+month index so we emit exactly once per game month. ck-05mg: via
    // the shared real-calendar helper — with 30-day months the rollover fired on
    // the wrong day and a phantom 13th month fired a 13th time per year.
    int32_t month_index = ck3::game_date::raw_to_month_index(raw_date);

    static int32_t s_last_month_index = INT32_MIN;
    if (raw_date == 0 || month_index == s_last_month_index) return;  // unchanged → cheap return
    s_last_month_index = month_index;

    // Past the gate: ~12x/game-year, so the full decode is cheap here.
    int year = 0, month = 0, day = 0;
    ck3::game_date::raw_to_ymd(raw_date, &year, &month, &day);

    // --- Controlled ruler ---
    void* ctrl = get_controlled_character();
    if (!ctrl || is_sentinel(ctrl)) return;
    uintptr_t c = reinterpret_cast<uintptr_t>(ctrl);
    if (!is_mapped(c)) return;
    uint32_t controlled_id = char_get_id(ctrl) & 0xFFFFFF;

    // --- Gather fields (all guarded safe_read; failures degrade to 0/empty) ---
    uintptr_t td = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);   // title_realm
    uintptr_t core = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);       // core_data

    int realm_total_counties = count_realm_counties(c, controlled_id);
    int realm_size = 0, held_titles = 0, military_strength = 0, faction_count = 0, tier = 0;
    bool at_war = false;
    std::string government, primary_title_key;
    if (td && is_mapped(td)) {
        realm_size        = safe_read<int32_t>(td + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (realm_size < 0 || realm_size > 10000) realm_size = 0;
        held_titles       = realm_size;
        military_strength = safe_read<int32_t>(td + ck3::title_realm::MIL_CURRENT);
        if (military_strength < 0) military_strength = 0;
        int32_t fc = safe_read<int32_t>(td + ck3::title_realm::FACTION_COUNT);
        faction_count = (fc > 0 && fc < 1000) ? fc : 0;
        int32_t wc = safe_read<int32_t>(td + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
        at_war = (wc > 0 && wc < 100);
        int32_t rt = safe_read<int32_t>(td + ck3::title_realm::TIER);
        if (rt >= 1 && rt <= 7) tier = rt;
        uintptr_t gov_obj = safe_read<uintptr_t>(td + ck3::title_realm::GOVERNMENT_TYPE_PTR);
        if (gov_obj && is_mapped(gov_obj)) government = ad_read_std_string(gov_obj + ck3::db_object::NAME_KEY);

        // Primary title key = first held title's type NAME_KEY.
        uintptr_t arr = safe_read<uintptr_t>(td + ck3::title_realm::TITLE_ARRAY_PTR);
        if (arr && is_mapped(arr) && realm_size > 0) {
            uint32_t ptid = safe_read<uint32_t>(arr);
            if (ptid != 0xFFFFFFFF) {
                uintptr_t tobj = lookup_title_obj(ptid);
                if (tobj) {
                    uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
                    if (type_ptr && is_mapped(type_ptr))
                        primary_title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                }
            }
        }
    }

    double gold = 0.0, prestige = 0.0;
    if (core && is_mapped(core)) {
        gold     = static_cast<double>(safe_read<int64_t>(core + ck3::core_data::GOLD)) / 100000.0;
        prestige = static_cast<double>(safe_read<int64_t>(core + ck3::core_data::PRESTIGE)) / 100000.0;
    }

    std::string faith;
    uint32_t faith_id = safe_read<uint32_t>(c + ck3::character::FAITH_ID_DIRECT);
    if (faith_id != 0 && faith_id != 0xFFFFFFFF) {
        uintptr_t fo = ad_db_lookup(ck3::globals::FAITH_DB, faith_id);
        if (fo) faith = ad_read_std_string(fo + faith::FAITH_KEY);
    }

    // --- Build the compact JSONL line via ResponseBuilder (consistent escaping) ---
    ResponseBuilder r;
    r.begin_object();
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%04d.%02d.%02d", year, month, day);
    r.key("date");                  r.value_string(date_str);
    r.key("controlled_id");         r.value_uint(controlled_id);
    r.key("realm_total_counties");  r.value_int(realm_total_counties);
    r.key("realm_size");            r.value_int(realm_size);
    r.key("held_titles");           r.value_int(held_titles);
    r.key("treasury");              r.value_float(gold);
    r.key("prestige");              r.value_float(prestige);
    if (!faith.empty())             { r.key("faith");      r.value_string(faith.c_str(), faith.size()); }
    if (!government.empty())        { r.key("government"); r.value_string(government.c_str(), government.size()); }
    r.key("tier");                  r.value_int(tier);
    if (!primary_title_key.empty()) { r.key("primary_title_key"); r.value_string(primary_title_key.c_str(), primary_title_key.size()); }
    r.key("faction_count");         r.value_int(faction_count);
    r.key("military_strength");     r.value_int(military_strength);
    r.key("at_war");                r.value_bool(at_war);
    r.end_object();
    std::string line = r.take();

    // --- Append to the JSONL file (best-effort; create the dir if needed) ---
    ::mkdir(bridge::instance_telemetry_dir(), bridge::instance_dir_mode());   // ck-qn03: per-instance
    std::string gen_path = std::string(bridge::instance_telemetry_dir()) + "/gen_current.jsonl";
    FILE* f = bridge::instance_fopen_private(gen_path.c_str(), "a");   // rc2 B1: 0600, O_NOFOLLOW
    if (f) {
        ::fputs(line.c_str(), f);
        ::fputc('\n', f);
        ::fclose(f);
    }
}

// Helper: emit neighbor ruler info into response
static void emit_neighbor_info(ResponseBuilder& r, void* character, void* neighbor_ptr,
                               uint32_t player_id, uint32_t our_liege) {
    uintptr_t their_addr = reinterpret_cast<uintptr_t>(neighbor_ptr);
    uint32_t their_id = safe_read<uint32_t>(their_addr + ck3::character::ID) & 0xFFFFFF;

    uintptr_t td = safe_read<uintptr_t>(their_addr + ck3::character::TITLE_REALM_DATA);
    int32_t mil = 0, rsz = 0;
    bool their_at_war = false;
    std::string gov;          // government-type name (tribal/feudal/clan/…) — empty if unreadable
    if ((td && is_mapped(td))) {
        mil = safe_read<int32_t>(td + ck3::title_realm::MIL_CURRENT);
        rsz = safe_read<int32_t>(td + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (rsz < 0 || rsz > 10000) rsz = 0;
        if (mil < 0 || mil > 10000000) mil = 0;
        int32_t wc = safe_read<int32_t>(td + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
        their_at_war = (wc > 0 && wc < 100);

        // Government type — title_realm+0x3F0 → CGovernmentType, name std::string
        // at +0x10 (HIGH; same read as query_character verbose @ ~8992). Lets the
        // agent pick tribal_subjugation_cb targets (independent + "*_tribal_*" gov)
        // WITHOUT trial-and-error valid_cbs_strict against every neighbor.
        uintptr_t gov_obj = safe_read<uintptr_t>(td + ck3::title_realm::GOVERNMENT_TYPE_PTR);
        if (gov_obj && is_mapped(gov_obj)) {
            gov = ad_read_std_string(gov_obj + ck3::db_object::NAME_KEY);
        }
    }

    // Independence — a tribal_subjugation_cb / duchy-CB target must be an
    // independent top-liege. get_liege_id_from_addr returns 0/0xFFFFFF when the
    // ruler has no liege. (Computed once; also reused below for `relation`.)
    uint32_t their_liege = get_liege_id_from_addr(their_addr);
    const bool independent =
        (their_liege == 0 || (their_liege & 0xFFFFFF) == 0xFFFFFF);

    r.begin_object();
    r.key("id"); r.value_uint(their_id);
    emit_char_names(r, their_addr);
    r.key("military_strength"); r.value_int(mil);
    r.key("realm_size"); r.value_int(rsz);
    r.key("is_at_war"); r.value_bool(their_at_war);
    if (!gov.empty()) { r.key("government"); r.value_string(gov.c_str(), gov.size()); }
    r.key("independent"); r.value_bool(independent);

    // Opinion
    {
        using fn_opinion = int(*)(void*, void*);
        static fn_opinion get_opinion = reinterpret_cast<fn_opinion>(
            bridge::game_fn(ck3::funcs::GET_OPINION_OF));
        int op = get_opinion(character, neighbor_ptr);
        if (op < -200 || op > 200) op = 0;
        r.key("opinion"); r.value_int(op);
    }

    // Relationship (reuses their_liege computed above for `independent`)
    const char* relation = "neighbor";
    if (their_liege == player_id) relation = "vassal";
    else if (our_liege == their_id) relation = "liege";
    r.key("relation"); r.value_string(relation);

    r.end_object();
}

// query_borders: de jure geographic neighbor detection
// Algorithm: enumerate counties in our de jure duchies and kingdoms,
// find holders outside our realm, walk liege chains to top-level rulers.
bool do_query_borders(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_borders: character not found\n");
        return false;
    }

    uint32_t player_id = char_get_id(character) & 0xFFFFFF;
    uintptr_t char_addr = reinterpret_cast<uintptr_t>(character);
    uint32_t our_liege = get_liege_id_from_addr(char_addr);
    uint32_t our_top_ruler = get_top_realm_ruler_id(char_addr);

    // Step 1: Get our held titles
    uintptr_t tr = safe_read<uintptr_t>(char_addr + ck3::character::TITLE_REALM_DATA);
    if ((!tr || !is_mapped(tr))) {
        if (out) *out = "{\"error\":\"no_realm\"}";
        return false;
    }
    uintptr_t title_arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t title_cnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (title_cnt < 0 || title_cnt > 500) title_cnt = 0;

    // Step 2: Collect de jure duchy and kingdom IDs from our counties
    std::set<uint32_t> our_duchies;
    std::set<uint32_t> our_kingdoms;
    std::set<uint32_t> our_title_ids;  // all our held title IDs for quick lookup

    if (title_arr && title_cnt > 0) {
        for (int32_t i = 0; i < title_cnt; i++) {
            uint32_t tid = safe_read<uint32_t>(title_arr + i * 4);
            if (tid == 0 || tid == 0xFFFFFFFF) continue;
            our_title_ids.insert(tid);

            uintptr_t tobj = lookup_title_obj(tid);
            if (!tobj) continue;
            int tier = get_title_tier(tobj);

            if (tier == 2) {  // county
                uint32_t duchy_id = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT);
                if (duchy_id && duchy_id != 0xFFFFFFFF) {
                    our_duchies.insert(duchy_id);
                    // Also get the kingdom
                    uintptr_t duchy_obj = lookup_title_obj(duchy_id);
                    if (duchy_obj) {
                        uint32_t kingdom_id = safe_read<uint32_t>(duchy_obj + ck3::title_obj::DE_JURE_PARENT);
                        if (kingdom_id && kingdom_id != 0xFFFFFFFF)
                            our_kingdoms.insert(kingdom_id);
                    }
                }
            } else if (tier == 3) {  // duchy
                our_duchies.insert(tid);
                uint32_t kingdom_id = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT);
                if (kingdom_id && kingdom_id != 0xFFFFFFFF)
                    our_kingdoms.insert(kingdom_id);
            }
        }
    }

    // Step 3: Enumerate counties in our de jure duchies → find external holders
    std::set<uint32_t> neighbor_ruler_ids;  // top-level realm ruler IDs
    // Also track county holders directly (for small independent counts)
    std::map<uint32_t, uintptr_t> ruler_ptrs;  // ruler_id → char_addr

    auto process_county_holder = [&](uint32_t county_id) {
        uintptr_t county_obj = lookup_title_obj(county_id);
        if (!county_obj) return;
        int tier = get_title_tier(county_obj);
        if (tier != 2) return;  // only process counties

        uint32_t holder_raw = safe_read<uint32_t>(county_obj + ck3::title_obj::HOLDER_CHAR_ID);
        if (holder_raw == 0 || holder_raw == 0xFFFFFFFF) return;
        uint32_t holder_id = holder_raw & 0xFFFFFF;
        if (holder_id == player_id) return;  // skip ourselves

        // Look up holder character
        void* holder = lookup_character(holder_raw);
        if (!holder || is_sentinel(holder)) return;

        // Walk to top-level ruler
        uint32_t top_ruler = get_top_realm_ruler_id(reinterpret_cast<uintptr_t>(holder));
        if (top_ruler == our_top_ruler) return;  // same realm as us, skip
        if (top_ruler == player_id) return;       // our vassal's vassal, skip

        // Belt-and-suspenders: also check direct liege via get_liege_id_from_addr
        // (uses resolve_title_realm with fallback probing). Catches cases where
        // get_top_realm_ruler_id fails to walk the full chain.
        uint32_t holder_liege = get_liege_id_from_addr(reinterpret_cast<uintptr_t>(holder));
        if (holder_liege == player_id) return;  // direct vassal, skip

        // Look up the top ruler
        void* ruler = lookup_character(top_ruler);
        if (!ruler || is_sentinel(ruler)) {
            // Fallback: use the direct holder
            ruler = holder;
            top_ruler = holder_id;
        }

        neighbor_ruler_ids.insert(top_ruler);
        ruler_ptrs[top_ruler] = reinterpret_cast<uintptr_t>(ruler);
    };

    // Process all counties in our de jure duchies
    for (uint32_t duchy_id : our_duchies) {
        uintptr_t duchy_obj = lookup_title_obj(duchy_id);
        if (!duchy_obj) continue;
        uintptr_t sub_arr = safe_read<uintptr_t>(duchy_obj + ck3::title_obj::SUB_TITLE_ARRAY);
        int32_t sub_cnt = safe_read<int32_t>(duchy_obj + ck3::title_obj::SUB_TITLE_COUNT);
        if (sub_cnt < 0 || sub_cnt > 200) sub_cnt = 0;
        if (!sub_arr || sub_cnt == 0) continue;

        for (int32_t i = 0; i < sub_cnt; i++) {
            uint32_t sub_id = safe_read<uint32_t>(sub_arr + i * 4);
            if (sub_id == 0 || sub_id == 0xFFFFFFFF) continue;
            process_county_holder(sub_id);
        }
    }

    // Step 4: Also check neighboring duchies (duchies in our de jure kingdoms)
    for (uint32_t kingdom_id : our_kingdoms) {
        uintptr_t kingdom_obj = lookup_title_obj(kingdom_id);
        if (!kingdom_obj) continue;
        uintptr_t sub_arr = safe_read<uintptr_t>(kingdom_obj + ck3::title_obj::SUB_TITLE_ARRAY);
        int32_t sub_cnt = safe_read<int32_t>(kingdom_obj + ck3::title_obj::SUB_TITLE_COUNT);
        if (sub_cnt < 0 || sub_cnt > 200) sub_cnt = 0;
        if (!sub_arr || sub_cnt == 0) continue;

        for (int32_t i = 0; i < sub_cnt; i++) {
            uint32_t duchy_id = safe_read<uint32_t>(sub_arr + i * 4);
            if (duchy_id == 0 || duchy_id == 0xFFFFFFFF) continue;
            if (our_duchies.count(duchy_id)) continue;  // already processed

            uintptr_t duchy_obj = lookup_title_obj(duchy_id);
            if (!duchy_obj) continue;
            uintptr_t county_arr = safe_read<uintptr_t>(duchy_obj + ck3::title_obj::SUB_TITLE_ARRAY);
            int32_t county_cnt = safe_read<int32_t>(duchy_obj + ck3::title_obj::SUB_TITLE_COUNT);
            if (county_cnt < 0 || county_cnt > 200) county_cnt = 0;
            if (!county_arr || county_cnt == 0) continue;

            for (int32_t j = 0; j < county_cnt; j++) {
                uint32_t county_id = safe_read<uint32_t>(county_arr + j * 4);
                if (county_id == 0 || county_id == 0xFFFFFFFF) continue;
                process_county_holder(county_id);
            }
        }
    }

    // Step 5: Build response
    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(player_id);

    r.key("neighbors"); r.begin_array();
    int count = 0;
    for (auto& [ruler_id, ruler_addr] : ruler_ptrs) {
        if (count >= 50) break;
        void* ruler = reinterpret_cast<void*>(ruler_addr);
        emit_neighbor_info(r, character, ruler, player_id, our_liege);
        count++;
    }
    r.end_array();

    r.key("count"); r.value_int(count);
    r.key("our_duchies"); r.value_int(static_cast<int>(our_duchies.size()));
    r.key("our_kingdoms"); r.value_int(static_cast<int>(our_kingdoms.size()));
    r.key("note"); r.value_string("De jure neighbors: rulers holding counties in our de jure duchies/kingdoms. Each carries government (tribal/feudal/clan/…) + independent — pick tribal_subjugation_cb targets as independent==true && government contains 'tribal', ranked by realm_size (folds the whole sub-realm).");

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_claims: available claims for a character
// R25 FIX: Claims are at core_data+0x1E8, NOT char+0x170!
// Access: core_data = *(char+0x1A8); claims PDX array = core_data+0x1E8
// PDX array struct: +0x00=data_ptr, +0x0C=count
// Each CClaim = 0x10 bytes: +0x00=vtable(8B), +0x08=title_id(u32), +0x0C=pressed(byte), +0x0D=implicit(byte)
// Claim state logic (FUN_03e725b0 / BuildClaimsList FUN_03f25c10):
//   implicit=1 (entry+0xD) → implicit claim
//   implicit=0 + pressed=1 (entry+0xC) → pressed/strong claim
//   both=0 → unpressed/weak claim
bool do_query_claims(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_claims: character not found\n");
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;
    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_id);

    // R25 FIX: Read claims from core_data+0x1E8, not char+0x170
    uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);  // char+0x1A8
    uintptr_t claim_array_ptr = 0;
    int32_t claim_count = 0;
    if (core_data) {
        uintptr_t claims_pdx = core_data + ck3::core_data::CLAIMS_ARRAY;  // core_data+0x1E8
        claim_array_ptr = safe_read<uintptr_t>(claims_pdx + 0x00);  // data pointer
        claim_count = safe_read<int32_t>(claims_pdx + 0x0C);         // count
    }

    // Sanity check: cap at 100 claims to avoid reading garbage
    if (claim_count < 0 || claim_count > 100) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_claims: claim_count=%d out of range, clamping to 0\n", claim_count);
        claim_count = 0;
    }

    r.key("claim_count"); r.value_int(claim_count);
    r.key("claims"); r.begin_array();
    if (claim_array_ptr && claim_count > 0) {
        for (int32_t i = 0; i < claim_count; i++) {
            uintptr_t entry = claim_array_ptr + i * ck3::claim_entry::ENTRY_SIZE;  // 0x10 stride

            // R25 FIX: +0x00 is vtable, title_id is at +0x08 (uint32, not packed ref)
            uint32_t title_id = safe_read<uint32_t>(entry + ck3::claim_entry::TITLE_ID);
            // Pressed flag at +0x0C (1 = pressed/strong claim)
            uint8_t pressed = safe_read<uint8_t>(entry + ck3::claim_entry::PRESSED);
            // Implicit flag at +0x0D (1 = inherited from parent/grandparent)
            uint8_t implicit_flag = safe_read<uint8_t>(entry + ck3::claim_entry::IMPLICIT);

            r.begin_object();
            r.key("title_id"); r.value_uint(title_id);

            // Holder (war defender) capture for declarability honesty below.
            uintptr_t holder_addr = 0;
            bool holder_present = false;
            uint32_t holder_char_id = 0;
            (void)holder_char_id;

            // Resolve title name via landed title database
            uintptr_t tobj = lookup_title_obj(title_id);
            if (tobj) {
                auto display_name = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!display_name.empty()) {
                    r.key("title_name"); r.value_string(display_name.c_str(), display_name.size());
                }
                int tier = get_title_tier(tobj);
                if (tier > 0) {
                    r.key("tier"); r.value_int(tier);
                    // Human-readable tier name
                    const char* tier_names[] = {"", "barony", "county", "duchy", "kingdom", "empire"};
                    if (tier >= 1 && tier <= 5) {
                        r.key("tier_name"); r.value_string(tier_names[tier]);
                    }
                }
                // Current holder of the claimed title
                uint32_t holder_raw = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID);
                if (holder_raw != 0 && holder_raw != 0xFFFFFFFF) {
                    uint32_t holder_id = holder_raw & 0xFFFFFF;
                    r.key("holder_id"); r.value_uint(holder_id);
                    void* holder = lookup_character(holder_raw);
                    if (holder && !is_sentinel(holder)) {
                        holder_addr = reinterpret_cast<uintptr_t>(holder);
                        holder_present = true;
                        holder_char_id = holder_id;
                        std::string hname = ad_read_char_name(holder_addr);
                        if (!hname.empty()) {
                            r.key("holder_name"); r.value_string(hname.c_str(), hname.size());
                        }
                    }
                }
            }

            r.key("pressed"); r.value_bool(pressed != 0);
            r.key("implicit"); r.value_bool(implicit_flag != 0);

            // Derive claim type string for convenience
            // R25: implicit(0xD)!=0 → implicit, pressed(0xC)!=0 → pressed/strong, both 0 → unpressed/weak
            if (implicit_flag) {
                r.key("type"); r.value_string("implicit");
            } else {
                r.key("type"); r.value_string(pressed ? "pressed" : "unpressed");
            }

            // --- War-declarability honesty (read-side) ---------------------
            // A pressed claim is only a backable claim_cb opener if the title
            // HOLDER (the war defender) is a legal target: independent
            // (intent_dispatch.cpp:1478) and not imprisoned (war_validity R4).
            // Otherwise the engine culls the steered war to war_count 0
            // (lessons-learned L48-50, L66-69 — Tortona/Louis-of-Italy).
            if (holder_present) {
                DefenderState ds = probe_defender_state(holder_addr);
                r.key("holder_independent"); r.value_bool(ds.independent);
                if (!ds.independent) {
                    r.key("holder_liege_id"); r.value_uint(ds.liege_id);
                }
                r.key("holder_imprisoned"); r.value_bool(ds.imprisoned);
                // Alliance/truce + already-at-war-opposing target-state gates
                // (mirror intent_dispatch.cpp:1565-1571 + engine Gate 0). Plus a
                // self-target guard: a pressed claim on a title the querying
                // character ITSELF holds (atk==def) cannot open a claim_cb war.
                bool in_alliance = false, in_truce = false;
                probe_alliance_truce(character, reinterpret_cast<void*>(holder_addr),
                                     in_alliance, in_truce);
                bool opp = actor_at_war_opposing(c, holder_char_id);
                bool is_self = (holder_char_id == char_id);
                r.key("holder_in_alliance"); r.value_bool(in_alliance);
                r.key("holder_in_truce"); r.value_bool(in_truce);
                r.key("already_at_war_opposing"); r.value_bool(opp);
                bool declarable = (pressed != 0) && !is_self && ds.independent
                                  && !ds.imprisoned && !in_alliance && !in_truce && !opp;
                r.key("declarable"); r.value_bool(declarable);
                if (!declarable) {
                    const char* reason = (pressed == 0) ? "claim_not_pressed"
                                       : (is_self) ? "target_is_self"
                                       : (!ds.independent) ? "target_not_independent"
                                       : (ds.imprisoned) ? "target_imprisoned"
                                       : (in_alliance) ? "target_in_alliance"
                                       : (in_truce) ? "target_in_truce"
                                       : "already_at_war_opposing";
                    r.key("block_reason"); r.value_string(reason);
                }
            }

            r.end_object();
        }
    }
    r.end_array();

    // De jure claims: counties in our held duchies that are held by others
    uint32_t player_id = char_get_id(character) & 0xFFFFFF;
    uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    r.key("de_jure_claims"); r.begin_array();
    int dejure_count = 0;

    if ((tr && is_mapped(tr))) {
        uintptr_t title_arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
        int32_t title_cnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (title_cnt < 0 || title_cnt > 500) title_cnt = 0;

        // Collect duchies we hold
        std::set<uint32_t> our_duchies;
        std::set<uint32_t> our_held;
        if (title_arr && title_cnt > 0) {
            for (int32_t i = 0; i < title_cnt; i++) {
                uint32_t tid = safe_read<uint32_t>(title_arr + i * 4);
                if (tid == 0 || tid == 0xFFFFFFFF) continue;
                our_held.insert(tid);
                uintptr_t tobj = lookup_title_obj(tid);
                if (!tobj) continue;
                int tier = get_title_tier(tobj);
                if (tier == 3) our_duchies.insert(tid);  // duchy
                if (tier == 2) {  // county → get parent duchy
                    uint32_t djp = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT);
                    // Only add if we hold this duchy
                    if (djp && djp != 0xFFFFFFFF && our_held.count(djp))
                        our_duchies.insert(djp);
                }
            }
        }

        // For each duchy, find counties held by others
        for (uint32_t duchy_id : our_duchies) {
            uintptr_t duchy_obj = lookup_title_obj(duchy_id);
            if (!duchy_obj) continue;
            uintptr_t sub_arr = safe_read<uintptr_t>(duchy_obj + ck3::title_obj::SUB_TITLE_ARRAY);
            int32_t sub_cnt = safe_read<int32_t>(duchy_obj + ck3::title_obj::SUB_TITLE_COUNT);
            if (sub_cnt < 0 || sub_cnt > 200) sub_cnt = 0;
            if (!sub_arr || sub_cnt == 0) continue;

            for (int32_t j = 0; j < sub_cnt && dejure_count < 50; j++) {
                uint32_t county_id = safe_read<uint32_t>(sub_arr + j * 4);
                if (county_id == 0 || county_id == 0xFFFFFFFF) continue;
                uintptr_t county_obj = lookup_title_obj(county_id);
                if (!county_obj) continue;
                if (get_title_tier(county_obj) != 2) continue;

                uint32_t holder_raw = safe_read<uint32_t>(county_obj + ck3::title_obj::HOLDER_CHAR_ID);
                if (holder_raw == 0 || holder_raw == 0xFFFFFFFF) continue;
                uint32_t holder_id = holder_raw & 0xFFFFFF;
                if (holder_id == player_id) continue;  // we hold it

                r.begin_object();
                r.key("title_id"); r.value_uint(county_id);

                // County name (a game std::string: read through the build's string ABI)
                {
                    std::string cname = ad_read_std_string(county_obj + ck3::title_obj::TITLE_DISPLAY_NAME);
                    if (!cname.empty()) { r.key("name"); r.value_string(cname.c_str(), cname.size()); }
                }

                r.key("holder_id"); r.value_uint(holder_id);

                // Holder name
                void* holder = lookup_character(holder_raw);
                if (holder && !is_sentinel(holder)) {
                    uintptr_t h_addr = reinterpret_cast<uintptr_t>(holder);
                    std::string hname = ad_read_std_string(h_addr + ck3::character::FIRST_NAME);
                    if (!hname.empty()) { r.key("holder_name"); r.value_string(hname.c_str(), hname.size()); }
                }

                r.key("duchy_id"); r.value_uint(duchy_id);
                r.key("type"); r.value_string("de_jure");

                // De-jure honesty: a de jure relationship is NOT a claim_entry
                // and presses NO claim_cb. Emit declarable=false unless the
                // querying char ALSO holds a pressed claim_entry on this title;
                // otherwise the write path yields claim_not_pressed and the
                // engine culls the war (war_count 0). Mirror the pressed-claims
                // path's target-state gates when a real pressed claim exists.
                bool dj_pressed = attacker_has_pressed_claim_on(c, holder_id);
                r.key("pressed"); r.value_bool(dj_pressed);
                if (!dj_pressed) {
                    r.key("declarable"); r.value_bool(false);
                    r.key("block_reason"); r.value_string("no_pressed_claim_de_jure_only");
                } else {
                    void* dj_holder = lookup_character(holder_raw);
                    DefenderState dds = (dj_holder && !is_sentinel(dj_holder))
                        ? probe_defender_state(reinterpret_cast<uintptr_t>(dj_holder))
                        : DefenderState{};
                    bool dj_in_alliance = false, dj_in_truce = false;
                    if (dj_holder && !is_sentinel(dj_holder))
                        probe_alliance_truce(character, dj_holder, dj_in_alliance, dj_in_truce);
                    bool dj_opp = actor_at_war_opposing(c, holder_id);
                    r.key("holder_independent"); r.value_bool(dds.independent);
                    r.key("holder_imprisoned"); r.value_bool(dds.imprisoned);
                    bool dj_decl = dds.independent && !dds.imprisoned
                                   && !dj_in_alliance && !dj_in_truce && !dj_opp;
                    r.key("declarable"); r.value_bool(dj_decl);
                    if (!dj_decl) {
                        const char* reason = (!dds.independent) ? "target_not_independent"
                                           : (dds.imprisoned) ? "target_imprisoned"
                                           : (dj_in_alliance) ? "target_in_alliance"
                                           : (dj_in_truce) ? "target_in_truce"
                                           : "already_at_war_opposing";
                        r.key("block_reason"); r.value_string(reason);
                    }
                }

                r.end_object();
                dejure_count++;
            }
        }
    }
    r.end_array();
    r.key("de_jure_claim_count"); r.value_int(dejure_count);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_de_jure: de jure hierarchy for a title — IMPLEMENTED + crash-hardened.
//   Title DB: ck3::globals::LANDED_TITLE_DB (== TITLE_DB == 0x0bed2700). Lookup
//   via lookup_title_obj() (dispatch_router.cpp): DB ptr through safe_read_offset,
//   is_mapped() on table+entry, ID-verify on the resolved object (ck-86ue.1).
//   Walk: DE_JURE_PARENT (title_obj+0x250, CORRECTED #116 — was the de-facto
//   field 0x230) upward (depth<5), SUB_TITLE_ARRAY/SUB_TITLE_COUNT for children
//   (sub_cnt clamped 0..500). Every read in do_query_de_jure goes through
//   safe_read (process_vm_readv, EFAULT-safe → T{} on unmapped) or the guarded
//   lookup_title_obj / emit_title_info / ad_read_std_string (safe_read_game_string)
//   primitives — no raw dereference, so the path cannot SIGSEGV. Classified
//   Tier-2 game-state (snapshot.cpp) → runs inline under the sigsetjmp guard when
//   paused, rejects when unpaused. (The old DAT_0bfd2700 / province-layout TODO
//   here was stale — wrong global + conflated a province struct with the title
//   object; none of those offsets are used by this handler.)
//   OPEN: title_key string→object lookup (Priority 2 below) is still unimplemented.
// =============================================================================
// query_title_hierarchy: show title relationships (de jure parent, sub-titles, holder)
// Accepts title_id to inspect a specific title, or character_id to show hierarchy
// for all held titles.
// =============================================================================
static void emit_title_info(ResponseBuilder& r, uintptr_t tobj) {
    if (!tobj) return;

    uint32_t tid = safe_read<uint32_t>(tobj + ck3::title_obj::TITLE_ID);
    r.key("title_id"); r.value_uint(tid & 0xFFFFFF);

    // Display name (custom/localized at title+0x58)
    auto display_name = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
    if (!display_name.empty()) {
        r.key("name"); r.value_string(display_name.c_str(), display_name.size());
    }

    // Title key from type definition (may be null for barony stubs)
    uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
    std::string title_key;
    int tier = 0;
    if (type_ptr && is_mapped(type_ptr)) {
        title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
    }
    if (!title_key.empty()) {
        r.key("key"); r.value_string(title_key.c_str(), title_key.size());
    }

    // Tier fallbacks for barony stubs (type_ptr=0): PROVINCE_PTR is set
    // only on barony-tier titles (offsets/3253 HIGH); key prefix is the
    // canonical CK3 naming convention (b_/c_/d_/k_/e_).
    if (tier < 1 || tier > 5) {
        uintptr_t province_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::PROVINCE_PTR);
        if (province_ptr && is_mapped(province_ptr)) {
            tier = 1;
        } else if (!title_key.empty()) {
            switch (title_key[0]) {
                case 'b': tier = 1; break;
                case 'c': tier = 2; break;
                case 'd': tier = 3; break;
                case 'k': tier = 4; break;
                case 'e': tier = 5; break;
                default: break;
            }
        } else if (!type_ptr) {
            // Type-less stubs in held_title lists are baronies in practice.
            tier = 1;
        }
    }
    if (tier >= 1 && tier <= 5) {
        r.key("tier"); r.value_int(tier);
        const char* tier_names[] = {"", "barony", "county", "duchy", "kingdom", "empire"};
        r.key("tier_name"); r.value_string(tier_names[tier]);
    }

    // Holder character ID
    uint32_t holder_id = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID);
    if (holder_id != 0xFFFFFFFF && holder_id != 0) {
        r.key("holder_id"); r.value_uint(holder_id & 0xFFFFFF);
        // Try to get holder name
        void* holder = lookup_character(holder_id);
        if (holder && !is_sentinel(holder)) {
            std::string hname = ad_read_char_name(reinterpret_cast<uintptr_t>(holder));
            if (!hname.empty()) {
                r.key("holder_name"); r.value_string(hname.c_str(), hname.size());
            }
        }
    }

    // De jure parent
    uint32_t parent_id = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT);
    if (parent_id != 0 && parent_id != 0xFFFFFFFF) {
        r.key("de_jure_parent_id"); r.value_uint(parent_id & 0xFFFFFF);
        uintptr_t parent_obj = lookup_title_obj(parent_id);
        if (parent_obj) {
            uintptr_t ptype = safe_read<uintptr_t>(parent_obj + ck3::title_obj::TITLE_TYPE_PTR);
            if (ptype) {
                auto pkey = ad_read_std_string(ptype + ck3::db_object::NAME_KEY);
                if (!pkey.empty()) {
                    r.key("de_jure_parent_key"); r.value_string(pkey.c_str(), pkey.size());
                }
            }
        }
    }
}

bool do_query_title_hierarchy(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    int char_id_param = json_get_int(params_json, "character_id");

    ResponseBuilder r;
    r.begin_object();

    // If a specific title_id is given, show that title's hierarchy
    if (title_id_param > 0) {
        uintptr_t tobj = lookup_title_obj(static_cast<uint32_t>(title_id_param));
        if (!tobj) {
            r.key("error"); r.value_string("title_not_found");
            r.key("title_id"); r.value_uint(static_cast<uint32_t>(title_id_param));
            r.end_object();
            if (out) *out = r.take();
            return false;
        }

        r.key("title"); r.begin_object();
        emit_title_info(r, tobj);

        // Sub-titles (de jure children)
        uintptr_t sub_arr = safe_read<uintptr_t>(tobj + ck3::title_obj::SUB_TITLE_ARRAY);
        int32_t sub_cnt = safe_read<int32_t>(tobj + ck3::title_obj::SUB_TITLE_COUNT);
        if (sub_cnt < 0 || sub_cnt > 500) sub_cnt = 0;

        r.key("sub_titles"); r.begin_array();
        if (sub_arr && sub_cnt > 0) {
            for (int32_t i = 0; i < sub_cnt; i++) {
                uint32_t sub_id = safe_read<uint32_t>(sub_arr + i * 4);
                if (sub_id == 0 || sub_id == 0xFFFFFFFF) continue;
                uintptr_t sub_obj = lookup_title_obj(sub_id);
                if (!sub_obj) continue;
                r.begin_object();
                emit_title_info(r, sub_obj);
                r.end_object();
            }
        }
        r.end_array();
        r.key("sub_title_count"); r.value_int(sub_cnt);

        r.end_object();  // title

    } else {
        // Show hierarchy for a character's held titles
        void* character = nullptr;
        if (char_id_param > 0) {
            character = lookup_character(static_cast<uint32_t>(char_id_param));
        } else {
            character = get_controlled_character();
        }

        if (!character || is_sentinel(character)) {
            r.key("error"); r.value_string("character_not_found");
            r.end_object();
            if (out) *out = r.take();
            return false;
        }

        uintptr_t c_ptr = reinterpret_cast<uintptr_t>(character);
        r.key("character_id"); r.value_uint(char_get_id(character) & 0xFFFFFF);

        uintptr_t title_data = safe_read<uintptr_t>(c_ptr + ck3::character::TITLE_REALM_DATA);
        if (!title_data) {
            r.key("error"); r.value_string("no_title_realm_data");
            r.end_object();
            if (out) *out = r.take();
            return false;
        }

        uintptr_t tdata = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
        int32_t tcnt = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (tcnt < 0 || tcnt > 500) tcnt = 0;

        r.key("held_titles"); r.begin_array();
        int emitted = 0;
        if (tdata && tcnt > 0) {
            for (int32_t i = 0; i < tcnt; i++) {
                uint32_t tid = safe_read<uint32_t>(tdata + i * 4);
                uintptr_t tobj = lookup_title_obj(tid);
                if (!tobj) continue;

                r.begin_object();
                emit_title_info(r, tobj);

                // Sub-titles for each held title
                uintptr_t sub_arr = safe_read<uintptr_t>(tobj + ck3::title_obj::SUB_TITLE_ARRAY);
                int32_t sub_cnt = safe_read<int32_t>(tobj + ck3::title_obj::SUB_TITLE_COUNT);
                if (sub_cnt < 0 || sub_cnt > 500) sub_cnt = 0;

                r.key("sub_titles"); r.begin_array();
                if (sub_arr && sub_cnt > 0) {
                    for (int32_t j = 0; j < sub_cnt; j++) {
                        uint32_t sub_id = safe_read<uint32_t>(sub_arr + j * 4);
                        if (sub_id == 0 || sub_id == 0xFFFFFFFF) continue;
                        uintptr_t sub_obj = lookup_title_obj(sub_id);
                        if (!sub_obj) continue;
                        r.begin_object();
                        emit_title_info(r, sub_obj);
                        r.end_object();
                    }
                }
                r.end_array();
                r.key("sub_title_count"); r.value_int(sub_cnt);

                r.end_object();
                emitted++;
            }
        }
        r.end_array();
        r.key("held_title_count"); r.value_int(emitted);
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

bool do_query_de_jure(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    const char* title_key = json_get_string(params_json, "title_key");

    uintptr_t tobj = 0;

    // Priority 1: direct title_id lookup
    if (title_id_param > 0) {
        tobj = lookup_title_obj(static_cast<uint32_t>(title_id_param));
        if (!tobj) {
            if (out) *out = "{\"error\":\"title_not_found\",\"title_id\":" + std::to_string(title_id_param) + "}";
            return false;
        }
    }
    // Priority 2: title_key (string lookup) — REUSE the canonical resolver
    // (action_handlers.cpp:resolve_title_id_by_key). It scans LANDED_TITLE_DB and
    // matches each title's type-object NAME_KEY string — the key→index map. Same
    // path used live by do_create_title and do_query_dejure_progress. Pure bounded
    // safe_read; no write, no engine call.
    else if (title_key) {
        uint32_t resolved = resolve_title_id_by_key(title_key);
        if (resolved == 0xFFFFFFFFu) {
            if (out) {
                ResponseBuilder r;
                r.begin_object();
                r.key("error"); r.value_string("title_key_not_found");
                r.key("title_key"); r.value_string(title_key);
                r.key("hint"); r.value_string("Pass a landed_title script key (e.g. \"d_northumbria\", \"e_britannia\") or use title_id");
                r.end_object();
                *out = r.take();
            }
            return false;
        }
        tobj = lookup_title_obj(resolved);
        if (!tobj) {
            if (out) *out = "{\"error\":\"title_not_found\",\"title_key\":\"" + std::string(title_key) + "\",\"resolved_title_id\":" + std::to_string(resolved) + "}";
            return false;
        }
    }
    // Priority 3: fallback to controlled character's primary title
    else {
        void* character = resolve_character(params_json);
        if (!character || is_sentinel(character)) {
            if (out) *out = "{\"error\":\"no_character\",\"hint\":\"Provide title_id, title_key, or character_id\"}";
            return false;
        }
        uintptr_t c_ptr = reinterpret_cast<uintptr_t>(character);
        uintptr_t title_data = safe_read<uintptr_t>(c_ptr + ck3::character::TITLE_REALM_DATA);
        if (!title_data) {
            if (out) *out = "{\"error\":\"no_title_realm_data\",\"hint\":\"Character has no landed titles\"}";
            return false;
        }
        uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
        int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (!title_arr_ptr || title_arr_count <= 0) {
            if (out) *out = "{\"error\":\"no_held_titles\",\"hint\":\"Character holds no titles\"}";
            return false;
        }
        uint32_t primary_tid = safe_read<uint32_t>(title_arr_ptr);
        tobj = lookup_title_obj(primary_tid);
        if (!tobj) {
            if (out) *out = "{\"error\":\"primary_title_not_found\",\"title_id\":" + std::to_string(primary_tid & 0xFFFFFF) + "}";
            return false;
        }
    }

    // Build response with de jure hierarchy for the resolved title
    ResponseBuilder r;
    r.begin_object();

    // Emit the title info
    r.key("title"); r.begin_object();
    emit_title_info(r, tobj);
    r.end_object();

    // Walk de jure parent chain upward
    r.key("de_jure_parents"); r.begin_array();
    {
        uintptr_t cur = tobj;
        int depth = 0;
        while (depth < 5) {  // max 5 tiers in CK3
            uint32_t parent_id = safe_read<uint32_t>(cur + ck3::title_obj::DE_JURE_PARENT);
            if (parent_id == 0 || parent_id == 0xFFFFFFFF) break;
            uintptr_t parent_obj = lookup_title_obj(parent_id);
            if (!parent_obj) break;
            r.begin_object();
            emit_title_info(r, parent_obj);
            r.end_object();
            cur = parent_obj;
            depth++;
        }
    }
    r.end_array();

    // De jure children (sub-titles)
    uintptr_t sub_arr = safe_read<uintptr_t>(tobj + ck3::title_obj::SUB_TITLE_ARRAY);
    int32_t sub_cnt = safe_read<int32_t>(tobj + ck3::title_obj::SUB_TITLE_COUNT);
    if (sub_cnt < 0 || sub_cnt > 500) sub_cnt = 0;

    // #109 (Surface 3): de-jure formation shortfall. Title formation (forming
    // k_england etc.) is de-jure-gated — the engine requires controlling a
    // threshold of the title's de-jure sub-titles. We expose the RAW shortfall
    // (held vs total de-jure children for a candidate ruler) so the agent can see
    // "control 3/5 de-jure duchies" instead of an opaque refusal. Pure read-side
    // arithmetic over data already gathered (each child's holder_id) → fully safe.
    //
    // NOTE / open RE: the exact formation THRESHOLD (all vs majority of de-jure
    // counties, and whether it counts vassals' holdings, not just direct holdings)
    // is decision/title-specific and NOT yet RE'd — the found_*_kingdom decisions
    // each carry their own scripted is_shown/is_valid de-jure trigger. This block
    // reports DIRECT-hold counts at duchy granularity as a planning signal, not the
    // authoritative gate. For the authoritative verdict, evaluate the formation
    // DECISION via list_decisions_annotated (failures[] carries the de-jure key).
    // See docs/notes/britannia-blockers.md.
    int32_t claimant = json_get_int(params_json, "claimant_id");
    if (claimant <= 0) claimant = json_get_int(params_json, "character_id");
    uint32_t want_holder = (claimant > 0) ? (static_cast<uint32_t>(claimant) & 0xFFFFFF) : 0;

    int held = 0, total = 0;
    r.key("de_jure_children"); r.begin_array();
    if (sub_arr && sub_cnt > 0) {
        for (int32_t i = 0; i < sub_cnt; i++) {
            uint32_t sub_id = safe_read<uint32_t>(sub_arr + i * 4);
            if (sub_id == 0 || sub_id == 0xFFFFFFFF) continue;
            uintptr_t sub_obj = lookup_title_obj(sub_id);
            if (!sub_obj) continue;
            total++;
            r.begin_object();
            emit_title_info(r, sub_obj);
            // Per-child controlled flag for the queried/claimant character.
            if (want_holder) {
                uint32_t h = safe_read<uint32_t>(sub_obj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
                bool controlled = (h == want_holder);
                if (controlled) held++;
                r.key("controlled_by_claimant"); r.value_bool(controlled);
            }
            r.end_object();
        }
    }
    r.end_array();
    r.key("de_jure_children_count"); r.value_int(sub_cnt);

    // Formation shortfall summary (only when a candidate ruler was given).
    if (want_holder) {
        r.key("de_jure_formation"); r.begin_object();
        r.key("claimant_id"); r.value_uint(want_holder);
        r.key("de_jure_children_held"); r.value_int(held);
        r.key("de_jure_children_total"); r.value_int(total);
        r.key("de_jure_children_short"); r.value_int(total - held);
        r.key("hold_granularity"); r.value_string("direct_de_jure_children");  // tier below this title
        r.key("threshold_confidence"); r.value_string("low");  // #109: direct-hold count, not the authoritative formation gate
        r.key("note"); r.value_string("DIRECT de-jure children this character holds, at the tier below this title (e.g. duchies of a kingdom). NOT the authoritative formation gate (threshold + vassal-hold counting are decision-specific and unverified). For the real verdict, eval the formation decision via list_decisions_annotated (failures[] carries the de-jure loc-key).");
        r.end_object();
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_county_details: county buildings, development, terrain
bool do_query_county_details(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = "{\"error\":\"character not found\"}";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    // #127 (Gen-26 F4): ruler faith/culture, for per-county wrong-faith/wrong-culture
    // targeting. CULTURE_ID_DIRECT=char+0xB0, FAITH_ID_DIRECT=char+0xB4 (both HIGH).
    // Names resolved via the same paths as observation.cpp resolve_culture/faith_name:
    //   culture: ad_db_lookup(CULTURE_INDEXED_DB, id) -> name @ db_object::NAME_KEY (0x10)
    //   faith:   ad_db_lookup(FAITH_DB, id) -> key @ faith::FAITH_KEY (0x98)
    //   (FAITH_INDEXED_DB is cap=0 + +0x10 is garbage for CFaith — #faith fix 2026-06-19)
    uint32_t ruler_culture_id = safe_read<uint32_t>(c + ck3::character::CULTURE_ID_DIRECT);
    uint32_t ruler_faith_id   = safe_read<uint32_t>(c + ck3::character::FAITH_ID_DIRECT);

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(my_id);
    if (ruler_culture_id != 0 && ruler_culture_id != 0xFFFFFFFF) {
        // FIX #culture_name: was reading db_object::NAME_KEY (+0x10 = the uint32 culture ID)
        // as a std::string. Route through emit_culture_fields (NAME +0x260 / SCRIPT_KEY +0x1C8).
        uintptr_t rco = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, ruler_culture_id);
        emit_culture_fields(r, rco, ruler_culture_id, "ruler_culture_id", "ruler_culture", "ruler_culture_key");
    }
    if (ruler_faith_id != 0 && ruler_faith_id != 0xFFFFFFFF) {
        r.key("ruler_faith_id"); r.value_uint(ruler_faith_id & 0xFFFFFF);
        uintptr_t rfo = ad_db_lookup(ck3::globals::FAITH_DB, ruler_faith_id);
        if (rfo) {
            std::string rfn = ad_read_std_string(rfo + faith::FAITH_KEY);
            if (!rfn.empty()) { r.key("ruler_faith"); r.value_string(rfn.c_str(), rfn.size()); }
        }
    }

    // ck-86ue.3: every derived-pointer hop now carries is_mapped() in addition
    // to the safe_read<T>() reads. safe_read returns 0 on unmapped addresses,
    // so the guards never gate previously-valid pointers — they stop subsequent
    // reads from a non-null-but-unmapped derived pointer (sentinel 0x1, freed-
    // and-reused, half-constructed object) from leaking garbage into the JSON.

    // Core data (char+0x1a8): gold, piety, prestige, monthly income, levels, caps
    uintptr_t core = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
    if (core && is_mapped(core)) {
        r.key("gold"); r.value_float(static_cast<double>(safe_read<int64_t>(core + ck3::core_data::GOLD)) / 100000.0);
        r.key("monthly_income"); r.value_float(static_cast<double>(safe_read<int64_t>(core + ck3::core_data::MONTHLY_INCOME_FP)) / 100000.0);
        r.key("piety"); r.value_float(static_cast<double>(safe_read<int64_t>(core + ck3::core_data::PIETY)) / 100000.0);
        r.key("piety_cap"); r.value_float(static_cast<double>(safe_read<int64_t>(core + ck3::core_data::PIETY_CAP)) / 100000.0);
        r.key("prestige"); r.value_float(static_cast<double>(safe_read<int64_t>(core + ck3::core_data::PRESTIGE)) / 100000.0);
        r.key("prestige_cap"); r.value_float(static_cast<double>(safe_read<int64_t>(core + ck3::core_data::PRESTIGE_CAP)) / 100000.0);
    }

    // Legitimacy from diplomatic sub-struct (char+0x1c0)
    uintptr_t diplo = safe_read<uintptr_t>(c + ck3::character::DIPLOMATIC_DATA);
    if (diplo && is_mapped(diplo)) {
        r.key("legitimacy"); r.value_float(static_cast<double>(safe_read<int64_t>(diplo + ck3::diplomatic_data::LEGITIMACY)) / 100000.0);
    }

    // Barter goods from title_realm sub-struct (char+0x1b8). tr is gated once
    // and reused for the county-iteration block below ("tr_ok" replaces a
    // second (tr && is_mapped(tr)) check there).
    uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    bool tr_ok = tr && is_mapped(tr);
    if (tr_ok) {
        r.key("barter_goods"); r.value_float(static_cast<double>(safe_read<int64_t>(tr + ck3::title_realm::BARTER_GOODS)) / 100000.0);
    }

    // County details: iterate held title array, filter tier==2 (counties)
    r.key("counties"); r.begin_array();
    int county_count = 0;
    if (tr_ok) {
        uintptr_t tdata = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
        int32_t tcnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (tcnt < 0 || tcnt > 500) tcnt = 0;

        uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::TITLE_DB);
        uintptr_t table = (db && is_mapped(db)) ? safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR) : 0;
        uint32_t capacity = (db && is_mapped(db)) ? safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT) : 0;

        if (tdata && is_mapped(tdata) && table && is_mapped(table) && capacity > 0) {
            for (int32_t i = 0; i < tcnt && county_count < 64; i++) {
                uint32_t tid = safe_read<uint32_t>(tdata + i * 4);
                uint32_t masked = tid & 0xFFFFFF;
                if (masked >= capacity) continue;

                uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                   + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
                if (!entry || !is_mapped(entry)) continue;
                // Verify ID (masked 24-bit comparison)
                if ((safe_read<uint32_t>(entry + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != masked) continue;

                // Check tier == 2 (county)
                uintptr_t type_ptr = safe_read<uintptr_t>(entry + ck3::title_obj::TITLE_TYPE_PTR);
                if (!type_ptr || !is_mapped(type_ptr)) continue;
                int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
                if (tier != 2) continue;

                r.begin_object();
                r.key("title_id"); r.value_uint(masked);

                // Title key from type definition
                auto title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                if (!title_key.empty()) {
                    r.key("key"); r.value_string(title_key.c_str(), title_key.size());
                }

                // Display name
                auto display_name = ad_read_std_string(entry + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!display_name.empty()) {
                    r.key("name"); r.value_string(display_name.c_str(), display_name.size());
                }

                // De jure parent
                uint32_t dejure = safe_read<uint32_t>(entry + ck3::title_obj::DE_JURE_PARENT);
                if (dejure != 0 && dejure != 0xFFFFFFFF) {
                    r.key("de_jure_parent_id"); r.value_uint(dejure & 0xFFFFFF);
                }

                // R19: Navigate county_title → first barony → province → county_data
                // title+0x428 is a sentinel on CLandedTitle! County data is on CProvince objects.
                uintptr_t county_data = 0;
                {
                    // Get first sub-title (barony) from county's sub-title array
                    int32_t sub_count = safe_read<int32_t>(entry + ck3::title_obj::SUB_TITLE_COUNT);
                    if (sub_count > 0) {
                        uintptr_t sub_arr = safe_read<uintptr_t>(entry + ck3::title_obj::SUB_TITLE_ARRAY);
                        if (sub_arr && is_mapped(sub_arr)) {
                            uint32_t barony_id = safe_read<uint32_t>(sub_arr);  // first sub-title ID
                            uint32_t barony_masked = barony_id & 0xFFFFFF;
                            if (barony_masked < capacity) {
                                uintptr_t barony = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                                   + static_cast<uintptr_t>(barony_masked) * ck3::title_db::ELEMENT_STRIDE);
                                if (barony && is_mapped(barony)) {
                                    // Get province from barony+0x490
                                    uintptr_t province = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
                                    if (province && is_mapped(province)) {
                                        // Get county_data from province+0x428
                                        county_data = safe_read<uintptr_t>(province + ck3::province::COUNTY_DATA_PTR);
                                        // Terrain (playbook: MaA-doomstack terrain matching). Chain:
                                        // province +0x008 → CJominiProvinceTemplate +0xB8 → CTerrainType* +0x10 key.
                                        // (re_cheap_read_offsets_terrain_stationing_truce.md, HIGH, core-dump validated.)
                                        uintptr_t tmpl = safe_read<uintptr_t>(province + 0x008);
                                        if (tmpl && is_mapped(tmpl)) {
                                            uintptr_t terr = safe_read<uintptr_t>(tmpl + ck3::province_template::TERRAIN_TYPE_PTR);
                                            if (terr && is_mapped(terr)) {
                                                std::string tn = ad_read_std_string(terr + ck3::db_object::NAME_KEY);
                                                if (!tn.empty()) { r.key("terrain"); r.value_string(tn.c_str(), tn.size()); }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if (!county_data || !is_mapped(county_data)) {
                    r.key("holdings"); r.begin_array(); r.end_array();
                    r.end_object();
                    county_count++;
                    continue;
                }

                // Development level (0-100)
                int32_t dev = safe_read<int32_t>(county_data + ck3::county::DEVELOPMENT);
                if (dev >= 0 && dev <= 200) {
                    r.key("development"); r.value_int(dev);
                }

                // County control (R16: int64 fixed-point ÷ 100000, range 0-100)
                int64_t control_fp = safe_read<int64_t>(county_data + ck3::county::CONTROL);
                double control = static_cast<double>(control_fp) / 100000.0;
                if (control >= 0.0 && control <= 110.0) {
                    r.key("control"); r.value_float(control);
                }

                // #127 (Gen-26 F4): per-county faith/culture + matches_ruler. Lets the
                // agent target conversion (task_conversion / task_promote_culture) at the
                // counties that are actually wrong-faith / wrong-culture, instead of just
                // seeing "low control" with no cause. county::FAITH_ID=0x208 (HIGH),
                // county::CULTURE_ID=0x20C (MEDIUM-HIGH) — core-dump validated (faith 23 =
                // "catholic", culture 229/231 vary per county). Pure safe_read, pause-safe.
                uint32_t cf = safe_read<uint32_t>(county_data + ck3::county::FAITH_ID);
                uint32_t cc = safe_read<uint32_t>(county_data + ck3::county::CULTURE_ID);
                bool faith_known = (cf != 0 && cf != 0xFFFFFFFF);
                bool cult_known  = (cc != 0 && cc != 0xFFFFFFFF);
                bool matches_faith = false, matches_culture = false;
                if (faith_known) {
                    r.key("faith_id"); r.value_uint(cf & 0xFFFFFF);
                    uintptr_t fo = ad_db_lookup(ck3::globals::FAITH_DB, cf);
                    if (fo) {
                        std::string fn = ad_read_std_string(fo + faith::FAITH_KEY);
                        if (!fn.empty()) { r.key("faith"); r.value_string(fn.c_str(), fn.size()); }
                    }
                    if (ruler_faith_id != 0 && ruler_faith_id != 0xFFFFFFFF) {
                        matches_faith = ((cf & 0xFFFFFF) == (ruler_faith_id & 0xFFFFFF));
                        r.key("matches_ruler_faith"); r.value_bool(matches_faith);
                    }
                }
                if (cult_known) {
                    // FIX #culture_name: emit_culture_fields (NAME +0x260 / SCRIPT_KEY +0x1C8),
                    // was reading +0x10 (the uint32 culture ID) as a std::string.
                    uintptr_t co = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, cc);
                    emit_culture_fields(r, co, cc);
                    if (ruler_culture_id != 0 && ruler_culture_id != 0xFFFFFFFF) {
                        matches_culture = ((cc & 0xFFFFFF) == (ruler_culture_id & 0xFFFFFF));
                        r.key("matches_ruler_culture"); r.value_bool(matches_culture);
                    }
                }
                // matches_ruler = faith AND culture both match (a fully "native" county).
                // Only emitted when both ids are known AND the ruler has both — otherwise
                // a missing read would make a wrong-faith county look matching.
                if (faith_known && cult_known
                    && ruler_faith_id != 0 && ruler_faith_id != 0xFFFFFFFF
                    && ruler_culture_id != 0 && ruler_culture_id != 0xFFFFFFFF) {
                    r.key("matches_ruler"); r.value_bool(matches_faith && matches_culture);
                }

                // Development progress toward next level (ck-3nzw)
                int64_t dev_prog_fp = safe_read<int64_t>(county_data + ck3::county::DEV_PROGRESS);
                double dev_prog_pct = static_cast<double>(dev_prog_fp) / 10000000.0 * 100.0;
                if (dev_prog_pct >= 0.0 && dev_prog_pct <= 100.0) {
                    r.key("dev_progress_pct"); r.value_float(dev_prog_pct);
                }

                // Average neighbor development level (ck-3nzw)
                uintptr_t nbr_arr = safe_read<uintptr_t>(county_data + ck3::county::NEIGHBOR_ARRAY);
                int32_t nbr_cnt = safe_read<int32_t>(county_data + ck3::county::NEIGHBOR_COUNT);
                if (nbr_cnt < 0 || nbr_cnt > 100) nbr_cnt = 0;
                if (nbr_arr && is_mapped(nbr_arr) && nbr_cnt > 0) {
                    int64_t nbr_dev_sum = 0;
                    int32_t nbr_valid = 0;
                    for (int32_t k = 0; k < nbr_cnt; k++) {
                        uintptr_t nbr = safe_read<uintptr_t>(nbr_arr + k * 8);
                        if (!nbr || !is_mapped(nbr)) continue;
                        int32_t nbr_dev = safe_read<int32_t>(nbr + ck3::county::DEVELOPMENT);
                        if (nbr_dev >= 0 && nbr_dev <= 200) {
                            nbr_dev_sum += nbr_dev;
                            nbr_valid++;
                        }
                    }
                    if (nbr_valid > 0) {
                        r.key("neighbor_count"); r.value_int(nbr_valid);
                        r.key("neighbor_avg_dev"); r.value_float(static_cast<double>(nbr_dev_sum) / nbr_valid);
                    }
                }

                // Province array: baronies/holdings within this county
                uintptr_t prov_arr = safe_read<uintptr_t>(county_data + ck3::county::PROVINCE_ARRAY);
                int32_t prov_cnt = safe_read<int32_t>(county_data + ck3::county::PROVINCE_COUNT);
                if (prov_cnt < 0 || prov_cnt > 16) prov_cnt = 0;

                r.key("holdings"); r.begin_array();
                if (prov_arr && is_mapped(prov_arr) && prov_cnt > 0) {
                    for (int32_t j = 0; j < prov_cnt; j++) {
                        uintptr_t prov = safe_read<uintptr_t>(prov_arr + j * 8);
                        if (!prov || !is_mapped(prov)) continue;

                        r.begin_object();

                        // Barony title id of this holding (province+0x318, HIGH).
                        // This is the landed-title id the construction command's
                        // +0x24 location field expects (see construction_check).
                        uint32_t barony_tid = safe_read<uint32_t>(prov + ck3::province::BARONY_TITLE_ID);
                        if (barony_tid != 0 && barony_tid != 0xFFFFFFFF) {
                            r.key("barony_title_id"); r.value_uint(barony_tid & 0xFFFFFF);
                        }

                        // Holding type name
                        uintptr_t holding_type = safe_read<uintptr_t>(prov + ck3::province::HOLDING_TYPE_PTR);
                        if (holding_type && is_mapped(holding_type)) {
                            auto ht_name = ad_read_std_string(holding_type + ck3::db_object::NAME_KEY);
                            if (!ht_name.empty()) {
                                r.key("holding_type"); r.value_string(ht_name.c_str(), ht_name.size());
                            }
                        }

                        // Holder character ID
                        uint32_t holder = safe_read<uint32_t>(prov + ck3::province::HOLDER_CHAR_ID);
                        if (holder != 0 && holder != 0xFFFFFFFF) {
                            r.key("holder_id"); r.value_uint(holder & 0xFFFFFF);
                        }

                        // Building slots (province+0x1F8 is building slot container)
                        // slot_container is an inline sub-struct, not a pointer — it's at
                        // a fixed offset within the province object, so it's valid if prov is.
                        uintptr_t slot_container = prov + ck3::province::BUILDING_SLOT_DATA;
                        int32_t total_slots = safe_read<int32_t>(slot_container + 0x24);
                        if (total_slots >= 0 && total_slots <= 20) {
                            r.key("building_slots_total"); r.value_int(total_slots);

                            // Iterate slot array: emit building names and levels (R16)
                            uintptr_t slot_arr = safe_read<uintptr_t>(slot_container + 0x18);
                            if (slot_arr && is_mapped(slot_arr) && total_slots > 0) {
                                int used = 0;
                                r.key("buildings"); r.begin_array();
                                for (int32_t s = 0; s < total_slots; s++) {
                                    uintptr_t btype = safe_read<uintptr_t>(slot_arr + s * 0x10);
                                    if (!btype || !is_mapped(btype)) continue;
                                    used++;

                                    r.begin_object();
                                    r.key("slot"); r.value_int(s);

                                    // R16: CBuildingType+0x10 — may be a std::string or a
                                    // char*. Try reading as std::string first (safer with
                                    // our char-by-char ad_read_std_string), fall back to
                                    // direct char* if that yields nothing.
                                    auto bname_str = ad_read_std_string(btype + ck3::building_type::NAME_PTR);
                                    if (!bname_str.empty()) {
                                        r.key("type"); r.value_string(bname_str.c_str(), bname_str.size());
                                    } else {
                                        // Fallback: treat NAME_PTR as char* (original assumption)
                                        uintptr_t name_ptr = safe_read<uintptr_t>(btype + ck3::building_type::NAME_PTR);
                                        if (name_ptr && is_mapped(name_ptr)) {
                                            char bname[64];
                                            bool name_ok = true;
                                            for (int n = 0; n < 63; n++) {
                                                bname[n] = safe_read<char>(name_ptr + n);
                                                if (bname[n] == '\0') break;
                                                if (bname[n] < 0x20 || bname[n] > 0x7e) { name_ok = false; break; }
                                            }
                                            bname[63] = '\0';
                                            if (name_ok && bname[0] != '\0') {
                                                r.key("type"); r.value_string(bname, strlen(bname));
                                            }
                                        }
                                    }

                                    // R16: CBuildingType+0x30 = int32 level
                                    int32_t level = safe_read<int32_t>(btype + ck3::building_type::LEVEL);
                                    if (level >= 0 && level <= 20) {
                                        r.key("level"); r.value_int(level);
                                    }

                                    r.end_object();
                                }
                                r.end_array();
                                r.key("building_slots_used"); r.value_int(used);
                            }
                        }

                        r.end_object();
                    }
                }
                r.end_array();

                r.end_object();
                county_count++;
            }
        }
    }
    r.end_array();
    r.key("county_count"); r.value_int(county_count);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// Helper: navigate county title_id → CCountyData pointer (ck-3nzw)
// Returns 0 if navigation fails at any step.
//
// ck-86ue.3: migrated to safe_read<T> + is_mapped() at every derived-pointer
// hop. Shared by do_query_development_growth (Tier-2 inline-safe gated) —
// the previous raw safe_read<>() chain could SIGSEGV if any link was unmapped
// (sentinel 0x1, freed-and-reused, half-constructed object).
static uintptr_t resolve_county_data(uint32_t title_id) {
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    if (!db || !is_mapped(db)) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT);
    if (!table || !is_mapped(table) || capacity == 0) return 0;

    uint32_t masked = title_id & 0xFFFFFF;
    if (masked >= capacity) return 0;

    uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                       + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
    if (!entry || !is_mapped(entry)) return 0;
    if ((safe_read<uint32_t>(entry + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != masked) return 0;

    // Navigate: county title → first barony sub-title → province → county_data
    int32_t sub_count = safe_read<int32_t>(entry + ck3::title_obj::SUB_TITLE_COUNT);
    if (sub_count <= 0) return 0;
    uintptr_t sub_arr = safe_read<uintptr_t>(entry + ck3::title_obj::SUB_TITLE_ARRAY);
    if (!sub_arr || !is_mapped(sub_arr)) return 0;

    uint32_t barony_id = safe_read<uint32_t>(sub_arr) & 0xFFFFFF;
    if (barony_id >= capacity) return 0;
    uintptr_t barony = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                        + static_cast<uintptr_t>(barony_id) * ck3::title_db::ELEMENT_STRIDE);
    if (!barony || !is_mapped(barony)) return 0;

    uintptr_t province = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
    if (!province || !is_mapped(province)) return 0;

    uintptr_t county_data = safe_read<uintptr_t>(province + ck3::province::COUNTY_DATA_PTR);
    if (county_data && !is_mapped(county_data)) return 0;
    return county_data;
}

// query_development_growth: estimate monthly development growth for counties (ck-3nzw)
// If title_id is provided, queries that specific county. Otherwise queries all counties
// of the specified character (like query_county_details).
bool do_query_development_growth(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");

    // Single county mode: resolve by title_id
    if (title_id_param > 0) {
        uintptr_t county_data = resolve_county_data(static_cast<uint32_t>(title_id_param));
        if ((!county_data || !is_mapped(county_data))) {
            if (out) *out = "{\"error\":\"county not found or navigation failed\"}";
            return false;
        }

        ResponseBuilder r;
        r.begin_object();
        r.key("title_id"); r.value_uint(static_cast<uint32_t>(title_id_param) & 0xFFFFFF);

        int32_t dev = safe_read<int32_t>(county_data + ck3::county::DEVELOPMENT);
        if (dev >= 0 && dev <= 200) { r.key("development"); r.value_int(dev); }

        int64_t prog_fp = safe_read<int64_t>(county_data + ck3::county::DEV_PROGRESS);
        r.key("dev_progress_pct"); r.value_float(static_cast<double>(prog_fp) / 10000000.0 * 100.0);

        // Neighbor spread estimation
        uintptr_t nbr_arr = safe_read<uintptr_t>(county_data + ck3::county::NEIGHBOR_ARRAY);
        int32_t nbr_cnt = safe_read<int32_t>(county_data + ck3::county::NEIGHBOR_COUNT);
        if (nbr_cnt < 0 || nbr_cnt > 100) nbr_cnt = 0;
        r.key("neighbor_count"); r.value_int(nbr_cnt);

        double nbr_avg = 0.0;
        double spread_sum = 0.0;
        int32_t nbr_valid = 0;
        if ((nbr_arr && is_mapped(nbr_arr)) && nbr_cnt > 0) {
            for (int32_t k = 0; k < nbr_cnt; k++) {
                uintptr_t nbr = safe_read<uintptr_t>(nbr_arr + k * 8);
                if ((!nbr || !is_mapped(nbr))) continue;
                int32_t nbr_dev = safe_read<int32_t>(nbr + ck3::county::DEVELOPMENT);
                if (nbr_dev < 0 || nbr_dev > 200) continue;
                nbr_valid++;
                nbr_avg += nbr_dev;
                int32_t diff = nbr_dev - dev;
                if (diff > 0) spread_sum += diff;
            }
        }
        if (nbr_valid > 0) {
            r.key("neighbor_avg_dev"); r.value_float(nbr_avg / nbr_valid);
        }
        r.key("estimated_neighbor_spread"); r.value_float(spread_sum);

        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    // Multi-county mode: iterate character's counties
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = "{\"error\":\"character not found\"}";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if ((!tr || !is_mapped(tr))) {
        if (out) *out = "{\"error\":\"no title realm data\"}";
        return false;
    }

    uintptr_t tdata = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t tcnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (tcnt < 0 || tcnt > 500) tcnt = 0;

    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    uintptr_t table = (db && is_mapped(db)) ? safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR) : 0;
    uint32_t capacity = (db && is_mapped(db)) ? safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT) : 0;

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_get_id(character) & 0xFFFFFF);
    r.key("counties"); r.begin_array();

    int county_count = 0;
    if ((tdata && is_mapped(tdata)) && (table && is_mapped(table)) && capacity > 0) {
        for (int32_t i = 0; i < tcnt && county_count < 64; i++) {
            uint32_t tid = safe_read<uint32_t>(tdata + i * 4);
            uint32_t masked = tid & 0xFFFFFF;
            if (masked >= capacity) continue;

            uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                               + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
            if ((!entry || !is_mapped(entry))) continue;
            if ((safe_read<uint32_t>(entry + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != masked) continue;

            uintptr_t type_ptr = safe_read<uintptr_t>(entry + ck3::title_obj::TITLE_TYPE_PTR);
            if ((!type_ptr || !is_mapped(type_ptr))) continue;
            if (safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER) != 2) continue;

            uintptr_t county_data = resolve_county_data(tid);
            if ((!county_data || !is_mapped(county_data))) continue;

            r.begin_object();
            r.key("title_id"); r.value_uint(masked);

            auto title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
            if (!title_key.empty()) {
                r.key("key"); r.value_string(title_key.c_str(), title_key.size());
            }

            int32_t dev = safe_read<int32_t>(county_data + ck3::county::DEVELOPMENT);
            if (dev >= 0 && dev <= 200) { r.key("development"); r.value_int(dev); }

            int64_t prog_fp = safe_read<int64_t>(county_data + ck3::county::DEV_PROGRESS);
            r.key("dev_progress_pct"); r.value_float(static_cast<double>(prog_fp) / 10000000.0 * 100.0);

            uintptr_t nbr_arr = safe_read<uintptr_t>(county_data + ck3::county::NEIGHBOR_ARRAY);
            int32_t nbr_cnt = safe_read<int32_t>(county_data + ck3::county::NEIGHBOR_COUNT);
            if (nbr_cnt < 0 || nbr_cnt > 100) nbr_cnt = 0;
            r.key("neighbor_count"); r.value_int(nbr_cnt);

            double nbr_avg = 0.0;
            double spread_sum = 0.0;
            int32_t nbr_valid = 0;
            if ((nbr_arr && is_mapped(nbr_arr)) && nbr_cnt > 0) {
                for (int32_t k = 0; k < nbr_cnt; k++) {
                    uintptr_t nbr = safe_read<uintptr_t>(nbr_arr + k * 8);
                    if ((!nbr || !is_mapped(nbr))) continue;
                    int32_t nbr_dev = safe_read<int32_t>(nbr + ck3::county::DEVELOPMENT);
                    if (nbr_dev < 0 || nbr_dev > 200) continue;
                    nbr_valid++;
                    nbr_avg += nbr_dev;
                    int32_t diff = nbr_dev - dev;
                    if (diff > 0) spread_sum += diff;
                }
            }
            if (nbr_valid > 0) {
                r.key("neighbor_avg_dev"); r.value_float(nbr_avg / nbr_valid);
            }
            r.key("estimated_neighbor_spread"); r.value_float(spread_sum);

            r.end_object();
            county_count++;
        }
    }

    r.end_array();
    r.key("county_count"); r.value_int(county_count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_province_adjacency: true geographic adjacency via province template SAdjacency data (ck-s1eb)
// Walks province_template adjacency arrays to find physically bordering provinces/counties,
// independent of de jure hierarchy. Returns per-province adjacency with type, distance, and
// the owning county/holder for each neighbor.
bool do_query_province_adjacency(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    if (title_id_param <= 0) {
        if (out) *out = R"({"error":"missing_title_id","hint":"Provide county title_id parameter"})";
        return false;
    }

    uint32_t masked_id = static_cast<uint32_t>(title_id_param) & 0xFFFFFF;

    // Resolve title DB
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    if ((!db || !is_mapped(db))) {
        if (out) *out = R"({"error":"title_db_not_found"})";
        return false;
    }
    uintptr_t table = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT);
    if ((!table || !is_mapped(table)) || capacity == 0) {
        if (out) *out = R"({"error":"title_db_empty"})";
        return false;
    }

    // Resolve the county title
    if (masked_id >= capacity) {
        if (out) *out = R"({"error":"title_id_out_of_range"})";
        return false;
    }
    uintptr_t county_entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                              + static_cast<uintptr_t>(masked_id) * ck3::title_db::ELEMENT_STRIDE);
    if ((!county_entry || !is_mapped(county_entry))) {
        if (out) *out = R"({"error":"county_not_found"})";
        return false;
    }
    if ((safe_read<uint32_t>(county_entry + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != masked_id) {
        if (out) *out = R"({"error":"title_id_mismatch"})";
        return false;
    }

    // Verify it's a county (tier 2)
    uintptr_t type_ptr = safe_read<uintptr_t>(county_entry + ck3::title_obj::TITLE_TYPE_PTR);
    if ((!type_ptr || !is_mapped(type_ptr)) || safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER) != 2) {
        if (out) *out = "{\"error\":\"not_a_county\",\"hint\":\"title_id must be a county tier 2\"}";
        return false;
    }

    // Get sub-titles (baronies) of this county
    int32_t sub_count = safe_read<int32_t>(county_entry + ck3::title_obj::SUB_TITLE_COUNT);
    uintptr_t sub_arr = safe_read<uintptr_t>(county_entry + ck3::title_obj::SUB_TITLE_ARRAY);
    if (sub_count <= 0 || sub_count > 20 || (!sub_arr || !is_mapped(sub_arr))) {
        if (out) *out = R"({"error":"no_baronies_found"})";
        return false;
    }

    // Helper lambda: resolve barony_title_id → province → template
    auto resolve_province_template = [&](uint32_t barony_id) -> uintptr_t {
        uint32_t bm = barony_id & 0xFFFFFF;
        if (bm >= capacity) return 0;
        uintptr_t barony = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                            + static_cast<uintptr_t>(bm) * ck3::title_db::ELEMENT_STRIDE);
        if ((!barony || !is_mapped(barony))) return 0;
        uintptr_t province = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
        if ((!province || !is_mapped(province))) return 0;
        uintptr_t tmpl = safe_read<uintptr_t>(province + ck3::province::TEMPLATE_PTR);
        return (tmpl && is_mapped(tmpl)) ? tmpl : 0;
    };

    // Collect province IDs belonging to THIS county (so we can exclude self-adjacency)
    std::set<int32_t> own_province_ids;
    for (int32_t i = 0; i < sub_count; i++) {
        uint32_t barony_id = safe_read<uint32_t>(sub_arr + i * 4);
        uintptr_t tmpl = resolve_province_template(barony_id);
        if (tmpl) {
            int32_t prov_id = safe_read<int32_t>(tmpl + ck3::province_template::PROVINCE_ID);
            if (prov_id > 0) own_province_ids.insert(prov_id);
        }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("title_id"); r.value_uint(masked_id);

    // County display name
    auto display_name = ad_read_std_string(county_entry + ck3::title_obj::TITLE_DISPLAY_NAME);
    if (!display_name.empty()) {
        r.key("name"); r.value_string(display_name.c_str(), display_name.size());
    }

    // Track unique neighbor counties to avoid duplicates from multi-province adjacency
    struct NeighborInfo {
        uint32_t county_title_id;
        uint32_t holder_id;
        int32_t type_flags;    // from first adjacency seen
        int32_t distance;      // minimum distance seen
        int province_count;    // how many province-level adjacencies
    };
    std::map<uint32_t, NeighborInfo> neighbor_counties;  // keyed by county title_id

    // Also collect raw province-level adjacency for detail
    r.key("province_adjacencies"); r.begin_array();
    int adj_total = 0;

    for (int32_t bi = 0; bi < sub_count; bi++) {
        uint32_t barony_id = safe_read<uint32_t>(sub_arr + bi * 4);
        uintptr_t tmpl = resolve_province_template(barony_id);
        if (!tmpl) continue;

        int32_t my_prov_id = safe_read<int32_t>(tmpl + ck3::province_template::PROVINCE_ID);
        uintptr_t adj_ptr = safe_read<uintptr_t>(tmpl + ck3::province_template::ADJACENCY_ARRAY_PTR);
        int32_t adj_count = safe_read<int32_t>(tmpl + ck3::province_template::ADJACENCY_COUNT);
        if ((!adj_ptr || !is_mapped(adj_ptr)) || adj_count <= 0 || adj_count > 50) continue;

        for (int32_t ai = 0; ai < adj_count; ai++) {
            uintptr_t entry = adj_ptr + static_cast<uintptr_t>(ai) * ck3::adjacency::STRIDE;
            int32_t neighbor_prov_id = safe_read<int32_t>(entry + ck3::adjacency::NEIGHBOR_PROV_ID);
            if (neighbor_prov_id <= 0) continue;

            // Skip self-county provinces
            if (own_province_ids.count(neighbor_prov_id)) continue;

            int32_t type_flags = safe_read<int32_t>(entry + ck3::adjacency::TYPE_FLAGS);
            int32_t distance = safe_read<int32_t>(entry + ck3::adjacency::DISTANCE);

            // Emit raw province adjacency
            r.begin_object();
            r.key("from_province_id"); r.value_int(my_prov_id);
            r.key("to_province_id"); r.value_int(neighbor_prov_id);
            r.key("type"); r.value_int(type_flags);
            r.key("distance"); r.value_int(distance);

            // Reverse-map neighbor province_id → barony → county → holder
            // Scan title DB for barony whose province template has this province_id
            // This is O(n) per lookup but adjacency counts are small (< 50 total)
            uint32_t nbr_county_id = 0;
            uint32_t nbr_holder_id = 0;
            for (uint32_t tid = 0; tid < capacity && tid < 30000; tid++) {
                uintptr_t te = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                + static_cast<uintptr_t>(tid) * ck3::title_db::ELEMENT_STRIDE);
                if ((!te || !is_mapped(te))) continue;
                // Check if barony (tier 1) — only baronies have provinces
                uintptr_t te_type = safe_read<uintptr_t>(te + ck3::title_obj::TITLE_TYPE_PTR);
                if ((!te_type || !is_mapped(te_type))) continue;
                if (safe_read<int32_t>(te_type + ck3::title_obj::TITLE_TYPE_TIER) != 1) continue;

                uintptr_t prov = safe_read<uintptr_t>(te + ck3::title_obj::PROVINCE_PTR);
                if ((!prov || !is_mapped(prov))) continue;
                uintptr_t pt = safe_read<uintptr_t>(prov + ck3::province::TEMPLATE_PTR);
                if ((!pt || !is_mapped(pt))) continue;
                if (safe_read<int32_t>(pt + ck3::province_template::PROVINCE_ID) != neighbor_prov_id) continue;

                // Found! Get de jure parent (county)
                nbr_county_id = safe_read<uint32_t>(te + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                // Get holder of the county
                if (nbr_county_id > 0 && nbr_county_id < capacity) {
                    uintptr_t county_te = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                           + static_cast<uintptr_t>(nbr_county_id) * ck3::title_db::ELEMENT_STRIDE);
                    if ((county_te && is_mapped(county_te))) {
                        nbr_holder_id = safe_read<uint32_t>(county_te + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
                    }
                }
                break;
            }

            if (nbr_county_id > 0) {
                r.key("county_id"); r.value_uint(nbr_county_id);
            }
            if (nbr_holder_id > 0 && nbr_holder_id != 0xFFFFFF) {
                r.key("holder_id"); r.value_uint(nbr_holder_id);
            }
            r.end_object();
            adj_total++;

            // Aggregate into neighbor_counties
            if (nbr_county_id > 0) {
                auto& ni = neighbor_counties[nbr_county_id];
                if (ni.county_title_id == 0) {
                    ni.county_title_id = nbr_county_id;
                    ni.holder_id = nbr_holder_id;
                    ni.type_flags = type_flags;
                    ni.distance = distance;
                    ni.province_count = 1;
                } else {
                    if (distance < ni.distance) ni.distance = distance;
                    ni.province_count++;
                }
            }
        }
    }
    r.end_array();

    // Summary: unique neighboring counties
    r.key("neighbor_counties"); r.begin_array();
    for (auto& [cid, ni] : neighbor_counties) {
        r.begin_object();
        r.key("county_id"); r.value_uint(ni.county_title_id);
        if (ni.holder_id > 0 && ni.holder_id != 0xFFFFFF) {
            r.key("holder_id"); r.value_uint(ni.holder_id);
        }
        r.key("border_provinces"); r.value_int(ni.province_count);
        r.key("min_distance"); r.value_int(ni.distance);
        r.key("adjacency_type"); r.value_int(ni.type_flags);

        // Get county name
        if (ni.county_title_id < capacity) {
            uintptr_t nbr_entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                                    + static_cast<uintptr_t>(ni.county_title_id) * ck3::title_db::ELEMENT_STRIDE);
            if ((nbr_entry && is_mapped(nbr_entry))) {
                auto nbr_name = ad_read_std_string(nbr_entry + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!nbr_name.empty()) {
                    r.key("name"); r.value_string(nbr_name.c_str(), nbr_name.size());
                }
            }
        }
        r.end_object();
    }
    r.end_array();

    r.key("adjacency_count"); r.value_int(adj_total);
    r.key("neighbor_county_count"); r.value_int(static_cast<int>(neighbor_counties.size()));
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// lane-query item 4 helpers for query_county_neighbors.
//
// Tier of a title id (1 barony .. 5 empire), 0 when unreadable.
static int nb_title_tier(uint32_t tid) {
    uintptr_t t = lookup_title_obj(tid & 0xFFFFFF);
    if (!t) return 0;
    uintptr_t tp = safe_read<uintptr_t>(t + ck3::title_obj::TITLE_TYPE_PTR);
    if (!tp || !is_mapped(tp)) return 0;
    int tier = safe_read<int32_t>(tp + ck3::title_obj::TITLE_TYPE_TIER);
    return (tier >= 1 && tier <= 5) ? tier : 0;
}

// A ruler's capital COUNTY: the capital barony's de-jure parent
// (title_realm+CAPITAL_BARONY_ID -> title_obj+DE_JURE_PARENT, both HIGH), else
// the first county in the held-title array. 0 when the character holds none.
static uint32_t nb_capital_county(uintptr_t c, const char** how) {
    uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (!tr || !is_mapped(tr)) return 0;
    uint32_t barony = safe_read<uint32_t>(tr + ck3::title_realm::CAPITAL_BARONY_ID);
    if (barony != 0 && barony != 0xFFFFFFFF) {
        uintptr_t bobj = lookup_title_obj(barony & 0xFFFFFF);
        if (bobj) {
            uint32_t parent = safe_read<uint32_t>(bobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
            if (parent && parent != 0xFFFFFF && nb_title_tier(parent) == 2) {
                if (how) *how = "capital_county";
                return parent;
            }
        }
    }
    uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t n = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (arr && is_mapped(arr) && n > 0 && n < 500) {
        for (int32_t i = 0; i < n; i++) {
            uint32_t tid = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
            if (nb_title_tier(tid) == 2) {
                if (how) *how = "first_held_county";
                return tid;
            }
        }
    }
    return 0;
}

// Is character c inside root's realm (c == root, or root is somewhere up c's
// liege chain)? Depth-capped, pure reads.
static bool nb_in_realm_of(uintptr_t c, uint32_t root) {
    uint32_t cur_id = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;
    uintptr_t cur = c;
    for (int depth = 0; depth < 30 && cur; depth++) {
        if (cur_id == root) return true;
        uint32_t liege = get_liege_id_from_addr(cur) & 0xFFFFFF;
        if (liege == 0 || liege == 0xFFFFFF || liege == cur_id) return false;
        void* next = lookup_character(liege);
        if (!next || is_sentinel(next)) return false;
        cur = reinterpret_cast<uintptr_t>(next);
        cur_id = liege;
    }
    return false;
}

// Holder of a county title (title_obj+HOLDER_CHAR_ID, HIGH); 0 when vacant.
static uint32_t nb_county_holder(uint32_t county_tid) {
    uintptr_t t = lookup_title_obj(county_tid & 0xFFFFFF);
    if (!t) return 0;
    uint32_t h = safe_read<uint32_t>(t + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
    return (h == 0xFFFFFF) ? 0 : h;
}

// query_county_neighbors: list neighboring counties with dev/faith/culture (ck-3nzw)
//
// lane-query item 4: title_id is no longer required. With title_id=0 the
// character (character_id, 0 = the controlled ruler) is resolved instead:
//   * the per-county list is for that character's capital county
//     (resolved_title_id / resolved_from say which county and how);
//   * bordering_realms[] answers "which realms border character X": every
//     county of X's realm (own + vassals) is walked, and each neighbouring
//     county held OUTSIDE that realm is credited to its holder's top liege.
// Every neighbour row now also carries its county title_id, holder_id and
// realm_ruler_id (the holder's top liege), which the old reply did not.
bool do_query_county_neighbors(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    const bool char_mode = (title_id_param <= 0);
    uint32_t root_id = 0;
    std::string root_name;
    const char* resolved_from = "title_id";
    if (char_mode) {
        int cid_param = json_get_int(params_json, "character_id");
        void* ch = resolve_character(params_json);
        if (!ch || is_sentinel(ch)) {
            if (out) *out = (cid_param > 0)
                ? R"J({"error":"character_not_found","hint":"character_id does not name a living character; or pass title_id (a county)"})J"
                : R"J({"error":"missing_title_id","hint":"pass title_id (a county), or character_id of a landed ruler; with neither, the controlled ruler is used and none is bound"})J";
            return false;
        }
        uintptr_t rc = reinterpret_cast<uintptr_t>(ch);
        root_id = safe_read<uint32_t>(rc + ck3::character::ID) & 0xFFFFFF;
        root_name = ad_read_char_name(rc);
        uint32_t cap = nb_capital_county(rc, &resolved_from);
        if (!cap) {
            if (out) *out = R"J({"error":"character_holds_no_county","hint":"this character holds no county, so it has no neighbours; pass title_id (a county) instead"})J";
            return false;
        }
        title_id_param = static_cast<int>(cap);
    }

    uintptr_t county_data = resolve_county_data(static_cast<uint32_t>(title_id_param));
    if ((!county_data || !is_mapped(county_data))) {
        if (out) *out = "{\"error\":\"county not found or navigation failed\"}";
        return false;
    }

    int32_t dev = safe_read<int32_t>(county_data + ck3::county::DEVELOPMENT);

    uintptr_t nbr_arr = safe_read<uintptr_t>(county_data + ck3::county::NEIGHBOR_ARRAY);
    int32_t nbr_cnt = safe_read<int32_t>(county_data + ck3::county::NEIGHBOR_COUNT);
    if (nbr_cnt < 0 || nbr_cnt > 100) nbr_cnt = 0;

    ResponseBuilder r;
    r.begin_object();
    r.key("title_id"); r.value_uint(static_cast<uint32_t>(title_id_param) & 0xFFFFFF);
    r.key("resolved_from"); r.value_string(resolved_from);
    if (char_mode) {
        r.key("character_id"); r.value_uint(root_id);
        if (!root_name.empty()) { r.key("character_name"); r.value_string(root_name.c_str(), root_name.size()); }
        r.key("resolved_title_id"); r.value_uint(static_cast<uint32_t>(title_id_param) & 0xFFFFFF);
    }
    r.key("development"); r.value_int(dev);
    r.key("neighbors"); r.begin_array();

    int valid_count = 0;
    if ((nbr_arr && is_mapped(nbr_arr)) && nbr_cnt > 0) {
        for (int32_t k = 0; k < nbr_cnt; k++) {
            uintptr_t nbr = safe_read<uintptr_t>(nbr_arr + k * 8);
            if ((!nbr || !is_mapped(nbr))) continue;

            r.begin_object();

            uint32_t ntid = safe_read<uint32_t>(nbr + ck3::county::COUNTY_TITLE_ID) & 0xFFFFFF;
            if (ntid && ntid != 0xFFFFFF) {
                r.key("title_id"); r.value_uint(ntid);
                uint32_t holder = nb_county_holder(ntid);
                if (holder) {
                    r.key("holder_id"); r.value_uint(holder);
                    void* hc = lookup_character(holder);
                    if (hc && !is_sentinel(hc)) {
                        r.key("realm_ruler_id");
                        r.value_uint(get_top_realm_ruler_id(reinterpret_cast<uintptr_t>(hc)) & 0xFFFFFF);
                    }
                }
            }

            int32_t nbr_dev = safe_read<int32_t>(nbr + ck3::county::DEVELOPMENT);
            if (nbr_dev >= 0 && nbr_dev <= 200) {
                r.key("development"); r.value_int(nbr_dev);
            }

            uint32_t faith = safe_read<uint32_t>(nbr + ck3::county::FAITH_ID);
            if (faith != 0 && faith != 0xFFFFFFFF) {
                r.key("faith_id"); r.value_uint(faith);
            }

            uint32_t culture = safe_read<uint32_t>(nbr + ck3::county::CULTURE_ID);
            if (culture != 0 && culture != 0xFFFFFFFF) {
                // FIX #culture_name: add culture + culture_key alongside culture_id.
                emit_culture_fields_by_id(r, culture);
            }

            // Dev difference: positive = neighbor is ahead
            r.key("dev_diff"); r.value_int(nbr_dev - dev);

            r.end_object();
            valid_count++;
        }
    }

    r.end_array();
    r.key("neighbor_count"); r.value_int(valid_count);

    if (char_mode) {
        // Realm counties of root (own + vassals), from one CHARACTER_DB walk.
        std::vector<uint32_t> realm_counties;
        std::unordered_set<uint32_t> realm_county_set;
        uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
        uintptr_t tbl = db ? safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR) : 0;
        uint32_t capacity = db ? safe_read<uint32_t>(db + ck3::hash_map::CAPACITY) : 0;
        uintptr_t sentinel = safe_read_offset<uintptr_t>(ck3::globals::CHAR_DB_SENTINEL);
        if (tbl && capacity > 0 && capacity <= 500000) {
            for (uint32_t i = 0; i < capacity && realm_counties.size() < 1000; i++) {
                uintptr_t c = safe_read<uintptr_t>(tbl + ck3::hash_map::ENTRY_HEADER_SIZE
                                                   + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
                if (!c || c < 0x10000 || c > 0x7FFFFFFFFFFF || c == sentinel) continue;
                if (safe_read<uint8_t>(c + ck3::character::DEAD_FLAG) != 0) continue;
                uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
                if (!tr || !is_mapped(tr)) continue;
                if (count_demesne_counties(c) <= 0) continue;
                if (!nb_in_realm_of(c, root_id)) continue;
                uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
                int32_t n = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
                if (!arr || !is_mapped(arr) || n <= 0 || n >= 500) continue;
                for (int32_t j = 0; j < n; j++) {
                    uint32_t tid = safe_read<uint32_t>(arr + j * 4) & 0xFFFFFF;
                    if (nb_title_tier(tid) == 2 && realm_county_set.insert(tid).second)
                        realm_counties.push_back(tid);
                }
            }
        }
        // Neighbouring counties outside the realm, credited to their top liege.
        std::map<uint32_t, int> by_ruler;
        std::unordered_set<uint32_t> outside_seen;
        int unheld = 0;
        for (uint32_t ctid : realm_counties) {
            uintptr_t cd = resolve_county_data(ctid);
            if (!cd || !is_mapped(cd)) continue;
            uintptr_t na = safe_read<uintptr_t>(cd + ck3::county::NEIGHBOR_ARRAY);
            int32_t nc = safe_read<int32_t>(cd + ck3::county::NEIGHBOR_COUNT);
            if (!na || !is_mapped(na) || nc <= 0 || nc > 100) continue;
            for (int32_t k = 0; k < nc; k++) {
                uintptr_t nbr = safe_read<uintptr_t>(na + k * 8);
                if (!nbr || !is_mapped(nbr)) continue;
                uint32_t ntid = safe_read<uint32_t>(nbr + ck3::county::COUNTY_TITLE_ID) & 0xFFFFFF;
                if (!ntid || ntid == 0xFFFFFF || realm_county_set.count(ntid)) continue;
                if (!outside_seen.insert(ntid).second) continue;
                uint32_t holder = nb_county_holder(ntid);
                void* hc = holder ? lookup_character(holder) : nullptr;
                if (!hc || is_sentinel(hc)) { unheld++; continue; }
                uintptr_t hcp = reinterpret_cast<uintptr_t>(hc);
                if (nb_in_realm_of(hcp, root_id)) continue;   // held inside after all
                by_ruler[get_top_realm_ruler_id(hcp) & 0xFFFFFF]++;
            }
        }
        std::vector<std::pair<uint32_t, int>> rows(by_ruler.begin(), by_ruler.end());
        std::stable_sort(rows.begin(), rows.end(), [](const std::pair<uint32_t, int>& a,
                                               const std::pair<uint32_t, int>& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first; });
        r.key("realm_county_count"); r.value_int(static_cast<int>(realm_counties.size()));
        r.key("border_county_count"); r.value_int(static_cast<int>(outside_seen.size()));
        if (unheld) { r.key("border_counties_unheld"); r.value_int(unheld); }
        r.key("bordering_realms"); r.begin_array();
        for (const auto& row : rows) {
            r.begin_object();
            r.key("ruler_id"); r.value_uint(row.first);
            void* rc = lookup_character(row.first);
            if (rc && !is_sentinel(rc)) {
                std::string nm = ad_read_char_name(reinterpret_cast<uintptr_t>(rc));
                if (!nm.empty()) { r.key("name"); r.value_string(nm.c_str(), nm.size()); }
            }
            r.key("border_counties"); r.value_int(row.second);
            r.end_object();
        }
        r.end_array();
        r.key("bordering_realm_count"); r.value_int(static_cast<int>(rows.size()));
        r.key("note"); r.value_string(
            "bordering_realms: counties adjacent to any county of this character's realm "
            "(own + vassals) and held outside it, grouped by the holder's top liege "
            "(realm_ruler_id). The per-county `neighbors` list is for resolved_title_id.");
    }
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// Resolve a siege's besieger (attacking) character id from the CSiege object:
//   CSiege+0xA0 attacker sub-object (gated, non-null = has besieger)
//   CSiege+0xA8 besieger army id → ARMY_DB[id]+0xEC owner-link id
//   → OWNER_LINK_DB[id]+0x16C owner character id
// Returns 0 if the chain breaks anywhere (all reads via safe_read/is_mapped).
// Mirrors FUN_06438260 (re_siege_mechanics.md / re_siege_progress_struct.md §4).
static uint32_t resolve_siege_besieger(uintptr_t siege_ptr, uint32_t* army_id_out) {
    if (army_id_out) *army_id_out = 0;
    uintptr_t atk_sub = safe_read<uintptr_t>(siege_ptr + ck3::siege_obj::ATTACKER_PTR);
    if (!atk_sub) return 0;  // no besieger
    uint32_t army_id = safe_read<uint32_t>(siege_ptr + ck3::siege_obj::ATTACKER_ARMY_ID);
    if (army_id == 0 || army_id == 0xFFFFFFFF) return 0;
    if (army_id_out) *army_id_out = army_id & 0xFFFFFF;
    uintptr_t army = ad_db_lookup(ck3::globals::ARMY_DB, army_id);
    if (!army || !is_mapped(army)) return 0;
    uint32_t link_id = safe_read<uint32_t>(army + ck3::army_obj::OWNER_LINK_ID);
    if (link_id == 0 || link_id == 0xFFFFFFFF) return 0;
    uintptr_t link = ad_db_lookup(ck3::globals::OWNER_LINK_DB, link_id);
    if (!link || !is_mapped(link)) return 0;
    return safe_read<uint32_t>(link + ck3::army_owner_link::OWNER_CHAR_ID) & 0xFFFFFF;
}

// Emit the per-war `sieges[]` array. There is NO CWar→siege list (verified:
// re_siege_progress_struct.md §1) — sieges are discovered by enumerating
// SIEGE_DB and filtering to this war: a siege belongs to the war if its
// besieger is one of the war's attacker participants. Progress = CSiege+0x1b0
// (int32 fixed-point /100000, 0..~20), gated by the phase enum CSiege+0x1b8
// (5=actively besieging, 0=idle). The `defender` is the war's primary defender
// (CSiege carries no clean per-siege target-title; the province→title back-link
// is a still-open RE question, so we report the war-level defender instead).
//
// Crash-safety: every read is safe_read/is_mapped; the SIEGE_DB walk is bounded
// by the DB capacity AND a hard cap (kMaxSiegeScan) so a corrupt capacity can't
// hang the read; emitted siege rows are capped at kMaxEmit.
static void emit_war_sieges(ResponseBuilder& r, uintptr_t war_ptr) {
    static constexpr uint32_t kMaxSiegeScan = 512;  // hard cap on DB slots walked
    static constexpr int      kMaxEmit      = 64;   // hard cap on emitted rows

    r.key("sieges"); r.begin_array();

    // Gather this war's attacker participant char ids (the sides that besiege).
    uint32_t atk_ids[64];
    int atk_n = 0;
    uintptr_t atk_container = war_ptr + ck3::war_obj::ATK_PARTICIPANT;
    uintptr_t atk_arr = safe_read<uintptr_t>(atk_container);
    int32_t atk_count = safe_read<int32_t>(atk_container + 0x0C);
    if (atk_count < 0 || atk_count > 100) atk_count = 0;
    if (atk_arr) {
        for (int32_t j = 0; j < atk_count && atk_n < 64; j++) {
            uintptr_t part_ptr = safe_read<uintptr_t>(atk_arr + j * 8);
            if (!part_ptr) continue;
            uint32_t pid = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
            if (pid) atk_ids[atk_n++] = pid;
        }
    }

    uint32_t primary_def = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;

    if (atk_n == 0) { r.end_array(); return; }

    // Enumerate SIEGE_DB (same layout as the char/army DBs: entries@+0x20,
    // capacity@+0x2C, entry stride 0x10, CSiege* at entry+0x08, id at obj+0x08).
    uintptr_t siege_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::SIEGE_DB);
    uintptr_t siege_null = safe_read<uintptr_t>(g_base_addr + ck3::globals::SIEGE_NULL);
    if (!siege_db) { r.end_array(); return; }
    uintptr_t entries = safe_read<uintptr_t>(siege_db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(siege_db + ck3::hash_map::CAPACITY);
    if (!entries || capacity == 0) { r.end_array(); return; }
    if (capacity > kMaxSiegeScan) capacity = kMaxSiegeScan;  // bound the walk

    int emitted = 0;
    for (uint32_t idx = 0; idx < capacity && emitted < kMaxEmit; idx++) {
        uintptr_t obj = safe_read<uintptr_t>(entries + ck3::hash_map::ENTRY_HEADER_SIZE
                                             + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
        if (!obj || obj == siege_null || !is_mapped(obj)) continue;
        // Validate the entry actually maps to this slot (low 24 bits of id == idx).
        uint32_t sid = safe_read<uint32_t>(obj + ck3::siege_obj::ID);
        if ((sid & 0xFFFFFF) != idx) continue;

        uint32_t army_id = 0;
        uint32_t besieger = resolve_siege_besieger(obj, &army_id);
        if (!besieger) continue;

        // War filter: the besieger must be one of this war's attackers.
        bool in_war = false;
        for (int k = 0; k < atk_n; k++) { if (atk_ids[k] == besieger) { in_war = true; break; } }
        if (!in_war) continue;

        int32_t prog_raw = safe_read<int32_t>(obj + ck3::siege_obj::PROGRESS);
        int32_t state    = safe_read<int32_t>(obj + ck3::siege_obj::STATE);
        double progress  = static_cast<double>(prog_raw) / 100000.0;  // 0.0 .. ~20.0
        bool active      = (state == 5);
        double max_obs   = static_cast<double>(ck3::siege_obj::PROGRESS_MAX_OBS);
        double pct       = (max_obs > 0.0) ? (progress / max_obs) * 100.0 : 0.0;
        if (pct < 0.0) pct = 0.0; if (pct > 100.0) pct = 100.0;

        // status: won = at/near cap; stalled = active-or-attacker-present but ~0
        // progress; ongoing = actively accruing. (active flag carries the engine
        // phase verbatim; status is the human-readable verdict.)
        const char* status;
        if (progress >= max_obs - 0.5)      status = "won";
        else if (active && progress > 0.05) status = "ongoing";
        else                                status = "stalled";

        r.begin_object();
        r.key("siege_id");       r.value_uint(sid & 0xFFFFFF);
        r.key("besieger_id");    r.value_uint(besieger);
        if (army_id) { r.key("besieger_army_id"); r.value_uint(army_id); }
        if (primary_def) { r.key("defender_id"); r.value_uint(primary_def); }
        r.key("progress");       r.value_double(progress);       // raw 0..~20
        r.key("progress_max");   r.value_double(max_obs);        // observed cap (~20)
        r.key("progress_pct");   r.value_double(pct);            // 0..100
        r.key("state");          r.value_int(state);             // engine phase enum
        r.key("active");         r.value_bool(active);           // state==5
        r.key("status");         r.value_string(status);
        r.end_object();
        emitted++;
    }

    r.end_array();
}

// query_wars: detailed war information for a character
// Build the JSON body for a single war object at `war_ptr`. Shared by the
// per-character path and the no-args WAR_DB-scan path so both return the same
// schema (minus the `is_attacker` field, which is only meaningful when the
// caller scoped the query to a character).
static void emit_war_entry(ResponseBuilder& r, uintptr_t war_ptr,
                           uint32_t scope_char_id, bool emit_is_attacker,
                           bool* scope_is_attacker_out, bool verbose = false) {
    uint32_t war_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);

    r.begin_object();

    r.key("war_id"); r.value_uint(war_id);

    // War name (SSO std::string at +0x2D8)
    std::string war_name = ad_read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR);
    if (!war_name.empty()) {
        r.key("name"); r.value_string(war_name.c_str(), war_name.size());
    }

    r.key("active"); r.value_bool(true);

    // CB info from embedded CCasusBelli sub-object at +0x98
    uintptr_t cb_type_ref = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TYPE_REF);
    if (cb_type_ref) {
        std::string cb_name = ad_read_std_string(cb_type_ref + ck3::db_object::NAME_KEY);
        if (!cb_name.empty()) {
            r.key("cb_type"); r.value_string(cb_name.c_str(), cb_name.size());
        }
    }

    uint32_t primary_atk_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
    uint32_t primary_def_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
    r.key("primary_attacker"); r.value_uint(primary_atk_ref);
    r.key("primary_defender"); r.value_uint(primary_def_ref);

    uintptr_t cb_titles_ptr = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TITLE_ARRAY);
    int32_t cb_title_count = safe_read<int32_t>(war_ptr + ck3::war_obj::CB_TITLE_COUNT);
    if (cb_title_count < 0 || cb_title_count > 20) cb_title_count = 0;
    r.key("target_titles"); r.begin_array();
    if (cb_titles_ptr) {
        for (int32_t t = 0; t < cb_title_count; t++) {
            uint64_t title_ref = safe_read<uint64_t>(cb_titles_ptr + t * 8);
            r.value_uint(static_cast<uint32_t>(title_ref & 0xFFFFFF));
        }
    }
    r.end_array();

    uintptr_t atk_container = war_ptr + ck3::war_obj::ATK_PARTICIPANT;
    uintptr_t atk_arr = safe_read<uintptr_t>(atk_container);
    int32_t atk_count = safe_read<int32_t>(atk_container + 0x0C);
    if (atk_count < 0 || atk_count > 100) atk_count = 0;

    uintptr_t def_container = war_ptr + ck3::war_obj::DEF_PARTICIPANT;
    uintptr_t def_arr = safe_read<uintptr_t>(def_container);
    int32_t def_count = safe_read<int32_t>(def_container + 0x0C);
    if (def_count < 0 || def_count > 100) def_count = 0;

    bool is_attacker = false;
    bool is_defender = false;

    // L1b: the full attacker/defender id arrays are not consumed by any agent
    // path (prompts.py / digestor.py read only primary_attacker/defender +
    // scalar scores). Skip emitting them in lean mode, but still scan to
    // compute is_attacker/is_defender (feeds is_attacker / my_side below).
    if (verbose) { r.key("attackers"); r.begin_array(); }
    if (atk_arr && atk_count > 0) {
        for (int32_t j = 0; j < atk_count && j < 20; j++) {
            uintptr_t part_ptr = safe_read<uintptr_t>(atk_arr + j * 8);
            if (!part_ptr) continue;
            uint32_t pid = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
            if (verbose) r.value_uint(pid);
            if (pid == scope_char_id) is_attacker = true;
        }
    }
    if (verbose) r.end_array();

    if (verbose) { r.key("defenders"); r.begin_array(); }
    if (def_arr && def_count > 0) {
        for (int32_t j = 0; j < def_count && j < 20; j++) {
            uintptr_t part_ptr = safe_read<uintptr_t>(def_arr + j * 8);
            if (!part_ptr) continue;
            uint32_t pid = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
            if (verbose) r.value_uint(pid);
            if (pid == scope_char_id) is_defender = true;
        }
    }
    if (verbose) r.end_array();

    if (emit_is_attacker) {
        r.key("is_attacker"); r.value_bool(is_attacker);
    }
    if (scope_is_attacker_out) *scope_is_attacker_out = is_attacker;

    int bc = 0;
    double bs = compute_battle_score(war_ptr, &bc);
    r.key("battle_score"); r.value_int(static_cast<int>(bs));
    r.key("battle_count"); r.value_int(bc);

    int ts = compute_ticking_score(war_ptr);
    r.key("ticking_score"); r.value_int(ts);

    // Attacker-relative score (positive = attacker advantage) used both for the
    // clamped fallback and for favored_side. bs/ts are computed attacker-minus-
    // defender (see compute_battle_score / compute_ticking_score in bridge.h).
    int attacker_advantage = static_cast<int>(bs) + ts;

    WarScoreBreakdown wbd{};
    WarScoreRejectReason wrej = WarScoreRejectReason::None;
    if (query_war_score_breakdown_fresh(war_id, &wbd, &wrej)) {
        r.key("war_score"); r.value_int(wbd.total);
        r.key("war_score_source"); r.value_string("computed", 8);
        // L1b: score_breakdown sub-object is not consumed by any agent path;
        // verbose-only.
        if (verbose) {
            r.key("score_breakdown"); r.begin_object();
            r.key("imprisonment"); r.value_int((wbd.imprisonment_atk + wbd.imprisonment_def) / 1000);
            r.key("battle"); r.value_int((wbd.battle_atk + wbd.battle_def) / 1000);
            r.key("occupation"); r.value_int((wbd.occupation_atk + wbd.occupation_def) / 1000);
            r.key("ticking"); r.value_int((wbd.ticking_atk + wbd.ticking_def) / 1000);
            r.key("landless"); r.value_int((wbd.landless_atk + wbd.landless_def) / 1000);
            r.end_object();
        }
    } else {
        // Cache MISS or STALE entry (hook refreshes are being rejected —
        // ck-xwlg): clamp the estimated (battle+ticking) score to [-100, 100]
        // rather than emitting the fossil 0.
        WarScoreBreakdown stale_probe{};
        bool had_stale = query_war_score_breakdown(war_id, &stale_probe);
        int fallback = attacker_advantage;
        if (fallback > 100) fallback = 100;
        if (fallback < -100) fallback = -100;
        r.key("war_score"); r.value_int(fallback);
        if (had_stale) {
            r.key("war_score_source"); r.value_string("computed_stale", 14);
            const char* rn = war_score_reject_name(wrej);
            r.key("score_hook_reject");
            r.value_string(rn, static_cast<int>(strlen(rn)));
        } else {
            r.key("war_score_source"); r.value_string("computed_fallback", 17);
        }
    }

    // Who is winning, and which side the scope character is on (TASK C).
    const char* favored_side = (attacker_advantage > 0) ? "attacker"
                             : (attacker_advantage < 0) ? "defender" : "even";
    r.key("favored_side"); r.value_string(favored_side);

    const char* my_side = is_attacker ? "attacker"
                        : is_defender ? "defender"
                        : (scope_char_id != 0) ? "neutral" : "unknown";
    r.key("my_side"); r.value_string(my_side);

    // Per-siege detail (additive — occupation_score remains the per-war aggregate).
    // SIEGE_DB cross-ref filtered to this war; crash-bounded. re_siege_progress.md
    emit_war_sieges(r, war_ptr);

    r.end_object();
}

// Enumerate every active war in WAR_DB. Used when the caller does not scope
// query_wars to a specific character (ck-fbij: no-args path previously returned
// success=false instead of surfacing the full active-war set).
static bool query_wars_all_active(std::string* out, bool verbose = false) {
    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (!war_db) {
        if (out) *out = R"({"success":false,"action":"query_wars","reason":"war_db_null"})";
        return false;
    }
    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
    if (!table || capacity == 0 || capacity > 100000) {
        if (out) *out = R"({"success":false,"action":"query_wars","reason":"war_db_invalid"})";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("scope"); r.value_string("all_active");
    r.key("wars"); r.begin_array();

    int war_count = 0;
    for (uint32_t idx = 0; idx < capacity && idx < 10000; idx++) {
        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
        uintptr_t war_ptr = safe_read<uintptr_t>(entry);
        if (!war_ptr || (!war_ptr || !is_mapped(war_ptr))) continue;

        uint32_t stored_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
        if ((stored_ref & 0xFFFFFF) != idx) continue;

        uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
        if (is_ended != 0) continue;

        emit_war_entry(r, war_ptr, /*scope_char_id=*/0,
                       /*emit_is_attacker=*/false, nullptr, verbose);
        war_count++;
    }

    r.end_array();
    r.key("war_count"); r.value_int(war_count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// L6: get_active_wars — unpaused-safe MINIMAL active-wars brief.
//
// The full per-turn report only surfaced war info in the paused-gated Tier-2
// block, so in an unpaused human-vs-agent game the agent never learned it was
// at war. This handler is bridge-internal (served inline even while UNPAUSED)
// and uses safe_read throughout (process_vm_readv → no crash on torn reads;
// best-effort values are fine for an unpaused brief). It walks the scope
// character's war-ref array (same pattern as do_query_wars) and emits a tiny
// per-war brief: war_id, name, primary attacker/defender, is_attacker, and —
// only if the WarScoreBreakdown cache has it — war_score + favored_side.
// =============================================================================
bool do_get_active_wars(const char* params_json, std::string* out) {
    safe_read_clear();

    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) {
            int cid = json_get_int(params_json, "character_id");
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "{\"success\":false,\"action\":\"get_active_wars\","
                     "\"reason\":\"character_not_found\",\"character_id\":%d}",
                     cid);
            *out = buf;
        }
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("action"); r.value_string("get_active_wars");
    r.key("character_id"); r.value_uint(my_id);
    r.key("wars"); r.begin_array();
    int war_count = 0;

    uintptr_t ruler_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (ruler_data) {
        uintptr_t war_array_base = ruler_data + ck3::title_realm::WAR_REF_ARRAY;
        uintptr_t war_ref_data = safe_read<uintptr_t>(war_array_base + 0x00);
        int32_t count = safe_read<int32_t>(war_array_base + 0x0C);
        if (count < 0 || count > 50) count = 0;

        uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);

        uint32_t seen_ids[64];
        int seen_count = 0;

        for (int32_t i = 0; i < count && war_ref_data; i++) {
            uint32_t war_ref = safe_read<uint32_t>(war_ref_data + i * 4);
            if (war_ref == 0 || war_ref == 0xFFFFFFFF) continue;

            uintptr_t war_ptr = 0;
            if (war_db) {
                uint32_t idx = war_ref & 0xFFFFFF;
                uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                if (idx < capacity) {
                    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                    if (table) {
                        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                        war_ptr = safe_read<uintptr_t>(entry);
                        if (war_ptr) {
                            uint32_t stored_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
                            if ((stored_ref & 0xFFFFFF) != idx) war_ptr = 0;
                        }
                    }
                }
            }
            if (!war_ptr || !is_mapped(war_ptr)) continue;

            uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
            if (is_ended != 0) continue;

            uint32_t war_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);

            bool duplicate = false;
            for (int s = 0; s < seen_count; s++) {
                if (seen_ids[s] == war_id) { duplicate = true; break; }
            }
            if (duplicate) continue;
            if (seen_count < 64) seen_ids[seen_count++] = war_id;

            r.begin_object();
            r.key("war_id"); r.value_uint(war_id);

            std::string war_name = ad_read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR);
            if (!war_name.empty()) {
                r.key("name"); r.value_string(war_name.c_str(), war_name.size());
            }

            uint32_t atk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
            uint32_t def = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
            r.key("attacker_id"); r.value_uint(atk);
            r.key("defender_id"); r.value_uint(def);

            // is_attacker: scan the attacker participant array for my_id (the
            // primary attacker may not be this character — could be a liege/
            // co-belligerent). Cheap, capped scan.
            bool is_attacker = false;
            {
                uintptr_t atk_container = war_ptr + ck3::war_obj::ATK_PARTICIPANT;
                uintptr_t atk_arr = safe_read<uintptr_t>(atk_container);
                int32_t atk_count = safe_read<int32_t>(atk_container + 0x0C);
                if (atk_count < 0 || atk_count > 100) atk_count = 0;
                if (atk_arr) {
                    for (int32_t j = 0; j < atk_count && j < 20; j++) {
                        uintptr_t part_ptr = safe_read<uintptr_t>(atk_arr + j * 8);
                        if (!part_ptr) continue;
                        uint32_t pid = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
                        if (pid == my_id) { is_attacker = true; break; }
                    }
                }
                // Fallback: primary attacker match if the array scan found nothing.
                if (!is_attacker && atk == my_id) is_attacker = true;
            }
            r.key("is_attacker"); r.value_bool(is_attacker);

            // war_score + favored_side ONLY if the breakdown cache has a FRESH
            // entry. A stale entry (hook refreshes rejected — ck-xwlg) is a
            // fossil, typically the all-zero day-one breakdown; flag it instead
            // of reporting it as truth (this compact path has no live estimate).
            WarScoreBreakdown wbd{};
            if (query_war_score_breakdown_fresh(war_id, &wbd, nullptr)) {
                r.key("war_score"); r.value_int(wbd.total);
                const char* favored_side = (wbd.total > 0) ? "attacker"
                                         : (wbd.total < 0) ? "defender" : "even";
                r.key("favored_side"); r.value_string(favored_side);
            } else if (query_war_score_breakdown(war_id, &wbd)) {
                r.key("war_score_stale"); r.value_bool(true);
            }

            r.end_object();
            war_count++;
        }
    }

    r.end_array();
    r.key("war_count"); r.value_int(war_count);
    if (safe_read_had_fault()) {
        r.key("degraded"); r.value_bool(true);
    }
    r.end_object();
    if (out) *out = r.take();
    return true;
}

bool do_query_wars(const char* params_json, std::string* out) {
    // No-args path (ck-fbij): enumerate the global WAR_DB when the caller
    // neither passed character_id nor set a controlled character. Previously
    // this returned success=false with no payload.
    safe_read_clear();
    int cid = json_get_int(params_json, "character_id");
    // L1b: lean-by-default — verbose adds full participant id arrays +
    // score_breakdown (neither consumed by the agent's war formatters).
    bool verbose = json_get_bool(params_json, "verbose", false);
    if (cid <= 0 && !get_controlled_character()) {
        return query_wars_all_active(out, verbose);
    }

    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_wars: character not found\n");
        // ck-09q4u.8: emit a structured failure so callers can distinguish
        // "actor not resolvable" from "actor has 0 wars". Previously the
        // handler returned false with no payload, the dispatch wrapper
        // synthesized {"success":false,"action":"query_wars"}, and callers
        // doing `resp.get("war_count", -1)` mistook the missing key for a
        // negative war count (sentinel leak).
        if (out) {
            const char* reason = (character && is_sentinel(character))
                ? "character_sentinel" : "character_not_found";
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "{\"success\":false,\"action\":\"query_wars\","
                     "\"reason\":\"%s\",\"character_id\":%d}",
                     reason, cid);
            *out = buf;
        }
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(my_id);

    // War list from ruler/title_realm sub-struct (R7-4 confirmed: char+0x1B8 -> +0x318)
    uintptr_t ruler_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);

    r.key("wars"); r.begin_array();
    int war_count = 0;

    if (ruler_data) {
        // CPdxArray<uint32> at ruler_data + 0x318: data_ptr at +0x00, count at +0x0C
        uintptr_t war_array_base = ruler_data + ck3::title_realm::WAR_REF_ARRAY;
        uintptr_t war_ref_data = safe_read<uintptr_t>(war_array_base + 0x00);
        int32_t count = safe_read<int32_t>(war_array_base + 0x0C);
        if (count < 0 || count > 50) count = 0;

        // War DB for resolving refs (TPdxRefDatabase<CWar,8>)
        uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);

        // Dedup: safety net in case of unexpected duplicates
        uint32_t seen_ids[256];
        int seen_count = 0;

        for (int32_t i = 0; i < count; i++) {
            // Each entry is a 4-byte uint32 war ref (not 8-byte pointer)
            uint32_t war_ref = safe_read<uint32_t>(war_ref_data + i * 4);
            if (war_ref == 0 || war_ref == 0xFFFFFFFF) continue;

            // Resolve war ref via TPdxRefDatabase
            uintptr_t war_ptr = 0;
            if (war_db) {
                uint32_t idx = war_ref & 0xFFFFFF;
                uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                if (idx < capacity) {
                    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                    if (table) {
                        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                        war_ptr = safe_read<uintptr_t>(entry);
                        // Verify war ref (masked 24-bit comparison)
                        if (war_ptr) {
                            uint32_t stored_ref = safe_read<uint32_t>(war_ptr + 0x08);
                            if ((stored_ref & 0xFFFFFF) != idx) war_ptr = 0;
                        }
                    }
                }
            }
            if (!war_ptr) continue;

            // Filter: skip ended wars (R7-4: war+0x2F8 == 0 means active)
            uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
            if (is_ended != 0) continue;

            // War ID
            uint32_t war_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);

            // Skip duplicates
            bool duplicate = false;
            for (int s = 0; s < seen_count; s++) {
                if (seen_ids[s] == war_id) { duplicate = true; break; }
            }
            if (duplicate) continue;
            if (seen_count < 256) seen_ids[seen_count++] = war_id;

            emit_war_entry(r, war_ptr, my_id,
                           /*emit_is_attacker=*/true, nullptr, verbose);
            war_count++;
        }
    }

    r.end_array();
    r.key("war_count"); r.value_int(war_count);

    if (safe_read_had_fault()) {
        r.key("degraded");    r.value_bool(true);
        r.key("fault_count"); r.value_int(safe_read_fault_count());
        log_degraded_read("query_wars");
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
        char addr_buf[24];
        snprintf(addr_buf, sizeof(addr_buf), "0x%lx", safe_read_last_fault());
        r.key("fault_addr");  r.value_string(addr_buf);
#endif
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_war_db_scan: Scan ALL wars in the global WAR_DB (TPdxRefDatabase<CWar,8>).
// Unlike query_wars (which reads a specific character's war ref array), this iterates
// the entire database to find every war object — including wars owned by opponents.
// Used by the war debug lab to detect "silent" war creation that bypasses the query layer.
// Input: { "include_ended": bool (default false) }
// Output: { "wars": [...], "total_slots": N, "active_count": N, "ended_count": N, "empty_count": N }
bool do_query_war_db_scan(const char* params_json, std::string* out) {
    if (!out) return false;

    bool include_ended = json_get_bool(params_json, "include_ended", false);

    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (!war_db) {
        *out = R"({"success":false,"error":"WAR_DB global is null"})";
        return false;
    }

    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
    if (!table || capacity == 0 || capacity > 100000) {
        *out = R"({"success":false,"error":"WAR_DB table invalid or capacity out of range"})";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("db_capacity"); r.value_uint(capacity);

    r.key("wars"); r.begin_array();

    int active_count = 0;
    int ended_count = 0;
    int empty_count = 0;

    for (uint32_t idx = 0; idx < capacity && idx < 10000; idx++) {
        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
        uintptr_t war_ptr = safe_read<uintptr_t>(entry);
        if (!war_ptr || (!war_ptr || !is_mapped(war_ptr))) {
            empty_count++;
            continue;
        }

        // Verify the slot is occupied: stored ref's 24-bit index should match slot
        uint32_t stored_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
        if ((stored_ref & 0xFFFFFF) != idx) {
            empty_count++;
            continue;
        }

        uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
        if (is_ended != 0) {
            ended_count++;
            if (!include_ended) continue;
        } else {
            active_count++;
        }

        r.begin_object();

        r.key("war_id"); r.value_uint(stored_ref);
        r.key("slot_index"); r.value_uint(idx);
        r.key("is_ended"); r.value_bool(is_ended != 0);

        // War name
        std::string war_name = ad_read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR);
        if (!war_name.empty()) {
            r.key("name"); r.value_string(war_name.c_str(), war_name.size());
        }

        // CB type
        uintptr_t cb_type_ref = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TYPE_REF);
        if (cb_type_ref) {
            std::string cb_name = ad_read_std_string(cb_type_ref + ck3::db_object::NAME_KEY);
            if (!cb_name.empty()) {
                r.key("cb_type"); r.value_string(cb_name.c_str(), cb_name.size());
            }
        }

        // Primary attacker/defender
        uint32_t primary_atk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
        uint32_t primary_def = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
        r.key("primary_attacker"); r.value_uint(primary_atk);
        r.key("primary_defender"); r.value_uint(primary_def);

        // Target titles
        uintptr_t cb_titles_ptr = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TITLE_ARRAY);
        int32_t cb_title_count = safe_read<int32_t>(war_ptr + ck3::war_obj::CB_TITLE_COUNT);
        if (cb_title_count < 0 || cb_title_count > 20) cb_title_count = 0;
        r.key("target_titles"); r.begin_array();
        if (cb_titles_ptr) {
            for (int32_t t = 0; t < cb_title_count; t++) {
                uint64_t title_ref = safe_read<uint64_t>(cb_titles_ptr + t * 8);
                r.value_uint(static_cast<uint32_t>(title_ref & 0xFFFFFF));
            }
        }
        r.end_array();

        r.end_object();
    }

    r.end_array();
    r.key("active_count"); r.value_int(active_count);
    r.key("ended_count"); r.value_int(ended_count);
    r.key("empty_count"); r.value_int(empty_count);

    r.end_object();
    *out = r.take();
    return true;
}

// Resolve a CWar* from WAR_DB by war id. Accepts either a gen-tagged war_id
// (high 8 bits = generation, e.g. 0x0B000002) OR a bare 24-bit slot index —
// only the low 24 bits select the slot, and the stored war_id is verified by
// its own low-24 bits, so a caller that passes the gen-tagged ref OR the bare
// index both resolve the same active war. Returns 0 if no live war occupies the
// slot. Pause-safe (pure safe_read over the hash map; no engine reentry).
static uintptr_t resolve_war_by_id(uint32_t war_id) {
    uint32_t idx = war_id & ck3::war_db::WAR_ID_IDX_MASK;
    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (!war_db || !is_mapped(war_db)) return 0;
    uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
    if (idx >= capacity) return 0;
    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
    if (!table || !is_mapped(table)) return 0;
    uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                      + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
    uintptr_t war_ptr = safe_read<uintptr_t>(entry);
    if (!war_ptr || !is_mapped(war_ptr)) return 0;
    // Verify the slot actually holds the war we asked for (24-bit ref match).
    if ((safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID) & 0xFFFFFF) != idx) return 0;
    return war_ptr;
}

// query_war_score_breakdown: detailed per-component war score breakdown.
//
// #120: the bare cache read returned `{found:false}` (or cache-only numbers)
// for any war the WAR_SCORE_UPDATE post-hook had not yet processed, so the
// agent saw a misleading bare 0 with no way to tell "winning, occupation will
// tick" from "siege-stalled, intervene". This now ALWAYS resolves the live
// CWar (when the id maps to one) and surfaces the direct-read battle_score +
// ticking_score + favored_side regardless of cache state, then LAYERS the
// engine-computed components (occupation / imprisonment / landless / clamped
// total) when the cache has them. The component split lets the agent diagnose
// the live East Anglia case: high battle_score + occupation 0 = battles won but
// war-goal counties not yet sieged (the score will not climb until occupation
// ticks), as opposed to a wrong-offset 0.
//
// Input: { "war_id": <uint32, gen-tagged ref OR bare slot idx> } for a specific
// war, or {} for all cached wars.
bool do_query_war_score_breakdown(const char* params_json, std::string* out) {
    ResponseBuilder r;
    r.begin_object();

    // Parse optional war_id
    uint32_t war_id = static_cast<uint32_t>(json_get_int(params_json, "war_id"));

    auto emit_breakdown = [&](uint32_t wid, const WarScoreBreakdown& bd) {
        r.begin_object();
        r.key("war_id"); r.value_uint(wid);
        r.key("total"); r.value_int(bd.total);
        r.key("cached_tick"); r.value_uint(static_cast<uint32_t>(bd.cached_tick));

        // Per-side breakdown (raw fixed-point / 1000 → percentage points)
        r.key("imprisonment_atk"); r.value_int(bd.imprisonment_atk / 1000);
        r.key("imprisonment_def"); r.value_int(bd.imprisonment_def / 1000);
        r.key("battle_atk"); r.value_int(bd.battle_atk / 1000);
        r.key("battle_def"); r.value_int(bd.battle_def / 1000);
        r.key("occupation_atk"); r.value_int(bd.occupation_atk / 1000);
        r.key("occupation_def"); r.value_int(bd.occupation_def / 1000);
        r.key("ticking_atk"); r.value_int(bd.ticking_atk / 1000);
        r.key("ticking_def"); r.value_int(bd.ticking_def / 1000);
        r.key("landless_atk"); r.value_int(bd.landless_atk / 1000);
        r.key("landless_def"); r.value_int(bd.landless_def / 1000);

        // Aggregated components (net attacker - defender)
        r.key("imprisonment"); r.value_int((bd.imprisonment_atk + bd.imprisonment_def) / 1000);
        r.key("battle"); r.value_int((bd.battle_atk + bd.battle_def) / 1000);
        r.key("occupation"); r.value_int((bd.occupation_atk + bd.occupation_def) / 1000);
        r.key("ticking"); r.value_int((bd.ticking_atk + bd.ticking_def) / 1000);
        r.key("landless"); r.value_int((bd.landless_atk + bd.landless_def) / 1000);

        r.end_object();
    };

    if (war_id != 0) {
        // Single war query.
        //
        // STEP 1 — live struct reads (always, independent of the cache). These
        // are pure process_vm_readv over the CWar and cannot SIGSEGV; they give
        // the agent the real won-battle + ticking momentum even before the
        // engine post-hook has populated the breakdown cache for this war.
        uintptr_t war_ptr = resolve_war_by_id(war_id);
        int live_battle = 0, live_battle_count = 0, live_ticking = 0;
        bool live = false;
        if (war_ptr) {
            uint8_t ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
            if (ended == 0) {
                live = true;
                int bc = 0;
                live_battle = static_cast<int>(compute_battle_score(war_ptr, &bc));
                live_battle_count = bc;
                live_ticking = compute_ticking_score(war_ptr);
            }
        }

        // STEP 2 — cached engine breakdown (occupation / imprisonment / landless
        // / clamped total). Present only after the WAR_SCORE_UPDATE post-hook
        // has run for this war.
        WarScoreBreakdown bd{};
        WarScoreRejectReason wrej = WarScoreRejectReason::None;
        bool fresh = query_war_score_breakdown_fresh(war_id, &bd, &wrej);
        // A stale entry (hook refreshes rejected — ck-xwlg) no longer counts as
        // engine truth for total/favored_side, but its components are still
        // shown below as the last-known values.
        bool cached_raw = fresh || query_war_score_breakdown(war_id, &bd);
        bool cached = fresh;

        r.key("found"); r.value_bool(live || cached);
        r.key("war_id"); r.value_uint(war_id);
        r.key("live"); r.value_bool(live);

        if (live || cached) {
            // Live momentum (attacker perspective, attacker-minus-defender).
            // battle/ticking come from the live struct when resolvable, else
            // fall back to the cached components so the field is never absent.
            int battle = live ? live_battle : (bd.battle_atk + bd.battle_def) / 1000;
            int ticking = live ? live_ticking : (bd.ticking_atk + bd.ticking_def) / 1000;
            int occupation = (bd.occupation_atk + bd.occupation_def) / 1000;
            int imprisonment = (bd.imprisonment_atk + bd.imprisonment_def) / 1000;
            int landless = (bd.landless_atk + bd.landless_def) / 1000;

            // total_warscore: the engine's clamped sum when cached; otherwise a
            // clamped battle+ticking estimate so the agent always gets a real
            // -100..100 figure instead of a bare misleading 0.
            int total;
            const char* total_source;
            if (cached) {
                total = bd.total;
                total_source = "engine_cached";
            } else {
                total = battle + ticking;
                if (total > 100) total = 100; else if (total < -100) total = -100;
                total_source = "live_estimate";
            }

            r.key("total_warscore"); r.value_int(total);
            r.key("total_warscore_source"); r.value_string(total_source, strlen(total_source));
            // cache_status lets the agent distinguish "occupation not yet
            // computed" (live_only) from a real engine-confirmed occupation 0
            // (cached). With cached + battle>0 + occupation 0, the war is
            // battle-won but the war-goal counties are not sieged → siege-stall.
            const char* cst = cached ? "cached" : cached_raw ? "stale" : "live_only";
            r.key("cache_status"); r.value_string(cst, static_cast<int>(strlen(cst)));
            if (!cached && cached_raw) {
                const char* rn = war_score_reject_name(wrej);
                r.key("score_hook_reject");
                r.value_string(rn, static_cast<int>(strlen(rn)));
            }

            // Components (attacker perspective). occupation/imprisonment/
            // landless are 0 (and cache_status=live_only) until the post-hook
            // runs; battle/ticking are always live.
            r.key("battle_score"); r.value_int(battle);
            r.key("battle_count"); r.value_int(live_battle_count);
            r.key("ticking_warscore"); r.value_int(ticking);
            r.key("occupation_score"); r.value_int(occupation);
            r.key("imprisonment_score"); r.value_int(imprisonment);
            r.key("landless_score"); r.value_int(landless);

            // favored_side from the best available net score.
            int net = cached ? bd.total : (battle + ticking);
            const char* fav = (net > 0) ? "attacker" : (net < 0) ? "defender" : "even";
            r.key("favored_side"); r.value_string(fav, strlen(fav));

            // Full cached per-side breakdown sub-object when available (keeps
            // the prior consumers of `breakdown.*` working; stale entries are
            // still emitted — cache_status above says how to read them).
            if (cached_raw) {
                r.key("breakdown");
                emit_breakdown(war_id, bd);
            }
        }
    } else {
        // All cached wars
        std::unordered_map<uint32_t, WarScoreBreakdown> all;
        query_all_war_score_breakdowns(&all);
        r.key("count"); r.value_int(static_cast<int>(all.size()));
        r.key("breakdowns"); r.begin_array();
        for (const auto& [wid, bd] : all) {
            emit_breakdown(wid, bd);
        }
        r.end_array();
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_war_stance — READ-ONLY AI war-stance / siege-intent visibility
// (re_query_war_stance.md, limitations 5–7).
//
// Per active war the queried character ATTACKS in, surface the AI's chosen war
// stance (offensive=will-siege vs defensive=never-sieges), layer the already-
// cached occupation_score, and derive a will_siege/stuck verdict so the agent
// abandons a doomed (defensive / occupation-0) war EARLY (white-peace) instead
// of bleeding prestige.
//
// Pure memory reads — NO engine call, NO game-thread reentry, NO write into AI
// state. Two layers:
//   (always-on)  the stance-DB 7-slot reverse map (core-dump-confirmed, process-
//                stable) + the war-score-cache occupation read. With the record
//                walk OFF, stance is reported as "unavailable" and the verdict is
//                derived from occupation alone (conservative).
//   (flag-gated) g_war_stance_record_walk_enabled (DEFAULT OFF): walk the AI-mgr
//                coordinator record array (ai_root+0x48 / count +0x54), read each
//                record's LIVE stance ptr (rec+0x70) + attacker bit (rec+0x68&4),
//                reverse-map the ptr to a stance name, and cross-validate the
//                record↔war match key (rec+0x18) against WAR_DB exactly as
//                FUN_04e60920 does (war+8 == rec+0x18). The match key is decompile-
//                confirmed but not yet live-probed on a known-at-war record
//                (re_query_war_stance.md §6) — hence OFF by default. A wrong key
//                degrades to "fewer wars matched", never to garbage or a crash.
//
// Input: { "character_id": <uint32> } (the attacker whose wars to inspect),
//        optional { "war_id": <uint32> } to scope to a single war.
// Pause: plain read ⇒ PAUSED-safe (no engine probe, unlike query_cbs_for_target).
// =============================================================================

// The 7 engine-singleton SAIWarStance slots inside the stance-DB instance, and
// whether each stance means the AI will march to SIEGE the war goal.
namespace {
struct StanceSlot { size_t off; const char* name; bool offensive; };
constexpr StanceSlot kStanceSlots[] = {
    { ck3::war_stance::OFF_ATK_OFFENSIVE, "attacker_offensive",       true  },
    { ck3::war_stance::OFF_ATK_DEFENSIVE, "attacker_defensive",       false },
    { ck3::war_stance::OFF_DEF_OFFENSIVE, "defender_offensive",       true  },
    { ck3::war_stance::OFF_DEF_DEFENSIVE, "defender_defensive",       false },
    { ck3::war_stance::OFF_DEF_DESPERATE, "defender_desperate",       false },
    { ck3::war_stance::OFF_GHW_ATTACKER,  "great_holy_war_attacker",  true  },
    { ck3::war_stance::OFF_GHW_DEFENDER,  "great_holy_war_defender",  false },
};
}  // namespace

bool do_query_war_stance(const char* params_json, std::string* out) {
    int actor_id = json_get_int(params_json, "character_id");
    if (actor_id <= 0) actor_id = json_get_int(params_json, "attacker_id");
    if (actor_id <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id <= 0) {
        if (out) *out = R"json({"success":false,"error":"no_actor_available","hint":"Provide character_id"})json";
        return false;
    }
    uint32_t my_id = static_cast<uint32_t>(actor_id) & 0xFFFFFF;
    uint32_t want_war = static_cast<uint32_t>(json_get_int(params_json, "war_id"));
    const bool walk_enabled =
        g_war_stance_record_walk_enabled.load(std::memory_order_relaxed);

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("action"); r.value_string("query_war_stance");
    r.key("character_id"); r.value_uint(my_id);

    // ---- Layer 1: stance-DB 7-slot reverse map (always; core-confirmed) -------
    // Build {stance_ptr → index into kStanceSlots} once. Process-stable for the
    // session (constructed once by STANCE_DB_CTOR). Pure safe_read, fail-soft.
    uintptr_t stance_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::STANCE_DB_GLOBAL);
    std::map<uintptr_t, int> stance_rev;   // ptr → kStanceSlots index
    bool stance_db_ok = false;
    if (stance_db && is_mapped(stance_db)) {
        stance_db_ok = true;
        for (int i = 0; i < (int)(sizeof(kStanceSlots)/sizeof(kStanceSlots[0])); ++i) {
            uintptr_t sp = safe_read<uintptr_t>(stance_db + kStanceSlots[i].off);
            if (sp && is_mapped(sp)) stance_rev[sp] = i;
        }
    }
    r.key("stance_db_resolved"); r.value_bool(stance_db_ok && stance_rev.size() == 7);
    r.key("record_walk_enabled"); r.value_bool(walk_enabled);

    // ---- Per-char AI controller state (ck-xwlg P2 instrumentation) ------------
    // ai_ctrl+0x28 gates the ENTIRE per-char AI tick (CAIManager_MainTick) —
    // when 0 the war coordinator never re-plans and armies park (the old
    // M-003 class). Surfacing it here makes "is the char's AI even running"
    // a one-query check at any park event. is_suspended (+0x2A) forces
    // IsEligible false; flags bit1 = is_ai_active.
    if (g_get_ai_ctrl) {
        void* actor_ctrl = lookup_character(my_id);
        void* actor_ai = (actor_ctrl && !is_sentinel(actor_ctrl))
                             ? g_get_ai_ctrl(actor_ctrl) : nullptr;
        if (actor_ai) {
            uintptr_t ap = reinterpret_cast<uintptr_t>(actor_ai);
            r.key("ai_enabled");
            r.value_bool(safe_read<uint8_t>(ap + ck3::ai_ctrl::AI_ENABLED) != 0);
            r.key("ai_suspended");
            r.value_bool(safe_read<uint8_t>(ap + ck3::ai_ctrl::IS_SUSPENDED) != 0);
            r.key("ai_active_flag");
            r.value_bool((safe_read<uint8_t>(ap + ck3::ai_ctrl::FLAGS) & 0x02) != 0);
        } else {
            r.key("ai_ctrl_resolved"); r.value_bool(false);
        }
    }

    // Reverse-map a record's stance ptr → (name, offensive). Returns -1 if the
    // ptr matches none of the 7 known slots (treat as not-offensive / "unknown").
    auto map_stance = [&](uintptr_t sp) -> int {
        auto it = stance_rev.find(sp);
        return (it == stance_rev.end()) ? -1 : it->second;
    };

    // ---- Layer 2 (flag-gated): AI-mgr coordinator record walk -----------------
    // Build {war_id → (stance_idx, is_attacker)} for the queried realm's records.
    // Only when the runtime flag is armed AND the live ai_manager is captured.
    std::map<uint32_t, std::pair<int,bool>> rec_stance;   // war_id → (slot idx, atk)
    int records_scanned = 0, records_matched = 0, records_unresolved = 0;
    bool record_walk_ran = false;
    if (walk_enabled && stance_db_ok) {
        void* mgr = get_ai_manager();
        uintptr_t ai_root = reinterpret_cast<uintptr_t>(mgr);
        if (ai_root && is_mapped(ai_root)) {
            uint32_t n = safe_read<uint32_t>(ai_root + ck3::war_stance::AIMGR_REC_COUNT);
            uintptr_t arr = safe_read<uintptr_t>(ai_root + ck3::war_stance::AIMGR_REC_ARRAY);
            if (n > 0 && n < 65536 && arr && is_mapped(arr)) {
                record_walk_ran = true;
                for (uint32_t i = 0; i < n && i < 4096; ++i) {
                    uintptr_t rec = safe_read<uintptr_t>(arr + (uintptr_t)i * 8);
                    if (!rec || !is_mapped(rec)) continue;
                    if (safe_read<uint8_t>(rec + ck3::war_stance::REC_ACTIVE_FLAG) == 0) continue;
                    records_scanned++;
                    uint32_t rec_war_id = safe_read<uint32_t>(rec + ck3::war_stance::REC_WAR_ID);
                    // Cross-validate the match key against WAR_DB (FUN_04e60920
                    // invariant: war+8 == rec+0x18). A failing key is skipped, so
                    // a wrong +0x18 assumption degrades to fewer matches, never to
                    // garbage. resolve_war_by_id already re-checks war+8 low-24.
                    uintptr_t war_ptr = resolve_war_by_id(rec_war_id);
                    if (!war_ptr ||
                        safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID) != rec_war_id) {
                        records_unresolved++;
                        continue;
                    }
                    bool is_atk = (safe_read<uint16_t>(rec + ck3::war_stance::REC_FLAGS) & 0x4) != 0;
                    uintptr_t sp = safe_read<uintptr_t>(rec + ck3::war_stance::REC_STANCE_PTR);
                    int idx = map_stance(sp);
                    rec_stance[rec_war_id & 0xFFFFFF] = { idx, is_atk };
                    records_matched++;
                }
            }
        }
    }
    r.key("record_walk_ran"); r.value_bool(record_walk_ran);
    if (record_walk_ran) {
        r.key("records_scanned");    r.value_int(records_scanned);
        r.key("records_matched");    r.value_int(records_matched);
        r.key("records_unresolved"); r.value_int(records_unresolved);
    }

    // ---- Emit per active attacker-war for the queried character ---------------
    // Iterate WAR_DB (same pattern as query_call_to_arms); keep wars where my_id
    // is the primary attacker (the side that decides to siege). Layer stance (from
    // the record walk when available) + occupation (from the cache) → verdict.
    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    uintptr_t table = war_db ? safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR) : 0;
    uint32_t capacity = war_db ? safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY) : 0;

    r.key("wars"); r.begin_array();
    int war_count = 0;
    if (table && capacity > 0 && capacity < 100000) {
        for (uint32_t idx = 0; idx < capacity && idx < 10000; idx++) {
            uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                              + (uintptr_t)idx * ck3::hash_map::ENTRY_STRIDE;
            uintptr_t war_ptr = safe_read<uintptr_t>(entry);
            if (!war_ptr || !is_mapped(war_ptr)) continue;
            uint32_t stored = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
            if ((stored & 0xFFFFFF) != idx) continue;
            if (safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED) != 0) continue;
            if (want_war != 0 && (want_war & 0xFFFFFF) != idx) continue;

            uint32_t patk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
            uint32_t pdef = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
            // Only the primary attacker decides the siege; that is who "stuck"
            // applies to. (A called ally cannot white-peace the war.)
            if (my_id != patk) continue;

            war_count++;

            // Occupation from the already-shipping war-score cache (no engine call).
            WarScoreBreakdown bd{};
            bool cached = query_war_score_breakdown(stored, &bd);
            int occupation = cached ? (bd.occupation_atk + bd.occupation_def) / 1000 : 0;
            int ticking    = cached ? (bd.ticking_atk + bd.ticking_def) / 1000 : 0;

            // Stance from the record walk if it matched this war; else unavailable.
            const char* stance = "unavailable";
            bool stance_known = false, will_siege_stance = false, is_attacker = true;
            auto rit = rec_stance.find(idx);
            if (rit != rec_stance.end()) {
                int sidx = rit->second.first;
                is_attacker = rit->second.second;
                if (sidx >= 0) {
                    stance = kStanceSlots[sidx].name;
                    will_siege_stance = kStanceSlots[sidx].offensive;
                    stance_known = true;
                } else {
                    stance = "unknown";   // ptr matched no slot — treat not-offensive
                }
            }

            r.begin_object();
            r.key("war_id"); r.value_uint(idx);
            std::string wname = ad_read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR);
            if (!wname.empty()) { r.key("war_name"); r.value_string(wname.c_str()); }
            r.key("primary_attacker"); r.value_uint(patk);
            r.key("primary_defender"); r.value_uint(pdef);
            r.key("is_attacker"); r.value_bool(is_attacker);
            r.key("stance"); r.value_string(stance);
            r.key("stance_known"); r.value_bool(stance_known);
            r.key("occupation_score"); r.value_int(occupation);
            r.key("ticking_warscore"); r.value_int(ticking);
            r.key("occupation_source"); r.value_string(cached ? "cached" : "uncached");

            // Verdict. When the stance is KNOWN (record walk armed + matched),
            // will_siege follows the stance directly. When UNAVAILABLE, fall back
            // to occupation: occupation>0 ⇒ prosecuting (will_siege implied);
            // occupation==0 ⇒ ambiguous (cannot tell "not sieging yet" from
            // "defensive will-never-siege") → conservative "indeterminate".
            bool will_siege;
            bool stuck;
            const char* advice;
            if (stance_known) {
                will_siege = will_siege_stance;
                stuck = is_attacker && !will_siege && occupation == 0;
                if (!is_attacker)                 advice = "defender";
                else if (stuck)                   advice = "bail_white_peace";
                else if (will_siege && occupation == 0) advice = "wait_mobilize";
                else                              advice = "prosecuting";
            } else {
                // Occupation-only fallback (record walk OFF or unmatched).
                will_siege = occupation > 0;
                stuck = false;   // never assert stuck without the stance signal
                advice = (occupation > 0) ? "prosecuting" : "indeterminate";
            }
            r.key("will_siege"); r.value_bool(will_siege);
            r.key("stuck"); r.value_bool(stuck);
            r.key("advice"); r.value_string(advice);
            if (!stance_known) {
                r.key("note"); r.value_string(
                    walk_enabled
                    ? "stance not matched to a coordinator record this query; verdict from occupation only"
                    : "record walk disabled (g_war_stance_record_walk_enabled=false); "
                      "verdict from occupation only — arm the flag for true stance");
            }
            r.end_object();
        }
    }
    r.end_array();
    r.key("war_count"); r.value_int(war_count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_council: council positions for a ruler
//
// PRIMARY PATH (v4 FIX): Direct DB scan of COUNCIL_DB (DAT_0bfcef08 / globals::COUNCIL_TYPE_DB).
// The DB is keyed by RULER CHARACTER ID at entry+0x08.
// Each council_type_obj has COUNCILLOR_IDS at +0x520 (sorted uint32[] of char IDs)
// and COUNCILLOR_COUNT at +0x52C.
//
// ROOT CAUSE OF PREVIOUS FAILURES:
// 1. RULER_CHAR_ID was at +0x3A0 — WRONG, that's an allocator/sentinel ptr (0xbf9efc0 in ctor).
//    The actual ruler char ID is at +0x08. Fixed in ck3_offsets.h.
// 2. council_instance at *(alive_data+0x4E8) is often NULL (lazy init), returning static default
//    with type_id=0xFFFFFFFF. The DB scan bypasses this entirely.
// 3. GetCouncilConfig (alive_data+0x4F0) config+0x40/+0x4C is also lazily populated and empty.
//
// FALLBACK: council_instance probe, then GET_COUNCIL_CONFIG, then inline struct.
//
// POSITION NAME PATH: If we find the council_type_obj, also try the position
// tracker at +0x1B8 for position assignment entries with names.

// Helper: emit a single councillor entry into the response. Returns true if emitted.
static bool emit_councillor(ResponseBuilder& r, uint32_t char_id, const char* position_name) {
    void* holder = lookup_character(char_id);
    if (!holder || is_sentinel(holder)) return false;
    uintptr_t h = reinterpret_cast<uintptr_t>(holder);

    r.begin_object();
    r.key("position"); r.value_string(position_name);
    r.key("holder_id"); r.value_uint(char_id & 0xFFFFFF);
    emit_char_names(r, h);

    r.key("skills"); r.begin_object();
    r.key("diplomacy");   r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_DIPLOMACY));
    r.key("martial");     r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_MARTIAL));
    r.key("stewardship"); r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_STEWARDSHIP));
    r.key("intrigue");    r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_INTRIGUE));
    r.key("learning");    r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_LEARNING));
    r.key("prowess");     r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_PROWESS));
    r.end_object();

    r.end_object();
    return true;
}

// Helper: try to find the council_type_obj by probing pointers on the council instance.
// council_type_obj is validated by checking RULER_CHAR_ID at +0x08 matches expected ruler.
// Also tries reading the instance itself as the council_type_obj (it might BE the type obj).
// NOTE: RULER_CHAR_ID was moved from +0x3A0 (WRONG: was allocator ptr) to +0x08 (CORRECT).
static uintptr_t find_council_type_obj(uintptr_t council_instance, uint32_t ruler_id) {
    if ((!council_instance || !is_mapped(council_instance))) return 0;

    // Probe: the instance itself might be the council_type_obj
    // Check if RULER_CHAR_ID at +0x08 matches and COUNCILLOR_COUNT at +0x52C is sane
    {
        uint32_t ruler_at = safe_read<uint32_t>(council_instance + ck3::council_type_obj::RULER_CHAR_ID);
        int32_t count_at = safe_read<int32_t>(council_instance + ck3::council_type_obj::COUNCILLOR_COUNT);
        if ((ruler_at & 0xFFFFFF) == ruler_id && count_at >= 0 && count_at <= 20) {
            return council_instance;
        }
    }

    // Probe pointer fields on the instance for a council_type_obj pointer
    static const size_t probe_offsets[] = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30 };
    for (size_t off : probe_offsets) {
        uintptr_t candidate = safe_read<uintptr_t>(council_instance + off);
        if ((!candidate || !is_mapped(candidate)) || candidate == council_instance) continue;
        uint32_t ruler_at = safe_read<uint32_t>(candidate + ck3::council_type_obj::RULER_CHAR_ID);
        int32_t count_at = safe_read<int32_t>(candidate + ck3::council_type_obj::COUNCILLOR_COUNT);
        if ((ruler_at & 0xFFFFFF) == ruler_id && count_at >= 0 && count_at <= 20) {
            return candidate;
        }
    }

    return 0;
}

// Helper: scan COUNCIL_DB for an entry whose RULER_CHAR_ID (+0x08) matches ruler_id.
// This bypasses the broken council_instance path entirely.
// CRITICAL FIX: The DB key IS the ruler char ID at entry+0x08 (NOT +0x3A0 which is an allocator ptr).
// DB format: TPdxRefDatabase hash table. Global ptr at db_global -> db.
//   db+0x20 = table base ptr, db+0x2C = capacity (uint32).
//   Each slot is 0x10 bytes; entry ptr at slot+0x08.
//   Entry match key at entry+0x08 = ruler char ID.
// Validation: entry+0x08 = ruler char ID, entry+0x52C = councillor count (0..20).
static uintptr_t scan_council_type_db_for_ruler(uint32_t ruler_id) {
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::COUNCIL_TYPE_DB);
    if (!db) {
        if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: DB ptr is null (global=0x%lx)\n", (unsigned long)ck3::globals::COUNCIL_TYPE_DB); fflush(g_logfile); }
        return 0;
    }
    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if ((!table || !is_mapped(table))) {
        if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: table ptr invalid (db=%p, table=%p)\n", (void*)db, (void*)table); fflush(g_logfile); }
        return 0;
    }
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (capacity == 0 || capacity > 100000) {
        if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: bad capacity=%u\n", capacity); fflush(g_logfile); }
        return 0;
    }

    if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: db=%p, table=%p, capacity=%u, looking for ruler=%u\n", (void*)db, (void*)table, capacity, ruler_id); fflush(g_logfile); }

    uint32_t masked = ruler_id & 0xFFFFFF;

    // Fast path 1: direct index lookup (works if capacity > masked, i.e. DB indexed by raw char ID)
    if (masked < capacity) {
        uintptr_t entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(masked) * ck3::hash_map::ENTRY_STRIDE);
        if ((entry && is_mapped(entry))) {
            uint32_t entry_ruler = safe_read<uint32_t>(entry + ck3::council_type_obj::RULER_CHAR_ID);
            if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: fast1 slot=%u entry=%p ruler=0x%x\n", masked, (void*)entry, entry_ruler); fflush(g_logfile); }
            if ((entry_ruler & 0xFFFFFF) == masked) {
                return entry;
            }
        }
    }

    // Fast path 2: hash slot lookup (capacity is power-of-2, slot = masked % capacity)
    // This handles cases where masked >= capacity (e.g. Matilda=36010, capacity=2048)
    if (capacity > 0 && (capacity & (capacity - 1)) == 0) {
        // Power of 2: use bitmask
        uint32_t slot = masked & (capacity - 1);
        uintptr_t entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(slot) * ck3::hash_map::ENTRY_STRIDE);
        if ((entry && is_mapped(entry))) {
            uint32_t entry_ruler = safe_read<uint32_t>(entry + ck3::council_type_obj::RULER_CHAR_ID);
            if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: fast2 hash_slot=%u entry=%p ruler=0x%x\n", slot, (void*)entry, entry_ruler); fflush(g_logfile); }
            if ((entry_ruler & 0xFFFFFF) == masked) {
                return entry;
            }
        }
        // Try a few neighboring slots for collision resolution (linear probing)
        for (uint32_t probe = 1; probe <= 8; probe++) {
            uint32_t pslot = (slot + probe) & (capacity - 1);
            entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                         + static_cast<uintptr_t>(pslot) * ck3::hash_map::ENTRY_STRIDE);
            if ((!entry || !is_mapped(entry))) break; // empty slot = end of chain
            uint32_t entry_ruler = safe_read<uint32_t>(entry + ck3::council_type_obj::RULER_CHAR_ID);
            if ((entry_ruler & 0xFFFFFF) == masked) {
                if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: fast2 probe=%u slot=%u found\n", probe, pslot); fflush(g_logfile); }
                return entry;
            }
        }
    } else if (capacity > 0) {
        // Non-power-of-2: use modulo
        uint32_t slot = masked % capacity;
        uintptr_t entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(slot) * ck3::hash_map::ENTRY_STRIDE);
        if ((entry && is_mapped(entry))) {
            uint32_t entry_ruler = safe_read<uint32_t>(entry + ck3::council_type_obj::RULER_CHAR_ID);
            if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] council_scan: fast2 mod_slot=%u entry=%p ruler=0x%x\n", slot, (void*)entry, entry_ruler); fflush(g_logfile); }
            if ((entry_ruler & 0xFFFFFF) == masked) {
                return entry;
            }
        }
    }

    // Also try with the full char ref (including generation byte) for hash computation
    // The full ref is at char+0x18, which char_get_id() returns without masking
    // We receive the already-masked ID, so we can't recover the generation byte here.
    // The linear scan below handles this case.

    // Slow path: linear scan all slots
    int logged = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        uintptr_t entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
        if ((!entry || !is_mapped(entry))) continue;
        uint32_t entry_ruler = safe_read<uint32_t>(entry + ck3::council_type_obj::RULER_CHAR_ID);
        if (logged < 5 && g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge] council_scan: slot[%u] entry=%p ruler=0x%x\n", i, (void*)entry, entry_ruler);
            fflush(g_logfile);
            logged++;
        }
        if ((entry_ruler & 0xFFFFFF) == masked) {
            return entry;
        }
    }
    return 0;
}

bool do_query_council(const char* params_json, std::string* out) {
    // L1b: lean-by-default — the 6-field per-holder skills object is unused by
    // digestor.py (it reads only a single aggregate skill scalar). In lean mode
    // emit one representative skill int; verbose restores the full object.
    bool verbose = json_get_bool(params_json, "verbose", false);
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_council: character not found\n");
        return false;
    }

    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(my_id);

    r.key("council"); r.begin_array();

    int emitted = 0;
    const char* method = "none";

    // Council vacancies accumulated during the primary instance walk (TASK B).
    std::vector<std::string> vacant_positions;

    static int diag_log_count = 0;
    bool do_log = (g_logfile && diag_log_count < 10);

    // =========================================================================
    // PRIMARY PATH (audit B1, 20260612): walk the council INSTANCE directly.
    // The DB-scan paths below regressed to method="none" for every ruler. This
    // mirrors do_set_council_task's proven walk and additionally reads each
    // position's HOLDER and CURRENT TASK:
    //   council = *(char + 0x1b8); ids @ council+0x230, count @ council+0x23c;
    //   each pid resolved via COUNCIL_POSITION_DB (deref, +0x20 = bucket array,
    //   entry = *(arr + 8 + (pid&0xffffff)*0x10)); pos_obj+0x30 = holder char id
    //   (0xFFFFFF = vacant), pos_obj+0x18 = current task obj, task name @ +0x10.
    // =========================================================================
    {
        uintptr_t c = reinterpret_cast<uintptr_t>(character);
        uintptr_t council = safe_read<uintptr_t>(c + ck3::globals::CHAR_COUNCIL_INSTANCE);
        uintptr_t posdb   = safe_read_offset<uintptr_t>(ck3::globals::COUNCIL_POSITION_DB);
        if (council && is_mapped(council) && posdb && is_mapped(posdb)) {
            uintptr_t id_arr  = safe_read<uintptr_t>(council + ck3::globals::COUNCIL_POS_ID_ARRAY);
            uint32_t  id_cnt  = safe_read<uint32_t>(council + ck3::globals::COUNCIL_POS_ID_COUNT);
            uintptr_t pdb_arr = safe_read<uintptr_t>(posdb + ck3::hash_map::TABLE_PTR);
            if (id_arr && is_mapped(id_arr) && pdb_arr && is_mapped(pdb_arr)
                    && id_cnt > 0 && id_cnt < 32) {
                for (uint32_t i = 0; i < id_cnt; i++) {
                    uint32_t pid = safe_read<uint32_t>(id_arr + i * 4);
                    uint32_t idx = pid & 0xFFFFFF;
                    uintptr_t pos_obj = safe_read<uintptr_t>(
                        pdb_arr + ck3::hash_map::ENTRY_HEADER_SIZE
                        + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
                    if (!pos_obj || !is_mapped(pos_obj)) continue;
                    if ((safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_KEY) & 0xFFFFFF) != idx)
                        continue;

                    std::string task_name;
                    uintptr_t cur_task = safe_read<uintptr_t>(pos_obj + ck3::globals::COUNCIL_POS_CURRENT_TASK);
                    if (cur_task && is_mapped(cur_task))
                        task_name = ad_read_std_string(cur_task + ck3::db_object::NAME_KEY);
                    // Holder resolution (fixed 20260619): +0x30 is a DISPLAY field that
                    // reads 0xFFFFFF even for FILLED seats — always blank for the
                    // chaplain/consort seats, and for some AI realms blank on EVERY seat.
                    // +0x38 is the AUTHORITATIVE seated councillor (matches
                    // do_set_council_task ctx_a; +0x3c carries the ruler id). Prefer +0x30
                    // when populated (== +0x38 for great officers), else fall back to +0x38.
                    // Was: ALL chaplain seats false-reported "vacant" — the seat IS filled.
                    uint32_t holder_display = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_HOLDER_ID) & 0xFFFFFF;
                    uint32_t holder_seated  = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_COUNCILLOR_ID) & 0xFFFFFF;
                    uint32_t holder = (holder_display && holder_display != 0xFFFFFF) ? holder_display : holder_seated;

                    // Position key/name — D6 FIX (probe P3, 2026-08-01).
                    // pos_obj is a council-position INSTANCE, NOT a named
                    // db_object: its +0x10 is the uint32 packed position id
                    // (validated via COUNCIL_POS_KEY just above), so the old
                    // read_db_obj_name(pos_obj) call could NEVER resolve —
                    // born-broken on every seat (probes-round3.md P3.1). The
                    // canonical `councillor_*` key lives on the position TYPE
                    // definition, reached through the assignment intermediate
                    // the walk already reads:
                    //   pos_obj +0x18 (COUNCIL_POS_CURRENT_TASK → intermediate;
                    //   task name @ +0x10) → +0x30 (CCouncilPositionType)
                    //   → +0x10 std::string key (council_pos_type::NAME_KEY,
                    //   exactly the db_object shape read_db_obj_name expects).
                    // Verified live on all 5 seats — including VACANT seats,
                    // which still carry a live intermediate (P3.2-P3.4).
                    std::string pos_key;
                    {
                        uintptr_t pos_type = (cur_task && is_mapped(cur_task))
                                           ? safe_read<uintptr_t>(cur_task + ck3::council_task_type::POSITION_TYPE_PTR) : 0;
                        const char* pkey_ptr = nullptr;
                        size_t pkey_len = 0;
                        if (pos_type && is_mapped(pos_type) &&
                            read_db_obj_name(pos_type, pkey_ptr, pkey_len) && pkey_len > 0)
                            pos_key.assign(pkey_ptr, std::min(pkey_len, (size_t)127));
                    }

                    r.begin_object();
                    r.key("position_id");  r.value_uint(idx);
                    if (!pos_key.empty()) {
                        r.key("position_key");  r.value_string(pos_key.c_str(), pos_key.size());
                        r.key("position_name"); r.value_string(pos_key.c_str(), pos_key.size());
                    }
                    r.key("current_task"); r.value_string(task_name.c_str(), task_name.size());
                    // A seat is filled only if the resolved holder is a real, live
                    // character. If neither +0x30 nor +0x38 resolves, it is genuinely
                    // vacant (guards against a stale +0x38 on a vacated seat).
                    void* hc = (holder && holder != 0xFFFFFF) ? lookup_character(holder) : nullptr;
                    bool filled = hc && !is_sentinel(hc);
                    r.key("holder_id"); r.value_uint(filled ? holder : 0xFFFFFF);
                    if (filled) {
                        r.key("holder_source");
                        r.value_string((holder_display && holder_display != 0xFFFFFF) ? "display" : "seated");
                    }
                    if (!filled) {
                        r.key("vacant"); r.value_bool(true);
                        if (!pos_key.empty()) vacant_positions.push_back(pos_key);
                        else vacant_positions.push_back(std::to_string(idx));
                    } else {
                        {
                            uintptr_t h = reinterpret_cast<uintptr_t>(hc);
                            emit_char_names(r, h);
                            if (verbose) {
                                r.key("skills"); r.begin_object();
                                r.key("diplomacy");   r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_DIPLOMACY));
                                r.key("martial");     r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_MARTIAL));
                                r.key("stewardship"); r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_STEWARDSHIP));
                                r.key("intrigue");    r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_INTRIGUE));
                                r.key("learning");    r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_LEARNING));
                                r.key("prowess");     r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_PROWESS));
                                r.end_object();
                            } else {
                                // Single representative skill scalar (martial) —
                                // digestor.py reads one aggregate skill value.
                                r.key("skill"); r.value_int(safe_read<int32_t>(h + ck3::character::SKILL_MARTIAL));
                            }
                        }
                    }
                    r.end_object();
                    emitted++;
                }
                if (emitted > 0) method = "council_instance_walk";
            }
        }
    }

    // =========================================================================
    // FALLBACK A: Hash map at council_type_obj+0x370 (legacy; only if the
    // instance walk above produced nothing). Regressed to none — kept as a net.
    // CCouncilMember::ValidateAssignment uses this FNV-1a hash map as its
    // primary councillor lookup. 8-byte entries: present_flag(1B) + pad(3B) + char_id(4B).
    // =========================================================================
    uintptr_t council_type_obj_addr = (emitted == 0) ? scan_council_type_db_for_ruler(my_id) : 0;
    if (do_log) {
        fprintf(g_logfile, "[CK3-Bridge] query_council: char_id=%u, DB scan result=%p\n",
                my_id, (void*)council_type_obj_addr);
        fflush(g_logfile);
    }

    if ((council_type_obj_addr && is_mapped(council_type_obj_addr))) {
        // Read hash map
        uintptr_t hash_table = safe_read<uintptr_t>(council_type_obj_addr + ck3::council_type_obj::COUNCILLOR_HASHMAP);
        int32_t mask = safe_read<int32_t>(council_type_obj_addr + ck3::council_type_obj::COUNCILLOR_HASHMAP_MASK);
        int32_t capacity = mask + 1;

        if (do_log) {
            fprintf(g_logfile, "[CK3-Bridge] query_council: hashmap=%p, mask=0x%x, capacity=%d\n",
                    (void*)hash_table, mask, capacity);
            fflush(g_logfile);
        }

        // Build position name map from position entry array at +0x3C0 for enrichment
        struct PosEntry { uint32_t char_id; std::string name; };
        std::vector<PosEntry> pos_names;

        uintptr_t pos_array = safe_read<uintptr_t>(council_type_obj_addr + ck3::council_type_obj::POSITION_ARRAY_PTR);
        int32_t pos_count = safe_read<int32_t>(council_type_obj_addr + ck3::council_type_obj::POSITION_ARRAY_COUNT);
        if ((pos_array && is_mapped(pos_array)) && pos_count > 0 && pos_count <= 20) {
            for (int32_t i = 0; i < pos_count; i++) {
                uintptr_t entry = pos_array + i * ck3::council_type_obj::POS_ENTRY_STRIDE;
                uintptr_t pos_data = safe_read<uintptr_t>(entry + ck3::council_type_obj::POS_ENTRY_DATA_PTR);
                uintptr_t cid_arr = safe_read<uintptr_t>(entry + ck3::council_type_obj::POS_ENTRY_CHAR_ARRAY);
                int32_t cid_count = safe_read<int32_t>(entry + ck3::council_type_obj::POS_ENTRY_CHAR_COUNT);

                // Try to read position name from position data object
                std::string pname = "unknown";
                if ((pos_data && is_mapped(pos_data))) {
                    // Position data is a db_object; name key at standard offset
                    const char* name_ptr = nullptr;
                    size_t name_len = 0;
                    if (read_db_obj_name(pos_data, name_ptr, name_len) && name_len > 0) {
                        pname.assign(name_ptr, std::min(name_len, (size_t)127));
                    }
                }

                if ((cid_arr && is_mapped(cid_arr)) && cid_count > 0 && cid_count <= 5) {
                    for (int32_t j = 0; j < cid_count; j++) {
                        uint32_t cid = safe_read<uint32_t>(cid_arr + j * 4);
                        if (cid != 0 && cid != 0xFFFFFFFF)
                            pos_names.push_back({cid & 0xFFFFFF, pname});
                    }
                }
            }
        }

        // If position array gave us councillors with names, use those
        if (!pos_names.empty()) {
            for (const auto& pe : pos_names) {
                if (emit_councillor(r, pe.char_id, pe.name.c_str()))
                    emitted++;
            }
            method = "position_array";
        }
        // Otherwise iterate hash map for char IDs (no position names)
        else if ((hash_table && is_mapped(hash_table)) && capacity > 0 && capacity <= 64) {
            for (int32_t i = 0; i < capacity; i++) {
                uintptr_t entry_base = hash_table + i * ck3::council_type_obj::COUNCILLOR_HASHMAP_ENTRY;
                uint8_t present = safe_read<uint8_t>(entry_base);
                if (present != 0) {
                    uint32_t cid = safe_read<uint32_t>(entry_base + 4);
                    if (cid != 0 && cid != 0xFFFFFFFF) {
                        if (emit_councillor(r, cid & 0xFFFFFF, "unknown"))
                            emitted++;
                    }
                }
            }
            method = "councillor_hashmap";
        }

        // Last resort: legacy flat array at +0x520/+0x52C (may be nil)
        if (emitted == 0) {
            uintptr_t flat_arr = safe_read<uintptr_t>(council_type_obj_addr + ck3::council_type_obj::COUNCILLOR_IDS);
            int32_t flat_count = safe_read<int32_t>(council_type_obj_addr + ck3::council_type_obj::COUNCILLOR_COUNT);
            if ((flat_arr && is_mapped(flat_arr)) && flat_count > 0 && flat_count <= 20) {
                for (int32_t i = 0; i < flat_count; i++) {
                    uint32_t cid = safe_read<uint32_t>(flat_arr + i * 4);
                    if (cid == 0 || cid == 0xFFFFFFFF) continue;
                    if (emit_councillor(r, cid & 0xFFFFFF, "unknown"))
                        emitted++;
                }
                method = "councillor_ids_legacy";
            }
        }
    }

    // =========================================================================
    // FALLBACK 1: GetCouncilConfig inline path (alive_data + 0x4F0)
    // The g_CouncilDB may not contain entries for the player character (only AI
    // rulers). The IsCouncillor() function uses alive_data+0x4F0 which has a
    // sorted councillor ID array at +0x40 and count at +0x4C.
    // =========================================================================
    if (emitted == 0) {
        uintptr_t c = reinterpret_cast<uintptr_t>(character);
        uintptr_t alive_data = safe_read<uintptr_t>(c + 0x1A8);
        if ((alive_data && is_mapped(alive_data))) {
            uintptr_t config_base = alive_data + ck3::core_data::COUNCIL_OWNER_INLINE;
            uintptr_t config_arr = safe_read<uintptr_t>(config_base + ck3::council_member::COUNCILLOR_ARRAY);
            int32_t config_count = safe_read<int32_t>(config_base + ck3::council_member::COUNCILLOR_COUNT);

            if (do_log) {
                fprintf(g_logfile, "[CK3-Bridge] query_council: fallback1 config: config_base=%p arr=%p count=%d\n",
                        (void*)config_base, (void*)config_arr, config_count);
                fflush(g_logfile);
            }

            if ((config_arr && is_mapped(config_arr)) && config_count > 0 && config_count <= 20) {
                for (int32_t i = 0; i < config_count; i++) {
                    uint32_t cid = safe_read<uint32_t>(config_arr + i * 4);
                    if (cid == 0 || cid == 0xFFFFFFFF) continue;
                    if (emit_councillor(r, cid & 0xFFFFFF, "unknown"))
                        emitted++;
                }
                method = "council_config_inline";
            }
        }
    }

    // FALLBACK 2/3/4 REMOVED: Native function calls (GET_COUNCIL_INSTANCE, GET_COUNCIL_CONFIG,
    // IS_COUNCILLOR) and COUNCIL_POSITION_DB scan all caused game crashes when called from the
    // IPC thread. The config+0xe8/0xf4 path (runtime-validated via wide probe) works reliably.

    if (do_log) {
        diag_log_count++;
        fprintf(g_logfile, "[CK3-Bridge] query_council: DONE char=%u emitted=%d method=%s\n",
                my_id, emitted, method);
        fflush(g_logfile);
    }

    r.end_array();
    r.key("councillor_count"); r.value_int(emitted);
    r.key("method"); r.value_string(method);

    // Council vacancies (TASK B) — collected during the primary instance walk.
    r.key("vacant_count"); r.value_int(static_cast<int>(vacant_positions.size()));
    r.key("vacant_positions"); r.begin_array();
    for (const auto& vp : vacant_positions) {
        r.value_string(vp.c_str(), vp.size());
    }
    r.end_array();

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_factions: factions targeting a ruler
// Factions are stored per-character in the title_realm sub-struct:
//   faction ID array at title_realm + FACTION_LIST (0x120), count at + FACTION_COUNT (0x12C).
// Each faction ID is looked up in the global FACTION_DB (TPdxRefDatabase pattern).
// CFaction fields: ID at +0x10, TYPE_PTR at +0x18, DEMAND_TIMESTAMP at +0x74.
// Faction type object has name at +0x10 (standard db_object name key).
// query_joinable_factions — structural guidance on factions the actor can
// create/join against its liege (the vassal-breaks-free lever). v1 is structural
// (no cross-thread game-trigger calls); native validity is confirmed by attempting
// the write — act(type=faction) returns null if the engine disallows it.
// lane-query item 2: the merc listing was 562 bare {company_id} rows (49 KB to
// an agent, labels null, no size). Each row now carries what a hirer needs to
// choose, from the CMercenaryCompany fields in ::merc_company (dump-verified
// 551/551, see ck3_offsets.h): home county, captain, troop count and the
// regiment breakdown, whether it is already under contract, and (with an actor)
// the map distance from the actor's capital. Rows already under contract are
// left out unless include_hired=true; nearest first when an actor is given,
// else largest first; max_results caps the rows (0 = all).
// NOT read: the hire price. The engine computes it per hirer (FUN_06108e60 over
// the regiment array, scripted values); it is not a stored field, so `cost` is
// null and the hire call's own affordability check is the authority. The
// engine's hire-range test is also not replicated: distance_px is a plain map
// distance between capitals, not the engine's range verdict.
static bool mc_capital_xy(uintptr_t c, int32_t& out_x, int32_t& out_y);  // defined below

static bool merc_county_xy(uint32_t county_tid, int32_t& x, int32_t& y) {
    uintptr_t t = lookup_title_obj(county_tid & 0xFFFFFF);
    if (!t) return false;
    int32_t sc = safe_read<int32_t>(t + ck3::title_obj::SUB_TITLE_COUNT);
    uintptr_t sa = safe_read<uintptr_t>(t + ck3::title_obj::SUB_TITLE_ARRAY);
    if (sc <= 0 || sc > 50 || !sa || !is_mapped(sa)) return false;
    uintptr_t barony = lookup_title_obj(safe_read<uint32_t>(sa) & 0xFFFFFF);
    if (!barony) return false;
    uintptr_t prov = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
    if (!prov || !is_mapped(prov)) return false;
    uintptr_t tmpl = safe_read<uintptr_t>(prov + ck3::province::TEMPLATE_PTR);
    if (!tmpl || !is_mapped(tmpl)) return false;
    x = safe_read<int32_t>(tmpl + ck3::province_template::CENTER_X);
    y = safe_read<int32_t>(tmpl + ck3::province_template::CENTER_Y);
    return true;
}

static int32_t merc_regiment_men(uintptr_t reg_ptr) {
    int32_t alive = 0;
    for (int ch = 0; ch < static_cast<int>(::regiment::CHUNK_COUNT); ++ch) {
        uintptr_t chunk = reg_ptr + ::regiment::CHUNK_BASE + ch * ::regiment::CHUNK_STRIDE;
        int32_t allocated = safe_read<int32_t>(chunk + ::maa_chunk::ALLOCATED_COUNT);
        int32_t current   = safe_read<int32_t>(chunk + ::maa_chunk::CURRENT_COUNT);
        int32_t status    = safe_read<int32_t>(chunk + ::maa_chunk::STATUS_FLAG);
        alive += (status != 3 || current != 0) ? current : allocated;
    }
    return alive;
}

bool do_query_hireable_mercenaries(const char* params_json, std::string* out) {
    int actor_param = json_get_int(params_json, "actor_id");
    if (actor_param <= 0) actor_param = json_get_int(params_json, "character_id");
    int max_results = json_get_int(params_json, "max_results");
    if (max_results < 0) max_results = 0;
    bool include_hired = json_get_bool(params_json, "include_hired", false);

    int32_t ax = 0, ay = 0;
    bool have_actor_xy = false;
    uint32_t actor_id = 0;
    if (actor_param > 0) {
        void* a = lookup_character(static_cast<uint32_t>(actor_param));
        if (a && !is_sentinel(a)) {
            actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(a) + ck3::character::ID) & 0xFFFFFF;
            have_actor_xy = mc_capital_xy(reinterpret_cast<uintptr_t>(a), ax, ay);
        }
    }

    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::MERC_COMPANY_DB);
    uintptr_t table = db ? safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR) : 0;
    uint32_t capacity = db ? safe_read<uint32_t>(db + ck3::hash_map::CAPACITY) : 0;

    struct Row { uintptr_t e; uint32_t id; int32_t men; int64_t dist2; bool has_dist; uint32_t hired_by; };
    std::vector<Row> rows;
    uint32_t total = 0, hired = 0;
    if (table && is_mapped(table) && capacity > 0 && capacity <= 100000) {
        for (uint32_t i = 0; i < capacity; ++i) {
            uintptr_t e = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
            if (!e || !is_mapped(e)) continue;
            uint32_t key = safe_read<uint32_t>(e + ck3::hash_map::ENTRY_KEY_REF);
            if ((key & 0xFFFFFF) != i) continue;
            ++total;
            uint32_t hb = safe_read<uint32_t>(e + ::merc_company::HIRED_BY);
            bool is_hired = (hb != 0xFFFFFFFF);
            if (is_hired) ++hired;
            if (is_hired && !include_hired) continue;
            Row row{e, key & 0xFFFFFF, 0, 0, false, hb};
            uint32_t n = safe_read<uint32_t>(e + ::merc_company::REGIMENT_COUNT);
            uintptr_t arr = safe_read<uintptr_t>(e + ::merc_company::REGIMENT_IDS);
            if (arr && is_mapped(arr) && n > 0 && n <= 64) {
                for (uint32_t k = 0; k < n; ++k) {
                    uintptr_t reg = ad_db_lookup(ck3::globals::REGIMENT_DB, safe_read<uint32_t>(arr + k * 4));
                    if (reg && is_mapped(reg)) row.men += merc_regiment_men(reg);
                }
            }
            int32_t cx = 0, cy = 0;
            if (have_actor_xy && merc_county_xy(safe_read<uint32_t>(e + ::merc_company::HOME_COUNTY_ID), cx, cy)) {
                int64_t dx = cx - ax, dy = cy - ay;
                row.dist2 = dx * dx + dy * dy;
                row.has_dist = true;
            }
            rows.push_back(row);
        }
    }
    // Ties are broken by company id: std::sort is not stable, and libstdc++ and the
    // MSVC STL order equal keys differently (P5: two 2312-man companies swapped).
    if (have_actor_xy) {
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.has_dist != b.has_dist) return a.has_dist;
            if (a.has_dist && a.dist2 != b.dist2) return a.dist2 < b.dist2;
            if (!a.has_dist && a.men != b.men) return a.men > b.men;
            return a.id < b.id; });
    } else {
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.men != b.men ? a.men > b.men : a.id < b.id; });
    }
    size_t listed = rows.size();
    if (max_results > 0 && rows.size() > static_cast<size_t>(max_results)) rows.resize(max_results);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_hireable_mercenaries");
    if (actor_id) { r.key("actor_id"); r.value_uint(actor_id); }
    r.key("companies"); r.begin_array();
    for (const Row& row : rows) {
        uintptr_t e = row.e;
        r.begin_object();
        r.key("company_id"); r.value_uint(row.id);
        uint32_t home = safe_read<uint32_t>(e + ::merc_company::HOME_COUNTY_ID) & 0xFFFFFF;
        std::string home_name, home_key;
        if (uintptr_t t = lookup_title_obj(home)) {
            home_name = ad_read_std_string(t + ck3::title_obj::TITLE_DISPLAY_NAME);
            uintptr_t tp = safe_read<uintptr_t>(t + ck3::title_obj::TITLE_TYPE_PTR);
            if (tp && is_mapped(tp)) home_key = ad_read_std_string(tp + ck3::db_object::NAME_KEY);
        }
        std::string label = std::string("Mercenary company from ")
            + (home_name.empty() ? (home_key.empty() ? std::to_string(home) : home_key) : home_name)
            + ", " + std::to_string(row.men) + " men";
        r.key("label"); r.value_string(label.c_str(), label.size());
        r.key("home_county_id"); r.value_uint(home);
        if (!home_key.empty()) { r.key("home_county_key"); r.value_string(home_key.c_str(), home_key.size()); }
        if (!home_name.empty()) { r.key("home_county_name"); r.value_string(home_name.c_str(), home_name.size()); }
        uint32_t cap = safe_read<uint32_t>(e + ::merc_company::CAPTAIN_CHAR_ID) & 0xFFFFFF;
        r.key("captain_id"); r.value_uint(cap);
        if (void* cc = lookup_character(cap)) {
            if (!is_sentinel(cc)) {
                std::string cn = ad_read_char_name(reinterpret_cast<uintptr_t>(cc));
                if (!cn.empty()) { r.key("captain_name"); r.value_string(cn.c_str(), cn.size()); }
            }
        }
        r.key("troops"); r.value_int(row.men);
        r.key("regiments"); r.begin_array();
        uint32_t n = safe_read<uint32_t>(e + ::merc_company::REGIMENT_COUNT);
        uintptr_t arr = safe_read<uintptr_t>(e + ::merc_company::REGIMENT_IDS);
        if (arr && is_mapped(arr) && n > 0 && n <= 64) {
            for (uint32_t k = 0; k < n; ++k) {
                uintptr_t reg = ad_db_lookup(ck3::globals::REGIMENT_DB, safe_read<uint32_t>(arr + k * 4));
                if (!reg || !is_mapped(reg)) continue;
                const char* tn = nullptr; size_t tl = 0;
                uintptr_t tptr = safe_read<uintptr_t>(reg + ::regiment::MAA_TYPE_PTR);
                if (tptr && is_mapped(tptr)) read_db_obj_name(tptr, tn, tl);
                r.begin_object();
                r.key("maa_type");
                if (tn && tl) r.value_string(tn, tl); else r.value_string("levies");
                r.key("men"); r.value_int(merc_regiment_men(reg));
                r.end_object();
            }
        }
        r.end_array();
        r.key("hired_by");
        if (row.hired_by == 0xFFFFFFFF) r.value_null(); else r.value_uint(row.hired_by & 0xFFFFFF);
        if (row.has_dist) {
            r.key("distance_px"); r.value_int(static_cast<int>(std::sqrt(static_cast<double>(row.dist2))));
        }
        r.key("cost"); r.value_null();
        r.end_object();
    }
    r.end_array();
    r.key("count"); r.value_uint(static_cast<uint32_t>(rows.size()));
    r.key("listed_before_cap"); r.value_uint(static_cast<uint32_t>(listed));
    r.key("total_companies"); r.value_uint(total);
    r.key("under_contract"); r.value_uint(hired);
    r.key("order"); r.value_string(have_actor_xy ? "nearest_first" : "largest_first");
    r.key("cost_note"); r.value_string(
        "cost is null: the hire price is computed by the game per hirer and is not a stored "
        "field. The hire itself refuses what the ruler cannot afford. distance_px is map "
        "distance between capitals, not the game's hire-range verdict.");
    r.key("hint"); r.value_string(
        "Hire via do(intent='hire_merc', args={company_id}). Actor must be an AI ruler.");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// do_query_maa_types — lane-query item 2: the enumerate behind
// do(action_class="men_at_arms"). do(intent="men_at_arms") refused with
// maa_type_required and pointed at an enumerate that did not exist. Lists every
// MaA type in MAA_TYPE_DB (109 in build e61ff264) with its upfront recruit cost
// (maa_type::RECRUIT_GOLD_COST, HIGH) and per-stack monthly upkeep
// (MAINT_GOLD_PER_STACK, HIGH); with an actor, the actor's gold, an
// `affordable` flag and how many regiments of each type the actor already
// fields (title_realm PERSONAL_MAA_PTR / TITLE_MAA_REG_PTR, as query_armies).
// NOT checked: whether the actor's culture/innovations unlock a type. That is
// decided by the game at recruit; a type already fielded is known to be
// available, the rest are candidates. Pure reads.
// =============================================================================
bool do_query_maa_types(const char* params_json, std::string* out) {
    int actor_param = json_get_int(params_json, "actor_id");
    if (actor_param <= 0) actor_param = json_get_int(params_json, "character_id");
    void* actor = (actor_param > 0) ? lookup_character(static_cast<uint32_t>(actor_param))
                                    : get_controlled_character();
    if (actor && is_sentinel(actor)) actor = nullptr;

    std::map<std::string, int> fielded;
    double gold = 0.0;
    bool have_gold = false;
    uint32_t actor_id = 0;
    if (actor) {
        uintptr_t a = reinterpret_cast<uintptr_t>(actor);
        actor_id = safe_read<uint32_t>(a + ck3::character::ID) & 0xFFFFFF;
        uintptr_t core = safe_read<uintptr_t>(a + ck3::character::REALM_DATA);
        if (core && is_mapped(core)) {
            gold = static_cast<double>(safe_read<int64_t>(core + ck3::core_data::GOLD)) / 100000.0;
            have_gold = true;
        }
        uintptr_t tr = safe_read<uintptr_t>(a + ck3::character::TITLE_REALM_DATA);
        if (tr && is_mapped(tr)) {
            const size_t srcs[] = {ck3::title_realm::PERSONAL_MAA_PTR, ck3::title_realm::TITLE_MAA_REG_PTR};
            for (size_t off : srcs) {
                uintptr_t arr = safe_read<uintptr_t>(tr + off);
                int32_t cnt = safe_read<int32_t>(tr + off + 0x0C);
                if (!arr || !is_mapped(arr) || cnt <= 0 || cnt > 500) continue;
                for (int32_t i = 0; i < cnt; ++i) {
                    uint32_t ref = safe_read<uint32_t>(arr + i * 4);
                    if (ref == 0 || ref == 0xFFFFFFFF) continue;
                    uintptr_t reg = ad_db_lookup(ck3::globals::REGIMENT_DB, ref);
                    if (!reg || !is_mapped(reg)) continue;
                    uintptr_t tptr = safe_read<uintptr_t>(reg + ::regiment::MAA_TYPE_PTR);
                    const char* tn = nullptr; size_t tl = 0;
                    if (tptr && is_mapped(tptr) && read_db_obj_name(tptr, tn, tl) && tl)
                        fielded[std::string(tn, tl)]++;
                }
            }
        }
    }

    uintptr_t db  = safe_read_offset<uintptr_t>(ck3::globals::MAA_TYPE_DB);
    uintptr_t arr = db ? safe_read<uintptr_t>(db + ck3::globals::MAA_TYPE_DB_ARR_OFF) : 0;
    uint32_t  cnt = db ? safe_read<uint32_t>(db + ck3::globals::MAA_TYPE_DB_CNT_OFF) : 0;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_maa_types");
    if (actor_id) { r.key("actor_id"); r.value_uint(actor_id); }
    if (have_gold) { r.key("gold"); r.value_float(gold); }
    r.key("types"); r.begin_array();
    int emitted = 0;
    if (arr && is_mapped(arr) && cnt > 0 && cnt <= 5000) {
        for (uint32_t i = 0; i < cnt; ++i) {
            uintptr_t e = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
            if (!e || !is_mapped(e)) continue;
            std::string key = ad_read_std_string(e + ::maa_type::NAME_KEY);
            if (key.empty()) continue;
            double cost = static_cast<double>(safe_read<int64_t>(e + ::maa_type::RECRUIT_GOLD_COST)) / 100000.0;
            double upkeep = static_cast<double>(safe_read<int64_t>(e + ::maa_type::MAINT_GOLD_PER_STACK)) / 100000.0;
            r.begin_object();
            r.key("maa_type"); r.value_string(key.c_str(), key.size());
            r.key("recruit_cost"); r.value_float(cost);
            r.key("upkeep_per_stack"); r.value_float(upkeep);
            if (have_gold) { r.key("affordable"); r.value_bool(cost <= gold); }
            auto it = fielded.find(key);
            r.key("fielded_regiments"); r.value_int(it == fielded.end() ? 0 : it->second);
            r.end_object();
            ++emitted;
        }
    }
    r.end_array();
    r.key("count"); r.value_int(emitted);
    r.key("note"); r.value_string(
        "Whether the ruler's culture/innovations unlock a type is decided by the game at "
        "recruit and is not checked here; fielded_regiments>0 means the realm already "
        "fields that type. Recruit via do(intent='men_at_arms', args={maa_type}).");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

bool do_query_joinable_factions(const char* params_json, std::string* out) {
    int cid = json_get_int(params_json, "character_id");
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_joinable_factions");
    r.key("actor_id"); r.value_int(cid > 0 ? (cid & 0xFFFFFF) : 0);
    r.key("faction_types"); r.begin_array();
    const char* types[] = {"independence_faction", "liberty_faction", "claimant_faction"};
    for (const char* t : types) {
        r.begin_object();
        r.key("faction_type"); r.value_string(t);
        r.key("action"); r.value_string("create_or_join");
        r.end_object();
    }
    r.end_array();
    r.key("hint"); r.value_string(
        "Pursue via act(type='faction', faction_action='create', faction_type=<key>, "
        "target_id=<your liege_id from query(domain='character')>). The engine validates and "
        "returns failure if disallowed (you're independent, crown authority too high, on "
        "cooldown). independence_faction/liberty_faction break free from your liege; "
        "claimant_faction backs a claimant (pass claimant_id/title_id).");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

bool do_query_factions(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_factions: character not found\n");
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(my_id);

    // Read global discontent threshold (R17: fixed-point / 100,000)
    int64_t discontent_threshold = safe_read_offset<int64_t>(ck3::globals::FACTION_DISCONTENT_THRESHOLD);
    double threshold_pct = (discontent_threshold > 0) ? (double)discontent_threshold / 100000.0 : 100.0;
    r.key("discontent_threshold_pct"); r.value_double(threshold_pct);

    r.key("factions"); r.begin_array();

    int32_t faction_count_out = 0;

    // Faction list is on the title_realm sub-struct: *(*(char+0x1B8)+0x120)
    uintptr_t title_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (title_data && is_mapped(title_data)) {
        uintptr_t faction_list = safe_read<uintptr_t>(title_data + ck3::title_realm::FACTION_LIST);
        int32_t faction_count = safe_read<int32_t>(title_data + ck3::title_realm::FACTION_COUNT);

        if (faction_list && is_mapped(faction_list) && faction_count > 0 && faction_count < 200) {
            for (int32_t i = 0; i < faction_count; i++) {
                uint32_t faction_id = safe_read<uint32_t>(faction_list + i * sizeof(uint32_t));
                if (faction_id == 0 || faction_id == 0xFFFFFFFF) continue;

                // Look up in global faction DB
                uintptr_t fo = ad_db_lookup(ck3::globals::FACTION_DB, faction_id);
                if (!fo || !is_mapped(fo)) continue;

                r.begin_object();
                r.key("faction_id"); r.value_uint(faction_id & 0xFFFFFF);
                faction_count_out++;

                // --- Faction type name ---
                uintptr_t type_ptr = safe_read<uintptr_t>(fo + ck3::faction_v2::TYPE_PTR);
                if (type_ptr && is_mapped(type_ptr)) {
                    std::string type_name = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                    if (!type_name.empty()) {
                        r.key("type"); r.value_string(type_name.c_str(), type_name.size());
                    } else {
                        r.key("type"); r.value_string("unknown");
                    }
                    // Is this a county-based faction?
                    uint8_t is_county = safe_read<uint8_t>(type_ptr + ck3::faction_v2::type_scripts::IS_COUNTY_FACTION);
                    if (is_county) {
                        r.key("is_county_faction"); r.value_bool(true);
                    }
                } else {
                    r.key("type"); r.value_string("unknown");
                }

                // --- Target character ---
                uint32_t target_ref = safe_read<uint32_t>(fo + ck3::faction_v2::TARGET_CHAR_REF);
                if (target_ref != 0 && target_ref != 0xFFFFFFFF) {
                    r.key("target_character_id"); r.value_uint(target_ref & 0xFFFFFF);
                }

                // --- Leader character ---
                uint32_t leader_ref = safe_read<uint32_t>(fo + ck3::faction_v2::LEADER_CHAR_REF);
                if (leader_ref != 0 && leader_ref != 0xFFFFFFFF) {
                    r.key("leader_character_id"); r.value_uint(leader_ref & 0xFFFFFF);
                }

                // --- Discontent (R17: int64 fixed-point / 100,000) ---
                int64_t discontent_raw = safe_read<int64_t>(fo + ck3::faction_v2::DISCONTENT);
                double discontent_pct = (double)discontent_raw / 100000.0;  // convert to percentage (fixed-point / 100,000)
                r.key("discontent_pct"); r.value_double(discontent_pct);

                // --- Base discontent (clamped to [0, threshold]) ---
                int64_t base_disc_raw = safe_read<int64_t>(fo + ck3::faction_v2::BASE_DISCONTENT);
                r.key("base_discontent_pct"); r.value_double((double)base_disc_raw / 100000.0);

                // --- Power ratio (faction_power / (liege_mil - faction_mil) * 100) ---
                int64_t power_ratio_raw = safe_read<int64_t>(fo + ck3::faction_v2::POWER_RATIO_CACHED);
                double power_ratio = (double)power_ratio_raw / 100000.0;
                r.key("power_ratio"); r.value_double(power_ratio);

                // --- Ultimatum state ---
                int64_t ult_progress_raw = safe_read<int64_t>(fo + ck3::faction_v2::ULTIMATUM_PROGRESS);
                double ult_progress = (double)ult_progress_raw / 100000.0;
                r.key("ultimatum_progress_pct"); r.value_double(ult_progress);

                int64_t ult_fire_date = safe_read<int64_t>(fo + ck3::faction_v2::ULTIMATUM_FIRE_DATE);
                int64_t sentinel = safe_read_offset<int64_t>(ck3::faction_globals::SENTINEL_DATE);
                if (ult_fire_date > 0 && ult_fire_date != sentinel) {
                    r.key("ultimatum_fire_date_raw"); r.value_int(static_cast<int32_t>(ult_fire_date));
                }

                // --- War state ---
                uint32_t war_ref = safe_read<uint32_t>(fo + ck3::faction_v2::WAR_REF);
                bool at_war = safe_read<uint8_t>(fo + ck3::faction_v2::AT_WAR_FLAG) != 0;
                r.key("at_war"); r.value_bool(at_war);
                if (war_ref != 0 && war_ref != 0xFFFFFFFF) {
                    r.key("war_id"); r.value_uint(war_ref & 0xFFFFFF);
                }
                bool war_inactive = safe_read<uint8_t>(fo + ck3::faction_v2::WAR_INACTIVE_FLAG) != 0;
                r.key("war_inactive"); r.value_bool(war_inactive);

                // --- Cooldown date ---
                int64_t cooldown = safe_read<int64_t>(fo + ck3::faction_v2::COOLDOWN_DATE);
                if (cooldown > 0) {
                    r.key("cooldown_date_raw"); r.value_int(static_cast<int32_t>(cooldown));
                }

                // --- Disabled / press demand flags ---
                bool disabled = safe_read<uint8_t>(fo + ck3::faction_v2::DISABLED_FLAG) != 0;
                if (disabled) {
                    r.key("disabled"); r.value_bool(true);
                }
                bool should_press = safe_read<uint8_t>(fo + ck3::faction_v2::SHOULD_PRESS_DEMAND) != 0;
                if (should_press) {
                    r.key("should_press_demand"); r.value_bool(true);
                }

                // --- Character members (R17: stride 0x20, char_ref at +0x08) ---
                uintptr_t cmem_ptr = safe_read<uintptr_t>(fo + ck3::faction_v2::CHAR_MEMBERS_PTR);
                int32_t cmem_count = safe_read<int32_t>(fo + ck3::faction_v2::CHAR_MEMBERS_COUNT);
                if (cmem_ptr && is_mapped(cmem_ptr) && cmem_count > 0 && cmem_count < 500) {
                    r.key("members"); r.begin_array();
                    for (int32_t m = 0; m < cmem_count; m++) {
                        uintptr_t entry = cmem_ptr + m * ck3::faction_v2::char_member::STRIDE;
                        uint8_t invalidated = safe_read<uint8_t>(entry + ck3::faction_v2::char_member::IS_INVALIDATED);
                        if (invalidated) continue;
                        uint32_t mem_char_ref = safe_read<uint32_t>(entry + ck3::faction_v2::char_member::CHAR_REF);
                        if (mem_char_ref == 0 || mem_char_ref == 0xFFFFFFFF) continue;
                        r.value_uint(mem_char_ref & 0xFFFFFF);
                    }
                    r.end_array();
                    r.key("member_count"); r.value_int(cmem_count);
                } else {
                    r.key("members"); r.begin_array(); r.end_array();
                    r.key("member_count"); r.value_int(0);
                }

                // --- County members (R17: stride 0x18, title_ref at +0x08) ---
                uintptr_t county_ptr = safe_read<uintptr_t>(fo + ck3::faction_v2::COUNTY_MEMBERS_PTR);
                int32_t county_count = safe_read<int32_t>(fo + ck3::faction_v2::COUNTY_MEMBERS_COUNT);
                if (county_ptr && is_mapped(county_ptr) && county_count > 0 && county_count < 500) {
                    r.key("county_members"); r.begin_array();
                    for (int32_t m = 0; m < county_count; m++) {
                        uintptr_t entry = county_ptr + m * ck3::faction_v2::county_member::STRIDE;
                        uint8_t invalidated = safe_read<uint8_t>(entry + ck3::faction_v2::county_member::IS_INVALIDATED);
                        if (invalidated) continue;
                        uint32_t title_ref = safe_read<uint32_t>(entry + ck3::faction_v2::county_member::TITLE_REF);
                        if (title_ref == 0 || title_ref == 0xFFFFFFFF) continue;
                        r.value_uint(title_ref & 0xFFFFFF);
                    }
                    r.end_array();
                    r.key("county_member_count"); r.value_int(county_count);
                }

                r.end_object();
            }
        }
    }

    r.end_array();
    r.key("faction_count"); r.value_int(faction_count_out);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_vassal_loyalty: per-vassal loyalty snapshot for a ruler.
//
// For each DIRECT vassal of the resolved ruler, returns:
//   { id, name, military_strength, opinion_of_me, in_faction, faction_type,
//     faction_id }
// sorted ascending by opinion_of_me (least loyal first), plus top-level
// count_disloyal (opinion_of_me < 0) and count_in_faction.
//
// Reuses three proven, SIGSEGV-safe building blocks under the reader thread:
//   - DIRECT vassal roster: title_realm::DIRECT_VASSAL_ARRAY (as do_query_realm)
//   - opinion_of_me: GET_OPINION_OF(vassal, liege) — the vassal's opinion of
//     the liege (same engine fn do_query_opinion uses)
//   - faction membership: cross-reference each vassal id against the ruler's
//     own factions' CHAR_MEMBERS arrays (as do_query_factions)
// Read-only: no game state is mutated. Classified as a game-state query
// (needs pause) like query_realm / query_opinion.
bool do_query_vassal_loyalty(const char* params_json, std::string* out) {
    void* ruler = resolve_character(params_json);
    if (!ruler || is_sentinel(ruler)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_vassal_loyalty: character not found\n");
        if (out) *out = "{\"error\":\"character not found\"}";
        return false;
    }

    uintptr_t rc = reinterpret_cast<uintptr_t>(ruler);
    uint32_t ruler_id = char_get_id(ruler) & 0xFFFFFF;
    uintptr_t title_data = safe_read<uintptr_t>(rc + ck3::character::TITLE_REALM_DATA);

    // Engine opinion getter (same as do_query_opinion). opinion_of_me for a
    // vassal v is GET_OPINION_OF(v, ruler) — v's opinion of the liege.
    using fn_opinion = int(*)(void*, void*);
    static fn_opinion get_opinion = reinterpret_cast<fn_opinion>(
        bridge::game_fn(ck3::funcs::GET_OPINION_OF));

    // --- Build a vassal-id → faction membership map from the ruler's factions ---
    // A vassal is "in_faction" if it appears as a (non-invalidated) char member
    // of any faction targeting this ruler. We record the first faction found.
    struct FactionInfo { bool in_faction; std::string type; uint32_t faction_id; };
    std::unordered_map<uint32_t, FactionInfo> faction_of;

    if (title_data && is_mapped(title_data)) {
        uintptr_t faction_list = safe_read<uintptr_t>(title_data + ck3::title_realm::FACTION_LIST);
        int32_t faction_count = safe_read<int32_t>(title_data + ck3::title_realm::FACTION_COUNT);
        if (faction_list && is_mapped(faction_list) && faction_count > 0 && faction_count < 200) {
            for (int32_t i = 0; i < faction_count; i++) {
                uint32_t faction_id = safe_read<uint32_t>(faction_list + i * sizeof(uint32_t));
                if (faction_id == 0 || faction_id == 0xFFFFFFFF) continue;
                uintptr_t fo = ad_db_lookup(ck3::globals::FACTION_DB, faction_id);
                if (!fo || !is_mapped(fo)) continue;

                std::string ftype = "unknown";
                uintptr_t type_ptr = safe_read<uintptr_t>(fo + ck3::faction_v2::TYPE_PTR);
                if (type_ptr && is_mapped(type_ptr)) {
                    std::string tn = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                    if (!tn.empty()) ftype = tn;
                }

                uintptr_t cmem_ptr = safe_read<uintptr_t>(fo + ck3::faction_v2::CHAR_MEMBERS_PTR);
                int32_t cmem_count = safe_read<int32_t>(fo + ck3::faction_v2::CHAR_MEMBERS_COUNT);
                if (cmem_ptr && is_mapped(cmem_ptr) && cmem_count > 0 && cmem_count < 500) {
                    for (int32_t m = 0; m < cmem_count; m++) {
                        uintptr_t entry = cmem_ptr + m * ck3::faction_v2::char_member::STRIDE;
                        uint8_t invalidated = safe_read<uint8_t>(entry + ck3::faction_v2::char_member::IS_INVALIDATED);
                        if (invalidated) continue;
                        uint32_t mem = safe_read<uint32_t>(entry + ck3::faction_v2::char_member::CHAR_REF);
                        if (mem == 0 || mem == 0xFFFFFFFF) continue;
                        mem &= 0xFFFFFF;
                        if (faction_of.find(mem) == faction_of.end()) {
                            faction_of[mem] = FactionInfo{ true, ftype, faction_id & 0xFFFFFF };
                        }
                    }
                }
            }
        }
    }

    // --- Iterate DIRECT vassals, gathering per-vassal loyalty rows ---
    struct VassalRow {
        uint32_t id;
        std::string name;
        int32_t military_strength;
        int opinion_of_me;
        bool in_faction;
        std::string faction_type;
        uint32_t faction_id;
    };
    std::vector<VassalRow> rows;

    if (title_data && is_mapped(title_data)) {
        uintptr_t vassal_arr = title_data + ck3::title_realm::DIRECT_VASSAL_ARRAY;
        uintptr_t vassal_data_ptr = safe_read<uintptr_t>(vassal_arr);
        int32_t vassal_cnt = safe_read<int32_t>(vassal_arr + 0x0C);
        if (vassal_data_ptr && is_mapped(vassal_data_ptr) && vassal_cnt > 0 && vassal_cnt < 500) {
            for (int32_t i = 0; i < vassal_cnt; i++) {
                uint32_t vid = safe_read<uint32_t>(vassal_data_ptr + i * 4);
                void* vassal_char = lookup_character(vid);
                if (!vassal_char || is_sentinel(vassal_char)) continue;
                if (g_char_is_alive && !g_char_is_alive(vassal_char)) continue;
                uintptr_t vc = reinterpret_cast<uintptr_t>(vassal_char);

                VassalRow row{};
                row.id = vid & 0xFFFFFF;
                row.name = ad_read_char_name(vc);

                uintptr_t vtitle = safe_read<uintptr_t>(vc + ck3::character::TITLE_REALM_DATA);
                if (vtitle && is_mapped(vtitle)) {
                    row.military_strength = safe_read<int32_t>(vtitle + ck3::title_realm::MIL_CURRENT);
                }

                // opinion_of_me = vassal's opinion of the liege.
                int op = get_opinion(vassal_char, ruler);
                if (op < -200 || op > 200) op = 0;  // sanity clamp (matches do_query_opinion)
                row.opinion_of_me = op;

                auto fit = faction_of.find(row.id);
                if (fit != faction_of.end()) {
                    row.in_faction = true;
                    row.faction_type = fit->second.type;
                    row.faction_id = fit->second.faction_id;
                }

                rows.push_back(std::move(row));
            }
        }
    }

    // Sort ascending by opinion_of_me (least loyal first); stable tiebreak by id.
    std::stable_sort(rows.begin(), rows.end(), [](const VassalRow& a, const VassalRow& b) {
        if (a.opinion_of_me != b.opinion_of_me) return a.opinion_of_me < b.opinion_of_me;
        return a.id < b.id;
    });

    int count_disloyal = 0;
    int count_in_faction = 0;
    for (const auto& v : rows) {
        if (v.opinion_of_me < 0) count_disloyal++;
        if (v.in_faction) count_in_faction++;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_vassal_loyalty");
    r.key("ruler_id"); r.value_uint(ruler_id);
    r.key("vassal_count"); r.value_int(static_cast<int>(rows.size()));
    r.key("count_disloyal"); r.value_int(count_disloyal);
    r.key("count_in_faction"); r.value_int(count_in_faction);
    r.key("vassals"); r.begin_array();
    for (const auto& v : rows) {
        r.begin_object();
        r.key("id"); r.value_uint(v.id);
        if (!v.name.empty()) { r.key("name"); r.value_string(v.name.c_str(), v.name.size()); }
        r.key("military_strength"); r.value_int(v.military_strength);
        r.key("opinion_of_me"); r.value_int(v.opinion_of_me);
        r.key("in_faction"); r.value_bool(v.in_faction);
        if (v.in_faction) {
            r.key("faction_type"); r.value_string(v.faction_type.c_str(), v.faction_type.size());
            r.key("faction_id"); r.value_uint(v.faction_id);
        }
        r.end_object();
    }
    r.end_array();
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// query_faction_threat: computed threat assessment for factions targeting a ruler
// Returns a threat-oriented summary with derived fields (threat level, days to
// ultimatum, power percentage) for agent decision-making.
bool do_query_faction_threat(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_faction_threat: character not found\n");
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    // Read current game date for days-to-ultimatum computation
    int32_t current_date_raw = 0;
    {
        uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
        if (gs) current_date_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    }

    // Read global thresholds
    int64_t discontent_threshold = safe_read_offset<int64_t>(ck3::globals::FACTION_DISCONTENT_THRESHOLD);
    double threshold_pct = (discontent_threshold > 0) ? (double)discontent_threshold / 100000.0 : 100.0;
    int32_t ultimatum_delay_days = safe_read_offset<int32_t>(ck3::faction_globals::ULTIMATUM_DELAY_DAYS);
    int64_t sentinel = safe_read_offset<int64_t>(ck3::faction_globals::SENTINEL_DATE);

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(my_id);
    r.key("discontent_threshold_pct"); r.value_double(threshold_pct);
    r.key("ultimatum_delay_days"); r.value_int(ultimatum_delay_days);

    r.key("factions"); r.begin_array();

    int32_t faction_count_out = 0;
    int32_t critical_count = 0;
    int32_t high_count = 0;

    uintptr_t title_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (title_data && is_mapped(title_data)) {
        uintptr_t faction_list = safe_read<uintptr_t>(title_data + ck3::title_realm::FACTION_LIST);
        int32_t faction_count = safe_read<int32_t>(title_data + ck3::title_realm::FACTION_COUNT);

        if (faction_list && is_mapped(faction_list) && faction_count > 0 && faction_count < 200) {
            for (int32_t i = 0; i < faction_count; i++) {
                uint32_t faction_id = safe_read<uint32_t>(faction_list + i * sizeof(uint32_t));
                if (faction_id == 0 || faction_id == 0xFFFFFFFF) continue;

                uintptr_t fo = ad_db_lookup(ck3::globals::FACTION_DB, faction_id);
                if (!fo || !is_mapped(fo)) continue;

                // Skip disabled factions
                bool disabled = safe_read<uint8_t>(fo + ck3::faction_v2::DISABLED_FLAG) != 0;
                if (disabled) continue;

                r.begin_object();
                r.key("faction_id"); r.value_uint(faction_id & 0xFFFFFF);
                faction_count_out++;

                // Type name
                uintptr_t type_ptr = safe_read<uintptr_t>(fo + ck3::faction_v2::TYPE_PTR);
                if (type_ptr && is_mapped(type_ptr)) {
                    std::string type_name = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                    if (!type_name.empty()) {
                        r.key("type"); r.value_string(type_name.c_str(), type_name.size());
                    } else {
                        r.key("type"); r.value_string("unknown");
                    }
                } else {
                    r.key("type"); r.value_string("unknown");
                }

                // Leader
                uint32_t leader_ref = safe_read<uint32_t>(fo + ck3::faction_v2::LEADER_CHAR_REF);
                bool has_leader = (leader_ref != 0 && leader_ref != 0xFFFFFFFF);
                r.key("leader_id"); r.value_uint(has_leader ? (leader_ref & 0xFFFFFF) : 0);

                // Discontent
                int64_t discontent_raw = safe_read<int64_t>(fo + ck3::faction_v2::DISCONTENT);
                double discontent = (double)discontent_raw / 100000.0;
                r.key("discontent"); r.value_double(discontent);

                // Power ratio (faction / (liege - faction) * 100)
                int64_t power_ratio_raw = safe_read<int64_t>(fo + ck3::faction_v2::POWER_RATIO_CACHED);
                double power_ratio = (double)power_ratio_raw / 100000.0;
                r.key("power_ratio"); r.value_double(power_ratio);

                // Power as percentage of liege total: faction/(faction+balance) = power_ratio/(power_ratio+100)
                double power_pct_of_liege = (power_ratio >= 0) ? (power_ratio / (power_ratio + 100.0)) * 100.0 : 0.0;
                if (power_pct_of_liege > 100.0) power_pct_of_liege = 100.0;
                r.key("power_pct_of_liege"); r.value_double(power_pct_of_liege);

                // Member counts
                int32_t cmem_count = safe_read<int32_t>(fo + ck3::faction_v2::CHAR_MEMBERS_COUNT);
                int32_t county_count = safe_read<int32_t>(fo + ck3::faction_v2::COUNTY_MEMBERS_COUNT);
                r.key("member_count"); r.value_int(cmem_count > 0 ? cmem_count : 0);
                r.key("county_count"); r.value_int(county_count > 0 ? county_count : 0);

                // War state
                bool at_war = safe_read<uint8_t>(fo + ck3::faction_v2::AT_WAR_FLAG) != 0;
                r.key("at_war"); r.value_bool(at_war);

                // Cooldown check
                int64_t cooldown = safe_read<int64_t>(fo + ck3::faction_v2::COOLDOWN_DATE);
                bool cooldown_active = (cooldown > 0 && current_date_raw > 0 && cooldown > current_date_raw);
                r.key("cooldown_active"); r.value_bool(cooldown_active);

                // Ultimatum progress and timing
                int64_t ult_progress_raw = safe_read<int64_t>(fo + ck3::faction_v2::ULTIMATUM_PROGRESS);
                double ult_progress = (double)ult_progress_raw / 100000.0;
                r.key("ultimatum_progress_pct"); r.value_double(ult_progress);

                int64_t ult_fire_date = safe_read<int64_t>(fo + ck3::faction_v2::ULTIMATUM_FIRE_DATE);
                bool has_ultimatum = (ult_fire_date > 0 && ult_fire_date != sentinel);
                r.key("ultimatum_active"); r.value_bool(has_ultimatum);

                if (has_ultimatum && current_date_raw > 0) {
                    int32_t days_remaining = (static_cast<int32_t>(ult_fire_date) - current_date_raw) / 24;
                    r.key("days_to_ultimatum"); r.value_int(days_remaining > 0 ? days_remaining : 0);
                }

                // Composite: can this faction fire its ultimatum?
                // Approximation of CanFireUltimatum: not at war, no cooldown, has leader, discontent >= threshold
                uint32_t war_ref = safe_read<uint32_t>(fo + ck3::faction_v2::WAR_REF);
                bool war_inactive = safe_read<uint8_t>(fo + ck3::faction_v2::WAR_INACTIVE_FLAG) != 0;
                bool no_war_blocker = (!at_war || war_inactive || war_ref == 0xFFFFFFFF);
                bool can_fire = no_war_blocker && !cooldown_active && has_leader && (discontent >= threshold_pct);
                r.key("can_fire_ultimatum"); r.value_bool(can_fire);

                // Threat level classification
                const char* threat_level;
                if (at_war && !war_inactive) {
                    threat_level = "at_war";
                } else if (has_ultimatum || ult_progress >= 100.0) {
                    threat_level = "critical";
                    critical_count++;
                } else if (discontent >= threshold_pct * 0.80 && power_ratio >= 80.0) {
                    threat_level = "high";
                    high_count++;
                } else if (discontent >= threshold_pct * 0.50 || power_ratio >= 100.0) {
                    threat_level = "medium";
                } else {
                    threat_level = "low";
                }
                r.key("threat_level"); r.value_string(threat_level);

                r.end_object();
            }
        }
    }

    r.end_array();
    r.key("faction_count"); r.value_int(faction_count_out);
    r.key("critical_threats"); r.value_int(critical_count);
    r.key("high_threats"); r.value_int(high_count);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// #112: live mobilized-soldier count for a CArmy.
//
// THE BUG: this query (and the raise_armies readback) used to report troop_count as
// `owner_link + 0x168` (army_owner_link::TROOP_COUNT). That field is NOT a soldier
// count — empirically it reads 0 for most armies and tiny ints (e.g. 3 for an army
// of 476 men in 6 regiments). A raised/marching army that DEMONSTRABLY fights would
// read troop_count:0 (Gen-16 Halfdan GHA), making the agent think mobilization broke.
//
// THE FIX: the real soldiers live in the army's regiment list. CArmy+0x18 (REG_VEC)
// is a uint32* array of CArmyRegiment ref-IDs, count at CArmy+0x20 (REG_COUNT, a
// slot/capacity count that over-counts — trailing IDs are 0). Each ID resolves via
// globals::ARMY_REGIMENT_DB (a DISTINCT DB from REGIMENT_DB/CRegiment); the regiment
// holds current (alive) men at +0x38 and max at +0x3c. Sum +0x38 over the live
// regiments = the army's true current strength. Validated against 407 armies in the
// 1097 core dump (cur<=max on every one; sane per-army totals 126..1697).
struct ArmyStrength { int32_t current = 0; int32_t max = 0; int32_t regiments = 0; };
static ArmyStrength army_regiment_strength(uintptr_t army_ptr) {
    ArmyStrength s;
    uintptr_t reg_vec = safe_read<uintptr_t>(army_ptr + ck3::army_obj::REG_VEC);
    int32_t reg_count = safe_read<int32_t>(army_ptr + ck3::army_obj::REG_COUNT);
    if (!reg_vec || reg_count <= 0 || reg_count > 5000) return s;
    for (int32_t j = 0; j < reg_count; ++j) {
        uint32_t rid = safe_read<uint32_t>(reg_vec + j * 4);
        if (rid == 0 || rid == 0xFFFFFFFF) continue;
        uintptr_t reg = ad_db_lookup(ck3::globals::ARMY_REGIMENT_DB, rid);
        if (!reg) continue;
        // ad_db_lookup masks to 24 bits; verify the full ID matches this slot.
        if (safe_read<uint32_t>(reg + ::army_regiment::ID) != rid) continue;
        s.current += safe_read<int32_t>(reg + ::army_regiment::MEN_CURRENT);
        s.max     += safe_read<int32_t>(reg + ::army_regiment::MEN_MAX);
        s.regiments++;
    }
    return s;
}

// query_schemes: active schemes for a character
// Round 6b: Army enumeration query — lists all armies owned by a character
bool do_query_armies(const char* params_json, std::string* out) {
    // L1b: lean-by-default — cap the emitted army rows and gate the secondary
    // per-army fields (special_status/transport_state/in_combat/is_retreating)
    // behind verbose. Lean mode keeps army_id/troop_count/commander_id.
    bool verbose = json_get_bool(params_json, "verbose", false);
    static constexpr int MAX_EMITTED_ARMIES = 50;
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_armies: character not found\n");
        return false;
    }
    uint32_t target_char_id = char_get_id(character);

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(target_char_id & 0xFFFFFF);

    // Round 6b: Army enumeration chain
    // FUN_06061870(GAME_STATE_ROOT) → sub-object, +0x50=army ID array, +0x5c=count
    using fn_army_enum = uintptr_t(*)(uintptr_t);
    auto army_enum = reinterpret_cast<fn_army_enum>(
        bridge::game_fn(ck3::funcs::ARMY_ENUM_SUB));
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::ARMY_ENUM_ENTRY);

    r.key("armies");
    r.begin_array();

    int army_count = 0;
    int64_t realm_troop_total = 0;  // #112: sum of live soldiers across this ruler's raised armies
    if (gs) {
        uintptr_t sub = army_enum(gs);
        if (sub) {
            uintptr_t id_array = safe_read<uintptr_t>(sub + ck3::army_enum::ID_ARRAY_PTR);
            int32_t id_count = safe_read<int32_t>(sub + ck3::army_enum::ID_ARRAY_COUNT);

            for (int32_t i = 0; i < id_count && i < 2000; ++i) {
                if (army_count >= MAX_EMITTED_ARMIES) break;
                uint32_t army_id = safe_read<uint32_t>(id_array + i * 4);
                if (army_id == 0 || army_id == 0xFFFFFFFF) continue;

                uintptr_t army_ptr = ad_db_lookup(ck3::globals::ARMY_DB, army_id);
                if (!army_ptr) continue;

                // Check owner via owner-link chain
                uint32_t link_id = safe_read<uint32_t>(army_ptr + ck3::army_obj::OWNER_LINK_ID);
                uintptr_t link_ptr = ad_db_lookup(ck3::globals::OWNER_LINK_DB, link_id);
                if (!link_ptr) continue;
                uint32_t owner_id = safe_read<uint32_t>(link_ptr + ck3::army_owner_link::OWNER_CHAR_ID);
                if (owner_id != target_char_id) continue;

                // #112: troop_count = SUM of current (alive) men across the army's
                // regiments (CArmy+0x18 list), NOT owner_link+0x168 (which read 0/tiny).
                ArmyStrength as = army_regiment_strength(army_ptr);
                realm_troop_total += as.current;

                r.begin_object();
                r.key("army_id");        r.value_uint(army_id & 0xFFFFFF);
                r.key("troop_count");    r.value_int(as.current);
                r.key("max_troop_count");r.value_int(as.max);
                if (verbose) {
                    r.key("regiment_count"); r.value_int(as.regiments);
                    r.key("special_status"); r.value_int(safe_read<uint8_t>(army_ptr + ck3::army_obj::SPECIAL_STATUS));
                    r.key("transport_state");r.value_int(safe_read<int32_t>(army_ptr + ck3::army_obj::TRANSPORT_STATE));

                    // Check combat/retreating status via function pointers
                    using fn_bool_army = bool(*)(uintptr_t);
                    auto in_combat = reinterpret_cast<fn_bool_army>(
                        bridge::game_fn(ck3::funcs::ARMY_IN_COMBAT));
                    auto is_retreating = reinterpret_cast<fn_bool_army>(
                        bridge::game_fn(ck3::funcs::ARMY_IS_RETREATING));
                    r.key("in_combat");    r.value_bool(in_combat(army_ptr));
                    r.key("is_retreating");r.value_bool(is_retreating(army_ptr));
                }

                // Commander: the army's own field. (This used to call
                // ARMY_GET_COMMANDER and keep its result as a u32 id, but that
                // function returns a SCOPE POINTER (FUN_06682250: [link+0x18] or
                // NoTargetScope), so the reported id was the low bits of a heap
                // address. Found by the Windows parity run, lane-p5.)
                uint32_t cmd_id = safe_read<uint32_t>(army_ptr + ck3::army_obj::COMMANDER_CHAR_ID);
                if (cmd_id != 0 && cmd_id != 0xFFFFFFFF) {
                    r.key("commander_id"); r.value_uint(cmd_id & 0xFFFFFF);
                }

                r.end_object();
                army_count++;
            }
        }
    }

    r.end_array();
    r.key("army_count"); r.value_int(army_count);
    // #112: realm total live soldiers across all enumerated raised armies (was
    // effectively 0 when sourced from owner_link+0x168).
    r.key("realm_troop_total"); r.value_int(static_cast<int32_t>(realm_troop_total));

    // Readiness summary: aggregate military stats from title_realm
    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    r.key("readiness_summary");
    r.begin_object();
    if (tr) {
        int32_t mil_current = safe_read<int32_t>(tr + ck3::title_realm::MIL_CURRENT);
        int32_t mil_max     = safe_read<int32_t>(tr + ck3::title_realm::MIL_MAX);
        int32_t own_domain  = safe_read<int32_t>(tr + ck3::title_realm::MIL_OWN_DOMAIN_MAX);
        r.key("total_current");  r.value_int(mil_current);
        r.key("total_max");      r.value_int(mil_max);
        r.key("own_domain_max"); r.value_int(own_domain);
        int pct = 0;
        if (mil_max > 0) {
            pct = static_cast<int>((static_cast<int64_t>(mil_current) * 100) / mil_max);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        r.key("readiness_pct"); r.value_int(pct);
        // Estimated days to full reinforcement (base 15% per month)
        int est_days = 0;
        if (mil_max > 0 && mil_current < mil_max) {
            est_days = static_cast<int>((static_cast<int64_t>(mil_max - mil_current) * 200) / mil_max);
        }
        r.key("est_days_to_full"); r.value_int(est_days);
    } else {
        r.key("total_current");  r.value_int(0);
        r.key("total_max");      r.value_int(0);
        r.key("own_domain_max"); r.value_int(0);
        r.key("readiness_pct");  r.value_int(0);
        r.key("est_days_to_full"); r.value_int(0);
    }
    r.end_object();

    r.end_object();

    if (out) *out = r.take();
    return true;
}

// FIX #scheme_legibility: resolve a char id -> display name (empty if unresolvable).
static std::string scheme_resolve_name(uint32_t id) {
    if (id == 0 || id == 0xFFFFFF) return {};
    void* c = lookup_character(id);
    if (!c || is_sentinel(c)) return {};
    return ad_read_char_name(reinterpret_cast<uintptr_t>(c));
}

// FIX #scheme_legibility: friendly Title-Cased label from a raw scheme type key.
// Pure string transform (no memory reads). Irregular overrides + default underscore->space
// + word capitalization.
static std::string scheme_type_label(const std::string& key) {
    if (key.empty()) return {};
    static const std::map<std::string, std::string> overrides = {
        {"murder", "Murder"}, {"abduct", "Abduction"}, {"sway", "Sway"},
        {"befriend", "Befriend"}, {"seduce", "Seduce"}, {"elope", "Elope"},
        {"fabricate_hook", "Fabricate Hook"}, {"claim_throne", "Claim Throne"},
    };
    auto it = overrides.find(key);
    if (it != overrides.end()) return it->second;
    std::string out;
    bool cap = true;
    for (char ch : key) {
        if (ch == '_') { out += ' '; cap = true; }
        else if (cap)  { out += static_cast<char>(toupper((unsigned char)ch)); cap = false; }
        else           { out += ch; }
    }
    return out;
}

// FIX #scheme_legibility: 3-level danger string from a scheme type key substring test.
// Returns "lethal" (murder), "high" (abduct/imprison/claim_throne), "moderate" (hook),
// else "low". dangerous = (level != "low").
static const char* scheme_danger_level(const std::string& s) {
    if (s.find("murder") != std::string::npos) return "lethal";
    if (s.find("abduct") != std::string::npos || s.find("imprison") != std::string::npos ||
        s.find("claim_throne") != std::string::npos) return "high";
    if (s.find("hook") != std::string::npos) return "moderate";
    return "low";
}

// FIX #scheme_legibility: best-effort ETA in days. SUCCESS_GROWTH (+0x1D8, int64 fixed-point
// /100000) is the per-monthly-tick progress growth. Returns -1 (omit) when growth<=0 or
// already complete. ADVISORY (MEDIUM): the inner formula (FUN_063c64d0) is not decompiled.
static int scheme_eta_days(uintptr_t scheme_ptr, int32_t progress, int32_t max_prog) {
    if (max_prog <= 0 || progress >= max_prog) return -1;
    int64_t growth_raw = safe_read<int64_t>(scheme_ptr + ck3::active_scheme::SUCCESS_GROWTH);
    double growth = static_cast<double>(growth_raw) / 100000.0;
    if (growth <= 0.0) return -1;
    double remaining = static_cast<double>(max_prog - progress);
    int eta_months = static_cast<int>(std::ceil(remaining / growth));
    if (eta_months < 0) return -1;
    return eta_months * 30;
}

// Round 10-8: Per-character scheme query via core_data + 0x98 ref list
// Resolves packed ref IDs through the scheme RefDB lookup table
// R10-8 fixes: type via CSchemeType* at +0x18, target at +0x28/+0x2C, status at +0x20
bool do_query_schemes(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_schemes: character not found\n");
        return false;
    }
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;

    // Get core_data (char + 0x1a8)
    uintptr_t core = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::REALM_DATA);
    if (!core || !is_mapped(core)) {
        ResponseBuilder r;
        r.begin_object();
        r.key("character_id"); r.value_uint(char_id);
        r.key("total_schemes"); r.value_int(0);
        r.key("schemes"); r.begin_array(); r.end_array();
        r.key("count"); r.value_int(0);
        r.key("status"); r.value_string("ok");
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    // Per-character scheme ref list: core_data + 0x98 (R7-3)
    uintptr_t ref_array_ptr = safe_read<uintptr_t>(core + ck3::core_data::SCHEME_REF_PTR);
    int32_t ref_count = safe_read<int32_t>(core + ck3::core_data::SCHEME_REF_COUNT);
    if (ref_count < 0) ref_count = 0;
    if (ref_count > 200) ref_count = 200;
    if (ref_array_ptr && !is_mapped(ref_array_ptr)) ref_array_ptr = 0;

    // Get the scheme RefDB for resolving ref IDs
    uintptr_t refDB = safe_read_offset<uintptr_t>(ck3::globals::SCHEME_REFDB);
    bool refDB_ok = (refDB && is_mapped(refDB));
    uintptr_t lookup_table = refDB_ok ? safe_read<uintptr_t>(refDB + ck3::hash_map::TABLE_PTR) : 0;
    uint32_t max_idx = refDB_ok ? safe_read<uint32_t>(refDB + ck3::hash_map::CAPACITY) : 0;
    if (lookup_table && !is_mapped(lookup_table)) lookup_table = 0;

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_id);
    r.key("total_schemes"); r.value_int(ref_count);
    r.key("schemes"); r.begin_array();

    int emitted = 0;

    for (int i = 0; i < ref_count && ref_array_ptr && lookup_table; ++i) {
        uint32_t ref = safe_read<uint32_t>(ref_array_ptr + static_cast<uintptr_t>(i) * 4);
        uint32_t idx = ref & 0xFFFFFF;
        if (idx >= max_idx) continue;

        uintptr_t scheme_ptr = safe_read<uintptr_t>(lookup_table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
        if (!scheme_ptr || !is_mapped(scheme_ptr)) continue;

        // Validate scheme ref (masked 24-bit comparison)
        uint32_t scheme_id = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::SCHEME_ID);
        if ((scheme_id & 0xFFFFFF) != idx) continue;

        // Check status (R10-8: 0=active, 1=invalidated)
        int32_t status = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::STATUS);
        if (status == 1) continue;  // skip invalidated schemes

        // Read scheme fields with corrected R10-8 offsets
        uint32_t owner_id  = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::OWNER_ID) & 0xFFFFFF;
        int32_t  success   = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::SUCCESS_CHANCE);
        int32_t  progress  = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::PROGRESS);
        int32_t  max_prog  = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::MAX_PROGRESS);
        int32_t  agents    = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::AGENT_SLOTS_COUNT);
        uint8_t  is_frozen = safe_read<uint8_t>(scheme_ptr + ck3::active_scheme::IS_FROZEN);

        // Resolve scheme type name via CSchemeType* at +0x18 (R10-8)
        const char* type_name = nullptr;
        size_t type_name_len = 0;
        uintptr_t type_ptr = safe_read<uintptr_t>(scheme_ptr + ck3::active_scheme::SCHEME_TYPE_PTR);
        if (type_ptr && is_mapped(type_ptr)) {
            read_db_obj_name(type_ptr, type_name, type_name_len);
        }

        // Resolve target (R10-8: target_type at +0x28, target_ref at +0x2C)
        int32_t target_type = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::TARGET_TYPE);
        uint32_t target_ref_raw = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::TARGET_REF);
        uint32_t target_id = 0;
        bool has_target = false;
        if (target_type == 0 && target_ref_raw != 0 && target_ref_raw != 0xFFFFFFFF) {
            target_id = target_ref_raw & 0xFFFFFF;
            has_target = true;
        }

        // FIX #scheme_legibility: richer additive fields.
        std::string type_key = (type_name && type_name_len) ? std::string(type_name, type_name_len) : std::string();
        std::string owner_name = scheme_resolve_name(owner_id);
        int32_t progress_pct = (max_prog > 0)
            ? std::max(0, std::min(100, (int)((int64_t)progress * 100 / max_prog))) : 0;
        int32_t discovery_timer = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::DISCOVERY_TIMER);
        int32_t exposure_timer  = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::EXPOSURE_TIMER);
        int64_t start_date_raw  = safe_read<int64_t>(scheme_ptr + ck3::active_scheme::START_DATE);
        int eta = scheme_eta_days(scheme_ptr, progress, max_prog);

        r.begin_object();
        r.key("scheme_id"); r.value_uint(ref & 0xFFFFFF);
        r.key("owner_id");  r.value_uint(owner_id);
        if (!owner_name.empty()) { r.key("owner_name"); r.value_string(owner_name.c_str(), owner_name.size()); }
        if (has_target) {
            r.key("target_id"); r.value_uint(target_id);
            std::string tn = scheme_resolve_name(target_id);
            if (!tn.empty()) { r.key("target_name"); r.value_string(tn.c_str(), tn.size()); }
        } else if (target_type != 4 && target_type != 0) {
            r.key("target_type"); r.value_int(target_type);
            r.key("target_ref"); r.value_uint(target_ref_raw & 0xFFFFFF);
        } else {
            r.key("target_id"); r.value_null();
        }
        r.key("success_chance"); r.value_int(success);
        r.key("progress");  r.value_int(progress);
        r.key("max_progress"); r.value_int(max_prog);
        r.key("progress_pct"); r.value_int(progress_pct);
        r.key("phase"); r.value_string(progress_pct >= 100 ? "ready" : "building");  // derived, not an engine field
        r.key("agents");    r.value_int(agents);   // agent slots filled
        r.key("is_frozen"); r.value_bool(is_frozen != 0);
        r.key("discovery_timer"); r.value_int(discovery_timer);
        r.key("exposure_timer"); r.value_int(exposure_timer);
        r.key("is_discovered"); r.value_bool(discovery_timer <= 0);
        if (eta >= 0) { r.key("eta_days"); r.value_int(eta); }  // advisory; omitted when growth<=0
        if (start_date_raw != 0) { r.key("start_date"); r.value_int(start_date_raw); }
        if (type_name && type_name_len > 0) {
            r.key("type"); r.value_string(type_name, type_name_len);
            std::string lbl = scheme_type_label(type_key);
            if (!lbl.empty()) { r.key("scheme_type_label"); r.value_string(lbl.c_str(), lbl.size()); }
        } else {
            r.key("type"); r.value_string("unknown");
        }
        r.end_object();
        ++emitted;
    }

    r.end_array();
    r.key("count"); r.value_int(emitted);
    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_incoming_schemes: the schemes TARGETING this character — i.e. the plots
// against me (murder, abduct, fabricate_hook, claim_throne, ...). Near-clone of
// do_query_schemes: the only structural change is the list offset
// (INCOMING_SCHEME_REF_PTR 0xB0, sibling of the owned list at 0x98 — validated
// HIGH, re_incoming_schemes.md). For the incoming view OWNER_ID is the
// load-bearing field (the schemer/threat). There is no confirmed hostility byte,
// so we flag danger by scheme-type NAME (murder/abduct/fabricate_hook/
// claim_throne = dangerous) rather than asserting an unverified flag.
bool do_query_incoming_schemes(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_incoming_schemes: character not found\n");
        return false;
    }
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;

    uintptr_t core = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::REALM_DATA);
    if (!core || !is_mapped(core)) {
        ResponseBuilder r; r.begin_object();
        r.key("character_id"); r.value_uint(char_id);
        r.key("total_schemes"); r.value_int(0);
        r.key("incoming_schemes"); r.begin_array(); r.end_array();
        r.key("count"); r.value_int(0);
        r.key("dangerous_count"); r.value_int(0);
        r.key("status"); r.value_string("ok");
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    // INCOMING (targeted-by) scheme ref list: core_data + 0xB0.
    uintptr_t ref_array_ptr = safe_read<uintptr_t>(core + ck3::core_data::INCOMING_SCHEME_REF_PTR);
    int32_t ref_count = safe_read<int32_t>(core + ck3::core_data::INCOMING_SCHEME_REF_COUNT);
    if (ref_count < 0) ref_count = 0;
    if (ref_count > 200) ref_count = 200;
    if (ref_array_ptr && !is_mapped(ref_array_ptr)) ref_array_ptr = 0;

    uintptr_t refDB = safe_read_offset<uintptr_t>(ck3::globals::SCHEME_REFDB);
    bool refDB_ok = (refDB && is_mapped(refDB));
    uintptr_t lookup_table = refDB_ok ? safe_read<uintptr_t>(refDB + ck3::hash_map::TABLE_PTR) : 0;
    uint32_t max_idx = refDB_ok ? safe_read<uint32_t>(refDB + ck3::hash_map::CAPACITY) : 0;
    if (lookup_table && !is_mapped(lookup_table)) lookup_table = 0;

    ResponseBuilder r; r.begin_object();
    r.key("character_id"); r.value_uint(char_id);
    r.key("total_schemes"); r.value_int(ref_count);
    r.key("incoming_schemes"); r.begin_array();

    int emitted = 0, dangerous = 0;
    for (int i = 0; i < ref_count && ref_array_ptr && lookup_table; ++i) {
        uint32_t ref = safe_read<uint32_t>(ref_array_ptr + static_cast<uintptr_t>(i) * 4);
        uint32_t idx = ref & 0xFFFFFF;
        if (idx >= max_idx) continue;
        uintptr_t scheme_ptr = safe_read<uintptr_t>(lookup_table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
        if (!scheme_ptr || !is_mapped(scheme_ptr)) continue;
        uint32_t scheme_id = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::SCHEME_ID);
        if ((scheme_id & 0xFFFFFF) != idx) continue;
        int32_t status = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::STATUS);
        if (status == 1) continue;  // invalidated

        uint32_t owner_id = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::OWNER_ID) & 0xFFFFFF;  // the SCHEMER
        int32_t  success  = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::SUCCESS_CHANCE);
        int32_t  progress = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::PROGRESS);
        int32_t  max_prog = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::MAX_PROGRESS);

        const char* type_name = nullptr; size_t type_name_len = 0;
        uintptr_t type_ptr = safe_read<uintptr_t>(scheme_ptr + ck3::active_scheme::SCHEME_TYPE_PTR);
        if (type_ptr && is_mapped(type_ptr)) read_db_obj_name(type_ptr, type_name, type_name_len);
        // FIX #scheme_legibility: 3-level danger + dangerous = (level != "low").
        std::string type_key = (type_name && type_name_len) ? std::string(type_name, type_name_len) : std::string();
        const char* danger_level = scheme_danger_level(type_key);
        bool danger = (std::string(danger_level) != "low");
        if (danger) dangerous++;

        std::string schemer_name = scheme_resolve_name(owner_id);
        int32_t progress_pct = (max_prog > 0)
            ? std::max(0, std::min(100, (int)((int64_t)progress * 100 / max_prog))) : 0;
        int32_t discovery_timer = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::DISCOVERY_TIMER);
        int eta = scheme_eta_days(scheme_ptr, progress, max_prog);

        r.begin_object();
        r.key("scheme_id"); r.value_uint(ref & 0xFFFFFF);
        r.key("schemer_id"); r.value_uint(owner_id);   // who is plotting against me
        if (!schemer_name.empty()) { r.key("schemer_name"); r.value_string(schemer_name.c_str(), schemer_name.size()); }
        r.key("type"); r.value_string(type_name && type_name_len ? type_name : "unknown",
                                       type_name && type_name_len ? type_name_len : 7);
        if (!type_key.empty()) {
            std::string lbl = scheme_type_label(type_key);
            if (!lbl.empty()) { r.key("scheme_type_label"); r.value_string(lbl.c_str(), lbl.size()); }
        }
        r.key("dangerous"); r.value_bool(danger);
        r.key("danger"); r.value_string(danger_level);
        r.key("success_chance"); r.value_int(success);
        r.key("progress"); r.value_int(progress);
        r.key("max_progress"); r.value_int(max_prog);
        r.key("progress_pct"); r.value_int(progress_pct);
        r.key("is_discovered"); r.value_bool(discovery_timer <= 0);
        if (eta >= 0) { r.key("eta_days"); r.value_int(eta); }
        r.end_object();
        ++emitted;
    }

    r.end_array();
    r.key("count"); r.value_int(emitted);
    r.key("dangerous_count"); r.value_int(dangerous);
    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_lifestyle: perks and XP for a character
bool do_query_lifestyle(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_lifestyle: character not found\n");
        return false;
    }

    uintptr_t core = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::REALM_DATA);
    if (!core) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_lifestyle: core_data is null\n");
        return false;
    }

    // Read lifestyle XP map: core_data+0x208 is ptr to array of 16B entries
    // Each entry: {lifestyle_ptr(8B), xp_int64(8B)}
    // Count at core_data+0x214
    uintptr_t xp_arr = safe_read<uintptr_t>(core + ck3::lifestyle::LIFESTYLE_XP_ARRAY);
    int32_t xp_count = safe_read<int32_t>(core + ck3::lifestyle::LIFESTYLE_XP_COUNT);
    if (xp_count < 0) xp_count = 0;
    if (xp_count > 50) xp_count = 50;  // safety cap (5 lifestyles max, but be generous)

    // Read perk list: ptr at core_data+0x220, count at core_data+0x22C
    uintptr_t perk_arr = safe_read<uintptr_t>(core + ck3::lifestyle::PERK_LIST_PTR);
    int32_t perk_count  = safe_read<int32_t>(core + ck3::lifestyle::PERK_LIST_COUNT);
    if (perk_count < 0) perk_count = 0;
    if (perk_count > 500) perk_count = 500;  // safety cap

    ResponseBuilder r;
    r.begin_object();

    // Emit per-lifestyle XP as a map
    r.key("lifestyle_xp"); r.begin_object();
    int64_t total_xp = 0;
    if (xp_arr && xp_count > 0) {
        for (int i = 0; i < xp_count; ++i) {
            uintptr_t entry_base = xp_arr + static_cast<uintptr_t>(i) * 16;
            uintptr_t lifestyle_ptr = safe_read<uintptr_t>(entry_base);
            int64_t xp_value = safe_read<int64_t>(entry_base + 8);
            if (!lifestyle_ptr) continue;

            const char* name = nullptr;
            size_t name_len = 0;
            if (read_db_obj_name(lifestyle_ptr, name, name_len)) {
                // name from read_db_obj_name is in a static buffer, already null-terminated
                r.key(name);
            } else {
                r.key("unknown");
            }
            // XP is stored as fixed-point (multiplied by 100000 internally), convert to display value
            r.value_int(static_cast<int32_t>(xp_value / 100000));
            total_xp += xp_value;
        }
    }
    r.end_object();
    r.key("total_xp"); r.value_int(static_cast<int32_t>(total_xp / 100000));

    r.key("perks"); r.begin_array();

    int emitted = 0;
    if (perk_arr && perk_count > 0) {
        for (int i = 0; i < perk_count; ++i) {
            uintptr_t perk_ptr = safe_read<uintptr_t>(perk_arr + static_cast<uintptr_t>(i) * 8);
            if (!perk_ptr) continue;

            const char* name = nullptr;
            size_t name_len = 0;
            if (read_db_obj_name(perk_ptr, name, name_len)) {
                r.value_string(name, name_len);
            } else {
                r.value_string("unknown");
            }
            ++emitted;
        }
    }

    r.end_array();
    r.key("perk_count"); r.value_int(emitted);

    // Current focus (core_data + 0x280 → CActiveFocus)
    uintptr_t active_focus_ptr = safe_read<uintptr_t>(core + ck3::lifestyle::ACTIVE_FOCUS_PTR);
    if ((active_focus_ptr && is_mapped(active_focus_ptr))) {
        uintptr_t focus_type_ptr = safe_read<uintptr_t>(active_focus_ptr + ck3::active_focus::FOCUS_TYPE_PTR);
        if ((focus_type_ptr && is_mapped(focus_type_ptr))) {
            std::string focus_name = ad_read_std_string(focus_type_ptr + ck3::db_object::NAME_KEY);
            if (!focus_name.empty()) {
                r.key("current_focus"); r.value_string(focus_name.c_str(), focus_name.size());
            } else {
                r.key("current_focus"); r.value_null();
            }
        } else {
            r.key("current_focus"); r.value_null();
        }
        int64_t date_set = safe_read<int64_t>(active_focus_ptr + ck3::active_focus::DATE_SET);
        r.key("focus_date_set"); r.value_int(static_cast<int32_t>(date_set));
    } else {
        r.key("current_focus"); r.value_null();
    }

    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_interaction_acceptance: check if a specific target would accept an
// interaction from a specific actor.
//
// This is a DEFERRED QUERY. AI_INTERACTION_DECISION requires TLS and engine
// state that's only safe to access from the game thread. The IPC thread
// fills in the request, the game thread evaluates it during the next
// drain_and_execute_intents() call (post-tick serial section), and we poll
// for the result here.
//
// The score hook (g_force_target_id) forces AI_INTERACTION_DECISION to
// evaluate our specific target rather than letting it iterate all candidates.
// If it returns true, the interaction is valid AND the target accepts.
//
// Required params: interaction_type, target_id (or recipient_id)
// Optional params: actor_id (defaults to controlled character)
bool do_query_interaction_acceptance(const char* params_json, std::string* out) {
    const char* type = json_get_string(params_json, "interaction_type");
    if (!type) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_interaction_acceptance: missing interaction_type\n");
        if (out) *out = "{\"error\":\"missing_interaction_type\"}";
        return false;
    }

    // Accept both "target_id" and "recipient_id" for the target
    int target_id = json_get_int(params_json, "target_id");
    if (target_id <= 0) target_id = json_get_int(params_json, "recipient_id");
    if (target_id <= 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_interaction_acceptance: missing target_id/recipient_id\n");
        if (out) *out = "{\"error\":\"missing_target_id\",\"hint\":\"Provide target_id or recipient_id\"}";
        return false;
    }

    // Resolve actor_id (defaults to controlled character)
    int actor_id = json_get_int(params_json, "actor_id");
    if (actor_id <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id <= 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_interaction_acceptance: no actor available\n");
        if (out) *out = "{\"error\":\"no_actor_available\"}";
        return false;
    }

    // Check if a query is already in flight
    int current_state = g_acceptance_query.state.load(std::memory_order_acquire);
    if (current_state == 1 || current_state == 4) {
        // Another query is pending or in-progress
        if (out) *out = "{\"error\":\"acceptance_query_busy\","
                        "\"hint\":\"Another acceptance query is already pending. Wait for it to complete.\"}";
        return true;  // not a hard error, just busy
    }

    // If there's a completed result from a previous query, check if it matches
    // our request (could be a poll for our own previous query)
    if (current_state == 2 || current_state == 3) {
        // Check if this matches the pending query's params
        bool matches = (g_acceptance_query.actor_id == static_cast<uint32_t>(actor_id) &&
                       g_acceptance_query.target_id == static_cast<uint32_t>(target_id) &&
                       strcmp(g_acceptance_query.interaction_type, type) == 0);

        if (matches) {
            // Return the result and clear the slot
            ResponseBuilder r;
            r.begin_object();
            r.key("action"); r.value_string("query_interaction_acceptance");
            r.key("interaction_type"); r.value_string(type);
            r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
            r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);

            if (current_state == 3) {
                r.key("valid"); r.value_bool(g_acceptance_query.valid);
                r.key("would_accept"); r.value_bool(false);
                r.key("error"); r.value_string(g_acceptance_query.error);
            } else {
                r.key("valid"); r.value_bool(g_acceptance_query.valid);
                r.key("would_accept"); r.value_bool(g_acceptance_query.would_accept);
            }
            r.key("deferred"); r.value_bool(true);
            r.key("status"); r.value_string("ok");
            r.end_object();
            if (out) *out = r.take();

            // Clear the slot for the next query
            g_acceptance_query.state.store(0, std::memory_order_release);
            return true;
        } else {
            // Previous result doesn't match — clear it and submit a new query
            g_acceptance_query.state.store(0, std::memory_order_release);
        }
    }

    // Submit a new deferred query
    g_acceptance_query.actor_id = static_cast<uint32_t>(actor_id);
    g_acceptance_query.target_id = static_cast<uint32_t>(target_id);
    strncpy(g_acceptance_query.interaction_type, type,
            sizeof(g_acceptance_query.interaction_type) - 1);
    g_acceptance_query.interaction_type[sizeof(g_acceptance_query.interaction_type) - 1] = '\0';
    g_acceptance_query.valid = false;
    g_acceptance_query.would_accept = false;
    g_acceptance_query.error[0] = '\0';

    // Publish the query (game thread will pick it up next tick)
    g_acceptance_query.state.store(1, std::memory_order_release);

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_interaction_acceptance: "
                  "deferred query submitted: %s actor=%d target=%d\n",
                  type, actor_id, target_id);

    // Wait for the game thread to process it (up to ~2 seconds, polling every 50ms)
    // This blocks the IPC handler but that's acceptable for a single-agent setup.
    for (int i = 0; i < 40; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int st = g_acceptance_query.state.load(std::memory_order_acquire);
        if (st == 2 || st == 3) {
            // Result ready — return it
            ResponseBuilder r;
            r.begin_object();
            r.key("action"); r.value_string("query_interaction_acceptance");
            r.key("interaction_type"); r.value_string(type);
            r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
            r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);

            if (st == 3) {
                r.key("valid"); r.value_bool(g_acceptance_query.valid);
                r.key("would_accept"); r.value_bool(false);
                r.key("error"); r.value_string(g_acceptance_query.error);
            } else {
                r.key("valid"); r.value_bool(g_acceptance_query.valid);
                r.key("would_accept"); r.value_bool(g_acceptance_query.would_accept);
            }
            r.key("deferred"); r.value_bool(true);
            r.key("wait_ms"); r.value_int((i + 1) * 50);
            r.key("status"); r.value_string("ok");
            r.end_object();
            if (out) *out = r.take();

            // Clear the slot
            g_acceptance_query.state.store(0, std::memory_order_release);
            return true;
        }
    }

    // Timeout — query is still pending
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_interaction_acceptance");
    r.key("interaction_type"); r.value_string(type);
    r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);
    r.key("error"); r.value_string("timeout_waiting_for_game_thread");
    r.key("hint"); r.value_string("Game must be unpaused for at least one tick. "
                                   "The query runs during drain_and_execute_intents on the game thread.");
    r.key("deferred"); r.value_bool(true);
    r.key("status"); r.value_string("timeout");
    r.end_object();
    if (out) *out = r.take();

    return true;
}

bool do_get_tick_info(const char* /*params_json*/, std::string* out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("get_tick_info");

    // Tick count
    uint64_t tick = g_tick_id.load(std::memory_order_relaxed);
    r.key("tick_id"); r.value_uint(tick);

    // Session state
    r.key("session_state"); r.value_string(session_state_string());

    // Game date (same logic as observation.cpp build_game_date)
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (gs) {
        int32_t raw_date = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
        // Raw date is in sub-day ticks (24 per day) since DATE_EPOCH. ck-05mg:
        // rendered through the shared real-calendar helper — this is the field
        // the meter lane copies verbatim as the paper's time axis.
        int year = 0, month = 0, day = 0;
        ck3::game_date::raw_to_ymd(raw_date, &year, &month, &day);
        char date_str[32];
        snprintf(date_str, sizeof(date_str), "%04d.%02d.%02d", year, month, day);
        r.key("game_date"); r.value_string(date_str);
        r.key("game_year"); r.value_int(year);

        // Game speed from game_state + GAME_SPEED (0-indexed: 0-4 stored, 1-5
        // display). ck-k57a: the `+ 1` is applied ONLY on a read that actually
        // happened. `safe_read` returns 0 on a fault, so the old
        // `value_int(speed_raw + 1)` reported a memory fault as a legal
        // "speed 1" — see bridge/include/speed_read.h for the full shape.
        // The fault counter that makes this decidable already existed; this
        // site simply never consulted it.
        const int fault_before = safe_read_fault_count();
        int32_t speed_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_SPEED);
        const bool speed_faulted = safe_read_fault_count() != fault_before;
        bridge::speed::emit(r, bridge::speed::classify(true, speed_faulted, speed_raw));
    } else {
        r.key("game_date"); r.value_string("unknown");
        r.key("game_year"); r.value_int(0);
        // Root unreadable: still a typed ABSENCE, not the integer 0. The 0 that
        // consumers have routed to abstain since ck-u2e6 is preserved on
        // `game_speed_legacy` for anything that predates `speed_read_status`.
        bridge::speed::emit(r, bridge::speed::classify(false, false, 0));
    }

    // Pause state — delegate to is_paused() (event_handler.cpp) which uses get_idler()
    r.key("paused"); r.value_bool(is_paused());

    // Pause sub-counts for diagnostics (event-pause, date-pause)
    uintptr_t gs2 = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (gs2) {
        r.key("event_pause_count"); r.value_int(safe_read<int32_t>(gs2 + ck3::game_state::EVENT_PAUSE_COUNT));
        r.key("date_pause_count"); r.value_int(safe_read<int32_t>(gs2 + ck3::game_state::DATE_PAUSE_COUNT));
    }

    // Controlled character info
    uint32_t ctrl_id = g_controlled_primary.load(std::memory_order_relaxed);
    r.key("controlled_character_id"); r.value_uint(ctrl_id);
    // rc2 B6: why no ruler is bound, when the bridge itself unbound one
    // ("heir_is_player": the human inherited the seat's ruler). "" otherwise.
    {
        const std::string why = ctrl_id == 0 ? g_session_registry.first_unbound_reason()
                                             : std::string();
        r.key("controlled_unbound_reason"); r.value_string(why.c_str());
    }
    r.key("agent_mode"); r.value_bool(g_agent_mode.load(std::memory_order_relaxed));

    // Controlled-ruler liveness. controlled_character_id does NOT auto-update
    // when the steered ruler dies — it keeps reporting the dead id until the
    // agent calls control(select <heir>). The per-turn state hook's dead-ruler
    // SUCCESSION alert depends on an `alive` flag, but the full character
    // profile (tier-2) is only read while PAUSED — so when the game runs
    // unpaused (speed 5) the death goes unnoticed and the agent steers a
    // corpse. Surface liveness here (tier-1, always available paused OR
    // unpaused) so the alert fires regardless of pause state. (live-run L25.)
    if (ctrl_id != 0 && g_char_is_alive) {
        void* cc = lookup_character(ctrl_id);
        if (cc) {
            bool ctrl_alive = true;
            int sig = sigsetjmp(g_recovery_point, 1);
            if (sig == 0) {
                ctrl_alive = g_char_is_alive(cc);
            } else {
                ctrl_alive = true;  // default to alive on crash (fail-safe)
            }
            r.key("controlled_alive"); r.value_bool(ctrl_alive);
        }
    }

    // Last successor resolved by the on_tick dead-ruler handoff (primary-title
    // current-holder). When auto-handoff is active this equals controlled_character_id
    // immediately after a succession (control already followed the heir). 0 = no
    // handoff has occurred this session. Lets the agent/monitor confirm the reign
    // followed the dynasty (and, in any expose-only fallback, recover with a single
    // set_controlled_character(controlled_heir_id)).
    uint32_t heir_id = g_controlled_heir.load(std::memory_order_relaxed);
    r.key("controlled_heir_id"); r.value_uint(heir_id);

    // Bridge toggles needed for per-scenario isolation (ck-fysa.5).
    // No symmetric getter exists for set_auto_respond_events / toggle_all_ai;
    // exposing them here keeps the harness on a single snapshot endpoint.
    r.key("auto_respond_events");
    r.value_bool(g_auto_respond_events.load(std::memory_order_relaxed));
    uint8_t ai_toggle = safe_read<uint8_t>(g_base_addr + ck3::globals::AI_GLOBAL_TOGGLE);
    r.key("all_ai_enabled"); r.value_bool(ai_toggle != 0);

    // ck-6kvq.9 / §2.10: pipeline state is published HERE and nowhere else.
    // get_tick_info is Tier-1, always safe, and already the universal liveness
    // endpoint — so the self-heal ladder, the overlay poller, the harness and
    // the agent all learn the pipeline for free. Always emitted, including in
    // legacy mode (where mode=="legacy" and every counter is 0): a client must
    // be able to tell which mode it is talking to without a second call.
    bridge::pipeline_emit_state(r);

    // ck-6kvq.13 / R6: the decision budget rides along on the same Tier-1
    // endpoint, for the same reason — every watchdog, the harness and the
    // agent learn whether a session is spent without a second call.
    bridge::budget_emit_state(r);

    // ck-9fc3 / R7: the save/world IDENTITY and the personality-lock restore
    // decision, published HERE for the same reason the pipeline state is —
    // get_tick_info is Tier-1, always safe, and already the universal liveness
    // endpoint. `save_identity.resume_gap` is what a harness samples to record
    // the resume-to-steer gap: how many game-days an episode ran with its
    // steering quarantined and nobody vouching for the world.
    r.key("save_identity"); r.value_raw(bridge::world_identity_emit_json());

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// get_ai_personality — reads 10 personality axis values for a character
// =============================================================================

// =============================================================================
// get_ai_evaluations — read-only memory mining of AI-relevant strategic context
//
// REWRITTEN: No game function calls. Pure pointer-chasing memory reads only.
// Assembles strategic context from existing game state:
//   1. AI personality axes (core_data+0x308)
//   2. AI controller tick counters (ai_ctrl+0x268..0x284)
//   3. Resources (gold, prestige, piety, income)
//   4. Military strength
//   5. Active wars summary
//   6. Neighbor power comparison (top 20 landed rulers with power ratios)
//   7. Alliance/truce counts from diplomatic data
//   8. Claim count
// =============================================================================

// B1 (20260621): forward decl of the capital-centroid helper (defined @~8375)
// so do_get_ai_evaluations can filter nearby_rulers by geography. Fail-open:
// returns false (candidate dropped) if any pointer hop is null/unmapped.
static bool mc_capital_xy(uintptr_t c, int32_t& out_x, int32_t& out_y);

bool do_get_ai_evaluations(const char* params_json, std::string* out) {
    // ck-86ue.2: All derived-pointer hops guarded with is_mapped() in addition
    // to the safe_read<T>() reads. Previous inline bounds checks
    // (`ptr > 0x10000 && ptr < 0x7FFFFFFFFFFF`) were equivalent low-bound
    // rejection but did not consult /proc/self/maps; is_mapped() does.
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        ADLOG("get_ai_evaluations: character not found\n");
        if (out) *out = R"({"success":false,"action":"get_ai_evaluations","reason":"character_not_found"})";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;
    ADLOG("get_ai_evaluations: reading strategic context for character %u\n", char_id);

    // Resolve sub-pointers — zero out anything that isn't actually mapped so
    // downstream "if (ptr)" predicates uniformly mean "valid".
    uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);      // 0x1a8
    uintptr_t title_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA); // 0x1b8
    uintptr_t diplo_data = safe_read<uintptr_t>(c + ck3::character::DIPLOMATIC_DATA);  // 0x1c0
    if (core_data && !is_mapped(core_data)) core_data = 0;
    if (title_data && !is_mapped(title_data)) title_data = 0;
    if (diplo_data && !is_mapped(diplo_data)) diplo_data = 0;

    if (!core_data) {
        ADLOG("get_ai_evaluations: character %u has no core_data (unlanded?)\n", char_id);
        if (out) *out = R"({"success":false,"action":"get_ai_evaluations","reason":"character_unlanded"})";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("action"); r.value_string("get_ai_evaluations");
    r.key("character_id"); r.value_uint(char_id);

    // -------------------------------------------------------------------------
    // 1. AI Personality (10 signed bytes at personality_ptr)
    // -------------------------------------------------------------------------
    static const char* axis_names[] = {
        "honor", "greed", "rationality", "energy", "boldness",
        "zeal", "vengefulness", "compassion", "sociability", "dominance"
    };
    uintptr_t personality_ptr = safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR);
    r.key("personality"); r.begin_object();
    if (personality_ptr && is_mapped(personality_ptr)) {
        // Check personality locks — if locked, report the locked value (what engine actually sees)
        PersonalityLock* pl_ptr = nullptr;
        {
            std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
            auto it = g_personality_locks.find(char_id);
            if (it != g_personality_locks.end()) pl_ptr = &it->second;
        }
        for (int i = 0; i < 10; i++) {
            int8_t val;
            if (pl_ptr && pl_ptr->locked[i]) {
                val = pl_ptr->axes[i];
            } else {
                val = safe_read<int8_t>(personality_ptr + i);
            }
            r.key(axis_names[i]); r.value_int(static_cast<int>(val));
        }
    }
    r.end_object();

    // -------------------------------------------------------------------------
    // 2. Resources
    // -------------------------------------------------------------------------
    double gold = static_cast<double>(safe_read<int64_t>(core_data + ck3::core_data::GOLD)) / 100000.0;
    double prestige = static_cast<double>(safe_read<int64_t>(core_data + ck3::core_data::PRESTIGE)) / 100000.0;
    double piety = static_cast<double>(safe_read<int64_t>(core_data + ck3::core_data::PIETY)) / 100000.0;
    double monthly_income = static_cast<double>(safe_read<int64_t>(core_data + ck3::core_data::MONTHLY_INCOME_FP)) / 100000.0;
    uint32_t stress = safe_read<uint32_t>(core_data + ck3::core_data::STRESS);

    r.key("gold"); r.value_float(gold);
    r.key("prestige"); r.value_float(prestige);
    r.key("piety"); r.value_float(piety);
    r.key("monthly_income"); r.value_float(monthly_income);
    r.key("stress"); r.value_uint(stress);

    // -------------------------------------------------------------------------
    // 3. Military strength
    // -------------------------------------------------------------------------
    int32_t mil_current = 0, mil_max = 0;
    if (title_data) {  // already is_mapped() at function head
        mil_current = safe_read<int32_t>(title_data + ck3::title_realm::MIL_CURRENT);
        mil_max = safe_read<int32_t>(title_data + ck3::title_realm::MIL_MAX);
    }
    r.key("military_strength"); r.value_int(mil_current);
    r.key("military_max"); r.value_int(mil_max);

    // -------------------------------------------------------------------------
    // 4. AI Controller tick counters (8 categories)
    // -------------------------------------------------------------------------
    uintptr_t ai_ctrl = safe_read<uintptr_t>(core_data + ck3::core_data::AI_CONTROLLER);
    r.key("ai_controller"); r.begin_object();
    if (ai_ctrl && is_mapped(ai_ctrl)) {
        uint8_t ai_enabled = safe_read<uint8_t>(ai_ctrl + ck3::ai_ctrl::AI_ENABLED);
        r.key("ai_enabled"); r.value_bool(ai_enabled != 0);

        r.key("tick_counters"); r.begin_array();
        for (int i = 0; i < 8; i++) {
            int32_t counter = safe_read<int32_t>(ai_ctrl + ck3::ai_ctrl::TICK_COUNTER_0 + i * 4);
            r.value_int(counter);
        }
        r.end_array();
    } else {
        r.key("ai_enabled"); r.value_bool(false);
        r.key("tick_counters"); r.begin_array(); r.end_array();
    }
    r.end_object();

    // -------------------------------------------------------------------------
    // 5. Active wars summary (from title_realm war ref array)
    // -------------------------------------------------------------------------
    r.key("active_wars"); r.begin_array();
    int war_count = 0;
    if (title_data) {  // already is_mapped() at function head
        uintptr_t war_array_base = title_data + ck3::title_realm::WAR_REF_ARRAY;
        uintptr_t war_ref_data = safe_read<uintptr_t>(war_array_base + 0x00);
        int32_t wcount = safe_read<int32_t>(war_array_base + 0x0C);
        if (wcount < 0 || wcount > 50) wcount = 0;
        if (war_ref_data && !is_mapped(war_ref_data)) { war_ref_data = 0; wcount = 0; }

        uintptr_t war_db = safe_read_offset<uintptr_t>(ck3::globals::WAR_DB);
        if (war_db && !is_mapped(war_db)) war_db = 0;

        for (int32_t i = 0; i < wcount; i++) {
            uint32_t war_ref = safe_read<uint32_t>(war_ref_data + i * 4);
            if (war_ref == 0 || war_ref == 0xFFFFFFFF) continue;

            // Resolve war ref via TPdxRefDatabase
            uintptr_t war_ptr = 0;
            if (war_db) {
                uint32_t idx = war_ref & 0xFFFFFF;
                uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                if (idx < capacity) {
                    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                    if (table && is_mapped(table)) {
                        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                        war_ptr = safe_read<uintptr_t>(entry);
                        // Verify war ref (masked 24-bit comparison)
                        if (war_ptr && is_mapped(war_ptr)) {
                            uint32_t stored_ref = safe_read<uint32_t>(war_ptr + 0x08);
                            if ((stored_ref & 0xFFFFFF) != idx) war_ptr = 0;
                        } else {
                            war_ptr = 0;
                        }
                    }
                }
            }
            if (!war_ptr) continue;

            // Skip ended wars
            uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
            if (is_ended != 0) continue;

            r.begin_object();
            uint32_t war_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
            r.key("war_id"); r.value_uint(war_id);

            // War name
            std::string war_name = ad_read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR);
            if (!war_name.empty()) {
                r.key("name"); r.value_string(war_name.c_str(), war_name.size());
            }

            // CB type name
            uintptr_t cb_type_ref = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TYPE_REF);
            if (cb_type_ref && is_mapped(cb_type_ref)) {
                std::string cb_name = ad_read_std_string(cb_type_ref + ck3::db_object::NAME_KEY);
                if (!cb_name.empty()) {
                    r.key("cb_type"); r.value_string(cb_name.c_str(), cb_name.size());
                }
            }

            // Primary attacker/defender
            uint32_t prim_atk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
            uint32_t prim_def = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
            r.key("primary_attacker"); r.value_uint(prim_atk);
            r.key("primary_defender"); r.value_uint(prim_def);

            // Determine our role
            bool is_attacker = false;
            uintptr_t atk_container = war_ptr + ck3::war_obj::ATK_PARTICIPANT;
            uintptr_t atk_arr = safe_read<uintptr_t>(atk_container);
            int32_t atk_count = safe_read<int32_t>(atk_container + 0x0C);
            if (atk_count > 0 && atk_count < 100 && atk_arr && is_mapped(atk_arr)) {
                for (int32_t j = 0; j < atk_count; j++) {
                    uintptr_t part = atk_arr + j * ck3::war_participant::ENTRY_SIZE;
                    uint32_t pid = safe_read<uint32_t>(part + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
                    if (pid == char_id) { is_attacker = true; break; }
                }
            }
            r.key("our_role"); r.value_string(is_attacker ? "attacker" : "defender");

            // Battle score from raw memory reads (safe from tick hook context).
            int bc = 0;
            double bs = compute_battle_score(war_ptr, &bc);
            r.key("battle_score"); r.value_int(static_cast<int>(bs));
            r.key("battle_count"); r.value_int(bc);

            // Ticking war score from raw memory reads (R24).
            int ts = compute_ticking_score(war_ptr);
            r.key("ticking_score"); r.value_int(ts);

            // War score: use cached breakdown if available, else estimate
            WarScoreBreakdown wbd{};
            if (query_war_score_breakdown(war_id, &wbd)) {
                r.key("war_score"); r.value_int(wbd.total);
                r.key("war_score_source"); r.value_string("computed", 8);
                r.key("score_breakdown"); r.begin_object();
                r.key("imprisonment"); r.value_int((wbd.imprisonment_atk + wbd.imprisonment_def) / 1000);
                r.key("battle"); r.value_int((wbd.battle_atk + wbd.battle_def) / 1000);
                r.key("occupation"); r.value_int((wbd.occupation_atk + wbd.occupation_def) / 1000);
                r.key("ticking"); r.value_int((wbd.ticking_atk + wbd.ticking_def) / 1000);
                r.key("landless"); r.value_int((wbd.landless_atk + wbd.landless_def) / 1000);
                r.end_object();
            } else {
                // Cache miss: clamp the (attacker-minus-defender) estimate to
                // the engine's ±100 demand-gating range instead of emitting an
                // uncapped value (consistent with emit_war_entry / build_wars).
                int est = static_cast<int>(bs) + ts;
                if (est > 100) est = 100; else if (est < -100) est = -100;
                r.key("war_score"); r.value_int(est);
                r.key("war_score_source"); r.value_string("computed_fallback", 17);
            }
            // Who is winning, and which side the scope character is on.
            {
                double adv = bs + ts;  // attacker-minus-defender (bridge.h convention)
                const char* fav = (adv > 0) ? "attacker" : (adv < 0 ? "defender" : "even");
                r.key("favored_side"); r.value_string(fav, strlen(fav));
                const char* mine = is_attacker ? "attacker" : "defender";
                r.key("my_side"); r.value_string(mine, strlen(mine));
            }

            r.end_object();
            war_count++;
        }
    }
    r.end_array();
    r.key("war_count"); r.value_int(war_count);

    // -------------------------------------------------------------------------
    // 6. Notable rulers: top 20 landed rulers sorted by military strength
    //    Uses enumerate_ai_characters — NOT geographic neighbors (adjacency
    //    detection not yet implemented). Sorted by military to show the most
    //    relevant/powerful rulers rather than random DB order.
    // -------------------------------------------------------------------------
    double our_mil = static_cast<double>(mil_current > 0 ? mil_current : 1);

    uint32_t our_liege = get_liege_id_from_addr(c);

    struct RulerCandidate {
        uint32_t id;
        void* raw_ptr;
        uintptr_t addr;
        uintptr_t td;
        int32_t mil;
        int32_t realm_size;
        bool at_war;
        const char* relation;
        int64_t dsq;   // squared capital-centroid distance (B1); <0 = unresolved
    };
    std::vector<RulerCandidate> ruler_candidates;
    ruler_candidates.reserve(200);

    // B1 (20260621): geographic origin — the scope ruler's capital map-pixel
    // centroid. When resolvable, nearby_rulers is filtered/sorted by squared
    // pixel distance (capital-centroid) instead of military rank, so a European
    // ruler no longer sees Tibet/Java. Fail-open: if the origin is unresolved
    // (unlanded ruler), fall back to the old military-rank behavior.
    int32_t ox = 0, oy = 0;
    bool have_origin = mc_capital_xy(c, ox, oy);

    enumerate_ai_characters([&](const CharacterInfo& info) -> bool {
        if (!info.alive || !info.has_realm) return true;
        if ((info.id & 0xFFFFFF) == char_id) return true;

        uintptr_t their_addr = reinterpret_cast<uintptr_t>(info.raw_ptr);
        uintptr_t td = safe_read<uintptr_t>(their_addr + ck3::character::TITLE_REALM_DATA);
        if (!td || !is_mapped(td)) return true;

        int32_t their_mil = safe_read<int32_t>(td + ck3::title_realm::MIL_CURRENT);
        int32_t their_rsz = safe_read<int32_t>(td + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (their_rsz < 0 || their_rsz > 10000 || their_mil < 0 || their_mil > 10000000) return true;

        // Geographic filter: compute the candidate's capital centroid and skip
        // anyone beyond diplomatic range. Only enforced when the origin
        // resolved; otherwise we keep every candidate (military fallback below).
        int64_t dsq = -1;
        if (have_origin) {
            int32_t tx = 0, ty = 0;
            if (!mc_capital_xy(their_addr, tx, ty)) return true; // unresolved → drop
            int64_t dx = static_cast<int64_t>(tx) - ox;
            int64_t dy = static_cast<int64_t>(ty) - oy;
            dsq = dx * dx + dy * dy;
            if (dsq > static_cast<int64_t>(ck3::diplomatic_range::MAXIMUM_RANGE_SQUARED))
                return true; // out of diplomatic range → not a neighbor
        }

        // Is at war (from diplomatic data war list count)
        uintptr_t their_diplo = safe_read<uintptr_t>(their_addr + ck3::character::DIPLOMATIC_DATA);
        bool their_at_war = false;
        if (their_diplo && is_mapped(their_diplo)) {
            int32_t wc = safe_read<int32_t>(their_diplo + ck3::diplomatic_data::WAR_LIST_COUNT);
            their_at_war = (wc > 0 && wc < 100);
        }

        // Relationship type: vassal, liege, or other
        uint32_t their_liege = get_liege_id_from_addr(their_addr);
        const char* relation = "other";
        if (their_liege == char_id) relation = "vassal";
        else if (our_liege == (info.id & 0xFFFFFF)) relation = "liege";

        ruler_candidates.push_back({
            info.id & 0xFFFFFF, info.raw_ptr, their_addr, td,
            their_mil, their_rsz, their_at_war, relation, dsq
        });
        return ruler_candidates.size() < 500; // safety cap
    });

    // B1: nearest-first when geography resolved (capital-centroid distance);
    // otherwise the legacy military-rank ordering.
    if (have_origin) {
        std::stable_sort(ruler_candidates.begin(), ruler_candidates.end(),
            [](const RulerCandidate& a, const RulerCandidate& b) {
                return a.dsq < b.dsq;
            });
    } else {
        std::stable_sort(ruler_candidates.begin(), ruler_candidates.end(),
            [](const RulerCandidate& a, const RulerCandidate& b) {
                return a.mil > b.mil;
            });
    }

    r.key("nearby_rulers"); r.begin_array();
    int neighbor_count = 0;
    size_t limit = std::min(ruler_candidates.size(), static_cast<size_t>(20));
    for (size_t i = 0; i < limit; i++) {
        const auto& rc = ruler_candidates[i];
        r.begin_object();
        r.key("id"); r.value_uint(rc.id);

        // Name
        emit_char_names(r, rc.addr);

        r.key("military_strength"); r.value_int(rc.mil);
        r.key("realm_size"); r.value_int(rc.realm_size);

        // Power ratio: their_mil / our_mil (>1 means they're stronger)
        double power_ratio = static_cast<double>(rc.mil) / our_mil;
        r.key("power_ratio"); r.value_float(power_ratio);

        r.key("is_at_war"); r.value_bool(rc.at_war);
        r.key("relation"); r.value_string(rc.relation);
        if (rc.dsq >= 0) {
            // Euclidean capital-centroid distance in map pixels (sqrt of dsq).
            r.key("capital_distance");
            r.value_int(static_cast<int>(std::llround(std::sqrt(static_cast<double>(rc.dsq)))));
        }

        r.end_object();
        neighbor_count++;
    }
    r.end_array();
    r.key("nearby_ruler_count"); r.value_int(neighbor_count);
    r.key("nearby_ruler_note"); r.value_string(have_origin
        ? "Geographic neighbors within diplomatic range (capital-centroid distance, px)"
        : "FALLBACK: origin capital unresolved; ranked by military strength");

    // -------------------------------------------------------------------------
    // 7. Diplomatic summary: alliance count, truce count, war counts
    // -------------------------------------------------------------------------
    r.key("diplomacy"); r.begin_object();

    // War participation counts from core_data
    uint16_t wars_atk = safe_read<uint16_t>(core_data + ck3::core_data::WAR_ATK_COUNT);
    uint16_t wars_def = safe_read<uint16_t>(core_data + ck3::core_data::WAR_DEF_COUNT);
    r.key("wars_as_attacker"); r.value_int(wars_atk);
    r.key("wars_as_defender"); r.value_int(wars_def);

    // Truce count from diplomatic data
    if (diplo_data) {  // already is_mapped() at function head
        int32_t truce_count = safe_read<int32_t>(diplo_data + ck3::diplomatic_data::TRUCE_LIST_COUNT);
        if (truce_count < 0 || truce_count > 500) truce_count = 0;
        r.key("truce_count"); r.value_int(truce_count);

        r.key("legitimacy"); r.value_float(
            static_cast<double>(safe_read<int64_t>(diplo_data + ck3::diplomatic_data::LEGITIMACY)) / 100000.0);
    } else {
        r.key("truce_count"); r.value_int(0);
        r.key("legitimacy"); r.value_float(0.0);
    }

    // Vassal counts from title_realm
    if (title_data) {  // already is_mapped() at function head
        int32_t direct_vassals = safe_read<int32_t>(title_data + ck3::title_realm::DIRECT_VASSAL_ARRAY + 0x0C);
        if (direct_vassals < 0 || direct_vassals > 1000) direct_vassals = 0;
        r.key("direct_vassal_count"); r.value_int(direct_vassals);

        int32_t domain_limit = safe_read<int32_t>(title_data + ck3::title_realm::DOMAIN_LIMIT_CACHED);
        int32_t vassal_limit = safe_read<int32_t>(title_data + ck3::title_realm::VASSAL_LIMIT_CACHED);
        r.key("domain_limit"); r.value_int(domain_limit);
        r.key("vassal_limit"); r.value_int(vassal_limit);

        int32_t held_titles = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (held_titles < 0 || held_titles > 500) held_titles = 0;
        r.key("held_title_count"); r.value_int(held_titles);

        int32_t realm_tier = safe_read<int32_t>(title_data + ck3::title_realm::TIER);
        r.key("realm_tier"); r.value_int(realm_tier);
    }

    r.end_object(); // diplomacy

    // -------------------------------------------------------------------------
    // 8. Claims count (R25 FIX: claims at core_data+0x1E8, not char+0x170)
    // -------------------------------------------------------------------------
    {
        int32_t claim_count = 0;
        // core_data was already is_mapped()-validated at the function head.
        if (core_data) {
            claim_count = safe_read<int32_t>(core_data + ck3::core_data::CLAIMS_ARRAY + 0x0C);
            if (claim_count < 0 || claim_count > 500) claim_count = 0;
        }
        r.key("claim_count"); r.value_int(claim_count);
    }

    // -------------------------------------------------------------------------
    // 9. Skills
    // -------------------------------------------------------------------------
    r.key("skills"); r.begin_object();
    r.key("diplomacy");   r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_DIPLOMACY));
    r.key("martial");     r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_MARTIAL));
    r.key("stewardship"); r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_STEWARDSHIP));
    r.key("intrigue");    r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_INTRIGUE));
    r.key("learning");    r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_LEARNING));
    r.key("prowess");     r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_PROWESS));
    r.end_object();

    r.end_object();
    if (out) *out = r.take();

    ADLOG("get_ai_evaluations: done for character %u (wars=%d, nearby_rulers=%d)\n",
          char_id, war_count, neighbor_count);
    return true;
}

std::string enumerate_interactions() {
    // Primary protection: validate every pointer via is_mapped() before deref.
    // SIGSEGV safety net kept as last-resort fallback.
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) != 0) {
        g_in_protected_region = false;
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] SIGSEGV safety net triggered in enumerate_interactions"
                        " — pointer validation missed a case!\n");
        return R"({"interactions":[],"count":0,"note":"partial_results_memory_fault"})";
    }

    ResponseBuilder rb;
    rb.begin_object();
    rb.key("interactions");
    rb.begin_array();

    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::INTERACTION_DB);
    int count = 0;
    if ((db && is_mapped(db))) {
        uintptr_t table_base = safe_read<uintptr_t>(db + ck3::interaction_ht::TABLE_OFFSET);
        uint32_t capacity_mask = safe_read<uint32_t>(db + ck3::interaction_ht::CAPACITY_OFFSET);

        if ((table_base && is_mapped(table_base)) && capacity_mask > 0 && capacity_mask < 0x100000) {
            for (uint32_t idx = 0; idx <= capacity_mask && count < 2000; idx++) {
                uintptr_t entry = table_base + idx * ck3::interaction_ht::ENTRY_SIZE;
                if ((!entry || !is_mapped(entry))) break;  // table moved under us

                int8_t status = safe_read<int8_t>(entry + 4);
                if (status == 0 || status == -1) continue;

                uintptr_t obj = safe_read<uintptr_t>(entry + 0x10);
                if ((!obj || !is_mapped(obj))) continue;

                uint32_t hash = safe_read<uint32_t>(entry + 8);

                rb.begin_object();

                // Try runtime name from game memory
                const char* name = nullptr;
                size_t name_len = 0;
                if (read_db_obj_name(obj, name, name_len)) {
                    rb.key("name"); rb.value_string(name, name_len);
                } else {
                    // Fall back to static hash table
                    const char* iname = resolve_interaction_name(hash);
                    if (iname) { rb.key("name"); rb.value_string(iname); }
                }

                rb.key("hash"); rb.value_uint(hash);
                rb.end_object();
                count++;
            }
        }
    }

    rb.end_array();
    rb.key("count"); rb.value_int(count);
    rb.end_object();

    g_in_protected_region = false;
    return rb.take();
}

// =============================================================================
// Decision validity / requirements evaluation (ck-decision-validity)
// =============================================================================
//
// Evaluates a single decision's is_shown / is_valid / cost trigger blocks
// against a character scope, capturing the requirement loc-keys for failing
// gates. Mirrors the established crash-net used by process_trigger_query +
// loud_extract_slot_key + do_send_decision: a fabricated RNG scope (decision
// trigger leaves advance the RNG via FUN_0774a4b0 and null-deref without it),
// a watchdog, and a per-call sigsetjmp recovery point.
//
// Decompile evidence (re_decision_prerequisites.md + live gh decompile):
//   BUILD_SCOPES (FUN_06de9d90): writes *(u16*)scope=4 (char scope type) and
//       *(u64*)(scope+8) = *(u32*)(char+0x18) (CCharacter::ID). Self-contained;
//       no TLS / interaction pipeline needed.
//   CHECK_IS_SHOWN (FUN_06dedc60): ScriptTriggerEvaluator(decision+0x688, scope)
//   CHECK_IS_VALID (FUN_06deddf0): cooldown + ScriptTriggerEvaluator(+0x848) +
//       cost + custom validator; param_5 (failures) is the 0xD8 diag record.
//   CHECK_COST (FUN_06dee080): ScriptTriggerEvaluator(+0x768) and, when a
//       failures ptr is passed, FUN_0763ddc0 (EVAL_WITH_FAILURES) populates the
//       same 0xD8 diag record whose std::strings at +0x18/+0x38 carry the
//       requirement loc-key (e.g. CHARACTER_HAS_TRUCE).
// Rather than reconstruct the param_5 std::vector<failure> struct, we evaluate
// the boolean blocks directly and harvest the loc-key from the diag record for
// the failing is_valid / cost blocks (the proven loud_extract path).

namespace {

// libstdc++ std::string reader (local copy of ai_eval_hook's static helper).
size_t dv_read_std_string(uintptr_t addr, char* out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    uintptr_t data_ptr = 0;
    uint64_t len = 0;   // game_string_view: the string ABI of the game's build
    if (!game_string_view(addr, &data_ptr, &len) || len > 1024) return 0;
    if (!is_mapped(data_ptr)) return 0;
    size_t n = (len < out_size - 1) ? static_cast<size_t>(len) : out_size - 1;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = safe_read<uint8_t>(data_ptr + i);
        if (b == 0) { n = i; break; }
        out[i] = static_cast<char>(b);
    }
    out[n] = '\0';
    return n;
}

bool dv_is_snake_upper_key(const char* s, size_t n) {
    if (n < 5) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// Evaluate one CAndTrigger block via ScriptTriggerEvaluator under crash-net.
// Returns true/false; on crash sets *crashed and returns false.
bool dv_eval_block(uintptr_t trigger_block, uintptr_t scope_ctx, bool* crashed) {
    *crashed = false;
    uintptr_t head = safe_read<uintptr_t>(trigger_block);
    if (head == 0) return true;  // absent trigger = auto-pass (matches engine)

    auto trigger_eval = reinterpret_cast<bool(*)(uintptr_t, uintptr_t)>(
        g_base_addr + ck3::trigger_eval_funcs::SCRIPT_TRIGGER_EVALUATOR);
    if (!is_mapped(reinterpret_cast<uintptr_t>(trigger_eval))) { *crashed = true; return false; }

    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        *crashed = true;
        return false;
    }
    bool result = false;
    try {
        result = trigger_eval(trigger_block, scope_ctx);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        *crashed = true;
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();
    return result;
}

// Harvest the requirement loc-key from the loud-failure diag record for a
// failing trigger block. Mirrors loud_extract_slot_key. On any failure leaves
// out_key empty and returns false.
bool dv_extract_failure_key(uintptr_t trigger_block, uintptr_t scope_ctx,
                            char* out_key, size_t out_size) {
    if (!out_key || out_size == 0) return false;
    out_key[0] = '\0';

    typedef void (*fn_eval_with_failures)(void**, uintptr_t, uintptr_t);
    typedef void (*fn_diag_dtor)(void*);
    auto fn_populate = reinterpret_cast<fn_eval_with_failures>(
        g_base_addr + ck3::trigger_eval_funcs::EVAL_WITH_FAILURES);
    auto fn_destroy = reinterpret_cast<fn_diag_dtor>(
        g_base_addr + ck3::trigger_eval_funcs::DIAG_OBJECT_DTOR);
    if (!is_mapped(reinterpret_cast<uintptr_t>(fn_populate)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(fn_destroy))) return false;

    void* record = nullptr;
    char buf1[96] = {}; char buf2[96] = {};
    size_t n1 = 0, n2 = 0;

    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        return false;
    }
    try {
        fn_populate(&record, trigger_block, scope_ctx);
        if (record && is_mapped(reinterpret_cast<uintptr_t>(record))) {
            uintptr_t rec = reinterpret_cast<uintptr_t>(record);
            n1 = dv_read_std_string(rec + ck3::diag_record::STR1_DATA, buf1, sizeof(buf1));
            n2 = dv_read_std_string(rec + ck3::diag_record::STR2_DATA, buf2, sizeof(buf2));
        }
    } catch (...) { n1 = n2 = 0; }
    g_in_protected_region = false;
    watchdog_disarm();

    if (record) {
        watchdog_arm(3);
        g_in_protected_region = true;
        sig = sigsetjmp(g_recovery_point, 1);
        if (sig == 0) {
            try { fn_destroy(record); operator delete(record); } catch (...) {}
        }
        g_in_protected_region = false;
        watchdog_disarm();
    }

    const char* pick = nullptr; size_t pick_n = 0;
    if (dv_is_snake_upper_key(buf1, n1)) { pick = buf1; pick_n = n1; }
    else if (dv_is_snake_upper_key(buf2, n2)) { pick = buf2; pick_n = n2; }
    if (!pick) return false;
    size_t copy_n = (pick_n < out_size - 1) ? pick_n : out_size - 1;
    memcpy(out_key, pick, copy_n);
    out_key[copy_n] = '\0';
    return true;
}

// Read static cost slot defaults (SLOT_DEFAULT at +0xA0) for non-empty slots.
// Returns the int64 default; only meaningful when the slot count (+0xC8) > 0.
// We surface the static defaults rather than running the math evaluator (which
// would need the full CScriptedCost eval path + RNG). Conventional CK3 cost
// ordering is slot0=gold, slot1=prestige, slot2=piety (MEDIUM confidence — see
// re_decision_trigger_tree.md open question on slot→resource mapping).
struct DvCost { bool has_gold, has_prestige, has_piety; double gold, prestige, piety; };

DvCost dv_read_cost(uintptr_t decision) {
    DvCost c = {false,false,false,0,0,0};
    // Prefer the primary CScriptedCost at +0x1628; fall back to +0xb78.
    uintptr_t cost_obj = decision + ck3::decision_type::CAN_TAKE_PRIMARY;
    // IS_EMPTY (FUN_06a8f650) returns true when all 10 slots have count==0.
    auto is_empty = reinterpret_cast<bool(*)(uintptr_t)>(
        g_base_addr + ck3::trigger_eval_funcs::IS_EMPTY);
    volatile bool primary_empty = true;
    if (is_mapped(reinterpret_cast<uintptr_t>(is_empty))) {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig == 0) {
            try { primary_empty = is_empty(cost_obj); } catch (...) { primary_empty = true; }
        } else { primary_empty = true; }
        g_in_protected_region = false;
        watchdog_disarm();
    }
    if (primary_empty) cost_obj = decision + ck3::decision_type::CAN_TAKE_FALLBACK;

    auto slot_static = [&](int idx, bool& has, double& val) {
        uintptr_t slot = cost_obj + ck3::scripted_cost::SLOT_BASE
                         + static_cast<uintptr_t>(idx) * ck3::scripted_cost::SLOT_STRIDE;
        uint32_t cnt = safe_read<uint32_t>(slot + ck3::scripted_cost::SLOT_COUNT);
        if (cnt == 0) return;
        int64_t def = safe_read<int64_t>(slot + ck3::scripted_cost::SLOT_DEFAULT);
        if (def == 0) return;
        has = true;
        val = static_cast<double>(def) / 100000.0;
    };
    slot_static(0, c.has_gold, c.gold);
    slot_static(1, c.has_prestige, c.prestige);
    slot_static(2, c.has_piety, c.piety);
    return c;
}

// Emit one decision object into rb. When valid_only, returns false (caller
// skips begin/end) if the decision is not available.
struct DvFields {
    bool shown, valid, cost_ok, available, eval_error;
    char fail_valid[96];
    char fail_cost[96];
};

void dv_evaluate(uintptr_t decision, uintptr_t scope_ctx, DvFields& f) {
    f.shown = f.valid = f.cost_ok = f.available = false;
    f.eval_error = false;
    f.fail_valid[0] = '\0';
    f.fail_cost[0]  = '\0';

    bool crashed = false;
    f.shown = dv_eval_block(decision + ck3::decision_type::IS_SHOWN, scope_ctx, &crashed);
    if (crashed) { f.eval_error = true; return; }

    f.valid = dv_eval_block(decision + ck3::decision_type::IS_VALID, scope_ctx, &crashed);
    if (crashed) { f.eval_error = true; return; }
    if (!f.valid) {
        dv_extract_failure_key(decision + ck3::decision_type::IS_VALID, scope_ctx,
                               f.fail_valid, sizeof(f.fail_valid));
    }

    f.cost_ok = dv_eval_block(decision + ck3::decision_type::COST_TRIGGER, scope_ctx, &crashed);
    if (crashed) { f.eval_error = true; return; }
    if (!f.cost_ok) {
        dv_extract_failure_key(decision + ck3::decision_type::COST_TRIGGER, scope_ctx,
                               f.fail_cost, sizeof(f.fail_cost));
    }

    f.available = f.shown && f.valid && f.cost_ok;
}

}  // namespace

// Annotated decision enumeration. Heavy (runs game-fn trigger evals per
// decision) — never on the snapshot path. Capped + time-guarded.
// #64 SELF-HEALING culprit skip-list. Some decisions' engine trigger-eval HANGS with
// SIGALRM masked (no watchdog can bound it). We cannot pre-detect which, so we learn
// them across runs: before each dv_evaluate we write the decision's name-hash to an
// in-flight marker file and clear it after the eval returns. A true freeze never clears
// it; on the NEXT bridge start (after the monitor relaunches the frozen game) we promote
// the stranded in-flight hash into the persistent skip file. Each culprit freezes at
// most ONCE, then is skipped forever. (re_resolution_decisions64.md)
// rc2 B1: in the instance state dir (/tmp in legacy layout, else private),
// opened through the verified state-dir fd (O_NOFOLLOW), never by path.
static constexpr const char* kDecisionInflight = "ck3-decision-inflight";
static constexpr const char* kDecisionSkip     = "ck3-decision-skip";
static std::unordered_set<uint32_t> g_decision_skip CK3_EARLY_INIT;
static bool g_decision_skip_loaded = false;
static void load_decision_skip() {
    if (g_decision_skip_loaded) return;
    g_decision_skip_loaded = true;
    g_decision_skip.insert(0x0d3cbc02);  // seed: known culprit (ruler 13902, full-scan idx=3)
    g_decision_skip.insert(0xcce24e04);  // seed: foster_integration_decision (self-heal-captured 2026-06-19)
    g_decision_skip.insert(0x5f86cc47);  // seed: create_armenian_empire_decision — froze game thread on #109 Surface-2 live check (2026-06-22); engine trigger-eval hangs with SIGALRM masked
    // Promote a stranded in-flight hash from a prior freeze, then clear the marker.
    if (FILE* f = bridge::instance_state_fopen(kDecisionInflight, "r")) {
        unsigned h = 0;
        if (fscanf(f, "%x", &h) == 1 && h) {
            g_decision_skip.insert(h);
            if (FILE* sf = bridge::instance_state_fopen(kDecisionSkip, "a")) { fprintf(sf, "%08x\n", h); fclose(sf); }
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] #64 promoted in-flight culprit hash 0x%08x to skip-list\n", h);
        }
        fclose(f);
        bridge::instance_state_unlink(kDecisionInflight);
    }
    // Load the persistent skip file.
    if (FILE* sf = bridge::instance_state_fopen(kDecisionSkip, "r")) {
        unsigned h;
        while (fscanf(sf, "%x", &h) == 1) if (h) g_decision_skip.insert(h);
        fclose(sf);
    }
}

std::string enumerate_decisions_annotated(uint32_t char_id, bool valid_only,
                                          const std::vector<uint32_t>& req_hashes,
                                          const std::vector<std::string>& req_names) {
    const bool list_mode = (!req_hashes.empty() || !req_names.empty());
    load_decision_skip();   // promotes any stranded in-flight culprit from a prior freeze
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) != 0) {
        g_in_protected_region = false;
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] SIGSEGV net in enumerate_decisions_annotated\n");
        return R"({"decisions":[],"count":0,"note":"partial_results_memory_fault"})";
    }
    g_in_protected_region = false;

    // Resolve character: explicit id → controlled → player.
    uint32_t use_id = char_id & 0xFFFFFF;
    if (use_id == 0) {
        use_id = g_controlled_primary.load(std::memory_order_relaxed) & 0xFFFFFF;
        if (use_id == 0) use_id = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID) & 0xFFFFFF;
    }
    void* character = (use_id != 0) ? lookup_character(use_id) : nullptr;
    if (!character || is_sentinel(character)) {
        ResponseBuilder e; e.begin_object();
        e.key("action"); e.value_string(valid_only ? "list_valid_decisions" : "list_decisions");
        e.key("error"); e.value_string("character_not_found");
        e.key("character_id"); e.value_uint(use_id);
        e.end_object();
        return e.take();
    }
    uintptr_t cptr = reinterpret_cast<uintptr_t>(character);

    // Character current resources (have-side). Read from realm/core data.
    // volatile: read in the affordability check AFTER the per-decision sigsetjmp/longjmp
    // (#64) — set once here, never mutated in the loop, so volatile just pins them across
    // the longjmp (matches idx/hash/emitted discipline below).
    volatile double cur_gold = 0, cur_piety = 0, cur_prestige = 0;
    {
        uintptr_t rd = safe_read<uintptr_t>(cptr + ck3::character::REALM_DATA);
        if (rd && is_mapped(rd)) {
            cur_gold     = static_cast<double>(safe_read<int64_t>(rd + ck3::character::REALM_GOLD)) / 100000.0;
            cur_piety    = static_cast<double>(safe_read<int64_t>(rd + ck3::character::REALM_PIETY)) / 100000.0;
            cur_prestige = static_cast<double>(safe_read<int64_t>(rd + ck3::character::REALM_PRESTIGE)) / 100000.0;
        }
    }

    // Build the character scope once (BUILD_SCOPES writes a {type,id} struct
    // into our buffer). Reused for every decision.
    alignas(8) uint8_t scope_buf[64] = {};
    {
        auto build_scopes = reinterpret_cast<void(*)(void*, void*)>(
            g_base_addr + ck3::decision_funcs::BUILD_SCOPES);
        if (!is_mapped(reinterpret_cast<uintptr_t>(build_scopes))) {
            ResponseBuilder e; e.begin_object();
            e.key("error"); e.value_string("build_scopes_not_mapped"); e.end_object();
            return e.take();
        }
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            ResponseBuilder e; e.begin_object();
            e.key("error"); e.value_string("build_scopes_crashed"); e.end_object();
            return e.take();
        }
        try { build_scopes(scope_buf, character); } catch (...) {}
        g_in_protected_region = false;
        watchdog_disarm();
    }
    uintptr_t scope_ctx = reinterpret_cast<uintptr_t>(scope_buf);

    // Arm the fake RNG scope around the whole eval loop (trigger leaves advance
    // the RNG; null RandomScopeManager+0x20 on this thread would null-deref).
    // ck-zgh4 scope latch. This one is a READ — discover(decisions) — so it
    // refuses with read wording: telling a seat its "action is unavailable"
    // when it asked for a listing would misdescribe what it lost. The latch
    // costs the agent a planning read as well as its arming verbs, which is a
    // real cost and the reason this site is called out separately.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        ResponseBuilder e; e.begin_object();
        e.key("error"); e.value_string(bridge::scope_latched_reason());
        e.key("retryable"); e.value_bool(false);
        e.key("note"); e.value_string(bridge::scope_latched_read_note());
        e.end_object();
        return e.take();
    }

    uint64_t saved_rng = 0;
    volatile bool rng_armed = arm_fake_random_scope(&saved_rng);  // read after the per-decision longjmp loop

    ResponseBuilder rb;
    rb.begin_object();
    rb.key("action"); rb.value_string(valid_only ? "list_valid_decisions" : "list_decisions");
    rb.key("character_id"); rb.value_uint(use_id);
    rb.key("current"); rb.begin_object();
        rb.key("gold");     rb.value_float(cur_gold);
        rb.key("piety");    rb.value_float(cur_piety);
        rb.key("prestige"); rb.value_float(cur_prestige);
    rb.end_object();
    rb.key("cost_mapping_confidence"); rb.value_string("medium");
    rb.key("decisions");
    rb.begin_array();

    // volatile: these accumulate across the per-decision sigsetjmp/longjmp (#64) —
    // non-volatile locals modified between sigsetjmp and longjmp are indeterminate.
    volatile int emitted = 0;
    volatile int eval_errors = 0;
    volatile int considered = 0;
    auto t_start = std::chrono::steady_clock::now();
    // Time budget: heavy path. Cap so a live agent query never wedges.
    const long kBudgetMs = 4000;

    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::DECISION_DB);
    if (db && is_mapped(db)) {
        uintptr_t table_base = safe_read<uintptr_t>(db + ck3::decision_ht::TABLE_OFFSET);
        uint32_t capacity_mask = safe_read<uint32_t>(db + ck3::decision_ht::CAPACITY_OFFSET);
        if (table_base && is_mapped(table_base) && capacity_mask > 0 && capacity_mask < 0x100000) {
            // idx volatile: the per-decision longjmp (#64) returns into this loop;
            // a clobbered counter could re-enter the offending decision and spin.
            for (volatile uint32_t idx = 0; idx <= capacity_mask && considered < 2000; idx++) {
                // Time guard.
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_start).count();
                if (elapsed > kBudgetMs) break;

                uintptr_t entry = table_base + idx * ck3::decision_ht::ENTRY_SIZE;
                if (!entry || !is_mapped(entry)) break;
                int8_t status = safe_read<int8_t>(entry + 4);
                if (status == 0 || status == -1) continue;
                uintptr_t obj = safe_read<uintptr_t>(entry + 0x10);
                if (!obj || !is_mapped(obj)) continue;
                volatile uint32_t hash = safe_read<uint32_t>(entry + 8);  // read in the wedged branch after longjmp

                // #64 LIST-MODE: when specific decisions were requested, evaluate ONLY those
                // (bounded → fast + freeze-safe). Match by name-hash or by decision key name.
                if (list_mode) {
                    bool want = false;
                    for (uint32_t h : req_hashes) { if (h == (uint32_t)hash) { want = true; break; } }
                    if (!want && !req_names.empty()) {
                        const char* nm = nullptr; size_t nl = 0;
                        if (read_db_obj_name(obj, nm, nl) && nm) {
                            for (const std::string& rn : req_names)
                                if (rn.size() == nl && memcmp(rn.data(), nm, nl) == 0) { want = true; break; }
                        }
                    }
                    if (!want) continue;  // not requested — skip without evaluating
                }
                considered = considered + 1;

                // #64 culprit skip-list (re_resolution_decisions64.md). The engine
                // trigger-eval HANGS with SIGALRM masked for specific decisions, so no
                // watchdog can interrupt it — the only safe fix is to never dv_evaluate
                // those. The trace (compile-gated) captures the culprit hash: build with
                // -DCK3_DECISION_EVAL_TRACE, run ONE list_valid_decisions on a known-bad
                // ruler; the LAST flushed "dv_eval PRE" line before the freeze is the
                // offender. Add it to kDecisionEvalSkip and rebuild clean.
#ifdef CK3_DECISION_EVAL_TRACE
                ADLOG("dv_eval PRE idx=%u hash=0x%08x considered=%d\n",
                      (unsigned)idx, (unsigned)hash, (int)considered);
#endif
                if (g_decision_skip.count((uint32_t)hash)) {
                    rb.begin_object();
                    const char* sn = resolve_decision_name((uint32_t)hash);
                    if (sn) { rb.key("name"); rb.value_string(sn); }
                    rb.key("hash"); rb.value_uint((uint32_t)hash);
                    rb.key("eval_error"); rb.value_bool(true);
                    rb.key("eval_skipped"); rb.value_bool(true);
                    rb.key("note"); rb.value_string("skip-listed: engine trigger-eval hangs on this decision (re_decision_eval_hang.md)");
                    DvCost sc = dv_read_cost(obj);   // static cost is safe (no engine eval)
                    rb.key("cost"); rb.begin_object();
                    if (sc.has_gold)     { rb.key("gold");     rb.value_float(sc.gold); }
                    if (sc.has_prestige) { rb.key("prestige"); rb.value_float(sc.prestige); }
                    if (sc.has_piety)    { rb.key("piety");    rb.value_float(sc.piety); }
                    rb.end_object();
                    rb.end_object();
                    emitted++; eval_errors++;
                    continue;
                }

                // #64 FIX (2026-06-19): bound EACH decision eval with its own
                // watchdog + sigsetjmp. The prior version froze the game because the
                // dv_evaluate loop ran with NO watchdog armed (only build_scopes was
                // wrapped) — a single runaway trigger-eval hung the game thread
                // forever. Now: arm watchdog(2s) per decision; on SIGALRM/SIGSEGV the
                // handler longjmps back HERE, we mark eval_error and CONTINUE to the
                // next decision (the for-loop's idx++ advances — NO re-entry into the
                // offending decision). This makes a hung/faulting decision cost ~2s
                // and skip, instead of freezing. (re_decision_eval_hang.md)
                DvFields f;
                {
                    volatile bool eval_done = false;
                    // #64 self-heal: mark this hash in-flight BEFORE the eval. If the engine
                    // trigger-eval hangs (game freezes), this marker survives the relaunch and
                    // load_decision_skip() promotes it to the skip-list next time. Cleared
                    // below on any return (success or caught fault) — only a true freeze strands it.
                    if (FILE* mf = bridge::instance_state_fopen(kDecisionInflight, "w")) {
                        fprintf(mf, "%08x\n", (unsigned)hash); fflush(mf); fclose(mf);
                    }
                    watchdog_arm(2);
                    g_in_protected_region = true;
                    if (sigsetjmp(g_recovery_point, 1) == 0) {
                        dv_evaluate(obj, scope_ctx, f);
                        eval_done = true;
                    }
                    g_in_protected_region = false;
                    watchdog_disarm();
                    bridge::instance_state_unlink(kDecisionInflight);   // eval returned (or faulted) → not a freeze culprit
                    if (!eval_done) {
                        // hung or faulted on this decision — skip it, keep going.
                        f.eval_error = true;
                        eval_errors++;
                        rb.begin_object();
                        const char* dn = resolve_decision_name(hash);  // hash-only; no obj deref (it may be the faulting ptr)
                        if (dn) { rb.key("name"); rb.value_string(dn); }
                        rb.key("hash"); rb.value_uint(hash);
                        rb.key("eval_error"); rb.value_bool(true);
                        rb.key("eval_wedged"); rb.value_bool(true);
                        rb.end_object();
                        emitted++;
                        continue;
                    }
                }

                // RESOURCE AFFORDABILITY (A10 / valid-decisions-cost-blind). cost_ok above
                // is only the `cost` TRIGGER block (+0x768 COST_TRIGGER) — non-resource
                // prerequisites. It does NOT compare the decision's resource COST
                // (CScriptedCost at +0x1628/+0xb78, read by dv_read_cost) against the
                // ruler's current funds — that gate lives in AI_TRY_DECISION_CORE's
                // can-take check (FUN_04dd2ca0 over +0x1628/+0xb78), which returns
                // "cannot afford decision". found_empire_decision is the canonical case:
                // cost_ok=true (no script prereqs fail) yet act() refuses for lack of
                // prestige/piety. Fold a pure read-side have-vs-need check into available
                // so the valid_only shortlist stops listing unaffordable decisions.
                // Static-cost only (no engine eval) → bounded, crash-free.
                DvCost cost = dv_read_cost(obj);
                bool afford_ok = true;
                if (cost.has_gold     && cur_gold     < cost.gold)     afford_ok = false;
                if (cost.has_prestige && cur_prestige < cost.prestige) afford_ok = false;
                if (cost.has_piety    && cur_piety    < cost.piety)    afford_ok = false;
                bool available = f.available && afford_ok;

                if (valid_only && !available) continue;

                rb.begin_object();
                const char* name = nullptr; size_t name_len = 0;
                if (read_db_obj_name(obj, name, name_len)) {
                    rb.key("name"); rb.value_string(name, name_len);
                } else {
                    const char* dname = resolve_decision_name(hash);
                    if (dname) { rb.key("name"); rb.value_string(dname); }
                }
                rb.key("hash"); rb.value_uint(hash);

                if (f.eval_error) {
                    rb.key("eval_error"); rb.value_bool(true);
                    eval_errors++;
                } else {
                    rb.key("shown");     rb.value_bool(f.shown);
                    rb.key("valid");     rb.value_bool(f.valid);
                    rb.key("cost_ok");   rb.value_bool(f.cost_ok);
                    rb.key("afford_ok"); rb.value_bool(afford_ok);
                    rb.key("available"); rb.value_bool(available);

                    rb.key("failures"); rb.begin_array();
                    if (!f.valid && f.fail_valid[0]) rb.value_string(f.fail_valid);
                    if (!f.cost_ok && f.fail_cost[0]) rb.value_string(f.fail_cost);
                    if (!afford_ok) rb.value_string("cannot_afford_decision");
                    rb.end_array();

                    // Cost block (static defaults; have-vs-need with current{}).
                    rb.key("cost"); rb.begin_object();
                    if (cost.has_gold)     { rb.key("gold");     rb.value_float(cost.gold); }
                    if (cost.has_prestige) { rb.key("prestige"); rb.value_float(cost.prestige); }
                    if (cost.has_piety)    { rb.key("piety");    rb.value_float(cost.piety); }
                    rb.end_object();
                }
                rb.end_object();
                emitted++;
            }
        }
    }

    if (rng_armed) restore_fake_random_scope(saved_rng);

    rb.end_array();
    rb.key("count"); rb.value_int(emitted);
    rb.key("considered"); rb.value_int(considered);
    rb.key("eval_errors"); rb.value_int(eval_errors);
    rb.key("valid_only"); rb.value_bool(valid_only);
    rb.end_object();
    return rb.take();
}

// #64 FIX: the annotated decision eval runs the engine trigger evaluator, which
// MUST run on the GAME thread (the watchdog is SIGEV_THREAD_ID-bound there; the IPC
// thread blocks SIGALRM → watchdog_arm is a no-op on the IPC thread → a slow trigger
// hung the IPC thread). Use the proven deferred-query idiom (mirrors g_trigger_query
// / g_named_trigger_query): the IPC handler submits + polls; process_decision_eval_
// query() runs enumerate_decisions_annotated on the game thread from the intent drain.
DecisionEvalQuery g_decision_eval_query CK3_EARLY_INIT = {};

void process_decision_eval_query() {  // GAME thread (drain_and_execute_intents)
    int expected = 1;
    if (!g_decision_eval_query.state.compare_exchange_strong(
            expected, 4, std::memory_order_acq_rel)) return;
    try {
        g_decision_eval_query.result_json = enumerate_decisions_annotated(
            g_decision_eval_query.char_id, g_decision_eval_query.valid_only,
            g_decision_eval_query.req_hashes, g_decision_eval_query.req_names);
        g_decision_eval_query.state.store(2, std::memory_order_release);
    } catch (...) {
        snprintf(g_decision_eval_query.error, sizeof(g_decision_eval_query.error),
                 "decision_eval_exception");
        g_decision_eval_query.state.store(3, std::memory_order_release);
    }
}

// Shared submit-and-poll for the two annotated decision queries (IPC thread).
static bool submit_decision_eval(const char* params_json, bool valid_only, std::string* out) {
    if (!out) return true;
    int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) cid = json_get_int(params_json, "character");
    if (cid <= 0) cid = json_get_int(params_json, "actor_id");

    // #64 list-mode: parse the optional "decisions" array (strings = decision key names,
    // numbers = name-hashes). Bounded eval over ONLY these → fast + freeze-safe. When the
    // list is empty/absent we DON'T run the full engine eval (slow + can freeze on a
    // culprit decision) — serve the light name+hash list instead (the safe default).
    std::vector<uint32_t>    req_hashes;
    std::vector<std::string> req_names;
    {
        rapidjson::Document d;
        if (!d.Parse(params_json).HasParseError() && d.IsObject() &&
            d.HasMember("decisions") && d["decisions"].IsArray()) {
            for (auto& v : d["decisions"].GetArray()) {
                if (v.IsString())      req_names.emplace_back(v.GetString(), v.GetStringLength());
                else if (v.IsUint())   req_hashes.push_back(v.GetUint());
                else if (v.IsInt())    req_hashes.push_back(static_cast<uint32_t>(v.GetInt()));
                else if (v.IsUint64()) req_hashes.push_back(static_cast<uint32_t>(v.GetUint64()));
            }
        }
    }
    if (req_hashes.empty() && req_names.empty()) {
        // No specific decisions requested → safe light list (name+hash, no engine eval).
        *out = enumerate_decisions();
        return true;
    }

    int st = g_decision_eval_query.state.load(std::memory_order_acquire);
    if (st == 1 || st == 4) { *out = R"({"error":"decision_eval_busy","hint":"Another decisions query is pending; retry shortly."})"; return true; }
    if (st == 2 || st == 3) g_decision_eval_query.state.store(0, std::memory_order_release);
    g_decision_eval_query.char_id    = (cid > 0) ? static_cast<uint32_t>(cid) : 0;
    g_decision_eval_query.valid_only = valid_only;
    g_decision_eval_query.req_hashes = std::move(req_hashes);
    g_decision_eval_query.req_names  = std::move(req_names);
    g_decision_eval_query.error[0]   = '\0';
    g_decision_eval_query.result_json.clear();
    g_decision_eval_query.state.store(1, std::memory_order_release);
    for (int i = 0; i < 140; ++i) {  // ~7s poll; the eval's internal budget is 4s
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int s = g_decision_eval_query.state.load(std::memory_order_acquire);
        if (s == 2) { *out = g_decision_eval_query.result_json; g_decision_eval_query.state.store(0, std::memory_order_release); return true; }
        if (s == 3) { *out = std::string("{\"error\":\"") + g_decision_eval_query.error + "\"}"; g_decision_eval_query.state.store(0, std::memory_order_release); return true; }
    }
    *out = R"({"error":"timeout_waiting_for_game_thread","hint":"Game must be unpaused for one tick; the eval runs during drain_and_execute_intents."})";
    return true;  // leave state for a retry to re-claim
}

bool do_list_valid_decisions(const char* params_json, std::string* response_out) {
    return submit_decision_eval(params_json, /*valid_only=*/true, response_out);
}
bool do_list_decisions_annotated(const char* params_json, std::string* response_out) {
    return submit_decision_eval(params_json, /*valid_only=*/false, response_out);
}

std::string enumerate_decisions() {
    // Primary protection: validate every pointer via is_mapped() before deref.
    // SIGSEGV safety net kept as last-resort fallback.
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) != 0) {
        g_in_protected_region = false;
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] SIGSEGV safety net triggered in enumerate_decisions"
                        " — pointer validation missed a case!\n");
        return R"({"decisions":[],"count":0,"note":"partial_results_memory_fault"})";
    }

    ResponseBuilder rb;
    rb.begin_object();
    rb.key("decisions");
    rb.begin_array();

    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::DECISION_DB);
    int count = 0;
    if ((db && is_mapped(db))) {
        uintptr_t table_base = safe_read<uintptr_t>(db + ck3::decision_ht::TABLE_OFFSET);
        uint32_t capacity_mask = safe_read<uint32_t>(db + ck3::decision_ht::CAPACITY_OFFSET);

        if ((table_base && is_mapped(table_base)) && capacity_mask > 0 && capacity_mask < 0x100000) {
            for (uint32_t idx = 0; idx <= capacity_mask && count < 2000; idx++) {
                uintptr_t entry = table_base + idx * ck3::decision_ht::ENTRY_SIZE;
                if ((!entry || !is_mapped(entry))) break;  // table moved under us

                int8_t status = safe_read<int8_t>(entry + 4);
                if (status == 0 || status == -1) continue;

                uintptr_t obj = safe_read<uintptr_t>(entry + 0x10);
                if ((!obj || !is_mapped(obj))) continue;

                uint32_t hash = safe_read<uint32_t>(entry + 8);

                rb.begin_object();

                const char* name = nullptr;
                size_t name_len = 0;
                if (read_db_obj_name(obj, name, name_len)) {
                    rb.key("name"); rb.value_string(name, name_len);
                } else {
                    const char* dname = resolve_decision_name(hash);
                    if (dname) { rb.key("name"); rb.value_string(dname); }
                }

                rb.key("hash"); rb.value_uint(hash);
                rb.end_object();
                count++;
            }
        }
    }

    rb.end_array();
    rb.key("count"); rb.value_int(count);
    rb.end_object();

    g_in_protected_region = false;
    return rb.take();
}

// =============================================================================
// ck-ul5i: engine-source truce partner enumeration.
//
// The verify-round3 live probe caught do_query_relations returning truces:[]
// for a live scripted truce that the ENGINE pair-probe sees and enforces
// (query_cb_validity_strict target_state.in_truce:true via GET_TRUCE_LIST, plus
// per-CB in_truce=1 culls in the bridge log) — the CDiplomaticRelation walk
// below is NOT the list the engine consults. This helper enumerates the SAME
// engine source the strict CB validator uses: GET_TRUCE_LIST(char) returns the
// genuine truce list (Ghidra-named "GetTruceList"; CDynArray, data ptr at +0x0,
// count at +0xC, 8-byte CCharacter* entries, sorted) — the exact list
// probe_alliance_truce() SIMD-searches for its in_truce verdict.
//
// Returns true when the engine list was READ (out_ids filled — possibly empty,
// an empty list is a valid answer); false when the probe could not run (getter
// unmapped / list unmapped / fault), so the caller can surface a typed
// engine_truce_probe:"unavailable" instead of silently reporting no truces.
// Same sigsetjmp + g_in_protected_region posture as probe_alliance_truce;
// bounded to 200 partners.
static bool collect_engine_truce_partners(void* character,
                                          std::vector<uint32_t>& out_ids) {
    out_ids.clear();
    if (!character || is_sentinel(character)) return false;
    auto get_truce_list = reinterpret_cast<uintptr_t(*)(void*)>(
        bridge::game_fn(ck3::funcs::GET_TRUCE_LIST));
    if (!is_mapped(reinterpret_cast<uintptr_t>(get_truce_list))) return false;
    volatile bool ok_v = false;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig == 0) {
        g_in_protected_region = true;
        uintptr_t tlist = get_truce_list(character);
        if (tlist && is_mapped(tlist)) {
            uintptr_t b = safe_read<uintptr_t>(tlist + 0);
            int32_t n = safe_read<int32_t>(tlist + 0xC);
            if (n >= 0 && n < 200) {
                ok_v = true;  // a valid (possibly empty) engine read
                if (b && is_mapped(b) && n > 0) {
                    for (int32_t i = 0; i < n; i++) {
                        uintptr_t partner = safe_read<uintptr_t>(b + (size_t)i * 8);
                        if (!partner || !is_mapped(partner)) continue;
                        void* pc = reinterpret_cast<void*>(partner);
                        if (is_sentinel(pc)) continue;
                        uint32_t pid = char_get_id(pc) & 0xFFFFFF;
                        if (pid) out_ids.push_back(pid);
                    }
                }
            }
        }
        g_in_protected_region = false;
    } else {
        g_in_protected_region = false;  // fault → typed "unavailable" upstream
        out_ids.clear();
        return false;
    }
    return ok_v;
}

// =============================================================================
// query_relations — truces, alliances, hooks for a character
// Pure memory reads + the GET_TRUCE_LIST engine getter (same posture as the
// strict CB validator's probe_alliance_truce). Returns:
//   truces[]: {target_id, source, end_date/end_date_raw when reachable,
//              reason/war_name on relation-walk entries}
//   engine_truce_count, engine_truce_probe ("ok" | "unavailable")
//   alliances[]: {ally_id, reason_count, reasons: [{through_char_1, through_char_2}]}
//   alliance_count, truce_count
//   hooks_note: string explaining hook data is not yet available from memory reads
// =============================================================================
bool do_query_relations(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = R"({"error":"character_not_found"})";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_relations");
    r.key("character_id"); r.value_uint(my_id);

    // ---- Game date for context (used to check truce expiry) ----
    int32_t current_date_raw = 0;
    {
        uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
        if (gs) current_date_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    }

    // Helper lambda: convert raw CK3 date to "YYYY.MM.DD" string
    auto format_date = [](int32_t raw_date, char* buf, size_t bufsz) {
        ck3::game_date::format_raw(raw_date, buf, bufsz);  // ck-05mg
    };

    // ---- 1. Truces ----
    // ck-ul5i: the ENGINE truce list is the authoritative membership source —
    // GET_TRUCE_LIST(char), the exact list the strict CB validator's
    // target_state.in_truce probe SIMD-searches and the engine's own truce
    // culls consult. The legacy CDiplomaticRelation walk (diplomatic_data
    // +0x6A0) was proven to MISS engine truces (verify-round3 §5.4: truces:[]
    // for a live scripted truce the engine enforces), so it no longer decides
    // membership; it is kept as the expiry/reason enrichment source and as a
    // supplementary emitter for any relation-walk entry the engine list lacks
    // (tagged with its source so consumers can weigh it).
    std::vector<uint32_t> engine_truce_ids;
    const bool engine_probe_ok =
        collect_engine_truce_partners(character, engine_truce_ids);

    r.key("truces"); r.begin_array();
    int truce_count = 0;
    int engine_truce_count = 0;
    for (uint32_t pid : engine_truce_ids) {
        r.begin_object();
        r.key("target_id"); r.value_uint(pid);
        r.key("source"); r.value_string("engine_truce_list");
        // End date if reachable: the engine char-ptr list carries no expiry, so
        // enrich from the relation walk — try our side first, then the
        // partner's (a scripted truce may be recorded on one side only). An
        // unreachable expiry is surfaced typed, never silently omitted.
        int32_t end_raw = 0;
        char end_str[32] = {};
        bool have_expiry =
            probe_truce_expiry(character, pid, end_raw, end_str, sizeof(end_str));
        if (!have_expiry) {
            void* pc = lookup_character(pid);
            if (pc && !is_sentinel(pc)) {
                have_expiry =
                    probe_truce_expiry(pc, my_id, end_raw, end_str, sizeof(end_str));
            }
        }
        if (have_expiry) {
            r.key("end_date_raw"); r.value_int(end_raw);
            r.key("end_date"); r.value_string(end_str);
        } else {
            r.key("end_date_unavailable"); r.value_bool(true);
        }
        r.end_object();
        truce_count++;
        engine_truce_count++;
    }

    // Legacy relation-walk entries (expiry+reason-rich) for partners the engine
    // list did not already cover.
    {
        uintptr_t diplo = safe_read<uintptr_t>(c + ck3::character::DIPLOMATIC_DATA);
        if (diplo && (diplo && is_mapped(diplo))) {
            uintptr_t truce_arr = safe_read<uintptr_t>(diplo + ck3::diplomatic_data::TRUCE_LIST_PTR);
            int32_t tc = safe_read<int32_t>(diplo + ck3::diplomatic_data::TRUCE_LIST_COUNT);
            if (tc < 0 || tc > 200) tc = 0;

            if (truce_arr && (truce_arr && is_mapped(truce_arr)) && tc > 0) {
                for (int32_t i = 0; i < tc; i++) {
                    uintptr_t rel = safe_read<uintptr_t>(truce_arr + i * 8);
                    if (!rel || (!rel || !is_mapped(rel))) continue;

                    uint32_t cid1 = safe_read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_LOW);
                    uint32_t cid2 = safe_read<uint32_t>(rel + ck3::diplomatic_relation::CHAR_ID_HIGH);

                    uint32_t target_id;
                    size_t expiry_off, reason_off;
                    if ((cid1 & 0xFFFFFF) == my_id) {
                        target_id = cid2 & 0xFFFFFF;
                        expiry_off = ck3::diplomatic_relation::TRUCE_EXPIRY_A;
                        reason_off = ck3::diplomatic_relation::TRUCE_REASON_TYPE_A;
                    } else if ((cid2 & 0xFFFFFF) == my_id) {
                        target_id = cid1 & 0xFFFFFF;
                        expiry_off = ck3::diplomatic_relation::TRUCE_EXPIRY_B;
                        reason_off = ck3::diplomatic_relation::TRUCE_REASON_TYPE_B;
                    } else {
                        continue;
                    }

                    // ck-ul5i: already emitted from the engine list above.
                    if (std::find(engine_truce_ids.begin(), engine_truce_ids.end(),
                                  target_id) != engine_truce_ids.end()) continue;

                    int32_t expiry = safe_read<int32_t>(rel + expiry_off);
                    int32_t reason = safe_read<int32_t>(rel + reason_off);

                    // reason==3 means invalid/expired — skip
                    if (reason == ck3::diplomatic_relation::TRUCE_REASON_INVALID) continue;
                    // Skip expired truces (expiry < current_date)
                    if (current_date_raw > 0 && expiry > 0 && expiry < current_date_raw) continue;

                    r.begin_object();
                    r.key("target_id"); r.value_uint(target_id);
                    r.key("source"); r.value_string("diplomatic_relation_walk");
                    r.key("end_date_raw"); r.value_int(expiry);
                    char date_buf[32];
                    format_date(expiry, date_buf, sizeof(date_buf));
                    r.key("end_date"); r.value_string(date_buf);
                    const char* reason_str = (reason == 0) ? "attacker" :
                                             (reason == 1) ? "defender" :
                                             (reason == 2) ? "ally" : "unknown";
                    r.key("reason"); r.value_string(reason_str);

                    // Try to read the truce reason string (std::string at the reason_str offset)
                    size_t reason_str_off = (expiry_off == ck3::diplomatic_relation::TRUCE_EXPIRY_A)
                        ? ck3::diplomatic_relation::TRUCE_REASON_STR_A
                        : ck3::diplomatic_relation::TRUCE_REASON_STR_B;
                    std::string war_name = ad_read_std_string(rel + reason_str_off);
                    if (!war_name.empty()) {
                        r.key("war_name"); r.value_string(war_name.c_str(), war_name.size());
                    }

                    r.end_object();
                    truce_count++;
                }
            }
        }
    }
    r.end_array();
    r.key("truce_count"); r.value_int(truce_count);
    r.key("engine_truce_count"); r.value_int(engine_truce_count);
    // Honesty flag (ck-ul5i): "unavailable" = the GET_TRUCE_LIST read could not
    // run (getter/list unmapped or faulted) — engine truces may be missing from
    // truces[] and in_truce enforcement can still refuse a war this reply does
    // not predict. Never silently empty.
    r.key("engine_truce_probe");
    r.value_string(engine_probe_ok ? "ok" : "unavailable");

    // ---- 2. Alliances (from core_data+0x20 relation container) ----
    r.key("alliances"); r.begin_array();
    int alliance_count = 0;
    {
        uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
        if (core_data && (core_data && is_mapped(core_data))) {
            uintptr_t rel_arr = safe_read<uintptr_t>(core_data + ck3::relation_container::ARRAY_PTR);
            int32_t rel_count = safe_read<int32_t>(core_data + ck3::relation_container::COUNT);
            if (rel_count < 0 || rel_count > 500) rel_count = 0;

            if (rel_arr && (rel_arr && is_mapped(rel_arr)) && rel_count > 0) {
                for (int32_t i = 0; i < rel_count; i++) {
                    uintptr_t entry_base = rel_arr + i * ck3::relation_container::ENTRY_STRIDE;
                    uint32_t other_id = safe_read<uint32_t>(entry_base + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
                    uintptr_t rel_ptr = safe_read<uintptr_t>(entry_base + ck3::relation_container::ENTRY_PTR);
                    if (!rel_ptr || (!rel_ptr || !is_mapped(rel_ptr))) continue;

                    // Check if this relation has an active alliance
                    int32_t reason_count = safe_read<int32_t>(rel_ptr + ck3::alliance::REASON_COUNT);
                    if (reason_count <= 0 || reason_count > 20) continue;

                    // Check "alliance broken" flag
                    uint8_t broken = safe_read<uint8_t>(rel_ptr + ck3::alliance::BROKEN_FLAG);
                    if (broken) continue;

                    r.begin_object();
                    r.key("ally_id"); r.value_uint(other_id);
                    r.key("reason_count"); r.value_int(reason_count);

                    // Read alliance reasons
                    r.key("reasons"); r.begin_array();
                    uintptr_t reason_arr = safe_read<uintptr_t>(rel_ptr + ck3::alliance::REASON_ARRAY);
                    if (reason_arr && (reason_arr && is_mapped(reason_arr))) {
                        for (int32_t j = 0; j < reason_count && j < 10; j++) {
                            uintptr_t re = reason_arr + j * ck3::alliance::REASON_ENTRY_SIZE;
                            uint32_t through1 = safe_read<uint32_t>(re + ck3::alliance::REASON_CHAR_REF_1) & 0xFFFFFF;
                            uint32_t through2 = safe_read<uint32_t>(re + ck3::alliance::REASON_CHAR_REF_2) & 0xFFFFFF;
                            r.begin_object();
                            r.key("through_char_1"); r.value_uint(through1);
                            r.key("through_char_2"); r.value_uint(through2);
                            r.end_object();
                        }
                    }
                    r.end_array();

                    r.end_object();
                    alliance_count++;
                }
            }
        }
    }
    r.end_array();
    r.key("alliance_count"); r.value_int(alliance_count);

    // ---- 3. Hooks ----
    // Hook data is now available via the dedicated query_hooks endpoint.
    r.key("hooks_note"); r.value_string("Use query_hooks for detailed hook data.");

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_hooks — List all hooks held by or on a character
// Iterates the CDiplomaticRelation sorted array at core_data+0x20. For each
// relation, checks both hook slots (+0x10 for charA, +0x18 for charB).
// Pure memory reads, safe while paused.
// =============================================================================
bool do_query_hooks(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = R"({"error":"character_not_found"})";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_hooks");
    r.key("character_id"); r.value_uint(my_id);

    // Game date for context
    int32_t current_date_raw = 0;
    {
        uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
        if (gs) current_date_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    }

    auto format_date = [](int32_t raw_date, char* buf, size_t bufsz) {
        ck3::game_date::format_raw(raw_date, buf, bufsz);  // ck-05mg
    };

    r.key("hooks"); r.begin_array();
    int hook_count = 0;

    uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
    if (core_data && (core_data && is_mapped(core_data))) {
        uintptr_t rel_arr = safe_read<uintptr_t>(core_data + ck3::relation_container::ARRAY_PTR);
        int32_t rel_count = safe_read<int32_t>(core_data + ck3::relation_container::COUNT);
        if (rel_count < 0 || rel_count > 500) rel_count = 0;

        if (rel_arr && (rel_arr && is_mapped(rel_arr)) && rel_count > 0) {
            for (int32_t i = 0; i < rel_count; i++) {
                uintptr_t entry_base = rel_arr + i * ck3::relation_container::ENTRY_STRIDE;
                uint32_t other_id = safe_read<uint32_t>(entry_base + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
                uintptr_t rel_ptr = safe_read<uintptr_t>(entry_base + ck3::relation_container::ENTRY_PTR);
                if (!rel_ptr || (!rel_ptr || !is_mapped(rel_ptr))) continue;

                // Determine which side we are: charA (CHAR_ID_LOW) or charB (CHAR_ID_HIGH)
                uint32_t char_a_id = safe_read<uint32_t>(rel_ptr + ck3::diplomatic_relation::CHAR_ID_LOW) & 0xFFFFFF;
                uint32_t char_b_id = safe_read<uint32_t>(rel_ptr + ck3::diplomatic_relation::CHAR_ID_HIGH) & 0xFFFFFF;

                bool i_am_a = (char_a_id == my_id);
                bool i_am_b = (char_b_id == my_id);
                if (!i_am_a && !i_am_b) continue;  // Should not happen, but guard

                // Hook I hold on the other character
                uintptr_t my_hook_ptr = safe_read<uintptr_t>(rel_ptr +
                    (i_am_a ? ck3::diplomatic_relation::HOOK_PTR_A
                            : ck3::diplomatic_relation::HOOK_PTR_B));

                // Hook the other character holds on me
                uintptr_t their_hook_ptr = safe_read<uintptr_t>(rel_ptr +
                    (i_am_a ? ck3::diplomatic_relation::HOOK_PTR_B
                            : ck3::diplomatic_relation::HOOK_PTR_A));

                // Emit hook I hold on the other character
                if (my_hook_ptr && (my_hook_ptr && is_mapped(my_hook_ptr))) {
                    uintptr_t hook_type_ptr = safe_read<uintptr_t>(my_hook_ptr + ck3::active_hook::HOOK_TYPE_PTR);
                    bool is_strong = false;
                    bool is_perpetual = false;
                    if (hook_type_ptr && (hook_type_ptr && is_mapped(hook_type_ptr))) {
                        is_strong = safe_read<uint8_t>(hook_type_ptr + ck3::hook_type::IS_STRONG) != 0;
                        is_perpetual = safe_read<uint8_t>(hook_type_ptr + ck3::hook_type::IS_PERPETUAL) != 0;
                    }

                    int64_t expiry_raw = safe_read<int64_t>(my_hook_ptr + ck3::active_hook::EXPIRY_DATE);
                    int64_t end_raw = safe_read<int64_t>(my_hook_ptr + ck3::active_hook::END_DATE);
                    uint32_t secret_ref = safe_read<uint32_t>(my_hook_ptr + ck3::active_hook::BACKING_SECRET_REF);

                    r.begin_object();
                    r.key("other_id"); r.value_uint(other_id);
                    r.key("direction"); r.value_string("held_by_me");
                    r.key("is_strong"); r.value_bool(is_strong);
                    r.key("is_perpetual"); r.value_bool(is_perpetual);

                    if (expiry_raw > 0) {
                        r.key("expiry_date_raw"); r.value_int(static_cast<int32_t>(expiry_raw));
                        char date_buf[32];
                        format_date(static_cast<int32_t>(expiry_raw), date_buf, sizeof(date_buf));
                        r.key("expiry_date"); r.value_string(date_buf);
                    }
                    if (end_raw > 0) {
                        r.key("end_date_raw"); r.value_int(static_cast<int32_t>(end_raw));
                        char date_buf[32];
                        format_date(static_cast<int32_t>(end_raw), date_buf, sizeof(date_buf));
                        r.key("end_date"); r.value_string(date_buf);
                    }
                    if (secret_ref != 0) {
                        r.key("backing_secret_ref"); r.value_uint(secret_ref & 0xFFFFFF);
                    }

                    r.end_object();
                    hook_count++;
                }

                // Emit hook they hold on me
                if (their_hook_ptr && (their_hook_ptr && is_mapped(their_hook_ptr))) {
                    uintptr_t hook_type_ptr = safe_read<uintptr_t>(their_hook_ptr + ck3::active_hook::HOOK_TYPE_PTR);
                    bool is_strong = false;
                    bool is_perpetual = false;
                    if (hook_type_ptr && (hook_type_ptr && is_mapped(hook_type_ptr))) {
                        is_strong = safe_read<uint8_t>(hook_type_ptr + ck3::hook_type::IS_STRONG) != 0;
                        is_perpetual = safe_read<uint8_t>(hook_type_ptr + ck3::hook_type::IS_PERPETUAL) != 0;
                    }

                    int64_t expiry_raw = safe_read<int64_t>(their_hook_ptr + ck3::active_hook::EXPIRY_DATE);
                    int64_t end_raw = safe_read<int64_t>(their_hook_ptr + ck3::active_hook::END_DATE);
                    uint32_t secret_ref = safe_read<uint32_t>(their_hook_ptr + ck3::active_hook::BACKING_SECRET_REF);

                    r.begin_object();
                    r.key("other_id"); r.value_uint(other_id);
                    r.key("direction"); r.value_string("held_on_me");
                    r.key("is_strong"); r.value_bool(is_strong);
                    r.key("is_perpetual"); r.value_bool(is_perpetual);

                    if (expiry_raw > 0) {
                        r.key("expiry_date_raw"); r.value_int(static_cast<int32_t>(expiry_raw));
                        char date_buf[32];
                        format_date(static_cast<int32_t>(expiry_raw), date_buf, sizeof(date_buf));
                        r.key("expiry_date"); r.value_string(date_buf);
                    }
                    if (end_raw > 0) {
                        r.key("end_date_raw"); r.value_int(static_cast<int32_t>(end_raw));
                        char date_buf[32];
                        format_date(static_cast<int32_t>(end_raw), date_buf, sizeof(date_buf));
                        r.key("end_date"); r.value_string(date_buf);
                    }
                    if (secret_ref != 0) {
                        r.key("backing_secret_ref"); r.value_uint(secret_ref & 0xFFFFFF);
                    }

                    r.end_object();
                    hook_count++;
                }
            }
        }
    }

    r.end_array();
    r.key("hook_count"); r.value_int(hook_count);

    if (current_date_raw > 0) {
        char date_buf[32];
        format_date(current_date_raw, date_buf, sizeof(date_buf));
        r.key("as_of_date"); r.value_string(date_buf);
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_available_cbs — List casus belli types and cross-reference with claims
// Pure memory reads only. Returns all CB types from the database plus which
// claims the actor holds that could potentially be used. Does NOT validate
// CB availability against a specific target (that requires calling game functions).
// =============================================================================
bool do_query_available_cbs(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = R"({"error":"character_not_found"})";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    // Optional target_id parameter
    int target_id_param = json_get_int(params_json, "target_id");
    uint32_t target_id = (target_id_param > 0) ? static_cast<uint32_t>(target_id_param) : 0;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_available_cbs");
    r.key("character_id"); r.value_uint(my_id);
    if (target_id > 0) {
        r.key("target_id"); r.value_uint(target_id);
    }

    // ---- 1. Character's claims (potential CB fuel) ----
    r.key("claims"); r.begin_array();
    int claim_count_total = 0;
    {
        uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
        if (core_data && is_mapped(core_data)) {
            uintptr_t claims_base = core_data + ck3::core_data::CLAIMS_ARRAY;
            if (!is_mapped_range(claims_base, 0x10)) goto claims_done;
            uintptr_t claims_data = safe_read<uintptr_t>(claims_base + 0x00);
            int32_t cc = safe_read<int32_t>(claims_base + 0x0C);
            if (cc < 0 || cc > 500) cc = 0;

            if (claims_data && is_mapped(claims_data) && cc > 0) {
                for (int32_t i = 0; i < cc && i < 50; i++) {
                    uintptr_t claim = claims_data + i * ck3::claim_entry::ENTRY_SIZE;
                    if (!is_mapped_range(claim, ck3::claim_entry::ENTRY_SIZE)) continue;
                    uint32_t title_ref = safe_read<uint32_t>(claim + ck3::claim_entry::TITLE_ID);
                    uint8_t pressed = safe_read<uint8_t>(claim + ck3::claim_entry::PRESSED);
                    uint8_t implicit = safe_read<uint8_t>(claim + ck3::claim_entry::IMPLICIT);

                    if (title_ref == 0 || title_ref == 0xFFFFFFFF) continue;

                    r.begin_object();
                    r.key("title_id"); r.value_uint(title_ref & 0xFFFFFF);
                    r.key("pressed"); r.value_bool(pressed != 0);
                    r.key("implicit"); r.value_bool(implicit != 0);

                    // Try to get title name — CLandedTitle has display name at +0x58,
                    // NOT the db_object +0x10 pattern (that's TITLE_ID on titles)
                    uintptr_t title_obj = lookup_title_obj(title_ref);
                    if (title_obj && is_mapped(title_obj)) {
                        std::string tname = ad_read_std_string(
                            title_obj + ck3::title_obj::TITLE_DISPLAY_NAME);
                        if (!tname.empty()) {
                            r.key("title_key"); r.value_string(tname.c_str(), tname.size());
                        }
                        int tier = get_title_tier(title_obj);
                        r.key("tier"); r.value_int(tier);

                        // If target specified, check if this title's holder matches
                        if (target_id > 0) {
                            // The title's holder (title+0x1B0, read here before P5, is not a
                            // holder: 0 on 902 of 907 sampled titles of the 867 core).
                            uint32_t holder_ref = safe_read<uint32_t>(title_obj + ck3::title_obj::HOLDER_CHAR_ID);
                            bool matches_target = ((holder_ref & 0xFFFFFF) == target_id);
                            r.key("held_by_target"); r.value_bool(matches_target);
                        }
                    }

                    r.end_object();
                    claim_count_total++;
                }
            }
        }
    }
    claims_done:
    r.end_array();
    r.key("claim_count"); r.value_int(claim_count_total);

    // ---- 2. CB type database enumeration ----
    r.key("cb_types"); r.begin_array();
    int cb_type_count = 0;
    {
        uintptr_t cb_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CASUS_BELLI_DB);
        if (cb_db && is_mapped(cb_db)) {
            uintptr_t cb_arr = safe_read<uintptr_t>(cb_db + ck3::game_db::ENTRIES_ARRAY);
            int32_t cb_count = safe_read<int32_t>(cb_db + ck3::game_db::LIVE_COUNT);
            if (cb_count < 0 || cb_count > 500) cb_count = 0;

            if (cb_arr && is_mapped(cb_arr)) {
                for (int32_t i = 0; i < cb_count && i < 100; i++) {
                    uintptr_t cb_type = safe_read<uintptr_t>(cb_arr + i * 8);
                    if (!cb_type || !is_mapped(cb_type)) continue;

                    uint8_t enabled = safe_read<uint8_t>(cb_type + ck3::cb_type::ENABLED_FLAG);
                    if (!enabled) continue;

                    r.begin_object();

                    uint32_t cb_idx = safe_read<uint32_t>(cb_type + ck3::cb_type::DB_INDEX);
                    r.key("index"); r.value_uint(cb_idx);

                    // CB name at +0x10 (ptr to string)
                    const char* cb_name = nullptr;
                    size_t cb_name_len = 0;
                    if (read_db_obj_name(cb_type, cb_name, cb_name_len)) {
                        r.key("name"); r.value_string(cb_name, cb_name_len);
                    }

                    // Target mode (0=none, 6=claim, other=title)
                    int32_t target_mode = safe_read<int32_t>(cb_type + ck3::cb_type::TARGET_MODE);
                    const char* mode_str = (target_mode == 0) ? "none" :
                                           (target_mode == 6) ? "claim" : "title";
                    r.key("target_mode"); r.value_string(mode_str);

                    r.end_object();
                    cb_type_count++;
                }
            }
        }
    }
    r.end_array();
    r.key("cb_type_count"); r.value_int(cb_type_count);

    r.key("note"); r.value_string(
        "CB availability against a specific target requires game function calls. "
        "Claims listed above are CBs the character can potentially use. "
        "Use declare_war to let the AI select the best available CB.");

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_valid_cbs — Validate which Casus Belli are available for attacker vs defender
// Requires game thread execution (trigger evaluator reads game state).
// Uses deferred query pattern: IPC thread submits, game thread evaluates,
// IPC thread polls for result.
// =============================================================================

// Emit the valid_cbs[] array + read-side declarability honesty for
// do_query_valid_cbs. Shared by BOTH emit paths (cached-poll + post-submit-wait)
// so the honesty layer can never drift between them. Engine verdict (cb.valid)
// is preserved UNCHANGED; the layer is purely ADDITIVE (target_state{} + per-CB
// declarable/cull_reason for claim CBs + declarable_count). Mirrors the
// shipped strict surface (do_query_cb_validity_strict 7387-7507). PURE reads.
static void emit_valid_cbs_with_honesty(ResponseBuilder& r,
                                        uint32_t actor_id, uint32_t target_id) {
    void* actor_char = lookup_character(actor_id);
    void* target_char = lookup_character(target_id);
    DefenderState tstate;
    bool actor_pressed = false, in_alliance = false, in_truce = false;
    bool already_at_war = false;
    if (target_char && !is_sentinel(target_char)) {
        tstate = probe_defender_state(reinterpret_cast<uintptr_t>(target_char));
    }
    if (actor_char && !is_sentinel(actor_char)) {
        actor_pressed = attacker_has_pressed_claim_on(
            reinterpret_cast<uintptr_t>(actor_char), target_id);
        probe_alliance_truce(actor_char, target_char, in_alliance, in_truce);
        already_at_war = actor_at_war_opposing(
            reinterpret_cast<uintptr_t>(actor_char), target_id);
    }

    r.key("target_state"); r.begin_object();
    r.key("independent");             r.value_bool(tstate.independent);
    if (!tstate.independent) { r.key("liege_id"); r.value_uint(tstate.liege_id); }
    r.key("imprisoned");              r.value_bool(tstate.imprisoned);
    r.key("in_alliance");             r.value_bool(in_alliance);
    r.key("in_truce");                r.value_bool(in_truce);
    r.key("already_at_war_opposing"); r.value_bool(already_at_war);
    r.key("pressed_claim_on_target"); r.value_bool(actor_pressed);
    r.end_object();

    r.key("valid_cbs"); r.begin_array();
    int valid_count = 0;
    int declarable_count = 0;
    for (const auto& cb : g_cb_query.results) {
        if (!cb.valid) continue;
        r.begin_object();
        r.key("cb_index"); r.value_uint(cb.cb_index);
        r.key("cb_name"); r.value_string(cb.cb_name);
        const char* mode_str = (cb.target_mode == 0) ? "none" :
                               (cb.target_mode == 6) ? "claim" : "title";
        r.key("target_mode"); r.value_string(mode_str);
        // Declarability honesty: only for claim CBs (mirror write-path scoping,
        // intent_dispatch.cpp:1455). Engine cb.valid is left untouched.
        const bool is_claim = (cb.target_mode == 6) || (strcmp(cb.cb_name, "claim_cb") == 0);
        if (is_claim) {
            const char* cull = nullptr;
            if (!actor_pressed)            cull = "claim_not_pressed";
            else if (!tstate.independent)  cull = "target_not_independent";
            else if (tstate.imprisoned)    cull = "target_imprisoned";
            else if (in_alliance)          cull = "target_in_alliance";
            else if (in_truce)             cull = "target_in_truce";
            else if (already_at_war)       cull = "already_at_war_opposing";
            bool declarable = (cull == nullptr);
            r.key("declarable"); r.value_bool(declarable);
            if (cull) { r.key("cull_reason"); r.value_string(cull); }
            if (declarable) declarable_count++;
        }
        r.end_object();
        valid_count++;
    }
    r.end_array();
    r.key("valid_count"); r.value_int(valid_count);
    // The agent should gate claim_cb declarations on declarable_count, not
    // valid_count — a CB can be engine-valid yet culled to war_count 0 on an
    // unpressed claim / bad target state (re_cb_validation_deep_dive 3.6).
    r.key("declarable_count"); r.value_int(declarable_count);
    r.key("total_checked"); r.value_int(static_cast<int>(g_cb_query.results.size()));
}

bool do_query_valid_cbs(const char* params_json, std::string* out) {
    // Resolve actor_id (defaults to controlled character)
    int actor_id = json_get_int(params_json, "actor_id");
    if (actor_id <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id <= 0) {
        if (out) *out = R"({"error":"no_actor_available"})";
        return false;
    }

    int target_id = json_get_int(params_json, "target_id");
    if (target_id <= 0) {
        if (out) *out = "{\"error\":\"missing_target_id\",\"hint\":\"Provide target_id - defender character ID\"}";
        return false;
    }

    // Check if a query is already in flight
    int current_state = g_cb_query.state.load(std::memory_order_acquire);
    if (current_state == 1 || current_state == 4) {
        if (out) *out = R"({"error":"cb_query_busy","hint":"Another CB query is already pending. Wait for it to complete."})";
        return true;
    }

    // If there's a completed result, check if it matches our request (poll)
    if (current_state == 2 || current_state == 3) {
        bool matches = (g_cb_query.actor_id == static_cast<uint32_t>(actor_id) &&
                       g_cb_query.target_id == static_cast<uint32_t>(target_id));

        if (matches) {
            ResponseBuilder r;
            r.begin_object();
            r.key("action"); r.value_string("query_valid_cbs");
            r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
            r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);

            if (current_state == 3) {
                r.key("error"); r.value_string(g_cb_query.error);
                r.key("valid_cbs"); r.begin_array(); r.end_array();
            } else {
                emit_valid_cbs_with_honesty(r, static_cast<uint32_t>(actor_id),
                                            static_cast<uint32_t>(target_id));
            }

            r.key("deferred"); r.value_bool(true);
            r.key("status"); r.value_string("ok");
            r.end_object();
            if (out) *out = r.take();

            // Clear the slot for the next query
            g_cb_query.results.clear();
            g_cb_query.state.store(0, std::memory_order_release);
            return true;
        } else {
            // Previous result doesn't match — clear and submit new
            g_cb_query.results.clear();
            g_cb_query.state.store(0, std::memory_order_release);
        }
    }

    // Submit a new deferred query
    g_cb_query.actor_id = static_cast<uint32_t>(actor_id);
    g_cb_query.target_id = static_cast<uint32_t>(target_id);
    g_cb_query.strict_mode = json_get_bool(params_json, "strict_mode", false);
    g_cb_query.results.clear();
    g_cb_query.error[0] = '\0';
    g_cb_query.service_start_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_end_ns.store(0, std::memory_order_relaxed);
    g_cb_query.service_tick.store(0, std::memory_order_relaxed);

    const uint64_t submit_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    g_cb_query.submit_ns.store(submit_ns, std::memory_order_relaxed);
    g_cb_query.submit_tick.store(g_tick_id.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);

    // Publish the query (game thread will pick it up next tick)
    g_cb_query.state.store(1, std::memory_order_release);

    ADLOG("query_valid_cbs: deferred query submitted: actor=%d target=%d submit_tick=%lu\n",
          actor_id, target_id,
          (unsigned long)g_cb_query.submit_tick.load(std::memory_order_relaxed));

    // Wait for the game thread to process it (up to ~3 seconds, polling every 50ms)
    // CB iteration is heavier than a single acceptance check, so allow more time.
    for (int i = 0; i < 60; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int st = g_cb_query.state.load(std::memory_order_acquire);
        if (st == 2 || st == 3) {
            // Result ready
            ResponseBuilder r;
            r.begin_object();
            r.key("action"); r.value_string("query_valid_cbs");
            r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
            r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);

            if (st == 3) {
                r.key("error"); r.value_string(g_cb_query.error);
                r.key("valid_cbs"); r.begin_array(); r.end_array();
            } else {
                emit_valid_cbs_with_honesty(r, static_cast<uint32_t>(actor_id),
                                            static_cast<uint32_t>(target_id));
            }

            r.key("deferred"); r.value_bool(true);
            r.key("wait_ms"); r.value_int((i + 1) * 50);
            r.key("status"); r.value_string("ok");
            r.end_object();
            if (out) *out = r.take();

            // Clear the slot
            g_cb_query.results.clear();
            g_cb_query.state.store(0, std::memory_order_release);
            return true;
        }
    }

    // Timeout — snapshot diagnostic state so caller can discriminate
    // race (state==1 final, game thread never drained) vs hang (state==4,
    // process_cb_query started but did not finish) vs empty (state==2 but
    // we missed the transition — shouldn't happen with 50ms polling).
    const int final_state = g_cb_query.state.load(std::memory_order_acquire);
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t submit_ns_val = g_cb_query.submit_ns.load(std::memory_order_relaxed);
    const uint64_t svc_start_ns = g_cb_query.service_start_ns.load(std::memory_order_relaxed);
    const uint64_t svc_end_ns = g_cb_query.service_end_ns.load(std::memory_order_relaxed);
    const uint64_t now_tick = g_tick_id.load(std::memory_order_relaxed);
    const uint64_t submit_tick_val = g_cb_query.submit_tick.load(std::memory_order_relaxed);
    const uint64_t service_tick_val = g_cb_query.service_tick.load(std::memory_order_relaxed);

    const uint64_t elapsed_ms = submit_ns_val ? (now_ns - submit_ns_val) / 1000000ULL : 0;
    const uint64_t service_latency_ms = (svc_start_ns && submit_ns_val)
        ? (svc_start_ns - submit_ns_val) / 1000000ULL : 0;
    const uint64_t service_duration_ms = (svc_end_ns && svc_start_ns)
        ? (svc_end_ns - svc_start_ns) / 1000000ULL
        : (svc_start_ns ? (now_ns - svc_start_ns) / 1000000ULL : 0);
    const uint64_t ticks_observed = (now_tick >= submit_tick_val)
        ? (now_tick - submit_tick_val) : 0;

    const char* classification;
    if (final_state == 1) {
        classification = "never_serviced";   // hypothesis 1: race (no tick drained)
    } else if (final_state == 4) {
        classification = "service_in_progress";  // hypothesis 2: hang inside process_cb_query
    } else if (final_state == 2 || final_state == 3) {
        classification = "completed_after_timeout";  // poll loop missed the transition
    } else {
        classification = "unknown";
    }

    ADLOG("query_valid_cbs: TIMEOUT final_state=%d classification=%s elapsed_ms=%lu "
          "service_latency_ms=%lu service_duration_ms=%lu ticks_observed=%lu\n",
          final_state, classification,
          (unsigned long)elapsed_ms, (unsigned long)service_latency_ms,
          (unsigned long)service_duration_ms, (unsigned long)ticks_observed);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_valid_cbs");
    r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);
    r.key("error"); r.value_string("timeout_waiting_for_game_thread");
    r.key("hint"); r.value_string("Game must be unpaused for at least one tick. "
                                   "The query runs during drain_and_execute_intents on the game thread.");
    r.key("deferred"); r.value_bool(true);
    r.key("status"); r.value_string("timeout");

    // ck-wc9w instrumentation: classify race / hang / empty
    r.key("diagnostic"); r.begin_object();
    r.key("final_state"); r.value_int(final_state);
    r.key("classification"); r.value_string(classification);
    r.key("elapsed_ms"); r.value_uint(elapsed_ms);
    r.key("service_latency_ms"); r.value_uint(service_latency_ms);
    r.key("service_duration_ms"); r.value_uint(service_duration_ms);
    r.key("submit_tick"); r.value_uint(submit_tick_val);
    r.key("service_tick"); r.value_uint(service_tick_val);
    r.key("current_tick"); r.value_uint(now_tick);
    r.key("ticks_observed"); r.value_uint(ticks_observed);

    // Partial per-CB timings if the game thread was mid-service.
    r.key("partial_results"); r.begin_array();
    for (const auto& cb : g_cb_query.results) {
        r.begin_object();
        r.key("cb_index"); r.value_uint(cb.cb_index);
        r.key("cb_name"); r.value_string(cb.cb_name);
        r.key("valid"); r.value_bool(cb.valid);
        r.key("eval_us"); r.value_uint(cb.eval_us);
        r.end_object();
    }
    r.end_array();
    r.end_object();

    r.end_object();
    if (out) *out = r.take();

    return true;
}

// =============================================================================
// query_cb_validity_strict — per-CB outer/inner verdict surface (ck-5qpg)
// =============================================================================
//
// Mirrors the spec wire format in re/specs/casus-belli/bridge-tools-proposed.md.
// Drives the same Stage 1 (bilateral) + Stage 2 (group-flag) outer filter as
// cb_query_count_valid_strict, but copies the per-CB result vector out of the
// shared g_cb_query slot so we can emit per-CB outer_status / outer_reason /
// inner_status fields in the response.
//
// Per-CB outer triggers (allowed_for_character / allowed_against_character /
// valid_to_start) and the distance / neighbor gates are tracked under ck-39e1
// and not yet wired here. CB entries that pass our current outer filter still
// run through CB_VALIDITY_CHECK (the engine's full inner pipeline), so this
// tool is strictly more informative than cb_query_count_valid_sync and never
// less safe than the existing strict variant.

// ── PAUSE SIGNAL (unified gateway, _HARDENING_PLAN FIX 3) ────────────────────
// The strict-CB family (do_query_cb_validity_strict / do_query_cbs_for_target)
// dispatches CB_VALIDITY_CHECK to the GAME THREAD via the g_cb_query slot, which
// is only serviced inside drain_and_execute_intents() — i.e. on_tick, which only
// fires while the game is UNPAUSED. Called while paused, cb_query_strict_full_sync
// would submit and then poll until its timeout (rc=-3) — a multi-second block for
// every paused caller.
//
// Instead we short-circuit: if the game is paused, return a FAST structured
// {status:"needs_unpaused"} so the MCP gateway can deterministically flip one tick
// and restore tempo, rather than absorbing a timeout. Pure atomic read of the
// pause flag — no game-object traversal, crash-proof. Returns true (a well-formed
// envelope, not an error) so the gateway treats it as a clean polarity signal.
static bool emit_needs_unpaused(const char* action, std::string* out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action");  r.value_string(action);
    r.key("status");  r.value_string("needs_unpaused");
    r.key("paused");  r.value_bool(true);
    r.key("hint");    r.value_string(
        "Engine CB validity runs on the game thread (drained on tick). "
        "Unpause one tick, re-issue, then restore tempo.");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// Per-CB precondition classifier + honest cull_reason (re_cb_adjacency_matrix.md)
//
// The engine's CB_VALIDITY_CHECK already enforces the adjacency / de-jure-liege /
// claim / faith / government gate for EVERY CB inside +0x118 (allowed_against)
// and +0x1f8 (valid_to_start), evaluated synchronously (skip_against=0). So a
// CB's inner_status=fail ALREADY incorporates that gate — we NEVER recompute or
// override the engine verdict. We only LABEL a fail the engine already produced
// with a typed cull_reason, derived from the SAME read-side probes the claim
// branch uses, so the agent learns WHY (not_realm_adjacent vs no_de_jure_claim vs
// claim_not_pressed vs faith_gate vs not_tribal_government vs target_not_independent
// vs in_truce/alliance) and can pick the right remediation. Purely additive: the
// engine `valid`/`inner_status` are unchanged; this is a LEGIBILITY string only.
// =============================================================================
enum class CBClass { ADJACENCY, DE_JURE, CLAIM, FAITH, GOVERNMENT, NONE };

static CBClass cb_precondition_class(const char* cb_name) {
    // CLAIM first (claim_cb is the only family with no valid_to_start/allowed_against;
    // it is gated purely on a PRESSED claim — handled by the existing claim branch).
    if (strcmp(cb_name, "claim_cb") == 0) return CBClass::CLAIM;
    // ADJACENCY (realm-level + sea-inclusive neighboring_county_or_viking_conquest_trigger):
    // county/duchy/ducal conquest + the kingdom-tier invasion war share this gate.
    if (strstr(cb_name, "conquest") != nullptr) return CBClass::ADJACENCY;
    if (strstr(cb_name, "invasion") != nullptr) return CBClass::ADJACENCY;
    // GOVERNMENT (tribal_subjugation_cb): both-tribal + defender-is-independent-top-liege.
    if (strstr(cb_name, "subjugation") != nullptr) return CBClass::GOVERNMENT;
    // DE_JURE (de_jure_cb / individual_*_de_jure_cb): gated on has_de_jure_claim_on.
    if (strstr(cb_name, "de_jure") != nullptr) return CBClass::DE_JURE;
    // FAITH (religious_war / minor_ / major_ / *_great_holy_war): faith-hostility +
    // attacker-faith-can-holy-war gate.
    if (strstr(cb_name, "religious_war") != nullptr ||
        strstr(cb_name, "holy_war") != nullptr) return CBClass::FAITH;
    return CBClass::NONE;
}

// Does `actor` (a CCharacter*) have a TRIBAL government? Reads title_realm +
// GOVERNMENT_TYPE_PTR → CGovernmentType name std::string @ +0x10 (same read as
// emit_neighbor_info / do_query_conquest_targets ~11618; no new offset). Returns
// false on any unmapped chain (fail-closed to "not tribal" — never upgrades a
// fail to declarable). Cheap: one pointer chase + one std::string read.
static bool cb_actor_is_tribal(void* actor_char) {
    if (!actor_char || is_sentinel(actor_char)) return false;
    uintptr_t tr = safe_read<uintptr_t>(
        reinterpret_cast<uintptr_t>(actor_char) + ck3::character::TITLE_REALM_DATA);
    if (!tr || !is_mapped(tr)) return false;
    uintptr_t gov_obj = safe_read<uintptr_t>(tr + ck3::title_realm::GOVERNMENT_TYPE_PTR);
    if (!gov_obj || !is_mapped(gov_obj)) return false;
    std::string gov = ad_read_std_string(gov_obj + ck3::db_object::NAME_KEY);
    return gov.find("tribal") != std::string::npos;
}

// Does `actor` hold a title that is the de-jure LIEGE (this-or-de-jure-above) of
// ANY title `target` holds? This is the cheap read-only approximation of the
// engine's has_de_jure_claim_on used by the de_jure family's allowed_against gate
// (re_cb_adjacency_matrix.md DE_JURE row). For each title the defender DIRECTLY
// holds, walk DE_JURE_PARENT upward and check whether the actor holds that
// ancestor. Bounded: defender title count capped, de-jure chain capped at tier
// depth. Pure safe_read; returns false on any unmapped chain (fail-closed — never
// upgrades a fail to declarable). NOTE: approximates the engine's relation-backed
// has_de_jure_claim_on by current de-jure topology; the engine verdict (inner_status)
// stays authoritative — this only LABELS a fail it already produced.
static bool cb_actor_has_de_jure_over_target(void* actor_char, uint32_t actor_id,
                                             void* target_char) {
    if (!actor_char || is_sentinel(actor_char)) return false;
    if (!target_char || is_sentinel(target_char)) return false;
    actor_id &= 0xFFFFFF;
    uintptr_t target_tr = safe_read<uintptr_t>(
        reinterpret_cast<uintptr_t>(target_char) + ck3::character::TITLE_REALM_DATA);
    if (!target_tr || !is_mapped(target_tr)) return false;
    uintptr_t arr = safe_read<uintptr_t>(target_tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t cnt = safe_read<int32_t>(target_tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (cnt < 0 || cnt > 500) cnt = 0;
    if (!arr || !is_mapped(arr)) return false;
    for (int32_t i = 0; i < cnt; i++) {
        uint32_t tid = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
        // Walk de-jure-parents upward from this defender-held title; if any
        // ancestor is held by the actor, the actor is its de-jure liege.
        uint32_t cur = tid;
        for (int depth = 0; depth < 6 && cur != 0; depth++) {
            uintptr_t tobj = lookup_title_obj(cur);
            if (!tobj) break;
            uint32_t holder = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
            if (depth > 0 && holder == actor_id) return true;  // actor holds an ancestor
            uint32_t parent = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
            if (parent == cur) break;  // self-loop guard
            cur = parent;
        }
    }
    return false;
}

// Is ANY county the `target` PERSONALLY holds realm-adjacent to ANY county the
// `actor`'s realm holds (the engine's neighboring_county_or_viking_conquest_trigger,
// realm-level + sea-inclusive per #121/#124)? Bounded BFS over the actor's realm
// (ruler + transitive vassals, same caps as do_query_conquest_targets) to build a
// county-adjacency frontier, then test the target's directly-held counties against
// it. We CANNOT cheaply reproduce the sea/viking coastal OR-branch here, so this
// probe is LAND-conservative: it returns true only on a confirmed land border. We
// therefore ONLY use a false result to emit "not_realm_adjacent" when the engine
// ALSO failed AND the land-adjacency probe found no border — and we never assert
// adjacency to upgrade a fail. The engine inner_status stays authoritative. Pure
// safe_read; caps bound the walk; fails toward "adjacent unknown" (returns
// adjacency true conservatively only on a real hit). Computed ONCE per (actor,
// target) pair, not per-CB.
static bool cb_target_land_adjacent_to_actor_realm(void* actor_char, uint32_t actor_id,
                                                   void* target_char) {
    if (!actor_char || is_sentinel(actor_char)) return false;
    if (!target_char || is_sentinel(target_char)) return false;
    actor_id &= 0xFFFFFF;

    // 1. Build the actor-realm county-adjacency frontier (set of CCountyData*
    //    that border any realm county). Bounded ruler-BFS, mirroring the
    //    do_query_conquest_targets posture.
    std::set<uintptr_t> reach_frontier;
    {
        auto add_neighbors = [&](void* ch) {
            if (!ch || is_sentinel(ch)) return;
            uintptr_t tr = safe_read<uintptr_t>(
                reinterpret_cast<uintptr_t>(ch) + ck3::character::TITLE_REALM_DATA);
            if (!tr || !is_mapped(tr)) return;
            uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
            int32_t cnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
            if (cnt < 0 || cnt > 500) cnt = 0;
            if (!arr || !is_mapped(arr)) return;
            for (int32_t i = 0; i < cnt; i++) {
                uint32_t tid = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
                uintptr_t tobj = lookup_title_obj(tid);
                if (!tobj || get_title_tier(tobj) != 2) continue;  // counties only
                uintptr_t cd = resolve_county_data(tid);
                if (!cd || !is_mapped(cd)) continue;
                uintptr_t nbr = safe_read<uintptr_t>(cd + ck3::county::NEIGHBOR_ARRAY);
                int32_t nc = safe_read<int32_t>(cd + ck3::county::NEIGHBOR_COUNT);
                if (nc < 0 || nc > 100) nc = 0;
                if (!nbr || !is_mapped(nbr)) continue;
                for (int32_t k = 0; k < nc; k++) {
                    uintptr_t ncd = safe_read<uintptr_t>(nbr + k * 8);
                    if (ncd && is_mapped(ncd)) reach_frontier.insert(ncd);
                }
            }
        };
        constexpr int MAX_REALM_RULERS = 2000;
        std::set<uint32_t> visited;
        std::vector<uint32_t> frontier;
        visited.insert(actor_id);
        frontier.push_back(actor_id);
        add_neighbors(actor_char);
        for (size_t qi = 0; qi < frontier.size() && visited.size() < MAX_REALM_RULERS; qi++) {
            uint32_t cur_id = frontier[qi];
            void* cur = (cur_id == actor_id) ? actor_char : lookup_character(cur_id);
            if (!cur || is_sentinel(cur)) continue;
            uintptr_t cur_tr = safe_read<uintptr_t>(
                reinterpret_cast<uintptr_t>(cur) + ck3::character::TITLE_REALM_DATA);
            if (!cur_tr || !is_mapped(cur_tr)) continue;
            uintptr_t varr = cur_tr + ck3::title_realm::DIRECT_VASSAL_ARRAY;
            uintptr_t vdata = safe_read<uintptr_t>(varr);
            int32_t vcnt = safe_read<int32_t>(varr + 0x0C);
            if (!vdata || !is_mapped(vdata) || vcnt <= 0 || vcnt > 500) continue;
            for (int32_t i = 0; i < vcnt; i++) {
                uint32_t vid = safe_read<uint32_t>(vdata + i * 4) & 0xFFFFFF;
                if (vid == 0 || visited.count(vid)) continue;
                visited.insert(vid);
                if (visited.size() >= MAX_REALM_RULERS) break;
                frontier.push_back(vid);
                add_neighbors(lookup_character(vid));
            }
        }
    }
    if (reach_frontier.empty()) return false;

    // 2. Does any county the TARGET personally holds sit on the actor's frontier?
    uintptr_t target_tr = safe_read<uintptr_t>(
        reinterpret_cast<uintptr_t>(target_char) + ck3::character::TITLE_REALM_DATA);
    if (!target_tr || !is_mapped(target_tr)) return false;
    uintptr_t tarr = safe_read<uintptr_t>(target_tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t tcnt = safe_read<int32_t>(target_tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (tcnt < 0 || tcnt > 500) tcnt = 0;
    if (!tarr || !is_mapped(tarr)) return false;
    for (int32_t i = 0; i < tcnt; i++) {
        uint32_t tid = safe_read<uint32_t>(tarr + i * 4) & 0xFFFFFF;
        uintptr_t tobj = lookup_title_obj(tid);
        if (!tobj || get_title_tier(tobj) != 2) continue;
        uintptr_t cd = resolve_county_data(tid);
        if (cd && is_mapped(cd) && reach_frontier.count(cd)) return true;
    }
    return false;
}

// Probes computed ONCE per (actor, target) pair, fed into cb_cull_reason for the
// non-claim families. The claim family keeps its own dedicated cull computation.
struct CBPreconditionProbes {
    bool actor_tribal       = false;  // GOVERNMENT family
    bool has_de_jure        = false;  // DE_JURE family
    bool land_adjacent      = false;  // ADJACENCY family (LAND-confirmed only)
    bool adjacency_probed   = false;  // did we run the land-adjacency BFS?
    // target_state / relation flags (shared with the claim branch).
    bool target_independent = true;
    bool target_imprisoned  = false;
    bool in_alliance        = false;
    bool in_truce           = false;
    bool already_at_war     = false;
};

// Honest cull_reason for a NON-claim CB whose engine inner_status FAILED. Returns
// a typed reason string (never null when inner failed) without ever changing the
// engine verdict. Ordering puts the family-specific gate first, then the universal
// relation/target-state gates that apply to every CB. For ADJACENCY we only assert
// "not_realm_adjacent" when our LAND-confirmed probe found NO border (sea/viking
// reach is not reproduced here, so a coastal target that the engine still failed
// falls through to the relation gates or the generic label rather than a false
// not_adjacent). See re_cb_adjacency_matrix.md "safe surfacing plan".
static const char* cb_cull_reason(CBClass cls, const CBPreconditionProbes& p) {
    // Universal relation/target-state gates (apply to every family the engine
    // failed) take precedence when present — they are HIGH-confidence reads.
    if (p.already_at_war)     return "already_at_war_opposing";
    if (p.in_truce)           return "in_truce";
    if (p.in_alliance)        return "in_alliance";
    switch (cls) {
        case CBClass::ADJACENCY:
            // Only label not-adjacent when the land BFS ran and found no border.
            if (p.adjacency_probed && !p.land_adjacent) return "not_realm_adjacent";
            return "engine_cb_validity_check_failed";
        case CBClass::DE_JURE:
            if (!p.has_de_jure) return "de_jure_not_liege";
            return "engine_cb_validity_check_failed";
        case CBClass::GOVERNMENT:
            if (!p.actor_tribal)        return "not_tribal_government";
            if (!p.target_independent)  return "target_not_independent";
            return "engine_cb_validity_check_failed";
        case CBClass::FAITH:
            // Faith-hostility / attacker-can-holy-war requires faith-DB walks we do
            // not reproduce read-only; surface a typed (still informative) label.
            return "faith_gate";
        case CBClass::CLAIM:
        case CBClass::NONE:
        default:
            return "engine_cb_validity_check_failed";
    }
}

bool do_query_cb_validity_strict(const char* params_json, std::string* out) {
    int actor_id = json_get_int(params_json, "actor_id");
    if (actor_id <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id <= 0) {
        if (out) *out = R"({"error":"no_actor_available"})";
        return false;
    }
    int target_id = json_get_int(params_json, "target_id");
    if (target_id <= 0) {
        if (out) *out = R"({"error":"missing_target_id","hint":"Provide target_id - defender character ID"})";
        return false;
    }
    int timeout_ms = json_get_int(params_json, "timeout_ms");
    if (timeout_ms <= 0) timeout_ms = 2000;
    bool include_ineligible = json_get_bool(params_json, "include_ineligible", true);

    // PAUSE SIGNAL: the engine CB validity check is game-thread-only; bail fast
    // when paused instead of blocking to the cb_query timeout.
    if (is_paused()) return emit_needs_unpaused("query_cb_validity_strict", out);

    CBStrictSummary summary{};
    std::vector<CBQueryResult> results;
    char err[96] = {};
    int rc = cb_query_strict_full_sync(static_cast<uint32_t>(actor_id),
                                       static_cast<uint32_t>(target_id),
                                       timeout_ms,
                                       &summary,
                                       &results,
                                       err, sizeof(err));

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_cb_validity_strict");
    r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);

    // Document which spec gates this implementation actually evaluates so
    // agents do not over-trust the verdict. ck-laf5 added per-CB
    // allowed_for_character via SCRIPT_TRIGGER_EVALUATOR on the actor-bound
    // scope_buf from ck-p3wn.
    r.key("gates_evaluated"); r.begin_array();
    r.value_string("stage1_bilateral");
    r.value_string("stage2_group_flags");
    r.value_string("stage2_group_is_valid_trigger");      // ck-p3wn
    r.value_string("stage3_kind_relation_mode");          // ck-it6e
    r.value_string("per_cb_allowed_for_character");       // ck-laf5
    r.value_string("inner_cb_validity_check");
    // read-side declarability honesty (war_validity_discovery): mirrors the
    // steered-write gates into the per-CB `declarable` field for claim CBs.
    r.value_string("declarable_pressed_claim");
    r.value_string("declarable_target_independent");
    r.value_string("declarable_target_imprisoned");
    r.value_string("declarable_target_alliance_truce");
    r.value_string("declarable_not_at_war_opposing");
    r.end_array();
    r.key("gates_deferred"); r.begin_array();
    r.value_string("per_cb_allowed_against_character");
    r.value_string("valid_to_start");
    r.value_string("ai_only_against_neighbors");
    r.value_string("max_ai_diplo_distance_to_title");
    r.end_array();

    if (rc < 0) {
        const char* status = "error";
        if (rc == -1) status = "busy";
        else if (rc == -3) status = "timeout";
        else if (rc == -4) status = "bilateral_filter_reject";
        r.key("status"); r.value_string(status);
        r.key("error"); r.value_string(err[0] ? err : "cb_query_failed");
        r.key("inner_valid_count"); r.value_int(0);
        r.end_object();
        if (out) *out = r.take();
        return rc != -1;  // surface busy as transient; others are terminal
    }

    r.key("status"); r.value_string("ok");

    r.key("summary"); r.begin_object();
    r.key("total_cbs");             r.value_int(summary.total_cbs);
    r.key("outer_filter_dropped");  r.value_int(summary.outer_filter_dropped);
    r.key("candidates_post_outer"); r.value_int(summary.candidates_post_outer);
    r.key("inner_valid");           r.value_int(summary.inner_valid);
    r.key("inner_invalid");         r.value_int(summary.inner_invalid);
    // ck-9fbs: ENABLED_FLAG enumeration of the per-install CB DB. Lets the
    // agent deterministically identify the DLC-disabled / has_game_rule
    // subset (e.g. the 16-CB delta on BuildID e61ff264… cycle 2-38) without
    // heuristic name-prefix matching.
    r.key("enabled_count");         r.value_int(summary.enabled_count);
    r.key("disabled_count");        r.value_int(summary.disabled_count);
    // D8 guard surface (probe P4, 2026-08-01): >0 means at least one CB's
    // option enumeration FILLED the bridge's CB-option buffer to capacity —
    // one entry away from the engine's CDynArray growth path through the
    // invalid CB_OPTION_ALLOCATOR_VTABLE (the 07-31/08-01 SIGSEGV class).
    // Option data beyond capacity was not appended: treat it as truncated
    // and raise kCBOptionEntryCap (ai_eval_hook.cpp).
    r.key("option_capacity_hits");  r.value_int(summary.option_capacity_hits);
    if (summary.option_capacity_hits > 0) {
        r.key("cb_enumeration_truncated"); r.value_bool(true);
    }
    {
        std::vector<std::string> names = summary.disabled_cb_names;
        std::stable_sort(names.begin(), names.end());
        r.key("disabled_cb_names"); r.begin_array();
        for (const auto& n : names) r.value_string(n.c_str());
        r.end_array();
    }
    r.end_object();

    // === Read-side declarability honesty (war_validity_discovery) ===========
    // The engine's CB_VALIDITY_CHECK (which drives inner_status) does NOT
    // short-circuit on pressed-ness at the C++ layer (re_cb_validation_deep_dive
    // 3.6) and the strict pipeline tests no target-state, so claim CBs report
    // inner_status=pass against an UNPRESSED claim or a bad-target-state defender
    // and the engine then culls the steered war to war_count 0. We mirror the
    // steered-write gates (intent_dispatch.cpp:1358-1572) as an ADDITIVE honesty
    // layer: target_state{...} + a per-CB `declarable` for claim CBs +
    // declarable_count. inner_valid/inner_valid_count are left UNCHANGED (engine
    // verdict). PURE IPC-thread memory reads — no engine evaluator, no WCC.
    void* actor_char = lookup_character(static_cast<uint32_t>(actor_id));
    void* target_char = lookup_character(static_cast<uint32_t>(target_id));
    DefenderState tstate;
    bool actor_pressed_on_target = false;
    bool in_alliance = false, in_truce = false;
    bool already_at_war = false;
    if (target_char && !is_sentinel(target_char)) {
        tstate = probe_defender_state(reinterpret_cast<uintptr_t>(target_char));
    }
    if (actor_char && !is_sentinel(actor_char)) {
        actor_pressed_on_target = attacker_has_pressed_claim_on(
            reinterpret_cast<uintptr_t>(actor_char), static_cast<uint32_t>(target_id));
        // Alliance/truce: mirror intent_dispatch.cpp:1511-1572 (pure list-getters,
        // fail OPEN on any unmapped chain — never block a legitimate war).
        probe_alliance_truce(actor_char, target_char, in_alliance, in_truce);
        // Already-at-war-opposing (engine Gate 0 / CB_ALREADY_AT_WAR; war_validity R5).
        already_at_war = actor_at_war_opposing(
            reinterpret_cast<uintptr_t>(actor_char), static_cast<uint32_t>(target_id));
    }

    r.key("target_state"); r.begin_object();
    r.key("independent");           r.value_bool(tstate.independent);
    if (!tstate.independent) { r.key("liege_id"); r.value_uint(tstate.liege_id); }
    r.key("imprisoned");            r.value_bool(tstate.imprisoned);
    r.key("in_alliance");           r.value_bool(in_alliance);
    r.key("in_truce");              r.value_bool(in_truce);
    r.key("already_at_war_opposing"); r.value_bool(already_at_war);
    r.key("pressed_claim_on_target"); r.value_bool(actor_pressed_on_target);
    r.end_object();

    // Per-CB precondition probes for the NON-claim families (computed ONCE; fed
    // into cb_cull_reason so a fail carries a typed reason). All pure safe_read,
    // fail-closed — they only LABEL an engine fail, never override the verdict.
    CBPreconditionProbes pre;
    pre.target_independent = tstate.independent;
    pre.target_imprisoned  = tstate.imprisoned;
    pre.in_alliance        = in_alliance;
    pre.in_truce           = in_truce;
    pre.already_at_war      = already_at_war;
    if (actor_char && !is_sentinel(actor_char)) {
        pre.actor_tribal = cb_actor_is_tribal(actor_char);
        pre.has_de_jure  = cb_actor_has_de_jure_over_target(
            actor_char, static_cast<uint32_t>(actor_id), target_char);
        pre.land_adjacent = cb_target_land_adjacent_to_actor_realm(
            actor_char, static_cast<uint32_t>(actor_id), target_char);
        pre.adjacency_probed = true;
    }

    int declarable_count = 0;
    r.key("results"); r.begin_array();
    for (const auto& cb : results) {
        const bool dropped = (strcmp(cb.outer_status, "dropped") == 0);
        if (dropped && !include_ineligible) continue;
        r.begin_object();
        r.key("cb_index"); r.value_uint(cb.cb_index);
        r.key("cb_name");  r.value_string(cb.cb_name);
        const char* mode_str = (cb.target_mode == 0) ? "none" :
                               (cb.target_mode == 6) ? "claim" : "title";
        r.key("target_mode"); r.value_string(mode_str);
        r.key("outer_status"); r.value_string(cb.outer_status[0] ? cb.outer_status : "pass");
        if (cb.outer_reason[0]) {
            r.key("outer_reason"); r.value_string(cb.outer_reason);
        }
        r.key("inner_status"); r.value_string(cb.inner_status[0] ? cb.inner_status : "unrun");
        if (!dropped) {
            r.key("eval_us"); r.value_uint(cb.eval_us);
        }

        // Declarability honesty. For claim CBs this mirrors the write-path reject
        // keys; for de_jure / conquest / invasion / religious / subjugation CBs we
        // now ALSO emit a per-family declarable + typed cull_reason derived from the
        // SAME read-side probes (cb_precondition_class + cb_cull_reason). ADDITIVE —
        // does not change inner_status/inner_valid_count.
        const bool inner_pass = (strcmp(cb.inner_status, "pass") == 0);
        const bool is_claim = (cb.target_mode == 6) || (strcmp(cb.cb_name, "claim_cb") == 0);
        if (is_claim) {
            const char* cull = nullptr;
            if (!actor_pressed_on_target)  cull = "claim_not_pressed";
            else if (!tstate.independent)  cull = "target_not_independent";
            else if (tstate.imprisoned)    cull = "target_imprisoned";
            else if (in_alliance)          cull = "target_in_alliance";
            else if (in_truce)             cull = "target_in_truce";
            else if (already_at_war)       cull = "already_at_war_opposing";
            bool declarable = inner_pass && (cull == nullptr);
            r.key("declarable"); r.value_bool(declarable);
            // D4-secondary (fix-plan FIX 3.2, 2026-08-01): EVERY non-declarable
            // claim row carries a typed cull_reason. Previously only
            // outer=pass + inner=pass + read-side-culled rows did — a claim CB
            // with outer=pass + inner=FAIL carried NO reason and re-tripped the
            // (fixed) suite gate-bias check. Mirror the non-claim branch below:
            // prefer the derived read-side cull; otherwise surface the engine's
            // inner verdict as the typed reason.
            if (!declarable) {
                r.key("cull_reason");
                r.value_string(cull ? cull : "engine_cb_validity_check_failed");
            }
            if (declarable) declarable_count++;
        } else if (!dropped) {
            // NON-claim families: engine inner verdict is authoritative; we surface
            // declarable = inner_pass and, on a fail, a typed cull_reason.
            const CBClass cls = cb_precondition_class(cb.cb_name);
            r.key("declarable"); r.value_bool(inner_pass);
            if (!inner_pass) {
                r.key("cull_reason"); r.value_string(cb_cull_reason(cls, pre));
            }
            if (inner_pass) declarable_count++;
        }
        r.end_object();
    }
    r.end_array();

    r.key("inner_valid_count"); r.value_int(rc);
    // Honest count: engine-valid CBs that ALSO pass the read-side write-gate
    // mirror (pressed/independent/free/not-allied/not-truced for claim CBs).
    // The agent should gate declarations on declarable_count, not inner_valid_count.
    r.key("declarable_count"); r.value_int(declarable_count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_cbs_for_target (ck cb-capability) — READ-ONLY enumeration of ALL casus
// belli the engine recognizes for an (attacker, target) pair, with per-CB
// validity (engine strict inner-status), scope, grants, unmet requirements,
// and notes.
//
// This is a CURATED VIEW over do_query_cb_validity_strict's machinery: it calls
// the SAME cb_query_strict_full_sync (the engine's CB_VALIDITY_CHECK iterated
// across the whole CASUS_BELLI_DB) and the SAME read-side declarability probes
// (probe_defender_state / attacker_has_pressed_claim_on / probe_alliance_truce /
// actor_at_war_opposing). It does NOT duplicate any offset or re-walk memory the
// strict path already covers — it filters the strict results down to the
// win-line CB families and annotates each with static semantics from
// docs/notes/cb-capability/inv_gamecb.md.
//
// PAUSE-STATE: same as query_cb_validity_strict / valid_cbs_strict — the engine
// CB_VALIDITY_CHECK is dispatched to the GAME THREAD via the g_cb_query slot and
// serviced inside drain_and_execute_intents(), so the game MUST BE UNPAUSED for
// this query to return engine verdicts (otherwise cb_query_strict_full_sync
// times out, rc<0). The read-side annotations (scope/grants/target_state) are
// pure IPC-thread memory reads and would work paused, but the `valid` field
// depends on the unpaused engine probe. Treat this as an UNPAUSED query like
// valid_cbs_strict (per MEMORY: bridge-pause-state-rules).
//
// CRASH-PROOF: cb_query_strict_full_sync is bounded (timeout, fail-closed rc<0)
// and never hangs the IPC thread (watchdog-guarded on the game side). Per-CB
// annotation is pure string lookup + the already-probed target_state — no
// further memory walks that can fault. A CB whose name we don't recognize is
// simply emitted with empty scope/grants (never skipped destructively, never
// crashes).
// =============================================================================

// Static per-CB semantics for the win-line CB families (inv_gamecb.md CB
// TRADEOFF TABLE). Keyed by exact engine CB name. `scope` is the territory
// granted on victory; `grants` is short human text. CB names not in this table
// fall through to a generic annotation.
struct CBSemantics {
    const char* cb_name;   // exact engine CASUS_BELLI_DB name
    const char* scope;     // county | duchy | kingdom | realm | faith_counties
    const char* grants;    // short grant text
};

static const CBSemantics* cb_semantics_for(const char* cb_name) {
    static const CBSemantics TABLE[] = {
        // conquest family
        {"county_conquest_cb",            "county",
         "one de-jure county (the targeted county)"},
        {"duchy_conquest_cb",             "duchy",
         "the whole targeted de-jure duchy (every county held by defender or his vassals)"},
        {"ducal_conquest_cb",             "duchy",
         "the targeted de-jure duchy (de-jure-duchy variant of conquest)"},
        // subjugation
        {"tribal_subjugation_cb",         "realm",
         "the ENTIRE defender realm (all his and his vassals' counties); once per ruler-lifetime"},
        // de_jure family
        {"de_jure_cb",                    "kingdom",
         "all of the defender's titles you are de-jure liege over (take-all)"},
        {"individual_duchy_de_jure_cb",   "duchy",
         "a whole de-jure duchy you hold the de-jure claim on (cheap prestige)"},
        {"individual_county_de_jure_cb",  "county",
         "one de-jure county you hold the de-jure claim on"},
        // holy war family
        // FIXPLAN A1: dropped the phantom "holy_war_cb" row — no such engine CB
        // exists. The real CASUS_BELLI_DB names are religious_war /
        // minor_religious_war / major_religious_war (kept below); "holy_war" is
        // only a piety-cost script variable, never a CB name. Keeping the
        // phantom row let a mislabeled name resolve to a row it never matches.
        {"religious_war",                 "faith_counties",
         "the targeted duchy's same-faith counties of the defender's faith"},
        {"major_religious_war",           "faith_counties",
         "the targeted kingdom's same-faith counties of the defender's faith"},
        {"minor_religious_war",           "faith_counties",
         "the targeted county of the defender's faith"},
        // great holy war
        {"undirected_great_holy_war",     "kingdom",
         "a whole de-jure kingdom to the GHW beneficiary (religious-head lifecycle, not a plain WCC)"},
        {"directed_great_holy_war",       "kingdom",
         "a whole de-jure kingdom to the designated winner (religious-head lifecycle, not a plain WCC)"},
        // claim
        {"claim_cb",                      "county",
         "the claimed title(s) and the counties under them (installs the claimant)"},
    };
    for (const auto& s : TABLE) {
        if (strcmp(s.cb_name, cb_name) == 0) return &s;
    }
    return nullptr;
}

// Returns true iff `cb_name` is one of the win-line CB families this handler
// surfaces. We match by family substring so the engine's exact variant names
// (e.g. individual_*_de_jure_cb, *_great_holy_war) are all captured without an
// exhaustive list, plus the explicit "conquest"/"claim"/"holy_war" tokens.
static bool cb_is_winline_family(const char* cb_name) {
    return strstr(cb_name, "conquest")    != nullptr ||
           strstr(cb_name, "subjugation") != nullptr ||
           strstr(cb_name, "de_jure")     != nullptr ||
           strstr(cb_name, "holy_war")    != nullptr ||
           strstr(cb_name, "religious_war") != nullptr ||
           strstr(cb_name, "great_holy_war") != nullptr ||
           strcmp(cb_name, "claim_cb")    == 0;
}

// Shared per-target emission body for query_cbs_for_target (single) and
// query_cbs_for_targets (batched). Appends the status / target_state /
// actor_resources / cbs[] keys onto an ALREADY-OPEN ResponseBuilder object —
// the caller is responsible for begin_object()/end_object() and any
// action/attacker/target framing. `rc`/`err`/`summary`/`results` are the
// outputs of a prior cb_query_strict_full_sync(actor_id, target_id, ...) call.
// Byte-identical to the inline body the single handler used before (extracted
// verbatim so the single-target wire shape is UNCHANGED).
static void emit_cbs_target_body(ResponseBuilder& r,
                                 int actor_id, int target_id,
                                 int rc, const char* err,
                                 const CBStrictSummary& summary,
                                 const std::vector<CBQueryResult>& results) {
    (void)summary;
    if (rc < 0) {
        const char* status = "error";
        if (rc == -1) status = "busy";
        else if (rc == -3) status = "timeout";
        else if (rc == -4) status = "bilateral_filter_reject";
        r.key("status"); r.value_string(status);
        r.key("error"); r.value_string(err && err[0] ? err : "cb_query_failed");
        r.key("cbs"); r.begin_array(); r.end_array();
        return;
    }

    r.key("status"); r.value_string("ok");

    // Read-side declarability probes — identical to query_cb_validity_strict's
    // honesty layer. Pure IPC-thread reads, fail-open on unmapped chains.
    void* actor_char = lookup_character(static_cast<uint32_t>(actor_id));
    void* target_char = lookup_character(static_cast<uint32_t>(target_id));
    DefenderState tstate;
    bool actor_pressed_on_target = false;
    bool in_alliance = false, in_truce = false, already_at_war = false;
    uint32_t claimed_title = 0;
    int64_t actor_prestige = 0, actor_piety = 0;
    int32_t truce_end_raw = 0;
    char truce_end_str[32] = {0};
    bool has_truce_expiry = false;
    if (target_char && !is_sentinel(target_char)) {
        tstate = probe_defender_state(reinterpret_cast<uintptr_t>(target_char));
    }
    if (actor_char && !is_sentinel(actor_char)) {
        actor_pressed_on_target = attacker_has_pressed_claim_on(
            reinterpret_cast<uintptr_t>(actor_char), static_cast<uint32_t>(target_id));
        claimed_title = resolve_pressed_claim_title(
            reinterpret_cast<uintptr_t>(actor_char), static_cast<uint32_t>(target_id));
        probe_alliance_truce(actor_char, target_char, in_alliance, in_truce);
        already_at_war = actor_at_war_opposing(
            reinterpret_cast<uintptr_t>(actor_char), static_cast<uint32_t>(target_id));
        uintptr_t rd = safe_read<uintptr_t>(
            reinterpret_cast<uintptr_t>(actor_char) + ck3::character::REALM_DATA);
        if (rd && is_mapped(rd)) {
            actor_prestige = safe_read<int64_t>(rd + ck3::character::REALM_PRESTIGE) / 100000;
            actor_piety    = safe_read<int64_t>(rd + ck3::character::REALM_PIETY) / 100000;
        }
        has_truce_expiry = probe_truce_expiry(
            actor_char, static_cast<uint32_t>(target_id),
            truce_end_raw, truce_end_str, sizeof(truce_end_str));
    }

    r.key("target_state"); r.begin_object();
    r.key("independent");             r.value_bool(tstate.independent);
    if (!tstate.independent) { r.key("liege_id"); r.value_uint(tstate.liege_id); }
    r.key("imprisoned");              r.value_bool(tstate.imprisoned);
    r.key("in_alliance");             r.value_bool(in_alliance);
    r.key("in_truce");                r.value_bool(in_truce);
    if (in_truce && has_truce_expiry) {
        r.key("truce_end_date");     r.value_string(truce_end_str);
        r.key("truce_end_date_raw"); r.value_int(truce_end_raw);
    }
    r.key("already_at_war_opposing"); r.value_bool(already_at_war);
    r.key("pressed_claim_on_target"); r.value_bool(actor_pressed_on_target);
    r.end_object();
    r.key("actor_resources"); r.begin_object();
    r.key("prestige"); r.value_int(static_cast<int>(actor_prestige));
    r.key("piety");    r.value_int(static_cast<int>(actor_piety));
    r.end_object();

    // Per-CB precondition probes for the NON-claim families (computed ONCE; fed
    // into cb_cull_reason). Same pure-read helpers as query_cb_validity_strict, so
    // the curated `cbs_for_target` view and the strict matrix stay consistent.
    CBPreconditionProbes pre;
    pre.target_independent = tstate.independent;
    pre.target_imprisoned  = tstate.imprisoned;
    pre.in_alliance        = in_alliance;
    pre.in_truce           = in_truce;
    pre.already_at_war      = already_at_war;
    if (actor_char && !is_sentinel(actor_char)) {
        pre.actor_tribal = cb_actor_is_tribal(actor_char);
        pre.has_de_jure  = cb_actor_has_de_jure_over_target(
            actor_char, static_cast<uint32_t>(actor_id), target_char);
        pre.land_adjacent = cb_target_land_adjacent_to_actor_realm(
            actor_char, static_cast<uint32_t>(actor_id), target_char);
        pre.adjacency_probed = true;
    }

    r.key("cbs"); r.begin_array();
    for (const auto& cb : results) {
        if (!cb_is_winline_family(cb.cb_name)) continue;  // curated view

        const bool dropped   = (strcmp(cb.outer_status, "dropped") == 0);
        const bool inner_pass = (strcmp(cb.inner_status, "pass") == 0);
        const bool is_claim  = (cb.target_mode == 6) || (strcmp(cb.cb_name, "claim_cb") == 0);
        const CBSemantics* sem = cb_semantics_for(cb.cb_name);
        const CBClass cls = cb_precondition_class(cb.cb_name);

        const char* claim_cull = nullptr;
        if (is_claim) {
            if (!actor_pressed_on_target)  claim_cull = "claim_not_pressed";
            else if (!tstate.independent)  claim_cull = "target_not_independent";
            else if (tstate.imprisoned)    claim_cull = "target_imprisoned";
            else if (in_alliance)          claim_cull = "target_in_alliance";
            else if (in_truce)             claim_cull = "target_in_truce";
            else if (already_at_war)       claim_cull = "already_at_war_opposing";
        }
        // NON-claim families: a typed cull_reason when the engine inner verdict
        // failed (the engine `valid`/`inner_status` stays authoritative — this only
        // LABELS the fail). claim_cull keeps its dedicated computation above.
        const char* family_cull = nullptr;
        if (!is_claim && !dropped && !inner_pass) {
            family_cull = cb_cull_reason(cls, pre);
        }
        bool valid = !dropped && inner_pass && (claim_cull == nullptr);

        r.begin_object();
        r.key("cb_key");  r.value_string(cb.cb_name);
        r.key("valid");   r.value_bool(valid);
        // Explicit per-CB declarable + typed cull_reason for EVERY family (was
        // claim-only). declarable == the honest "can the agent declare this now".
        r.key("declarable"); r.value_bool(valid);
        if (claim_cull)       { r.key("cull_reason"); r.value_string(claim_cull); }
        else if (family_cull) { r.key("cull_reason"); r.value_string(family_cull); }

        if (sem) { r.key("scope"); r.value_string(sem->scope); }
        else     { r.key("scope"); r.value_string(cb.target_mode == 0 ? "realm" : "title"); }

        if (sem) { r.key("grants"); r.value_string(sem->grants); }
        else     { r.key("grants"); r.value_string(""); }

        r.key("requirements_unmet"); r.begin_array();
        if (dropped) {
            r.value_string("outer_filter_dropped");
            if (cb.outer_reason[0]) r.value_string(cb.outer_reason);
        } else if (!inner_pass) {
            // Typed family reason in place of the opaque generic when we have one.
            r.value_string(family_cull ? family_cull : "engine_cb_validity_check_failed");
        }
        if (claim_cull) r.value_string(claim_cull);
        r.end_array();

        r.key("notes"); r.begin_array();
        r.value_string(cb.target_mode == 0
                       ? "target_mode=0: no cb_title_id needed to dispatch"
                       : (cb.target_mode == 6
                          ? "target_mode=6 (claim): pass claimant_id + cb_title_id to dispatch"
                          : "title-targeted: REQUIRES cb_title_id to dispatch"));
        if (is_claim && inner_pass && claim_cull) {
            r.value_string("engine inner verdict is over-optimistic for claim_cb; gate on `valid`/cull, not inner-pass");
        }
        if (strstr(cb.cb_name, "great_holy_war") != nullptr) {
            r.value_string("UNSUPPORTED via WCC: GHW uses the religious-head CGreatHolyWar lifecycle, not a plain declaration");
        } else if (strstr(cb.cb_name, "holy_war") != nullptr ||
                   strstr(cb.cb_name, "religious_war") != nullptr) {
            r.value_string("WCC dispatches by name but NOT in the step-2d engine-validity gated set; pre-validate yourself");
            r.value_string("unreformed pagans cannot DECLARE holy war (attacker-side gate) — expect engine inner fail while unreformed");
        }
        r.value_string("deferred gates NOT re-checked: valid_to_start, ai_only_against_neighbors, max_ai_diplo_distance_to_title, per_cb_allowed_against_character");
        r.end_array();

        uint32_t cb_title_id = 0, claimant_id = 0;
        bool needs_cb_title_id = (cb.target_mode != 0);
        const char* title_source = nullptr;
        if (cb.target_mode == 0) {
            needs_cb_title_id = false;
        } else if (is_claim) {
            cb_title_id = claimed_title;
            claimant_id = (cb_title_id != 0) ? (static_cast<uint32_t>(actor_id) & 0xFFFFFF) : 0;
            title_source = "pressed_claim";
        } else {
            title_source = "query_conquest_targets";
        }
        r.key("cb_title_id");      r.value_uint(cb_title_id);
        r.key("needs_cb_title_id"); r.value_bool(needs_cb_title_id);
        if (claimant_id) { r.key("claimant_id"); r.value_uint(claimant_id); }
        if (cb_title_id == 0 && needs_cb_title_id && title_source)
            { r.key("title_source"); r.value_string(title_source); }

        CBCost cost = cb_cost_for(cb.cb_name);
        bool affordable = (actor_prestige >= cost.prestige) && (actor_piety >= cost.piety);
        r.key("cost_prestige"); r.value_int(cost.prestige);
        r.key("cost_piety");    r.value_int(cost.piety);
        r.key("affordable");    r.value_bool(affordable);
        if (!affordable) {
            r.key("funding_hint"); r.value_string(
                cost.piety > actor_piety
                ? "needs_piety — pray / convert holy sites or win battles to fund"
                : "needs_prestige — raid (act type=raid) or win battles to fund");
        }

        r.key("outer_status"); r.value_string(cb.outer_status[0] ? cb.outer_status : "pass");
        r.key("inner_status"); r.value_string(cb.inner_status[0] ? cb.inner_status : "unrun");
        r.key("cb_index");     r.value_uint(cb.cb_index);
        r.key("target_mode");  r.value_int(cb.target_mode);
        r.end_object();
    }
    r.end_array();
}

bool do_query_cbs_for_target(const char* params_json, std::string* out) {
    // Inputs: {attacker_id | character_id, target_id}. attacker_id is the
    // canonical key; character_id is accepted as an alias (json_get_int returns
    // 0 on absent, so we try both).
    int actor_id = json_get_int(params_json, "attacker_id");
    if (actor_id <= 0) actor_id = json_get_int(params_json, "character_id");
    if (actor_id <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id <= 0) {
        if (out) *out = R"json({"error":"no_actor_available","hint":"Provide attacker_id (or character_id)"})json";
        return false;
    }
    int target_id = json_get_int(params_json, "target_id");
    if (target_id <= 0) {
        if (out) *out = R"({"error":"missing_target_id","hint":"Provide target_id - defender character ID"})";
        return false;
    }
    int timeout_ms = json_get_int(params_json, "timeout_ms");
    if (timeout_ms <= 0) timeout_ms = 2000;

    // PAUSE SIGNAL: same game-thread CB validity dependency as
    // query_cb_validity_strict — bail fast when paused (see emit_needs_unpaused).
    if (is_paused()) return emit_needs_unpaused("query_cbs_for_target", out);

    // Run the engine strict validator over the WHOLE CB DB (same call
    // query_cb_validity_strict uses). Game thread services it; UNPAUSED required.
    CBStrictSummary summary{};
    std::vector<CBQueryResult> results;
    char err[96] = {};
    int rc = cb_query_strict_full_sync(static_cast<uint32_t>(actor_id),
                                       static_cast<uint32_t>(target_id),
                                       timeout_ms,
                                       &summary,
                                       &results,
                                       err, sizeof(err));

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_cbs_for_target");
    r.key("attacker_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);
    r.key("pause_state_required"); r.value_string("unpaused");

    // Per-target body (status / target_state / actor_resources / cbs) is shared
    // verbatim with the batched query_cbs_for_targets handler.
    emit_cbs_target_body(r, actor_id, target_id, rc, err, summary, results);
    r.end_object();
    if (out) *out = r.take();
    return rc != -1;  // busy is transient; others terminal/ok
}

// =============================================================================
// query_cbs_for_targets (BATCHED — _slot_drain_profile.md §3b) — resolve the
// win-line CBs for N targets in ONE deferred drain window instead of N.
//
// WHY: per _slot_drain_profile.md, each single-target query_cbs_for_target pays
// its OWN coordinator pulse `[A]` (one game-tick wait) + pause/unpause flip when
// the agent loops it over N candidate targets (N × ~100ms + clock-thrash). This
// handler is ONE deferred-drain action: the ipc_server Tier-0 coordinator pulses
// ONCE, and inside that single unpaused window we submit cb_query_strict_full_sync
// per target back-to-back on the SAME single-flight g_cb_query slot. Cost drops
// from N×(pulse `[A]` + drain `[C]`) to (one `[A]`) + N×(drain `[C]`).
//
// CRASH-SAFETY / DISCIPLINE (UNCHANGED): each target goes through the IDENTICAL
// cb_query_strict_full_sync (same single-flight slot acquire, bounded timeout,
// fail-closed rc<0, stale-reclaim) — no new engine path, no name-cache drain, no
// new offsets. All read-side probes are safe_read/is_mapped (fail-open). The
// per-target emission body is byte-identical to query_cbs_for_target (shared
// emit_cbs_target_body). Inputs: {attacker_id|character_id, target_ids:[...]}.
// Caps the batch at 32 targets so one request can't monopolise the slot.
// =============================================================================
bool do_query_cbs_for_targets(const char* params_json, std::string* out) {
    int actor_id = json_get_int(params_json, "attacker_id");
    if (actor_id <= 0) actor_id = json_get_int(params_json, "character_id");
    if (actor_id <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id <= 0) {
        if (out) *out = R"json({"error":"no_actor_available","hint":"Provide attacker_id (or character_id)"})json";
        return false;
    }

    // Parse target_ids: a JSON array of integers. Reuse the rapidjson document
    // the rest of the bridge uses (json_get_* are scalar-only).
    //
    // N4 fix (surface-audit 2026-08-01): handlers receive the WHOLE inner
    // action object from the actions-envelope path
    // ({"action":...,"params":{...},"request_id":...}), so target_ids usually
    // lives UNDER "params". The scalar json_get_* helpers already do this
    // params-fallback; this array lookup never did — every envelope-framed
    // caller (sweep, MCP) got missing_target_ids while flat raw-socket callers
    // worked (probes-round3.md P4.6). Also tolerate int64/uint64 and numeric
    // strings, and distinguish "key absent" from "key present but unusable".
    std::vector<uint32_t> target_ids;
    bool target_ids_key_seen = false;
    {
        rapidjson::Document doc;
        doc.Parse(params_json);
        if (!doc.HasParseError() && doc.IsObject()) {
            const rapidjson::Value* obj = &doc;
            if (!obj->HasMember("target_ids")) {
                auto pit = doc.FindMember("params");
                if (pit != doc.MemberEnd() && pit->value.IsObject())
                    obj = &pit->value;
            }
            if (obj->HasMember("target_ids")) {
                target_ids_key_seen = true;
                const auto& arr = (*obj)["target_ids"];
                if (arr.IsArray()) {
                    for (rapidjson::SizeType i = 0; i < arr.Size() && target_ids.size() < 32; i++) {
                        const auto& el = arr[i];
                        long long id = -1;
                        if (el.IsInt())         id = el.GetInt();
                        else if (el.IsUint())   id = el.GetUint();
                        else if (el.IsInt64())  id = el.GetInt64();
                        else if (el.IsUint64()) id = static_cast<long long>(el.GetUint64());
                        else if (el.IsString()) {
                            char* end = nullptr;
                            long long sv = strtoll(el.GetString(), &end, 10);
                            if (end != el.GetString() && end && *end == '\0') id = sv;
                        }
                        if (id > 0) target_ids.push_back(static_cast<uint32_t>(id) & 0xFFFFFF);
                    }
                }
            }
        }
    }
    if (target_ids.empty()) {
        if (out) {
            *out = target_ids_key_seen
                ? R"json({"error":"invalid_target_ids","hint":"target_ids must be a JSON array (<=32) of positive defender character IDs, e.g. {\"target_ids\":[11232]}; numeric strings are accepted"})json"
                : R"json({"error":"missing_target_ids","hint":"Provide target_ids - a JSON array (<=32) of defender character IDs (top-level or inside params)"})json";
        }
        return false;
    }

    int timeout_ms = json_get_int(params_json, "timeout_ms");
    if (timeout_ms <= 0) timeout_ms = 2000;

    // Same game-thread CB-validity dependency as query_cbs_for_target — bail fast
    // when paused (the coordinator pulses for us when this is a deferred-drain
    // action; a direct paused call returns the typed needs-unpaused contract).
    if (is_paused()) return emit_needs_unpaused("query_cbs_for_targets", out);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_cbs_for_targets");
    r.key("attacker_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("count"); r.value_int(static_cast<int>(target_ids.size()));
    r.key("pause_state_required"); r.value_string("unpaused");
    r.key("batched"); r.value_bool(true);  // one drain window for all targets

    int ok_count = 0, busy_count = 0;
    r.key("results"); r.begin_array();
    for (uint32_t tid : target_ids) {
        // Each target: ONE strict probe on the shared single-flight slot. We are
        // already inside the coordinator's single unpaused window, so these drain
        // on consecutive live ticks without re-pulsing per target.
        CBStrictSummary summary{};
        std::vector<CBQueryResult> results;
        char err[96] = {};
        int rc = cb_query_strict_full_sync(static_cast<uint32_t>(actor_id),
                                           tid, timeout_ms,
                                           &summary, &results,
                                           err, sizeof(err));
        if (rc >= 0) ok_count++;
        else if (rc == -1) busy_count++;

        r.begin_object();
        r.key("target_id"); r.value_uint(tid);
        emit_cbs_target_body(r, actor_id, static_cast<int>(tid), rc, err, summary, results);
        r.end_object();
    }
    r.end_array();
    r.key("ok_count");   r.value_int(ok_count);
    r.key("busy_count"); r.value_int(busy_count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_alliances — Dedicated alliance endpoint with full detail
// Returns active alliances from the CDiplomaticRelation sorted array at
// core_data+0x20, plus forced allies from *(char+0x1A0)+0x20.
// Pure memory reads only, safe while paused.
// =============================================================================
bool do_query_alliances(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = R"({"error":"character_not_found"})";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_alliances");
    r.key("character_id"); r.value_uint(my_id);

    // Own military strength (for total callable strength calculation)
    uintptr_t my_title_realm = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    int32_t own_mil_current = 0;
    int32_t own_mil_max = 0;
    if (my_title_realm && (my_title_realm && is_mapped(my_title_realm))) {
        own_mil_current = safe_read<int32_t>(my_title_realm + ck3::title_realm::MIL_CURRENT);
        own_mil_max = safe_read<int32_t>(my_title_realm + ck3::title_realm::MIL_MAX);
    }
    r.key("own_military_strength"); r.value_int(own_mil_current);
    r.key("own_military_max"); r.value_int(own_mil_max);

    // Alliance reason enum (for reference):
    //   0=Marriage, 1=Betrothal, 2=DiplomacyMarriage,
    //   3=DiplomacyNegotiated, 4=Tributary, 5=BloodBrother
    // We report through-character IDs so the agent can infer the relationship type.

    // Running sum of ally military strength for total callable strength
    int64_t total_ally_strength = 0;
    int64_t total_ally_max = 0;

    // ---- 1. Alliances from CDiplomaticRelation sorted array (core_data+0x20) ----
    r.key("alliances"); r.begin_array();
    int alliance_count = 0;
    {
        uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
        if (core_data && (core_data && is_mapped(core_data))) {
            uintptr_t rel_arr = safe_read<uintptr_t>(core_data + ck3::relation_container::ARRAY_PTR);
            int32_t rel_count = safe_read<int32_t>(core_data + ck3::relation_container::COUNT);
            if (rel_count < 0 || rel_count > 500) rel_count = 0;

            if (rel_arr && (rel_arr && is_mapped(rel_arr)) && rel_count > 0) {
                for (int32_t i = 0; i < rel_count; i++) {
                    uintptr_t entry_base = rel_arr + i * ck3::relation_container::ENTRY_STRIDE;
                    uint32_t other_id = safe_read<uint32_t>(entry_base + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
                    uintptr_t rel_ptr = safe_read<uintptr_t>(entry_base + ck3::relation_container::ENTRY_PTR);
                    if (!rel_ptr || (!rel_ptr || !is_mapped(rel_ptr))) continue;

                    // Check active alliance: reason_count > 0 and not broken
                    int32_t reason_count = safe_read<int32_t>(rel_ptr + ck3::alliance::REASON_COUNT);
                    if (reason_count <= 0 || reason_count > 20) continue;

                    uint8_t broken = safe_read<uint8_t>(rel_ptr + ck3::alliance::BROKEN_FLAG);
                    if (broken) continue;

                    r.begin_object();
                    r.key("ally_id"); r.value_uint(other_id);
                    r.key("reason_count"); r.value_int(reason_count);
                    r.key("is_forced"); r.value_bool(false);

                    // Read ally's military strength
                    void* ally_char = lookup_character(other_id);
                    int32_t ally_mil = 0;
                    int32_t ally_mil_max = 0;
                    if (ally_char && !is_sentinel(ally_char)) {
                        uintptr_t ally_c = reinterpret_cast<uintptr_t>(ally_char);
                        uintptr_t ally_title_realm = safe_read<uintptr_t>(ally_c + ck3::character::TITLE_REALM_DATA);
                        if (ally_title_realm && (ally_title_realm && is_mapped(ally_title_realm))) {
                            ally_mil = safe_read<int32_t>(ally_title_realm + ck3::title_realm::MIL_CURRENT);
                            ally_mil_max = safe_read<int32_t>(ally_title_realm + ck3::title_realm::MIL_MAX);
                        }
                    }
                    r.key("military_strength"); r.value_int(ally_mil);
                    r.key("military_max"); r.value_int(ally_mil_max);
                    total_ally_strength += ally_mil;
                    total_ally_max += ally_mil_max;

                    // Read alliance reasons (through-character pairs)
                    r.key("reasons"); r.begin_array();
                    uintptr_t reason_arr = safe_read<uintptr_t>(rel_ptr + ck3::alliance::REASON_ARRAY);
                    if (reason_arr && (reason_arr && is_mapped(reason_arr))) {
                        for (int32_t j = 0; j < reason_count && j < 10; j++) {
                            uintptr_t re = reason_arr + j * ck3::alliance::REASON_ENTRY_SIZE;
                            uint32_t through_owner = safe_read<uint32_t>(re + ck3::alliance::REASON_CHAR_REF_1) & 0xFFFFFF;
                            uint32_t through_target = safe_read<uint32_t>(re + ck3::alliance::REASON_CHAR_REF_2) & 0xFFFFFF;
                            r.begin_object();
                            r.key("through_owner"); r.value_uint(through_owner);
                            r.key("through_target"); r.value_uint(through_target);
                            r.end_object();
                        }
                    }
                    r.end_array();

                    r.end_object();
                    alliance_count++;
                }
            }
        }
    }
    r.end_array();
    r.key("alliance_count"); r.value_int(alliance_count);

    // ---- 2. Forced allies (per-character fast path at *(char+0x1A0)+0x20) ----
    r.key("forced_allies"); r.begin_array();
    int forced_count = 0;
    {
        uintptr_t title_data = safe_read<uintptr_t>(c + ck3::character::TITLE_DATA);
        if (title_data && (title_data && is_mapped(title_data))) {
            // CPdxArray<uint32> at title_data + FORCED_ALLY_ARRAY
            // CPdxArray layout: +0x00 = data_ptr, +0x0C = count
            uintptr_t fa_base = title_data + ck3::alliance::FORCED_ALLY_ARRAY;
            uintptr_t fa_data = safe_read<uintptr_t>(fa_base + 0x00);
            int32_t fa_count = safe_read<int32_t>(fa_base + 0x0C);
            if (fa_count < 0 || fa_count > 200) fa_count = 0;

            if (fa_data && (fa_data && is_mapped(fa_data)) && fa_count > 0) {
                for (int32_t i = 0; i < fa_count; i++) {
                    uint32_t ally_id = safe_read<uint32_t>(fa_data + i * sizeof(uint32_t)) & 0xFFFFFF;
                    if (ally_id == 0 || ally_id == 0xFFFFFF) continue;

                    r.begin_object();
                    r.key("ally_id"); r.value_uint(ally_id);
                    r.key("is_forced"); r.value_bool(true);

                    // Read forced ally's military strength
                    void* fa_char = lookup_character(ally_id);
                    int32_t fa_mil = 0;
                    int32_t fa_mil_max = 0;
                    if (fa_char && !is_sentinel(fa_char)) {
                        uintptr_t fa_c = reinterpret_cast<uintptr_t>(fa_char);
                        uintptr_t fa_title_realm = safe_read<uintptr_t>(fa_c + ck3::character::TITLE_REALM_DATA);
                        if (fa_title_realm && (fa_title_realm && is_mapped(fa_title_realm))) {
                            fa_mil = safe_read<int32_t>(fa_title_realm + ck3::title_realm::MIL_CURRENT);
                            fa_mil_max = safe_read<int32_t>(fa_title_realm + ck3::title_realm::MIL_MAX);
                        }
                    }
                    r.key("military_strength"); r.value_int(fa_mil);
                    r.key("military_max"); r.value_int(fa_mil_max);
                    total_ally_strength += fa_mil;
                    total_ally_max += fa_mil_max;

                    r.end_object();
                    forced_count++;
                }
            }
        }
    }
    r.end_array();
    r.key("forced_ally_count"); r.value_int(forced_count);

    r.key("total_allies"); r.value_int(alliance_count + forced_count);
    r.key("total_ally_strength"); r.value_int(static_cast<int32_t>(total_ally_strength));
    r.key("total_ally_max"); r.value_int(static_cast<int32_t>(total_ally_max));
    r.key("total_callable_strength"); r.value_int(own_mil_current + static_cast<int32_t>(total_ally_strength));
    r.key("total_callable_max"); r.value_int(own_mil_max + static_cast<int32_t>(total_ally_max));
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_call_to_arms — alliance-hygiene read lever (#122)
//
// Surfaces the wars a ruler has been *dragged into as a called ally* — the
// distant ally-offensive wars that tie up the controlled ruler's levies. The
// k_danelaw run surfaced this: a León/Iberia holy war pulled King Ragnarr's
// army away during his recovery trough (a major contributor to the
// succession-trough collapse). The agent needs to SEE which alliance pulled it
// in (so it can decide to decline / break) — that is what this lever provides.
//
// Detection (validated structurally against the 1097 core dump, 6/6 hits):
//   For each ACTIVE war in WAR_DB:
//     - if the ruler is a participant but NOT the war's primary attacker/defender
//       (= a co-belligerent / called ally), AND
//     - the ruler holds an active (non-broken) alliance with the primary on the
//       side the ruler is fighting on,
//     then this is a CALL-TO-ARMS the ruler answered through an alliance.
//   `is_offensive` = true when the ruler sits on the ATTACKER side and is not
//   the war's primary attacker — i.e. an aggressive war it did not start (the
//   "keep my army home" candidates). Defensive calls (ally being attacked) are
//   reported too but flagged is_offensive=false.
//
// Pure memory reads only (safe_read over WAR_DB + the per-char relation array),
// so this is pause-safe and adds no unpause requirement. NO engine call.
// =============================================================================
bool do_query_call_to_arms(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = R"({"error":"character_not_found"})";
        return false;
    }
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    // Build the ruler's active-alliance set once (ally_id -> active alliance).
    // Mirrors do_query_alliances: CDiplomaticRelation array at core_data+0x20,
    // alliance == (REASON_COUNT > 0 && BROKEN_FLAG == 0).
    auto is_active_ally = [&](uint32_t other_id) -> bool {
        uintptr_t c = reinterpret_cast<uintptr_t>(character);
        uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
        if (!core_data || !is_mapped(core_data)) return false;
        uintptr_t rel_arr = safe_read<uintptr_t>(core_data + ck3::relation_container::ARRAY_PTR);
        int32_t rel_count = safe_read<int32_t>(core_data + ck3::relation_container::COUNT);
        if (!rel_arr || !is_mapped(rel_arr) || rel_count <= 0 || rel_count > 500) return false;
        for (int32_t i = 0; i < rel_count; i++) {
            uintptr_t eb = rel_arr + i * ck3::relation_container::ENTRY_STRIDE;
            uint32_t key = safe_read<uint32_t>(eb + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
            if (key != (other_id & 0xFFFFFF)) continue;
            uintptr_t rel = safe_read<uintptr_t>(eb + ck3::relation_container::ENTRY_PTR);
            if (!rel || !is_mapped(rel)) return false;
            int32_t rc = safe_read<int32_t>(rel + ck3::alliance::REASON_COUNT);
            if (rc <= 0 || rc > 20) return false;
            return safe_read<uint8_t>(rel + ck3::alliance::BROKEN_FLAG) == 0;
        }
        return false;
    };

    // side_has: is char `want` a participant in the container at war+off?
    auto side_has = [&](uintptr_t war_ptr, size_t off, uint32_t want) -> bool {
        uintptr_t arr = safe_read<uintptr_t>(war_ptr + off);
        int32_t cnt = safe_read<int32_t>(war_ptr + off + 0x0C);
        if (!arr || !is_mapped(arr) || cnt <= 0 || cnt > 100) return false;
        for (int32_t j = 0; j < cnt && j < 30; j++) {
            uintptr_t pp = safe_read<uintptr_t>(arr + j * 8);
            if (!pp || !is_mapped(pp)) continue;
            if ((safe_read<uint32_t>(pp + ck3::war_participant::CHAR_ID) & 0xFFFFFF) == (want & 0xFFFFFF))
                return true;
        }
        return false;
    };

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_call_to_arms");
    r.key("character_id"); r.value_uint(my_id);

    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    uintptr_t table = war_db ? safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR) : 0;
    uint32_t capacity = war_db ? safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY) : 0;

    r.key("calls"); r.begin_array();
    int call_count = 0;
    int offensive_count = 0;
    if (table && capacity > 0 && capacity < 100000) {
        for (uint32_t idx = 0; idx < capacity && idx < 10000; idx++) {
            uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                              + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
            uintptr_t war_ptr = safe_read<uintptr_t>(entry);
            if (!war_ptr || !is_mapped(war_ptr)) continue;
            if ((safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID) & 0xFFFFFF) != idx) continue;
            if (safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED) != 0) continue;

            uint32_t patk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
            uint32_t pdef = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;

            bool on_atk = side_has(war_ptr, ck3::war_obj::ATK_PARTICIPANT, my_id);
            bool on_def = side_has(war_ptr, ck3::war_obj::DEF_PARTICIPANT, my_id);
            if (!on_atk && !on_def) continue;            // not in this war
            if (my_id == patk || my_id == pdef) continue; // we are a war PRIMARY, not a called ally

            // Identify the primary on OUR side — the realm that called us in.
            uint32_t called_by = on_atk ? patk : pdef;
            // Only count it as an alliance call-to-arms if we actually hold an
            // active alliance with the side primary (filters out vassal/realm
            // levies and forced participants that are not alliance drags).
            if (!is_active_ally(called_by)) continue;

            bool is_offensive = on_atk;  // attacking on a war we did not start

            r.begin_object();
            r.key("war_id"); r.value_uint(idx);
            std::string wname = ad_read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR);
            if (!wname.empty()) { r.key("war_name"); r.value_string(wname.c_str()); }
            r.key("called_by_ally"); r.value_uint(called_by);
            r.key("my_side"); r.value_string(on_atk ? "attacker" : "defender");
            r.key("is_offensive"); r.value_bool(is_offensive);
            r.key("primary_attacker"); r.value_uint(patk);
            r.key("primary_defender"); r.value_uint(pdef);
            // The opponent primary (who our levies are fighting) — the "distant"
            // realm. The agent can geo-judge whether this is worth the levies.
            r.key("opponent_primary"); r.value_uint(on_atk ? pdef : patk);
            // Hygiene hints: declining future calls / breaking the alliance.
            r.key("hint"); r.value_string(
                is_offensive
                ? "offensive ally-call: break the alliance with called_by_ally to stop future drags "
                  "(act type=break_alliance target_id=<called_by_ally>); the current war must run its course"
                : "defensive ally-call");
            r.end_object();
            call_count++;
            if (is_offensive) offensive_count++;
        }
    }
    r.end_array();
    r.key("call_count"); r.value_int(call_count);
    r.key("offensive_call_count"); r.value_int(offensive_count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_cb_catalog — Enumerate all CB types from CASUS_BELLI_DB (pure memory reads)
// Cacheable: CB types don't change during a game session, so we scan once and
// return the cached result on subsequent calls.
//
// Diagnostic-only — NOT exposed via the MCP `discover`/`query` surface. Agent
// callers should use `discover(what="valid_cbs_strict")` instead, which already
// returns per-CB index/name/target_mode plus enabled_count/disabled_count and
// disabled_cb_names in the summary. This handler stays in the bridge for
// scripts/tests that need the static catalog without an actor+target pair
// (re/findings/re_test_suite_audit.md §3; ck-xoj6x).
// =============================================================================

// Cache: populated on first call, returned verbatim afterwards
static std::string s_cb_catalog_cache CK3_EARLY_INIT;
static bool        s_cb_catalog_valid = false;

bool do_query_cb_catalog(const char* /*params_json*/, std::string* out) {
    // Return cached result if available
    if (s_cb_catalog_valid) {
        if (out) *out = s_cb_catalog_cache;
        return true;
    }

    // Resolve CASUS_BELLI_DB global pointer
    uintptr_t cb_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CASUS_BELLI_DB);
    if (!cb_db || !is_mapped(cb_db)) {
        if (out) *out = R"({"error":"casus_belli_db_not_found"})";
        return false;
    }

    // DB layout: +0x88 = array of CCasusBelliType*, +0x94 = count (int32)
    uintptr_t cb_arr  = safe_read<uintptr_t>(cb_db + ck3::game_db::ENTRIES_ARRAY);
    int32_t   cb_count = safe_read<int32_t>(cb_db + ck3::game_db::LIVE_COUNT);
    if (cb_count < 0 || cb_count > 500) cb_count = 0;

    if (!cb_arr || !is_mapped(cb_arr) || cb_count == 0) {
        if (out) *out = R"({"error":"casus_belli_db_empty","cb_db_ptr":")" +
            std::to_string(cb_db) + R"(","count":)" + std::to_string(cb_count) + "}";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_cb_catalog");

    r.key("cb_types"); r.begin_array();
    int emitted = 0;
    for (int32_t i = 0; i < cb_count && i < 200; i++) {
        uintptr_t cb_type = safe_read<uintptr_t>(cb_arr + i * 8);
        if (!cb_type || !is_mapped(cb_type)) continue;

        // Name via NAME_KEY (+0x10, std::string / SSO)
        std::string cb_name = ad_read_std_string(cb_type + ck3::db_object::NAME_KEY);

        // DB index at +0x08
        uint32_t cb_idx = safe_read<uint32_t>(cb_type + ck3::cb_type::DB_INDEX);

        // Target mode at +0x738 (0=none, 6=claim, other=title-targeted)
        int32_t target_mode = safe_read<int32_t>(cb_type + ck3::cb_type::TARGET_MODE);

        // Enabled flag at +0x73E
        uint8_t enabled = safe_read<uint8_t>(cb_type + ck3::cb_type::ENABLED_FLAG);

        r.begin_object();
        r.key("index"); r.value_uint(cb_idx);
        if (!cb_name.empty()) {
            r.key("name"); r.value_string(cb_name.c_str(), cb_name.size());
        } else {
            r.key("name"); r.value_null();
        }
        // Emit target_mode as both numeric and human-readable
        r.key("target_mode"); r.value_int(target_mode);
        const char* mode_label = (target_mode == ck3::cb_type::TARGET_NONE)  ? "none" :
                                 (target_mode == ck3::cb_type::TARGET_CLAIM) ? "claim" : "title";
        r.key("target_mode_label"); r.value_string(mode_label);
        r.key("enabled"); r.value_bool(enabled != 0);
        r.end_object();
        emitted++;
    }
    r.end_array();

    r.key("total"); r.value_int(emitted);
    r.key("cached"); r.value_bool(false);
    r.end_object();

    std::string result = r.take();

    // Cache for future calls
    s_cb_catalog_cache = result;
    s_cb_catalog_valid = true;

    if (out) *out = result;
    return true;
}

// =============================================================================
// search_characters — Ledger-style scan of CHARACTER_DB with filter + sort
// Parameters:
//   filter: "all" | "alive" | "dead" | "ruler" | "independent" | "ai_controlled"
//   sort: "military" | "gold" | "realm_size" | "tier" | "age" |
//         "diplomacy" | "martial" | "stewardship" | "intrigue" | "learning" | "prowess"
//   sort_order: "desc" | "asc"
//   max_results: 1-5000 (default 50; values above 5000 are clamped, not reset)
// =============================================================================

// Sort key extraction from a character pointer.
static int64_t extract_sort_key(uintptr_t c, uintptr_t title_realm, uintptr_t realm_data,
                                 const char* sort_field) {
    if (!sort_field || !sort_field[0]) sort_field = "military";

    if (strcmp(sort_field, "military") == 0) {
        return title_realm ? safe_read<int32_t>(title_realm + ck3::title_realm::MIL_CURRENT) : 0;
    }
    if (strcmp(sort_field, "gold") == 0) {
        return realm_data ? safe_read<int64_t>(realm_data + ck3::character::REALM_GOLD) : 0;
    }
    if (strcmp(sort_field, "realm_size") == 0) {
        return title_realm ? safe_read<int32_t>(title_realm + ck3::title_realm::TITLE_ARRAY_COUNT) : 0;
    }
    // "realm_counties" is keyed by do_search_characters itself (it needs the
    // realm-county index); it never reaches this function.
    if (strcmp(sort_field, "tier") == 0) {
        return title_realm ? safe_read<int32_t>(title_realm + ck3::title_realm::REALM_TIER) : 0;
    }
    if (strcmp(sort_field, "age") == 0) {
        // birth_date_short: lower value = born earlier = older
        return static_cast<int64_t>(safe_read<uint16_t>(c + ck3::character::BIRTH_DATE_SHORT));
    }
    // Skills
    if (strcmp(sort_field, "diplomacy") == 0)   return safe_read<int32_t>(c + ck3::character::SKILL_DIPLOMACY);
    if (strcmp(sort_field, "martial") == 0)     return safe_read<int32_t>(c + ck3::character::SKILL_MARTIAL);
    if (strcmp(sort_field, "stewardship") == 0) return safe_read<int32_t>(c + ck3::character::SKILL_STEWARDSHIP);
    if (strcmp(sort_field, "intrigue") == 0)    return safe_read<int32_t>(c + ck3::character::SKILL_INTRIGUE);
    if (strcmp(sort_field, "learning") == 0)    return safe_read<int32_t>(c + ck3::character::SKILL_LEARNING);
    if (strcmp(sort_field, "prowess") == 0)     return safe_read<int32_t>(c + ck3::character::SKILL_PROWESS);

    // Default fallback: military
    return title_realm ? safe_read<int32_t>(title_realm + ck3::title_realm::MIL_CURRENT) : 0;
}

// §1a (re_heir_education_RE.md): query_heir_education READ.
// The childhood education focus and the adult lifestyle focus share ONE slot:
// core_data(char+0x1a8/REALM_DATA) + ACTIVE_FOCUS_PTR(0x280) -> CActiveFocus ->
// FOCUS_TYPE_PTR(0x08) -> NAME_KEY(0x10) = focus name ("education_martial", ...).
// Decompile-proven across SetLifestyleFocus / set_focus effect / AI default-picker.
// Reuses ONLY existing HIGH offsets — no new offsets. guardian_id/_martial stay
// null (§1b DEEP-RE: relation-index sub-object offset unpinned; needs 1 live probe).
static const char* education_focus_to_skill(const std::string& f) {
    if (f == "education_martial")     return "martial";
    if (f == "education_diplomacy")   return "diplomacy";
    if (f == "education_stewardship") return "stewardship";
    if (f == "education_intrigue")    return "intrigue";
    if (f == "education_learning")    return "learning";
    return nullptr;
}

bool do_query_heir_education(const char* params_json, std::string* out) {
    int char_id_param = json_get_int(params_json, "character_id");
    void* character = nullptr;
    if (char_id_param > 0) {
        character = lookup_character(static_cast<uint32_t>(char_id_param));
    }
    ResponseBuilder r;
    r.begin_object();
    if (!character) {
        r.key("error"); r.value_string("character_not_found");
        r.key("character_id"); r.value_int(char_id_param);
        r.end_object();
        if (out) *out = r.take();
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    r.key("character_id"); r.value_uint(safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF);

    // §1a: active focus name via the proven adult-focus chain (same slot for a child).
    std::string focus_name;
    uintptr_t cd = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);  // core_data (present for all living chars)
    if (cd && is_mapped(cd)) {
        uintptr_t af = safe_read<uintptr_t>(cd + ck3::lifestyle::ACTIVE_FOCUS_PTR);
        if (af && is_mapped(af)) {
            uintptr_t ft = safe_read<uintptr_t>(af + ck3::active_focus::FOCUS_TYPE_PTR);
            if (ft && is_mapped(ft)) focus_name = ad_read_std_string(ft + ck3::db_object::NAME_KEY);
        }
    }
    if (!focus_name.empty()) {
        r.key("education_focus"); r.value_string(focus_name.c_str(), focus_name.size());
        const char* skill = education_focus_to_skill(focus_name);
        r.key("education_focus_skill");
        if (skill) r.value_string(skill); else r.value_null();
        r.key("education_focus_is_education"); r.value_bool(focus_name.rfind("education_", 0) == 0);
    } else {
        r.key("education_focus"); r.value_null();
        r.key("education_focus_skill"); r.value_null();
        r.key("education_focus_is_education"); r.value_bool(false);
    }

    // Current martial (the conqueror-gate base) — HIGH offset, always readable.
    r.key("child_martial"); r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_MARTIAL));

    // §1b DEEP-RE: guardian relation-index offset unpinned in the dumps. Honest null
    // (NOT faked) until one live probe of FUN_06777940(ward, rel_type_id=8) confirms it.
    r.key("guardian_id"); r.value_null();
    r.key("guardian_martial"); r.value_null();
    r.key("guardian_learning"); r.value_null();
    // §1c DEEP-RE: education_<skill>_variable needs the char variable-store accessor.
    r.key("projected_education_rank"); r.value_null();
    r.key("guardian_unresolved"); r.value_bool(true);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// §4 (re_heir_guardian_RE.md): immediate-liege / court-membership resolver.
// Covers BOTH arms the guardian-finder needs:
//   (a) unlanded courtier -> courtier_data(char+0x1b0)+0xC8 back-ref (the load-bearing arm;
//       get_liege_id_from_addr returns 0 for courtiers because resolve_title_realm is null).
//   (b) landed direct vassal -> get_liege_id_from_addr (realm-hierarchy holder).
// Read-only, HIGH-confidence offsets, same safe_read/is_mapped discipline as the DB walk.
static uint32_t immediate_liege_id(uintptr_t c) {
    uintptr_t cd = safe_read<uintptr_t>(c + ck3::character::COURTIER_DATA);   // +0x1b0
    if (cd && is_mapped(cd)) {
        uint32_t lid = safe_read<uint32_t>(cd + ck3::courtier_data::LIEGE_CHAR_REF) & 0xFFFFFF; // +0xC8
        if (lid && lid != 0xFFFFFF) return lid;
    }
    return get_liege_id_from_addr(c) & 0xFFFFFF;
}

bool do_search_characters(const char* params_json, std::string* out) {
    // Parse parameters
    const char* filter_str = json_get_string(params_json, "filter");
    const char* sort_str   = json_get_string(params_json, "sort");
    const char* order_str  = json_get_string(params_json, "sort_order");
    int max_results        = json_get_int(params_json, "max_results");
    // §4: court_of=<ruler_id> -> keep only that ruler's immediate court members
    // (courtiers + direct vassals). Read-only predicate over the existing DB walk.
    int court_of           = json_get_int(params_json, "court_of");
    // L1b: lean-by-default. The skills object, government string, and per-row
    // GET_AGE call are NOT consumed by any agent path (digestor.py / prompts.py
    // read only id/name/military/gold/realm_size/tier). Emit them only on opt-in.
    bool verbose           = json_get_bool(params_json, "verbose", false);

    // Defaults
    if (!filter_str || !filter_str[0]) filter_str = "alive";
    if (!sort_str || !sort_str[0])     sort_str   = "military";
    if (!order_str || !order_str[0])   order_str  = "desc";
    // ck-bd4w: clamp out-of-range values rather than resetting to default.
    // L1b: lean default lowers the row count (25) and clamp ceiling (500) to
    // keep payloads small; verbose callers retain the wider 5000 ceiling for
    // bulk pool enumeration (16B per Candidate -> ~80 KB worst case).
    if (max_results <= 0) max_results = verbose ? 50 : 25;
    int max_clamp = verbose ? 5000 : 500;
    if (max_results > max_clamp) max_results = max_clamp;

    bool descending = (strcmp(order_str, "asc") != 0);

    // For age sort: "desc" means oldest first.
    // birth_date_short is days-from-epoch: lower = born earlier = older.
    // So for age desc (oldest): we want the N smallest birth_date values.
    // For age asc (youngest): we want the N largest birth_date values.
    bool age_sort = (strcmp(sort_str, "age") == 0);

    // Resolve CHARACTER_DB
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (!db) {
        if (out) *out = "{\"error\":\"CHARACTER_DB not available\"}";
        return false;
    }
    uintptr_t tbl = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (!tbl || capacity == 0 || capacity > 500000) {
        if (out) *out = "{\"error\":\"CHARACTER_DB table invalid\"}";
        return false;
    }
    uintptr_t sentinel = safe_read_offset<uintptr_t>(ck3::globals::CHAR_DB_SENTINEL);

    // Filter flags
    bool filter_alive       = true;   // always filter dead unless filter=all or filter=dead
    bool filter_dead_only   = false;  // ck-a1fw: filter=dead → only dead characters
    bool filter_ruler       = false;
    bool filter_independent = false;
    bool filter_ai          = false;

    if (strcmp(filter_str, "all") == 0)               { filter_alive = false; }
    else if (strcmp(filter_str, "alive") == 0)         { /* defaults */ }
    else if (strcmp(filter_str, "dead") == 0)          { filter_alive = false; filter_dead_only = true; }
    else if (strcmp(filter_str, "ruler") == 0)         { filter_ruler = true; }
    else if (strcmp(filter_str, "independent") == 0)   { filter_ruler = true; filter_independent = true; }
    else if (strcmp(filter_str, "ai_controlled") == 0) { filter_ai = true; }

    // Candidate struct for bounded-heap top-N selection
    struct Candidate {
        uintptr_t char_ptr;
        int64_t sort_key;
    };

    std::vector<Candidate> heap;
    heap.reserve(static_cast<size_t>(max_results) + 1);

    // Heap direction:
    // For non-age descending: keep largest N -> use min-heap (evict smallest)
    // For non-age ascending: keep smallest N -> use max-heap (evict largest)
    // For age descending (oldest): keep smallest birth_date N -> use max-heap (evict largest birth_date)
    // For age ascending (youngest): keep largest birth_date N -> use min-heap (evict smallest birth_date)
    bool use_min_heap;
    if (age_sort) {
        use_min_heap = !descending;  // asc (youngest) -> min-heap on birth_date
    } else {
        use_min_heap = descending;   // desc (biggest) -> min-heap
    }

    auto cmp_min = [](const Candidate& a, const Candidate& b) { return a.sort_key > b.sort_key; };
    auto cmp_max = [](const Candidate& a, const Candidate& b) { return a.sort_key < b.sort_key; };

    int scanned = 0, matched = 0;

    // lane-query item 5: realm county counts (one pass, see
    // build_realm_county_index). Emitted per row as `realm_counties` and usable
    // as sort="realm_counties".
    std::unordered_map<uint32_t, int> realm_county_index;
    build_realm_county_index(realm_county_index);
    const bool sort_realm_counties = (strcmp(sort_str, "realm_counties") == 0);

    for (uint32_t i = 0; i < capacity; i++) {
        uintptr_t entry = safe_read<uintptr_t>(tbl + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
        if (!entry || entry < 0x10000 || entry > 0x7FFFFFFFFFFF) continue;
        if (entry == sentinel) continue;
        scanned++;

        uintptr_t c = entry;

        // Alive/dead filter: read DEAD_FLAG byte directly (faster than function call for bulk scan)
        // filter=alive (default)    → skip dead
        // filter=all                → keep both
        // filter=dead  (ck-a1fw)    → skip alive
        uint8_t dead_flag_byte = safe_read<uint8_t>(c + ck3::character::DEAD_FLAG);
        if (filter_alive) {
            if (dead_flag_byte != 0) continue;
        } else if (filter_dead_only) {
            if (dead_flag_byte == 0) continue;
        }

        // Title realm (needed for ruler/independent/military/realm_size/tier)
        uintptr_t title_realm = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);

        // Ruler filter
        if (filter_ruler && !title_realm) continue;

        // Independent filter: must be a ruler whose RESOLVED liege id is 0.
        // FIX (independence-resolver): the old test (title_realm+0x448 LIEGE_PTR null) was
        // wrong in BOTH directions — it kept null-0x448 vassals (most vassals) and excluded
        // independents whose 0x448 is a non-null mapped sentinel. Route through the
        // authoritative get_liege_id_from_addr (PATH2 realm-hierarchy, 0xFFFFFF normalized
        // to 0) so independent ⇔ liege_id == 0.
        if (filter_independent) {
            if (!title_realm || !is_mapped(title_realm)) continue;
            if (get_liege_id_from_addr(c) != 0) continue;
        }

        // AI controlled filter
        if (filter_ai) {
            // lane-query item 3: was the +0x199 byte, which is not an AI flag.
            if (!char_is_ai_controlled(c)) continue;
        }

        // §4: court_of predicate — keep only c whose immediate liege == court_of.
        if (court_of > 0 && immediate_liege_id(c) != (uint32_t)court_of) continue;

        // Resolve realm_data for gold sort
        uintptr_t realm_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);

        int64_t key;
        if (sort_realm_counties) {
            auto it = realm_county_index.find(safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF);
            key = (it == realm_county_index.end()) ? 0 : it->second;
        } else {
            key = extract_sort_key(c, (title_realm && is_mapped(title_realm)) ? title_realm : 0,
                                   (realm_data && is_mapped(realm_data)) ? realm_data : 0, sort_str);
        }
        matched++;

        // Insert into bounded heap (maintain top-N)
        heap.push_back(Candidate{ c, key });

        if (static_cast<int>(heap.size()) > max_results) {
            if (use_min_heap) {
                std::push_heap(heap.begin(), heap.end(), cmp_min);
                std::pop_heap(heap.begin(), heap.end(), cmp_min);
            } else {
                std::push_heap(heap.begin(), heap.end(), cmp_max);
                std::pop_heap(heap.begin(), heap.end(), cmp_max);
            }
            heap.pop_back();
        } else {
            if (use_min_heap) {
                std::push_heap(heap.begin(), heap.end(), cmp_min);
            } else {
                std::push_heap(heap.begin(), heap.end(), cmp_max);
            }
        }
    }

    // Final sort of the N results in display order
    if (age_sort) {
        if (descending) {
            // Oldest first = lowest birth_date_short
            std::stable_sort(heap.begin(), heap.end(), [](const Candidate& a, const Candidate& b) {
                return a.sort_key < b.sort_key;
            });
        } else {
            std::stable_sort(heap.begin(), heap.end(), [](const Candidate& a, const Candidate& b) {
                return a.sort_key > b.sort_key;
            });
        }
    } else {
        if (descending) {
            std::stable_sort(heap.begin(), heap.end(), [](const Candidate& a, const Candidate& b) {
                return a.sort_key > b.sort_key;
            });
        } else {
            std::stable_sort(heap.begin(), heap.end(), [](const Candidate& a, const Candidate& b) {
                return a.sort_key < b.sort_key;
            });
        }
    }

    // Build JSON response
    ResponseBuilder r;
    r.begin_object();
    r.key("filter"); r.value_string(filter_str, strlen(filter_str));
    r.key("sort");   r.value_string(sort_str, strlen(sort_str));
    r.key("sort_order"); r.value_string(order_str, strlen(order_str));
    if (court_of > 0) { r.key("court_of"); r.value_int(court_of); }
    r.key("scanned"); r.value_int(scanned);
    r.key("matched"); r.value_int(matched);
    r.key("returned"); r.value_int(static_cast<int>(heap.size()));

    r.key("characters"); r.begin_array();
    for (const auto& cand : heap) {
        uintptr_t c = cand.char_ptr;
        r.begin_object();

        uint32_t id = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;
        r.key("id"); r.value_uint(id);

        // Name: `name`, or `name_key` + `name_pending` until the game thread
        // localizes it (lane-namekey).
        emit_char_name(r, c);

        // ck-a1fw: is_dead exposed so harness can consume filter=dead/all results.
        uint8_t dead_byte = safe_read<uint8_t>(c + ck3::character::DEAD_FLAG);
        r.key("is_dead"); r.value_bool(dead_byte != 0);

        // Flags
        uintptr_t title_realm = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
        bool is_ruler = (title_realm != 0 && (title_realm && is_mapped(title_realm)));
        r.key("is_ruler"); r.value_bool(is_ruler);

        // FIX (independence-resolver): independent ⇔ resolved liege id == 0 (PATH2 authoritative,
        // 0xFFFFFF sentinel normalized). Was: raw title_realm+0x448 null-test (wrong both ways).
        bool is_independent = is_ruler && (get_liege_id_from_addr(c) == 0);
        r.key("is_independent"); r.value_bool(is_independent);

        // lane-query item 3: the same definition the `character` profile uses.
        r.key("is_ai"); r.value_bool(char_is_ai_controlled(c));

        // Military
        int32_t mil = 0;
        if (is_ruler) {
            mil = safe_read<int32_t>(title_realm + ck3::title_realm::MIL_CURRENT);
        }
        r.key("military"); r.value_int(mil);

        // Gold
        uintptr_t realm_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
        double gold = 0.0;
        if (realm_data && (realm_data && is_mapped(realm_data))) {
            gold = static_cast<double>(safe_read<int64_t>(realm_data + ck3::character::REALM_GOLD)) / 100000.0;
        }
        r.key("gold"); r.value_float(gold);

        // Realm size. UNCHANGED MEANING (lane-query item 5): `realm_size` is the
        // count of titles this character holds DIRECTLY, all tiers, baronies
        // included (title_realm+0x1EC). It is NOT territory: it drops when the
        // AI grants titles to vassals and stays flat when a vassal conquers.
        // `held_title_count` is the same number under its honest name;
        // `realm_counties` is the territory measure (own + vassal counties).
        int32_t realm_size = 0;
        if (is_ruler) {
            realm_size = safe_read<int32_t>(title_realm + ck3::title_realm::TITLE_ARRAY_COUNT);
        }
        r.key("realm_size"); r.value_int(realm_size);
        r.key("held_title_count"); r.value_int(realm_size);
        {
            auto it = realm_county_index.find(id);
            r.key("realm_counties"); r.value_int(it == realm_county_index.end() ? 0 : it->second);
        }

        // Tier
        int32_t tier = 0;
        if (is_ruler) {
            tier = safe_read<int32_t>(title_realm + ck3::title_realm::REALM_TIER);
        }
        r.key("tier"); r.value_int(tier);

        // Government type (ck-51, 20260614): title_realm+0x3F0 → CGovernmentType*;
        // key std::string at gov_obj+0x10. Lets callers filter feudal/clan/
        // administrative/tribal/mandala/etc. (the feudal-vs-admin distinction the
        // matrix flagged as missing). Empty for unlanded chars.
        // Government type / age / skills are verbose-only (L1b) — not read by any
        // agent consumer in lean mode.
        if (verbose) {
            if (is_ruler) {
                uintptr_t gov_obj = safe_read<uintptr_t>(title_realm + ck3::title_realm::GOVERNMENT_TYPE_PTR);
                if (gov_obj && is_mapped(gov_obj)) {
                    std::string gov = ad_read_std_string(gov_obj + ck3::db_object::NAME_KEY);
                    if (!gov.empty()) { r.key("government"); r.value_string(gov.c_str(), gov.size()); }
                }
            }

            // Age (call GET_AGE for proper display value)
            {
                auto fn_age = reinterpret_cast<int16_t(*)(void*)>(bridge::game_fn(ck3::funcs::GET_AGE));
                int16_t age = fn_age(reinterpret_cast<void*>(c));
                r.key("age"); r.value_int(age);
            }

            // Skills
            r.key("skills"); r.begin_object();
            r.key("diplomacy");   r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_DIPLOMACY));
            r.key("martial");     r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_MARTIAL));
            r.key("stewardship"); r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_STEWARDSHIP));
            r.key("intrigue");    r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_INTRIGUE));
            r.key("learning");    r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_LEARNING));
            r.key("prowess");     r.value_int(safe_read<int32_t>(c + ck3::character::SKILL_PROWESS));
            r.end_object();
        }

        r.end_object();
    }
    r.end_array();
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_marriage_candidates — the marriage-market browser the agent was missing.
// Scans CHARACTER_DB for landed rulers who are (a) alive, (b) not the actor,
// (c) unmarried (best-effort, landed-spouse slot), and — when require_in_range —
// (d) within diplomatic range of the actor's capital. Returns top-N by `sort`
// (default military), each annotated with age / distance / has_spouse so the
// agent can pick a concrete target_id instead of blind-firing arrange_marriage.
//
// Honest scope: the engine's full eligibility (faith marriage-type doctrine,
// consanguinity via IsCloseFamily, sex-doctrine) is NOT replicated here — it is
// game-data-driven and lives in the scripted is_valid/can_send triggers. After
// picking a candidate, gate the actual proposal with
//   discover(what="acceptance", interaction_type="arrange_marriage_interaction").
// Uses only HIGH-confidence offsets already exercised by do_search_characters
// and do_query_diplomatic_range. (re_marriage_eligibility.md, ck-leg)
// =============================================================================

// Resolve a character's capital map coordinates (liege-walk for unlanded).
// Mirrors the get_capital_coords lambda in do_query_diplomatic_range; kept as a
// file-static so the marriage scan can call it per candidate without touching
// the working diplomatic-range handler.
static bool mc_capital_xy(uintptr_t c, int32_t& out_x, int32_t& out_y) {
    for (int hops = 0; hops < 10; hops++) {
        uintptr_t title_realm = resolve_title_realm(c);
        if (title_realm) {
            uint32_t barony_tid = safe_read<uint32_t>(title_realm + ck3::title_realm::CAPITAL_BARONY_ID);
            if (barony_tid != 0 && barony_tid != 0xFFFFFFFF) {
                uintptr_t barony = lookup_title_obj(barony_tid);
                if (barony) {
                    uintptr_t province = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
                    if (province && is_mapped(province)) {
                        uintptr_t tmpl = safe_read<uintptr_t>(province + ck3::province::TEMPLATE_PTR);
                        if (tmpl && is_mapped(tmpl)) {
                            out_x = safe_read<int32_t>(tmpl + ck3::province_template::CENTER_X);
                            out_y = safe_read<int32_t>(tmpl + ck3::province_template::CENTER_Y);
                            return true;
                        }
                    }
                }
            }
            uintptr_t liege_ptr = safe_read<uintptr_t>(title_realm + ck3::title_realm::LIEGE_PTR);
            if (liege_ptr && is_mapped(liege_ptr)) { c = liege_ptr; continue; }
            return false;
        }
        return false;  // unlanded with no realm — skip (marriage browser is landed-focused)
    }
    return false;
}

// Returns true if the character has at least one LIVING spouse, via the
// canonical family SPOUSE_ARRAY path (mirrors build_char_profile spouses at
// line ~467). The Realm spouse slot (realm_data+0x288) misses most married
// characters; the family array is the authoritative source. Best-effort and
// fail-open: an unreadable family pointer reports "no spouse" so the candidate
// is still surfaced (the proposal is later gated by discover(acceptance)).
static bool mc_has_living_spouse(uintptr_t c) {
    uintptr_t family_ptr = safe_read<uintptr_t>(c + ck3::character::FAMILY_DATA);
    if (!family_ptr || !is_mapped(family_ptr)) return false;
    uintptr_t spouse_arr = family_ptr + ck3::family::SPOUSE_ARRAY;
    int32_t spouse_count = safe_read<int32_t>(spouse_arr + 0x0C);
    uintptr_t spouse_data = safe_read<uintptr_t>(spouse_arr + 0x00);
    if (!spouse_data || !is_mapped(spouse_data) || spouse_count <= 0 || spouse_count > 10) {
        return false;
    }
    for (int i = 0; i < spouse_count; i++) {
        uint32_t spouse_id = safe_read<uint32_t>(spouse_data + i * sizeof(uint32_t)) & 0xFFFFFF;
        if (spouse_id == 0 || spouse_id == 0xFFFFFF) continue;
        void* spouse_char = lookup_character(spouse_id);
        if (spouse_char && g_char_is_alive && g_char_is_alive(spouse_char)) return true;
    }
    return false;
}

bool do_query_marriage_candidates(const char* params_json, std::string* out) {
    int actor_param = json_get_int(params_json, "character_id");
    if (actor_param < 0) actor_param = 0;
    uint32_t actor_id = static_cast<uint32_t>(actor_param) & 0xFFFFFF;
    if (actor_id == 0) actor_id = g_controlled_primary.load(std::memory_order_relaxed) & 0xFFFFFF;

    const char* sort_str = json_get_string(params_json, "sort");
    if (!sort_str || !sort_str[0]) sort_str = "military";
    int max_results = json_get_int(params_json, "max_results");
    if (max_results <= 0) max_results = 30;
    if (max_results > 500) max_results = 500;
    // require_in_range defaults TRUE; pass require_in_range=0 to browse globally.
    bool require_in_range = true;
    if (json_has_key(params_json, "require_in_range")) {
        require_in_range = json_get_int(params_json, "require_in_range") != 0;
    }

    void* actor = lookup_character(actor_id);
    if (!actor || is_sentinel(actor)) {
        if (out) *out = R"({"error":"actor_not_found","action":"query_marriage_candidates"})";
        return false;
    }
    int32_t ax = 0, ay = 0;
    bool actor_located = mc_capital_xy(reinterpret_cast<uintptr_t>(actor), ax, ay);
    if (require_in_range && !actor_located) {
        if (out) *out = R"({"error":"actor_capital_unresolved","action":"query_marriage_candidates","hint":"pass require_in_range=0 to browse without a range filter"})";
        return false;
    }

    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (!db) { if (out) *out = R"({"error":"CHARACTER_DB not available"})"; return false; }
    uintptr_t tbl = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (!tbl || capacity == 0 || capacity > 500000) {
        if (out) *out = R"({"error":"CHARACTER_DB table invalid"})"; return false;
    }
    uintptr_t sentinel = safe_read_offset<uintptr_t>(ck3::globals::CHAR_DB_SENTINEL);
    ADLOG("marriage_candidates: ENTER actor=%u located=%d capacity=%u require_in_range=%d\n",
          actor_id, actor_located ? 1 : 0, capacity, require_in_range ? 1 : 0);

    // FIX I14: diplomatic range is now a SCAN PREDICATE — distance is computed in
    // the scan loop BEFORE a candidate enters the heap, so the heap fills only with
    // in-range candidates ranked by the sort field (rank-then-filter evicted every
    // nearby neighbour for peripheral actors → matched=3697, returned=0). No over-
    // fetch cushion needed; matched now counts post-range survivors (honest shape).
    int scan_keep = max_results;
    if (scan_keep > 600) scan_keep = 600;   // bounds the post-scan capital calcs

    struct Cand { uintptr_t ptr; int64_t key; int32_t dist; };
    std::vector<Cand> heap;
    heap.reserve(static_cast<size_t>(scan_keep) + 1);
    auto cmp_min = [](const Cand& a, const Cand& b) { return a.key > b.key; }; // min-heap: evict smallest
    int scanned = 0, matched = 0;

    for (uint32_t i = 0; i < capacity; i++) {
        uintptr_t entry = safe_read<uintptr_t>(tbl + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
        if (!entry || entry < 0x10000 || entry > 0x7FFFFFFFFFFF) continue;
        if (entry == sentinel) continue;
        scanned++;
        uintptr_t c = entry;

        // (a) alive
        if (safe_read<uint8_t>(c + ck3::character::DEAD_FLAG) != 0) continue;
        // (b) not the actor
        if ((safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF) == actor_id) continue;
        // landed-ruler focus: must have a title_realm
        uintptr_t title_realm = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
        if (!title_realm || !is_mapped(title_realm)) continue;
        // (c) unmarried — via the canonical family SPOUSE_ARRAY (the Realm
        // spouse slot at realm_data+0x288 misses most married characters).
        uintptr_t realm_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);
        if (mc_has_living_spouse(c)) continue;  // married
        // FIX I14: diplomatic range is a SCAN PREDICATE — exclude out-of-range
        // candidates BEFORE they enter the heap so the heap fills with in-range
        // rulers ranked by the sort field (was: range-filter the ranked top-N,
        // which evicted every nearby neighbour for a peripheral actor).
        int32_t cand_dist = -1;
        if (actor_located && require_in_range) {
            int32_t cx = 0, cy = 0;
            if (!mc_capital_xy(c, cx, cy)) continue;   // unplaceable → exclude
            int64_t dx = static_cast<int64_t>(ax) - cx;
            int64_t dy = static_cast<int64_t>(ay) - cy;
            int64_t dsq = dx * dx + dy * dy;
            if (dsq > static_cast<int64_t>(ck3::diplomatic_range::MAXIMUM_RANGE_SQUARED)) continue;
            cand_dist = static_cast<int32_t>(std::sqrt(static_cast<double>(dsq)) + 0.5);
        }
        matched++;

        int64_t key = extract_sort_key(c, title_realm, (realm_data && is_mapped(realm_data)) ? realm_data : 0, sort_str);
        heap.push_back(Cand{ c, key, cand_dist });
        if (static_cast<int>(heap.size()) > scan_keep) {
            std::push_heap(heap.begin(), heap.end(), cmp_min);
            std::pop_heap(heap.begin(), heap.end(), cmp_min);
            heap.pop_back();
        } else {
            std::push_heap(heap.begin(), heap.end(), cmp_min);
        }
    }

    ADLOG("marriage_candidates: scan done scanned=%d matched=%d kept=%zu scan_keep=%d\n",
          scanned, matched, heap.size(), scan_keep);
    std::stable_sort(heap.begin(), heap.end(), [](const Cand& a, const Cand& b) { return a.key > b.key; });

    // FIX I14: range is already enforced in the scan (cand.dist set there). Take the
    // first max_results, and for the non-range path compute distance for display only.
    if (static_cast<int>(heap.size()) > max_results) heap.resize(max_results);
    if (actor_located && !require_in_range) {
        for (auto& cand : heap) {
            int32_t cx = 0, cy = 0;
            if (mc_capital_xy(cand.ptr, cx, cy)) {
                int64_t dx = static_cast<int64_t>(ax) - cx;
                int64_t dy = static_cast<int64_t>(ay) - cy;
                int64_t dsq = dx * dx + dy * dy;
                cand.dist = static_cast<int32_t>(std::sqrt(static_cast<double>(dsq)) + 0.5);
            }
        }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_marriage_candidates");
    r.key("actor_id"); r.value_uint(actor_id);
    r.key("sort"); r.value_string(sort_str, strlen(sort_str));
    r.key("require_in_range"); r.value_bool(require_in_range);
    r.key("scanned"); r.value_int(scanned);
    r.key("matched"); r.value_int(matched);
    r.key("returned"); r.value_int(static_cast<int>(heap.size()));
    r.key("note"); r.value_string("landed unmarried rulers; faith/consanguinity/sex gates NOT applied — confirm with discover(what=acceptance, interaction_type=arrange_marriage_interaction)");
    r.key("candidates"); r.begin_array();
    for (const auto& cand : heap) {
        uintptr_t c = cand.ptr;
        r.begin_object();
        r.key("id"); r.value_uint(safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF);
        emit_char_name(r, c);   // name, or name_key + name_pending (lane-namekey)
        {
            auto fn_age = reinterpret_cast<int16_t(*)(void*)>(bridge::game_fn(ck3::funcs::GET_AGE));
            int16_t age = fn_age(reinterpret_cast<void*>(c));
            r.key("age"); r.value_int(age);
            r.key("likely_adult"); r.value_bool(age >= 16);
        }
        uintptr_t title_realm = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
        bool is_ruler = (title_realm && is_mapped(title_realm));
        r.key("is_ruler"); r.value_bool(is_ruler);
        // FIX (independence-resolver): independent ⇔ resolved liege id == 0 (see search_characters).
        bool is_independent = is_ruler && (get_liege_id_from_addr(c) == 0);
        r.key("is_independent"); r.value_bool(is_independent);
        r.key("military"); r.value_int(is_ruler ? safe_read<int32_t>(title_realm + ck3::title_realm::MIL_CURRENT) : 0);
        r.key("tier"); r.value_int(is_ruler ? safe_read<int32_t>(title_realm + ck3::title_realm::REALM_TIER) : 0);
        r.key("realm_size"); r.value_int(is_ruler ? safe_read<int32_t>(title_realm + ck3::title_realm::TITLE_ARRAY_COUNT) : 0);
        r.key("has_spouse"); r.value_bool(false);  // filtered above via family SPOUSE_ARRAY
        if (cand.dist >= 0) { r.key("distance"); r.value_int(cand.dist); }
        r.end_object();
    }
    r.end_array();
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_title_claimants: Who has claims on title X?
// Reads claimant array from title+0x2A8 (data ptr) and title+0x2B4 (count).
// Each entry is a uint32 char ref (stride 4). For each claimant, resolves the
// character and checks their claim array for pressed/implicit info.
// =============================================================================
bool do_query_title_claimants(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    if (title_id_param <= 0) {
        if (out) *out = R"({"error":"missing_title_id","hint":"Provide title_id parameter"})";
        return false;
    }

    uint32_t title_id = static_cast<uint32_t>(title_id_param) & 0xFFFFFF;
    uintptr_t tobj = lookup_title_obj(title_id);
    if (!tobj) {
        if (out) *out = R"({"error":"title_not_found"})";
        return false;
    }

    // Optional attacker scoping: when provided, per-claimant court-membership
    // and the combined backable verdict are computed (mirroring the write gate
    // intent_dispatch.cpp:1396-1431). When absent, behaviour is the unchanged
    // global roster (backward compatible).
    int atk_param = json_get_int(params_json, "attacker_id");
    if (atk_param <= 0) atk_param = json_get_int(params_json, "actor_id");
    uint32_t atk_id = (atk_param > 0) ? (static_cast<uint32_t>(atk_param) & 0xFFFFFF) : 0;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_title_claimants");
    if (atk_id != 0) { r.key("attacker_id"); r.value_uint(atk_id); }

    // Emit title info
    r.key("title_id"); r.value_uint(title_id);
    auto display_name = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
    if (!display_name.empty()) {
        r.key("title_name"); r.value_string(display_name.c_str(), display_name.size());
    }
    uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
    if (type_ptr) {
        auto title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        if (!title_key.empty()) {
            r.key("title_key"); r.value_string(title_key.c_str(), title_key.size());
        }
        int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
        if (tier >= 1 && tier <= 5) {
            r.key("tier"); r.value_int(tier);
            const char* tier_names[] = {"", "barony", "county", "duchy", "kingdom", "empire"};
            r.key("tier_name"); r.value_string(tier_names[tier]);
        }
    }

    // Current holder (the war defender for a claim_cb on this title).
    uint32_t holder_raw = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID);
    bool holder_present = false;
    DefenderState holder_state;
    void* holder_ptr = nullptr;           // function-scope: alliance/truce probe below
    uint32_t holder_char_id = 0;
    if (holder_raw != 0 && holder_raw != 0xFFFFFFFF) {
        uint32_t holder_id = holder_raw & 0xFFFFFF;
        r.key("holder_id"); r.value_uint(holder_id);
        void* holder = lookup_character(holder_raw);
        if (holder && !is_sentinel(holder)) {
            uintptr_t holder_addr = reinterpret_cast<uintptr_t>(holder);
            holder_ptr = holder;
            holder_char_id = holder_id;
            std::string hname = ad_read_char_name(holder_addr);
            if (!hname.empty()) {
                r.key("holder_name"); r.value_string(hname.c_str(), hname.size());
            }
            // War-declarability honesty: the defender (holder) must be a legal
            // claim_cb target — independent (intent_dispatch.cpp:1478) and not
            // imprisoned (war_validity R4) — or the engine culls the war.
            holder_state = probe_defender_state(holder_addr);
            holder_present = true;
            r.key("holder_independent"); r.value_bool(holder_state.independent);
            if (!holder_state.independent) {
                r.key("holder_liege_id"); r.value_uint(holder_state.liege_id);
            }
            r.key("holder_imprisoned"); r.value_bool(holder_state.imprisoned);
        }
    }

    // Attacker-vs-holder relation (alliance/truce/already-at-war) — constant per
    // query (depends only on attacker+holder, NOT the claimant), so compute once
    // outside the claimant loop. Mirrors the write-path target-state gates
    // (intent_dispatch.cpp:1565-1571) + engine Gate 0, matching the strict
    // surface. Only meaningful when an attacker is scoped and a holder exists.
    bool holder_in_alliance = false, holder_in_truce = false, holder_already_at_war = false;
    void* atk_char = (atk_id != 0) ? lookup_character(atk_id) : nullptr;
    if (atk_id != 0 && holder_present && atk_char && !is_sentinel(atk_char)) {
        probe_alliance_truce(atk_char, holder_ptr, holder_in_alliance, holder_in_truce);
        holder_already_at_war = actor_at_war_opposing(
            reinterpret_cast<uintptr_t>(atk_char), holder_char_id);
        r.key("holder_in_alliance"); r.value_bool(holder_in_alliance);
        r.key("holder_in_truce"); r.value_bool(holder_in_truce);
        r.key("already_at_war_opposing"); r.value_bool(holder_already_at_war);
    }

    // Read claimant array from title object
    uintptr_t claimant_arr = safe_read<uintptr_t>(tobj + ck3::title_obj::CLAIMANT_ARRAY_PTR);
    int32_t claimant_count = safe_read<int32_t>(tobj + ck3::title_obj::CLAIMANT_COUNT);

    // Sanity check
    if (claimant_count < 0 || claimant_count > 500) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_title_claimants: claimant_count=%d out of range, clamping to 0\n", claimant_count);
        claimant_count = 0;
    }

    // Cap at 50
    int32_t emit_count = (claimant_count > 50) ? 50 : claimant_count;

    r.key("claimant_count"); r.value_int(claimant_count);
    r.key("claimants"); r.begin_array();

    if (claimant_arr && emit_count > 0) {
        for (int32_t i = 0; i < emit_count; i++) {
            uint32_t char_ref = safe_read<uint32_t>(claimant_arr + i * 4);
            if (char_ref == 0 || char_ref == 0xFFFFFFFF) continue;

            uint32_t char_id = char_ref & 0xFFFFFF;
            void* character = lookup_character(char_ref);
            if (!character || is_sentinel(character)) continue;

            uintptr_t c = reinterpret_cast<uintptr_t>(character);

            r.begin_object();
            r.key("char_id"); r.value_uint(char_id);

            // Character name: name, or name_key + name_pending (lane-namekey)
            emit_char_name(r, c);

            // Is alive?
            bool alive = g_char_is_alive ? g_char_is_alive(reinterpret_cast<void*>(c)) : true;
            r.key("alive"); r.value_bool(alive);

            // Is ruler? (char+0x1B8 != 0)
            uintptr_t title_realm = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
            r.key("is_ruler"); r.value_bool(title_realm != 0);

            // Look up claim type by scanning character's claim array
            bool pressed = false;
            bool implicit_flag = false;
            bool found_claim = false;

            uintptr_t core_data = safe_read<uintptr_t>(c + ck3::character::REALM_DATA);  // char+0x1A8
            if (core_data) {
                uintptr_t claims_pdx = core_data + ck3::core_data::CLAIMS_ARRAY;  // core_data+0x1E8
                uintptr_t claim_data_ptr = safe_read<uintptr_t>(claims_pdx + 0x00);
                int32_t claim_cnt = safe_read<int32_t>(claims_pdx + 0x0C);
                if (claim_cnt < 0 || claim_cnt > 200) claim_cnt = 0;

                for (int32_t j = 0; j < claim_cnt; j++) {
                    uintptr_t entry = claim_data_ptr + j * ck3::claim_entry::ENTRY_SIZE;
                    uint32_t claim_tid = safe_read<uint32_t>(entry + ck3::claim_entry::TITLE_ID);
                    if ((claim_tid & 0xFFFFFF) == title_id) {
                        pressed = (safe_read<uint8_t>(entry + ck3::claim_entry::PRESSED) != 0);
                        implicit_flag = (safe_read<uint8_t>(entry + ck3::claim_entry::IMPLICIT) != 0);
                        found_claim = true;
                        break;
                    }
                }
            }

            r.key("pressed"); r.value_bool(pressed);
            r.key("implicit"); r.value_bool(implicit_flag);
            if (found_claim) {
                if (implicit_flag) {
                    r.key("type"); r.value_string("implicit");
                } else {
                    r.key("type"); r.value_string(pressed ? "pressed" : "unpressed");
                }
            } else {
                r.key("type"); r.value_string("unknown");
            }

            // --- Court-membership + backable verdict (attacker-scoped) -------
            // Mirrors the write gate intent_dispatch.cpp:1396-1431: a claimant
            // is only usable by the attacker if it presses its own claim (self)
            // or its resolved liege/court-host IS the attacker. An independent
            // claimant or a courtier of a THIRD party reports backable=false so
            // the agent does not pick a claimant the WCC rejects as
            // claimant_not_in_court.
            if (atk_id != 0) {
                bool in_court = false;
                bool court_resolved = false;
                uint32_t claimant_liege_id = 0;
                if (char_id == atk_id) {
                    in_court = true; court_resolved = true; claimant_liege_id = atk_id;
                } else {
                    // Landed claimant → authoritative liege resolution
                    // (get_liege_id_from_addr normalizes the 0xFFFFFF "no-liege"
                    // sentinel and the junk economy-sentinel object the raw
                    // title_realm+0x448 LIEGE_PTR returns for an independent
                    // ruler — L22 / dispatch_router.cpp:85). Returns 0 for a
                    // landless courtier (no title_realm), so the courtier_data
                    // fallback below still runs.
                    uint32_t lid = get_liege_id_from_addr(c) & 0xFFFFFF;
                    if (lid != 0 && lid != 0xFFFFFF) {
                        claimant_liege_id = lid;
                        court_resolved = true;
                    }
                    // Unlanded courtier → courtier_data.liege_char_ref.
                    if (!court_resolved) {
                        uintptr_t cd = safe_read<uintptr_t>(c + ck3::character::COURTIER_DATA);
                        if (cd && is_mapped(cd)) {
                            claimant_liege_id = safe_read<uint32_t>(cd + ck3::courtier_data::LIEGE_CHAR_REF) & 0xFFFFFF;
                            court_resolved = true;
                        }
                    }
                    in_court = (court_resolved && claimant_liege_id == atk_id);
                }
                r.key("in_attacker_court"); r.value_bool(in_court);
                if (court_resolved) { r.key("liege_id"); r.value_uint(claimant_liege_id); }
                // backable: pressed claim + claimant in attacker's court + holder
                // (defender) is a legal target (independent, not imprisoned, not
                // allied/truced with the attacker, not already at opposing war).
                bool holder_ok = !holder_present ||
                                 (holder_state.independent && !holder_state.imprisoned
                                  && !holder_in_alliance && !holder_in_truce
                                  && !holder_already_at_war);
                bool backable = pressed && in_court && holder_ok;
                r.key("backable_by_attacker"); r.value_bool(backable);
                if (!backable) {
                    const char* reason = (!pressed) ? "claim_not_pressed"
                                       : (!in_court) ? "claimant_not_in_court"
                                       : (holder_present && !holder_state.independent) ? "target_not_independent"
                                       : (holder_present && holder_state.imprisoned) ? "target_imprisoned"
                                       : (holder_present && holder_in_alliance) ? "target_in_alliance"
                                       : (holder_present && holder_in_truce) ? "target_in_truce"
                                       : "already_at_war_opposing";
                    r.key("not_backable_reason"); r.value_string(reason);
                }
            }

            r.end_object();
        }
    }

    r.end_array();
    if (claimant_count > 50) {
        r.key("truncated"); r.value_bool(true);
    }
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_title_history — chronological HOLDER history for a title.
//
// Reads the in-memory CPdxArray<CTitleHolderHistoryEntry> on CLandedTitle+0x2C0:
// who held the title and when, plus creation/grant + destruction events. Pure
// pointer-chase (no game-function call, no hook, observer-safe). The last entry's
// char always equals the title's current HOLDER_CHAR_ID. Covers HOLDER succession
// only — NOT de-jure-liege / law-change history (those are not in this array).
// (history/legibility tools, re_history_legibility_tools.md.)
//
// params: title_id (uint32, required), since_date ("Y.M.D" optional, packed-date
//         cutoff), limit (int, default 64), order ("newest"|"oldest", default newest).
// =============================================================================
bool do_query_title_history(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    if (title_id_param <= 0) {
        if (out) *out = R"({"error":"missing_title_id","hint":"Provide title_id parameter"})";
        return false;
    }
    uint32_t title_id = static_cast<uint32_t>(title_id_param) & 0xFFFFFF;
    uintptr_t tobj = lookup_title_obj(title_id);
    if (!tobj) {
        if (out) *out = R"({"error":"title_not_found"})";
        return false;
    }

    int limit = json_get_int(params_json, "limit");
    if (limit <= 0 || limit > 512) limit = 64;
    const char* order = json_get_string(params_json, "order");
    bool newest_first = !(order && strcmp(order, "oldest") == 0);

    // Optional since_date "Y.M.D" → packed (year<<16)|(month<<8)|day for cutoff.
    uint32_t since_packed = 0;
    const char* since_date = json_get_string(params_json, "since_date");
    if (since_date && since_date[0]) {
        int yy = 0, mm = 0, dd = 0;
        if (sscanf(since_date, "%d.%d.%d", &yy, &mm, &dd) >= 1) {
            since_packed = (static_cast<uint32_t>(yy) << 16) |
                           ((static_cast<uint32_t>(mm) & 0xFF) << 8) |
                           (static_cast<uint32_t>(dd) & 0xFF);
        }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_title_history");
    r.key("title_id"); r.value_uint(title_id);

    auto display_name = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
    if (!display_name.empty()) { r.key("title_name"); r.value_string(display_name.c_str(), display_name.size()); }
    uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
    if (type_ptr) {
        auto title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        if (!title_key.empty()) { r.key("title_key"); r.value_string(title_key.c_str(), title_key.size()); }
    }

    // Current holder (== last history entry's char).
    uint32_t holder_raw = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID);
    if (holder_raw != 0 && holder_raw != 0xFFFFFFFF) {
        uint32_t holder_id = holder_raw & 0xFFFFFF;
        r.key("current_holder_id"); r.value_uint(holder_id);
        void* holder = lookup_character(holder_raw);
        if (holder && !is_sentinel(holder)) {
            std::string hname = ad_read_char_name(reinterpret_cast<uintptr_t>(holder));
            if (!hname.empty()) { r.key("current_holder_name"); r.value_string(hname.c_str(), hname.size()); }
        }
    }

    uintptr_t hist_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_HOLDER_HISTORY_PTR);
    int32_t hist_count = safe_read<int32_t>(tobj + ck3::title_obj::TITLE_HOLDER_HISTORY_COUNT);
    if (hist_count < 0 || hist_count > 4096) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_title_history: count=%d out of range, clamping 0\n", hist_count);
        hist_count = 0;
    }
    r.key("count"); r.value_int(hist_count);

    r.key("entries"); r.begin_array();
    int emitted = 0;
    bool truncated = false;
    if (hist_ptr && is_mapped(hist_ptr) && hist_count > 0) {
        for (int k = 0; k < hist_count; k++) {
            // Walk newest-first or oldest-first; entries are stored ascending by date.
            int idx = newest_first ? (hist_count - 1 - k) : k;
            uintptr_t e = hist_ptr + static_cast<uintptr_t>(idx) * ck3::title_obj::THHE_STRIDE;
            if (!is_mapped(e)) break;

            uint32_t packed = safe_read<uint32_t>(e + ck3::title_obj::THHE_PACKED_DATE);
            if (since_packed && packed < since_packed) continue;  // before cutoff

            if (emitted >= limit) { truncated = true; break; }

            uint32_t etype  = safe_read<uint32_t>(e + ck3::title_obj::THHE_TYPE);
            uint32_t cref   = safe_read<uint32_t>(e + ck3::title_obj::THHE_CHAR_REF);
            uint32_t daycnt = safe_read<uint32_t>(e + ck3::title_obj::THHE_DAYCOUNT);

            int yy = static_cast<int>((packed >> 16) & 0xFFFF);
            int mm = static_cast<int>((packed >> 8) & 0xFF);
            int dd = static_cast<int>(packed & 0xFF);
            char date_str[16];
            snprintf(date_str, sizeof(date_str), "%04d.%02d.%02d", yy, mm, dd);

            const char* etype_str = (etype == 4) ? "grant" :
                                    (etype == 20) ? "succession" :
                                    (etype == 6) ? "destroyed" : "unknown";

            r.begin_object();
            r.key("date"); r.value_string(date_str);
            r.key("daycount"); r.value_uint(daycnt);
            r.key("event_type"); r.value_string(etype_str);
            r.key("event_type_raw"); r.value_uint(etype);

            uint32_t cid = cref & 0xFFFFFF;
            if (cid != 0 && cid != 0xFFFFFF) {
                r.key("char_id"); r.value_uint(cid);
                void* ch = lookup_character(cref);
                if (ch && !is_sentinel(ch)) {
                    std::string cname = ad_read_char_name(reinterpret_cast<uintptr_t>(ch));
                    if (!cname.empty()) { r.key("char_name"); r.value_string(cname.c_str(), cname.size()); }
                }
            }
            r.end_object();
            emitted++;
        }
    }
    r.end_array();
    r.key("emitted"); r.value_int(emitted);
    r.key("truncated"); r.value_bool(truncated);
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_title_succession — title-centric succession line (for PLOTTING).
//
// For an ARBITRARY title (not just the controlled char's), returns: current
// holder, the title's succession LAW (order/gender/division), the designated
// heir, and the ORDERED HEIR LINE (who inherits next on the holder's death, and
// after that — murder-chain / back-a-claimant planning). For ELECTIVE titles the
// heir array is the candidate ordering; call query_election_state(title_id) for
// live vote tallies. Reuses the per-title succession reads from do_query_succession
// (SUCCESSION_HEIRS at +0x290 etc.), generalized to any title. Pure native read.
// =============================================================================
bool do_query_title_succession(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    if (title_id_param <= 0) {
        if (out) *out = R"({"error":"missing_title_id","hint":"Provide title_id parameter"})";
        return false;
    }
    uint32_t title_id = static_cast<uint32_t>(title_id_param) & 0xFFFFFF;
    uintptr_t tobj = lookup_title_obj(title_id);
    if (!tobj) {
        if (out) *out = R"({"error":"title_not_found"})";
        return false;
    }

    auto dec_order = [](uint8_t v) -> const char* {
        switch (v) { case 0: return "inheritance"; case 1: return "election";
            case 2: return "appointment"; case 8: return "noble_family";
            case 9: return "unset"; default: return "unknown"; } };
    auto dec_gender = [](uint8_t v) -> const char* {
        switch (v) { case 0: return "male_only"; case 1: return "male_preference";
            case 2: return "equal"; case 3: return "female_preference";
            case 4: return "female_only"; case 5: return "unset"; default: return "unknown"; } };
    auto dec_div = [](uint8_t v) -> const char* {
        switch (v) { case 0: return "single_heir"; case 1: return "partition";
            case 2: return "unset"; default: return "unknown"; } };

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_title_succession");
    r.key("title_id"); r.value_uint(title_id);

    auto display_name = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
    if (!display_name.empty()) { r.key("title_name"); r.value_string(display_name.c_str(), display_name.size()); }
    uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
    int tier = 0;
    if (type_ptr) {
        auto title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        if (!title_key.empty()) { r.key("title_key"); r.value_string(title_key.c_str(), title_key.size()); }
        tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
        if (tier >= 1 && tier <= 5) { r.key("tier"); r.value_int(tier); }
    }

    uint32_t holder_raw = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID);
    if (holder_raw != 0 && holder_raw != 0xFFFFFFFF) {
        uint32_t holder_id = holder_raw & 0xFFFFFF;
        r.key("current_holder_id"); r.value_uint(holder_id);
        void* holder = lookup_character(holder_raw);
        if (holder && !is_sentinel(holder)) {
            std::string hname = ad_read_char_name(reinterpret_cast<uintptr_t>(holder));
            if (!hname.empty()) { r.key("current_holder_name"); r.value_string(hname.c_str(), hname.size()); }
        }
    }

    uint8_t order    = title_succ_rule(tobj).order;
    uint8_t gender   = title_succ_rule(tobj).gender;
    uint8_t division = title_succ_rule(tobj).division;
    r.key("succession_order"); r.value_string(dec_order(order));
    r.key("gender_law"); r.value_string(dec_gender(gender));
    r.key("title_division"); r.value_string(dec_div(division));
    bool elective = (order == 1);
    r.key("is_elective"); r.value_bool(elective);
    if (elective) {
        r.key("elective_hint");
        r.value_string("call query_election_state(title_id) for candidate vote tallies");
    }

    int32_t designated = safe_read<int32_t>(tobj + ck3::title_obj::DESIGNATED_HEIR_ID);
    if (designated != -1 && designated != 0) {
        r.key("designated_heir_id"); r.value_int(designated & 0xFFFFFF);
    }

    // Ordered heir line (cached SUCCESSION_HEIRS array). Position 1 = inherits next.
    uintptr_t heir_arr = safe_read<uintptr_t>(tobj + ck3::title_obj::SUCCESSION_HEIRS);
    int32_t heir_count = safe_read<int32_t>(tobj + ck3::title_obj::SUCCESSION_HEIR_COUNT);
    if (heir_count < 0) heir_count = 0;
    if (heir_count > 50) heir_count = 50;
    r.key("heir_count"); r.value_int(heir_count);
    r.key("heirs"); r.begin_array();
    int max_heirs = (heir_count > 16) ? 16 : heir_count;
    if (!heir_arr || heir_arr < 0x10000) max_heirs = 0;
    for (int hi = 0; hi < max_heirs; ++hi) {
        uint32_t heir_ref = safe_read<uint32_t>(heir_arr + static_cast<uintptr_t>(hi) * 4);
        uint32_t heir_cid = heir_ref & 0xFFFFFF;
        if (heir_cid == 0 || heir_cid == 0xFFFFFF) continue;
        void* heir_char = lookup_character(heir_ref);
        if (!heir_char || is_sentinel(heir_char)) continue;
        std::string heir_name = ad_read_char_name(reinterpret_cast<uintptr_t>(heir_char));
        r.begin_object();
        r.key("position"); r.value_int(hi + 1);
        r.key("character_id"); r.value_uint(heir_cid);
        if (!heir_name.empty()) { r.key("name"); r.value_string(heir_name.c_str(), heir_name.size()); }
        r.end_object();
    }
    r.end_array();

    // Convenience: who inherits NEXT (position-1 heir) hoisted to top level for plotting.
    if (max_heirs > 0 && heir_arr) {
        uint32_t h0 = safe_read<uint32_t>(heir_arr) & 0xFFFFFF;
        if (h0 != 0 && h0 != 0xFFFFFF) {
            r.key("primary_heir_id"); r.value_uint(h0);
            void* h0c = lookup_character(safe_read<uint32_t>(heir_arr));
            if (h0c && !is_sentinel(h0c)) {
                std::string n = ad_read_char_name(reinterpret_cast<uintptr_t>(h0c));
                if (!n.empty()) { r.key("primary_heir_name"); r.value_string(n.c_str(), n.size()); }
            }
        }
    }

    // #97: succession-stability companion fields (parity with do_query_succession).
    // primary_heir_id is slot-0 of the cached, re-orderable heir array; these derived
    // fields expose the stable invariant. Computed from local division + heir_count
    // already read above — no new reads, no engine calls.
    {
        bool realm_intact = (division == 0 /*single_heir*/) && (heir_count >= 1);
        r.key("succession_stable"); r.value_bool(realm_intact);
        const char* heir_conf;
        if (division == 0 && heir_count == 1) {
            heir_conf = "high";
        } else if (division == 0 && heir_count > 1) {
            heir_conf = "low";
        } else {
            heir_conf = "n/a";
        }
        r.key("primary_heir_confidence"); r.value_string(heir_conf);
        if (heir_conf[0] == 'l') {  // "low"
            r.key("heir_note"); r.value_string(
                "primary_heir_id may oscillate among equal-priority heirs; realm "
                "still passes intact under single_heir -- assert on "
                "succession_stable, not heir id");
        }
    }

    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_valid_war_targets: For a given actor, enumerate who they can attack and
// with what CB. Scans the actor's claims array and resolves title holders as
// potential targets. Also checks if actor is a vassal (potential independence war).
// =============================================================================
bool do_query_valid_war_targets(const char* params_json, std::string* out) {
    // Resolve actor
    int actor_id_param = json_get_int(params_json, "actor_id");
    void* actor = nullptr;
    if (actor_id_param > 0) {
        actor = lookup_character(static_cast<uint32_t>(actor_id_param));
    } else {
        actor = get_controlled_character();
    }
    if (!actor || is_sentinel(actor)) {
        if (out) *out = R"({"error":"actor_not_found"})";
        return false;
    }

    uintptr_t ac = reinterpret_cast<uintptr_t>(actor);
    uint32_t actor_id = char_get_id(actor) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_valid_war_targets");
    r.key("actor_id"); r.value_uint(actor_id);

    // Actor name
    std::string actor_name = ad_read_char_name(ac);
    if (!actor_name.empty()) {
        r.key("actor_name"); r.value_string(actor_name.c_str(), actor_name.size());
    }

    r.key("targets"); r.begin_array();
    int target_count = 0;
    int declarable_count = 0;

    // --- 1. Claim-based war targets ---
    // Read actor's claims from core_data+0x1E8
    uintptr_t core_data = safe_read<uintptr_t>(ac + ck3::character::REALM_DATA);  // char+0x1A8
    if (core_data) {
        uintptr_t claims_pdx = core_data + ck3::core_data::CLAIMS_ARRAY;
        uintptr_t claim_data_ptr = safe_read<uintptr_t>(claims_pdx + 0x00);
        int32_t claim_count = safe_read<int32_t>(claims_pdx + 0x0C);
        if (claim_count < 0 || claim_count > 200) claim_count = 0;

        for (int32_t i = 0; i < claim_count && target_count < 50; i++) {
            uintptr_t entry = claim_data_ptr + i * ck3::claim_entry::ENTRY_SIZE;
            uint32_t title_id_raw = safe_read<uint32_t>(entry + ck3::claim_entry::TITLE_ID);
            uint32_t title_id = title_id_raw & 0xFFFFFF;
            uint8_t pressed = safe_read<uint8_t>(entry + ck3::claim_entry::PRESSED);
            uint8_t implicit_val = safe_read<uint8_t>(entry + ck3::claim_entry::IMPLICIT);

            if (title_id == 0) continue;

            // Resolve the title to find its holder
            uintptr_t tobj = lookup_title_obj(title_id);
            if (!tobj) continue;

            uint32_t holder_raw = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID);
            if (holder_raw == 0 || holder_raw == 0xFFFFFFFF) continue;

            uint32_t holder_id = holder_raw & 0xFFFFFF;
            // Skip if actor already holds this title
            if (holder_id == actor_id) continue;

            void* holder = lookup_character(holder_raw);
            if (!holder || is_sentinel(holder)) continue;

            // Check holder is alive
            bool holder_alive = g_char_is_alive ? g_char_is_alive(holder) : true;
            if (!holder_alive) continue;

            r.begin_object();
            r.key("target_char_id"); r.value_uint(holder_id);

            std::string hname = ad_read_char_name(reinterpret_cast<uintptr_t>(holder));
            if (!hname.empty()) {
                r.key("target_name"); r.value_string(hname.c_str(), hname.size());
            }

            r.key("title_id"); r.value_uint(title_id);

            auto title_display = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
            if (!title_display.empty()) {
                r.key("title_name"); r.value_string(title_display.c_str(), title_display.size());
            }

            int tier = get_title_tier(tobj);
            if (tier >= 1 && tier <= 5) {
                r.key("tier"); r.value_int(tier);
                const char* tier_names[] = {"", "barony", "county", "duchy", "kingdom", "empire"};
                r.key("tier_name"); r.value_string(tier_names[tier]);
            }

            r.key("cb_type"); r.value_string("claim");
            r.key("pressed"); r.value_bool(pressed != 0);
            r.key("implicit"); r.value_bool(implicit_val != 0);

            // --- War-declarability honesty (read-side) ---------------------
            // Mirror the steered-write gates so the agent does not pick targets
            // the engine culls to war_count 0. A claim_cb target must have a
            // PRESSED claim (intent_dispatch.cpp:1427), be INDEPENDENT (1478),
            // and not be imprisoned (war_validity R4). Rows are KEPT (diagnostic)
            // with a declarable flag + block_reason rather than dropped.
            DefenderState ds = probe_defender_state(reinterpret_cast<uintptr_t>(holder));
            r.key("target_independent"); r.value_bool(ds.independent);
            if (!ds.independent) { r.key("target_liege_id"); r.value_uint(ds.liege_id); }
            r.key("target_imprisoned"); r.value_bool(ds.imprisoned);
            // Alliance/truce + already-at-war-opposing target-state gates
            // (mirror intent_dispatch.cpp:1565-1571 + engine Gate 0). Without
            // these a pressed claim on an independent, un-imprisoned holder who
            // is allied/truced/already-warred with the actor was reported
            // declarable=true and the engine culled the war to war_count 0.
            bool t_in_alliance = false, t_in_truce = false;
            probe_alliance_truce(actor, holder, t_in_alliance, t_in_truce);
            bool t_opp = actor_at_war_opposing(ac, holder_id);
            r.key("target_in_alliance"); r.value_bool(t_in_alliance);
            r.key("target_in_truce"); r.value_bool(t_in_truce);
            r.key("already_at_war_opposing"); r.value_bool(t_opp);
            bool declarable = (pressed != 0) && ds.independent && !ds.imprisoned
                              && !t_in_alliance && !t_in_truce && !t_opp;
            r.key("declarable"); r.value_bool(declarable);
            if (!declarable) {
                const char* reason = (pressed == 0) ? "claim_not_pressed"
                                   : (!ds.independent) ? "target_not_independent"
                                   : (ds.imprisoned) ? "target_imprisoned"
                                   : (t_in_alliance) ? "target_in_alliance"
                                   : (t_in_truce) ? "target_in_truce"
                                   : "already_at_war_opposing";
                r.key("block_reason"); r.value_string(reason);
            } else {
                declarable_count++;
            }
            r.end_object();
            target_count++;
        }
    }

    // --- 2. Independence war target (if actor is a vassal) ---
    // Resolve the actor's liege via the AUTHORITATIVE resolver
    // (get_liege_id_from_addr) instead of the raw, unreliable title_realm+0x448
    // LIEGE_PTR. The raw pointer for an INDEPENDENT actor points at a junk
    // economy/sentinel object whose char-id reads 0xFFFFFF, which the old code
    // emitted as a bogus independence row against character 16777215 — a doomed
    // war the engine culls. The resolver returns 0 for an independent actor, so
    // no row is emitted. (L22 / dispatch_router.cpp:85.)
    {
        uint32_t liege_id = get_liege_id_from_addr(ac) & 0xFFFFFF;
        if (liege_id != 0 && liege_id != 0xFFFFFF && liege_id != actor_id && target_count < 50) {
            void* liege = lookup_character(liege_id);
            if (liege && !is_sentinel(liege)) {
                uintptr_t liege_ptr = reinterpret_cast<uintptr_t>(liege);
                bool liege_alive = g_char_is_alive ? g_char_is_alive(liege) : true;
                if (liege_alive) {
                    r.begin_object();
                    r.key("target_char_id"); r.value_uint(liege_id);

                    std::string lname = ad_read_char_name(liege_ptr);
                    if (!lname.empty()) {
                        r.key("target_name"); r.value_string(lname.c_str(), lname.size());
                    }

                    r.key("title_id"); r.value_uint(0);
                    r.key("cb_type"); r.value_string("independence");
                    r.key("pressed"); r.value_bool(true);
                    r.key("implicit"); r.value_bool(false);
                    // independence_cb legitimately targets the liege (a
                    // non-independent attacker's overlord); it is NOT gated on
                    // target-independence. But the engine still culls an
                    // independence war if the liege is imprisoned, the actor
                    // holds a truce with the liege (war_validity A10 /
                    // CHasTruceTrigger), or the actor is already at war with the
                    // liege on opposing sides (Gate 0). Surface all three.
                    DefenderState lds = probe_defender_state(liege_ptr);
                    r.key("target_imprisoned"); r.value_bool(lds.imprisoned);
                    bool l_in_alliance = false, l_in_truce = false;
                    probe_alliance_truce(actor, liege, l_in_alliance, l_in_truce);
                    bool l_opp = actor_at_war_opposing(ac, liege_id);
                    r.key("target_in_truce"); r.value_bool(l_in_truce);
                    r.key("already_at_war_opposing"); r.value_bool(l_opp);
                    bool indep_declarable = !lds.imprisoned && !l_in_truce && !l_opp;
                    r.key("declarable"); r.value_bool(indep_declarable);
                    if (!indep_declarable) {
                        const char* reason = (lds.imprisoned) ? "target_imprisoned"
                                           : (l_in_truce) ? "target_in_truce"
                                           : "already_at_war_opposing";
                        r.key("block_reason"); r.value_string(reason);
                    } else {
                        declarable_count++;
                    }
                    r.end_object();
                    target_count++;
                }
            }
        }
    }

    // --- 3. Conquest + tribal_subjugation CB rows (O1, _HARDENING_PLAN FIX 2) ---
    // The claim/independence passes only surface the actor's (often-blocked)
    // claims. The win-line CBs — county_conquest_cb and tribal_subjugation_cb —
    // were previously discoverable ONLY via query_conquest_targets / per-target
    // strict-CB probing, forcing plan_war to hand-synthesise rows and emit the
    // `enumerator_omits_conquest_cb` warning. Surface them DIRECTLY here so every
    // CB class enumerates uniformly.
    //
    // Candidate source: the actor's de-jure footprint. Walk the actor's held
    // titles → de-jure duchies + their de-jure kingdoms → enumerate every de-jure
    // county → resolve external INDEPENDENT top-liege holders (the same de-jure-
    // neighbour scan do_query_borders uses; reuses get_top_realm_ruler_id /
    // get_liege_id_from_addr / lookup_title_obj — NO new offsets). For each unique
    // independent top-liege:
    //   • a county_conquest_cb row (relation-gated: independent, non-kin, not
    //     allied/truced, not already at war opposing — mirrors the #74 write gates);
    //   • a tribal_subjugation_cb row IF the top-liege government contains "tribal"
    //     (realm-scope CB, no claim/title needed; title_id:0).
    // Heuristic `declarable` only (relation gates) — the authoritative engine
    // verdict comes from plan_war's per-row query_cb_validity_strict (UNPAUSED),
    // matching how query_conquest_targets defers geography to the engine. Default
    // ON; pass include_conquest_cbs=false for the legacy claim/independence-only
    // shape (zero-regression for old callers).
    bool include_conquest_cbs = true;
    if (json_has_key(params_json, "include_conquest_cbs"))
        include_conquest_cbs = json_get_bool(params_json, "include_conquest_cbs");
    if (include_conquest_cbs) {
        uint32_t actor_house = char_house_id(ac);
        uint32_t actor_dyn   = char_dynasty_id(ac);
        uint32_t our_top     = get_top_realm_ruler_id(ac);

        // (a) Collect the actor's de-jure duchies + kingdoms from held titles.
        std::set<uint32_t> our_duchies, our_kingdoms;
        if (core_data) {
            uintptr_t tr = safe_read<uintptr_t>(ac + ck3::character::TITLE_REALM_DATA);
            if (tr && is_mapped(tr)) {
                uintptr_t ta = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
                int32_t tc = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
                if (tc < 0 || tc > 500) tc = 0;
                for (int32_t i = 0; ta && is_mapped(ta) && i < tc; i++) {
                    uint32_t tid = safe_read<uint32_t>(ta + i * 4) & 0xFFFFFF;
                    uintptr_t tobj = lookup_title_obj(tid);
                    if (!tobj) continue;
                    int tier = get_title_tier(tobj);
                    if (tier == 2) {  // county → its de-jure duchy (+ kingdom)
                        uint32_t dj = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                        if (dj) {
                            our_duchies.insert(dj);
                            uintptr_t dobj = lookup_title_obj(dj);
                            if (dobj) {
                                uint32_t kid = safe_read<uint32_t>(dobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                                if (kid) our_kingdoms.insert(kid);
                            }
                        }
                    } else if (tier == 3) {  // duchy
                        our_duchies.insert(tid);
                        uint32_t kid = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                        if (kid) our_kingdoms.insert(kid);
                    }
                }
            }
        }

        // (b) Resolve external independent top-liege holders across the footprint.
        std::set<uint32_t> seen_lieges;          // emitted top-liege ids (dedupe)
        // PERF (_enumerator_profile.md HOT LOOP #2 / item 4): the per-county
        // top-liege walk (get_top_realm_ruler_id ≈ 15-30 syscalls) is the dominant
        // cost and many de-jure counties share ONE holder/top-liege. Memoise the
        // holder→top-liege resolution so each distinct holder pays the walk once,
        // and short-circuit BEFORE the costly name/government string reads on a
        // top-liege we already decided about. seen_lieges only dedupes EMISSION;
        // these memos dedupe the WALK that precedes it.
        std::map<uint32_t, uint32_t> top_of_holder;     // holder_id -> top-liege id
        std::set<uint32_t> rejected_lieges;             // top-lieges already discarded (not our target)
        auto consider_county = [&](uint32_t county_id) {
            if (target_count >= 50) return;
            uintptr_t cobj = lookup_title_obj(county_id);
            if (!cobj || get_title_tier(cobj) != 2) return;
            uint32_t holder_raw = safe_read<uint32_t>(cobj + ck3::title_obj::HOLDER_CHAR_ID);
            if (holder_raw == 0 || holder_raw == 0xFFFFFFFF) return;
            uint32_t holder_id = holder_raw & 0xFFFFFF;
            if (holder_id == actor_id) return;
            // CHEAP top-liege memo: skip the holder lookup + 15-30-syscall liege
            // walk entirely if we've already resolved this holder's top-liege.
            uint32_t top;
            auto tit = top_of_holder.find(holder_id);
            if (tit != top_of_holder.end()) {
                top = tit->second;
            } else {
                void* holder = lookup_character(holder_raw);
                if (!holder || is_sentinel(holder)) { top_of_holder.emplace(holder_id, 0); return; }
                // Walk to the independent top-liege (the legitimate war target).
                top = get_top_realm_ruler_id(reinterpret_cast<uintptr_t>(holder));
                if (top == 0 || top == 0xFFFFFF) top = holder_id;
                top_of_holder.emplace(holder_id, top);
            }
            if (top == 0) return;
            if (top == our_top || top == actor_id) return;        // our own realm
            if (seen_lieges.count(top)) return;                   // already emitted
            if (rejected_lieges.count(top)) return;               // already discarded (cheap memo)
            void* tl = lookup_character(top);
            if (!tl || is_sentinel(tl)) { rejected_lieges.insert(top); return; }
            bool tl_alive = g_char_is_alive ? g_char_is_alive(tl) : true;
            if (!tl_alive) { rejected_lieges.insert(top); return; }
            uintptr_t tlp = reinterpret_cast<uintptr_t>(tl);
            // Must be independent (a conquest/subjugation target is a top-liege).
            uint32_t tl_liege = get_liege_id_from_addr(tlp) & 0xFFFFFF;
            bool independent = (tl_liege == 0 || tl_liege == 0xFFFFFF);
            if (!independent) { rejected_lieges.insert(top); return; }
            seen_lieges.insert(top);

            // Relation gates (shared by both CB rows).
            bool kin = is_kin_of(tlp, actor_house, actor_dyn);
            bool tl_alliance = false, tl_truce = false;
            probe_alliance_truce(actor, tl, tl_alliance, tl_truce);
            bool tl_opp = actor_at_war_opposing(ac, top);
            DefenderState tds = probe_defender_state(tlp);

            std::string tl_name = ad_read_char_name(tlp);

            // Government (for tribal_subjugation eligibility) — title_realm+
            // GOVERNMENT_TYPE_PTR → name std::string at +0x10 (same read as
            // emit_neighbor_info @ line ~2058; no new offset).
            std::string gov;
            uintptr_t tltr = safe_read<uintptr_t>(tlp + ck3::character::TITLE_REALM_DATA);
            if (tltr && is_mapped(tltr)) {
                uintptr_t gov_obj = safe_read<uintptr_t>(tltr + ck3::title_realm::GOVERNMENT_TYPE_PTR);
                if (gov_obj && is_mapped(gov_obj)) gov = ad_read_std_string(gov_obj + ck3::db_object::NAME_KEY);
            }
            bool tribal = gov.find("tribal") != std::string::npos;

            // --- county_conquest_cb row ---
            r.begin_object();
            r.key("target_char_id"); r.value_uint(top);
            if (!tl_name.empty()) { r.key("target_name"); r.value_string(tl_name.c_str(), tl_name.size()); }
            r.key("title_id"); r.value_uint(county_id);
            {
                uintptr_t ctype = safe_read<uintptr_t>(cobj + ck3::title_obj::TITLE_TYPE_PTR);
                std::string ckey = (ctype && is_mapped(ctype)) ? ad_read_std_string(ctype + ck3::db_object::NAME_KEY) : std::string();
                if (!ckey.empty()) { r.key("title_key"); r.value_string(ckey.c_str(), ckey.size()); }
            }
            r.key("tier"); r.value_int(2);
            r.key("tier_name"); r.value_string("county");
            r.key("cb_type"); r.value_string("county_conquest_cb");
            if (!gov.empty()) { r.key("target_government"); r.value_string(gov.c_str(), gov.size()); }
            r.key("target_independent"); r.value_bool(true);
            r.key("target_imprisoned"); r.value_bool(tds.imprisoned);
            r.key("is_kin"); r.value_bool(kin);
            r.key("target_in_alliance"); r.value_bool(tl_alliance);
            r.key("target_in_truce"); r.value_bool(tl_truce);
            r.key("already_at_war_opposing"); r.value_bool(tl_opp);
            {
                bool decl = !kin && !tl_alliance && !tl_truce && !tl_opp && !tds.imprisoned;
                r.key("declarable"); r.value_bool(decl);
                if (!decl) {
                    const char* reason = kin ? "target_is_kin"
                                       : tds.imprisoned ? "target_imprisoned"
                                       : tl_alliance ? "target_in_alliance"
                                       : tl_truce ? "target_in_truce"
                                       : "already_at_war_opposing";
                    r.key("block_reason"); r.value_string(reason);
                } else {
                    declarable_count++;
                }
                r.key("declarable_authoritative"); r.value_bool(false);  // engine verdict via strict-CB (UNPAUSED)
            }
            r.end_object();
            target_count++;

            // --- tribal_subjugation_cb row (independent tribal top-liege only) ---
            if (tribal && target_count < 50) {
                r.begin_object();
                r.key("target_char_id"); r.value_uint(top);
                if (!tl_name.empty()) { r.key("target_name"); r.value_string(tl_name.c_str(), tl_name.size()); }
                r.key("title_id"); r.value_uint(0);  // realm-scope CB — no title target
                r.key("cb_type"); r.value_string("tribal_subjugation_cb");
                r.key("target_government"); r.value_string(gov.c_str(), gov.size());
                r.key("target_independent"); r.value_bool(true);
                r.key("target_imprisoned"); r.value_bool(tds.imprisoned);
                r.key("is_kin"); r.value_bool(kin);
                r.key("target_in_alliance"); r.value_bool(tl_alliance);
                r.key("target_in_truce"); r.value_bool(tl_truce);
                r.key("already_at_war_opposing"); r.value_bool(tl_opp);
                {
                    bool decl = !kin && !tl_alliance && !tl_truce && !tl_opp && !tds.imprisoned;
                    r.key("declarable"); r.value_bool(decl);
                    if (!decl) {
                        const char* reason = kin ? "target_is_kin"
                                           : tds.imprisoned ? "target_imprisoned"
                                           : tl_alliance ? "target_in_alliance"
                                           : tl_truce ? "target_in_truce"
                                           : "already_at_war_opposing";
                        r.key("block_reason"); r.value_string(reason);
                    } else {
                        declarable_count++;
                    }
                    r.key("declarable_authoritative"); r.value_bool(false);
                }
                r.end_object();
                target_count++;
            }
        };

        // De-jure duchies: enumerate counties directly.
        for (uint32_t dj : our_duchies) {
            if (target_count >= 50) break;
            uintptr_t dobj = lookup_title_obj(dj);
            if (!dobj) continue;
            uintptr_t sa = safe_read<uintptr_t>(dobj + ck3::title_obj::SUB_TITLE_ARRAY);
            int32_t sc = safe_read<int32_t>(dobj + ck3::title_obj::SUB_TITLE_COUNT);
            if (sc < 0 || sc > 200) sc = 0;
            for (int32_t i = 0; sa && is_mapped(sa) && i < sc && target_count < 50; i++)
                consider_county(safe_read<uint32_t>(sa + i * 4) & 0xFFFFFF);
        }
        // De-jure kingdoms: walk their duchies' counties (skip already-done duchies).
        for (uint32_t kid : our_kingdoms) {
            if (target_count >= 50) break;
            uintptr_t kobj = lookup_title_obj(kid);
            if (!kobj) continue;
            uintptr_t ka = safe_read<uintptr_t>(kobj + ck3::title_obj::SUB_TITLE_ARRAY);
            int32_t kc = safe_read<int32_t>(kobj + ck3::title_obj::SUB_TITLE_COUNT);
            if (kc < 0 || kc > 200) kc = 0;
            for (int32_t i = 0; ka && is_mapped(ka) && i < kc && target_count < 50; i++) {
                uint32_t dj = safe_read<uint32_t>(ka + i * 4) & 0xFFFFFF;
                if (our_duchies.count(dj)) continue;
                uintptr_t dobj = lookup_title_obj(dj);
                if (!dobj) continue;
                uintptr_t sa = safe_read<uintptr_t>(dobj + ck3::title_obj::SUB_TITLE_ARRAY);
                int32_t sc = safe_read<int32_t>(dobj + ck3::title_obj::SUB_TITLE_COUNT);
                if (sc < 0 || sc > 200) sc = 0;
                for (int32_t j = 0; sa && is_mapped(sa) && j < sc && target_count < 50; j++)
                    consider_county(safe_read<uint32_t>(sa + j * 4) & 0xFFFFFF);
            }
        }
    }

    r.end_array();
    r.key("target_count"); r.value_int(target_count);
    // Number of rows that mirror the write-path gates as actionable (would not
    // be culled by the engine on pressed/independence/imprisonment). The agent
    // should prefer declarable==true rows.
    r.key("declarable_count"); r.value_int(declarable_count);
    r.key("include_conquest_cbs"); r.value_bool(include_conquest_cbs);
    // lane-query item 6: say at the TOP that this count is not the game's
    // verdict. The rows already carried declarable_authoritative:false, but a
    // reader counting "declarable" saw 5 here while the strict engine CB check
    // (query_cb_validity_strict / cbs / plan_war) returned 0 of 98 for the
    // same neighbours. The strict check is authoritative; this list is the
    // candidate set (relation gates only: kin, alliance, truce, imprisonment,
    // already at war; no CB validity, geography or cost).
    r.key("declarable_authoritative"); r.value_bool(false);
    r.key("authority"); r.value_string(
        "candidates only: declarable here means no relation gate (kin, alliance, truce, "
        "imprisonment, already at war) blocks the row. Whether the game will accept the "
        "war is decided by the strict engine CB check: cbs(target_id) or "
        "discover(what='valid_cbs_strict'), which plan_war also runs. Trust that answer "
        "when the two disagree.");
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_conquest_targets (#110) — de-jure-duchy-completion conquest targeting.
//
// The win-line problem (docs/notes/britannia-strategy-evolve.md): the Britannia
// agent has the directional-conquest LEVER (#74: steered county_conquest_cb wars
// work) but no way to FIND non-kin conquest targets — query_valid_war_targets
// only surfaces the ruler's (blocked) claims, and query_borders anonymizes
// neighbors. This query closes that gap: it enumerates CONQUERABLE counties to
// build toward a 2nd duchy, with a per-duchy completion rollup, so the agent can
// pick the duchy CLOSEST to completion and declare directly on the holder.
//
// Algorithm (bounded, PAUSED-safe pure reads + one optional game-thread engine
// check per top candidate):
//   1. Resolve ruler; gather held county title_ids and their CCountyData* ptrs.
//      Build the REACH FRONTIER = set of CCountyData* adjacent (county::NEIGHBOR
//      graph) to any held county.
//   2. Build the candidate duchy set: (a) duchies the ruler partially holds, and
//      (#117) when include_expansion: (b) duchies DE-JURE under a British kingdom
//      (k_england/k_scotland/k_wales/k_ireland) and (c) duchies with a county in the
//      reach frontier. The de-jure parent (title_obj::DE_JURE_PARENT, CORRECTED to
//      0x250 in #116) is STATIC map data — set even pre-kingdom — so the British
//      footprint enumeration works regardless of which kingdom titles are held.
//   3. For each candidate duchy's NOT-held counties: emit county_id/key, holder,
//      holder top-liege, independence, kin/allied/truced flags, de_jure_duchy, and
//      a HEURISTIC conquerable_now mirroring the #74 county_conquest gates
//      (independent + not-kin + not-allied + not-truced + not-already-at-war +
//      adjacent). Per-duchy rollup {held_by_ruler_count,total_count,remaining[]}.
//   4. Authoritative: for up to MAX_ENGINE_CHECKS best heuristic candidates, run
//      the engine cb_query_strict_full_sync and set conquerable_now from the
//      county_conquest_cb inner verdict. Bounded + short per-check timeout so a
//      wedged/paused game thread degrades to the heuristic (never hangs).
// =============================================================================

// A dynasty id is only a dynasty id if DYNASTY_DB agrees it is one (ck-vvqu).
//
// WHY THIS EXISTS. char_dynasty_id trusted any non-sentinel value it found and
// returned it as a dynasty. The bridge-verification seat measured what that
// actually yields: when the family struct populates at runtime the field holds
// a CHARACTER id, and the bridge reported it as dynasty_id. Five of the eight
// sealed E0 smoke bundles stamp a phantom into side_v2's roster_dynasties --
// the input that classifies every k_england county in the paper's primary
// outcome. Every measured phantom resolves as a LIVING CHARACTER in the dump
// and every one exceeds DYNASTY_DB's measured capacity of 16,384.
//
// THE ROUND TRIP IS THE CHECK, AND IT WAS ALREADY WRITTEN DOWN. The offsets
// header says of dynasty_obj::ID: "DB lookup validates *(obj+0x10)==id", and
// do_query_dynasty does lookup + sentinel. A LOOKUP THAT RETURNS SOMETHING IS
// NOT A LOOKUP THAT RETURNED THE THING YOU ASKED FOR -- so the id the object
// reports about itself must equal the id we asked for.
//
// AND THIS CANNOT SEE AN IN-RANGE PHANTOM THAT HAPPENS TO BE A REAL DYNASTY.
// A character id below the dynasty id space could resolve, round-trip, and be
// wrong. That is stated here rather than left for a reader to assume away: the
// measurement that found this defect had the same floor, and "incidence may be
// higher than measured" is the finding's own sentence.
static bool dynasty_id_resolves(uint32_t id) {
    if (id == 0 || id == 0xFFFFFFFF) return false;
    uintptr_t d = ad_db_lookup(ck3::globals::DYNASTY_DB, id & 0xFFFFFF);
    if (!d || !is_mapped(d)) return false;
    uintptr_t sentinel = safe_read_offset<uintptr_t>(ck3::globals::DYNASTY_DB_SENTINEL);
    if (d == sentinel) return false;
    return (safe_read<uint32_t>(d + dynasty_obj::ID) & 0xFFFFFF) == (id & 0xFFFFFF);
}

// Read a character's dynasty id (0 if unresolved). Mirrors do_query_dynasty.
// FIX I12 (#dynasty_fallback): family+0x10 is 0/0xFFFFFFFF for landed rulers, so
// derive dynasty_id from the house (char+0x150 -> HOUSE_DB -> +0x24) as a fallback.
//
// FIX ck-vvqu: THE I12 COMMENT ABOVE IS FALSE AS A GENERALISATION AND IS KEPT
// SO A READER CAN SEE WHAT WAS BELIEVED. family+0x10 is NOT uniformly sentinel
// for landed rulers -- the struct populates later, at runtime, per game state,
// which is why three episodes read right and five read wrong at the same world
// date. BOTH PATHS ARE NOW VALIDATED against DYNASTY_DB, and a value that does
// not resolve falls THROUGH rather than being returned.
//
// UNRESOLVED (0) IS THE SAFE DIRECTION AND IT IS NOT MERELY SAFER -- it is what
// the consumer already handles: side_predicate REFUSES to say `not_english` for
// a top liege whose dynasty could not be read, because that liege might belong
// to the dynasty we failed to read. A wrong-but-plausible id gets no such
// treatment, which is exactly why returning one is worse than returning none.
static uint32_t char_dynasty_id(uintptr_t char_addr) {
    if (!char_addr || !is_mapped(char_addr)) return 0;
    uintptr_t fam = safe_read<uintptr_t>(char_addr + ck3::character::FAMILY_DATA);
    if (fam && is_mapped(fam)) {
        uint32_t d = safe_read<uint32_t>(fam + ck3::family::DYNASTY_ID);
        if (dynasty_id_resolves(d)) return d & 0xFFFFFF;
    }
    uint32_t house_id = safe_read<uint32_t>(char_addr + ck3::character::HOUSE_ID_DIRECT);
    if (house_id != 0 && house_id != 0xFFFFFFFF) {
        uintptr_t hobj = ad_db_lookup(ck3::globals::HOUSE_DB, house_id & 0xFFFFFF);
        if (hobj && is_mapped(hobj)) {
            uint32_t d = safe_read<uint32_t>(hobj + dynasty_house_obj::DYNASTY_ID);
            if (dynasty_id_resolves(d)) return d & 0xFFFFFF;
        }
    }
    return 0;
}

// FIX #is_kin: read a character's house id (0 if none). char+0x150 is the reliable kin field
// (uniformly populated where family+0x10/+0x18 are 0xFFFFFFFF for landed rulers).
static uint32_t char_house_id(uintptr_t char_addr) {
    if (!char_addr || !is_mapped(char_addr)) return 0;
    uint32_t h = safe_read<uint32_t>(char_addr + ck3::character::HOUSE_ID_DIRECT);
    if (h == 0 || h == 0xFFFFFFFF) return 0;
    return h & 0xFFFFFF;
}

// House-primary kin test. House match is primary (uniformly populated); dynasty match is the
// secondary catch (cadet members whose house differs but dynasty matches).
static bool is_kin_of(uintptr_t target, uint32_t ref_house, uint32_t ref_dyn) {
    if (!target || !is_mapped(target)) return false;
    if (ref_house != 0 && char_house_id(target) == ref_house) return true;
    if (ref_dyn != 0 && char_dynasty_id(target) == ref_dyn) return true;
    return false;
}

// Returns true if `cid` (masked) appears in the PDX-array (data@+0, count@+0xC, uint32 ids)
// at family_ptr+arr_off. Bounded scan.
static bool family_array_contains(uintptr_t family_ptr, size_t arr_off, uint32_t cid) {
    if (!family_ptr || !is_mapped(family_ptr) || cid == 0) return false;
    uintptr_t data = safe_read<uintptr_t>(family_ptr + arr_off);
    int32_t cnt = safe_read<int32_t>(family_ptr + arr_off + 0x0C);
    if (!data || !is_mapped(data) || cnt <= 0 || cnt > 200) return false;
    for (int32_t i = 0; i < cnt; i++) {
        uint32_t v = safe_read<uint32_t>(data + static_cast<uintptr_t>(i) * 4) & 0xFFFFFF;
        if (v == (cid & 0xFFFFFF)) return true;
    }
    return false;
}

bool do_query_conquest_targets(const char* params_json, std::string* out) {
    // Resolve ruler (actor_id / character_id / controlled default).
    int rid = json_get_int(params_json, "character_id");
    if (rid <= 0) rid = json_get_int(params_json, "actor_id");
    void* ruler = (rid > 0) ? lookup_character(static_cast<uint32_t>(rid))
                            : get_controlled_character();
    if (!ruler || is_sentinel(ruler)) {
        if (out) *out = R"({"error":"ruler_not_found"})";
        return false;
    }
    uintptr_t rc = reinterpret_cast<uintptr_t>(ruler);
    uint32_t ruler_id = char_get_id(ruler) & 0xFFFFFF;
    uint32_t ruler_dyn = char_dynasty_id(rc);
    uint32_t ruler_house = char_house_id(rc);  // FIX #is_kin: house-primary kin test

    // Tunables (bounded scan).
    // ★ SECTION-A FIX (re_throughput_validation_unblock.md): the engine-check loop
    // is SERIAL on the IPC thread (single-flight g_cb_query slot), so each check is
    // effectively ~one tick + up-to-engine_timeout_ms, and N checks stack. The old
    // defaults (6 × 1200ms ≈ 7.2s + 1.5s pulse + lock) exceeded a typical client read
    // timeout (10s recv, but with connect/retry overhead the worst case crossed it),
    // so the client gave up before the verdict landed (the 12-25s no-response band).
    // Lower defaults to 3 × 800ms ≈ 2.4s worst case so the response ALWAYS lands well
    // under the client timeout. Un-checked candidates degrade to the heuristic
    // (conquerable_now_source="heuristic" / block_reason="not_adjacent_unverified") —
    // a documented, non-fatal degradation; the agent re-queries a specific target via
    // query_cbs_for_target for the authoritative verdict, or raises max_engine_checks
    // (clamped at 16) when it has budgeted for a longer call. This query is also
    // unpaused-deferred (is_deferred_drain_action): it MUST be issued UNPAUSED (the
    // coordinator pulses the clock to run the engine checks) and the agent should poll
    // for the response — while PAUSED the engine checks degrade straight to heuristic.
    int max_engine_checks = json_get_int(params_json, "max_engine_checks");
    if (max_engine_checks < 0) max_engine_checks = 0;
    if (max_engine_checks == 0 && !json_has_key(params_json, "max_engine_checks"))
        max_engine_checks = 3;          // default: authoritatively verify up to 3 (was 6)
    if (max_engine_checks > 16) max_engine_checks = 16;
    int engine_timeout_ms = json_get_int(params_json, "engine_timeout_ms");
    if (engine_timeout_ms <= 0) engine_timeout_ms = 800;  // was 1200 (per-check cap)
    // #117 OUTWARD EXPANSION: in addition to the duchies the ruler partially
    // holds, also surface NOT-held duchies that are (a) de-jure under a British
    // kingdom (k_england/k_scotland/k_wales/k_ireland — the e_britannia footprint)
    // OR (b) adjacent to the realm via the county reach-frontier. At King-tier the
    // ruler holds COMPLETE duchies → held_duchies_partial yields no remaining
    // counties; expansion targets are how the agent SEES the next duchy/realm to
    // de-jure-conquer toward British-Isles control. Default ON; pass
    // include_expansion=false for the legacy partial-completion-only behaviour.
    bool include_expansion = true;
    if (json_has_key(params_json, "include_expansion"))
        include_expansion = json_get_bool(params_json, "include_expansion");

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_conquest_targets");
    r.key("ruler_id"); r.value_uint(ruler_id);
    {
        std::string rn = ad_read_char_name(rc);
        if (!rn.empty()) { r.key("ruler_name"); r.value_string(rn.c_str(), rn.size()); }
    }
    r.key("ruler_dynasty_id"); r.value_uint(ruler_dyn);

    // --- Title DB handle (shared) ---
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    uintptr_t table = (db && is_mapped(db)) ? safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR) : 0;
    uint32_t capacity = (db && is_mapped(db)) ? safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT) : 0;
    if (!table || !is_mapped(table) || capacity == 0 || capacity > 0x200000) {
        r.key("error"); r.value_string("title_db_unavailable");
        r.end_object();
        if (out) *out = r.take();
        return false;
    }
    auto title_at = [&](uint32_t tid) -> uintptr_t {
        uint32_t m = tid & 0xFFFFFF;
        if (m == 0 || m >= capacity) return 0;
        uintptr_t e = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                          + static_cast<uintptr_t>(m) * ck3::title_db::ELEMENT_STRIDE);
        if (!e || !is_mapped(e)) return 0;
        if ((safe_read<uint32_t>(e + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != m) return 0;
        return e;
    };

    // --- 1. Realm held counties + reach frontier (county adjacency) ---
    //
    // #121 FIX (domain → REALM adjacency): the engine's conquest-CB neighbor gate
    // is `character_is_land_realm_neighbor = scope:defender` — a REALM-level check
    // that is satisfied if ANY province in the attacker's whole realm (the ruler
    // PLUS all vassals, recursively) borders the defender. The old code built the
    // reach frontier from the ruler's DIRECTLY-HELD (personal-domain) counties
    // only, so a king whose personal demesne (~5 counties) doesn't border a region
    // his VASSALS' counties DO border saw those targets as `not_adjacent` — and
    // (line ~10008) the authoritative engine CB check was SKIPPED for them. That is
    // the #121 over-restriction: domain-adjacency masking realm-adjacency, the
    // exact symptom of Ælfgar (personal York-area domain) being unable to SEE
    // Scotland targets his Lothian-holding realm actually borders.
    //
    // Fix: walk the whole realm (ruler + transitive vassals via DIRECT_VASSAL_ARRAY)
    // and feed EVERY realm county's neighbors into the reach frontier. Bounded:
    // a visited-ruler set + hard caps (rulers, total counties) keep it a single
    // cheap pass — same posture as the rest of this PAUSED-safe query.
    //
    // held_counties / held_duchies_partial stay PERSONAL-domain: they drive
    // partial-duchy COMPLETION (which de-jure duchies the ruler can finish with his
    // OWN holdings), which is correctly a personal-demesne notion. Only the
    // ADJACENCY frontier becomes realm-wide.
    std::set<uint32_t> held_counties;          // county title ids the ruler PERSONALLY holds
    std::set<uint32_t> held_duchies_partial;   // de-jure duchies the ruler personally touches
    std::set<uintptr_t> reach_frontier;        // CCountyData* adjacent to ANY realm county
    // PERF (bounded-candidate-set): the county title ids of EVERY realm + frontier
    // county. Their de-jure-duchy parents form the bounded duchy candidate set —
    // replacing the old unconditional O(32,768) title-DB scan (the ~11.5s 0-result
    // cost in _enumerator_profile.md). A neighbor CCountyData* maps back to its
    // county tid via county::COUNTY_TITLE_ID (+0x18, core-dump verified).
    std::set<uint32_t> reach_county_tids;      // realm + adjacent-frontier county tids
    {
        // Accumulate the neighbors of one character's held counties into the
        // reach frontier. is_ruler=true also records personal held_counties /
        // held_duchies_partial (only the controlled ruler's own holdings).
        auto add_held_counties = [&](uintptr_t cc, bool is_ruler) {
            uintptr_t tr = safe_read<uintptr_t>(cc + ck3::character::TITLE_REALM_DATA);
            if (!tr || !is_mapped(tr)) return;
            uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
            int32_t cnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
            if (cnt < 0 || cnt > 500) cnt = 0;
            if (!arr || !is_mapped(arr)) return;
            for (int32_t i = 0; i < cnt; i++) {
                uint32_t tid = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
                uintptr_t tobj = title_at(tid);
                if (!tobj) continue;
                if (get_title_tier(tobj) != 2) continue;  // counties only
                if (is_ruler) {
                    held_counties.insert(tid);
                    uint32_t dj = safe_read<uint32_t>(tobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                    if (dj) held_duchies_partial.insert(dj);
                }
                reach_county_tids.insert(tid);   // realm county → candidate-duchy source
                // Adjacency frontier from this county's CCountyData neighbors.
                uintptr_t cd = resolve_county_data(tid);
                if (cd && is_mapped(cd)) {
                    uintptr_t nbr = safe_read<uintptr_t>(cd + ck3::county::NEIGHBOR_ARRAY);
                    int32_t nc = safe_read<int32_t>(cd + ck3::county::NEIGHBOR_COUNT);
                    if (nc < 0 || nc > 100) nc = 0;
                    if (nbr && is_mapped(nbr)) {
                        for (int32_t k = 0; k < nc; k++) {
                            uintptr_t ncd = safe_read<uintptr_t>(nbr + k * 8);
                            if (ncd && is_mapped(ncd)) {
                                reach_frontier.insert(ncd);
                                // Map the neighbor CCountyData* back to its county tid
                                // (county::COUNTY_TITLE_ID +0x18) so its de-jure duchy
                                // becomes a bounded candidate without a full DB scan.
                                uint32_t nctid = safe_read<uint32_t>(ncd + ck3::county::COUNTY_TITLE_ID) & 0xFFFFFF;
                                if (nctid) reach_county_tids.insert(nctid);
                            }
                        }
                    }
                }
            }
        };

        // BFS over the realm: ruler + transitive vassals. Bounded so a deep/wide
        // realm can't blow up the scan (caps mirror the rest of this query).
        constexpr int MAX_REALM_RULERS = 2000;  // distinct landed chars walked
        std::set<uint32_t> visited_rulers;
        std::vector<uint32_t> frontier_rulers;
        visited_rulers.insert(ruler_id);
        frontier_rulers.push_back(ruler_id);
        add_held_counties(rc, /*is_ruler=*/true);

        for (size_t qi = 0; qi < frontier_rulers.size()
                            && visited_rulers.size() < MAX_REALM_RULERS; qi++) {
            uint32_t cur_id = frontier_rulers[qi];
            void* cur = (cur_id == ruler_id) ? ruler : lookup_character(cur_id);
            if (!cur || is_sentinel(cur)) continue;
            uintptr_t cur_tr = safe_read<uintptr_t>(
                reinterpret_cast<uintptr_t>(cur) + ck3::character::TITLE_REALM_DATA);
            if (!cur_tr || !is_mapped(cur_tr)) continue;
            uintptr_t vassal_arr = cur_tr + ck3::title_realm::DIRECT_VASSAL_ARRAY;
            uintptr_t vdata = safe_read<uintptr_t>(vassal_arr);
            int32_t vcnt = safe_read<int32_t>(vassal_arr + 0x0C);
            if (!vdata || !is_mapped(vdata) || vcnt <= 0 || vcnt > 500) continue;
            for (int32_t i = 0; i < vcnt; i++) {
                uint32_t vid = safe_read<uint32_t>(vdata + i * 4) & 0xFFFFFF;
                if (vid == 0 || visited_rulers.count(vid)) continue;
                visited_rulers.insert(vid);
                if (visited_rulers.size() >= MAX_REALM_RULERS) break;
                frontier_rulers.push_back(vid);
                void* vch = lookup_character(vid);
                if (vch && !is_sentinel(vch)) {
                    add_held_counties(reinterpret_cast<uintptr_t>(vch), /*is_ruler=*/false);
                }
            }
        }
    }

    // Candidate holder cache to dedupe relation/engine probing.
    struct HolderInfo {
        uint32_t id = 0;
        bool independent = true, is_kin = false, is_allied = false, is_truced = false;
        bool already_at_war = false;
        uint32_t top_liege = 0;
        bool top_liege_independent = true;
    };
    std::map<uint32_t, HolderInfo> holder_cache;
    auto probe_holder = [&](uint32_t hid) -> HolderInfo& {
        auto it = holder_cache.find(hid);
        if (it != holder_cache.end()) return it->second;
        HolderInfo hi; hi.id = hid;
        void* h = lookup_character(hid);
        if (h && !is_sentinel(h)) {
            uintptr_t ha = reinterpret_cast<uintptr_t>(h);
            DefenderState ds = probe_defender_state(ha);
            hi.independent = ds.independent;
            hi.top_liege = get_top_realm_ruler_id(ha);
            // FIX #is_kin: house-primary test (was dynasty-only → false-negative on the
            // ruler's own kin whose dynasty field is 0xFFFFFFFF but house matches).
            hi.is_kin = is_kin_of(ha, ruler_house, ruler_dyn);
            probe_alliance_truce(ruler, h, hi.is_allied, hi.is_truced);
            hi.already_at_war = actor_at_war_opposing(rc, hid);
            // Top-liege independence: a target whose top realm ruler is someone
            // else is a SUB-VASSAL — county_conquest needs an independent holder
            // (the engine kind_relation cull). top_liege == hid => independent top.
            hi.top_liege_independent = (hi.top_liege == 0 || hi.top_liege == hid);
        }
        return holder_cache.emplace(hid, hi).first->second;
    };

    // --- 2. Build candidate duchy set, then roll up each ---
    // A duchy is a CANDIDATE if any of:
    //   (a) the ruler PARTIALLY holds it (foothold-completion — the original #110
    //       flow toward a 2nd duchy);
    //   (b) [#117 expansion] it is DE-JURE under a British kingdom
    //       (k_england/k_scotland/k_wales/k_ireland) — the e_britannia footprint,
    //       so a King-tier ruler holding complete duchies still SEES the next
    //       British duchies to conquer;
    //   (c) [#117 expansion] one of its counties is ADJACENT to the realm
    //       (reach frontier) — outward neighbours regardless of kingdom.
    struct CountyTarget {
        uint32_t county_id; std::string key; std::string name;
        uint32_t holder_id; uint32_t duchy_id; std::string duchy_key;
        bool adjacent;
    };
    struct DuchyRollup {
        uint32_t duchy_id; std::string key; std::string name;
        uint32_t de_jure_kingdom_id = 0; std::string de_jure_kingdom_key;
        int held = 0, total = 0;
        std::vector<CountyTarget> remaining;  // not held by ruler
        bool adjacent_any = false;
        bool partial = false;
        bool de_jure_british = false;   // #117: de-jure under a British kingdom
        bool expansion = false;         // #117: surfaced as an outward target (not partially held)
    };
    std::vector<DuchyRollup> duchies;
    int scanned = 0, duchy_seen = 0;

    // British de-jure kingdom keys (the e_britannia footprint).
    static const char* BRITISH_KINGDOMS[] = {"k_england", "k_scotland", "k_wales", "k_ireland"};
    auto is_british_kingdom_key = [](const std::string& k) -> bool {
        for (const char* bk : BRITISH_KINGDOMS) if (k == bk) return true;
        return false;
    };

    // Candidate duchy ids → (de_jure_kingdom_id, de_jure_kingdom_key, british).
    struct CandMeta { uint32_t kingdom_id = 0; std::string kingdom_key; bool british = false; };
    std::map<uint32_t, CandMeta> candidate_duchies;

    // British de-jure kingdom title-id cache. The 4 British kingdom ids are STATIC
    // map data (never change across a session), so once discovered they are cached
    // process-wide and re-seeded on every call — guaranteeing the full e_britannia
    // footprint surfaces even for a ruler whose own realm/frontier momentarily does
    // not chain to one of them. Discovered cheaply (no DB scan) from the de-jure
    // chains of the realm/frontier counties below.
    static std::mutex s_brit_kingdom_mtx;
    static std::map<uint32_t, std::string> s_brit_kingdoms;   // kingdom tid -> british key

    // Resolve a title's de-jure kingdom key (cheap: one type-ptr + one std::string).
    auto kingdom_key = [&](uint32_t kid) -> std::string {
        uintptr_t kobj = title_at(kid);
        if (!kobj) return std::string();
        uintptr_t ktype = safe_read<uintptr_t>(kobj + ck3::title_obj::TITLE_TYPE_PTR);
        if (!ktype || !is_mapped(ktype)) return std::string();
        return ad_read_std_string(ktype + ck3::db_object::NAME_KEY);
    };

    // (a) Partially-held duchies are always candidates.
    for (uint32_t m : held_duchies_partial) candidate_duchies.emplace(m, CandMeta{});

    // (b)+(c) Expansion candidates — BOUNDED candidate set (NO full-DB scan).
    //
    // PERF FIX (_enumerator_profile.md HOT LOOP #1): the previous implementation
    // scanned all 32,768 title-DB slots (4 syscalls/slot = ~131K process_vm_readv
    // → ~11.5s even at 0 results) just to find ~1,877 duchies. Instead we derive a
    // SMALL candidate set directly:
    //   (b) British footprint: the de-jure duchies of the 4 British kingdoms
    //       (k_england/k_scotland/k_wales/k_ireland), found by following the de-jure
    //       chain (county → duchy → kingdom) of the realm/frontier counties and the
    //       cached British kingdom ids, then walking each British kingdom's
    //       SUB_TITLE_ARRAY (≈24 duchies total). The whole-footprint signal is
    //       preserved (every British duchy, near or far) — re_conquest_targets_query.md.
    //   (c) Adjacency: the de-jure duchies of the realm + reach-frontier counties
    //       (reach_county_tids), so outward neighbours surface regardless of kingdom.
    // Expected candidate count ~24-40 duchies vs 32,768 slots ⇒ ~3 orders of
    // magnitude fewer syscalls; 0-result cost drops from ~11.5s to sub-second.
    if (include_expansion) {
        // Candidate British kingdoms: union of the cached set + any British kingdom
        // reachable from the realm/frontier counties' de-jure (county→duchy→kingdom).
        std::map<uint32_t, std::string> brit_kingdoms;   // kingdom tid -> key
        {
            std::lock_guard<std::mutex> lk(s_brit_kingdom_mtx);
            brit_kingdoms = s_brit_kingdoms;   // seed from prior discovery
        }
        // British-kingdom discovery from the ruler's OWN partially-held duchies —
        // the most reliable seed for a British ruler (a held British duchy always
        // chains to its British de-jure kingdom). Ensures the full footprint
        // surfaces on the FIRST call even if no realm county reads its tid.
        for (uint32_t dj : held_duchies_partial) {
            uintptr_t dobj = title_at(dj);
            if (!dobj) continue;
            uint32_t kid = safe_read<uint32_t>(dobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
            if (kid && !brit_kingdoms.count(kid)) {
                std::string kk = kingdom_key(kid);
                if (is_british_kingdom_key(kk)) brit_kingdoms.emplace(kid, kk);
            }
        }
        // (c) Adjacency duchies from realm/frontier county tids; opportunistically
        //     discover British kingdoms along the way.
        std::set<uint32_t> adj_duchies;
        for (uint32_t cid : reach_county_tids) {
            uintptr_t cobj = title_at(cid);
            if (!cobj || get_title_tier(cobj) != 2) continue;
            uint32_t dj = safe_read<uint32_t>(cobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
            if (!dj) continue;
            adj_duchies.insert(dj);
            uintptr_t dobj = title_at(dj);
            if (!dobj) continue;
            uint32_t kid = safe_read<uint32_t>(dobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
            if (kid && !brit_kingdoms.count(kid)) {
                std::string kk = kingdom_key(kid);
                if (is_british_kingdom_key(kk)) brit_kingdoms.emplace(kid, kk);
            }
        }
        // Persist any newly-discovered British kingdoms for subsequent calls.
        if (!brit_kingdoms.empty()) {
            std::lock_guard<std::mutex> lk(s_brit_kingdom_mtx);
            for (const auto& bk : brit_kingdoms) s_brit_kingdoms.emplace(bk.first, bk.second);
        }
        // (b) British footprint: every de-jure duchy of each British kingdom.
        for (const auto& bk : brit_kingdoms) {
            uintptr_t kobj = title_at(bk.first);
            if (!kobj) continue;
            uintptr_t sub_arr = safe_read<uintptr_t>(kobj + ck3::title_obj::SUB_TITLE_ARRAY);
            int32_t sub_cnt = safe_read<int32_t>(kobj + ck3::title_obj::SUB_TITLE_COUNT);
            if (sub_cnt < 0 || sub_cnt > 200) sub_cnt = 0;
            if (!sub_arr || !is_mapped(sub_arr)) continue;
            for (int32_t i = 0; i < sub_cnt; i++) {
                uint32_t did = safe_read<uint32_t>(sub_arr + i * 4) & 0xFFFFFF;
                uintptr_t dobj = title_at(did);
                if (!dobj || get_title_tier(dobj) != 3) continue;  // duchies only
                duchy_seen++;
                CandMeta cm; cm.kingdom_id = bk.first; cm.kingdom_key = bk.second; cm.british = true;
                candidate_duchies.emplace(did, cm);  // emplace: don't clobber a partial entry
            }
        }
        // (c) Adjacency duchies — candidates if NOT already a British candidate.
        //     A duchy with any county in the reach frontier surfaces as an outward
        //     (expansion) target. Per-duchy de-jure-kingdom key resolved once.
        for (uint32_t did : adj_duchies) {
            uintptr_t dobj = title_at(did);
            if (!dobj || get_title_tier(dobj) != 3) continue;
            if (candidate_duchies.count(did)) continue;   // already a British/partial candidate
            // Confirm at least one of its counties is on the reach frontier.
            bool frontier = false;
            uintptr_t sub_arr = safe_read<uintptr_t>(dobj + ck3::title_obj::SUB_TITLE_ARRAY);
            int32_t sub_cnt = safe_read<int32_t>(dobj + ck3::title_obj::SUB_TITLE_COUNT);
            if (sub_cnt < 0 || sub_cnt > 200) sub_cnt = 0;
            if (sub_arr && is_mapped(sub_arr)) {
                for (int32_t i = 0; i < sub_cnt && !frontier; i++) {
                    uint32_t cid = safe_read<uint32_t>(sub_arr + i * 4) & 0xFFFFFF;
                    uintptr_t cobj = title_at(cid);
                    if (!cobj || get_title_tier(cobj) != 2) continue;
                    uintptr_t cd = resolve_county_data(cid);
                    if (cd && reach_frontier.count(cd)) frontier = true;
                }
            }
            if (!frontier) continue;
            duchy_seen++;
            uint32_t kid = safe_read<uint32_t>(dobj + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
            CandMeta cm; cm.kingdom_id = kid;
            if (kid) cm.kingdom_key = kingdom_key(kid);
            cm.british = is_british_kingdom_key(cm.kingdom_key);
            candidate_duchies.emplace(did, cm);
        }
    }

    // Roll up each candidate duchy (PASS 1 cheap candidacy facts, PASS 2 strings).
    for (const auto& cand : candidate_duchies) {
        uint32_t m = cand.first;
        uintptr_t dtobj = title_at(m);
        if (!dtobj || get_title_tier(dtobj) != 3) continue;   // duchies only
        scanned++;

        uintptr_t sub_arr = safe_read<uintptr_t>(dtobj + ck3::title_obj::SUB_TITLE_ARRAY);
        int32_t sub_cnt = safe_read<int32_t>(dtobj + ck3::title_obj::SUB_TITLE_COUNT);
        if (sub_cnt < 0 || sub_cnt > 200) sub_cnt = 0;
        if (!sub_arr || !is_mapped(sub_arr) || sub_cnt == 0) continue;

        // ---- PASS 1: candidacy (CHEAP — no string reads). Collect remaining
        // county ids + holder + adjacency; defer all name/key reads to PASS 2.
        bool any_adjacent = false;
        int d_total = 0, d_held = 0;
        struct RemRaw { uint32_t cid; uint32_t holder; bool adj; };
        std::vector<RemRaw> rem_raw;
        for (int32_t i = 0; i < sub_cnt; i++) {
            uint32_t cid = safe_read<uint32_t>(sub_arr + i * 4) & 0xFFFFFF;
            uintptr_t cobj = title_at(cid);
            if (!cobj || get_title_tier(cobj) != 2) continue;
            d_total++;
            uint32_t holder = safe_read<uint32_t>(cobj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
            bool ruler_holds = (holder == ruler_id) || held_counties.count(cid);
            if (ruler_holds) { d_held++; continue; }
            bool adj = false;
            uintptr_t cd = resolve_county_data(cid);
            if (cd && reach_frontier.count(cd)) adj = true;
            if (adj) any_adjacent = true;
            rem_raw.push_back({cid, holder, adj});
        }
        if (d_total == 0) continue;
        bool partial = held_duchies_partial.count(m) || d_held > 0;

        // ---- PASS 2: candidate duchy — read strings.
        DuchyRollup dr; dr.duchy_id = m;
        dr.total = d_total; dr.held = d_held;
        dr.partial = partial; dr.adjacent_any = any_adjacent;
        dr.de_jure_kingdom_id = cand.second.kingdom_id;
        dr.de_jure_kingdom_key = cand.second.kingdom_key;
        dr.de_jure_british = cand.second.british;
        dr.expansion = !partial;   // a candidate the ruler does NOT partially hold = outward target
        {
            uintptr_t dtype = safe_read<uintptr_t>(dtobj + ck3::title_obj::TITLE_TYPE_PTR);
            if (dtype && is_mapped(dtype)) dr.key = ad_read_std_string(dtype + ck3::db_object::NAME_KEY);
            dr.name = ad_read_std_string(dtobj + ck3::title_obj::TITLE_DISPLAY_NAME);
        }
        for (const auto& rr : rem_raw) {
            uintptr_t cobj = title_at(rr.cid);
            if (!cobj) continue;
            CountyTarget ct;
            ct.county_id = rr.cid;
            uintptr_t ctype = safe_read<uintptr_t>(cobj + ck3::title_obj::TITLE_TYPE_PTR);
            if (ctype && is_mapped(ctype)) ct.key = ad_read_std_string(ctype + ck3::db_object::NAME_KEY);
            ct.name = ad_read_std_string(cobj + ck3::title_obj::TITLE_DISPLAY_NAME);
            ct.holder_id = rr.holder;
            ct.duchy_id = m; ct.duchy_key = dr.key;
            ct.adjacent = rr.adj;
            dr.remaining.push_back(std::move(ct));
        }
        duchies.push_back(std::move(dr));
    }

    // --- 3. Order duchies: closest-to-completion first (most held, fewest
    //        remaining), partial before expansion targets, adjacent before far,
    //        British before non-British. The engine-check loop below consumes this
    //        order, so the most actionable targets get the authoritative checks. ---
    std::stable_sort(duchies.begin(), duchies.end(), [](const DuchyRollup& a, const DuchyRollup& b){
        if (a.partial != b.partial) return a.partial;            // partial (foothold) first
        if (a.partial) {                                         // among partials: closest to completion
            if (a.held != b.held) return a.held > b.held;        // more held first
            return a.remaining.size() < b.remaining.size();      // fewer remaining
        }
        // among expansion targets: adjacent first, then British, then more-held.
        if (a.adjacent_any != b.adjacent_any) return a.adjacent_any;
        if (a.de_jure_british != b.de_jure_british) return a.de_jure_british;
        if (a.held != b.held) return a.held > b.held;
        return a.remaining.size() < b.remaining.size();
    });

    // --- 4. Authoritative engine county_conquest_cb check on top candidates ---
    // Collect candidate (holder) ids in priority order; verify up to the cap.
    //
    // #124 FIX (land-only sea-blind adjacency → engine-authoritative): the bridge's
    // `adjacent` flag comes from the county `NEIGHBOR_ARRAY` (CCountyData neighbors),
    // which is sea-INCLUSIVE only for SHORT STRAITS (core-dump: c_galloway lists
    // c_ulster + c_isle_of_man across the North Channel) but does NOT model the wider
    // Viking longboat reach. CK3's ACTUAL conquest-CB neighbor rule
    // (`county_conquest_cb.valid_to_start` → `neighboring_county_or_viking_conquest_trigger`
    // in the game files) is, for a viking-invasion-capable attacker
    // (`can_use_viking_invasion_cbs_trigger`: culture has innovation_longboats, faith
    // has tenet_warmonger, realm has a coastal county, etc.), satisfied by
    // `is_coastal_county = yes` + realm sea-CONNECTIVITY — far wider than the
    // short-strait neighbor graph. So a coastal Irish/Welsh county that the engine
    // WOULD grant a Viking ruler was being HIDDEN here as `not_adjacent` and never
    // engine-checked. That was the #124 over-restriction.
    //
    // Fix: the engine `CB_VALIDITY_CHECK` (which evaluates that very trigger,
    // including coastal + viking sea-connectivity) is the AUTHORITY — exactly the
    // legitimacy posture of the steered WRITE path (re_conquest_adjacency_domain_vs_realm_121.md).
    // So we gate engine verification on the cheap RELATION pre-filter ONLY
    // (independent top-liege, non-kin, non-allied, non-truced, not already at war —
    // all genuine engine rejections regardless of geography) and let the ENGINE
    // decide adjacency/sea-reach. We still PRIORITISE short-strait-adjacent targets
    // (pass 1) before non-adjacent coastal/expansion ones (pass 2) so the bounded
    // engine-check budget is spent on the most likely candidates first.
    std::map<uint32_t, int> engine_verdict;   // holder_id -> 1 pass / 0 fail / -1 unrun
    int engine_checks_done = 0;
    // RELATION gate only — NO adjacency (the engine owns the geography rule, #124).
    auto relation_ok = [&](const HolderInfo& hi) -> bool {
        return hi.independent && hi.top_liege_independent && !hi.is_kin
               && !hi.is_allied && !hi.is_truced && !hi.already_at_war;
    };
    // Legacy heuristic (relation + short-strait adjacency) — the conservative
    // fallback when the engine check did NOT run (cap exhausted / paused thread).
    auto heuristic_now = [&](const HolderInfo& hi, bool adjacent) -> bool {
        return adjacent && relation_ok(hi);
    };
    auto run_engine_check = [&](uint32_t holder_id) {
        if (engine_verdict.count(holder_id)) return;
        CBStrictSummary summary{};
        std::vector<CBQueryResult> results;
        char err[96] = {};
        int rcq = cb_query_strict_full_sync(ruler_id, holder_id,
                                            engine_timeout_ms, &summary, &results,
                                            err, sizeof(err));
        engine_checks_done++;
        if (rcq < 0) { engine_verdict[holder_id] = -1; return; }
        int verdict = 0;
        for (const auto& cb : results) {
            if (strcmp(cb.cb_name, "county_conquest_cb") == 0) {
                verdict = (strcmp(cb.inner_status, "pass") == 0) ? 1 : 0;
                break;
            }
        }
        engine_verdict[holder_id] = verdict;
    };
    // Pass 1: short-strait-adjacent, relation-OK candidates (most likely to pass).
    // Pass 2: non-adjacent but relation-OK candidates (the #124 cross-sea targets) —
    // the engine decides whether the Viking coastal rule grants them. Both passes
    // share the single bounded budget; duchy ordering already front-loads the most
    // actionable duchies.
    for (int pass = 0; pass < 2 && engine_checks_done < max_engine_checks; pass++) {
        bool want_adjacent = (pass == 0);
        for (const auto& dr : duchies) {
            if (engine_checks_done >= max_engine_checks) break;
            for (const auto& ct : dr.remaining) {
                if (engine_checks_done >= max_engine_checks) break;
                if (ct.holder_id == 0 || ct.holder_id == ruler_id) continue;
                if (ct.adjacent != want_adjacent) continue;
                if (engine_verdict.count(ct.holder_id)) continue;
                HolderInfo& hi = probe_holder(ct.holder_id);
                if (!relation_ok(hi)) continue;
                run_engine_check(ct.holder_id);
            }
        }
    }

    // --- 5. Emit ---
    int total_targets = 0, conquerable_count = 0, expansion_count = 0, british_count = 0;
    r.key("duchies"); r.begin_array();
    for (const auto& dr : duchies) {
        if (dr.expansion) expansion_count++;
        if (dr.de_jure_british) british_count++;
        r.begin_object();
        r.key("duchy_id"); r.value_uint(dr.duchy_id);
        if (!dr.key.empty())  { r.key("duchy_key");  r.value_string(dr.key.c_str(), dr.key.size()); }
        if (!dr.name.empty()) { r.key("duchy_name"); r.value_string(dr.name.c_str(), dr.name.size()); }
        // #117: de-jure kingdom (k_england/k_scotland/k_wales/k_ireland for the
        // British footprint) so the agent can group targets by the kingdom it is
        // de-jure-conquering toward e_britannia.
        if (dr.de_jure_kingdom_id) {
            r.key("de_jure_kingdom_id"); r.value_uint(dr.de_jure_kingdom_id);
            if (!dr.de_jure_kingdom_key.empty()) {
                r.key("de_jure_kingdom_key"); r.value_string(dr.de_jure_kingdom_key.c_str(), dr.de_jure_kingdom_key.size());
            }
        }
        r.key("de_jure_british"); r.value_bool(dr.de_jure_british);
        r.key("expansion_target"); r.value_bool(dr.expansion);  // outward (not partially held)
        r.key("held_by_ruler_count"); r.value_int(dr.held);
        r.key("total_count");         r.value_int(dr.total);
        r.key("remaining_count");     r.value_int(static_cast<int>(dr.remaining.size()));
        r.key("partially_held");      r.value_bool(dr.partial);
        r.key("adjacent");            r.value_bool(dr.adjacent_any);

        r.key("remaining_counties"); r.begin_array();
        for (const auto& ct : dr.remaining) {
            total_targets++;
            HolderInfo& hi = probe_holder(ct.holder_id);
            r.begin_object();
            r.key("county_id"); r.value_uint(ct.county_id);
            if (!ct.key.empty())  { r.key("county_key");  r.value_string(ct.key.c_str(), ct.key.size()); }
            if (!ct.name.empty()) { r.key("county_name"); r.value_string(ct.name.c_str(), ct.name.size()); }
            r.key("holder_id"); r.value_uint(ct.holder_id);
            if (ct.holder_id) {
                void* h = lookup_character(ct.holder_id);
                if (h && !is_sentinel(h)) {
                    std::string hn = ad_read_char_name(reinterpret_cast<uintptr_t>(h));
                    if (!hn.empty()) { r.key("holder_name"); r.value_string(hn.c_str(), hn.size()); }
                }
            }
            r.key("holder_top_liege_id");          r.value_uint(hi.top_liege);
            r.key("holder_top_liege_independent"); r.value_bool(hi.top_liege_independent);
            r.key("is_independent"); r.value_bool(hi.independent);
            r.key("is_kin");         r.value_bool(hi.is_kin);
            r.key("is_allied");      r.value_bool(hi.is_allied);
            r.key("is_truced");      r.value_bool(hi.is_truced);
            r.key("already_at_war_opposing"); r.value_bool(hi.already_at_war);
            r.key("adjacent"); r.value_bool(ct.adjacent);

            bool heur = heuristic_now(hi, ct.adjacent);
            bool conquerable_now = heur;
            const char* src = "heuristic";
            bool engine_ran = false;
            auto ev = engine_verdict.find(ct.holder_id);
            if (ev != engine_verdict.end() && ev->second >= 0) {
                conquerable_now = (ev->second == 1);
                src = "engine_county_conquest_cb";
                engine_ran = true;
            }
            r.key("conquerable_now"); r.value_bool(conquerable_now);
            r.key("conquerable_now_source"); r.value_string(src);
            if (conquerable_now) conquerable_count++;
            if (!conquerable_now) {
                // #124: relation gates first (definitive engine rejections), then the
                // ENGINE verdict (authoritative for geography incl. coastal/viking
                // sea-reach), then the heuristic-only signals. A non-adjacent target
                // the engine REJECTED is `engine_cb_invalid` (honest), not the blanket
                // `not_adjacent`. A non-adjacent target we could NOT engine-check
                // (cap/paused) is `not_adjacent_unverified` — the agent can raise
                // max_engine_checks or unpause to get the authoritative verdict
                // instead of treating short-strait non-adjacency as a hard block.
                const char* reason =
                    (!hi.independent)              ? "holder_not_independent" :
                    (!hi.top_liege_independent)    ? "holder_is_subvassal" :
                    (hi.is_kin)                    ? "holder_is_kin" :
                    (hi.is_allied)                 ? "holder_allied" :
                    (hi.is_truced)                 ? "holder_truced" :
                    (hi.already_at_war)            ? "already_at_war_opposing" :
                    (engine_ran)                   ? "engine_cb_invalid" :
                    (!ct.adjacent)                 ? "not_adjacent_unverified" : "blocked";
                r.key("block_reason"); r.value_string(reason);
            } else {
                // The actionable lever for the agent (mirrors #74 / embodiment doc).
                // Legacy single `declare` = the county_conquest option (back-compat
                // with existing harness scenarios / consumers).
                r.key("declare"); r.begin_object();
                r.key("type"); r.value_string("war");
                r.key("method"); r.value_string("steered");
                r.key("casus_belli"); r.value_string("county_conquest_cb");
                r.key("target_id"); r.value_uint(ct.holder_id);
                r.key("cb_title_id"); r.value_uint(ct.county_id);
                r.end_object();

                // declare_options[] (re_warplanner_enumeration.md): each carries the
                // EXPLICIT cb_title_id so do(declare_war, declare_args=opt) is
                // declare-FIRST-TRY. county_conquest takes ONE county (ct.county_id);
                // duchy_conquest takes the WHOLE de-jure duchy (dr.duchy_id) and is
                // offered when the duchy is at least partially held / a foothold so
                // the agent can choose "one county" vs "the whole duchy".
                r.key("declare_options"); r.begin_array();
                r.begin_object();
                r.key("casus_belli"); r.value_string("county_conquest_cb");
                r.key("scope"); r.value_string("county");
                r.key("target_id"); r.value_uint(ct.holder_id);
                r.key("cb_title_id"); r.value_uint(ct.county_id);
                r.end_object();
                if (dr.duchy_id) {
                    r.begin_object();
                    r.key("casus_belli"); r.value_string("duchy_conquest_cb");
                    r.key("scope"); r.value_string("duchy");
                    r.key("target_id"); r.value_uint(ct.holder_id);
                    r.key("cb_title_id"); r.value_uint(dr.duchy_id);
                    r.key("note"); r.value_string(
                        "takes the whole de-jure duchy; pre-validate via query_cbs_for_target "
                        "(duchy_conquest has stricter gates than county_conquest)");
                    r.end_object();
                }
                r.end_array();
            }
            r.end_object();
        }
        r.end_array();
        r.end_object();
    }
    r.end_array();

    r.key("duchy_count");        r.value_int(static_cast<int>(duchies.size()));
    r.key("expansion_count");    r.value_int(expansion_count);   // #117 outward (not partially held)
    r.key("british_count");      r.value_int(british_count);     // #117 de-jure under a British kingdom
    r.key("include_expansion");  r.value_bool(include_expansion);
    r.key("target_count");       r.value_int(total_targets);
    r.key("conquerable_count");  r.value_int(conquerable_count);
    r.key("engine_checks_done"); r.value_int(engine_checks_done);
    r.key("max_engine_checks");  r.value_int(max_engine_checks);
    // ★ SECTION-A: truncation flag. When engine_checks_done hit the cap there may be
    // relation-OK candidates whose conquerable_now is HEURISTIC-only (unverified). The
    // agent can raise max_engine_checks or re-query a specific target via
    // query_cbs_for_target for the authoritative verdict on those.
    r.key("engine_checks_truncated"); r.value_bool(engine_checks_done >= max_engine_checks);
    r.key("duchies_scanned");    r.value_int(duchy_seen);
    r.key("note"); r.value_string(
        "Conquest targeting toward the de-jure ladder (2nd duchy -> kingdom -> e_britannia). "
        "Duchies are ordered: partially-held (foothold-completion) first, then OUTWARD "
        "EXPANSION targets (expansion_target=true) — duchies de-jure under a British kingdom "
        "(de_jure_british=true, de_jure_kingdom_key=k_england/k_scotland/k_wales/k_ireland) or "
        "adjacent to the realm — so a King-tier ruler holding complete duchies still SEES the "
        "next British duchies to conquer. Pick a duchy (foothold first; else an adjacent British "
        "one), then declare on a remaining county whose conquerable_now=true via the embedded "
        "`declare` block (act type=war method=steered casus_belli=county_conquest_cb). "
        "conquerable_now is engine-verified (source=engine_county_conquest_cb) for top candidates "
        "and heuristic otherwise; heuristic mirrors the #74 gates (independent, non-kin, "
        "non-allied, non-truced, not-at-war, adjacent). holder_top_liege_id is the INDEPENDENT "
        "top-liege (war the right target). #124: the `adjacent` flag is the engine county-neighbor "
        "graph (sea-inclusive for SHORT STRAITS only — Galloway<->Ulster — NOT the full Viking "
        "longboat reach), so it is a PRIORITY signal, not a hard block. The ENGINE check now runs "
        "for relation-OK targets even when adjacent=false (the engine evaluates the real "
        "`neighboring_county_or_viking_conquest_trigger`: coastal + viking sea-connectivity), so a "
        "coastal Irish/Welsh county a Viking ruler CAN conquer surfaces as conquerable_now=true even "
        "though it is not a short-strait neighbor. block_reason=engine_cb_invalid means the ENGINE "
        "refused (e.g. inland/non-coastal/wrong-government); block_reason=not_adjacent_unverified "
        "means the bounded engine-check budget didn't reach it (raise max_engine_checks or unpause "
        "for the authoritative verdict; engine_checks_truncated=true flags that the cap was hit). "
        "Engine checks need an UNPAUSED game thread (this query is deferred-drain: issue it "
        "UNPAUSED and poll for the response) — while paused they degrade to the heuristic. "
        "Defaults are bounded to max_engine_checks=3 x engine_timeout_ms=800ms (~2.4s worst case) "
        "to land under the client read timeout; raise max_engine_checks (<=16) for fuller "
        "verification at the cost of a longer call. Pass include_expansion=false for legacy "
        "partial-completion-only. See re_conquest_targets_query.md / "
        "re_directional_conquest_enabler.md / re_conquest_sea_adjacency_124.md.");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_cb_options — For a given actor+target pair, enumerate which CB options
// are available for declare_war.  Uses BUILD_SEND_INTERACTION_CMD to create a
// temporary command buffer, sets war_target/claimant, then calls PopulateOptions
// to let the engine evaluate trigger blocks.  Read-only — does NOT dispatch.
//
// Diagnostic-only — NOT exposed via the MCP `discover`/`query` surface. Agent
// callers should use `discover(what="valid_cbs_strict")`, which evaluates the
// same Stage 1+2 outer filter plus the engine's inner CB_VALIDITY_CHECK without
// the BUILD_SEND/PopulateOptions reentry cost. This handler stays for
// diagnostic cross-validation against the engine's own option population path
// (re/findings/re_test_suite_audit.md §3; ck-xoj6x).
// =============================================================================

// CB flag_key hash → human-readable name lookup table
static const char* hash_to_cb_name(uint32_t hash) {
    switch (hash) {
        case 0xa85d2f59: return "independence_cb";
        case 0x03b2d89a: return "claim_cb";
        case 0xd57f651d: return "de_jure_county_cb";
        case 0xd0a0d46c: return "holy_war_cb";
        case 0x04f79547: return "conquest_cb";
        case 0xb4d80728: return "county_conquest_cb";
        case 0xc2b4785e: return "duchy_conquest_cb";
        case 0x53f14471: return "subjugation_cb";
        case 0xcfe68e73: return "forced_vassalization_cb";
        case 0xb2ba4026: return "independence_faction_war";
        case 0x2709d66e: return "claimant_faction_war";
        case 0x0fcd9ec4: return "liberty_faction_war";
        default: return nullptr;
    }
}

// Function pointer type for BUILD_SEND_INTERACTION_CMD
using fn_build_send_interaction_cmd_q = void(*)(void* cmd_buf, void* interaction,
                                                 uint32_t actor_id, uint32_t target_id,
                                                 int zero, int one);
// Function pointer type for PopulateOptions / CheckShouldPopulate
using fn_populate_options = void(*)(void* cmd_buf);
using fn_check_populate   = bool(*)(void* cmd_buf);

bool do_query_cb_options(const char* params_json, std::string* out) {
    // --- Parse parameters ---
    int actor_id_param = json_get_int(params_json, "actor_id");
    if (actor_id_param <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id_param = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id_param <= 0) {
        if (out) *out = R"({"error":"no_actor_available"})";
        return false;
    }

    int target_id_param = json_get_int(params_json, "target_id");
    if (target_id_param <= 0) {
        if (out) *out = R"({"error":"missing_target_id","hint":"Provide target_id - defender character ID"})";
        return false;
    }

    int claimant_id_param = json_get_int(params_json, "claimant_id");
    uint32_t actor_id   = static_cast<uint32_t>(actor_id_param) & 0xFFFFFF;
    uint32_t target_id  = static_cast<uint32_t>(target_id_param) & 0xFFFFFF;
    uint32_t claimant_id = (claimant_id_param > 0)
                           ? (static_cast<uint32_t>(claimant_id_param) & 0xFFFFFF)
                           : actor_id;

    // --- Resolve interaction (defaults to declare_war_interaction) ---
    const char* itype = json_get_string(params_json, "interaction_type");
    if (!itype || !itype[0]) itype = "declare_war_interaction";
    std::string itype_str(itype);  // copy since json_get_string uses thread-local
    void* interaction = lookup_interaction(itype_str.c_str());
    if (!interaction) {
        if (out) {
            std::string err = R"({"error":"interaction_not_found","interaction_type":")" + itype_str + R"("})";
            *out = err;
        }
        return false;
    }

    uintptr_t idef = reinterpret_cast<uintptr_t>(interaction);

    // --- Read option array from interaction definition ---
    uintptr_t option_array_ptr = safe_read<uintptr_t>(idef + ck3::interaction_def::OPTION_ARRAY_PTR);
    int32_t option_count = safe_read<int32_t>(idef + ck3::interaction_def::OPTION_COUNT);

    ADLOG("query_cb_options: idef=0x%lx option_array=0x%lx option_count=%d\n",
          idef, option_array_ptr, option_count);

    if (!option_array_ptr || !is_mapped(option_array_ptr) ||
        option_count <= 0 || option_count > 100) {
        if (out) {
            char buf[256];
            // rc2 B2: no option_array_ptr (a process VA) in the reply.
            snprintf(buf, sizeof(buf),
                     R"({"error":"no_cb_options","option_count":%d})", option_count);
            *out = buf;
        }
        return false;
    }

    // --- Resolve function pointers ---
    auto build_interaction_cmd = reinterpret_cast<fn_build_send_interaction_cmd_q>(
        bridge::game_fn(ck3::funcs::BUILD_SEND_INTERACTION_CMD));
    auto check_pop = reinterpret_cast<fn_check_populate>(
        bridge::game_fn(ck3::funcs::INTERACTION_CHECK_POPULATE));
    auto populate = reinterpret_cast<fn_populate_options>(
        bridge::game_fn(ck3::funcs::INTERACTION_POPULATE_OPTIONS));

    if (!is_mapped(reinterpret_cast<uintptr_t>(build_interaction_cmd)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(check_pop)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(populate))) {
        if (out) *out = R"({"error":"function_not_mapped","hint":"BUILD_SEND/CheckPop/Populate not mapped"})";
        return false;
    }

    // --- Build temporary cmd_buf ---
    alignas(16) uint8_t cmd_buf[ck3::interaction_cmd::CMD_BUF_SIZE] = {};

    // SIGSEGV protection for BUILD_SEND_INTERACTION_CMD
    bool build_ok = false;
    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        ADLOG("query_cb_options: BUILD_SEND_INTERACTION_CMD crashed (%s)\n", sig_name);
        if (out) {
            char buf[128];
            snprintf(buf, sizeof(buf), R"({"error":"build_cmd_crash","signal":"%s"})", sig_name);
            *out = buf;
        }
        return false;
    }

    try {
        build_interaction_cmd(cmd_buf, interaction, actor_id, target_id, 0, 1);
        build_ok = true;
    } catch (...) {
        ADLOG("query_cb_options: BUILD_SEND_INTERACTION_CMD threw exception\n");
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!build_ok) {
        if (out) *out = R"({"error":"build_cmd_exception"})";
        return false;
    }

    // --- Set war_target and claimant ---
    *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::WAR_TARGET_REF) = target_id;
    *reinterpret_cast<uint32_t*>(cmd_buf + ck3::interaction_ctx::CLAIMANT_REF) = claimant_id;

    ADLOG("query_cb_options: cmd_buf built, war_target=%u claimant=%u\n",
          target_id, claimant_id);

    // --- Call CheckShouldPopulate + PopulateOptions (SIGSEGV protected) ---
    volatile bool populated = false;
    volatile bool check_result = false;

    watchdog_arm(5);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN";
        ADLOG("query_cb_options: PopulateOptions crashed (%s)\n", sig_name);
        // Continue without population — we still have the raw option entries
    } else {
        try {
            check_result = check_pop(cmd_buf);
            ADLOG("query_cb_options: CheckShouldPopulate returned %s\n",
                  check_result ? "true" : "false");
            if (check_result) {
                populate(cmd_buf);
                populated = true;
                ADLOG("query_cb_options: PopulateOptions completed\n");
            }
        } catch (...) {
            ADLOG("query_cb_options: PopulateOptions threw exception\n");
        }
        g_in_protected_region = false;
        watchdog_disarm();
    }

    // --- Read selection state from cmd_buf ---
    uintptr_t selections_ptr = safe_read<uintptr_t>(
        reinterpret_cast<uintptr_t>(cmd_buf) + ck3::interaction_ctx::EVAL_FLAGS_2);
    int32_t sel_count = safe_read<int32_t>(
        reinterpret_cast<uintptr_t>(cmd_buf) + ck3::interaction_ctx::EVAL_SLOT_COUNT);

    ADLOG("query_cb_options: selections_ptr=0x%lx sel_count=%d populated=%d\n",
          selections_ptr, sel_count, populated);

    // --- Build response ---
    int32_t emit_count = (option_count > 50) ? 50 : option_count;

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_cb_options");
    r.key("actor_id"); r.value_uint(actor_id);
    r.key("target_id"); r.value_uint(target_id);
    r.key("claimant_id"); r.value_uint(claimant_id);
    r.key("option_count"); r.value_int(option_count);
    r.key("populated"); r.value_bool(populated);
    r.key("check_should_populate"); r.value_bool(check_result);

    r.key("options"); r.begin_array();
    int valid_count = 0;

    for (int32_t i = 0; i < emit_count; i++) {
        uintptr_t opt = option_array_ptr + (static_cast<uintptr_t>(i) * 0x7D0);
        if (!is_mapped_range(opt, 0x3B0)) continue;

        uint32_t flag_key = safe_read<uint32_t>(opt + ck3::interaction_def::OPT_FLAG_KEY_HASH);

        r.begin_object();
        r.key("index"); r.value_int(i);

        // Hex string of flag_key
        char hex_buf[16];
        snprintf(hex_buf, sizeof(hex_buf), "0x%08x", flag_key);
        r.key("flag_key_hash"); r.value_string(hex_buf);

        // Human-readable CB name
        const char* name = hash_to_cb_name(flag_key);
        if (name) {
            r.key("cb_name"); r.value_string(name);
        } else {
            r.key("cb_name"); r.value_string(hex_buf);
        }

        // Try to read the CB type object pointer from option+0x00
        // (the first field may point to a CCasusBelliType or contain type info)
        uintptr_t cb_type_ptr = safe_read<uintptr_t>(opt + 0x00);
        if (cb_type_ptr && is_mapped(cb_type_ptr)) {
            // Try reading the CB name from the type object
            const char* cb_obj_name = nullptr;
            size_t cb_obj_name_len = 0;
            if (read_db_obj_name(cb_type_ptr, cb_obj_name, cb_obj_name_len)) {
                r.key("cb_type_name"); r.value_string(cb_obj_name, cb_obj_name_len);
            }
        }

        // Selection state (if PopulateOptions succeeded)
        if (populated && selections_ptr && is_mapped(selections_ptr) && i < sel_count) {
            uint8_t sel_byte = safe_read<uint8_t>(selections_ptr + i);
            r.key("selected"); r.value_bool(sel_byte != 0);
            if (sel_byte != 0) valid_count++;
        }

        r.end_object();
    }

    r.end_array();
    if (option_count > 50) {
        r.key("truncated"); r.value_bool(true);
    }
    r.key("valid_count"); r.value_int(valid_count);
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_pending_events — enumerate pending player events from the event queue
//
// Detects when the game is paused due to an event popup waiting for player
// input.  Returns the event queue contents for the controlled character,
// including event_id (key), instance_id, option_count, narrative flag, and
// the event-pause count from game state.
//
// The agent uses this to:
//   1. Detect "paused because of event popup" vs normal pause
//   2. Read event details for informed option selection
//   3. Respond via the existing respond_event action
// =============================================================================

bool do_query_pending_events(const char* /*params_json*/, std::string* out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_pending_events");

    // Read event-pause and date-pause counts from game state
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    int32_t event_pause_count = 0;
    int32_t date_pause_count = 0;
    if (gs) {
        event_pause_count = safe_read<int32_t>(gs + ck3::game_state::EVENT_PAUSE_COUNT);
        date_pause_count = safe_read<int32_t>(gs + ck3::game_state::DATE_PAUSE_COUNT);
    }
    r.key("event_pause_count"); r.value_int(event_pause_count);
    r.key("date_pause_count"); r.value_int(date_pause_count);
    r.key("paused"); r.value_bool(is_paused());

    // Determine the player character ID
    uint32_t player_id = g_controlled_primary.load(std::memory_order_relaxed);
    if (player_id == 0) {
        player_id = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
    }
    player_id &= 0xFFFFFF;
    r.key("player_char_id"); r.value_uint(player_id);

    // Access the event queue
    uintptr_t queue = 0;
    if (gs) {
        uintptr_t subsys = safe_read<uintptr_t>(gs + ck3::event_sys::GAME_STATE_SUBSYS_PTR);
        if (subsys && (subsys && is_mapped(subsys))) {
            queue = subsys + ck3::event_sys::EVENT_QUEUE_OFFSET;
        }
    }

    // Read raw queue count (total events, not just for player)
    int32_t raw_queue_count = 0;
    uintptr_t event_list_ptr = 0;
    if (queue) {
        raw_queue_count = safe_read<int32_t>(queue + ck3::event_sys::EVENT_LIST_COUNT);
        event_list_ptr = safe_read<uintptr_t>(queue + ck3::event_sys::EVENT_LIST_PTR);
    }
    r.key("raw_queue_count"); r.value_int(raw_queue_count);

    // Diagnostic: dump queue address and nearby memory for RE validation
    {
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
        char diag[256];
        uintptr_t subsys_val = gs ? safe_read<uintptr_t>(gs + ck3::event_sys::GAME_STATE_SUBSYS_PTR) : 0;
        snprintf(diag, sizeof(diag), "gs=%p subsys=%p queue=%p list=%p count_addr=%p",
                 (void*)gs, (void*)subsys_val, (void*)queue,
                 (void*)event_list_ptr, queue ? (void*)(queue + ck3::event_sys::EVENT_LIST_COUNT) : nullptr);
        r.key("_diag_addrs"); r.value_string(diag);
#endif
        // Also read next_id counter to verify we're at the right spot
        if (queue) {
            uint32_t next_id = safe_read<uint32_t>(queue + ck3::event_sys::EVENT_QUEUE_NEXT_ID);
            r.key("_diag_next_event_id"); r.value_uint(next_id);
        }
    }

    // Enumerate pending events for this player
    r.key("events"); r.begin_array();
    int event_count = 0;

    if (queue && player_id > 0 && raw_queue_count > 0) {
        uintptr_t event_list = safe_read<uintptr_t>(queue + ck3::event_sys::EVENT_LIST_PTR);
        if (event_list && (event_list && is_mapped(event_list))) {
            int32_t count = raw_queue_count;
            if (count > 32) count = 32;  // hard cap

            for (int i = 0; i < count; i++) {
                uintptr_t entry_addr = event_list + i * 8;
                if ((!entry_addr || !is_mapped(entry_addr))) break;
                uintptr_t sp_event = safe_read<uintptr_t>(entry_addr);
                if (!sp_event || (!sp_event || !is_mapped(sp_event))) continue;

                // Read character ID from SPlayerEventData
                uint32_t char_id = safe_read<uint32_t>(sp_event + ck3::event_sys::SPEVENT_CHAR_ID);
                if ((char_id & 0xFFFFFF) != player_id) continue;

                // Get CEvent* from SPlayerEventData
                uintptr_t event_def = safe_read<uintptr_t>(sp_event + ck3::event_sys::SPEVENT_EVENT_DEF);
                if (!event_def || (!event_def || !is_mapped(event_def))) continue;

                // Read event state from CEvent
                int32_t state = safe_read<int32_t>(event_def + ck3::event_sys::CEVENT_STATE);

                // Read instance ID
                uint32_t instance_id = safe_read<uint32_t>(sp_event + ck3::event_sys::SPEVENT_INSTANCE_ID);

                // Read option count from CEvent
                int32_t option_count = safe_read<int32_t>(event_def + ck3::event_sys::CEVENT_OPTION_COUNT);

                // Read narrative flag from CEvent
                uint8_t narrative = safe_read<uint8_t>(event_def + ck3::event_sys::CEVENT_NARRATIVE_FLAG);

                // Read event key from CEvent (db_object NAME_KEY at +0x10)
                std::string event_key = ad_read_std_string(event_def + ck3::db_object::NAME_KEY);

                r.begin_object();
                r.key("instance_id"); r.value_uint(instance_id);
                r.key("event_id"); r.value_string(event_key.empty() ? "unknown" : event_key.c_str());
                r.key("state"); r.value_int(state);
                r.key("state_label");
                if (state == 1) r.value_string("player");
                else if (state == 2) r.value_string("pending");
                else r.value_string("other");
                r.key("option_count"); r.value_int(option_count);
                r.key("is_narrative"); r.value_bool(narrative != 0);
                r.key("char_id"); r.value_uint(char_id & 0xFFFFFF);
                r.end_object();

                event_count++;
                if (event_count >= 16) break;  // output cap
            }
        }
    }

    r.end_array();
    r.key("count"); r.value_int(event_count);

    // Summary flag: is the game paused specifically because of a pending event popup?
    bool event_popup_active = (event_pause_count > 0) && (event_count > 0);
    r.key("event_popup_active"); r.value_bool(event_popup_active);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// event_probabilities — AI option weights for pending events
// =============================================================================
//
// For each pending event, enumerates options and reads their ai_chance weight.
// Uses the direct override at option+0x318 (most events set this). When the
// direct override is 0, the weight would require script evaluation via
// AI_CHANCE_COMPUTE (game-thread context needed) — we report it as script-based.
//
// Output includes per-option weights and normalized probabilities for the
// weighted_random selection behavior the engine uses.

bool do_query_event_probabilities(const char* /*params_json*/, std::string* out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_event_probabilities");

    // Determine player character ID
    uint32_t player_id = g_controlled_primary.load(std::memory_order_relaxed);
    if (player_id == 0) {
        player_id = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
    }
    player_id &= 0xFFFFFF;
    r.key("player_char_id"); r.value_uint(player_id);

    // Access event queue
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    uintptr_t queue = 0;
    if (gs) {
        uintptr_t subsys = safe_read<uintptr_t>(gs + ck3::event_sys::GAME_STATE_SUBSYS_PTR);
        if (subsys && (subsys && is_mapped(subsys))) {
            queue = subsys + ck3::event_sys::EVENT_QUEUE_OFFSET;
        }
    }

    r.key("events"); r.begin_array();
    int event_count = 0;

    if (queue && player_id > 0) {
        int32_t raw_count = safe_read<int32_t>(queue + ck3::event_sys::EVENT_LIST_COUNT);
        uintptr_t event_list = safe_read<uintptr_t>(queue + ck3::event_sys::EVENT_LIST_PTR);

        if (raw_count > 0 && event_list && (event_list && is_mapped(event_list))) {
            if (raw_count > 32) raw_count = 32;

            for (int i = 0; i < raw_count && event_count < 16; i++) {
                uintptr_t entry_addr = event_list + i * 8;
                if ((!entry_addr || !is_mapped(entry_addr))) break;
                uintptr_t sp_event = safe_read<uintptr_t>(entry_addr);
                if (!sp_event || (!sp_event || !is_mapped(sp_event))) continue;

                uint32_t char_id = safe_read<uint32_t>(sp_event + ck3::event_sys::SPEVENT_CHAR_ID);
                if ((char_id & 0xFFFFFF) != player_id) continue;

                uintptr_t event_def = safe_read<uintptr_t>(sp_event + ck3::event_sys::SPEVENT_EVENT_DEF);
                if (!event_def || (!event_def || !is_mapped(event_def))) continue;

                int32_t state = safe_read<int32_t>(event_def + ck3::event_sys::CEVENT_STATE);
                if (state < 2) continue;

                uint32_t instance_id = safe_read<uint32_t>(sp_event + ck3::event_sys::SPEVENT_INSTANCE_ID);
                int32_t option_count = safe_read<int32_t>(event_def + ck3::event_sys::CEVENT_OPTION_COUNT);
                std::string event_key = ad_read_std_string(event_def + ck3::db_object::NAME_KEY);

                r.begin_object();
                r.key("instance_id"); r.value_uint(instance_id);
                r.key("event_id"); r.value_string(event_key.empty() ? "unknown" : event_key.c_str());
                r.key("option_count"); r.value_int(option_count);

                // Read options array pointer from CEvent
                uintptr_t options_ptr = safe_read<uintptr_t>(event_def + ck3::event_sys::CEVENT_OPTIONS_ARRAY);

                r.key("options"); r.begin_array();

                // Track weights for probability calculation
                int64_t weights[16] = {};
                int valid_options = 0;
                int64_t weight_sum = 0;
                bool has_script_weights = false;

                if (options_ptr && (options_ptr && is_mapped(options_ptr)) && option_count > 0) {
                    int opts = option_count;
                    if (opts > 16) opts = 16;

                    for (int j = 0; j < opts; j++) {
                        // Options array is an array of pointers to CJominiEventOption
                        uintptr_t opt_ptr_addr = options_ptr + j * 8;
                        if ((!opt_ptr_addr || !is_mapped(opt_ptr_addr))) break;
                        uintptr_t opt = safe_read<uintptr_t>(opt_ptr_addr);
                        if (!opt || (!opt || !is_mapped(opt))) {
                            r.begin_object();
                            r.key("index"); r.value_int(j);
                            r.key("error"); r.value_string("invalid_pointer");
                            r.end_object();
                            continue;
                        }

                        // Read direct ai_chance override (int32 at option+0x318)
                        int32_t direct_weight = safe_read<int32_t>(opt + ck3::event_sys::OPTION_AI_CHANCE_DIRECT);

                        r.begin_object();
                        r.key("index"); r.value_int(j);

                        if (direct_weight != 0) {
                            // Direct weight: fixed-point /100000 but the direct override
                            // is raw int32 (typically 1-100 scale before engine scaling).
                            // Report both raw and as float.
                            r.key("weight_raw"); r.value_int(direct_weight);
                            r.key("weight_type"); r.value_string("direct");
                            weights[j] = static_cast<int64_t>(direct_weight);
                            if (direct_weight > 0) weight_sum += direct_weight;
                        } else {
                            // Script-based weight: needs AI_CHANCE_COMPUTE with game-thread
                            // context. Report as script-based with the script block address.
                            r.key("weight_raw"); r.value_int(0);
                            r.key("weight_type"); r.value_string("script");
                            has_script_weights = true;
                            // Use weight of 1 as fallback for probability estimation
                            weights[j] = 1;
                            weight_sum += 1;
                        }

                        r.end_object();
                        valid_options++;
                    }
                }

                r.end_array(); // options

                // Compute normalized probabilities from available weights
                r.key("probabilities"); r.begin_array();
                if (weight_sum > 0) {
                    for (int j = 0; j < valid_options; j++) {
                        r.begin_object();
                        r.key("option"); r.value_int(j);
                        double prob = static_cast<double>(weights[j]) / static_cast<double>(weight_sum);
                        r.key("probability"); r.value_double(prob);
                        r.end_object();
                    }
                }
                r.end_array();

                r.key("has_script_weights"); r.value_bool(has_script_weights);
                if (has_script_weights) {
                    r.key("probability_note"); r.value_string(
                        "Some options use script-evaluated weights. "
                        "Probabilities for script options use fallback weight=1. "
                        "Actual probabilities depend on character traits, modifiers, and game state.");
                }

                r.end_object();
                event_count++;
            }
        }
    }

    r.end_array(); // events
    r.key("count"); r.value_int(event_count);

    r.end_object();
    if (out) *out = r.take();
    return true;
}
#ifdef CK3_BRIDGE_DEBUG_VERBS  // rc2 B2: raw memory reads (memory_probe, resolve_global) are compiled out of release builds

// =============================================================================
// memory_probe — General-purpose hex dump for RE validation
// =============================================================================

// Global name -> offset mapping for resolve_global and memory_probe base="global"
struct GlobalEntry { const char* name; uintptr_t offset; };
static const GlobalEntry g_global_table[] = {
    {"AI_ENABLED_FLAG",     ck3::globals::AI_ENABLED_FLAG},
    {"AI_GLOBAL_TOGGLE",    ck3::globals::AI_GLOBAL_TOGGLE},
    {"AI_RNG_COUNTER",      ck3::globals::AI_RNG_COUNTER},
    {"AI_WAR_SUPPRESS",     ck3::globals::AI_WAR_SUPPRESS},
    {"AI_EVENT_EVAL_FLAG",  ck3::globals::AI_EVENT_EVAL_FLAG},
    {"GAME_STATE_ROOT",     ck3::globals::GAME_STATE_ROOT},
    {"CHARACTER_DB",        ck3::globals::CHARACTER_DB},
    {"INTERACTION_DB",      ck3::globals::INTERACTION_DB},
    {"TRAIT_DB",            ck3::globals::TRAIT_DB},
    {"BUILDING_TYPE_DB",    ck3::globals::BUILDING_TYPE_DB},
    {"CULTURE_DB",          ck3::globals::CULTURE_DB},
    {"FAITH_DB",            ck3::globals::FAITH_DB},
    {"REALM_LAW_DB",        ck3::globals::REALM_LAW_DB},
    {"FACTION_TYPE_DB_V2",  ck3::globals::FACTION_TYPE_DB_V2},
    {"IDLER_SINGLETON",     ck3::globals::IDLER_SINGLETON},
    {"AI_TOGGLE_BASE",      ck3::globals::AI_TOGGLE_BASE},
    {"CHAR_DB_SENTINEL",    ck3::globals::CHAR_DB_SENTINEL},
    {"GAME_IS_LOADED",      ck3::globals::GAME_IS_LOADED},
    {"AI_NULL_CONTROLLER",  ck3::globals::AI_NULL_CONTROLLER},
    {"PLAYER_CHAR_ID",      ck3::globals::PLAYER_CHAR_ID},
    {"DECISION_DB",         ck3::globals::DECISION_DB},
    {"MAX_GAME_SPEED",      ck3::globals::MAX_GAME_SPEED},
    {"EVENT_MANAGER",       ck3::globals::EVENT_MANAGER},
    {"COMMAND_DISPATCH_MGR", ck3::globals::COMMAND_DISPATCH_MGR},
    {"COMMAND_DISPATCH_GATE", ck3::globals::COMMAND_DISPATCH_GATE},
    {"LANDED_TITLE_DB",     ck3::globals::LANDED_TITLE_DB},
    {"COUNCIL_TYPE_DB",     ck3::globals::COUNCIL_TYPE_DB},
    {"CASUS_BELLI_DB",      ck3::globals::CASUS_BELLI_DB},
    {"WAR_DB",              ck3::globals::WAR_DB},
    {"ARMY_DB",             ck3::globals::ARMY_DB},
    {"VASSAL_CONTRACT_DB",  ck3::globals::VASSAL_CONTRACT_DB},
    {"SCHEME_REFDB",        ck3::globals::SCHEME_REFDB},
    {"SCHEME_TYPE_DB",      ck3::globals::SCHEME_TYPE_DB},
    {"LOCALIZATION_DB",     ck3::globals::LOCALIZATION_DB},
    {"HOUSE_DB",            ck3::globals::HOUSE_DB},
    {"DYNASTY_DB",          ck3::globals::DYNASTY_DB},
    {"FOCUS_DB",            ck3::globals::FOCUS_DB},
    {"FACTION_DB",          ck3::globals::FACTION_DB},
    {"OPINION_MODIFIER_DB", ck3::globals::OPINION_MODIFIER_DB},
    {"HOOK_TYPE_DB",        ck3::globals::HOOK_TYPE_DB},
    {"FAST_TICK_TOGGLE",    ck3::globals::FAST_TICK_TOGGLE},
    {nullptr, 0}
};

static uintptr_t lookup_global_offset(const char* name) {
    for (const GlobalEntry* e = g_global_table; e->name; e++) {
        if (strcmp(e->name, name) == 0) return e->offset;
    }
    return 0;
}

static uintptr_t parse_hex_address(const char* str) {
    if (!str) return 0;
    return static_cast<uintptr_t>(strtoull(str, nullptr, 16));
}

bool do_query_memory_probe(const char* params_json, std::string* out) {
    if (!out) return false;

    // Parse parameters
    const char* base_str = json_get_string(params_json, "base");
    const char* address_str = json_get_string(params_json, "address");
    const char* offset_str = json_get_string(params_json, "offset");
    const char* global_name = json_get_string(params_json, "global_name");
    int char_id = json_get_int(params_json, "char_id");
    int length = json_get_int(params_json, "length");

    if (length <= 0) length = 64;
    if (length > 4096) length = 4096;
    // Round up to 8-byte alignment for cleaner annotation
    length = (length + 7) & ~7;

    uintptr_t target_addr = 0;
    uintptr_t extra_offset = offset_str ? parse_hex_address(offset_str) : 0;

    // Resolve base address
    if (address_str && (!base_str || strcmp(base_str, "absolute") == 0)) {
        // Absolute address mode
        target_addr = parse_hex_address(address_str) + extra_offset;
    } else if (base_str && strcmp(base_str, "global") == 0) {
        // Global + offset mode
        if (!global_name) {
            *out = R"({"success":false,"error":"missing global_name for base=global"})";
            return false;
        }
        uintptr_t goff = lookup_global_offset(global_name);
        if (goff == 0) {
            *out = R"({"success":false,"error":"unknown global_name"})";
            return false;
        }
        target_addr = g_base_addr + goff + extra_offset;
    } else if (base_str && strcmp(base_str, "character") == 0) {
        if (char_id <= 0) {
            *out = R"({"success":false,"error":"missing char_id for base=character"})";
            return false;
        }
        void* ch = lookup_character(static_cast<uint32_t>(char_id));
        if (!ch || is_sentinel(ch)) {
            *out = R"({"success":false,"error":"character not found"})";
            return false;
        }
        target_addr = reinterpret_cast<uintptr_t>(ch) + extra_offset;
    } else if (base_str && strcmp(base_str, "alive_data") == 0) {
        if (char_id <= 0) {
            *out = R"({"success":false,"error":"missing char_id for base=alive_data"})";
            return false;
        }
        void* ch = lookup_character(static_cast<uint32_t>(char_id));
        if (!ch || is_sentinel(ch)) {
            *out = R"({"success":false,"error":"character not found"})";
            return false;
        }
        uintptr_t alive = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(ch) + 0x1A8);
        if ((!alive || !is_mapped(alive))) {
            *out = R"({"success":false,"error":"alive_data is null"})";
            return false;
        }
        target_addr = alive + extra_offset;
    } else if (base_str && strcmp(base_str, "landed_data") == 0) {
        if (char_id <= 0) {
            *out = R"({"success":false,"error":"missing char_id for base=landed_data"})";
            return false;
        }
        void* ch = lookup_character(static_cast<uint32_t>(char_id));
        if (!ch || is_sentinel(ch)) {
            *out = R"({"success":false,"error":"character not found"})";
            return false;
        }
        uintptr_t landed = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(ch) + ck3::character::TITLE_REALM_DATA);
        if ((!landed || !is_mapped(landed))) {
            *out = R"({"success":false,"error":"landed_data is null"})";
            return false;
        }
        target_addr = landed + extra_offset;
    } else if (base_str && strcmp(base_str, "title_realm") == 0) {
        if (char_id <= 0) {
            *out = R"({"success":false,"error":"missing char_id for base=title_realm"})";
            return false;
        }
        void* ch = lookup_character(static_cast<uint32_t>(char_id));
        if (!ch || is_sentinel(ch)) {
            *out = R"({"success":false,"error":"character not found"})";
            return false;
        }
        uintptr_t tr = resolve_title_realm(reinterpret_cast<uintptr_t>(ch));
        if (!tr) {
            *out = R"({"success":false,"error":"title_realm is null"})";
            return false;
        }
        target_addr = tr + extra_offset;
    } else {
        *out = R"json({"success":false,"error":"unknown base type (use: absolute, global, character, alive_data, landed_data, title_realm)"})json";
        return false;
    }

    // Validate target address range
    if (!is_mapped_range(target_addr, static_cast<size_t>(length))) {
        ResponseBuilder r;
        r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("error"); r.value_string("address range not mapped");
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "0x%lx", target_addr);
        r.key("address"); r.value_string(addr_buf);
        r.key("length"); r.value_int(length);
        r.end_object();
        *out = r.take();
        return false;
    }

    // Read memory
    std::vector<uint8_t> data(static_cast<size_t>(length));
    if (!safe_read_bytes(target_addr, data.data(), static_cast<size_t>(length))) {
        *out = R"({"success":false,"error":"safe_read_bytes failed"})";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);

    // Address as hex string
    char addr_buf[32];
    snprintf(addr_buf, sizeof(addr_buf), "0x%lx", target_addr);
    r.key("address"); r.value_string(addr_buf);
    r.key("length"); r.value_int(length);

    // Hex dump string
    std::string hex_str;
    hex_str.reserve(static_cast<size_t>(length) * 3);
    for (int i = 0; i < length; i++) {
        char hx[4];
        snprintf(hx, sizeof(hx), "%02x", data[static_cast<size_t>(i)]);
        if (i > 0) hex_str += ' ';
        hex_str += hx;
    }
    r.key("hex"); r.value_string(hex_str.c_str());

    // Annotations: interpret every 8-byte-aligned position
    r.key("annotations"); r.begin_array();
    for (int off = 0; off + 8 <= length; off += 8) {
        // Read as pointer (8 bytes)
        uint64_t val64;
        memcpy(&val64, data.data() + off, 8);
        // Read as two uint32s
        uint32_t lo32, hi32;
        memcpy(&lo32, data.data() + off, 4);
        memcpy(&hi32, data.data() + off + 4, 4);

        bool ptr_valid = is_mapped(static_cast<uintptr_t>(val64));

        r.begin_object();
        r.key("offset"); r.value_int(off);

        // Pointer interpretation
        char ptr_buf[32];
        snprintf(ptr_buf, sizeof(ptr_buf), "0x%lx", static_cast<unsigned long>(val64));
        r.key("as_ptr"); r.value_string(ptr_buf);
        r.key("ptr_valid"); r.value_bool(ptr_valid);

        // uint32 pair interpretation
        r.key("as_u32_lo"); r.value_uint(lo32);
        r.key("as_u32_hi"); r.value_uint(hi32);

        // int32 pair interpretation
        int32_t ilo32, ihi32;
        memcpy(&ilo32, data.data() + off, 4);
        memcpy(&ihi32, data.data() + off + 4, 4);
        r.key("as_i32_lo"); r.value_int(ilo32);
        r.key("as_i32_hi"); r.value_int(ihi32);

        // float pair interpretation
        float flo, fhi;
        memcpy(&flo, data.data() + off, 4);
        memcpy(&fhi, data.data() + off + 4, 4);
        // Only emit float if it looks plausible (not NaN/Inf and within a reasonable range)
        if (flo == flo && flo > -1e10 && flo < 1e10) {  // NaN check: NaN != NaN
            r.key("as_f32_lo"); r.value_float(flo);
        }
        if (fhi == fhi && fhi > -1e10 && fhi < 1e10) {
            r.key("as_f32_hi"); r.value_float(fhi);
        }

        r.end_object();
    }
    r.end_array();

    r.end_object();
    *out = r.take();
    return true;
}

// =============================================================================
// resolve_global — Read a global with type inference
// =============================================================================

bool do_query_resolve_global(const char* params_json, std::string* out) {
    if (!out) return false;

    const char* name = json_get_string(params_json, "name");
    const char* address_str = json_get_string(params_json, "address");

    uintptr_t target_addr = 0;
    const char* resolved_name = nullptr;

    if (name) {
        uintptr_t goff = lookup_global_offset(name);
        if (goff == 0) {
            // Check if name is actually a hex address
            if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
                target_addr = g_base_addr + parse_hex_address(name);
            } else {
                *out = R"({"success":false,"error":"unknown global name"})";
                return false;
            }
        } else {
            target_addr = g_base_addr + goff;
            resolved_name = name;
        }
    } else if (address_str) {
        uintptr_t raw = parse_hex_address(address_str);
        // If address looks like a relative offset (< 0x10000000), add base
        if (raw < 0x10000000) {
            target_addr = g_base_addr + raw;
        } else {
            target_addr = raw;
        }
        // Try to find a matching global name
        for (const GlobalEntry* e = g_global_table; e->name; e++) {
            if (g_base_addr + e->offset == target_addr) {
                resolved_name = e->name;
                break;
            }
        }
    } else {
        *out = R"({"success":false,"error":"provide 'name' or 'address'"})";
        return false;
    }

    if (!is_mapped_range(target_addr, 8)) {
        *out = R"({"success":false,"error":"address not mapped"})";
        return false;
    }

    // Read 8 bytes at the global address
    uint64_t raw_val = safe_read<uint64_t>(target_addr);
    uint32_t val32 = static_cast<uint32_t>(raw_val);
    int32_t ival32 = static_cast<int32_t>(raw_val);
    float fval;
    memcpy(&fval, &val32, 4);

    bool looks_like_ptr = is_mapped(static_cast<uintptr_t>(raw_val));

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);

    if (resolved_name) {
        r.key("name"); r.value_string(resolved_name);
    }

    char addr_buf[32];
    snprintf(addr_buf, sizeof(addr_buf), "0x%lx", target_addr);
    r.key("address"); r.value_string(addr_buf);

    r.key("raw_u64"); r.value_uint(raw_val);

    char hex_buf[32];
    snprintf(hex_buf, sizeof(hex_buf), "0x%016lx", static_cast<unsigned long>(raw_val));
    r.key("hex"); r.value_string(hex_buf);

    // Interpretations
    r.key("interpretations"); r.begin_object();
    r.key("as_uint32"); r.value_uint(val32);
    r.key("as_int32"); r.value_int(ival32);
    if (fval == fval && fval > -1e10 && fval < 1e10) {
        r.key("as_float"); r.value_float(fval);
    }

    char ptr_str[64];
    snprintf(ptr_str, sizeof(ptr_str), "0x%lx (%s)", static_cast<unsigned long>(raw_val),
             looks_like_ptr ? "MAPPED" : "UNMAPPED");
    r.key("as_pointer"); r.value_string(ptr_str);

    // Heuristic: what does the value look like?
    if (looks_like_ptr) {
        r.key("looks_like"); r.value_string("pointer");
    } else if (val32 > 0 && val32 < 0x1000000 && raw_val == val32) {
        r.key("looks_like"); r.value_string("small_id_or_count");
    } else if (val32 == 0) {
        r.key("looks_like"); r.value_string("zero/null");
    } else if (val32 == 0xFFFFFFFF) {
        r.key("looks_like"); r.value_string("invalid_id (0xFFFFFFFF)");
    } else {
        r.key("looks_like"); r.value_string("integer");
    }

    r.end_object();  // interpretations

    // If pointer is valid, dereference and show first 32 bytes
    if (looks_like_ptr) {
        uintptr_t deref_addr = static_cast<uintptr_t>(raw_val);
        if (is_mapped_range(deref_addr, 32)) {
            uint8_t deref_data[32];
            if (safe_read_bytes(deref_addr, deref_data, 32)) {
                std::string deref_hex;
                deref_hex.reserve(96);
                for (int i = 0; i < 32; i++) {
                    char hx[4];
                    snprintf(hx, sizeof(hx), "%02x", deref_data[i]);
                    if (i > 0) deref_hex += ' ';
                    deref_hex += hx;
                }
                r.key("deref_hex"); r.value_string(deref_hex.c_str());

                // Also show first 8 bytes as pointer
                uint64_t deref_ptr;
                memcpy(&deref_ptr, deref_data, 8);
                char dp_buf[32];
                snprintf(dp_buf, sizeof(dp_buf), "0x%lx", static_cast<unsigned long>(deref_ptr));
                r.key("deref_first_ptr"); r.value_string(dp_buf);
                r.key("deref_first_ptr_valid"); r.value_bool(is_mapped(static_cast<uintptr_t>(deref_ptr)));
            }
        }
    }

    // List available global names (for discoverability)
    r.key("available_globals"); r.begin_array();
    for (const GlobalEntry* e = g_global_table; e->name; e++) {
        r.value_string(e->name);
    }
    r.end_array();

    r.end_object();
    *out = r.take();
    return true;
}
#endif  // CK3_BRIDGE_DEBUG_VERBS (rc2 B2)

// =============================================================================
// query_trait_db — Enumerate all traits in the trait database with full metadata
// =============================================================================
bool do_query_trait_db(const char* /*params_json*/, std::string* out) {
    uintptr_t tdb = safe_read_offset<uintptr_t>(ck3::globals::TRAIT_DB);
    if (!tdb) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_trait_db: trait DB not found\n");
        return false;
    }

    int32_t max_id = safe_read<int32_t>(tdb + ck3::trait_db::ARRAY_COUNT);
    uintptr_t trait_table = safe_read<uintptr_t>(tdb + ck3::trait_db::ARRAY_PTR);
    if (!trait_table || max_id <= 0 || max_id > 1000) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_trait_db: invalid table ptr=%lx count=%d\n",
                trait_table, max_id);
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("trait_count"); r.value_int(max_id);
    r.key("traits"); r.begin_array();

    for (int32_t id = 0; id < max_id; id++) {
        uintptr_t te = safe_read<uintptr_t>(trait_table + id * 8);
        if (!te || (!te || !is_mapped(te))) continue;

        r.begin_object();
        r.key("id"); r.value_uint(id);

        const char* name = nullptr;
        size_t name_len = 0;
        if (read_db_obj_name(te, name, name_len)) {
            r.key("name"); r.value_string(name, name_len);
        }

        uint8_t category = safe_read<uint8_t>(te + ck3::trait_entry::CATEGORY);
        r.key("category"); r.value_string(trait_category_name(category));
        r.key("category_id"); r.value_uint(category);

        uint32_t flags = safe_read<uint32_t>(te + ck3::trait_entry::FLAGS);
        r.key("flags"); r.value_uint(flags);
        r.key("is_genetic"); r.value_bool((flags & ck3::trait_flags::GENETIC) != 0);
        r.key("is_physical"); r.value_bool((flags & ck3::trait_flags::PHYSICAL) != 0);
        r.key("is_education"); r.value_bool((flags & ck3::trait_flags::EDUCATION_TYPE) != 0);
        r.key("is_immortal"); r.value_bool((flags & ck3::trait_flags::IMMORTAL) != 0);
        r.key("is_incapacitating"); r.value_bool((flags & ck3::trait_flags::INCAPACITATING) != 0);
        r.key("good_genetic"); r.value_bool((flags & ck3::trait_flags::GOOD_GENETIC) != 0);
        r.key("enables_inbred"); r.value_bool((flags & ck3::trait_flags::ENABLES_INBRED) != 0);
        r.key("disables_combat"); r.value_bool((flags & ck3::trait_flags::DISABLE_COMBAT_LEADER) != 0);

        // Opinions
        int32_t same_op = safe_read<int32_t>(te + ck3::trait_entry::SAME_OPINION);
        int32_t same_faith_op = safe_read<int32_t>(te + ck3::trait_entry::SAME_FAITH_OPINION);
        int32_t opp_op = safe_read<int32_t>(te + ck3::trait_entry::OPPOSITE_OPINION);
        r.key("same_opinion"); r.value_int(same_op);
        r.key("same_faith_opinion"); r.value_int(same_faith_op);
        r.key("opposite_opinion"); r.value_int(opp_op);

        // Inheritance
        int32_t inherit = safe_read<int32_t>(te + ck3::trait_entry::INHERIT_CHANCE);
        int32_t both_parent = safe_read<int32_t>(te + ck3::trait_entry::BOTH_PARENT_CHANCE);
        uint8_t parent_sex = safe_read<uint8_t>(te + ck3::trait_entry::PARENT_INHERIT_SEX);
        uint8_t child_sex = safe_read<uint8_t>(te + ck3::trait_entry::CHILD_INHERIT_SEX);
        r.key("inherit_chance"); r.value_int(inherit);
        r.key("both_parent_chance"); r.value_int(both_parent);
        r.key("parent_inherit_sex"); r.value_uint(parent_sex);
        r.key("child_inherit_sex"); r.value_uint(child_sex);

        // Age limits
        int32_t min_age = safe_read<int32_t>(te + ck3::trait_entry::MINIMUM_AGE);
        int32_t max_age = safe_read<int32_t>(te + ck3::trait_entry::MAXIMUM_AGE);
        uint8_t valid_sex = safe_read<uint8_t>(te + ck3::trait_entry::VALID_SEX);
        r.key("minimum_age"); r.value_int(min_age);
        r.key("maximum_age"); r.value_int(max_age);
        r.key("valid_sex"); r.value_uint(valid_sex);

        // Education level range
        int16_t min_val = safe_read<int16_t>(te + ck3::trait_entry::MIN_VALUE);
        int16_t max_val = safe_read<int16_t>(te + ck3::trait_entry::MAX_VALUE);
        if (min_val != 0 || max_val != 0) {
            r.key("min_value"); r.value_int(min_val);
            r.key("max_value"); r.value_int(max_val);
        }

        // Group name
        std::string group = ad_read_std_string(te + ck3::trait_entry::GROUP_NAME);
        if (!group.empty()) {
            r.key("group"); r.value_string(group.c_str());
        }

        // Level tracks (commander traits)
        uint32_t lt_count = safe_read<uint32_t>(te + ck3::trait_entry::LEVEL_TRACK_PACKED);
        if (lt_count > 0 && lt_count < 100) {
            r.key("level_track_count"); r.value_uint(lt_count);
        }

        // Ruler designer cost
        int32_t rd_cost = safe_read<int32_t>(te + ck3::trait_entry::RULER_DESIGNER_COST);
        if (rd_cost != 0) {
            r.key("ruler_designer_cost"); r.value_int(rd_cost);
        }

        r.end_object();
    }

    r.end_array();
    r.end_object();
    *out = r.take();
    return true;
}

// =============================================================================
// query_trait_compatibility — Read compatibility map for a specific trait
// Shows which other traits conflict or synergize with the queried trait
// =============================================================================
bool do_query_trait_compatibility(const char* params_json, std::string* out) {
    int trait_id = json_get_int(params_json, "trait_id");
    if (trait_id < 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_trait_compatibility: missing trait_id\n");
        return false;
    }

    uintptr_t tdb = safe_read_offset<uintptr_t>(ck3::globals::TRAIT_DB);
    if (!tdb) return false;

    int32_t max_id = safe_read<int32_t>(tdb + ck3::trait_db::ARRAY_COUNT);
    uintptr_t trait_table = safe_read<uintptr_t>(tdb + ck3::trait_db::ARRAY_PTR);
    if (!trait_table || trait_id >= max_id) return false;

    uintptr_t te = safe_read<uintptr_t>(trait_table + trait_id * 8);
    if (!te || (!te || !is_mapped(te))) return false;

    ResponseBuilder r;
    r.begin_object();
    r.key("trait_id"); r.value_int(trait_id);

    const char* name = nullptr;
    size_t name_len = 0;
    if (read_db_obj_name(te, name, name_len)) {
        r.key("trait_name"); r.value_string(name, name_len);
    }

    // Compatibility map at CTrait+0x194
    // CK3 hash map format: ptr at +0x194 points to bucket array
    // Each bucket entry is 0x18 bytes: [+0] key_hash, [+4] trait_id, [+8] value (int32), [+0xC] next_ptr...
    // The hash map at +0x194 is embedded in the struct (not a pointer to a separate allocation)
    // Format: { ptr_to_buckets at +0, bucket_count_mask at +0xC }
    uintptr_t compat_base = te + ck3::trait_entry::COMPATIBILITY_MAP;
    uintptr_t buckets_ptr = safe_read<uintptr_t>(compat_base);
    uint32_t bucket_mask = safe_read<uint32_t>(compat_base + 0xC);

    r.key("compatibilities"); r.begin_array();

    if (buckets_ptr && (buckets_ptr && is_mapped(buckets_ptr)) && bucket_mask > 0 && bucket_mask < 0x10000) {
        uint32_t bucket_count = bucket_mask + 1;
        for (uint32_t b = 0; b < bucket_count; b++) {
            // Each bucket slot is 0x18 bytes
            uintptr_t entry = buckets_ptr + b * 0x18;
            // Check if slot is occupied: first 8 bytes are the key (trait ref)
            uintptr_t key_ref = safe_read<uintptr_t>(entry);
            if (!key_ref || key_ref == 0xFFFFFFFFFFFFFFFF) continue;

            // The value is a CFixedPoint (int32 / 1000 or similar) at offset +0x10
            int32_t compat_value = safe_read<int32_t>(entry + 0x10);

            // Try to resolve the target trait from the key reference
            // The key is likely a CJominiDbRef or direct trait pointer
            uint32_t target_id = safe_read<uint32_t>(entry + 0x08);

            r.begin_object();
            r.key("target_trait_id"); r.value_uint(target_id);

            // Try to resolve target trait name
            if (target_id < static_cast<uint32_t>(max_id)) {
                uintptr_t target_te = safe_read<uintptr_t>(trait_table + target_id * 8);
                if (target_te && (target_te && is_mapped(target_te))) {
                    const char* tname = nullptr;
                    size_t tname_len = 0;
                    if (read_db_obj_name(target_te, tname, tname_len)) {
                        r.key("target_trait_name"); r.value_string(tname, tname_len);
                    }
                }
            }

            r.key("value"); r.value_int(compat_value);
            r.end_object();
        }
    }

    r.end_array();
    r.end_object();
    *out = r.take();
    return true;
}

// =============================================================================
// Law Query Handlers
// =============================================================================

// query_realm_laws: list active realm laws for a character
bool do_query_realm_laws(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = R"({"error":"character_not_found"})";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uintptr_t title_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (!title_data || (!title_data || !is_mapped(title_data))) {
        if (out) *out = R"({"error":"not_a_ruler"})";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_get_id(character) & 0xFFFFFF);

    // Read active law container from title_realm_data+0x200
    uintptr_t law_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm_laws::ACTIVE_LAW_ARRAY);
    uint32_t law_count = safe_read<uint32_t>(title_data + ck3::title_realm_laws::ACTIVE_LAW_COUNT);

    if (!law_arr_ptr || (!law_arr_ptr || !is_mapped(law_arr_ptr)) || law_count == 0 || law_count > 200) {
        r.key("active_laws"); r.begin_array(); r.end_array();
        r.key("law_count"); r.value_int(0);
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    r.key("active_laws"); r.begin_array();

    uint32_t actual_law_count = 0;
    for (uint32_t i = 0; i < law_count; i++) {
        uintptr_t law_ptr = safe_read<uintptr_t>(law_arr_ptr + i * 8);
        if (!law_ptr || (!law_ptr || !is_mapped(law_ptr))) continue;

        r.begin_object();

        uint32_t law_id = safe_read<uint32_t>(law_ptr + ck3::realm_law_entry::LAW_ID);
        r.key("law_id"); r.value_uint(law_id);

        auto law_name = ad_read_std_string(law_ptr + ck3::realm_law_entry::LAW_NAME);
        if (!law_name.empty()) {
            r.key("name"); r.value_string(law_name.c_str(), law_name.size());
        }

        // Read group info
        uintptr_t group_ptr = safe_read<uintptr_t>(law_ptr + ck3::realm_law_entry::LAW_GROUP_PTR);
        if (group_ptr && (group_ptr && is_mapped(group_ptr))) {
            uint32_t group_id = safe_read<uint32_t>(group_ptr + ck3::law_group::GROUP_ID);
            r.key("group_id"); r.value_uint(group_id);

            auto group_name = ad_read_std_string(group_ptr + ck3::law_group::GROUP_NAME);
            if (!group_name.empty()) {
                r.key("group_name"); r.value_string(group_name.c_str(), group_name.size());
            }

            // Find this law's index within the group (its "level")
            uint32_t member_count = safe_read<uint32_t>(group_ptr + ck3::law_group::MEMBER_COUNT);
            uintptr_t member_arr = safe_read<uintptr_t>(group_ptr + ck3::law_group::MEMBER_LAW_ARRAY);
            if (member_arr && (member_arr && is_mapped(member_arr)) && member_count > 0 && member_count <= 100) {
                for (uint32_t m = 0; m < member_count; m++) {
                    uintptr_t member_law = safe_read<uintptr_t>(member_arr + m * 8);
                    if (member_law == law_ptr) {
                        r.key("level"); r.value_uint(m);
                        break;
                    }
                }
                r.key("max_level"); r.value_uint(member_count - 1);
            }
        }

        r.end_object();
        actual_law_count++;
    }

    r.end_array();
    r.key("law_count"); r.value_uint(actual_law_count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_law_groups: enumerate all law groups and their member laws
bool do_query_law_groups(const char* /*params_json*/, std::string* out) {
    // Read the realm law DB global
    uintptr_t db_ptr_ptr = safe_read_offset<uintptr_t>(ck3::globals::REALM_LAW_DB);
    if (!db_ptr_ptr || (!db_ptr_ptr || !is_mapped(db_ptr_ptr))) {
        if (out) *out = R"({"error":"realm_law_db_not_found"})";
        return false;
    }

    uintptr_t law_arr = safe_read<uintptr_t>(db_ptr_ptr + ck3::realm_law_db::LAW_PTR_ARRAY);
    uint32_t law_count = safe_read<uint32_t>(db_ptr_ptr + ck3::realm_law_db::LAW_COUNT);

    if (!law_arr || (!law_arr || !is_mapped(law_arr)) || law_count == 0 || law_count > 500) {
        if (out) *out = R"({"error":"realm_law_db_empty"})";
        return false;
    }

    // Collect unique groups by scanning all laws
    struct GroupInfo {
        uintptr_t ptr;
        uint32_t id;
        std::string name;
        std::string default_law;
        std::vector<std::pair<uint32_t, std::string>> members; // law_id, law_name
    };
    std::map<uintptr_t, GroupInfo> groups;

    for (uint32_t i = 0; i < law_count; i++) {
        uintptr_t law_ptr = safe_read<uintptr_t>(law_arr + i * 8);
        if (!law_ptr || (!law_ptr || !is_mapped(law_ptr))) continue;

        uintptr_t group_ptr = safe_read<uintptr_t>(law_ptr + ck3::realm_law_entry::LAW_GROUP_PTR);
        if (!group_ptr || (!group_ptr || !is_mapped(group_ptr))) continue;

        if (groups.find(group_ptr) == groups.end()) {
            GroupInfo gi;
            gi.ptr = group_ptr;
            gi.id = safe_read<uint32_t>(group_ptr + ck3::law_group::GROUP_ID);
            gi.name = ad_read_std_string(group_ptr + ck3::law_group::GROUP_NAME);
            gi.default_law = ad_read_std_string(group_ptr + ck3::law_group::DEFAULT_LAW_NAME);

            // Read members in order from the group's own member array
            uint32_t mc = safe_read<uint32_t>(group_ptr + ck3::law_group::MEMBER_COUNT);
            uintptr_t ma = safe_read<uintptr_t>(group_ptr + ck3::law_group::MEMBER_LAW_ARRAY);
            if (ma && (ma && is_mapped(ma)) && mc > 0 && mc <= 100) {
                for (uint32_t m = 0; m < mc; m++) {
                    uintptr_t ml = safe_read<uintptr_t>(ma + m * 8);
                    if (!ml || (!ml || !is_mapped(ml))) continue;
                    uint32_t mid = safe_read<uint32_t>(ml + ck3::realm_law_entry::LAW_ID);
                    auto mname = ad_read_std_string(ml + ck3::realm_law_entry::LAW_NAME);
                    gi.members.push_back({mid, mname});
                }
            }
            groups[group_ptr] = std::move(gi);
        }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("total_laws"); r.value_uint(law_count);
    r.key("total_groups"); r.value_uint(static_cast<uint64_t>(groups.size()));
    r.key("groups"); r.begin_array();

    for (auto& [ptr, gi] : groups) {
        r.begin_object();
        r.key("group_id"); r.value_uint(gi.id);
        if (!gi.name.empty()) {
            r.key("name"); r.value_string(gi.name.c_str(), gi.name.size());
        }
        if (!gi.default_law.empty()) {
            r.key("default_law"); r.value_string(gi.default_law.c_str(), gi.default_law.size());
        }
        r.key("member_count"); r.value_uint(static_cast<uint64_t>(gi.members.size()));
        r.key("members"); r.begin_array();
        for (auto& [mid, mname] : gi.members) {
            r.begin_object();
            r.key("law_id"); r.value_uint(mid);
            if (!mname.empty()) {
                r.key("name"); r.value_string(mname.c_str(), mname.size());
            }
            r.end_object();
        }
        r.end_array();
        r.end_object();
    }

    r.end_array();
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_available_law_changes: for each active law, list other options in the same group
bool do_query_available_law_changes(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = R"({"error":"character_not_found"})";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uintptr_t title_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (!title_data || (!title_data || !is_mapped(title_data))) {
        if (out) *out = R"({"error":"not_a_ruler"})";
        return false;
    }

    uintptr_t law_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm_laws::ACTIVE_LAW_ARRAY);
    uint32_t law_count = safe_read<uint32_t>(title_data + ck3::title_realm_laws::ACTIVE_LAW_COUNT);

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_get_id(character) & 0xFFFFFF);
    r.key("groups"); r.begin_array();

    if (law_arr_ptr && (law_arr_ptr && is_mapped(law_arr_ptr)) && law_count > 0 && law_count <= 200) {
        for (uint32_t i = 0; i < law_count; i++) {
            uintptr_t law_ptr = safe_read<uintptr_t>(law_arr_ptr + i * 8);
            if (!law_ptr || (!law_ptr || !is_mapped(law_ptr))) continue;

            uintptr_t group_ptr = safe_read<uintptr_t>(law_ptr + ck3::realm_law_entry::LAW_GROUP_PTR);
            if (!group_ptr || (!group_ptr || !is_mapped(group_ptr))) continue;

            auto current_name = ad_read_std_string(law_ptr + ck3::realm_law_entry::LAW_NAME);
            auto group_name = ad_read_std_string(group_ptr + ck3::law_group::GROUP_NAME);

            r.begin_object();
            if (!group_name.empty()) {
                r.key("group_name"); r.value_string(group_name.c_str(), group_name.size());
            }
            if (!current_name.empty()) {
                r.key("current_law"); r.value_string(current_name.c_str(), current_name.size());
            }

            // List all other members as available options
            uint32_t mc = safe_read<uint32_t>(group_ptr + ck3::law_group::MEMBER_COUNT);
            uintptr_t ma = safe_read<uintptr_t>(group_ptr + ck3::law_group::MEMBER_LAW_ARRAY);
            r.key("options"); r.begin_array();
            if (ma && (ma && is_mapped(ma)) && mc > 0 && mc <= 100) {
                for (uint32_t m = 0; m < mc; m++) {
                    uintptr_t ml = safe_read<uintptr_t>(ma + m * 8);
                    if (!ml || (!ml || !is_mapped(ml)) || ml == law_ptr) continue;
                    auto mname = ad_read_std_string(ml + ck3::realm_law_entry::LAW_NAME);
                    if (!mname.empty()) {
                        r.begin_object();
                        r.key("name"); r.value_string(mname.c_str(), mname.size());
                        r.key("law_id"); r.value_uint(safe_read<uint32_t>(ml + ck3::realm_law_entry::LAW_ID));
                        r.key("level"); r.value_uint(m);
                        r.end_object();
                    }
                }
            }
            r.end_array();
            r.end_object();
        }
    }

    r.end_array();
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// query_building_types: enumerate building types from the building type DB
bool do_query_building_types(const char* params_json, std::string* out) {
    // Optional filter: category (1095=constructible, 1825=holding)
    int filter_category = json_get_int(params_json, "category");

    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::BUILDING_TYPE_DB);
    if ((!db || !is_mapped(db))) {
        if (out) *out = "{\"error\":\"building type DB not found\"}";
        return false;
    }

    uintptr_t entry_table = safe_read<uintptr_t>(db + ck3::building_db::ENTRY_TABLE);
    uint32_t entry_count = safe_read<uint32_t>(db + ck3::building_db::ENTRY_COUNT);
    if ((!entry_table || !is_mapped(entry_table)) || entry_count == 0 || entry_count > 5000) {
        if (out) *out = "{\"error\":\"building type DB entry table invalid\"}";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("total_in_db"); r.value_uint(entry_count);

    r.key("building_types"); r.begin_array();
    int emitted = 0;
    for (uint32_t i = 0; i < entry_count && emitted < 2000; i++) {
        uintptr_t btype = safe_read<uintptr_t>(entry_table + i * 8);
        if ((!btype || !is_mapped(btype))) continue;

        // Read category for filtering
        uint32_t category = safe_read<uint32_t>(btype + ck3::building_type::TYPE_CATEGORY);
        if (filter_category > 0 && static_cast<int>(category) != filter_category) continue;

        // Read name
        auto name = ad_read_std_string(btype + ck3::building_type::NAME_PTR);
        if (name.empty()) continue;

        r.begin_object();
        r.key("index"); r.value_uint(i);
        r.key("name"); r.value_string(name.c_str(), name.size());

        int32_t level = safe_read<int32_t>(btype + ck3::building_type::LEVEL);
        if (level >= 0 && level <= 20) {
            r.key("level"); r.value_int(level);
        }

        r.key("category"); r.value_uint(category);

        // Source file path
        auto source = safe_read_script_text(btype + ck3::building_type::SOURCE_FILE_PATH);
        if (!source.empty()) {
            r.key("source_file"); r.value_string(source.c_str(), source.size());
        }

        // Enable trigger name
        auto trigger = safe_read_script_text(btype + ck3::building_type::ENABLE_TRIGGER);
        if (!trigger.empty()) {
            r.key("enable_trigger"); r.value_string(trigger.c_str(), trigger.size());
        }

        // Next level pointer (upgrade chain)
        uintptr_t next_level = safe_read<uintptr_t>(btype + ck3::building_type::NEXT_LEVEL_PTR);
        if ((next_level && is_mapped(next_level))) {
            auto next_name = ad_read_std_string(next_level + ck3::building_type::NAME_PTR);
            if (!next_name.empty()) {
                r.key("next_level"); r.value_string(next_name.c_str(), next_name.size());
            }
        }

        // Scripted value IDs (cost references)
        uint32_t sv1 = safe_read<uint32_t>(btype + ck3::building_type::SCRIPTED_VALUE_1);
        uint32_t sv2 = safe_read<uint32_t>(btype + ck3::building_type::SCRIPTED_VALUE_2);
        uint32_t sv3 = safe_read<uint32_t>(btype + ck3::building_type::SCRIPTED_VALUE_3);
        if (sv1 > 0) {
            r.key("scripted_values"); r.begin_array();
            r.value_uint(sv1);
            r.value_uint(sv2);
            r.value_uint(sv3);
            r.end_array();
        }

        r.end_object();
        emitted++;
    }
    r.end_array();
    r.key("count"); r.value_int(emitted);

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_election_state — read election candidates, electors, and votes for a title
// =============================================================================
bool do_query_election_state(const char* params_json, std::string* out) {
    int title_id_param = json_get_int(params_json, "title_id");
    int char_id_param = json_get_int(params_json, "character_id");

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_election_state");

    // If no title_id given, find the primary title of the character
    uint32_t title_id = 0;
    if (title_id_param > 0) {
        title_id = static_cast<uint32_t>(title_id_param) & 0xFFFFFF;
    } else {
        // Resolve character and find their primary (highest-tier) title
        void* character = nullptr;
        if (char_id_param > 0) {
            character = lookup_character(static_cast<uint32_t>(char_id_param));
        } else {
            character = get_controlled_character();
        }
        if (!character || is_sentinel(character)) {
            r.key("error"); r.value_string("character_not_found");
            r.key("hint"); r.value_string("Provide title_id or character_id");
            r.end_object();
            if (out) *out = r.take();
            return false;
        }

        uintptr_t c = reinterpret_cast<uintptr_t>(character);
        uintptr_t title_data = resolve_title_realm(c);
        if (!title_data) {
            r.key("error"); r.value_string("unlanded_character");
            r.end_object();
            if (out) *out = r.take();
            return false;
        }

        // Find highest-tier title with election succession
        uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
        int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (title_arr_count < 0 || title_arr_count > 200) title_arr_count = 0;

        int best_tier = -1;
        for (int32_t i = 0; i < title_arr_count && title_arr_ptr; i++) {
            uint32_t tid = safe_read<uint32_t>(title_arr_ptr + i * sizeof(uint32_t));
            uintptr_t tptr = lookup_title_obj(tid & 0xFFFFFF);
            if (!tptr) continue;
            uint8_t order = title_succ_rule(tptr).order;
            if (order != 1) continue;  // only election titles
            uintptr_t type_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::TITLE_TYPE_PTR);
            if (!type_ptr) continue;
            int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
            if (tier > best_tier) {
                best_tier = tier;
                title_id = tid & 0xFFFFFF;
            }
        }

        if (title_id == 0) {
            r.key("error"); r.value_string("no_elective_title");
            r.key("hint"); r.value_string("Character has no titles with election succession");
            r.end_object();
            if (out) *out = r.take();
            return true;  // not an error, just no election titles
        }
    }

    uintptr_t tobj = lookup_title_obj(title_id);
    if (!tobj) {
        r.key("error"); r.value_string("title_not_found");
        r.key("title_id"); r.value_uint(title_id);
        r.end_object();
        if (out) *out = r.take();
        return false;
    }

    // Emit title info
    r.key("title_id"); r.value_uint(title_id);
    auto display_name = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
    if (!display_name.empty()) {
        r.key("title_name"); r.value_string(display_name.c_str(), display_name.size());
    }
    uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
    if (type_ptr) {
        auto title_key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        if (!title_key.empty()) {
            r.key("title_key"); r.value_string(title_key.c_str(), title_key.size());
        }
        int tier = safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
        r.key("tier"); r.value_int(tier);
    }

    // Succession order for context
    uint8_t succ_order = title_succ_rule(tobj).order;
    const char* order_str = "unknown";
    switch (succ_order) {
        case 0: order_str = "inheritance"; break;
        case 1: order_str = "election"; break;
        case 2: order_str = "appointment"; break;
        case 8: order_str = "noble_family"; break;
        case 9: order_str = "unset"; break;
    }
    r.key("succession_order"); r.value_string(order_str);

    // Holder
    uint32_t holder_id = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID);
    if (holder_id != 0 && holder_id != 0xFFFFFFFF) {
        r.key("holder_id"); r.value_uint(holder_id & 0xFFFFFF);
    }

    // Read election state at title+0x280
    uintptr_t estate_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::ELECTION_STATE_PTR);
    if (!estate_ptr || (!estate_ptr || !is_mapped(estate_ptr))) {
        r.key("election_state"); r.value_string("null");
        r.key("note"); r.value_string("No election state pointer on title");
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    // Read candidate array
    uintptr_t cand_arr = safe_read<uintptr_t>(estate_ptr + ck3::election_state::CANDIDATE_ARRAY);
    int32_t cand_count = safe_read<int32_t>(estate_ptr + ck3::election_state::CANDIDATE_COUNT);
    if (cand_count < 0 || cand_count > 200) cand_count = 0;

    r.key("candidate_count"); r.value_int(cand_count);
    r.key("candidates"); r.begin_array();
    if ((cand_arr && is_mapped(cand_arr))) {
        for (int32_t i = 0; i < cand_count; i++) {
            uint32_t cid = safe_read<uint32_t>(cand_arr + i * 4);
            if (cid == 0 || cid == 0xFFFFFFFF) continue;
            uint32_t clean_cid = cid & 0xFFFFFF;
            r.begin_object();
            r.key("character_id"); r.value_uint(clean_cid);
            void* cchar = lookup_character(cid);
            if (cchar && !is_sentinel(cchar)) {
                std::string cname = ad_read_char_name(reinterpret_cast<uintptr_t>(cchar));
                if (!cname.empty()) {
                    r.key("name"); r.value_string(cname.c_str(), cname.size());
                }
            }
            r.end_object();
        }
    }
    r.end_array();

    // Read elector array
    uintptr_t elec_arr = safe_read<uintptr_t>(estate_ptr + ck3::election_state::ELECTOR_ARRAY);
    int32_t elec_count = safe_read<int32_t>(estate_ptr + ck3::election_state::ELECTOR_COUNT);
    if (elec_count < 0 || elec_count > 200) elec_count = 0;

    r.key("elector_count"); r.value_int(elec_count);
    r.key("electors"); r.begin_array();
    if ((elec_arr && is_mapped(elec_arr))) {
        for (int32_t i = 0; i < elec_count; i++) {
            uint32_t eid = safe_read<uint32_t>(elec_arr + i * 4);
            if (eid == 0 || eid == 0xFFFFFFFF) continue;
            uint32_t clean_eid = eid & 0xFFFFFF;
            r.begin_object();
            r.key("character_id"); r.value_uint(clean_eid);
            void* echar = lookup_character(eid);
            if (echar && !is_sentinel(echar)) {
                std::string ename = ad_read_char_name(reinterpret_cast<uintptr_t>(echar));
                if (!ename.empty()) {
                    r.key("name"); r.value_string(ename.c_str(), ename.size());
                }
            }
            r.end_object();
        }
    }
    r.end_array();

    // Read vote array (SVoteEntry, 12-byte stride)
    uintptr_t vote_arr = safe_read<uintptr_t>(estate_ptr + ck3::election_state::VOTE_ARRAY);
    int32_t vote_count = safe_read<int32_t>(estate_ptr + ck3::election_state::VOTE_COUNT);
    if (vote_count < 0 || vote_count > 500) vote_count = 0;

    r.key("vote_count"); r.value_int(vote_count);
    r.key("votes"); r.begin_array();

    // Also accumulate per-candidate vote totals for prediction
    std::map<uint32_t, int32_t> candidate_scores;

    if ((vote_arr && is_mapped(vote_arr))) {
        for (int32_t i = 0; i < vote_count; i++) {
            uintptr_t entry = vote_arr + i * ck3::vote_entry::STRIDE;
            int32_t voter_ref = safe_read<int32_t>(entry + ck3::vote_entry::VOTER_CHAR_REF);
            int32_t candidate_ref = safe_read<int32_t>(entry + ck3::vote_entry::CANDIDATE_CHAR_REF);
            int32_t weight = safe_read<int32_t>(entry + ck3::vote_entry::VOTE_WEIGHT);

            if (voter_ref <= 0 && candidate_ref <= 0) continue;

            r.begin_object();
            uint32_t voter_cid = static_cast<uint32_t>(voter_ref) & 0xFFFFFF;
            uint32_t cand_cid = static_cast<uint32_t>(candidate_ref) & 0xFFFFFF;

            r.key("voter_id"); r.value_uint(voter_cid);
            r.key("candidate_id"); r.value_uint(cand_cid);
            r.key("weight"); r.value_int(weight);

            // Resolve voter name
            if (voter_ref > 0) {
                void* vc = lookup_character(static_cast<uint32_t>(voter_ref));
                if (vc && !is_sentinel(vc)) {
                    std::string vname = ad_read_char_name(reinterpret_cast<uintptr_t>(vc));
                    if (!vname.empty()) {
                        r.key("voter_name"); r.value_string(vname.c_str(), vname.size());
                    }
                }
            }

            r.end_object();

            // Accumulate scores
            if (candidate_ref > 0) {
                candidate_scores[cand_cid] += weight;
            }
        }
    }
    r.end_array();

    // Emit aggregated prediction (sorted by score descending)
    r.key("prediction"); r.begin_array();
    // Sort by score
    std::vector<std::pair<uint32_t, int32_t>> sorted_scores(
        candidate_scores.begin(), candidate_scores.end());
    std::stable_sort(sorted_scores.begin(), sorted_scores.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& [cid, score] : sorted_scores) {
        r.begin_object();
        r.key("candidate_id"); r.value_uint(cid);
        r.key("total_score"); r.value_int(score);
        void* cc = lookup_character(cid);
        if (cc && !is_sentinel(cc)) {
            std::string cname = ad_read_char_name(reinterpret_cast<uintptr_t>(cc));
            if (!cname.empty()) {
                r.key("name"); r.value_string(cname.c_str(), cname.size());
            }
        }
        r.end_object();
    }
    r.end_array();

    r.end_object();
    if (out) *out = r.take();
    return true;
}


// query_diplomatic_range: check if two characters are within diplomatic range
// SAFE: pure memory reads, no game function calls
bool do_query_diplomatic_range(const char* params_json, std::string* out) {
    int char_a_id = json_get_int(params_json, "character_id_a");
    int char_b_id = json_get_int(params_json, "character_id_b");
    if (char_a_id < 0 || char_b_id < 0) {
        if (out) *out = "{\"error\":\"missing character_id_a or character_id_b\"}";
        return false;
    }

    void* char_a = lookup_character(static_cast<uint32_t>(char_a_id));
    void* char_b = lookup_character(static_cast<uint32_t>(char_b_id));
    if (!char_a || is_sentinel(char_a)) {
        if (out) *out = "{\"error\":\"character_a not found\"}";
        return false;
    }
    if (!char_b || is_sentinel(char_b)) {
        if (out) *out = "{\"error\":\"character_b not found\"}";
        return false;
    }

    // Helper lambda: resolve character → capital barony → province → template coordinates
    // For unlanded characters, walk up the liege chain to find a landed character.
    auto get_capital_coords = [](void* character, int32_t& out_x, int32_t& out_y,
                                 uint32_t& out_prov_id, uint32_t& out_barony_id) -> bool {
        uintptr_t c = reinterpret_cast<uintptr_t>(character);

        // Walk up liege chain for unlanded characters (max 10 hops to prevent infinite loops)
        for (int hops = 0; hops < 10; hops++) {
            uintptr_t title_realm = resolve_title_realm(c);
            if (title_realm) {
                uint32_t barony_tid = safe_read<uint32_t>(title_realm + ck3::title_realm::CAPITAL_BARONY_ID);
                if (barony_tid != 0 && barony_tid != 0xFFFFFFFF) {
                    uintptr_t barony = lookup_title_obj(barony_tid);
                    if (barony) {
                        uintptr_t province = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
                        if ((province && is_mapped(province))) {
                            uintptr_t tmpl = safe_read<uintptr_t>(province + ck3::province::TEMPLATE_PTR);
                            if ((tmpl && is_mapped(tmpl))) {
                                out_x = safe_read<int32_t>(tmpl + ck3::province_template::CENTER_X);
                                out_y = safe_read<int32_t>(tmpl + ck3::province_template::CENTER_Y);
                                out_prov_id = safe_read<uint32_t>(tmpl + ck3::province_template::PROVINCE_ID);
                                out_barony_id = barony_tid & 0xFFFFFF;
                                return true;
                            }
                        }
                    }
                }
                // Capital-barony path failed (e.g. administrative "minister"
                // office-holders whose primary title is an office with no capital
                // barony — capital_barony_id is null). FALLBACK (20260614): scan the
                // char's held titles for ANY barony (tier 1) with a valid
                // province→template, and use that as the location proxy.
                uintptr_t held_arr = safe_read<uintptr_t>(title_realm + ck3::title_realm::TITLE_ARRAY_PTR);
                int32_t held_cnt = safe_read<int32_t>(title_realm + ck3::title_realm::TITLE_ARRAY_COUNT);
                if (held_cnt < 0 || held_cnt > 500) held_cnt = 0;
                if (held_arr && is_mapped(held_arr)) {
                    for (int32_t i = 0; i < held_cnt; i++) {
                        uint32_t htid = safe_read<uint32_t>(held_arr + i * 4);
                        if (htid == 0 || htid == 0xFFFFFFFF) continue;
                        uintptr_t hobj = lookup_title_obj(htid);
                        if (!hobj) continue;
                        uintptr_t hprov = safe_read<uintptr_t>(hobj + ck3::title_obj::PROVINCE_PTR);
                        if (!hprov || !is_mapped(hprov)) continue;  // only barony-tier titles have a province
                        uintptr_t htmpl = safe_read<uintptr_t>(hprov + ck3::province::TEMPLATE_PTR);
                        if (!htmpl || !is_mapped(htmpl)) continue;
                        out_x = safe_read<int32_t>(htmpl + ck3::province_template::CENTER_X);
                        out_y = safe_read<int32_t>(htmpl + ck3::province_template::CENTER_Y);
                        out_prov_id = safe_read<uint32_t>(htmpl + ck3::province_template::PROVINCE_ID);
                        out_barony_id = htid & 0xFFFFFF;
                        return true;
                    }
                }
                // Still nothing — try liege via title_realm.
                uintptr_t liege_ptr = safe_read<uintptr_t>(title_realm + ck3::title_realm::LIEGE_PTR);
                if ((liege_ptr && is_mapped(liege_ptr))) {
                    c = reinterpret_cast<uintptr_t>(liege_ptr);
                    continue;
                }
                return false;
            }

            // Unlanded (courtier): resolve liege via courtier_data+0xC8 packed char ref
            uintptr_t courtier_data = safe_read<uintptr_t>(c + ck3::character::COURTIER_DATA);
            if ((courtier_data && is_mapped(courtier_data))) {
                uint32_t liege_ref = safe_read<uint32_t>(courtier_data + ck3::courtier_data::LIEGE_CHAR_REF);
                if (liege_ref != 0 && liege_ref != 0xFFFFFFFF) {
                    void* liege_char = lookup_character(liege_ref);
                    if (liege_char && !is_sentinel(liege_char)) {
                        c = reinterpret_cast<uintptr_t>(liege_char);
                        continue;
                    }
                }
            }
            return false;
        }
        return false;
    };

    int32_t ax = 0, ay = 0, bx = 0, by = 0;
    uint32_t a_prov_id = 0, b_prov_id = 0;
    uint32_t a_barony_id = 0, b_barony_id = 0;

    bool got_a = get_capital_coords(char_a, ax, ay, a_prov_id, a_barony_id);
    bool got_b = get_capital_coords(char_b, bx, by, b_prov_id, b_barony_id);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_diplomatic_range");
    r.key("character_id_a"); r.value_uint(static_cast<uint32_t>(char_a_id));
    r.key("character_id_b"); r.value_uint(static_cast<uint32_t>(char_b_id));

    if (!got_a || !got_b) {
        r.key("error"); r.value_string(!got_a ? "could not resolve capital for character_a"
                                              : "could not resolve capital for character_b");
        r.key("in_range"); r.value_bool(false);
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    int64_t dx = static_cast<int64_t>(ax) - static_cast<int64_t>(bx);
    int64_t dy = static_cast<int64_t>(ay) - static_cast<int64_t>(by);
    int64_t dist_sq = dx * dx + dy * dy;
    double distance = 0.0;
    if (dist_sq > 0) {
        distance = std::sqrt(static_cast<double>(dist_sq));
    }
    bool in_range = dist_sq <= static_cast<int64_t>(ck3::diplomatic_range::MAXIMUM_RANGE_SQUARED);

    r.key("in_range");          r.value_bool(in_range);
    r.key("distance");          r.value_int(static_cast<int>(distance + 0.5));
    r.key("distance_squared");  r.value_int(static_cast<int>(dist_sq));
    r.key("threshold");         r.value_int(ck3::diplomatic_range::MAXIMUM_RANGE);

    r.key("capital_a"); r.begin_object();
    r.key("province_id"); r.value_uint(a_prov_id);
    r.key("barony_id");   r.value_uint(a_barony_id);
    r.key("x");           r.value_int(ax);
    r.key("y");           r.value_int(ay);
    r.end_object();

    r.key("capital_b"); r.begin_object();
    r.key("province_id"); r.value_uint(b_prov_id);
    r.key("barony_id");   r.value_uint(b_barony_id);
    r.key("x");           r.value_int(bx);
    r.key("y");           r.value_int(by);
    r.end_object();

    r.end_object();
    if (out) *out = r.take();
    return true;
}


// query_dynasty: get dynasty details by dynasty_id
// SAFE: pure memory reads, no game function calls
bool do_query_dynasty(const char* params_json, std::string* out) {
    int dynasty_id = json_get_int(params_json, "dynasty_id");
    if (dynasty_id < 0) {
        // Fallback: resolve from controlled character's dynasty
        void* ctrl = get_controlled_character();
        if (ctrl && !is_sentinel(ctrl)) {
            uintptr_t family = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(ctrl) + ck3::character::FAMILY_DATA);
            if (family && is_mapped(family)) {
                uint32_t dyn = safe_read<uint32_t>(family + ck3::family::DYNASTY_ID);
                if (dyn != 0 && dyn != 0xFFFFFFFF) {
                    dynasty_id = static_cast<int>(dyn & 0xFFFFFF);
                }
            }
            // FIX I12 (#dynasty_fallback): derive dynasty_id from the house when family+0x10 is unset.
            if (dynasty_id < 0) {
                uint32_t house_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(ctrl) + ck3::character::HOUSE_ID_DIRECT);
                if (house_id != 0 && house_id != 0xFFFFFFFF) {
                    uintptr_t hobj = ad_db_lookup(ck3::globals::HOUSE_DB, house_id & 0xFFFFFF);
                    if (hobj && is_mapped(hobj)) {
                        uint32_t d = safe_read<uint32_t>(hobj + dynasty_house_obj::DYNASTY_ID);
                        if (d != 0 && d != 0xFFFFFFFF) dynasty_id = static_cast<int>(d & 0xFFFFFF);
                    }
                }
            }
        }
        if (dynasty_id < 0) {
            if (out) *out = "{\"error\":\"missing dynasty_id and no controlled character dynasty\"}";
            return false;
        }
    }

    uintptr_t dynasty = ad_db_lookup(ck3::globals::DYNASTY_DB, static_cast<uint32_t>(dynasty_id));
    if (!dynasty || !is_mapped(dynasty)) {
        if (out) *out = "{\"error\":\"dynasty not found\"}";
        return false;
    }

    // Check sentinel
    uintptr_t sentinel = safe_read_offset<uintptr_t>(ck3::globals::DYNASTY_DB_SENTINEL);
    if (dynasty == sentinel) {
        if (out) *out = "{\"error\":\"dynasty is sentinel (null dynasty)\"}";
        return false;
    }

    ResponseBuilder r;
    r.begin_object();

    uint32_t id = safe_read<uint32_t>(dynasty + dynasty_obj::ID) & 0xFFFFFF;
    r.key("dynasty_id"); r.value_uint(id);

    // Dynasty name at +0x80 (HIGH confidence from decompilation of Dynasty_GetName)
    std::string name = ad_read_std_string(dynasty + dynasty_obj::NAME);
    if (!name.empty()) {
        r.key("name"); r.value_string(name.c_str(), name.size());
    } else {
        // Fallback: try custom name if has_custom_name flag is set
        uint8_t has_custom = safe_read<uint8_t>(dynasty + dynasty_obj::HAS_CUSTOM_NAME);
        if (has_custom) {
            std::string custom = ad_read_std_string(dynasty + dynasty_obj::CUSTOM_NAME);
            if (!custom.empty()) {
                r.key("name"); r.value_string(custom.c_str(), custom.size());
            } else {
                r.key("name"); r.value_string("(unknown)", 9);
            }
        } else {
            r.key("name"); r.value_string("(unknown)", 9);
        }
    }

    // Head character ID
    // FIX I13: emit null (not the 0xFFFFFF sentinel) for headless/extinct dynasties,
    // plus an additive head_valid flag as a cheap extinct/defunct signal.
    uint32_t head_raw = safe_read<uint32_t>(dynasty + dynasty_obj::HEAD_CHAR_ID);
    uint32_t head_id  = head_raw & 0xFFFFFF;
    bool head_valid = (head_raw != 0xFFFFFFFF && head_id != 0xFFFFFF && head_id != 0);
    r.key("head_character_id");
    if (head_valid) r.value_uint(head_id); else r.value_null();
    r.key("head_valid"); r.value_bool(head_valid);

    // Prestige (renown) — int64 fixed-point / 100000
    int64_t prestige_raw = safe_read<int64_t>(dynasty + dynasty_obj::PRESTIGE);
    r.key("prestige"); r.value_float(static_cast<double>(prestige_raw) / 100000.0);

    // Prestige level (0-10)
    uint32_t prestige_level = safe_read<uint32_t>(dynasty + dynasty_obj::PRESTIGE_LEVEL);
    if (prestige_level > 10) prestige_level = 0;  // sanity clamp
    r.key("prestige_level"); r.value_uint(prestige_level);

    // Legacy perks count
    int32_t perks_count = safe_read<int32_t>(dynasty + dynasty_obj::LEGACY_PERKS_COUNT);
    if (perks_count < 0 || perks_count > 100) perks_count = 0;  // sanity clamp
    r.key("legacy_perks_count"); r.value_int(perks_count);

    // FIX #dynasty_house: legacies[] — walk LEGACY_PERKS_ARRAY (array of 8-byte perk ptrs)
    // and surface each unlocked perk by key, parsed into track + rank. Plus a tracks{} aggregate
    // (track -> max rank). Pure reads, guarded; matches the existing safety class.
    {
        uintptr_t perks_ptr = safe_read<uintptr_t>(dynasty + dynasty_obj::LEGACY_PERKS_ARRAY);
        std::map<std::string, int> track_max;
        r.key("legacies"); r.begin_array();
        if (perks_ptr && is_mapped(perks_ptr) && perks_count > 0) {
            for (int32_t i = 0; i < perks_count; i++) {
                uintptr_t perk_obj = safe_read<uintptr_t>(perks_ptr + static_cast<uintptr_t>(i) * 8);
                if (!perk_obj || !is_mapped(perk_obj)) continue;
                std::string key = ad_read_std_string(perk_obj + dynasty_legacy_perk::NAME_KEY);
                if (key.empty()) continue;
                // Parse '<track>_legacy_<N>' — find last "_legacy_".
                std::string track; int rank = 0; bool parsed = false;
                size_t pos = key.rfind("_legacy_");
                if (pos != std::string::npos) {
                    track = key.substr(0, pos);
                    std::string suf = key.substr(pos + 8);
                    if (!suf.empty()) {
                        rank = atoi(suf.c_str());
                        parsed = (rank > 0);
                    }
                }
                r.begin_object();
                r.key("perk_key"); r.value_string(key.c_str(), key.size());
                if (parsed) {
                    r.key("track"); r.value_string(track.c_str(), track.size());
                    r.key("rank"); r.value_int(rank);
                    if (rank > track_max[track]) track_max[track] = rank;
                } else {
                    r.key("track"); r.value_null();
                    r.key("rank"); r.value_null();
                }
                r.end_object();
            }
        }
        r.end_array();
        r.key("tracks"); r.begin_object();
        for (const auto& kv : track_max) {
            r.key(kv.first.c_str()); r.value_int(kv.second);
        }
        r.end_object();
    }

    // Houses count
    int32_t houses_count = safe_read<int32_t>(dynasty + dynasty_obj::HOUSES_COUNT);
    if (houses_count < 0 || houses_count > 1000) houses_count = 0;  // sanity clamp
    r.key("houses_count"); r.value_int(houses_count);

    // Head character name (if resolvable)
    if (head_id != 0 && head_id != 0xFFFFFF) {
        void* head_char = lookup_character(head_id);
        if (head_char && !is_sentinel(head_char)) {
            std::string head_name = ad_read_char_name(reinterpret_cast<uintptr_t>(head_char));
            if (!head_name.empty()) {
                r.key("head_character_name"); r.value_string(head_name.c_str(), head_name.size());
            }
        }
    }

    r.end_object();

    if (out) *out = r.take();
    return true;
}


// ---------------------------------------------------------------------------
// query_levy_readiness: focused levy/military readiness assessment
// ---------------------------------------------------------------------------
bool do_query_levy_readiness(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query_levy_readiness: character not found\n");
        return false;
    }
    uint32_t char_id = char_get_id(character);

    ResponseBuilder r;
    r.begin_object();
    r.key("character_id"); r.value_uint(char_id & 0xFFFFFF);

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);

    int32_t mil_current = 0, mil_max = 0, mil_hireable = 0;
    int32_t mil_without_hires = 0, mil_own_domain_max = 0;
    int64_t quality_current = 0, quality_max = 0;

    if (tr && is_mapped(tr)) {
        mil_current       = safe_read<int32_t>(tr + ck3::title_realm::MIL_CURRENT);
        mil_max           = safe_read<int32_t>(tr + ck3::title_realm::MIL_MAX);
        mil_hireable      = safe_read<int32_t>(tr + ck3::title_realm::MIL_HIREABLE);
        mil_without_hires = safe_read<int32_t>(tr + ck3::title_realm::MIL_WITHOUT_HIRES);
        mil_own_domain_max = safe_read<int32_t>(tr + ck3::title_realm::MIL_OWN_DOMAIN_MAX);
        quality_current   = safe_read<int64_t>(tr + ck3::title_realm::MIL_QUALITY_CURRENT_I64);
        quality_max       = safe_read<int64_t>(tr + ck3::title_realm::MIL_QUALITY_MAX_I64);
    }

    r.key("military_current");      r.value_int(mil_current);
    r.key("military_max");          r.value_int(mil_max);
    r.key("military_hireable");     r.value_int(mil_hireable);
    r.key("military_without_hires"); r.value_int(mil_without_hires);
    r.key("levy_own_domain_max");   r.value_int(mil_own_domain_max);
    r.key("quality_current");       r.value_int(quality_current);
    r.key("quality_max");           r.value_int(quality_max);

    // Computed: readiness percentage (clamped 0-100)
    int readiness_pct = 0;
    if (mil_max > 0) {
        readiness_pct = static_cast<int>((static_cast<int64_t>(mil_current) * 100) / mil_max);
        if (readiness_pct < 0) readiness_pct = 0;
        if (readiness_pct > 100) readiness_pct = 100;
    }
    r.key("readiness_pct"); r.value_int(readiness_pct);

    // Base reinforcement rate: 15% of max per month (CK3 game define)
    r.key("reinforcement_rate_base_pct"); r.value_int(15);

    // Estimated days to full: (max - current) * 30 / (max * 0.15)
    // Simplified: (max - current) * 200 / max
    int est_days = 0;
    if (mil_max > 0 && mil_current < mil_max) {
        est_days = static_cast<int>((static_cast<int64_t>(mil_max - mil_current) * 200) / mil_max);
    }
    r.key("est_days_to_full"); r.value_int(est_days);

    // Per-regiment MaA breakdown: iterate personal + title MaA regiment arrays.
    // FIX 2026-06-19: the MaA regiment arrays (PERSONAL_MAA_PTR 0x108, TITLE_MAA_REG_PTR
    // 0x2A8, ...) are sub-offsets of TITLE_REALM_DATA (0x1b8), NOT REALM_DATA (0x1a8).
    // The prior base (REALM_DATA) read count=0 → recruited MaA never surfaced (no
    // regiment_id for stationing). Verified live: char+0x1b8+0x108 count=5 with valid refs.
    uintptr_t ruler_data = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    r.key("regiments");
    r.begin_array();
    int reg_emitted = 0;

    if (ruler_data && is_mapped(ruler_data)) {
        // Regiment arrays to scan: personal (0x108), title (0x2A8)
        struct RegSource {
            size_t offset;
            const char* source_name;
        };
        RegSource sources[] = {
            {ck3::title_realm::PERSONAL_MAA_PTR, "personal"},
            {ck3::title_realm::TITLE_MAA_REG_PTR, "title"},
            {ck3::title_realm::MERC_REGIMENT_PTR, "mercenary"},
            {ck3::title_realm::HOLY_ORDER_REG_PTR, "holy_order"},
        };

        for (auto& src : sources) {
            uintptr_t arr_ptr = safe_read<uintptr_t>(ruler_data + src.offset);
            int32_t arr_count = safe_read<int32_t>(ruler_data + src.offset + 0x0C);
            if (arr_count <= 0 || arr_count > 500 || !arr_ptr || !is_mapped(arr_ptr)) continue;

            for (int32_t i = 0; i < arr_count; ++i) {
                uint32_t reg_ref = safe_read<uint32_t>(arr_ptr + i * 4);
                if (reg_ref == 0 || reg_ref == 0xFFFFFFFF) continue;

                uintptr_t reg_ptr = ad_db_lookup(ck3::globals::REGIMENT_DB, reg_ref);
                if (!reg_ptr || !is_mapped(reg_ptr)) continue;

                int32_t max_count = safe_read<int32_t>(reg_ptr + ::regiment::MAX_COUNT);
                if (max_count <= 0) continue;

                // Count alive men using SMaAChunk GetCurrentCount logic
                int32_t alive = 0;
                for (int ch = 0; ch < static_cast<int>(::regiment::CHUNK_COUNT); ++ch) {
                    uintptr_t chunk = reg_ptr + ::regiment::CHUNK_BASE
                                    + ch * ::regiment::CHUNK_STRIDE;
                    int32_t allocated = safe_read<int32_t>(chunk + ::maa_chunk::ALLOCATED_COUNT);
                    int32_t current   = safe_read<int32_t>(chunk + ::maa_chunk::CURRENT_COUNT);
                    int32_t status    = safe_read<int32_t>(chunk + ::maa_chunk::STATUS_FLAG);
                    if (status != 3 || current != 0)
                        alive += current;
                    else
                        alive += allocated;
                }

                // Get MaA type name
                uintptr_t type_ptr = safe_read<uintptr_t>(reg_ptr + ::regiment::MAA_TYPE_PTR);
                const char* type_name = nullptr;
                size_t type_name_len = 0;
                if (type_ptr && is_mapped(type_ptr)) {
                    read_db_obj_name(type_ptr, type_name, type_name_len);
                }

                r.begin_object();
                r.key("regiment_id"); r.value_uint(reg_ref & 0xFFFFFF);
                r.key("source"); r.value_string(src.source_name);
                if (type_name && type_name_len > 0) {
                    r.key("type"); r.value_string(type_name, type_name_len);
                } else {
                    r.key("type"); r.value_string("levy");
                }
                r.key("current"); r.value_int(alive);
                r.key("max"); r.value_int(max_count);
                int pct = static_cast<int>((static_cast<int64_t>(alive) * 100) / max_count);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                r.key("strength_pct"); r.value_int(pct);
                // Stationing (playbook: MaA must be STATIONED in their bonus holding).
                uint32_t station_tid = safe_read<uint32_t>(reg_ptr + ::regiment::STATION_TITLE_ID) & 0xFFFFFF;
                if (station_tid != 0 && station_tid != 0xFFFFFF) {
                    r.key("stationed_title_id"); r.value_uint(station_tid);
                    uintptr_t st = ad_db_lookup(ck3::globals::LANDED_TITLE_DB, station_tid);
                    if (st && is_mapped(st)) {
                        std::string sn = ad_read_std_string(st + ck3::title_obj::TITLE_DISPLAY_NAME);
                        if (!sn.empty()) { r.key("stationed_title"); r.value_string(sn.c_str(), sn.size()); }
                    }
                } else {
                    r.key("stationed"); r.value_bool(false);  // mobile / not stationed
                }
                // Maintenance gold per stack (× alive stacks ≈ total upkeep).
                if (type_ptr && is_mapped(type_ptr)) {
                    int64_t maint_fp = safe_read<int64_t>(type_ptr + ::maa_type::MAINT_GOLD_PER_STACK);
                    r.key("maint_gold_per_stack"); r.value_float(static_cast<float>(maint_fp) / 100000.0f);
                }
                r.end_object();
                ++reg_emitted;
            }
        }
    }

    r.end_array();
    r.key("regiment_count"); r.value_int(reg_emitted);

    r.end_object();
    *out = r.take();
    return true;
}


// =============================================================================
// query_partition_preview: predict post-death title distribution for a character
// Simulates the tier-descending greedy partition algorithm mapped in
// re/findings/re_succession_partition.md.
//
// Returns {heir_id: [title_ids]} mapping showing which heir gets which titles.
// Uses the primary title's heir list and iterates held titles tier-descending.
// =============================================================================
bool do_query_partition_preview(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = "{\"error\":\"character not found\"}";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;

    // Get title_realm data
    uintptr_t title_data = resolve_title_realm(c);
    if (!title_data || !is_mapped(title_data)) {
        if (out) *out = "{\"error\":\"character is unlanded\"}";
        return false;
    }

    // Get held title array
    uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (title_arr_count < 0 || title_arr_count > 500) title_arr_count = 0;

    if (!title_arr_ptr || !is_mapped(title_arr_ptr) || title_arr_count == 0) {
        if (out) *out = "{\"error\":\"no held titles\"}";
        return false;
    }

    // Collect held titles with their info (skip baronies, tier <= 1)
    struct TitleInfo {
        uint32_t id;
        uintptr_t ptr;
        int tier;
        uint32_t de_jure_parent;
        uint8_t division;
    };
    std::vector<TitleInfo> held_titles;
    held_titles.reserve(title_arr_count);

    int best_tier = -1;
    uint32_t primary_title_id = 0;
    uintptr_t primary_title_ptr = 0;

    for (int32_t i = 0; i < title_arr_count; i++) {
        uint32_t tid = safe_read<uint32_t>(title_arr_ptr + i * sizeof(uint32_t));
        uintptr_t tptr = lookup_title_obj(tid);
        if (!tptr || !is_mapped(tptr)) continue;

        int tier = get_title_tier(tptr);
        if (tier <= 1) continue;  // skip baronies

        uint32_t dejure = safe_read<uint32_t>(tptr + ck3::title_obj::DE_JURE_PARENT);
        uint8_t division = title_succ_rule(tptr).division;
        uint8_t skip_flag = safe_read<uint8_t>(tptr + ck3::title_obj::TITLE_SKIP_FLAG_2D);
        if (skip_flag) continue;

        held_titles.push_back({tid & 0xFFFFFF, tptr, tier, dejure & 0xFFFFFF, division});

        if (tier > best_tier) {
            best_tier = tier;
            primary_title_id = tid & 0xFFFFFF;
            primary_title_ptr = tptr;
        }
    }

    if (!primary_title_ptr) {
        if (out) *out = "{\"error\":\"no primary title found\"}";
        return false;
    }

    // Check if primary title uses partition
    uint8_t primary_division = title_succ_rule(primary_title_ptr).division;
    bool is_partition = (primary_division == 1);

    // Get heir list from primary title
    uintptr_t heir_arr_ptr = safe_read<uintptr_t>(primary_title_ptr + ck3::title_obj::SUCCESSION_HEIRS);
    int32_t heir_count = safe_read<int32_t>(primary_title_ptr + ck3::title_obj::SUCCESSION_HEIR_COUNT);
    if (heir_count < 0) heir_count = 0;
    if (heir_count > 50) heir_count = 50;

    struct HeirInfo {
        uint32_t char_id;
        std::string name;
    };
    std::vector<HeirInfo> heirs;

    if (heir_arr_ptr && is_mapped(heir_arr_ptr)) {
        for (int32_t hi = 0; hi < heir_count; hi++) {
            uint32_t href = safe_read<uint32_t>(heir_arr_ptr + static_cast<uintptr_t>(hi) * 4);
            uint32_t hcid = href & 0xFFFFFF;
            if (hcid == 0 || hcid == 0xFFFFFF) continue;

            void* heir_char = lookup_character(href);
            if (!heir_char || is_sentinel(heir_char)) continue;

            std::string hname = ad_read_char_name(reinterpret_cast<uintptr_t>(heir_char));
            heirs.push_back({hcid, std::move(hname)});
        }
    }

    // Build the result
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_partition_preview");
    r.key("character_id"); r.value_uint(char_id);
    r.key("primary_title_id"); r.value_uint(primary_title_id);
    r.key("primary_title_tier"); r.value_int(best_tier);
    r.key("is_partition"); r.value_bool(is_partition);
    r.key("heir_count"); r.value_int(static_cast<int>(heirs.size()));
    r.key("title_count"); r.value_int(static_cast<int>(held_titles.size()));

    if (heirs.empty()) {
        r.key("error"); r.value_string("no heirs found on primary title");
        r.key("distribution"); r.begin_object(); r.end_object();
        r.end_object();
        if (out) *out = r.take();
        return true;
    }

    // --- Partition simulation ---
    std::map<uint32_t, std::vector<uint32_t>> distribution;
    for (auto& h : heirs) distribution[h.char_id] = {};

    if (!is_partition || heirs.size() <= 1) {
        // Single heir or non-partition: all titles go to primary heir
        for (auto& t : held_titles) {
            distribution[heirs[0].char_id].push_back(t.id);
        }
    } else {
        // Partition: tier-descending greedy with de jure scoring
        uint32_t primary_heir = heirs[0].char_id;
        std::set<uint32_t> assigned;

        // Pre-assign primary title
        distribution[primary_heir].push_back(primary_title_id);
        assigned.insert(primary_title_id);

        // Pre-assign de jure children of primary title to primary heir
        if (primary_title_ptr) {
            int32_t djc_count = safe_read<int32_t>(primary_title_ptr + ck3::title_obj::DE_JURE_CHILDREN_COUNT);
            uintptr_t djc_arr = safe_read<uintptr_t>(primary_title_ptr + ck3::title_obj::DE_JURE_CHILDREN_ARRAY);
            if (djc_count > 0 && djc_count < 200 && djc_arr && is_mapped(djc_arr)) {
                for (int32_t di = 0; di < djc_count; di++) {
                    uint32_t child_tid = safe_read<uint32_t>(djc_arr + static_cast<uintptr_t>(di) * 4) & 0xFFFFFF;
                    for (auto& t : held_titles) {
                        if (t.id == child_tid && assigned.find(child_tid) == assigned.end()) {
                            distribution[primary_heir].push_back(child_tid);
                            assigned.insert(child_tid);
                            break;
                        }
                    }
                }
            }
        }

        // Read de jure scoring weights
        int32_t own_weight = safe_read<int32_t>(bridge::game_fn(ck3::funcs::PARTITION_DEJURE_OWN_WEIGHT));
        int32_t other_weight = safe_read<int32_t>(bridge::game_fn(ck3::funcs::PARTITION_DEJURE_OTHER_WEIGHT));
        if (own_weight == 0) own_weight = 100;
        if (other_weight == 0) other_weight = 50;

        auto is_dejure_related = [&](uint32_t title_a, uint32_t title_b) -> bool {
            for (auto& t : held_titles) {
                if (t.id == title_a && t.de_jure_parent == title_b) return true;
                if (t.id == title_b && t.de_jure_parent == title_a) return true;
            }
            uintptr_t ptr_a = lookup_title_obj(title_a);
            uintptr_t ptr_b = lookup_title_obj(title_b);
            if (ptr_a && is_mapped(ptr_a) && ptr_b && is_mapped(ptr_b)) {
                uint32_t parent_a = safe_read<uint32_t>(ptr_a + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                uint32_t parent_b = safe_read<uint32_t>(ptr_b + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                if (parent_a == title_b || parent_b == title_a) return true;
                if (parent_a != 0 && parent_a == parent_b) return true;
            }
            return false;
        };

        // Tier-descending distribution
        for (int tier = best_tier; tier >= 2; tier--) {
            std::vector<uint32_t> unassigned_at_tier;
            for (auto& t : held_titles) {
                if (t.tier == tier && assigned.find(t.id) == assigned.end()) {
                    unassigned_at_tier.push_back(t.id);
                }
            }
            if (unassigned_at_tier.empty()) continue;

            std::map<uint32_t, int> heir_title_count;
            for (auto& h : heirs) {
                heir_title_count[h.char_id] = 0;
                for (auto& tid : distribution[h.char_id]) {
                    for (auto& t : held_titles) {
                        if (t.id == tid && t.tier == tier) {
                            heir_title_count[h.char_id]++;
                            break;
                        }
                    }
                }
            }

            while (!unassigned_at_tier.empty()) {
                // Pick least-titled heir
                uint32_t chosen_heir = heirs[0].char_id;
                int min_count = heir_title_count[chosen_heir];
                for (size_t hi = 1; hi < heirs.size(); hi++) {
                    int cnt = heir_title_count[heirs[hi].char_id];
                    if (cnt < min_count) {
                        min_count = cnt;
                        chosen_heir = heirs[hi].char_id;
                    }
                }

                // Score each unassigned title for chosen heir
                int best_score = -999999;
                size_t best_idx = 0;
                for (size_t ti = 0; ti < unassigned_at_tier.size(); ti++) {
                    uint32_t tid = unassigned_at_tier[ti];
                    int own_matches = 0, other_matches = 0;

                    for (auto& atid : distribution[chosen_heir]) {
                        if (is_dejure_related(tid, atid)) own_matches++;
                    }
                    for (auto& h : heirs) {
                        if (h.char_id == chosen_heir) continue;
                        for (auto& atid : distribution[h.char_id]) {
                            if (is_dejure_related(tid, atid)) other_matches++;
                        }
                    }

                    int score = own_matches * own_weight - other_matches * other_weight;
                    if (score > best_score) {
                        best_score = score;
                        best_idx = ti;
                    }
                }

                // Assign best title to chosen heir
                uint32_t assigned_tid = unassigned_at_tier[best_idx];
                distribution[chosen_heir].push_back(assigned_tid);
                assigned.insert(assigned_tid);
                heir_title_count[chosen_heir]++;
                unassigned_at_tier.erase(unassigned_at_tier.begin() + best_idx);

                // Cascade: assign de jure children to same heir
                uintptr_t atptr = lookup_title_obj(assigned_tid);
                if (atptr) {
                    int32_t djc_count = safe_read<int32_t>(atptr + ck3::title_obj::DE_JURE_CHILDREN_COUNT);
                    uintptr_t djc_arr = safe_read<uintptr_t>(atptr + ck3::title_obj::DE_JURE_CHILDREN_ARRAY);
                    if (djc_count > 0 && djc_count < 200 && djc_arr && djc_arr >= 0x10000) {
                        for (int32_t di = 0; di < djc_count; di++) {
                            uint32_t child_tid = safe_read<uint32_t>(djc_arr + static_cast<uintptr_t>(di) * 4) & 0xFFFFFF;
                            for (auto it = unassigned_at_tier.begin(); it != unassigned_at_tier.end(); ++it) {
                                if (*it == child_tid) {
                                    distribution[chosen_heir].push_back(child_tid);
                                    assigned.insert(child_tid);
                                    heir_title_count[chosen_heir]++;
                                    unassigned_at_tier.erase(it);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Emit distribution
    r.key("distribution"); r.begin_object();
    for (auto& h : heirs) {
        auto& titles = distribution[h.char_id];
        if (titles.empty()) continue;

        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "%u", h.char_id);
        r.key(key_buf); r.begin_object();
        r.key("name"); r.value_string(h.name.c_str(), h.name.size());
        r.key("is_primary_heir"); r.value_bool(h.char_id == heirs[0].char_id);
        r.key("title_count"); r.value_int(static_cast<int>(titles.size()));
        r.key("titles"); r.begin_array();
        for (auto tid : titles) {
            r.begin_object();
            r.key("title_id"); r.value_uint(tid);
            uintptr_t tptr = lookup_title_obj(tid);
            if (tptr) {
                r.key("tier"); r.value_int(get_title_tier(tptr));
                uintptr_t type_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::TITLE_TYPE_PTR);
                if (type_ptr) {
                    std::string tname = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                    if (!tname.empty()) {
                        r.key("key"); r.value_string(tname.c_str(), tname.size());
                    }
                }
                std::string dname = ad_read_std_string(tptr + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!dname.empty()) {
                    r.key("name"); r.value_string(dname.c_str(), dname.size());
                }
            }
            r.end_object();
        }
        r.end_array();
        r.end_object();
    }
    r.end_object();

    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}



// =============================================================================
// query_innovation_progress — Read innovation spread progress for a culture (ck-sdy2)
// Takes culture_id (from character's culture) or character_id (resolves culture).
// Returns per-innovation progress array with name, progress %, and known status.
// =============================================================================
bool do_query_innovation_progress(const char* params_json, std::string* out) {
    int culture_id_param = json_get_int(params_json, "culture_id");
    uintptr_t culture_obj = 0;

    if (culture_id_param > 0) {
        culture_obj = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB,
                                   static_cast<uint32_t>(culture_id_param));
    } else {
        void* character = resolve_character(params_json);
        if (!character || is_sentinel(character)) {
            if (out) *out = R"({"error":"character not found"})";
            return false;
        }
        uintptr_t c = reinterpret_cast<uintptr_t>(character);
        uint32_t cid = safe_read<uint32_t>(c + ck3::character::CULTURE_ID_DIRECT);
        if (cid == 0 || cid == 0xFFFFFFFF) {
            if (out) *out = R"({"error":"character has no culture"})";
            return false;
        }
        culture_obj = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, cid);
        culture_id_param = static_cast<int>(cid & 0xFFFFFF);
    }

    if ((!culture_obj || !is_mapped(culture_obj))) {
        if (out) *out = R"({"error":"culture not found"})";
        return false;
    }

    // FIX #culture_name: read NAME (+0x260) not +0x10 (uint32 ID); add culture_key (+0x1C8).
    std::string culture_name = ad_read_std_string(culture_obj + culture::NAME);
    std::string culture_key  = ad_read_std_string(culture_obj + culture::SCRIPT_KEY);

    // FIX #innovations: build an era index->name map (mirrors do_query_fascination_progress
    // walk). Used to label per-innovation era and the era_summary. era_name_for() returns
    // "regional" for era_index >= era_count (e.g. MaA innovations carry era_index=4).
    std::string era_names[8];
    int era_name_count = 0;
    {
        uintptr_t era_ptr0 = safe_read<uintptr_t>(culture_obj + culture::ERA_ENTRIES_PTR);
        int32_t ec = safe_read<int32_t>(culture_obj + culture::ERA_ENTRIES_COUNT);
        if (ec < 0) ec = 0; if (ec > 8) ec = 8;
        if (era_ptr0 && is_mapped(era_ptr0)) {
            for (int i = 0; i < ec; i++) {
                uintptr_t entry = era_ptr0 + static_cast<uintptr_t>(i) * culture_era::STRIDE;
                if (!entry || !is_mapped(entry)) break;
                uintptr_t et = safe_read<uintptr_t>(entry + culture_era::ERA_TYPE_PTR);
                if (et && is_mapped(et)) era_names[i] = ad_read_std_string(et + innovation_type::NAME_KEY);
                era_name_count = i + 1;
            }
        }
    }
    auto era_name_for = [&](int32_t idx) -> std::string {
        if (idx >= 0 && idx < era_name_count && !era_names[idx].empty()) return era_names[idx];
        return "regional";  // era_index >= era_count (MaA/regional innovations)
    };

    // Innovation flat array: base at culture + INNOVATION_ARRAY, stride = 0x210
    uintptr_t innov_array = safe_read<uintptr_t>(culture_obj + culture::INNOVATION_ARRAY);

    // Known innovations CPdxArray: data_ptr at +0x00, count at +0x0C
    uintptr_t known_data = safe_read<uintptr_t>(culture_obj + culture::KNOWN_INNOVATIONS);
    int32_t known_count = safe_read<int32_t>(culture_obj + culture::KNOWN_INNOVATIONS + 0x0C);
    if (known_count < 0 || known_count > 500) known_count = 0;

    // Build a set of known innovation type ptrs for lookup
    std::vector<uintptr_t> known_type_ptrs;
    if ((known_data && is_mapped(known_data)) && known_count > 0) {
        known_type_ptrs.reserve(known_count);
        // The elements ARE the types (CPdxArray<CCultureInnovationType*>, as the header
        // says): before P5 this read elem+0x10 as an instance's type pointer, which on
        // Linux is the type's key-string data, so era bins counted text bytes.
        for (int32_t k = 0; k < known_count; k++) {
            uintptr_t type_ptr = safe_read<uintptr_t>(known_data + k * 8);
            if (type_ptr && is_mapped(type_ptr)) known_type_ptrs.push_back(type_ptr);
        }
    }

    // FIX #fascination: PRIMARY/ACTIVE fascination is at +0x6F0 (FASCINATION_PTR), NOT the
    // bonus-marker +0x6D8. is_fascination is compared against this primary.
    uintptr_t fascination_ptr = safe_read<uintptr_t>(culture_obj + culture::FASCINATION_PTR);
    uintptr_t bonus_fasc_ptr   = safe_read<uintptr_t>(culture_obj + culture::BONUS_FASCINATION_PTR);

    // FIX #innovations: per-era known counts, binned from the authoritative KNOWN list
    // (NOT culture_era +0x44, which is a GLOBAL era static identical across all cultures).
    int era_known[8] = {0};
    bool any_regional_known = false;
    for (const auto& ktp : known_type_ptrs) {
        int32_t e = safe_read<int32_t>(ktp + innovation_type::ERA_INDEX);
        if (e >= 0 && e < 8) era_known[e]++;
        else any_regional_known = true;
    }
    // Per-era TOTAL denominator: read the era-struct INNOVATION_TOTAL (+0x40), the correct
    // global per-era count (42 tribal, etc.). Indexed alongside the era-name walk.
    int era_total[8] = {0};
    {
        uintptr_t era_ptr0 = safe_read<uintptr_t>(culture_obj + culture::ERA_ENTRIES_PTR);
        if (era_ptr0 && is_mapped(era_ptr0)) {
            for (int i = 0; i < era_name_count; i++) {
                uintptr_t entry = era_ptr0 + static_cast<uintptr_t>(i) * culture_era::STRIDE;
                if (!entry || !is_mapped(entry)) break;
                era_total[i] = safe_read<int32_t>(entry + culture_era::INNOVATION_TOTAL);
            }
        }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("culture_id"); r.value_int(culture_id_param);
    if (!culture_name.empty()) {
        r.key("culture_name"); r.value_string(culture_name.c_str(), culture_name.size());
    }
    if (!culture_key.empty()) {
        r.key("culture_key"); r.value_string(culture_key.c_str(), culture_key.size());
    }
    // FIX #innovations: surface the fascination by NAME + era at top level.
    if (fascination_ptr && is_mapped(fascination_ptr)) {
        std::string fn = ad_read_std_string(fascination_ptr + innovation_type::NAME_KEY);
        if (!fn.empty()) { r.key("fascination_name"); r.value_string(fn.c_str(), fn.size()); }
        int32_t fe = safe_read<int32_t>(fascination_ptr + innovation_type::ERA_INDEX);
        r.key("fascination_era"); r.value_int(fe);
        std::string fen = era_name_for(fe);
        r.key("fascination_era_name"); r.value_string(fen.c_str(), fen.size());
    }
    r.key("known_count"); r.value_int(known_count);
    r.key("innovations"); r.begin_array();

    int innov_count = 0;
    if ((innov_array && is_mapped(innov_array))) {
        // Iterate innovation instances. Max ~200 innovation types in the game.
        for (int idx = 0; idx < 200 && innov_count < 200; idx++) {
            uintptr_t inst = innov_array + static_cast<uintptr_t>(idx) *
                             innovation_instance::STRIDE;
            if ((!inst || !is_mapped(inst))) break;

            uintptr_t type_ptr = safe_read<uintptr_t>(inst +
                                                  innovation_instance::INNOVATION_TYPE_PTR);
            if ((!type_ptr || !is_mapped(type_ptr))) continue;

            // Skip null sentinel
            uintptr_t null_sentinel = safe_read_offset<uintptr_t>(ck3::globals::NULL_INNOVATION_TYPE);
            if (type_ptr == null_sentinel) continue;

            // Read innovation name
            std::string innov_name = ad_read_std_string(type_ptr +
                                                        innovation_type::NAME_KEY);
            if (innov_name.empty()) continue;

            // Read spread progress (0-10000000)
            int64_t progress = safe_read<int64_t>(inst + innovation_instance::SPREAD_PROGRESS);
            double progress_pct = static_cast<double>(progress) / 100000.0;  // 10M = 100%

            // Check if known (completed)
            bool is_known = false;
            for (const auto& ktp : known_type_ptrs) {
                if (ktp == type_ptr) { is_known = true; break; }
            }

            // Check if this is the fascination target (PRIMARY 0x6F0)
            bool is_fascination = ((fascination_ptr && is_mapped(fascination_ptr)) && type_ptr == fascination_ptr);
            bool is_bonus_fascination = ((bonus_fasc_ptr && is_mapped(bonus_fasc_ptr)) && type_ptr == bonus_fasc_ptr);

            // Era index + name
            int32_t era_index = safe_read<int32_t>(type_ptr + innovation_type::ERA_INDEX);
            std::string era_nm = era_name_for(era_index);

            // FIX #innovations: category for cheap agent filtering.
            const char* category = is_known ? "known"
                                 : (progress_pct > 0.0 || is_fascination) ? "researching"
                                 : "available";

            r.begin_object();
            r.key("name"); r.value_string(innov_name.c_str(), innov_name.size());
            r.key("db_index"); r.value_int(idx);
            r.key("progress_pct"); r.value_float(progress_pct);
            r.key("is_known"); r.value_bool(is_known);
            r.key("is_fascination"); r.value_bool(is_fascination);
            r.key("is_bonus_fascination"); r.value_bool(is_bonus_fascination);
            r.key("category"); r.value_string(category);
            r.key("era"); r.value_int(era_index);
            r.key("era_name"); r.value_string(era_nm.c_str(), era_nm.size());

            // Exposure/spread rate info (MEDIUM confidence)
            int32_t exposure = safe_read<int32_t>(inst + innovation_instance::EXPOSURE_COUNT);
            int32_t current = safe_read<int32_t>(inst + innovation_instance::CURRENT_COUNT);
            r.key("exposure_count"); r.value_int(exposure);
            r.key("current_count"); r.value_int(current);

            r.end_object();
            innov_count++;
        }
    }

    r.end_array();
    r.key("innovation_count"); r.value_int(innov_count);

    // FIX #innovations: era_summary — the feudalize-gate answer (per-era known/total).
    // current_era = highest era whose fascination_progress == 100% (10M), if derivable.
    int current_era = -1;
    {
        uintptr_t era_ptr0 = safe_read<uintptr_t>(culture_obj + culture::ERA_ENTRIES_PTR);
        int32_t fe = (fascination_ptr && is_mapped(fascination_ptr))
            ? safe_read<int32_t>(fascination_ptr + innovation_type::ERA_INDEX) : -1;
        r.key("era_summary"); r.begin_array();
        for (int i = 0; i < era_name_count; i++) {
            r.begin_object();
            r.key("era_index"); r.value_int(i);
            std::string en = era_name_for(i);
            r.key("era_name"); r.value_string(en.c_str(), en.size());
            r.key("known"); r.value_int(era_known[i]);
            r.key("total"); r.value_int(era_total[i]);
            r.key("is_fascination_era"); r.value_bool(fe == i);
            // mark current_era from fascination_progress completion
            if (era_ptr0 && is_mapped(era_ptr0)) {
                uintptr_t entry = era_ptr0 + static_cast<uintptr_t>(i) * culture_era::STRIDE;
                if (entry && is_mapped(entry)) {
                    int64_t fp = safe_read<int64_t>(entry + culture_era::FASCINATION_PROGRESS);
                    if (fp >= 10000000) current_era = i;
                }
            }
            r.end_object();
        }
        // regional bucket only if any known innovation has era_index >= era_count
        if (any_regional_known) {
            r.begin_object();
            r.key("era_index"); r.value_int(era_name_count > 4 ? era_name_count : 4);
            r.key("era_name"); r.value_string("regional");
            int reg_known = 0;
            for (const auto& ktp : known_type_ptrs) {
                int32_t e = safe_read<int32_t>(ktp + innovation_type::ERA_INDEX);
                if (e < 0 || e >= 8) reg_known++;
            }
            r.key("known"); r.value_int(reg_known);
            r.key("total"); r.value_int(0);
            r.key("is_fascination_era"); r.value_bool(false);
            r.end_object();
        }
        r.end_array();
    }
    if (current_era >= 0) { r.key("current_era"); r.value_int(current_era); }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_fascination_progress — Read fascination/era progress for a culture (ck-sdy2)
// Returns current fascination innovation and per-era fascination progress.
// =============================================================================
bool do_query_fascination_progress(const char* params_json, std::string* out) {
    int culture_id_param = json_get_int(params_json, "culture_id");
    uintptr_t culture_obj = 0;

    if (culture_id_param > 0) {
        culture_obj = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB,
                                   static_cast<uint32_t>(culture_id_param));
    } else {
        void* character = resolve_character(params_json);
        if (!character || is_sentinel(character)) {
            if (out) *out = R"({"error":"character not found"})";
            return false;
        }
        uintptr_t c = reinterpret_cast<uintptr_t>(character);
        uint32_t cid = safe_read<uint32_t>(c + ck3::character::CULTURE_ID_DIRECT);
        if (cid == 0 || cid == 0xFFFFFFFF) {
            if (out) *out = R"({"error":"character has no culture"})";
            return false;
        }
        culture_obj = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, cid);
        culture_id_param = static_cast<int>(cid & 0xFFFFFF);
    }

    if ((!culture_obj || !is_mapped(culture_obj))) {
        if (out) *out = R"({"error":"culture not found"})";
        return false;
    }

    // FIX #culture_name: read NAME (+0x260) not +0x10; add culture_key (+0x1C8).
    std::string culture_name = ad_read_std_string(culture_obj + culture::NAME);
    std::string culture_key  = ad_read_std_string(culture_obj + culture::SCRIPT_KEY);

    ResponseBuilder r;
    r.begin_object();
    r.key("culture_id"); r.value_int(culture_id_param);
    if (!culture_name.empty()) {
        r.key("culture_name"); r.value_string(culture_name.c_str(), culture_name.size());
    }
    if (!culture_key.empty()) {
        r.key("culture_key"); r.value_string(culture_key.c_str(), culture_key.size());
    }

    // FIX #fascination: PRIMARY/ACTIVE fascination is at +0x6F0 (FASCINATION_PTR), NOT the
    // bonus-marker +0x6D8 (which the old code wrongly reported as "fascination").
    uintptr_t fascination_ptr = safe_read<uintptr_t>(culture_obj + culture::FASCINATION_PTR);
    uintptr_t bonus_fasc_ptr   = safe_read<uintptr_t>(culture_obj + culture::BONUS_FASCINATION_PTR);
    if ((fascination_ptr && is_mapped(fascination_ptr))) {
        std::string fasc_name = ad_read_std_string(fascination_ptr +
                                                   innovation_type::NAME_KEY);
        if (!fasc_name.empty()) {
            r.key("fascination"); r.value_string(fasc_name.c_str(), fasc_name.size());
        }
        int32_t fasc_era = safe_read<int32_t>(fascination_ptr + innovation_type::ERA_INDEX);
        r.key("fascination_era"); r.value_int(fasc_era);
    } else {
        r.key("fascination"); r.value_null();
    }
    if ((bonus_fasc_ptr && is_mapped(bonus_fasc_ptr))) {
        std::string bn = ad_read_std_string(bonus_fasc_ptr + innovation_type::NAME_KEY);
        if (!bn.empty()) { r.key("bonus_fascination"); r.value_string(bn.c_str(), bn.size()); }
    }

    // Era entries: array at culture + ERA_ENTRIES_PTR, count at ERA_ENTRIES_COUNT
    uintptr_t era_ptr = safe_read<uintptr_t>(culture_obj + culture::ERA_ENTRIES_PTR);
    int32_t era_count = safe_read<int32_t>(culture_obj + culture::ERA_ENTRIES_COUNT);
    if (era_count < 0 || era_count > 20) era_count = 0;

    r.key("era_count"); r.value_int(era_count);
    r.key("eras"); r.begin_array();

    if ((era_ptr && is_mapped(era_ptr)) && era_count > 0) {
        for (int32_t i = 0; i < era_count; i++) {
            uintptr_t entry = era_ptr + static_cast<uintptr_t>(i) * culture_era::STRIDE;
            if ((!entry || !is_mapped(entry))) break;

            int64_t fasc_progress = safe_read<int64_t>(entry + culture_era::FASCINATION_PROGRESS);
            double progress_pct = static_cast<double>(fasc_progress) / 100000.0;  // 10M = 100%

            r.begin_object();
            r.key("era_index"); r.value_int(i);

            // R15: read era type name from ERA_TYPE_PTR -> NAME_KEY
            uintptr_t era_type = safe_read<uintptr_t>(entry + culture_era::ERA_TYPE_PTR);
            if ((era_type && is_mapped(era_type))) {
                std::string era_name = ad_read_std_string(era_type +
                                                          innovation_type::NAME_KEY);
                if (!era_name.empty()) {
                    r.key("era_name"); r.value_string(era_name.c_str(), era_name.size());
                }
            }

            r.key("fascination_progress_pct"); r.value_float(progress_pct);
            r.key("is_complete"); r.value_bool(fasc_progress >= 10000000);
            r.end_object();
        }
    }

    r.end_array();

    // FIX #fascination: eligible_fascinations — innovations NOT yet known whose era is
    // <= current_era. current_era = first era with fascination_progress < 10M (else last).
    // STRUCTURAL INFERENCE (MEDIUM): the engine command gate (CanBeFascination) may apply
    // extra scripted prerequisites; treat this as a SUPERSET. Final eligibility is enforced
    // by the set command.
    {
        int32_t current_era = 0;
        if ((era_ptr && is_mapped(era_ptr)) && era_count > 0) {
            current_era = era_count - 1;
            for (int32_t i = 0; i < era_count; i++) {
                uintptr_t entry = era_ptr + static_cast<uintptr_t>(i) * culture_era::STRIDE;
                if (!entry || !is_mapped(entry)) break;
                int64_t fp = safe_read<int64_t>(entry + culture_era::FASCINATION_PROGRESS);
                if (fp < 10000000) { current_era = i; break; }
            }
        }
        r.key("current_era"); r.value_int(current_era);

        // Build known-type set
        uintptr_t known_data = safe_read<uintptr_t>(culture_obj + culture::KNOWN_INNOVATIONS);
        int32_t known_count = safe_read<int32_t>(culture_obj + culture::KNOWN_INNOVATIONS + 0x0C);
        if (known_count < 0 || known_count > 500) known_count = 0;
        std::vector<uintptr_t> known_set;
        if ((known_data && is_mapped(known_data)) && known_count > 0) {
            for (int32_t k = 0; k < known_count; k++) {
                uintptr_t kp = safe_read<uintptr_t>(known_data + k * 8);
                if (kp && is_mapped(kp)) known_set.push_back(kp);
            }
        }
        auto is_known_type = [&](uintptr_t tp) {
            for (auto k : known_set) if (k == tp) return true;
            return false;
        };

        r.key("eligible_fascinations"); r.begin_array();
        uintptr_t innov_array = safe_read<uintptr_t>(culture_obj + culture::INNOVATION_ARRAY);
        uintptr_t null_sentinel = safe_read_offset<uintptr_t>(ck3::globals::NULL_INNOVATION_TYPE);
        if (innov_array && is_mapped(innov_array)) {
            for (int idx = 0; idx < 200; idx++) {
                uintptr_t inst = innov_array + static_cast<uintptr_t>(idx) * innovation_instance::STRIDE;
                if (!inst || !is_mapped(inst)) break;
                uintptr_t type_ptr = safe_read<uintptr_t>(inst + innovation_instance::INNOVATION_TYPE_PTR);
                if (!type_ptr || !is_mapped(type_ptr) || type_ptr == null_sentinel) continue;
                std::string nm = ad_read_std_string(type_ptr + innovation_type::NAME_KEY);
                if (nm.empty()) continue;
                if (is_known_type(type_ptr)) continue;
                int32_t e = safe_read<int32_t>(type_ptr + innovation_type::ERA_INDEX);
                if (e < 0 || e > current_era) continue;  // not yet reachable
                int64_t prog = safe_read<int64_t>(inst + innovation_instance::SPREAD_PROGRESS);
                r.begin_object();
                r.key("name"); r.value_string(nm.c_str(), nm.size());
                r.key("era"); r.value_int(e);
                r.key("db_index"); r.value_int(idx);
                r.key("progress_pct"); r.value_float(static_cast<double>(prog) / 100000.0);
                r.end_object();
            }
        }
        r.end_array();
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_research_speed — Read research speed modifiers for a culture (ck-sdy2)
// Returns culture's research speed modifier and global speed multipliers.
// =============================================================================
bool do_query_research_speed(const char* params_json, std::string* out) {
    int culture_id_param = json_get_int(params_json, "culture_id");
    uintptr_t culture_obj = 0;

    if (culture_id_param > 0) {
        culture_obj = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB,
                                   static_cast<uint32_t>(culture_id_param));
    } else {
        void* character = resolve_character(params_json);
        if (!character || is_sentinel(character)) {
            if (out) *out = R"({"error":"character not found"})";
            return false;
        }
        uintptr_t c = reinterpret_cast<uintptr_t>(character);
        uint32_t cid = safe_read<uint32_t>(c + ck3::character::CULTURE_ID_DIRECT);
        if (cid == 0 || cid == 0xFFFFFFFF) {
            if (out) *out = R"({"error":"character has no culture"})";
            return false;
        }
        culture_obj = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, cid);
        culture_id_param = static_cast<int>(cid & 0xFFFFFF);
    }

    if ((!culture_obj || !is_mapped(culture_obj))) {
        if (out) *out = R"({"error":"culture not found"})";
        return false;
    }

    // FIX #culture_name: read NAME (+0x260) not +0x10 (the uint32 ID); add culture_key (+0x1C8).
    std::string culture_name = ad_read_std_string(culture_obj + culture::NAME);
    std::string culture_key  = ad_read_std_string(culture_obj + culture::SCRIPT_KEY);

    ResponseBuilder r;
    r.begin_object();
    r.key("culture_id"); r.value_int(culture_id_param);
    if (!culture_name.empty()) {
        r.key("culture_name"); r.value_string(culture_name.c_str(), culture_name.size());
    }
    if (!culture_key.empty()) {
        r.key("culture_key"); r.value_string(culture_key.c_str(), culture_key.size());
    }

    // Research speed modifier: int32 fixed-point /100000
    int32_t research_mod = safe_read<int32_t>(culture_obj + culture::RESEARCH_SPEED_MOD);
    r.key("research_speed_modifier"); r.value_float(static_cast<double>(research_mod) / 100000.0);
    r.key("research_speed_raw"); r.value_int(research_mod);

    // Global speed multipliers (BSS — may be zero if not yet initialized)
    int64_t spread_mult = safe_read_offset<int64_t>(ck3::globals::SPREAD_SPEED_MULT);
    int64_t fasc_mult = safe_read_offset<int64_t>(ck3::globals::FASCINATION_SPEED_MULT);
    r.key("global_spread_multiplier"); r.value_float(static_cast<double>(spread_mult) / 100000.0);
    r.key("global_fascination_multiplier"); r.value_float(static_cast<double>(fasc_mult) / 100000.0);

    r.end_object();
    if (out) *out = r.take();
    return true;
}


// =============================================================================
// query_succession_simulation: comprehensive "what happens when I die" prediction
// Handles mixed succession types per-title: partition, single_heir, elective.
// Uses per-title heir lists (not just primary title's) for accurate simulation.
// Returns per-heir distribution + player_heir summary of kept/lost titles.
// =============================================================================
bool do_query_succession_simulation(const char* params_json, std::string* out) {
    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        if (out) *out = "{\"error\":\"character not found\"}";
        return false;
    }

    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t char_id = char_get_id(character) & 0xFFFFFF;

    uintptr_t title_data = resolve_title_realm(c);
    if (!title_data || !is_mapped(title_data)) {
        if (out) *out = "{\"error\":\"character is unlanded\"}";
        return false;
    }

    uintptr_t title_arr_ptr = safe_read<uintptr_t>(title_data + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t title_arr_count = safe_read<int32_t>(title_data + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (title_arr_count < 0 || title_arr_count > 500) title_arr_count = 0;

    if (!title_arr_ptr || !is_mapped(title_arr_ptr) || title_arr_count == 0) {
        if (out) *out = "{\"error\":\"no held titles\"}";
        return false;
    }

    // Collect held titles with succession info
    struct TitleInfo {
        uint32_t id;
        uintptr_t ptr;
        int tier;
        uint32_t de_jure_parent;
        uint8_t succ_order;   // 0=inheritance, 1=election, 2=appointment
        uint8_t division;     // 0=single_heir, 1=partition
        uint8_t rank;         // 0=confederate, 1=regular
        std::string key;
        std::string display_name;
    };
    std::vector<TitleInfo> held_titles;
    held_titles.reserve(title_arr_count);

    int best_tier = -1;
    uint32_t primary_title_id = 0;
    uintptr_t primary_title_ptr = 0;

    for (int32_t i = 0; i < title_arr_count; i++) {
        uint32_t tid = safe_read<uint32_t>(title_arr_ptr + i * sizeof(uint32_t));
        uintptr_t tptr = lookup_title_obj(tid);
        if (!tptr || !is_mapped(tptr)) continue;

        int tier = get_title_tier(tptr);
        if (tier <= 1) continue;  // skip baronies

        uint8_t skip_flag = safe_read<uint8_t>(tptr + ck3::title_obj::TITLE_SKIP_FLAG_2D);
        if (skip_flag) continue;

        uint32_t dejure = safe_read<uint32_t>(tptr + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
        uint8_t order = title_succ_rule(tptr).order;
        uint8_t division = title_succ_rule(tptr).division;
        uint8_t rank = title_succ_rule(tptr).rank;

        std::string key;
        uintptr_t type_ptr = safe_read<uintptr_t>(tptr + ck3::title_obj::TITLE_TYPE_PTR);
        if (type_ptr && is_mapped(type_ptr)) {
            key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        }
        std::string dname = ad_read_std_string(tptr + ck3::title_obj::TITLE_DISPLAY_NAME);

        held_titles.push_back({tid & 0xFFFFFF, tptr, tier, dejure, order, division, rank,
                               std::move(key), std::move(dname)});

        if (tier > best_tier) {
            best_tier = tier;
            primary_title_id = tid & 0xFFFFFF;
            primary_title_ptr = tptr;
        }
    }

    if (!primary_title_ptr) {
        if (out) *out = "{\"error\":\"no primary title found\"}";
        return false;
    }

    // Read heir list for a title
    auto read_heir_list = [](uintptr_t tptr) -> std::vector<uint32_t> {
        std::vector<uint32_t> heirs;
        uintptr_t heir_arr = safe_read<uintptr_t>(tptr + ck3::title_obj::SUCCESSION_HEIRS);
        int32_t heir_count = safe_read<int32_t>(tptr + ck3::title_obj::SUCCESSION_HEIR_COUNT);
        if (heir_count < 0) heir_count = 0;
        if (heir_count > 50) heir_count = 50;
        if (!heir_arr || !is_mapped(heir_arr)) return heirs;

        for (int32_t i = 0; i < heir_count; i++) {
            uint32_t href = safe_read<uint32_t>(heir_arr + static_cast<uintptr_t>(i) * 4);
            uint32_t hcid = href & 0xFFFFFF;
            if (hcid == 0 || hcid == 0xFFFFFF) continue;
            void* hchar = lookup_character(href);
            if (!hchar || is_sentinel(hchar)) continue;
            heirs.push_back(hcid);
        }
        return heirs;
    };

    // Get primary title's heir list (used for partition distribution)
    std::vector<uint32_t> primary_heirs = read_heir_list(primary_title_ptr);
    if (primary_heirs.empty()) {
        if (out) *out = "{\"error\":\"no heirs found on primary title\"}";
        return false;
    }

    uint32_t player_heir = primary_heirs[0];

    // Resolve heir names (all unique heirs across titles)
    std::map<uint32_t, std::string> heir_names;
    auto ensure_name = [&](uint32_t hcid) {
        if (heir_names.count(hcid)) return;
        void* hchar = lookup_character(hcid);
        if (hchar && !is_sentinel(hchar)) {
            heir_names[hcid] = ad_read_char_name(reinterpret_cast<uintptr_t>(hchar));
        } else {
            heir_names[hcid] = "?";
        }
    };
    for (uint32_t h : primary_heirs) ensure_name(h);

    // Distribution: heir_id -> list of title indices
    std::map<uint32_t, std::vector<size_t>> distribution;
    for (uint32_t h : primary_heirs) distribution[h] = {};
    std::set<uint32_t> assigned;

    // --- Step 1: Handle elective and single-heir titles first ---
    // These titles have their own heir lists independent of partition
    for (size_t ti = 0; ti < held_titles.size(); ti++) {
        auto& t = held_titles[ti];
        if (t.succ_order == 1) {
            // Elective: winner is the title's first heir
            std::vector<uint32_t> title_heirs = read_heir_list(t.ptr);
            uint32_t winner = title_heirs.empty() ? player_heir : title_heirs[0];
            ensure_name(winner);
            if (!distribution.count(winner)) distribution[winner] = {};
            distribution[winner].push_back(ti);
            assigned.insert(t.id);
        } else if (t.division == 0 && t.id != primary_title_id) {
            // Single-heir (non-partition) non-primary: goes to this title's first heir
            std::vector<uint32_t> title_heirs = read_heir_list(t.ptr);
            uint32_t winner = title_heirs.empty() ? player_heir : title_heirs[0];
            ensure_name(winner);
            if (!distribution.count(winner)) distribution[winner] = {};
            distribution[winner].push_back(ti);
            assigned.insert(t.id);
        }
    }

    // --- Step 2: Handle partition titles ---
    // Check primary title's division
    uint8_t primary_division = title_succ_rule(primary_title_ptr).division;
    bool is_partition = (primary_division == 1);

    // Pre-assign primary title to primary heir
    for (size_t ti = 0; ti < held_titles.size(); ti++) {
        if (held_titles[ti].id == primary_title_id && assigned.find(primary_title_id) == assigned.end()) {
            distribution[player_heir].push_back(ti);
            assigned.insert(primary_title_id);
            break;
        }
    }

    if (!is_partition || primary_heirs.size() <= 1) {
        // Non-partition or single heir: remaining unassigned go to primary heir
        for (size_t ti = 0; ti < held_titles.size(); ti++) {
            if (assigned.find(held_titles[ti].id) == assigned.end()) {
                distribution[player_heir].push_back(ti);
                assigned.insert(held_titles[ti].id);
            }
        }
    } else {
        // Partition: tier-descending greedy with de jure scoring
        // Pre-assign de jure children of primary title to primary heir
        int32_t djc_count = safe_read<int32_t>(primary_title_ptr + ck3::title_obj::DE_JURE_CHILDREN_COUNT);
        uintptr_t djc_arr = safe_read<uintptr_t>(primary_title_ptr + ck3::title_obj::DE_JURE_CHILDREN_ARRAY);
        if (djc_count > 0 && djc_count < 200 && djc_arr && is_mapped(djc_arr)) {
            for (int32_t di = 0; di < djc_count; di++) {
                uint32_t child_tid = safe_read<uint32_t>(djc_arr + static_cast<uintptr_t>(di) * 4) & 0xFFFFFF;
                for (size_t ti = 0; ti < held_titles.size(); ti++) {
                    if (held_titles[ti].id == child_tid && assigned.find(child_tid) == assigned.end()
                        && held_titles[ti].division == 1) {
                        distribution[player_heir].push_back(ti);
                        assigned.insert(child_tid);
                        break;
                    }
                }
            }
        }

        // Read scoring weights
        int32_t own_weight = safe_read<int32_t>(bridge::game_fn(ck3::funcs::PARTITION_DEJURE_OWN_WEIGHT));
        int32_t other_weight = safe_read<int32_t>(bridge::game_fn(ck3::funcs::PARTITION_DEJURE_OTHER_WEIGHT));
        if (own_weight == 0) own_weight = 100;
        if (other_weight == 0) other_weight = 50;

        // De jure relationship check
        auto is_dejure_related = [&](uint32_t title_a, uint32_t title_b) -> bool {
            uintptr_t ptr_a = lookup_title_obj(title_a);
            uintptr_t ptr_b = lookup_title_obj(title_b);
            if (ptr_a && is_mapped(ptr_a) && ptr_b && is_mapped(ptr_b)) {
                uint32_t parent_a = safe_read<uint32_t>(ptr_a + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                uint32_t parent_b = safe_read<uint32_t>(ptr_b + ck3::title_obj::DE_JURE_PARENT) & 0xFFFFFF;
                if (parent_a == title_b || parent_b == title_a) return true;
                if (parent_a != 0 && parent_a == parent_b) return true;
            }
            return false;
        };

        // Get assigned title IDs per heir for scoring
        auto get_heir_title_ids = [&](uint32_t heir_id) -> std::vector<uint32_t> {
            std::vector<uint32_t> ids;
            for (size_t ti : distribution[heir_id]) {
                ids.push_back(held_titles[ti].id);
            }
            return ids;
        };

        // Tier-descending distribution of partition-eligible titles
        for (int tier = best_tier; tier >= 2; tier--) {
            // Collect unassigned partition titles at this tier
            std::vector<size_t> unassigned_at_tier;
            for (size_t ti = 0; ti < held_titles.size(); ti++) {
                auto& t = held_titles[ti];
                if (t.tier == tier && t.division == 1
                    && assigned.find(t.id) == assigned.end()) {
                    unassigned_at_tier.push_back(ti);
                }
            }
            if (unassigned_at_tier.empty()) continue;

            // Count per-heir titles at this tier
            std::map<uint32_t, int> heir_tier_count;
            for (uint32_t h : primary_heirs) {
                heir_tier_count[h] = 0;
                for (size_t ti : distribution[h]) {
                    if (held_titles[ti].tier == tier) heir_tier_count[h]++;
                }
            }

            while (!unassigned_at_tier.empty()) {
                // Pick least-titled heir
                uint32_t chosen = primary_heirs[0];
                int min_count = heir_tier_count[chosen];
                for (size_t hi = 1; hi < primary_heirs.size(); hi++) {
                    int cnt = heir_tier_count[primary_heirs[hi]];
                    if (cnt < min_count) {
                        min_count = cnt;
                        chosen = primary_heirs[hi];
                    }
                }

                // Score unassigned titles for chosen heir
                int best_score = -999999;
                size_t best_idx = 0;
                auto chosen_titles = get_heir_title_ids(chosen);
                for (size_t ui = 0; ui < unassigned_at_tier.size(); ui++) {
                    uint32_t tid = held_titles[unassigned_at_tier[ui]].id;
                    int own_matches = 0, other_matches = 0;

                    for (uint32_t atid : chosen_titles) {
                        if (is_dejure_related(tid, atid)) own_matches++;
                    }
                    for (uint32_t h : primary_heirs) {
                        if (h == chosen) continue;
                        for (size_t oti : distribution[h]) {
                            if (is_dejure_related(tid, held_titles[oti].id)) other_matches++;
                        }
                    }

                    int score = own_matches * own_weight - other_matches * other_weight;
                    if (score > best_score) {
                        best_score = score;
                        best_idx = ui;
                    }
                }

                // Assign best title
                size_t assigned_ti = unassigned_at_tier[best_idx];
                distribution[chosen].push_back(assigned_ti);
                assigned.insert(held_titles[assigned_ti].id);
                heir_tier_count[chosen]++;
                unassigned_at_tier.erase(unassigned_at_tier.begin() + best_idx);

                // Cascade de jure children
                uintptr_t atptr = held_titles[assigned_ti].ptr;
                if (atptr) {
                    int32_t cdjc = safe_read<int32_t>(atptr + ck3::title_obj::DE_JURE_CHILDREN_COUNT);
                    uintptr_t cdja = safe_read<uintptr_t>(atptr + ck3::title_obj::DE_JURE_CHILDREN_ARRAY);
                    if (cdjc > 0 && cdjc < 200 && cdja && cdja >= 0x10000) {
                        for (int32_t di = 0; di < cdjc; di++) {
                            uint32_t child_tid = safe_read<uint32_t>(cdja + static_cast<uintptr_t>(di) * 4) & 0xFFFFFF;
                            for (auto it = unassigned_at_tier.begin(); it != unassigned_at_tier.end(); ++it) {
                                if (held_titles[*it].id == child_tid) {
                                    distribution[chosen].push_back(*it);
                                    assigned.insert(child_tid);
                                    heir_tier_count[chosen]++;
                                    unassigned_at_tier.erase(it);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Any remaining unassigned titles go to primary heir
        for (size_t ti = 0; ti < held_titles.size(); ti++) {
            if (assigned.find(held_titles[ti].id) == assigned.end()) {
                distribution[player_heir].push_back(ti);
                assigned.insert(held_titles[ti].id);
            }
        }
    }

    // --- Build response ---
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_succession_simulation");
    r.key("character_id"); r.value_uint(char_id);
    r.key("primary_title_id"); r.value_uint(primary_title_id);
    r.key("primary_title_tier"); r.value_int(best_tier);
    r.key("total_titles"); r.value_int(static_cast<int>(held_titles.size()));

    // Succession type summary
    int partition_count = 0, single_count = 0, elective_count = 0;
    for (auto& t : held_titles) {
        if (t.succ_order == 1) elective_count++;
        else if (t.division == 1) partition_count++;
        else single_count++;
    }
    r.key("succession_types"); r.begin_object();
    r.key("partition"); r.value_int(partition_count);
    r.key("single_heir"); r.value_int(single_count);
    r.key("elective"); r.value_int(elective_count);
    r.end_object();

    // Primary heir summary
    auto& player_titles = distribution[player_heir];
    int player_kept = static_cast<int>(player_titles.size());
    int player_lost = static_cast<int>(held_titles.size()) - player_kept;

    r.key("player_heir"); r.begin_object();
    r.key("character_id"); r.value_uint(player_heir);
    r.key("name"); r.value_string(heir_names[player_heir].c_str(), heir_names[player_heir].size());
    r.key("titles_kept"); r.value_int(player_kept);
    r.key("titles_lost"); r.value_int(player_lost);
    r.key("is_partition"); r.value_bool(is_partition);

    // Rank of partition (confederate vs regular)
    uint8_t primary_rank = title_succ_rule(primary_title_ptr).rank;
    const char* rank_str = (primary_rank == 0) ? "confederate" : (primary_rank == 1) ? "regular" : "none";
    r.key("partition_type"); r.value_string(rank_str);
    r.end_object();

    // Per-heir distribution
    r.key("heirs"); r.begin_array();
    // Primary heir first, then others
    auto emit_heir = [&](uint32_t hcid) {
        auto& titles = distribution[hcid];
        if (titles.empty()) return;

        r.begin_object();
        r.key("character_id"); r.value_uint(hcid);
        r.key("name"); r.value_string(heir_names[hcid].c_str(), heir_names[hcid].size());
        r.key("is_player_heir"); r.value_bool(hcid == player_heir);
        r.key("title_count"); r.value_int(static_cast<int>(titles.size()));
        r.key("titles"); r.begin_array();
        for (size_t ti : titles) {
            auto& t = held_titles[ti];
            r.begin_object();
            r.key("title_id"); r.value_uint(t.id);
            r.key("tier"); r.value_int(t.tier);
            if (!t.key.empty()) {
                r.key("key"); r.value_string(t.key.c_str(), t.key.size());
            }
            if (!t.display_name.empty()) {
                r.key("name"); r.value_string(t.display_name.c_str(), t.display_name.size());
            }
            const char* succ_type = (t.succ_order == 1) ? "elective" :
                                    (t.division == 1) ? "partition" : "single_heir";
            r.key("succession_type"); r.value_string(succ_type);
            r.end_object();
        }
        r.end_array();
        r.end_object();
    };

    emit_heir(player_heir);
    for (auto& [hcid, titles] : distribution) {
        if (hcid != player_heir) emit_heir(hcid);
    }
    r.end_array();

    // Titles lost (convenience list for agent decision-making)
    r.key("titles_lost"); r.begin_array();
    for (auto& [hcid, titles] : distribution) {
        if (hcid == player_heir) continue;
        for (size_t ti : titles) {
            auto& t = held_titles[ti];
            r.begin_object();
            r.key("title_id"); r.value_uint(t.id);
            r.key("tier"); r.value_int(t.tier);
            if (!t.key.empty()) {
                r.key("key"); r.value_string(t.key.c_str(), t.key.size());
            }
            if (!t.display_name.empty()) {
                r.key("name"); r.value_string(t.display_name.c_str(), t.display_name.size());
            }
            r.key("heir_id"); r.value_uint(hcid);
            r.key("heir_name"); r.value_string(heir_names[hcid].c_str(), heir_names[hcid].size());
            r.end_object();
        }
    }
    r.end_array();

    r.key("status"); r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}


// =============================================================================
// query_trigger_result — Evaluate individual trigger slots on an interaction
// =============================================================================
//
// Deferred query: fills TriggerQuery, game thread evaluates via TLS pipeline +
// FUN_07628500, IPC thread polls for result.
//
// Parameters:
//   interaction_type: string (e.g. "declare_war_interaction")
//   trigger_slot:     int (-1=all, 0-6=specific) or string name
//   actor_id:         int (optional, defaults to controlled character)
//   target_id:        int (required)

bool do_query_trigger_result(const char* params_json, std::string* out) {
    // Copy immediately: json_get_string returns a pointer into a thread-local
    // static buffer that is clobbered by the trigger_slot read below.
    const char* raw_type = json_get_string(params_json, "interaction_type");
    if (!raw_type || !raw_type[0]) {
        if (out) *out = R"({"error":"missing_interaction_type"})";
        return false;
    }
    std::string type_str(raw_type);
    const char* type = type_str.c_str();

    int target_id = json_get_int(params_json, "target_id");
    if (target_id <= 0) {
        if (out) *out = R"({"error":"missing_target_id"})";
        return false;
    }

    int actor_id = json_get_int(params_json, "actor_id");
    if (actor_id <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        actor_id = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (actor_id <= 0) {
        if (out) *out = R"({"error":"no_actor_available"})";
        return false;
    }

    // Parse trigger_slot — accept int or string name
    int trigger_slot = -1;  // default: all
    if (json_has_key(params_json, "trigger_slot")) {
        const char* slot_str = json_get_string(params_json, "trigger_slot");
        if (slot_str && slot_str[0]) {
            // Try as name first
            if (strcmp(slot_str, "all") == 0) {
                trigger_slot = -1;
            } else if (strcmp(slot_str, "is_valid") == 0) {
                trigger_slot = 0;
            } else if (strcmp(slot_str, "is_shown") == 0) {
                trigger_slot = 1;
            } else if (strcmp(slot_str, "is_valid_showing_failures_only") == 0) {
                trigger_slot = 2;
            } else if (strcmp(slot_str, "can_be_sent_despite_rejection") == 0) {
                trigger_slot = 3;
            } else if (strcmp(slot_str, "can_send") == 0) {
                trigger_slot = 4;
            } else if (strcmp(slot_str, "auto_accept") == 0) {
                trigger_slot = 5;
            } else if (strcmp(slot_str, "on_auto_accept") == 0) {
                trigger_slot = 6;
            } else {
                // Try as integer
                trigger_slot = atoi(slot_str);
            }
        } else {
            trigger_slot = json_get_int(params_json, "trigger_slot");
        }
    }

    if (trigger_slot < -1 || trigger_slot > 6) {
        if (out) *out = R"({"error":"invalid_trigger_slot","hint":"Use -1 or 'all' for all slots, 0-6 or slot name for specific"})";
        return false;
    }

    // Check if a query is already in flight
    int current_state = g_trigger_query.state.load(std::memory_order_acquire);
    if (current_state == 1 || current_state == 4) {
        if (out) *out = R"({"error":"trigger_query_busy","hint":"Another trigger query is pending. Wait for it to complete."})";
        return true;
    }

    // Check for completed result matching our request
    if (current_state == 2 || current_state == 3) {
        bool matches = (g_trigger_query.actor_id == static_cast<uint32_t>(actor_id) &&
                       g_trigger_query.target_id == static_cast<uint32_t>(target_id) &&
                       strcmp(g_trigger_query.interaction_type, type) == 0 &&
                       g_trigger_query.trigger_slot == trigger_slot);

        if (matches) {
            ResponseBuilder r;
            r.begin_object();
            r.key("action"); r.value_string("query_trigger_result");
            r.key("interaction_type"); r.value_string(type);
            r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
            r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);

            if (current_state == 3) {
                r.key("error"); r.value_string(g_trigger_query.error);
                r.key("status"); r.value_string("error");
            } else {
                r.key("results"); r.begin_array();
                for (int i = 0; i < g_trigger_query.result_count; i++) {
                    const TriggerSlotResult& res = g_trigger_query.results[i];
                    r.begin_object();
                    r.key("slot"); r.value_int(res.slot);
                    r.key("name"); r.value_string(res.name);
                    char offset_hex[16];
                    snprintf(offset_hex, sizeof(offset_hex), "0x%04x", res.offset);
                    r.key("offset"); r.value_string(offset_hex);
                    r.key("present"); r.value_bool(res.present);
                    r.key("result"); r.value_bool(res.result);
                    if (res.loc_key[0]) {
                        r.key("loc_key"); r.value_string(res.loc_key);
                    }
                    r.end_object();
                }
                r.end_array();
                r.key("status"); r.value_string("ok");
            }
            r.key("deferred"); r.value_bool(true);
            r.end_object();
            if (out) *out = r.take();
            g_trigger_query.state.store(0, std::memory_order_release);
            return true;
        } else {
            // Previous result doesn't match — clear and submit new
            g_trigger_query.state.store(0, std::memory_order_release);
        }
    }

    // Submit new deferred query
    g_trigger_query.actor_id = static_cast<uint32_t>(actor_id);
    g_trigger_query.target_id = static_cast<uint32_t>(target_id);
    strncpy(g_trigger_query.interaction_type, type,
            sizeof(g_trigger_query.interaction_type) - 1);
    g_trigger_query.interaction_type[sizeof(g_trigger_query.interaction_type) - 1] = '\0';
    g_trigger_query.trigger_slot = trigger_slot;
    g_trigger_query.extract_loc_keys = json_get_bool(params_json, "extract_loc_keys", false);
    g_trigger_query.result_count = 0;
    g_trigger_query.error[0] = '\0';

    g_trigger_query.state.store(1, std::memory_order_release);

    ADLOG("query_trigger_result: deferred query submitted: %s actor=%d target=%d slot=%d\n",
          type, actor_id, target_id, trigger_slot);

    // Poll for result (up to ~3 seconds, 50ms intervals — trigger eval needs a game tick)
    for (int i = 0; i < 60; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int st = g_trigger_query.state.load(std::memory_order_acquire);
        if (st == 2 || st == 3) {
            ResponseBuilder r;
            r.begin_object();
            r.key("action"); r.value_string("query_trigger_result");
            r.key("interaction_type"); r.value_string(type);
            r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
            r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);

            if (st == 3) {
                r.key("error"); r.value_string(g_trigger_query.error);
                r.key("status"); r.value_string("error");
            } else {
                r.key("results"); r.begin_array();
                for (int j = 0; j < g_trigger_query.result_count; j++) {
                    const TriggerSlotResult& res = g_trigger_query.results[j];
                    r.begin_object();
                    r.key("slot"); r.value_int(res.slot);
                    r.key("name"); r.value_string(res.name);
                    char offset_hex[16];
                    snprintf(offset_hex, sizeof(offset_hex), "0x%04x", res.offset);
                    r.key("offset"); r.value_string(offset_hex);
                    r.key("present"); r.value_bool(res.present);
                    r.key("result"); r.value_bool(res.result);
                    r.end_object();
                }
                r.end_array();
                r.key("status"); r.value_string("ok");
            }
            r.key("deferred"); r.value_bool(true);
            r.key("wait_ms"); r.value_int((i + 1) * 50);
            r.end_object();
            if (out) *out = r.take();
            g_trigger_query.state.store(0, std::memory_order_release);
            return true;
        }
    }

    // Timeout
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_trigger_result");
    r.key("interaction_type"); r.value_string(type);
    r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);
    r.key("error"); r.value_string("timeout_waiting_for_game_thread");
    r.key("hint"); r.value_string("Game must be unpaused for at least one tick. "
                                   "The query runs during drain_and_execute_intents on the game thread.");
    r.key("deferred"); r.value_bool(true);
    r.key("status"); r.value_string("timeout");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// Query: Trigger Slot Inspector (ck-vqth)
// Reads interaction trigger slot structure from memory. IPC-thread safe.
// Returns per-slot: RTTI class name, children count, child types, label string.
// Complements do_query_trigger_result (which evaluates pass/fail on game thread).
// =============================================================================

// Read RTTI class name from a vtable pointer.
// Itanium ABI: vtable[-1] → typeinfo, typeinfo+8 → mangled name.
static std::string read_rtti_name(uintptr_t vtable_ptr) {
    char buf[128] = {};
    if (!game_rtti_name(vtable_ptr, buf, sizeof buf)) return "";
    // Skip leading digits (mangled name length prefix like "11CAndTrigger")
    const char* p = buf;
    while (*p >= '0' && *p <= '9') p++;
    return p;
}

// Slot descriptor table — all trigger slots in CCharacterInteractionType.
//
// Names mirror g_trigger_slots in bridge/src/ai_eval_hook.cpp so that callers
// receive the same slot name from query_trigger_slots and query_trigger_result.
// They also match the Python schema in ck3agent/server/tools.py (the
// `trigger_slot` enum and `_INTERACTION_HARD_GATE_SLOTS = (0, 1, 4)` indexing).
//
// NOTE: this naming scheme pre-dates ck-angk and is NOT the canonical slot map
// in re/findings/re_interaction_slot_layout_corrected.md — see ck3_offsets.h
// CANONICAL CORRECTED NAMES block. A separate sweep is required to migrate
// both tables (and the Python contract) to ck-angk canonical names.
struct TriggerSlotDesc {
    const char* name;
    uint32_t    offset;
    const char* role;  // hard_gate, soft, conditional, effect
};

static const TriggerSlotDesc g_slot_descs[] = {
    {"is_valid",                       ck3::interaction_def::IS_SHOWN_TRIGGER,        "hard_gate"},
    {"is_shown",                       ck3::interaction_def::IS_VALID_TRIGGER,        "soft"},
    {"auto_accept",                    ck3::interaction_def::AUTO_ACCEPT_TRIGGER,     "effect"},
    {"is_valid_showing_failures_only", ck3::interaction_def::HAS_VALID_TARGET,        "soft"},
    {"on_auto_accept",                 ck3::interaction_def::ON_AUTO_ACCEPT,          "effect"},
    {"can_send",                       ck3::interaction_def::CAN_SEND_TRIGGER,        "hard_gate"},
    {"can_be_sent_despite_rejection",  ck3::interaction_def::CAN_SEND_WITH_EXCEPTION, "conditional"},
};
static const int NUM_SLOT_DESCS = sizeof(g_slot_descs) / sizeof(g_slot_descs[0]);

bool do_query_trigger_slots(const char* params_json, std::string* out) {
    const char* itype = json_get_string(params_json, "interaction_type");
    if (!itype || !itype[0]) itype = json_get_string(params_json, "interaction");
    if (!itype || !itype[0]) {
        if (out) *out = R"({"success":false,"reason":"missing interaction_type"})";
        return false;
    }

    void* interaction = lookup_interaction(itype);
    if (!interaction) {
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("reason"); r.value_string("interaction_not_found");
            r.key("interaction"); r.value_string(itype);
            r.end_object();
            *out = r.take();
        }
        return false;
    }

    uintptr_t idef = reinterpret_cast<uintptr_t>(interaction);

    // Read AI response flag (controls can_send bypass)
    uint8_t ai_flag = safe_read<uint8_t>(idef + ck3::interaction_def::AI_RESPONSE_FLAG);

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("interaction"); r.value_string(itype);
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
    {
        char hex[20]; snprintf(hex, sizeof(hex), "0x%lx", (unsigned long)idef);
        r.key("interaction_addr"); r.value_string(hex);
    }
#endif
    r.key("ai_response_flag"); r.value_int(ai_flag);
    r.key("can_send_bypassed_for_ai"); r.value_bool(ai_flag != 0);

    r.key("slots"); r.begin_array();
    for (int i = 0; i < NUM_SLOT_DESCS; i++) {
        const TriggerSlotDesc& slot = g_slot_descs[i];
        uintptr_t trigger_addr = idef + slot.offset;

        // Read CAndTrigger structure
        uintptr_t vtable = safe_read<uintptr_t>(trigger_addr + ck3::trigger_struct::VTABLE);
        uint64_t packed = safe_read<uint64_t>(trigger_addr + ck3::trigger_struct::CHILDREN_PACKED);
        int32_t children = static_cast<int32_t>(packed >> 32);

        // Read label string (debug info from script compiler)
        uintptr_t label_ptr = safe_read<uintptr_t>(trigger_addr + ck3::trigger_struct::LABEL);
        char label_buf[128] = {};
        if ((label_ptr && is_mapped(label_ptr))) {
            for (int j = 0; j < 127; j++) {
                uint8_t ch = safe_read<uint8_t>(label_ptr + j);
                if (ch == 0 || ch < 0x20 || ch > 0x7e) break;
                label_buf[j] = static_cast<char>(ch);
            }
        }

        // Read source file path
        uintptr_t src_ptr = safe_read<uintptr_t>(trigger_addr + ck3::trigger_struct::SOURCE_FILE);
        char src_buf[200] = {};
        if ((src_ptr && is_mapped(src_ptr))) {
            for (int j = 0; j < 199; j++) {
                uint8_t ch = safe_read<uint8_t>(src_ptr + j);
                if (ch == 0 || ch < 0x20 || ch > 0x7e) break;
                src_buf[j] = static_cast<char>(ch);
            }
        }

        // Read RTTI class name
        std::string class_name = read_rtti_name(vtable);

        r.begin_object();
        r.key("name"); r.value_string(slot.name);
        {
            char hex[12]; snprintf(hex, sizeof(hex), "0x%x", slot.offset);
            r.key("offset"); r.value_string(hex);
        }
        r.key("role"); r.value_string(slot.role);
        r.key("class"); r.value_string(class_name.empty() ? "unknown" : class_name.c_str());
        r.key("children"); r.value_int(children);
        r.key("empty"); r.value_bool(children == 0);
        if (label_buf[0]) {
            r.key("label"); r.value_string(label_buf);
        }
        if (src_buf[0]) {
            r.key("source"); r.value_string(src_buf);
        }

        // For non-empty slots, walk children to show their RTTI types
        if (children > 0 && children <= 20) {
            uintptr_t data_ptr = safe_read<uintptr_t>(trigger_addr + ck3::trigger_struct::CHILDREN_DATA);
            if ((data_ptr && is_mapped(data_ptr))) {
                r.key("child_types"); r.begin_array();
                int max_show = (children > 8) ? 8 : children;
                for (int c = 0; c < max_show; c++) {
                    uintptr_t child_ptr = safe_read<uintptr_t>(data_ptr + c * 8);
                    if ((!child_ptr || !is_mapped(child_ptr))) { r.value_string("null"); continue; }
                    uintptr_t child_vt = safe_read<uintptr_t>(child_ptr);
                    std::string cname = read_rtti_name(child_vt);
                    r.value_string(cname.empty() ? "unknown" : cname.c_str());
                }
                if (children > 8) r.value_string("...");
                r.end_array();
            }
        }

        r.end_object();
    }
    r.end_array();

    r.key("note"); r.value_string("children=0 means empty trigger (always true). "
        "Use query_trigger_result with actor_id/target_id for pass/fail evaluation.");
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// query_debug_flags — read current state of game debug flags
// ─────────────────────────────────────────────────────────────────────────────
bool do_query_debug_flags(const char* /*params_json*/, std::string* out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("query"); r.value_string("debug_flags");

    uintptr_t flags_base = bridge::game_fn(ck3::funcs::DEBUG_FLAGS_BASE);
    if (!is_mapped(flags_base)) {
        r.key("error"); r.value_string("debug_flags_base_not_mapped");
        r.end_object();
        if (out) *out = r.take();
        return false;
    }

    // Read individual debug flags
    r.key("yesmen");      r.value_int(safe_read<uint8_t>(flags_base + 0x0));
    r.key("nomen");       r.value_int(safe_read<uint8_t>(flags_base + 0x1));
    r.key("instant");     r.value_int(safe_read<uint8_t>(flags_base + 0x2));
    r.key("ai_watch_sub");r.value_int(safe_read<uint8_t>(flags_base + 0x3));
    r.key("aiwatch");     r.value_int(safe_read<uint8_t>(flags_base + 0x4));
    r.key("warwatch");    r.value_int(safe_read<uint8_t>(flags_base + 0x5));
    r.key("instabuild");  r.value_int(safe_read<int32_t>(flags_base + 0x8));
    r.key("bypass");      r.value_int(safe_read<uint8_t>(flags_base + ck3::funcs::DEBUG_BYPASS_OFFSET));

    r.key("note"); r.value_string(
        "Toggle debug_mode via execute_console_command with command='debug_mode'. "
        "Script debug output goes to error.log (error_log_scopes) and debug.log "
        "(debug_log_scopes) in the game's log directory.");
    r.end_object();

    if (out) *out = r.take();
    return true;
}

// =============================================================================
// query_character_memories — READ-ONLY enumeration of a character's memories.
//
// Read recipe (re/findings/re_character_memories.md, live-validated 2026-06-20):
//   1. CCharacter* (resolve_character; defaults to controlled if no character_id)
//   2. landed = *(char + 0x1b8); null => no memories (unlanded char) -> empty list
//   3. mem-vector @ landed + char_memory::LIST_IN_LANDED (0x218):
//        data ptr @ +0x00, count(i32) @ +0x0c, element = u32 ref-id (idx|gen<<24)
//   4. per ref-id, resolve the pooled CCharacterMemory* through the ref-database
//        singleton (char_memory::REFDB_GLOBAL): memobj = *(*(DB+0x20)+8+(idx)*0x10);
//        validated by *(memobj + 0x08) == ref_id.
//   5. per memory expose: ref_id, idx, generation, primary participant char
//        (+0x20, with id+name), owner (+0x28), participant count (+0x70), dates
//        (+0xa4 / +0xac), and optional scalars. The memory TYPE NAME is NOT
//        recoverable from the pooled object on this build — confirmed negative by
//        five methods (re_character_memories.md OQ-1) — so "type" is null by design.
//
// SAFE: every dereference goes through safe_read + is_mapped; fail-open to an
// empty/partial list. No writes. PAUSED-only (caller's responsibility, same as
// other reads).
// =============================================================================
bool do_query_character_memories(const char* params_json, std::string* out) {
    if (!out) return false;
    safe_read_clear();

    void* character = resolve_character(params_json);
    if (!character || is_sentinel(character)) {
        *out = R"({"success":false,"error":"character not found"})";
        return false;
    }
    uintptr_t c = reinterpret_cast<uintptr_t>(character);
    uint32_t my_id = char_get_id(character) & 0xFFFFFF;

    ResponseBuilder r;
    r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("character_id"); r.value_uint(my_id);

    // Game-date decode: raw int32 = 24 ticks/day since DATE_EPOCH. Emit "YYYY.MM.DD".
    // Mirrors do_query_opinion_breakdown's formatter. raw<=0 => "none".
    auto format_date = [](int32_t raw_date, char* buf, size_t bufsz) {
        if (raw_date <= 0) { snprintf(buf, bufsz, "none"); return; }
        ck3::game_date::format_raw(raw_date, buf, bufsz);  // ck-05mg
    };

    // Resolve the per-character memory ref-id vector. It lives on landed_data
    // (char + 0x1b8); unlanded characters have no landed_data and thus no
    // memory collection (engine returns a static empty sentinel — FUN_0674f780).
    uintptr_t landed = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    uintptr_t list_data = 0;
    int32_t   list_count = 0;
    if (landed && is_mapped(landed)) {
        uintptr_t vec = landed + char_memory::LIST_IN_LANDED;
        list_data  = safe_read<uintptr_t>(vec + char_memory::LIST_DATA);
        list_count = safe_read<int32_t>(vec + char_memory::LIST_COUNT);
    }

    // Resolve the CCharacterMemory ref-database singleton (TPdxRefDatabase pattern,
    // decompile FUN_0617ff10). The memobj pointer is at the slot BASE
    // (array + REFDB_ENTRY_HDR + idx*stride), NOT slot+8.
    uintptr_t refdb = safe_read_offset<uintptr_t>(char_memory::REFDB_GLOBAL);
    uintptr_t refdb_arr = 0;
    uint32_t  refdb_cap = 0;
    if (refdb && is_mapped(refdb)) {
        refdb_arr = safe_read<uintptr_t>(refdb + char_memory::REFDB_ARRAY);
        refdb_cap = safe_read<uint32_t>(refdb + char_memory::REFDB_CAPACITY);
    }

    // Memory-type database (CCharacterMemoryTypeDatabase singleton). Used to map a
    // type index -> human name. The per-memory type-index FIELD is not yet pinned
    // on this build, so "type" stays null; DB is resolved so type resolution can be
    // wired the moment the field is found (see re_character_memories.md OQ-1).
    uintptr_t type_db = safe_read_offset<uintptr_t>(char_memory::MEMORY_TYPE_DB);
    uintptr_t type_entries = 0;
    uint32_t  type_count = 0;
    if (type_db && is_mapped(type_db)) {
        type_entries = safe_read<uintptr_t>(type_db + ck3::game_db::ENTRIES_ARRAY);
        type_count   = safe_read<uint32_t>(type_db + ck3::game_db::ENTRY_COUNT);
        if (type_count > 4096) type_count = 0;  // sanity guard
    }

    r.key("count"); r.value_int(list_count < 0 ? 0 : list_count);
    r.key("memory_type_db_count"); r.value_uint(type_count);
    r.key("memories"); r.begin_array();

    int emitted = 0;
    bool list_ok = list_data && is_mapped(list_data) && list_count > 0 && list_count < 4096
                   && refdb_arr && is_mapped(refdb_arr) && refdb_cap > 0;
    if (list_ok) {
        for (int32_t i = 0; i < list_count; i++) {
            uint32_t ref_id = safe_read<uint32_t>(list_data + (uintptr_t)i * char_memory::REFID_STRIDE);
            uint32_t idx = ref_id & 0xFFFFFF;
            uint32_t gen = ref_id >> 24;
            if (idx >= refdb_cap) continue;

            // CORRECTED: memobj is at slot BASE, not slot+8 (the old +8 read the
            // freelist marker 0x00ffffff00ffffff and dropped every memory).
            uintptr_t memobj = safe_read<uintptr_t>(
                refdb_arr + char_memory::REFDB_ENTRY_HDR
                + (uintptr_t)idx * char_memory::REFDB_ENTRY_STRIDE);
            if (!memobj || !is_mapped(memobj)) continue;
            // Generation validity: the slot must really hold THIS memory.
            if (safe_read<uint32_t>(memobj + char_memory::OBJ_REF_ID) != ref_id) continue;

            r.begin_object();
            r.key("ref_id"); r.value_uint(ref_id);
            r.key("index"); r.value_uint(idx);
            r.key("generation"); r.value_uint(gen);

            // Owner (+0x28) — the character this memory belongs to (== the queried
            // character for its own list). Serializer tag 0x292d.
            uintptr_t owner = safe_read<uintptr_t>(memobj + char_memory::OBJ_OWNER_CHAR);
            if (owner && is_mapped(owner)) {
                r.key("owner_id");
                r.value_uint(safe_read<uint32_t>(owner + ck3::character::ID) & 0xFFFFFF);
            }

            // Primary/subject participant (+0x20). The "other" character in the
            // memory (killer, lover, deceased relative, …). Serializer tag 0x29ae.
            uintptr_t pchar = safe_read<uintptr_t>(memobj + char_memory::OBJ_PRIMARY_CHAR);
            if (pchar && is_mapped(pchar)) {
                uint32_t pid = safe_read<uint32_t>(pchar + ck3::character::ID) & 0xFFFFFF;
                r.key("primary_participant_id"); r.value_uint(pid);
                std::string pname = ad_read_char_name(pchar);
                if (!pname.empty()) {
                    r.key("primary_participant_name");
                    r.value_string(pname.c_str(), pname.size());
                }
            }

            // Participant sub-collection (data @ +0x68, count @ +0x70). Each element
            // is a script-scope object (stride not flat {id,role}); the {char_id,
            // role-name} decode is an OPEN question (re_character_memories.md OQ-3).
            // We expose the count so callers know how many participants exist.
            uintptr_t pdata = safe_read<uintptr_t>(memobj + char_memory::OBJ_PART_DATA);
            int32_t   pcnt  = safe_read<int32_t>(memobj + char_memory::OBJ_PART_COUNT);
            r.key("participant_count"); r.value_int((pcnt > 0 && pcnt < 256) ? pcnt : 0);
            // Best-effort: do not fabricate roles; report whether the data ptr maps.
            r.key("participants_resolved"); r.value_bool(false);
            (void)pdata;

            // Dates (corrected offsets, serializer tags 0x6b5 / 0x3b04; both decode
            // to sane game years). Emit both raw + formatted.
            int32_t d_creation = safe_read<int32_t>(memobj + char_memory::OBJ_DATE_CREATION);
            int32_t d_end      = safe_read<int32_t>(memobj + char_memory::OBJ_DATE_END);
            char dbuf[24];
            r.key("creation_date_raw"); r.value_int(d_creation);
            format_date(d_creation, dbuf, sizeof(dbuf));
            r.key("creation_date"); r.value_string(dbuf);
            r.key("end_date_raw"); r.value_int(d_end);
            format_date(d_end, dbuf, sizeof(dbuf));
            r.key("end_date"); r.value_string(dbuf);

            // Optional scalar fields.
            int32_t var_int = safe_read<int32_t>(memobj + char_memory::OBJ_VAR_INT);
            if (var_int != -1) { r.key("var_int"); r.value_int(var_int); }
            int64_t relevance = safe_read<int64_t>(memobj + char_memory::OBJ_RELEVANCE);
            if (relevance != -100000) { r.key("relevance_raw"); r.value_int(relevance); }

            // Type name: NOT recoverable from the pooled 0xd8 memory object on this
            // build. Confirmed by five independent methods on populated LIVE memobjs
            // (re_character_memories.md OQ-1, "RESOLVED-AS-NEGATIVE 2026-06-20"): no
            // u32 type-index field, no single-/two-hop pointer to a CCharacterMemoryType,
            // and the engine's own serializer (FUN_064d3ae0) never writes a type. The
            // engine resolves a memory's type only via a virtual comparator on a
            // transient scope-list-builder — a CALL the read-only bridge cannot make.
            // The type DB itself IS resolved (see memory_type_db_count) so name lookup
            // is ready the day a safe in-engine call primitive exists. Stays null.
            r.key("type"); r.value_null();

            r.end_object();
            emitted++;
        }
    }
    r.end_array();
    r.key("emitted"); r.value_int(emitted);

    if (!landed || !is_mapped(landed)) {
        r.key("note"); r.value_string("character is unlanded (no memory collection)");
    } else if (refdb && (!refdb_arr || refdb_cap == 0)) {
        r.key("note"); r.value_string("memory ref-database not populated (empty refdb)");
    }

    if (safe_read_had_fault()) {
        r.key("degraded"); r.value_bool(true);
        r.key("fault_count"); r.value_int(safe_read_fault_count());
        log_degraded_read("query_character_memories");
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

} // namespace bridge
