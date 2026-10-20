// phase3_probes.cpp — Phase-3 Wave-2 probe handlers (ck-e58l.8)
//
// Five read-only probes that convert silent failures in the interaction +
// war-creation pipelines into loud diagnostics. See re/findings/re_interaction_trysend_cascade_v3.md,
// re_fun_07628500_script_trigger_evaluator.md, re_pending_interaction_consumer.md,
// re_war_create_core_decomp.md, re_consumer_cansend_state_drift_static.md.
//
//   1. probe_interaction_cascade  — FUN_06b42310 with non-NULL error collector
//   2. probe_gate_4_slots         — FUN_07628500 per Gate-4 slot
//   3. list_pending_interactions  — walk CPendingCharacterInteractionManager
//   4. tick_diff                  — snapshot state, diff across ticks
//   5. probe_war_orphans          — compare WAR_DB vs game-state active-wars
//
// Probes 3 and 5 are pure memory reads and run inline. Probes 1, 2, and 4 need
// game-thread access (TLS context + tick advance); they submit deferred queries
// processed in drain_and_execute_intents and poll for completion.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // for memmem
#endif

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "bridge.h"
#include "ck3_offsets.h"

#include <iterator>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace bridge {

// External state — snapshot classifier + global tick id
extern std::atomic<uint64_t> g_tick_id;

// ============================================================================
// Singleton resolution — CPendingCharacterInteractionManager
// ============================================================================
//
// The manager is a heap-allocated game-manager singleton. Empirically (ck-r56i
// core-dump trace) it lives in a glibc mmap-arena 700+MB from the binary base,
// in a ~128MB anonymous region — NOT in .data/.bss and NOT in the brk heap.
//
// Strategy: scan all writable anonymous mappings for the 16-byte signature
// [primary_vtable][secondary_vtable]. Both vtables are known compile-time
// constants (base + PENDING_MGR_VTABLE and base + PENDING_MGR_SEC_VTABLE),
// so the 16-byte match is essentially unique — no false positives observed.
// DB-ref at +0x50 is still sanity-checked.
//
// Cached after first resolution. Scan stats are surfaced to callers via
// g_pending_mgr_scan_stats for diagnostics when the scan fails.

struct PendingMgrScanStats {
    uint32_t regions_considered;
    uint32_t regions_scanned;
    uint64_t bytes_scanned;
    uint32_t sig_matches;
    uint32_t rejected_bad_sec_vt;
    uint32_t rejected_bad_dbref;
    uint32_t skipped_size_cap;
    uint32_t skipped_read_fail;
    bool     aborted_budget;
    bool     ran;
};

static std::atomic<uintptr_t> g_pending_mgr_cached{0};
static std::mutex             g_pending_mgr_stats_mutex;
static PendingMgrScanStats    g_pending_mgr_scan_stats{};

// Safety cap: never scan more than this many bytes across all regions.
// A full rw-p footprint of the game is ~10GB; we cap at 16GB to be safe.
static constexpr uint64_t PENDING_MGR_SCAN_BUDGET = 16ULL * 1024 * 1024 * 1024;

// Per-region size cap. The manager's region is ~128MB in the 867 save;
// allow up to 512MB to be robust against larger saves.
static constexpr uint64_t PENDING_MGR_REGION_CAP  = 512ULL * 1024 * 1024;

static uintptr_t resolve_pending_manager_singleton() {
    uintptr_t cached = g_pending_mgr_cached.load(std::memory_order_relaxed);
    if (cached) return cached;

    uintptr_t vtable_runtime = g_base_addr + ::event_weight::PENDING_MGR_VTABLE;
    uintptr_t sec_vt_runtime = g_base_addr + ::event_weight::PENDING_MGR_SEC_VTABLE;
    if (!is_mapped(vtable_runtime) || !is_mapped(sec_vt_runtime)) return 0;

    PendingMgrScanStats stats{};
    stats.ran = true;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        std::lock_guard<std::mutex> lk(g_pending_mgr_stats_mutex);
        g_pending_mgr_scan_stats = stats;
        return 0;
    }

    // 16-byte signature: [primary_vt_qword][secondary_vt_qword]
    uint64_t sig[2] = { vtable_runtime, sec_vt_runtime };
    const unsigned char* sig_bytes = reinterpret_cast<const unsigned char*>(sig);

    uintptr_t found = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        char perms[8] = {};
        int n = sscanf(line, "%lx-%lx %7s", &start, &end, perms);
        if (n < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;

        stats.regions_considered++;

        size_t region_size = end - start;
        if (region_size > PENDING_MGR_REGION_CAP) {
            stats.skipped_size_cap++;
            continue;
        }

        if (stats.bytes_scanned + region_size > PENDING_MGR_SCAN_BUDGET) {
            stats.aborted_budget = true;
            break;
        }

        stats.regions_scanned++;

        // Scan in 1 MB chunks, overlapping by 8 bytes so a signature straddling
        // a chunk boundary isn't missed.
        constexpr size_t CHUNK = 1024 * 1024;
        std::vector<unsigned char> buf(CHUNK);
        uintptr_t addr = start;
        while (addr < end) {
            size_t want = std::min<size_t>(CHUNK, end - addr);
            if (!safe_read_bytes(addr, buf.data(), want)) {
                stats.skipped_read_fail++;
                addr += want;
                continue;
            }
            stats.bytes_scanned += want;

            // Find signature occurrences in this chunk
            const unsigned char* haystack = buf.data();
            size_t pos = 0;
            while (pos + sizeof(sig) <= want) {
                const unsigned char* m = static_cast<const unsigned char*>(
                    memmem(haystack + pos, want - pos, sig_bytes, sizeof(sig)));
                if (!m) break;
                size_t off = static_cast<size_t>(m - haystack);
                uintptr_t candidate = addr + off;
                // Alignment check on the ABSOLUTE address, not the in-chunk
                // offset: chunks stride by CHUNK - (sizeof(sig) - 1) = 0xffff1
                // which is not 8-byte aligned, so after chunk 1 the in-chunk
                // offset of an aligned absolute match is nonzero mod 8 and
                // would be incorrectly rejected (ck-1hl6).
                if ((candidate & 0x7) == 0) {
                    stats.sig_matches++;
                    uintptr_t sec = safe_read<uintptr_t>(candidate + 0x08);
                    if (sec != sec_vt_runtime) {
                        stats.rejected_bad_sec_vt++;
                    } else {
                        uintptr_t dbref = safe_read<uintptr_t>(candidate + 0x50);
                        if (!is_mapped(dbref)) {
                            stats.rejected_bad_dbref++;
                        } else {
                            found = candidate;
                            ADLOG("phase3_probes: resolved pending_mgr singleton at 0x%lx "
                                  "(region %lx-%lx, scanned %lu regions / %lu MB)\n",
                                  candidate, start, end,
                                  (unsigned long)stats.regions_scanned,
                                  (unsigned long)(stats.bytes_scanned / (1024 * 1024)));
                            break;
                        }
                    }
                }
                pos = off + 1;
            }

            if (found) break;
            // Overlap by sizeof(sig) - 1 so a straddling match isn't missed.
            if (want < CHUNK) { addr += want; } else { addr += want - (sizeof(sig) - 1); }
        }

        if (found) break;
    }
    fclose(f);

    {
        std::lock_guard<std::mutex> lk(g_pending_mgr_stats_mutex);
        g_pending_mgr_scan_stats = stats;
    }

    if (found) {
        g_pending_mgr_cached.store(found, std::memory_order_relaxed);
        return found;
    }
    ADLOG("phase3_probes: pending_mgr singleton NOT FOUND — regions_scanned=%u "
          "sig_matches=%u bad_sec_vt=%u bad_dbref=%u aborted=%d\n",
          stats.regions_scanned, stats.sig_matches,
          stats.rejected_bad_sec_vt, stats.rejected_bad_dbref,
          stats.aborted_budget ? 1 : 0);
    return 0;
}

