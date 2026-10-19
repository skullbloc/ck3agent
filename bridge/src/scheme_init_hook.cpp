// scheme_init_hook.cpp — Post-hook on CREATE_ACTIVE_SCHEME to log every scheme
// creation the AI (or player) makes, capturing the exact parameters used.
//
// Architecture: CREATE_ACTIVE_SCHEME is called whenever a new scheme (murder,
// fabricate_hook, seduce, etc.) is started in the game engine. We install an
// inline hook that calls the original function first (via trampoline), then
// reads the newly created CActiveScheme object's fields and stores them in a
// thread-safe ring buffer. The IPC handler reads the buffer for the agent to
// observe what real scheme initiations look like.
//
// This is a pure observer — it does not modify any game state.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <shared_mutex>
#include <deque>
#include <map>
#include <chrono>
#include <cstring>

namespace bridge {

// =============================================================================
// Scheme Initiation Record
// =============================================================================

struct SchemeInitRecord {
    uint64_t tick;                // game tick when scheme was created
    uint64_t timestamp_ms;       // wall-clock time (epoch ms)
    uint32_t scheme_id;          // CActiveScheme packed ref ID
    uint32_t owner_id;           // owner char packed ref (24-bit)
    int32_t  target_type;        // 0=char, 1=title, 2=culture, 3=faith, 4=none
    uint32_t target_ref;         // target packed ref (resolved via target_type)
    char     scheme_type_name[128]; // scheme type name (e.g. "murder", "fabricate_hook")
    int32_t  progress;           // initial progress value
    int32_t  max_progress;       // max progress needed for success
    int32_t  success_chance;     // computed success chance %
    int64_t  success_base;       // base success value (fixed-point)
    int64_t  success_growth;     // growth rate (fixed-point)
    int64_t  success_max;        // success maximum (fixed-point)
    int32_t  agent_count;        // number of agent slots
    char     owner_name[128];    // owner character name
    char     target_name[128];   // target character name (if target_type=0)
};

static std::shared_mutex g_scheme_init_mutex;
static std::deque<SchemeInitRecord> g_scheme_inits CK3_EARLY_INIT;
static constexpr size_t MAX_SCHEME_INITS = 64;

// Trampoline to original CREATE_ACTIVE_SCHEME
static void* g_orig_create_active_scheme = nullptr;

// =============================================================================
// #132 — Scheme-start argument template cache. (SUPERSEDED as an argument source by
// lane-scheme 2026-10-18 — see the block after this one; kept as a witness.)
//
// CREATE_ACTIVE_SCHEME's 5th & 6th args (forwarded to InitializeScheme) are NOT
// scalars: param_5 is a subtype-scope object (vtable-dereferenced; param_5[1] →
// scheme+0x34 subtype id, observed -1 for every real murder) and param_6 a scope
// whose +0x10 → scheme+0x38 (also -1). They cannot be passed as 0 (InitializeScheme
// dereferences param_5 with a virtual call). The ONLY safe source of valid args is
// the engine itself — so this observer hook records the (param5,param6) the engine
// passes for each scheme TYPE name, and execute_scheme_intent reuses the latest
// captured pair for the requested type. The AI starts schemes constantly, so a
// template for "murder"/"fabricate_hook" is almost always available; if not, the
// scheme-start handler refuses with a clear "no_arg_template_captured" reason.
//
// Keyed by scheme-type name. Values are the raw 64-bit args as the engine passed
// them. These point at long-lived engine objects (the script-system's shared
// "no subtype / no special scope" sentinels — param5[1]==-1, *(param6+0x10)==-1),
// not per-scheme allocations, so they remain valid for reuse.
// =============================================================================
// lane-scheme 2026-10-18 (re/findings/re_scheme_start_args.md): RE has since
// named the two args. The single engine caller (the start_scheme effect, call @
// image 0x052baa73) passes p5 = the scheme's CTaskContract* and p6 = its
// CArtifact*, each a RefDB lookup of an OPTIONAL script scope that falls back to
// the DB's TPdxNullObject when the scope is absent. A murder carries neither, so
// the engine passes the two null objects -- process-lifetime singletons. A start
// that DID carry a contract/artifact would leave a pointer to a destroyable
// object here, so the cache now records which case it saw (`null_args`) and
// execute_scheme_intent no longer replays a captured pointer at all: it passes
// the live null objects itself and uses this cache only as a witness of how the
// engine starts each type.
struct SchemeArgTemplate {
    uint64_t param5;
    uint64_t param6;
    bool     valid;
    bool     null_args;   // the LAST start passed exactly the null contract/artifact
    uint32_t null_seen;   // engine starts of this type with the null objects
    uint32_t real_seen;   // engine starts of this type carrying a real contract/artifact
};
// Set by execute_scheme_intent around its own CreateActiveScheme call, so the
// witness below counts only the ENGINE's starts, never the bridge's own.
thread_local bool g_scheme_start_from_bridge = false;

static std::shared_mutex g_scheme_tpl_mutex;
static std::map<std::string, SchemeArgTemplate> g_scheme_arg_templates CK3_EARLY_INIT;

// Accessor for execute_scheme_intent (intent_dispatch.cpp). Returns true and
// fills out_p5/out_p6 if a template for `type_name` has been observed.
bool scheme_get_arg_template(const std::string& type_name,
                             uint64_t* out_p5, uint64_t* out_p6) {
    std::shared_lock<std::shared_mutex> lock(g_scheme_tpl_mutex);
    auto it = g_scheme_arg_templates.find(type_name);
    if (it == g_scheme_arg_templates.end() || !it->second.valid) return false;
    if (out_p5) *out_p5 = it->second.param5;
    if (out_p6) *out_p6 = it->second.param6;
    return true;
}

// How the engine itself has started `type_name` this process: 0 = never seen,
// 1 = at least one start passed the null contract/artifact objects (the type is
// startable without them), 2 = seen, and EVERY start carried a real contract or
// artifact (the bridge cannot start it faithfully).
int scheme_arg_template_status(const std::string& type_name) {
    std::shared_lock<std::shared_mutex> lock(g_scheme_tpl_mutex);
    auto it = g_scheme_arg_templates.find(type_name);
    if (it == g_scheme_arg_templates.end() || !it->second.valid) return 0;
    if (it->second.null_seen > 0) return 1;
    return it->second.real_seen > 0 ? 2 : 0;
}

// The two null objects CreateActiveScheme expects when a scheme has no task
// contract / artifact (what the start_scheme effect passes when its optional
// scopes are absent). 0 when the DB is not up yet. Fault-tolerant reads.
bool scheme_null_arg_objects(uint64_t* out_contract, uint64_t* out_artifact) {
    uintptr_t c = safe_read<uintptr_t>(g_base_addr + ck3::globals::TASK_CONTRACT_NULL);
    uintptr_t a = safe_read<uintptr_t>(g_base_addr + ck3::globals::ARTIFACT_NULL);
    if (!c || !a || !is_mapped(c) || !is_mapped(a)) return false;
    if (out_contract) *out_contract = c;
    if (out_artifact) *out_artifact = a;
    return true;
}

// =============================================================================
// CREATE_ACTIVE_SCHEME signature (from Ghidra decompilation):
//   CActiveScheme* CreateActiveScheme(long scheme_mgr, long* scheme_type,
//                                      long owner_char, uint64 target_scope,
//                                      uint64 param5, uint64 param6)
// Returns: pointer to newly created CActiveScheme (or SCHEME_NULL sentinel)
// =============================================================================

using fn_create_active_scheme = void*(*)(void* scheme_mgr, void* scheme_type,
                                          void* owner_char, uint64_t target_scope,
                                          uint64_t param5, uint64_t param6);

// =============================================================================
// Post-Hook: called instead of CREATE_ACTIVE_SCHEME
// =============================================================================

static void* on_create_active_scheme(void* scheme_mgr, void* scheme_type,
                                      void* owner_char, uint64_t target_scope,
                                      uint64_t param5, uint64_t param6) {
    // Call original first — the scheme is created
    auto orig = reinterpret_cast<fn_create_active_scheme>(g_orig_create_active_scheme);
    void* result = orig(scheme_mgr, scheme_type, owner_char, target_scope,
                        param5, param6);

    // Validate result pointer (SCHEME_NULL sentinel is at a known address)
    uintptr_t scheme_ptr = reinterpret_cast<uintptr_t>(result);
    if (!scheme_ptr || scheme_ptr < 0x10000) {
        return result;
    }

    // Engine refusal: CreateActiveScheme returns the null-scheme OBJECT whose
    // pointer lives in the SCHEME_NULL slot, not the slot's address
    // (lane-scheme 2026-10-18; see execute_scheme_intent). The old address
    // compare never matched, so every refused AI start was recorded here as an
    // initiation of a scheme with id 0xFFFFFF.
    uintptr_t null_slot = g_base_addr + ck3::globals::SCHEME_NULL;
    uintptr_t null_obj = safe_read<uintptr_t>(null_slot);
    if (scheme_ptr == null_slot || (null_obj && scheme_ptr == null_obj)) {
        return result;
    }

    // Build the record from the newly created CActiveScheme object
    SchemeInitRecord rec{};
    rec.tick = g_tick_id.load(std::memory_order_relaxed);
    rec.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // SIGSEGV protection around all memory reads.
    //
    // lane-scheme 2026-10-18: SAVE AND RESTORE the caller's recovery state. This
    // hook runs INSIDE other protected regions: execute_scheme_intent calls the
    // hooked entry under its own sigsetjmp, and an interaction the bridge sends
    // (start_murder_interaction) reaches it through the engine's effect walk under
    // that handler's net. Before this, the hook overwrote the thread's
    // g_recovery_point with a frame that is dead once it returns and then cleared
    // g_in_protected_region, so a later fault or watchdog SIGALRM in the OUTER
    // region either crashed the game unprotected or siglongjmp'd into a dead frame.
    sigjmp_buf saved_recovery;
    memcpy(&saved_recovery, &g_recovery_point, sizeof(sigjmp_buf));
    const bool saved_protected = g_in_protected_region;
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        // Scheme ID (packed ref at +0x10)
        rec.scheme_id = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::SCHEME_ID);

