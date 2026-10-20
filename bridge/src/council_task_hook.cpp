// council_task_hook.cpp — Post-hook on SET_COUNCIL_TASK to log every council
// task assignment the AI (or player) makes, capturing the exact parameters.
//
// Architecture: SET_COUNCIL_TASK is called whenever a council task is assigned
// to a councillor (e.g. chancellor gets "fabricate_claim", steward gets
// "collect_taxes"). We install an inline hook that calls the original function
// first (via trampoline), then reads the effect object's fields and stores
// them in a thread-safe ring buffer. The IPC handler reads the buffer for the
// agent to observe what real council task assignments look like.
//
// This is a pure observer — it does not modify any game state.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <shared_mutex>
#include <deque>
#include <chrono>

namespace bridge {

// =============================================================================
// Council Task Assignment Record
// =============================================================================

struct CouncilTaskRecord {
    uint64_t tick;                 // game tick when task was assigned
    uint64_t timestamp_ms;        // wall-clock time (epoch ms)
    uint32_t councillor_id;       // character ref of the councillor
    uint32_t ruler_id;            // character ref of the ruler who owns the council
    uint32_t position_type_ref;   // council position type ref (from CouncilPositionDB)
    uint32_t task_type_ref;       // task type reference
    char     councillor_name[128];// councillor character name
    char     ruler_name[128];     // ruler character name
    char     position_name[128];  // position type name (e.g. "councillor_chancellor")
    char     task_name[128];      // task type name if resolvable
    uint8_t  flag;                // param_2 flag value
};

static std::shared_mutex g_council_task_mutex;
static std::deque<CouncilTaskRecord> g_council_tasks CK3_EARLY_INIT;
static constexpr size_t MAX_COUNCIL_TASKS = 64;

// Trampoline to original SET_COUNCIL_TASK
static void* g_orig_set_council_task = nullptr;

// =============================================================================
// SET_COUNCIL_TASK signature (from Ghidra decompilation of SetCouncilTask):
//   void SetCouncilTask(long effect_obj, char flag)
//
// Key fields in effect_obj:
//   +0x1b0: ptr to intermediate council data
//     intermediate+0xC8: councillor character ref (uint32)
//     intermediate+0xF0: council position type ref (uint32)
//   +0x18: task type reference/ID
// =============================================================================

using fn_set_council_task = void(*)(void* effect_obj, char flag);

// =============================================================================
// Post-Hook: called instead of SET_COUNCIL_TASK
// =============================================================================

static void on_set_council_task(void* effect_obj, char flag) {
    // Call original first — the task assignment happens
    auto orig = reinterpret_cast<fn_set_council_task>(g_orig_set_council_task);
    orig(effect_obj, flag);

    // Validate effect_obj pointer
    uintptr_t eff = reinterpret_cast<uintptr_t>(effect_obj);
    if (!eff || eff < 0x10000) {
        ADLOG("council_task_hook: SET_COUNCIL_TASK called with null effect\n");
        return;
    }

    // Build the record from the effect object fields
    CouncilTaskRecord rec{};
    rec.tick = g_tick_id.load(std::memory_order_relaxed);
    rec.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    rec.flag = static_cast<uint8_t>(flag);

    // SIGSEGV protection around all memory reads
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        // Read task type reference from effect_obj+0x18
        rec.task_type_ref = safe_read<uint32_t>(eff + 0x18);

        // Read intermediate pointer from effect_obj+0x1b0
        uintptr_t intermediate = safe_read<uintptr_t>(eff + 0x1b0);
        if (intermediate && intermediate > 0x10000 && is_mapped(intermediate)) {
            // Councillor character ref from intermediate+0xC8 (200).
            // Guard against packed-id shapes: a genuine char ref has its high
            // byte as a small generation counter, so (raw>>24) is usually 0 and
            // the masked id resolves to a char whose +0x18 id-field round-trips.
            // We accept the masked id provisionally and validate at lookup time
            // below (resolved char's id must match) so packed garbage does not
            // surface as a bogus councillor_name.
            uint32_t raw_char = safe_read<uint32_t>(intermediate + 0xC8);
            rec.councillor_id = raw_char & 0xFFFFFF;

            // Council position type ref from intermediate+0xF0
            rec.position_type_ref = safe_read<uint32_t>(intermediate + 0xF0);

            // Try to look up the position type name via CouncilPositionDB
            uintptr_t pos_db = g_base_addr + ck3::globals::COUNCIL_POSITION_DB;
            uintptr_t pos_db_ptr = safe_read<uintptr_t>(pos_db);
            if (pos_db_ptr && pos_db_ptr > 0x10000 && is_mapped(pos_db_ptr)) {
                uint32_t pos_max = safe_read<uint32_t>(pos_db_ptr + ck3::hash_map::CAPACITY);
                uint32_t pos_idx = rec.position_type_ref & 0xFFFFFF;
                if (pos_idx < pos_max) {
                    uintptr_t array_base = safe_read<uintptr_t>(pos_db_ptr + ck3::hash_map::TABLE_PTR);
                    if (array_base && is_mapped(array_base)) {
                        uintptr_t entry = safe_read<uintptr_t>(array_base + ck3::hash_map::ENTRY_HEADER_SIZE
                                                               + pos_idx * ck3::hash_map::ENTRY_STRIDE);
                        if (entry && entry > 0x10000 && is_mapped(entry)) {
                            // Validate ID match
                            uint32_t entry_id = safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF);
                            if (entry_id == rec.position_type_ref) {
                                // Position entry has an intermediate ptr at +0x18
                                uintptr_t pos_inter = safe_read<uintptr_t>(entry + ck3::council_assign::INTERMEDIATE_PTR);
                                if (pos_inter && pos_inter > 0x10000 && is_mapped(pos_inter)) {
                                    // Position type object at *(pos_inter+0x30)
                                    uintptr_t pos_type_obj = safe_read<uintptr_t>(pos_inter + ck3::council_task_type::POSITION_TYPE_PTR);
                                    if (pos_type_obj && pos_type_obj > 0x10000 && is_mapped(pos_type_obj)) {
                                        std::string pname = ad_read_std_string(
                                            pos_type_obj + ck3::council_pos_type::NAME_KEY);
                                        if (!pname.empty()) {
                                            strncpy(rec.position_name, pname.c_str(),
                                                    sizeof(rec.position_name) - 1);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Look up councillor character name. Reject packed-id shapes: the
        // resolved char's authoritative id-field (+0x18, masked) must match the
        // id we looked up with, else this is not a clean character ref.
        if (rec.councillor_id && rec.councillor_id != 0xFFFFFF) {
            uintptr_t coun_char = ad_db_lookup(ck3::globals::CHARACTER_DB,
                                                rec.councillor_id);
            if (coun_char) {
                uint32_t rt_id = char_get_id(reinterpret_cast<void*>(coun_char)) & 0xFFFFFF;
                if (rt_id != rec.councillor_id) {
                    coun_char = 0;
                    rec.councillor_id = 0;
                }
            }
            if (coun_char) {
                std::string name = ad_read_char_name(coun_char);
                strncpy(rec.councillor_name, name.c_str(),
                        sizeof(rec.councillor_name) - 1);

                // Try to find the ruler from the councillor's council instance
                // core_data = *(char+0x1A8), council_instance = *(core_data+0x4E8)
                uintptr_t core_data = safe_read<uintptr_t>(coun_char + ck3::character::REALM_DATA);
                if (core_data && core_data > 0x10000 && is_mapped(core_data)) {
                    uintptr_t council_inst = safe_read<uintptr_t>(
                        core_data + ck3::core_data::COUNCIL_INSTANCE_PTR);
                    if (council_inst && council_inst > 0x10000 && is_mapped(council_inst)) {
                        // Council type obj has ruler_char_id at +0x08
                        uint32_t ruler_ref = safe_read<uint32_t>(
                            council_inst + ck3::council_type_obj::RULER_CHAR_ID);
                        if (ruler_ref && ruler_ref != 0xFFFFFFFF) {
                            rec.ruler_id = ruler_ref & 0xFFFFFF;
                        }
                    }
                }

                // Fallback: try reading ruler from the effect object's structure
                // The SetCouncilTask decompilation shows effect_obj+0x1b8 has
                // a council position tracker with character data
                if (!rec.ruler_id || rec.ruler_id == 0xFFFFFF) {
                    uintptr_t tracker = safe_read<uintptr_t>(eff + 0x1b8);
                    if (tracker && tracker > 0x10000 && is_mapped(tracker)) {
                        uintptr_t ruler_data = safe_read<uintptr_t>(tracker + 0x1c0);
                        if (ruler_data && ruler_data > 0x10000 && is_mapped(ruler_data)) {
                            uint32_t ruler_cid = safe_read<uint32_t>(ruler_data + 0x18);
                            if (ruler_cid && ruler_cid != 0xFFFFFFFF) {
                                rec.ruler_id = ruler_cid & 0xFFFFFF;
                            }
                        }
                    }
                }
            }
        }

        // Look up ruler name
        if (rec.ruler_id && rec.ruler_id != 0xFFFFFF) {
            uintptr_t ruler_char = ad_db_lookup(ck3::globals::CHARACTER_DB,
                                                 rec.ruler_id);
            if (ruler_char) {
                std::string name = ad_read_char_name(ruler_char);
                strncpy(rec.ruler_name, name.c_str(),
                        sizeof(rec.ruler_name) - 1);
            }
        }

        // Try to resolve task type name — task_type_ref might be a DB index
        // into a council task type database
        if (rec.task_type_ref && rec.task_type_ref != 0xFFFFFFFF) {
            // Council task types are game database objects with NAME_KEY at +0x10
            // Try looking up in the generic way via the position data
            // The task type is stored at pos_data+0x08 in position entries
            // For now, store the raw ref — we can resolve the name later
            // if we find the task type database
        }

        // Store in ring buffer
        {
            std::unique_lock<std::shared_mutex> lock(g_council_task_mutex);
            g_council_tasks.push_back(rec);
            while (g_council_tasks.size() > MAX_COUNCIL_TASKS) {
                g_council_tasks.pop_front();
            }
        }

        // Log to bridge log
        if (g_trace) ADLOG("COUNCIL TASK: councillor=%u(%s) ruler=%u(%s) position=%s "
              "task_ref=%u flag=%u tick=%lu\n",
              rec.councillor_id, rec.councillor_name,
              rec.ruler_id, rec.ruler_name,
              rec.position_name[0] ? rec.position_name : "unknown",
              rec.task_type_ref, rec.flag, rec.tick);
    } else {
        // SIGSEGV recovery
        ADLOG("council_task_hook: SIGSEGV reading effect at %p\n", effect_obj);
    }
    g_in_protected_region = false;
}

// =============================================================================
// Initialization
// =============================================================================

void init_council_task_hook() {
    // NOT INSTALLED (ADR 0001 P6, found reviewing the hook for MSVC). The target is
    // not the setter this hook was written for: ck3_offsets.h has SET_COUNCIL_TASK
    // as the task-AVAILABILITY check (corrected 20260612), and its decompile is
    // `bool SetCouncilTask(long obj, uint32 task, uint64 x)`: it uses all three
    // arguments and returns the answer. The hook's prototype is
    // `void(void* effect_obj, char flag)`: it passed the original two of them and
    // returned nothing, so every caller of the check read whatever the hook left in
    // rax, and the records it kept read effect-object fields off a different
    // object. The live per-seat read (emit_live_council_seats) does not use them.
    // `query_council_tasks` without a character now reports no hook assignments.
    if ((true)) {
        ADLOG("council_task_hook: not installed (SET_COUNCIL_TASK is an availability "
              "check, bool(obj, u32, u64), not the setter the hook expects)\n");
        return;
    }
    uintptr_t target = bridge::game_fn(ck3::funcs::SET_COUNCIL_TASK);
    g_orig_set_council_task = install_hook(target, reinterpret_cast<void*>(on_set_council_task));

    if (g_orig_set_council_task) {
        ADLOG("council_task_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_set_council_task);
    } else {
        ADLOG("council_task_hook: FAILED to install at 0x%lx\n", target);
    }
}

// =============================================================================
// Query API (called from IPC thread)
// =============================================================================

// -----------------------------------------------------------------------------
// B4 (20260621): LIVE per-seat council task read.
//
// The historical do_query_council_tasks dumped the g_council_tasks ring buffer
// (intercepted SetCouncilTask calls), which is empty/stale outside an active
// hook firing and frequently yielded blank task_name/councillor_name. The
// CORRECT source is the council INSTANCE walk already shipping in
// do_query_council (query_handlers.cpp): per seat the position object exposes
// the current task, the seated councillor, and (new here) the task target
// scope. This emits task_name, councillor_name and target_title per seat from
// authoritative live memory. Pure reads, fail-open via is_mapped/safe_read.
// -----------------------------------------------------------------------------
static void emit_live_council_seats(ResponseBuilder& r, void* character) {
    r.key("seats"); r.begin_array();
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

                // Current task name
                std::string task_name;
                uintptr_t cur_task = safe_read<uintptr_t>(pos_obj + ck3::globals::COUNCIL_POS_CURRENT_TASK);
                if (cur_task && is_mapped(cur_task))
                    task_name = ad_read_std_string(cur_task + ck3::db_object::NAME_KEY);

                // Position key/name
                std::string pos_key;
                {
                    const char* pkey_ptr = nullptr; size_t pkey_len = 0;
                    if (read_db_obj_name(pos_obj, pkey_ptr, pkey_len) && pkey_len > 0)
                        pos_key.assign(pkey_ptr, std::min(pkey_len, (size_t)127));
                }

                // Seated councillor (authoritative +0x38, display fallback +0x30)
                uint32_t holder_display = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_HOLDER_ID) & 0xFFFFFF;
                uint32_t holder_seated  = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_COUNCILLOR_ID) & 0xFFFFFF;
                uint32_t holder = (holder_display && holder_display != 0xFFFFFF) ? holder_display : holder_seated;
                uint32_t ruler_id = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_CTX_RULER) & 0xFFFFFF;

                void* hc = (holder && holder != 0xFFFFFF) ? lookup_character(holder) : nullptr;
                bool filled = hc && !is_sentinel(hc);

                // Task target scope (tag==8 => title target at +0x48)
                std::string target_title, target_title_key;
                uint32_t target_title_id = 0;
                uint32_t target_tag = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_TASK_TARGET_TAG);
                if (target_tag == 8) {
                    uint32_t tid = safe_read<uint32_t>(pos_obj + ck3::globals::COUNCIL_POS_TASK_TARGET_ID) & 0xFFFFFF;
                    if (tid && tid != 0xFFFFFF) {
                        uintptr_t tobj = lookup_title_obj(tid);
                        if (tobj && is_mapped(tobj)) {
                            target_title_id = tid;
                            target_title = ad_read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
                            uintptr_t tp = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
                            if (tp && is_mapped(tp))
                                target_title_key = ad_read_std_string(tp + ck3::db_object::NAME_KEY);
                        }
                    }
                }

                r.begin_object();
                r.key("position_id"); r.value_uint(idx);
                if (!pos_key.empty()) { r.key("position_key"); r.value_string(pos_key.c_str(), pos_key.size()); }
                r.key("task_name"); r.value_string(task_name.c_str(), task_name.size());
                r.key("ruler_id"); r.value_uint(ruler_id);
                if (filled) {
                    r.key("councillor_id"); r.value_uint(holder);
                    r.key("councillor_name"); r.value_string(ad_read_char_name(reinterpret_cast<uintptr_t>(hc)).c_str());
                } else {
                    r.key("vacant"); r.value_bool(true);
                }
                if (target_title_id) {
                    r.key("target_title_id"); r.value_uint(target_title_id);
                    r.key("target_title"); r.value_string(target_title.c_str(), target_title.size());
                    if (!target_title_key.empty()) {
                        r.key("target_title_key"); r.value_string(target_title_key.c_str(), target_title_key.size());
                    }
                }
                r.end_object();
            }
        }
    }
    r.end_array();
}

bool do_query_council_tasks(const char* params_json, std::string* out) {
    if (!out) return false;

    int limit = json_get_int(params_json, "limit");
    if (limit <= 0 || limit > 64) limit = 64;

    uint64_t since_tick = 0;
    if (json_has_key(params_json, "since_tick")) {
        since_tick = static_cast<uint64_t>(json_get_int(params_json, "since_tick"));
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_council_tasks");
    r.key("success"); r.value_bool(true);
    r.key("current_tick"); r.value_uint(g_tick_id.load(std::memory_order_relaxed));
    r.key("hook_installed"); r.value_bool(g_orig_set_council_task != nullptr);

    // PRIMARY (B4): live per-seat read when a character is supplied. This is the
    // authoritative source — the deque below is kept only as legacy telemetry.
    void* character = resolve_character(params_json);
    bool have_char = character && !is_sentinel(character);
    r.key("live_read"); r.value_bool(have_char);
    if (have_char) {
        r.key("character_id"); r.value_uint(char_get_id(character) & 0xFFFFFF);
        emit_live_council_seats(r, character);
    }

    // Backward-compat hook telemetry: only surface the intercepted-call ring
    // buffer when NO character was supplied (otherwise the live seats are
    // authoritative and the deque is just noise).
    r.key(have_char ? "hook_assignments" : "assignments"); r.begin_array();
    if (!have_char) {
        std::shared_lock<std::shared_mutex> lock(g_council_task_mutex);
        int emitted = 0;
        // Iterate in reverse (newest first)
        for (auto it = g_council_tasks.rbegin();
             it != g_council_tasks.rend() && emitted < limit; ++it) {
            if (it->tick < since_tick) continue;

            r.begin_object();
            r.key("tick"); r.value_uint(it->tick);
            r.key("timestamp_ms"); r.value_uint(it->timestamp_ms);
            r.key("councillor_id"); r.value_uint(it->councillor_id);
            r.key("ruler_id"); r.value_uint(it->ruler_id);
            r.key("position_type_ref"); r.value_uint(it->position_type_ref);
            r.key("task_type_ref"); r.value_uint(it->task_type_ref);
            r.key("councillor_name"); r.value_string(it->councillor_name);
            r.key("ruler_name"); r.value_string(it->ruler_name);
            r.key("position_name"); r.value_string(it->position_name);
            r.key("task_name"); r.value_string(it->task_name);
            r.key("flag"); r.value_uint(it->flag);
            r.end_object();
            emitted++;
        }
    }
    r.end_array();

    r.key("total_recorded"); r.value_uint(
        static_cast<uint64_t>(g_council_tasks.size()));
    r.end_object();

    *out = r.take();
    return true;
}

} // namespace bridge