static void emit_pending_mgr_scan_stats(ResponseBuilder& r) {
    PendingMgrScanStats s;
    {
        std::lock_guard<std::mutex> lk(g_pending_mgr_stats_mutex);
        s = g_pending_mgr_scan_stats;
    }
    r.key("scan_stats");
    r.begin_object();
    r.key("ran"); r.value_bool(s.ran);
    r.key("regions_considered"); r.value_uint(s.regions_considered);
    r.key("regions_scanned"); r.value_uint(s.regions_scanned);
    r.key("bytes_scanned"); r.value_uint(static_cast<uint32_t>(s.bytes_scanned / (1024 * 1024)));
    r.key("sig_matches"); r.value_uint(s.sig_matches);
    r.key("rejected_bad_sec_vt"); r.value_uint(s.rejected_bad_sec_vt);
    r.key("rejected_bad_dbref"); r.value_uint(s.rejected_bad_dbref);
    r.key("skipped_size_cap"); r.value_uint(s.skipped_size_cap);
    r.key("skipped_read_fail"); r.value_uint(s.skipped_read_fail);
    r.key("aborted_budget"); r.value_bool(s.aborted_budget);
    r.end_object();
}

// ============================================================================
// WAR_DB + active-wars helpers (for probes 3 and 5)
// ============================================================================

struct WarSlotInfo {
    uint32_t war_id;         // 24-bit masked index
    uint32_t slot_index;     // DB slot
    uintptr_t war_ptr;
    uint32_t attacker_id;
    uint32_t defender_id;
    uint32_t cb_type_ref;
    std::string cb_type;          // rc2 B2: name of the CB type at war+0xA0
    int32_t  start_date;
    bool     is_ended;
};