        // Owner ID (at +0x24, masked to 24-bit)
        rec.owner_id = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::OWNER_ID) & 0xFFFFFF;

        // Target type and ref
        rec.target_type = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::TARGET_TYPE);
        rec.target_ref = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::TARGET_REF);

        // Scheme type name (read from CSchemeType* at +0x18)
        uintptr_t type_ptr = safe_read<uintptr_t>(scheme_ptr + ck3::active_scheme::SCHEME_TYPE_PTR);
        if (type_ptr > 0x10000 && is_mapped(type_ptr)) {
            // CSchemeType has name at db_object::NAME_KEY (+0x10)
            std::string type_name = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
            if (!type_name.empty()) {
                strncpy(rec.scheme_type_name, type_name.c_str(),
                        sizeof(rec.scheme_type_name) - 1);
                // #132: cache the (param5,param6) the engine used for this scheme
                // type so execute_scheme_intent can reuse them when starting a
                // scheme forward. Only record successful, non-sentinel creations
                // (we're already past the null-sentinel guard above) so the args
                // are known-good for THIS scheme type. param5/param6 reference
                // long-lived shared script-scope sentinels, safe to reuse.
                if (!g_scheme_start_from_bridge) {
                    uint64_t null_c = 0, null_a = 0;
                    const bool have_nulls = scheme_null_arg_objects(&null_c, &null_a);
                    const bool null_args = have_nulls && param5 == null_c && param6 == null_a;
                    bool changed = false;
                    uint32_t null_seen = 0, real_seen = 0;
                    {
                        std::unique_lock<std::shared_mutex> tl(g_scheme_tpl_mutex);
                        auto it = g_scheme_arg_templates.find(type_name);
                        if (it != g_scheme_arg_templates.end()) {
                            null_seen = it->second.null_seen;
                            real_seen = it->second.real_seen;
                            changed = (it->second.null_args != null_args);
                        } else {
                            changed = true;  // first sighting of this type
                        }
                        (null_args ? null_seen : real_seen) += 1;
                        g_scheme_arg_templates[type_name] = SchemeArgTemplate{
                            param5, param6, true, null_args, null_seen, real_seen };
                    }
                    // Logged on the first sighting of a type and whenever the
                    // contract/artifact case flips: the live witness for the
                    // finding's claim that engine murders pass the null objects.
                    if (changed) ADLOG("scheme_init_hook: arg template type=%s p5=0x%lx "
                          "p6=0x%lx null_contract=0x%lx null_artifact=0x%lx null_args=%d "
                          "null_seen=%u real_seen=%u\n", type_name.c_str(),
                          (unsigned long)param5, (unsigned long)param6,
                          (unsigned long)null_c, (unsigned long)null_a,
                          null_args ? 1 : 0, null_seen, real_seen);
                }
            }
        }

        // Progress values
        rec.progress = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::PROGRESS);
        rec.max_progress = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::MAX_PROGRESS);
        rec.success_chance = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::SUCCESS_CHANCE);

        // Success modifiers (fixed-point)
        rec.success_base = safe_read<int64_t>(scheme_ptr + ck3::active_scheme::SUCCESS_BASE);
        rec.success_growth = safe_read<int64_t>(scheme_ptr + ck3::active_scheme::SUCCESS_GROWTH);
        rec.success_max = safe_read<int64_t>(scheme_ptr + ck3::active_scheme::SUCCESS_MAX);

        // Agent count
        rec.agent_count = safe_read<int32_t>(scheme_ptr + ck3::active_scheme::AGENT_SLOTS_COUNT);

        // Owner character name
        uintptr_t owner_char_ptr = ad_db_lookup(ck3::globals::CHARACTER_DB, rec.owner_id);
        if (owner_char_ptr) {
            std::string name = ad_read_char_name(owner_char_ptr);
            strncpy(rec.owner_name, name.c_str(), sizeof(rec.owner_name) - 1);
        }

        // Target character name (only if target is a character)
        if (rec.target_type == 0 && rec.target_ref != 0xFFFFFFFF) {
            uint32_t target_id = rec.target_ref & 0xFFFFFF;
            uintptr_t target_char_ptr = ad_db_lookup(ck3::globals::CHARACTER_DB, target_id);
            if (target_char_ptr) {
                std::string name = ad_read_char_name(target_char_ptr);
                strncpy(rec.target_name, name.c_str(), sizeof(rec.target_name) - 1);
            }
        }

        // Store in ring buffer
        {
            std::unique_lock<std::shared_mutex> lock(g_scheme_init_mutex);
            g_scheme_inits.push_back(rec);
            while (g_scheme_inits.size() > MAX_SCHEME_INITS) {
                g_scheme_inits.pop_front();
            }
        }

        // Log to bridge log
        if (g_trace) ADLOG("SCHEME INITIATED: id=%u type=%s owner=%u(%s) target_type=%d "
              "target=%u(%s) progress=%d/%d chance=%d agents=%d tick=%lu\n",
              rec.scheme_id, rec.scheme_type_name,
              rec.owner_id, rec.owner_name,
              rec.target_type, rec.target_ref, rec.target_name,
              rec.progress, rec.max_progress, rec.success_chance,
              rec.agent_count, rec.tick);
    } else {
        // SIGSEGV recovery
        ADLOG("scheme_init_hook: SIGSEGV reading new scheme at %p\n", result);
    }
    memcpy(&g_recovery_point, &saved_recovery, sizeof(sigjmp_buf));
    g_in_protected_region = saved_protected;

    return result;
}