// Enumerate all live war entries in WAR_DB.
// Returns slots whose payload pointer is mapped and whose stored war_id
// matches the slot index (standard validity check).
static std::vector<WarSlotInfo> enumerate_war_db() {
    std::vector<WarSlotInfo> out;
    uintptr_t war_db_ptr = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (!war_db_ptr || !is_mapped(war_db_ptr)) return out;

    uintptr_t table = safe_read<uintptr_t>(war_db_ptr + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(war_db_ptr + ck3::hash_map::CAPACITY);
    if (!table || !is_mapped(table)) return out;
    if (capacity == 0 || capacity > 100000) return out;

    out.reserve(capacity);
    for (uint32_t idx = 0; idx < capacity && idx < 10000; idx++) {
        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
        uintptr_t wp = safe_read<uintptr_t>(entry);
        if (!wp || !is_mapped(wp)) continue;
        uint32_t stored_ref = safe_read<uint32_t>(wp + ck3::war_obj::WAR_ID);
        if ((stored_ref & 0xFFFFFF) != idx) continue;

        WarSlotInfo info{};
        info.war_id = stored_ref & 0xFFFFFF;
        info.slot_index = idx;
        info.war_ptr = wp;
        // Primary attacker/defender/CB refs per ck-e58l.5 §3
        info.attacker_id = safe_read<uint32_t>(wp + 0x228) & 0xFFFFFF;
        info.defender_id = safe_read<uint32_t>(wp + 0x22C) & 0xFFFFFF;
        info.cb_type_ref = safe_read<uint32_t>(wp + 0xA0);
        if (uintptr_t cbt = safe_read<uintptr_t>(wp + ck3::war_obj::CB_TYPE_REF);
            cbt && is_mapped(cbt)) {
            const char* nm = nullptr;
            size_t nl = 0;
            if (read_db_obj_name(cbt, nm, nl) && nm) info.cb_type.assign(nm, nl);
        }
        info.start_date  = safe_read<int32_t>(wp + 0x80);
        info.is_ended    = safe_read<uint8_t>(wp + ck3::war_obj::IS_ENDED) != 0;
        out.push_back(info);
    }
    return out;
}

// Raw read of the fields ck-e58l.5 Block K *decomp-inferred* to be the
// game-state active-wars (ptr/cap/count/allocator at gs+0x28/0x30/0x34/0x38).
// ck-ltsh (2026-04-21) found the inference is WRONG: on both the 867 AD core
// dump and the live 1097 AD session, gs+0x20 is a small int (0x24), gs+0x28
// is 0xffffffff sentinel, and gs+0x34 is garbage. WAR_CREATE_CORE's param_1
// is not *(GAME_STATE_ROOT) — it's a different struct (likely a CWarManager)
// that we have not yet pinned. Until then we cannot read the true active-wars
// array. The raw read is still emitted as a diagnostic so future RE can see
// when/if the sentinel flips to a real pointer.
struct ActiveWarsRaw {
    uintptr_t arr_ptr;
    int32_t   capacity;
    int32_t   count;
    uintptr_t allocator;
    bool      mapped_gs;
};

static ActiveWarsRaw read_active_wars_raw() {
    ActiveWarsRaw r{0, 0, 0, 0, false};
    uintptr_t gs_ptr = safe_read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    if (!gs_ptr || !is_mapped(gs_ptr)) return r;
    r.mapped_gs = true;
    r.arr_ptr   = safe_read<uintptr_t>(gs_ptr + ck3::game_state::ACTIVE_WARS_PTR);
    r.capacity  = safe_read<int32_t>(gs_ptr + ck3::game_state::ACTIVE_WARS_CAPACITY);
    r.count     = safe_read<int32_t>(gs_ptr + ck3::game_state::ACTIVE_WARS_COUNT);
    r.allocator = safe_read<uintptr_t>(gs_ptr + ck3::game_state::ACTIVE_WARS_ALLOCATOR);
    return r;
}

// Returns the decoded active-wars id set IF the raw fields look self-consistent
// (mapped ptr, non-negative count <= capacity, reasonable size). Returns an
// empty vector when the fields look like the known-broken sentinel pattern, so
// callers can distinguish "really empty" from "read target is wrong".
static bool read_active_wars_array(std::vector<uint32_t>& out, ActiveWarsRaw& raw_out) {
    out.clear();
    raw_out = read_active_wars_raw();
    if (!raw_out.mapped_gs) return false;
    if (!raw_out.arr_ptr || !is_mapped(raw_out.arr_ptr)) return false;
    if (raw_out.count < 0 || raw_out.capacity < 0) return false;
    if (raw_out.count > raw_out.capacity || raw_out.count > 10000) return false;

    out.reserve(static_cast<size_t>(raw_out.count));
    for (int32_t i = 0; i < raw_out.count; i++) {
        uint32_t id = safe_read<uint32_t>(raw_out.arr_ptr + i * 4);
        out.push_back(id & 0xFFFFFF);
    }
    return true;
}

// ============================================================================
// Probe 3: list_pending_interactions
// ============================================================================
//
// Walk CPendingCharacterInteractionManager per ck-1ps.5 + ck-e58l.1 layout:
//   mgr +0x20: Phase 1 ID array ptr
//   mgr +0x2C: Phase 1 count
//   mgr +0x38: Phase 2 ID array ptr
//   mgr +0x44: Phase 2 count
//   mgr +0x50: TPdxRefDatabase reference (table +0x20, capacity +0x2C)
//
// Entry layout (0x5C8 bytes, ck-1ps.5):
//   +0x000 primary vtable
//   +0x008 secondary vtable
//   +0x010 DB id (24-bit masked)
//   +0x018 deep-copied ctx
//     +0x018+0x000 ctx[0] (interaction_def*)
//     +0x018+0x2D8 actor_id
//     +0x018+0x2DC target_id
//     +0x018+0x2E0 secondary/war_target
//     +0x018+0x2E4 claimant
//   +0x5C0 state (1=pending, 2=init-accepted, 3=fully-accepted)
//
// ck-e58l.1 §3.1 notes the deep copy moves these fields to entry+0x2F0..+0x2FC
// relative to the raw DB slot. Because entry+0x18 is the ctx base, the context
// offsets (0x2D8 etc.) are correct relative to that pointer — read through
// entry+0x18 + 0x2D8.

static void emit_pending_entry(ResponseBuilder& r, uintptr_t entry_ptr) {
    uintptr_t ctx = entry_ptr + 0x18;
    uintptr_t def_ptr = safe_read<uintptr_t>(ctx);
    uint32_t actor  = safe_read<uint32_t>(ctx + 0x2D8) & 0xFFFFFF;
    uint32_t target = safe_read<uint32_t>(ctx + 0x2DC) & 0xFFFFFF;
    uint32_t secondary = safe_read<uint32_t>(ctx + 0x2E0) & 0xFFFFFF;
    uint32_t claimant  = safe_read<uint32_t>(ctx + 0x2E4) & 0xFFFFFF;
    uint32_t db_id  = safe_read<uint32_t>(entry_ptr + 0x10) & 0xFFFFFF;
    int32_t  state  = safe_read<int32_t>(entry_ptr + 0x5C0);

    r.begin_object();
    r.key("db_id"); r.value_uint(db_id);
    r.key("actor_id"); r.value_uint(actor);
    r.key("target_id"); r.value_uint(target);
    if (secondary) { r.key("secondary_id"); r.value_uint(secondary); }
    if (claimant)  { r.key("claimant_id");  r.value_uint(claimant);  }
    r.key("state"); r.value_int(state);

    // Interaction name (via the def)
    if (def_ptr && is_mapped(def_ptr)) {
        const char* name = nullptr;
        size_t len = 0;
        if (read_db_obj_name(def_ptr, name, len) && name) {
            r.key("interaction_name"); r.value_string(name, len);
        }
    }

#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%lx", entry_ptr);
    r.key("entry_ptr"); r.value_string(buf);
#endif
    r.end_object();
}

bool do_list_pending_interactions(const char* /*params_json*/, std::string* response_out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("list_pending_interactions");

    uintptr_t mgr = resolve_pending_manager_singleton();
    if (!mgr) {
        r.key("pending"); r.begin_array(); r.end_array();
        r.key("count"); r.value_int(0);
        r.key("error"); r.value_string("pending_manager_singleton_not_resolved");
        r.key("hint"); r.value_string("16-byte [primary_vtable][secondary_vtable] signature "
                                       "not found in any writable mapping. If sig_matches > 0, "
                                       "check rejected_* counters. If sig_matches == 0, the "
                                       "vtable offsets may have drifted — re-RE against current binary.");
        emit_pending_mgr_scan_stats(r);
        r.end_object();
        if (response_out) *response_out = r.take();
        return true;
    }

    // Read DB (primary storage) and iterate all valid slots.
    uintptr_t dbref = safe_read<uintptr_t>(mgr + 0x50);
    r.key("pending"); r.begin_array();
    int emitted = 0;
    if (dbref && is_mapped(dbref)) {
        uintptr_t table = safe_read<uintptr_t>(dbref + ck3::hash_map::TABLE_PTR);
        uint32_t cap = safe_read<uint32_t>(dbref + ck3::hash_map::CAPACITY);
        if (table && is_mapped(table) && cap > 0 && cap < 100000) {
            for (uint32_t idx = 0; idx < cap && idx < 10000; idx++) {
                // Same TPdxRefDatabase shape as WAR_DB — walk via ck3::hash_map.
                uintptr_t entry_slot = table + ck3::hash_map::ENTRY_HEADER_SIZE
                                       + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                uintptr_t entry = safe_read<uintptr_t>(entry_slot);
                if (!entry || !is_mapped(entry)) continue;
                uint32_t stored_id = safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) & 0xFFFFFF;
                if (stored_id != idx) continue;
                // Sanity: vtable[0] should equal PENDING_INTERACTION_VTABLE
                uintptr_t entry_vt = safe_read<uintptr_t>(entry);
                uintptr_t want_vt = g_base_addr + ::event_weight::PENDING_INTERACTION_VTABLE;
                if (entry_vt != want_vt) continue;

                emit_pending_entry(r, entry);
                emitted++;
            }
        }
    }
    r.end_array();

    // Also emit Phase 1 + Phase 2 "to-process" ID arrays for debugging.
    r.key("phase1_ids"); r.begin_array();
    {
        uintptr_t arr = safe_read<uintptr_t>(mgr + 0x20);
        int32_t cnt  = safe_read<int32_t>(mgr + 0x2C);
        if (arr && is_mapped(arr) && cnt > 0 && cnt < 10000) {
            for (int32_t i = 0; i < cnt; i++) {
                uint32_t id = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
                r.value_uint(id);
            }
        }
    }
    r.end_array();

    r.key("phase2_ids"); r.begin_array();
    {
        uintptr_t arr = safe_read<uintptr_t>(mgr + 0x38);
        int32_t cnt  = safe_read<int32_t>(mgr + 0x44);
        if (arr && is_mapped(arr) && cnt > 0 && cnt < 10000) {
            for (int32_t i = 0; i < cnt; i++) {
                uint32_t id = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
                r.value_uint(id);
            }
        }
    }
    r.end_array();

    r.key("count"); r.value_int(emitted);
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
    {
        char mgr_hex[32];
        snprintf(mgr_hex, sizeof(mgr_hex), "0x%lx", mgr);
        r.key("manager_ptr"); r.value_string(mgr_hex);
    }
#endif
    emit_pending_mgr_scan_stats(r);
    r.end_object();
    if (response_out) *response_out = r.take();
    return true;
}

// ============================================================================
// Probe 5: probe_war_orphans
// ============================================================================
//
// SF3 detector per ck-e58l.5 §5: WAR_DB entries absent from the game-state
// active-wars int32 array. These are wars whose vtable[2] validity check
// failed in Block J — on_declaration already fired but the caller sees null.

bool do_probe_war_orphans(const char* /*params_json*/, std::string* response_out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("probe_war_orphans");

    auto db_entries = enumerate_war_db();

    std::vector<uint32_t> active_wars;
    ActiveWarsRaw raw{};
    bool active_wars_trustworthy = read_active_wars_array(active_wars, raw);

    // SF3 orphan detection requires the game-state active-wars array as ground
    // truth for "wars the caller path committed to". ck-ltsh (2026-04-21)
    // established that the ck3_offsets.h ACTIVE_WARS_* offsets are on the
    // wrong struct (ck-e58l.5 decomp inferred param_1=*(GAME_STATE_ROOT) but
    // the gs+0x20 field is not a pointer). Until the true CWarManager
    // location is found, we cannot distinguish save-loaded wars from SF3
    // orphans: on a freshly loaded save, every WAR_DB entry looks like an
    // orphan, giving 100% false positives. The honest answer is: report
    // WAR_DB state as ground truth, emit raw diagnostic fields so future
    // RE can confirm when/where the real active-wars struct moves to.

    int active_count = 0;
    int db_active = 0;
    for (const auto& w : db_entries) {
        if (!w.is_ended) db_active++;
    }

    r.key("orphans"); r.begin_array();
    r.end_array();
    r.key("count"); r.value_int(0);

    r.key("war_db_total"); r.value_int(db_active);
    r.key("active_wars_total"); r.value_int(
        active_wars_trustworthy ? static_cast<int>(active_wars.size()) : -1);

    // Diagnostic block: raw ACTIVE_WARS_* fields + heuristic classification.
    // Keep this keyed "diagnostic" to discourage callers from treating it
    // as the primary result.
    r.key("diagnostic"); r.begin_object();
    r.key("active_wars_offsets_trustworthy"); r.value_bool(active_wars_trustworthy);
    r.key("game_state_mapped"); r.value_bool(raw.mapped_gs);
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%lx", raw.arr_ptr);
        r.key("active_wars_ptr_raw"); r.value_string(buf);
    }
#endif
    r.key("active_wars_capacity_raw"); r.value_int(raw.capacity);
    r.key("active_wars_count_raw"); r.value_int(raw.count);
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%lx", raw.allocator);
        r.key("active_wars_allocator_raw"); r.value_string(buf);
    }
#endif
    r.key("note"); r.value_string(
        "SF3 orphan detection disabled: ck-ltsh proved the ACTIVE_WARS_* "
        "offsets in ck3_offsets.h are on the wrong struct. WAR_DB scan is "
        "the authoritative 'live wars' source until the true CWarManager "
        "location is found. See bead ck-ltsh for the investigation.");
    r.end_object();

    // Emit the WAR_DB entries as the actual list of active wars so callers
    // still get useful data in the 'wars' field (replaces the old 'orphans'
    // shape callers consumed).
    r.key("wars"); r.begin_array();
    for (const auto& w : db_entries) {
        if (w.is_ended) continue;
        r.begin_object();
        r.key("war_id"); r.value_uint(w.war_id);
        r.key("db_slot"); r.value_uint(w.slot_index);
        r.key("attacker"); r.value_uint(w.attacker_id);
        r.key("defender"); r.value_uint(w.defender_id);
        // rc2 B2: the CB type by name. The raw field is the low half of a
        // CCasusBelliType* (war+0xA0 is a uint64 pointer), so it and war_ptr
        // are debug-only.
        if (!w.cb_type.empty()) { r.key("cb_type"); r.value_string(w.cb_type.c_str()); }
        r.key("start_date"); r.value_int(w.start_date);
#ifdef CK3_BRIDGE_DEBUG_VERBS   // rc2 B2: a process VA defeats ASLR; debug builds only
        r.key("cb_type_ref"); r.value_uint(w.cb_type_ref);
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%lx", w.war_ptr);
            r.key("war_ptr"); r.value_string(buf);
        }