// =============================================================================
// Initialization
// =============================================================================

void init_scheme_init_hook() {
    uintptr_t target = g_base_addr + ck3::funcs::CREATE_ACTIVE_SCHEME;
    g_orig_create_active_scheme = install_hook(target,
        reinterpret_cast<void*>(on_create_active_scheme));

    if (g_orig_create_active_scheme) {
        ADLOG("scheme_init_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_create_active_scheme);
    } else {
        ADLOG("scheme_init_hook: FAILED to install at 0x%lx\n", target);
    }
}

// =============================================================================
// Query API (called from IPC thread)
// =============================================================================

bool do_query_scheme_inits(const char* params_json, std::string* out) {
    if (!out) return false;

    int limit = json_get_int(params_json, "limit");
    if (limit <= 0 || limit > 64) limit = 64;

    uint64_t since_tick = 0;
    if (json_has_key(params_json, "since_tick")) {
        since_tick = static_cast<uint64_t>(json_get_int(params_json, "since_tick"));
    }

    // Optional filter by scheme type name (e.g. "murder", "fabricate_hook")
    const char* type_filter = nullptr;
    if (json_has_key(params_json, "scheme_type")) {
        type_filter = json_get_string(params_json, "scheme_type");
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_scheme_inits");
    r.key("success"); r.value_bool(true);
    r.key("current_tick"); r.value_uint(g_tick_id.load(std::memory_order_relaxed));
    r.key("hook_installed"); r.value_bool(g_orig_create_active_scheme != nullptr);

    r.key("scheme_inits"); r.begin_array();
    {
        std::shared_lock<std::shared_mutex> lock(g_scheme_init_mutex);
        int emitted = 0;
        // Iterate in reverse (newest first)
        for (auto it = g_scheme_inits.rbegin();
             it != g_scheme_inits.rend() && emitted < limit; ++it) {
            if (it->tick < since_tick) continue;
            if (type_filter && type_filter[0] != '\0' &&
                strncmp(it->scheme_type_name, type_filter, 128) != 0) {
                continue;
            }

            r.begin_object();
            r.key("tick"); r.value_uint(it->tick);
            r.key("timestamp_ms"); r.value_uint(it->timestamp_ms);
            r.key("scheme_id"); r.value_uint(it->scheme_id);
            r.key("owner_id"); r.value_uint(it->owner_id);
            r.key("owner_name"); r.value_string(it->owner_name);
            r.key("scheme_type"); r.value_string(it->scheme_type_name);
            r.key("target_type"); r.value_int(it->target_type);
            r.key("target_ref"); r.value_uint(it->target_ref);
            r.key("target_name"); r.value_string(it->target_name);
            r.key("progress"); r.value_int(it->progress);
            r.key("max_progress"); r.value_int(it->max_progress);
            r.key("success_chance"); r.value_int(it->success_chance);
            r.key("success_base"); r.value_int(it->success_base);
            r.key("success_growth"); r.value_int(it->success_growth);
            r.key("success_max"); r.value_int(it->success_max);
            r.key("agent_count"); r.value_int(it->agent_count);
            r.end_object();
            emitted++;
        }
    }
    r.end_array();

    r.key("total_recorded"); r.value_uint(
        static_cast<uint64_t>(g_scheme_inits.size()));
    r.end_object();

    *out = r.take();
    return true;
}

} // namespace bridge