#endif
        r.end_object();
        active_count++;
    }
    r.end_array();

    r.end_object();
    if (response_out) *response_out = r.take();
    (void)active_count;
    return true;
}

// ============================================================================
// tick_diff — snapshot ring buffer
// ============================================================================

struct TickSnapshot {
    std::string tag;
    uint64_t    tick_id;
    int32_t     game_date;

    std::vector<uint32_t> wars;              // active war IDs (from WAR_DB, not ended)
    std::vector<uint32_t> war_orphans;       // WAR_DB ids not in active-wars array
    std::vector<uint32_t> pending_ids;       // DB ids of pending interactions
    // Truces omitted — offsets not yet mapped.
};

static std::mutex g_snapshot_mutex;
static constexpr size_t SNAPSHOT_RING_SIZE = 10;
static std::vector<TickSnapshot> g_snapshot_ring CK3_EARLY_INIT;   // growable up to SNAPSHOT_RING_SIZE

static TickSnapshot capture_snapshot() {
    TickSnapshot s;
    s.tick_id = g_tick_id.load(std::memory_order_relaxed);
    uintptr_t gs = safe_read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    if (gs) s.game_date = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);

    auto wars = enumerate_war_db();
    // Orphan detection is disabled until the true active-wars struct is
    // located (ck-ltsh). Leaving s.war_orphans empty keeps tick_diff's
    // war_orphans channel silent instead of mirroring the full wars delta.
    s.wars.reserve(wars.size());
    for (const auto& w : wars) {
        if (w.is_ended) continue;
        s.wars.push_back(w.war_id);
    }
    std::sort(s.wars.begin(), s.wars.end());

    // Pending interactions
    uintptr_t mgr = resolve_pending_manager_singleton();
    if (mgr) {
        uintptr_t dbref = safe_read<uintptr_t>(mgr + 0x50);
        if (dbref && is_mapped(dbref)) {
            uintptr_t table = safe_read<uintptr_t>(dbref + ck3::hash_map::TABLE_PTR);
            uint32_t cap = safe_read<uint32_t>(dbref + ck3::hash_map::CAPACITY);
            if (table && is_mapped(table) && cap > 0 && cap < 100000) {
                uintptr_t want_vt = g_base_addr + ::event_weight::PENDING_INTERACTION_VTABLE;
                for (uint32_t idx = 0; idx < cap && idx < 10000; idx++) {
                    uintptr_t slot = table + ck3::hash_map::ENTRY_HEADER_SIZE
                                     + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                    uintptr_t entry = safe_read<uintptr_t>(slot);
                    if (!entry || !is_mapped(entry)) continue;
                    uint32_t stored = safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) & 0xFFFFFF;
                    if (stored != idx) continue;
                    uintptr_t vt = safe_read<uintptr_t>(entry);
                    if (vt != want_vt) continue;
                    s.pending_ids.push_back(idx);
                }
            }
        }
    }
    std::sort(s.pending_ids.begin(), s.pending_ids.end());
    return s;
}

static void store_snapshot(TickSnapshot s) {
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (g_snapshot_ring.size() >= SNAPSHOT_RING_SIZE) {
        g_snapshot_ring.erase(g_snapshot_ring.begin());
    }
    g_snapshot_ring.push_back(std::move(s));
}

static bool find_snapshot_by_tag(const char* tag, TickSnapshot& out) {
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    for (const auto& s : g_snapshot_ring) {
        if (s.tag == tag) { out = s; return true; }
    }
    return false;
}

static void emit_uint_diff(ResponseBuilder& r, const char* key,
                           const std::vector<uint32_t>& before,
                           const std::vector<uint32_t>& after) {
    // before/after are sorted
    std::vector<uint32_t> added, removed;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                        std::back_inserter(removed));
    r.key(key); r.begin_object();
    r.key("added"); r.begin_array();
    for (uint32_t v : added) r.value_uint(v);
    r.end_array();
    r.key("removed"); r.begin_array();
    for (uint32_t v : removed) r.value_uint(v);
    r.end_array();
    r.end_object();
}

bool do_tick_diff(const char* params_json, std::string* response_out) {
    const char* before_tag = json_get_string(params_json, "before_tag");
    bool store_as = json_has_key(params_json, "store_as");
    const char* store_tag = store_as ? json_get_string(params_json, "store_as") : nullptr;

    TickSnapshot before;
    bool have_before = false;

    // ck-4vg9s: snapshot pause state alongside tick counters so harness
    // preflight can distinguish "game paused, no tick advance possible" from
    // "game ticking but state happened not to move." With multiple polecats
    // sharing one bridge, a sibling's pause_game can no-op our unpause and
    // strand this scenario in tick_from==tick_to.
    bool paused_before = is_paused();

    if (before_tag && before_tag[0]) {
        have_before = find_snapshot_by_tag(before_tag, before);
        if (!have_before) {
            ResponseBuilder r; r.begin_object();
            r.key("action"); r.value_string("tick_diff");
            r.key("error"); r.value_string("before_tag_not_found");
            r.key("before_tag"); r.value_string(before_tag);
            r.end_object();
            if (response_out) *response_out = r.take();
            return true;
        }
    } else {
        // No tag provided — capture NOW, wait for tick advance, capture again
        before = capture_snapshot();
        have_before = true;
    }

    TickSnapshot after;
    if (before_tag && before_tag[0]) {
        // Caller supplied a tag — just diff against current state
        after = capture_snapshot();
    } else {
        // Wait up to ~3s for tick to advance
        uint64_t start_tick = before.tick_id;
        for (int i = 0; i < 60; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint64_t now = g_tick_id.load(std::memory_order_relaxed);
            if (now != start_tick) break;
        }
        after = capture_snapshot();
    }

    bool paused_after = is_paused();

    if (store_tag && store_tag[0]) {
        TickSnapshot copy = after;
        copy.tag = store_tag;
        store_snapshot(std::move(copy));
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("tick_diff");
    r.key("tick_advanced_from"); r.value_int(before.tick_id);
    r.key("tick_advanced_to"); r.value_int(after.tick_id);
    r.key("game_date_before"); r.value_int(before.game_date);
    r.key("game_date_after"); r.value_int(after.game_date);
    r.key("paused_before"); r.value_bool(paused_before);
    r.key("paused_after"); r.value_bool(paused_after);

    emit_uint_diff(r, "wars", before.wars, after.wars);
    emit_uint_diff(r, "pending", before.pending_ids, after.pending_ids);
    emit_uint_diff(r, "war_orphans", before.war_orphans, after.war_orphans);

    r.end_object();
    if (response_out) *response_out = r.take();
    return true;
}

// ============================================================================
// Probes 1 + 2: cascade + gate-4 slots — deferred to game thread
// ============================================================================
//
// These build a scope context via the TLS pipeline (CTX_INIT/RESET/SET_TARGET)
// and invoke validation functions. Both use a shared query struct.

struct CascadeQuery {
    // --- Request ---
    uint32_t actor_id;
    uint32_t target_id;
    char interaction_type[128];
    uint32_t cb_type_id;       // 0 = none
    uint32_t claimant_id;      // 0 = none
    int mode;                  // 0 = full cascade, 1 = gate-4 slots only
    // Opt-in: extract loc keys from the loud-failure evaluator on the first
    // failing Gate 4 slot. Adds one FUN_0763ddc0 call per failing slot; all
    // calls are sigjmp-guarded. Off by default (ck-wtk7).
    bool extract_loc_keys;

    // --- Result ---
    bool approved;
    char rejection_summary[128];   // e.g. "GATE_1_LIVENESS", "GATE_4_SLOT_IS_SHOWN"
    char rejection_key[96];        // extracted localization key, if found
    // Per-slot results (gate-4 probe)
    struct SlotResult {
        int      slot_idx;
        char     name[48];
        uint32_t offset;
        bool     present;
        bool     passed;
    } slots[8];
    int slot_count;
    char error[128];

    // --- Sync: 0=empty, 1=pending, 2=done, 3=error, 4=in-progress ---
    std::atomic<int> state{0};
};

static CascadeQuery g_cascade_query CK3_EARLY_INIT;

bool cascade_query_pending() {
    return g_cascade_query.state.load(std::memory_order_acquire) == 1;
}

// Game-thread processor — called from drain_and_execute_intents.
// Uses the existing TLS pipeline helpers that live in ai_eval_hook.cpp. To
// avoid duplicating the TLS construction here, we delegate to the trigger
// query (process_trigger_query) which already handles gate-4 slot evaluation.
// For the "full cascade" probe, we invoke INTERACTION_TRY_SEND_CASCADE with a
// 256-byte zeroed scratch buffer as param_4. If the function returns 0, we
// scan the buffer for the first printable ASCII string (best-effort).
//
// Function is defined below in this file. It must be called only from the
// game thread (has TLS access).
void process_cascade_query();

bool do_probe_interaction_cascade(const char* params_json, std::string* response_out) {
    const char* type = json_get_string(params_json, "interaction_name");
    if (!type || !type[0]) type = json_get_string(params_json, "interaction_type");
    if (!type || !type[0]) {
        if (response_out) *response_out = R"({"error":"missing_interaction_name"})";
        return false;
    }

    int actor_id = json_get_int(params_json, "actor_id");
    int target_id = json_get_int(params_json, "target_id");
    if (actor_id <= 0 || target_id <= 0) {
        if (response_out) *response_out = R"({"error":"missing_actor_or_target"})";
        return false;
    }

    int cb_type_id = json_get_int(params_json, "cb_type_id");
    int claimant_id = json_get_int(params_json, "claimant_id");
    bool extract_loc_keys = json_get_bool(params_json, "extract_loc_keys", false);

    int current_state = g_cascade_query.state.load(std::memory_order_acquire);
    if (current_state == 1 || current_state == 4) {
        if (response_out) *response_out = R"({"error":"cascade_query_busy"})";
        return true;
    }
    g_cascade_query.state.store(0, std::memory_order_release);

    g_cascade_query.actor_id = static_cast<uint32_t>(actor_id);
    g_cascade_query.target_id = static_cast<uint32_t>(target_id);
    strncpy(g_cascade_query.interaction_type, type,
            sizeof(g_cascade_query.interaction_type) - 1);
    g_cascade_query.interaction_type[sizeof(g_cascade_query.interaction_type) - 1] = '\0';
    g_cascade_query.cb_type_id = cb_type_id > 0 ? static_cast<uint32_t>(cb_type_id) : 0;
    g_cascade_query.claimant_id = claimant_id > 0 ? static_cast<uint32_t>(claimant_id) : 0;
    g_cascade_query.mode = 0;
    g_cascade_query.extract_loc_keys = extract_loc_keys;
    g_cascade_query.approved = false;
    g_cascade_query.rejection_summary[0] = '\0';
    g_cascade_query.rejection_key[0] = '\0';
    g_cascade_query.slot_count = 0;
    g_cascade_query.error[0] = '\0';
    g_cascade_query.state.store(1, std::memory_order_release);

    // Poll up to 3s for completion
    bool done = false;
    for (int i = 0; i < 60; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int st = g_cascade_query.state.load(std::memory_order_acquire);
        if (st == 2 || st == 3) { done = true; break; }
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("probe_interaction_cascade");
    r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);
    r.key("interaction_name"); r.value_string(type);

    if (!done) {
        r.key("error"); r.value_string("timeout_waiting_for_game_thread");
        r.key("hint"); r.value_string("Game must be unpaused for at least one tick.");
        g_cascade_query.state.store(0, std::memory_order_release);
        r.end_object();
        if (response_out) *response_out = r.take();
        return true;
    }

    int st = g_cascade_query.state.load(std::memory_order_acquire);
    if (st == 3) {
        r.key("error"); r.value_string(g_cascade_query.error);
    } else {
        r.key("approved"); r.value_bool(g_cascade_query.approved);
        if (!g_cascade_query.approved) {
            r.key("rejected_at"); r.value_string(g_cascade_query.rejection_summary);
        }
        // rejection_key is always emitted (schema contract per ck-f4r2):
        //  approved → "NONE__APPROVED" sentinel
        //  rejected → specific key if bridge resolved one, else empty string
        if (g_cascade_query.approved) {
            r.key("rejection_key"); r.value_string("NONE__APPROVED");
        } else {
            r.key("rejection_key"); r.value_string(g_cascade_query.rejection_key);
        }
        r.key("gate_details"); r.begin_array();
        for (int j = 0; j < g_cascade_query.slot_count; j++) {
            const auto& s = g_cascade_query.slots[j];
            r.begin_object();
            r.key("gate"); r.value_string(s.name);
            r.key("slot"); r.value_int(s.slot_idx);
            char off[16]; snprintf(off, sizeof(off), "0x%04x", s.offset);
            r.key("offset"); r.value_string(off);
            r.key("present"); r.value_bool(s.present);
            r.key("passed"); r.value_bool(s.passed);
            r.end_object();
        }
        r.end_array();
    }
    r.end_object();
    g_cascade_query.state.store(0, std::memory_order_release);
    if (response_out) *response_out = r.take();
    return true;
}

bool do_probe_gate_4_slots(const char* params_json, std::string* response_out) {
    // Gate-4 slot evaluation is a subset of the trigger-query already implemented
    // in query_trigger_result. Re-route to that handler with trigger_slot = "all".
    //
    // json_get_string returns a pointer into a thread-local static buffer that
    // is clobbered by every subsequent call (including those inside
    // do_query_trigger_result). Copy to a std::string so the caller-provided
    // name survives downstream calls and can be echoed back correctly.
    const char* raw = json_get_string(params_json, "interaction_name");
    if (!raw || !raw[0]) raw = json_get_string(params_json, "interaction_type");
    std::string interaction_name = (raw && raw[0]) ? std::string(raw) : std::string();
    int actor_id = json_get_int(params_json, "actor_id");
    int target_id = json_get_int(params_json, "target_id");
    if (interaction_name.empty() || actor_id <= 0 || target_id <= 0) {
        if (response_out) *response_out = R"({"error":"missing_actor_target_or_interaction"})";
        return false;
    }

    // Build params for query_trigger_result. ResponseBuilder escapes the
    // client's name (rc2: it used to be spliced into the JSON with %s).
    ResponseBuilder fw;
    fw.begin_object();
    fw.key("interaction_type"); fw.value_string(interaction_name.c_str(), interaction_name.size());
    fw.key("actor_id");         fw.value_int(actor_id);
    fw.key("target_id");        fw.value_int(target_id);
    fw.key("trigger_slot");     fw.value_string("all");
    fw.end_object();
    const std::string forwarded = fw.take();

    std::string inner;
    bool ok = do_query_trigger_result(forwarded.c_str(), &inner);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("probe_gate_4_slots");
    r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id) & 0xFFFFFF);
    r.key("target_id"); r.value_uint(static_cast<uint32_t>(target_id) & 0xFFFFFF);
    r.key("interaction_name"); r.value_string(interaction_name.c_str());
    r.key("underlying"); r.value_string(inner.c_str(), inner.size());
    r.end_object();
    if (response_out) *response_out = r.take();
    return ok;
}

// ============================================================================
// process_cascade_query — game-thread processor
// ============================================================================
//
// Currently a minimal implementation: we do NOT yet build the TLS ctx here and
// call FUN_06b42310 with a param_4 collector. Instead, we evaluate the same
// gate-4 trigger slots via the existing trigger_query infrastructure and
// derive a rejection_summary from the first failing slot.
//
// Full cascade probing with a bridge-allocated error collector (per ck-t7m3 §9)
// is a follow-up — the collector's internal shape (FUN_0763ddc0 / FUN_0763dfe0
// record format) has not been fully RE'd. See re_interaction_trysend_cascade_v3.md
// §8 ("Candidate future bead"). This handler therefore currently surfaces gate-4
// slot results only; gates 1/2/3/5 are reported as "not probed".

void process_cascade_query() {
    int expected = 1;
    if (!g_cascade_query.state.compare_exchange_strong(expected, 4,
            std::memory_order_acq_rel)) {
        return;
    }

    // GATE 1 — actor/target liveness (ck-iyug).
    // The native cascade's first gate (FUN_06b3def0) rejects a dead actor or
    // target before any gate-4 trigger evaluation. The trigger-query path below
    // does not consult liveness, so without this preflight a dead-actor call
    // flows into gate-4, passes all slots, and returns approved. Check it here
    // so A.2 gate.1 scenarios surface rejected_at="GATE_1".
    {
        void* actor_ptr = lookup_character(g_cascade_query.actor_id);
        void* target_ptr = lookup_character(g_cascade_query.target_id);
        auto not_alive = [](void* c) -> bool {
            if (!c || is_sentinel(c)) return true;
            if (g_char_is_alive) return !g_char_is_alive(c);
            return safe_read<uint8_t>(
                reinterpret_cast<uintptr_t>(c) + ck3::character::DEAD_FLAG) != 0;
        };
        bool actor_dead = not_alive(actor_ptr);
        bool target_dead = not_alive(target_ptr);
        if (actor_dead || target_dead) {
            g_cascade_query.approved = false;
            snprintf(g_cascade_query.rejection_summary,
                     sizeof(g_cascade_query.rejection_summary), "GATE_1");
            const char* key = actor_dead ? "DEAD_ACTOR" : "DEAD_TARGET";
            snprintf(g_cascade_query.rejection_key,
                     sizeof(g_cascade_query.rejection_key), "%s", key);
            g_cascade_query.slot_count = 0;
            g_cascade_query.state.store(2, std::memory_order_release);
            return;
        }
    }

    // GATE 2 — initial scope filter preflight (ck-9f32).
    //
    // The native cascade's Gate 2 (FUN_06b41e70) runs seven sub-gates before
    // gate-4 trigger evaluation. Until the bridge wires the native error
    // collector (param_4 in FUN_06b42310; see re_interaction_trysend_cascade_v3.md
    // §8), we can't observe the cascade's own rejection payload. Instead we
    // reproduce two sub-gate predicates the A.2 observability scenarios need
    // to distinguish from GATE_4_SLOT_A:
    //
    //   2b → SAME_ACTOR_TARGET     (actor == target; FUN_06b41640 liveness core)
    //   2d → DEFENDER_ALREADY_AT_WAR (declare_war_interaction against a pair
    //        where the target already holds an active WAR_DB entry; approximates
    //        FUN_067f0e40's conflict family — duplicate / one-way block)
    //
    // Other sub-gates (2a/2c/2e/2f/2g) remain unprobed here and continue to
    // surface as GATE_4_* or approved. Widening gate-2 coverage is tracked in
    // the follow-up bead blocked by ck-t7m3 §9.
    {
        uint32_t actor  = g_cascade_query.actor_id  & 0xFFFFFF;
        uint32_t target = g_cascade_query.target_id & 0xFFFFFF;

        if (actor == target) {
            g_cascade_query.approved = false;
            snprintf(g_cascade_query.rejection_summary,
                     sizeof(g_cascade_query.rejection_summary), "GATE_2_SLOT_B");
            snprintf(g_cascade_query.rejection_key,
                     sizeof(g_cascade_query.rejection_key), "SAME_ACTOR_TARGET");
            g_cascade_query.slot_count = 0;
            g_cascade_query.state.store(2, std::memory_order_release);
            return;
        }

        if (strcmp(g_cascade_query.interaction_type, "declare_war_interaction") == 0) {
            for (const auto& w : enumerate_war_db()) {
                if (w.is_ended) continue;
                if (w.attacker_id == target || w.defender_id == target) {
                    g_cascade_query.approved = false;
                    snprintf(g_cascade_query.rejection_summary,
                             sizeof(g_cascade_query.rejection_summary),
                             "GATE_2_SLOT_D");
                    snprintf(g_cascade_query.rejection_key,
                             sizeof(g_cascade_query.rejection_key),
                             "DEFENDER_ALREADY_AT_WAR");
                    g_cascade_query.slot_count = 0;
                    g_cascade_query.state.store(2, std::memory_order_release);
                    return;
                }
            }
        }
    }

    // Delegate to the existing trigger_query to get per-slot pass/fail.
    // We re-enter its processor by populating its request fields and calling it
    // synchronously here (we are already on the game thread).
    int prev_trig_state = g_trigger_query.state.load(std::memory_order_acquire);
    if (prev_trig_state == 1 || prev_trig_state == 4) {
        // Trigger query in flight — don't stomp. Report busy.
        snprintf(g_cascade_query.error, sizeof(g_cascade_query.error),
                 "trigger_query_busy");
        g_cascade_query.state.store(3, std::memory_order_release);
        return;
    }

    g_trigger_query.actor_id = g_cascade_query.actor_id;
    g_trigger_query.target_id = g_cascade_query.target_id;
    strncpy(g_trigger_query.interaction_type, g_cascade_query.interaction_type,
            sizeof(g_trigger_query.interaction_type) - 1);
    g_trigger_query.interaction_type[sizeof(g_trigger_query.interaction_type) - 1] = '\0';
    g_trigger_query.trigger_slot = -1;  // all
    g_trigger_query.extract_loc_keys = g_cascade_query.extract_loc_keys;
    g_trigger_query.result_count = 0;
    g_trigger_query.error[0] = '\0';
    g_trigger_query.state.store(1, std::memory_order_release);

    process_trigger_query();

    int final_trig = g_trigger_query.state.load(std::memory_order_acquire);
    if (final_trig == 3) {
        snprintf(g_cascade_query.error, sizeof(g_cascade_query.error),
                 "%s", g_trigger_query.error);
        g_trigger_query.state.store(0, std::memory_order_release);
        g_cascade_query.state.store(3, std::memory_order_release);
        return;
    }

    // Copy results; infer "approved" as AND of non-absent slots.
    bool all_pass = true;
    const char* first_fail_name = nullptr;
    const char* first_fail_loc_key = nullptr;
    int first_fail_slot = -1;
    int copy_n = g_trigger_query.result_count;
    if (copy_n > 8) copy_n = 8;
    for (int i = 0; i < copy_n; i++) {
        const TriggerSlotResult& src = g_trigger_query.results[i];
        auto& dst = g_cascade_query.slots[i];
        dst.slot_idx = src.slot;
        strncpy(dst.name, src.name, sizeof(dst.name) - 1);
        dst.name[sizeof(dst.name) - 1] = '\0';
        dst.offset = src.offset;
        dst.present = src.present;
        dst.passed = src.result;
        if (src.present && !src.result && !first_fail_name) {
            first_fail_name = src.name;
            first_fail_loc_key = src.loc_key[0] ? src.loc_key : nullptr;
            first_fail_slot = src.slot;
            all_pass = false;
        }
    }
    g_cascade_query.slot_count = copy_n;
    g_cascade_query.approved = all_pass;
    if (!all_pass && first_fail_name) {
        // Emit canonical letter-suffix format (ck-f4r2 schema contract):
        //   slot_idx 1 (is_shown)                       → GATE_4_SLOT_A
        //   slot_idx 2 (is_valid_showing_failures_only) → GATE_4_SLOT_B
        //   slot_idx 3 (can_be_sent_despite_rejection)  → GATE_4_SLOT_C
        //   slot_idx 4 (can_send)                       → GATE_4_SLOT_D
        //   slot_idx 5 (auto_accept)                    → GATE_4_SLOT_E
        //   slot_idx 6 (on_auto_accept)                 → GATE_4_SLOT_F
        // Slot index is preserved verbatim in the per-slot gate_details array.
        if (first_fail_slot >= 1 && first_fail_slot <= 6) {
            char letter = static_cast<char>('A' + (first_fail_slot - 1));
            snprintf(g_cascade_query.rejection_summary,
                     sizeof(g_cascade_query.rejection_summary),
                     "GATE_4_SLOT_%c", letter);
        } else {
            // Out-of-range slot — preserve raw format for diagnostic fidelity.
            snprintf(g_cascade_query.rejection_summary,
                     sizeof(g_cascade_query.rejection_summary),
                     "GATE_4_SLOT_%d_%s", first_fail_slot, first_fail_name);
        }

        // Prefer the loc key from the loud-failure evaluator when the
        // caller opted in via extract_loc_keys and the populator yielded
        // a SCREAMING_SNAKE_CASE identifier (ck-wtk7 Strategy C). Fall
        // back to the slot-name derived key otherwise.
        if (first_fail_loc_key && first_fail_loc_key[0]) {
            snprintf(g_cascade_query.rejection_key,
                     sizeof(g_cascade_query.rejection_key),
                     "%s", first_fail_loc_key);
        } else {
            char upper_name[48];
            size_t i = 0;
            for (; i < sizeof(upper_name) - 1 && first_fail_name[i]; ++i) {
                char c = first_fail_name[i];
                upper_name[i] = (c >= 'a' && c <= 'z')
                    ? static_cast<char>(c - 'a' + 'A')
                    : c;
            }
            upper_name[i] = '\0';
            snprintf(g_cascade_query.rejection_key,
                     sizeof(g_cascade_query.rejection_key),
                     "TRIGGER_FAILED__%s", upper_name);
        }
    }
    g_trigger_query.state.store(0, std::memory_order_release);
    g_cascade_query.state.store(2, std::memory_order_release);
}

} // namespace bridge
