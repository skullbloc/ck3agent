// Internal header for action_dispatch split files.
// NOT part of the public bridge.h API. Only included by:
//   dispatch_router.cpp, query_handlers.cpp, action_handlers.cpp,
//   personality.cpp, ai_eval_hook.cpp
#pragma once

#include "bridge.h"
#include "ck3_offsets.h"
#include "name_tables.h"
#include "personality_locks.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <map>
#include <mutex>
#include <setjmp.h>
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// Local logging macro -- writes to g_logfile only (stderr is unsafe in pressure-vessel)
#define ADLOG(fmt, ...) do { \
    if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] " fmt, ##__VA_ARGS__); fflush(g_logfile); } \
} while(0)

namespace bridge {

// SIGSEGV recovery (defined in bridge_init.cpp)
extern thread_local sigjmp_buf g_recovery_point;
extern thread_local bool g_in_protected_region;

// =============================================================================
// Function Pointers (initialized in dispatch_router.cpp)
// =============================================================================

extern fn_ai_char_toggle     g_ai_char_toggle;
extern fn_char_is_alive      g_char_is_alive;
extern fn_get_ai_controller  g_get_ai_ctrl;
extern fn_get_council_config g_get_council_config;
extern fn_get_council_instance g_get_council_instance;

// =============================================================================
// Personality Lock System (personality.cpp)
// =============================================================================

// PersonalityLock, the lock tables and the world-reset (quarantine) half now
// live in personality_locks.h — an engine-free header, so the reset path can be
// compiled and asserted on a host with no CK3 (ck-hxh7 / ck-9fc3).

// save_personality_locks_locked() and the lock-file reader moved to
// personality_persist.cpp and are declared in personality_locks.h (ck-9fc3).
// load_personality_locks() is GONE: it had no reachable caller, took no save
// identity, and restoring a previous world's steering is exactly the P1.

// =============================================================================
// Simple JSON Response Builder
// =============================================================================

class ResponseBuilder {
public:
    void begin_object() { buf_ += '{'; }
    void end_object() { trim_comma(); buf_ += "},"; }
    void begin_array() { buf_ += '['; }
    void end_array() { trim_comma(); buf_ += "],"; }

    void key(const char* k) { buf_ += '"'; buf_ += k; buf_ += "\":"; }

    void escape_char(char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", uc);
            buf_ += esc;
        } else {
            switch (c) {
                case '"':  buf_ += "\\\""; break;
                case '\\': buf_ += "\\\\"; break;
                default:   buf_ += c; break;
            }
        }
    }

    void value_string(const char* v) {
        buf_ += '"';
        for (const char* p = v; *p; ++p) escape_char(*p);
        buf_ += "\",";
    }

    void value_string(const char* v, size_t len) {
        buf_ += '"';
        for (size_t i = 0; i < len; ++i) escape_char(v[i]);
        buf_ += "\",";
    }

    void value_int(int64_t v) { buf_ += std::to_string(v); buf_ += ','; }
    void value_uint(uint64_t v) { buf_ += std::to_string(v); buf_ += ','; }
    void value_float(float v) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "%.2f", v);
        buf_ += tmp; buf_ += ',';
    }
    void value_double(double v) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "%.2f", v);
        buf_ += tmp; buf_ += ',';
    }
    // ck-ijfa: a RATE needs more than two decimals. 0.076 game-days per
    // wall-second rounds to 0.08 at %.2f — a 5% error in the very conversion
    // factor the pre-registered thresholds are restated against, introduced by
    // the printer.
    void value_double_prec(double v, int prec) {
        char tmp[48]; snprintf(tmp, sizeof(tmp), "%.*g", prec, v);
        buf_ += tmp; buf_ += ',';
    }
    void value_bool(bool v) { buf_ += v ? "true," : "false,"; }
    void value_null() { buf_ += "null,"; }

    // FIX I11: value_raw splices PRE-FORMED, KNOWN-VALID JSON verbatim. No escaping/validation —
    // caller guarantees well-formedness. Do NOT use for untrusted/possibly-empty payloads.
    void value_raw(const char* v) { buf_ += v; buf_ += ','; }
    void value_raw(const std::string& v) { buf_ += v; buf_ += ','; }

    std::string take() { trim_comma(); return std::move(buf_); }
    const std::string& str() const { return buf_; }

private:
    void trim_comma() {
        if (!buf_.empty() && buf_.back() == ',') buf_.pop_back();
    }
    std::string buf_;
};

// =============================================================================
// JSON Helpers (dispatch_router.cpp)
// =============================================================================

const char* json_get_string(const char* json, const char* key);
int json_get_int(const char* json, const char* key);
bool json_get_bool(const char* json, const char* key, bool default_val = false);
// Strict boolean accessor (N3, surface-audit 2026-08-01): returns true and sets
// *out ONLY when the key is present (top-level or under "params") AND is a JSON
// boolean. Returns false when absent or present-with-any-other-type. Callers
// pair it with json_has_key to distinguish "absent" (legacy default semantics)
// from "present but not a boolean" (typed reject). The lenient json_get_bool
// silently returned default_val for e.g. "enabled":"banana" — the N3
// silent-accept mechanism.
bool json_get_bool_strict(const char* json, const char* key, bool* out);
bool json_has_key(const char* json, const char* key);

// =============================================================================
// Pointer Validation Helper
// =============================================================================

static inline bool safe_ptr(uintptr_t p) {
    return p >= 0x10000 && p < 0x7FFFFFFFFFFF;
}

// =============================================================================
// Character Lookup Helpers (dispatch_router.cpp)
// =============================================================================

void* get_controlled_character();
uint32_t get_player_character_id();
void* resolve_character(const char* params_json);
void* resolve_actor(const char* params_json);
// Player-protection invariant: returns true (and writes a rejection to *out) if
// the actor IS the human player. Mutating handlers gate on this; the player is a
// valid TARGET but never a valid ACTOR. No-op in observer mode (no player).
bool reject_player_actor(uint32_t actor_id, const char* action, std::string* out);
bool reject_player_actor(void* actor, const char* action, std::string* out);
bool is_sentinel(void* ptr);
uint32_t char_get_id(void* character);
void* char_get_realm(void* character);

// Monthly telemetry emitter (query_handlers.cpp). Appends ONE compact JSONL line
// describing the controlled ruler to <telemetry>/gen_current.jsonl on each
// game-MONTH rollover. Cheap, crash-safe, no engine calls; no-op when no
// controlled character. Call from on_tick (game thread). raw_date is the engine
// date at game_state+GAME_DATE; the function tracks month internally and only
// writes on a change.
void emit_monthly_telemetry(int32_t raw_date);

// =============================================================================
// Title/Realm Helpers (dispatch_router.cpp)
// =============================================================================

uintptr_t resolve_title_realm(uintptr_t char_addr);
uint32_t get_liege_id_from_addr(uintptr_t char_addr);
uintptr_t lookup_title_obj(uint32_t title_id);
int get_title_tier(uintptr_t title_ptr);

// Resolve a landed-title script key (e.g. "e_britannia", "d_northumbria") to its
// title id by iterating LANDED_TITLE_DB + matching the title-type NAME_KEY.
// Returns 0xFFFFFFFF if not found. Defined in action_handlers.cpp.
uint32_t resolve_title_id_by_key(const char* key);

// Recursively collect tier-2 (county) de-jure descendant title ids of `title_id`
// into `out` (`seen` is the caller-supplied cycle-guard scratch, `depth` starts 0).
// Pure safe_read; reused by the #131 create_title de-jure gate and the read-only
// de-jure meter. Defined in intent_dispatch.cpp.
void collect_dejure_counties(uint32_t title_id, std::vector<uint32_t>& out,
                             std::vector<uint32_t>& seen, int depth);

// =============================================================================
// DB Object / String Helpers (dispatch_router.cpp)
// =============================================================================

std::string ad_read_std_string(uintptr_t str_addr);
uintptr_t ad_db_lookup(uintptr_t db_global, uint32_t id);
std::string ad_read_char_name(uintptr_t c);
void emit_char_names(ResponseBuilder& r, uintptr_t c);
// FIX #culture_name: emit culture_id + culture (display name +0x260) + culture_key
// (scripted key +0x1C8). Safe on null/unmapped obj (emits just the id).
void emit_culture_fields(ResponseBuilder& r, uintptr_t culture_obj, uint32_t culture_id,
                         const char* id_key = "culture_id",
                         const char* name_key = "culture",
                         const char* script_key = "culture_key");
void emit_culture_fields_by_id(ResponseBuilder& r, uint32_t culture_id,
                               const char* id_key = "culture_id",
                               const char* name_key = "culture",
                               const char* script_key = "culture_key");
bool read_db_obj_name(uintptr_t obj_ptr, const char*& name_out, size_t& len_out);
const char* trait_category_name(uint8_t cat);

// =============================================================================
// Interaction/Decision Lookup (dispatch_router.cpp)
// =============================================================================

void* lookup_decision(const char* decision_type);
void* lookup_interaction(const char* interaction_type);

// =============================================================================
// AI Control (dispatch_router.cpp)
// =============================================================================

void disable_all_ai();
void enable_all_ai();

// =============================================================================
// Subsystem Toggles (dispatch_router.cpp)
// =============================================================================

struct SubsystemToggle {
    const char* name;
    uintptr_t offset;
};
extern const SubsystemToggle g_subsystem_toggles[];
extern const size_t g_subsystem_toggle_count;

// =============================================================================
// Query Handlers (query_handlers.cpp)
// =============================================================================

bool do_query_character(const char* params_json, std::string* out);
bool do_query_opinion(const char* params_json, std::string* out);
bool do_query_opinion_breakdown(const char* params_json, std::string* out);
bool do_query_imprisonment_reasons(const char* params_json, std::string* out);
bool do_query_realm(const char* params_json, std::string* out);
// READ-ONLY de-jure-control meter: how many of <title_key>'s (default "e_britannia")
// de-jure counties does ruler character_id hold? Returns {held,total,fraction}.
// Pure safe_read; never writes / never calls a hangable engine fn (replaces the
// hang-prone create_title-refusal probe). Defined in query_handlers.cpp.
bool do_query_dejure_progress(const char* params_json, std::string* out);
bool do_query_succession(const char* params_json, std::string* out);
// Per-title partition map keyed on a CHARACTER: for each held title (tier>1),
// title_key/tier/projected_heir_id/projected_heir_name/succession_law +
// splits_away (heir differs from the primary title's heir). Pure safe_read +
// is_mapped; reads the engine's cached per-title heir array, never calls the
// computing succession path. Defined in query_handlers.cpp.
bool do_query_title_succession_map(const char* params_json, std::string* out);
bool do_query_borders(const char* params_json, std::string* out);
bool do_query_claims(const char* params_json, std::string* out);
bool do_query_de_jure(const char* params_json, std::string* out);
bool do_query_county_details(const char* params_json, std::string* out);
bool do_query_development_growth(const char* params_json, std::string* out);
bool do_query_county_neighbors(const char* params_json, std::string* out);
bool do_query_province_adjacency(const char* params_json, std::string* out);
// READ-ONLY baked map-graph window (map_graph.cpp). mmaps ck3map.bin (built by
// ck3mapgen) and BFS-windows the baked CSR county graph from a scope-derived seed
// set, painting a LIVE overlay (holder / faith / culture / dev / control) onto only
// the windowed nodes. ZERO topology/de-jure/terrain memory reads (all baked). Self-
// disables to {"error":"map_artifact_missing"} when the artifact is absent. The
// loader map_graph_init() is called once from bridge init.
bool do_query_map_graph(const char* params_json, std::string* out);
void map_graph_init();
bool do_query_title_hierarchy(const char* params_json, std::string* out);
bool do_query_wars(const char* params_json, std::string* out);
// L6: unpaused-safe minimal active-wars brief for the per-turn agent report.
// Bridge-internal (served inline even while UNPAUSED) — best-effort safe_read.
bool do_get_active_wars(const char* params_json, std::string* out);
bool do_query_war_db_scan(const char* params_json, std::string* out);
// ck-sd4d: ended wars, from the WAR_END_CORE hook ledger (war_history_hook.cpp).
bool do_query_war_history(const char* params_json, std::string* out);
bool do_query_war_score_breakdown(const char* params_json, std::string* out);
// READ-ONLY per-war AI war-stance / siege-intent visibility (re_query_war_stance.md,
// limitations 5–7). Pure memory reads: stance-DB 7-slot reverse map (core-confirmed)
// + (behind g_war_stance_record_walk_enabled, default OFF) the AI-mgr coordinator
// record walk that reads the LIVE chosen stance ptr per war. Derives will_siege/stuck
// so the agent bails (white-peace) a defensive/occupation-0 war before the bleed.
bool do_query_war_stance(const char* params_json, std::string* out);
// Runtime flag: arm the AI-mgr coordinator record walk inside do_query_war_stance.
// DEFAULT OFF — the record↔war-id match key (rec+0x18) is decompile-confirmed but
// not yet live-probed on a known-at-war record (re_query_war_stance.md §6). When OFF
// the handler still returns occupation-derived stance from the war-score cache.
extern std::atomic<bool> g_war_stance_record_walk_enabled;
bool do_query_council(const char* params_json, std::string* out);
bool do_query_factions(const char* params_json, std::string* out);
bool do_query_vassal_loyalty(const char* params_json, std::string* out);
bool do_query_faction_threat(const char* params_json, std::string* out);
bool do_query_schemes(const char* params_json, std::string* out);
bool do_query_incoming_schemes(const char* params_json, std::string* out);
bool do_query_armies(const char* params_json, std::string* out);
bool do_query_lifestyle(const char* params_json, std::string* out);
bool do_query_interaction_acceptance(const char* params_json, std::string* out);
bool do_query_relations(const char* params_json, std::string* out);
bool do_query_hooks(const char* params_json, std::string* out);
bool do_query_available_cbs(const char* params_json, std::string* out);
bool do_query_valid_cbs(const char* params_json, std::string* out);
bool do_query_cb_validity_strict(const char* params_json, std::string* out);
// READ-ONLY enumeration of ALL win-line casus belli for an (attacker, target)
// pair, with per-CB validity/scope/grants/requirements_unmet/notes. Curated view
// over cb_query_strict_full_sync (UNPAUSED required — engine probe on game thread).
bool do_query_cbs_for_target(const char* params_json, std::string* out);
// BATCHED win-line CB enumeration: resolve N target_ids in ONE deferred drain
// window (one coordinator pulse) instead of N. Same single-flight g_cb_query
// discipline + per-target body as do_query_cbs_for_target (_slot_drain_profile.md §3b).
bool do_query_cbs_for_targets(const char* params_json, std::string* out);
bool do_query_alliances(const char* params_json, std::string* out);
bool do_query_call_to_arms(const char* params_json, std::string* out);
bool do_query_diplomatic_range(const char* params_json, std::string* out);
bool do_query_cb_catalog(const char* params_json, std::string* out);
bool do_get_tick_info(const char* params_json, std::string* out);

// ck-9fc3: the harness declares WHICH SAVE is loaded; the bridge decides whether
// quarantined agent steering may be restored. See include/world_identity.h.
bool do_declare_save_identity(const char* params_json, std::string* out);

// ck-6kvq.5: install the SessionRegistry steering-purge hook.  Called once from
// bridge_init(), never from a static initialiser (ck-22n3 / ck-xqol: dll_init
// runs before most .init_array entries, so anything a constructor installs may
// not exist yet when the reader thread first touches it).
void install_steering_purge_hook();
bool do_get_ai_evaluations(const char* params_json, std::string* out);
bool do_search_characters(const char* params_json, std::string* out);
bool do_query_heir_education(const char* params_json, std::string* out);
bool do_query_marriage_candidates(const char* params_json, std::string* out);
bool do_query_title_claimants(const char* params_json, std::string* out);
bool do_query_title_history(const char* params_json, std::string* out);
bool do_query_title_succession(const char* params_json, std::string* out);
bool do_query_valid_war_targets(const char* params_json, std::string* out);
// #110: de-jure-duchy-completion conquest targeting. Surfaces conquerable
// counties (held by independent, non-kin, non-allied neighbors) to build toward
// a 2nd duchy, with a per-duchy completion rollup. Optional engine
// county_conquest_cb validity check sets conquerable_now authoritatively.
bool do_query_conquest_targets(const char* params_json, std::string* out);
bool do_query_cb_options(const char* params_json, std::string* out);
bool do_query_election_state(const char* params_json, std::string* out);
bool do_query_dynasty(const char* params_json, std::string* out);
bool do_query_levy_readiness(const char* params_json, std::string* out);
bool do_query_partition_preview(const char* params_json, std::string* out);
bool do_query_succession_simulation(const char* params_json, std::string* out);
bool do_query_pending_events(const char* params_json, std::string* out);
bool do_query_event_probabilities(const char* params_json, std::string* out);
bool do_query_building_types(const char* params_json, std::string* out);
bool do_query_memory_probe(const char* params_json, std::string* out);
bool do_query_character_memories(const char* params_json, std::string* out);
bool do_query_resolve_global(const char* params_json, std::string* out);
bool do_query_trait_db(const char* params_json, std::string* out);
bool do_query_trait_compatibility(const char* params_json, std::string* out);
bool do_query_realm_laws(const char* params_json, std::string* out);
bool do_query_law_groups(const char* params_json, std::string* out);
bool do_query_available_law_changes(const char* params_json, std::string* out);
bool do_query_innovation_progress(const char* params_json, std::string* out);
bool do_query_fascination_progress(const char* params_json, std::string* out);
bool do_query_research_speed(const char* params_json, std::string* out);
std::string enumerate_interactions();
std::string enumerate_decisions();

// Annotated decision enumeration (ck-decision-validity). Evaluates each
// decision's is_shown/is_valid/cost triggers against `char_id` under the
// crash-net (fake RNG scope + watchdog + sigsetjmp) and emits, per decision,
// {name, hash, shown, valid, cost_ok, available, failures[], cost{}, current{}}.
// If `valid_only` is true, only decisions with available==true are emitted
// (powers discover(what="valid_decisions")). When char_id==0 the controlled /
// player character is used. Heavy — NOT used on the snapshot path.
std::string enumerate_decisions_annotated(uint32_t char_id, bool valid_only,
                                          const std::vector<uint32_t>& req_hashes = {},
                                          const std::vector<std::string>& req_names = {});
bool do_list_valid_decisions(const char* params_json, std::string* response_out);
bool do_list_decisions_annotated(const char* params_json, std::string* response_out);

// =============================================================================
// Action Handlers (action_handlers.cpp)
// =============================================================================

bool do_send_interaction(const char* params_json, std::string* response_out);
bool do_send_decision(const char* params_json, std::string* response_out);
bool do_set_lifestyle_focus(const char* params_json, std::string* response_out);
bool do_set_education_focus(const char* params_json, std::string* response_out);
bool do_set_council_task(const char* params_json, std::string* response_out);
bool do_query_fabricate_targets(const char* params_json, std::string* response_out);
bool do_construction_check(const char* params_json, std::string* response_out);
bool do_add_construction(const char* params_json, std::string* response_out);
bool do_construct_building(const char* params_json, std::string* response_out);

// ALLOW_RANDOM_IN_SCOPE guard (intent_dispatch.cpp) — arm a per-thread fake RNG
// scope around any engine call that advances the RNG (else null-deref in
// FUN_0774a4b0, the steered-war crash). arm returns true if armed; pass the
// saved value to restore afterward.
bool arm_fake_random_scope(uint64_t* out_saved);
void restore_fake_random_scope(uint64_t saved);
bool do_declare_war(const char* params_json, std::string* response_out);
bool do_declare_war_debug(const char* params_json, std::string* response_out);
bool do_queue_intent(const char* params_json, std::string* response_out);
bool do_query_intents(const char* params_json, std::string* response_out);
bool do_query_intent_results(const char* params_json, std::string* response_out);
// ck-jsis0: probe a single dispatch by id. Returns success / rejected /
// crashed / pending / not_found. Disambiguates the cases query_intent_results
// silently rolled together — "not in the last N completed" used to overlap
// with "still pending" and "evicted from the completed ring".
bool do_query_dispatch_result(const char* params_json, std::string* response_out);
bool do_cancel_intent(const char* params_json, std::string* response_out);
bool do_clear_intents(const char* params_json, std::string* response_out);
bool do_respond_event(const char* params_json, std::string* response_out);
bool do_change_realm_law(const char* params_json, std::string* response_out);
bool do_pre_validate_war(const char* params_json, std::string* response_out);
bool do_execute_console_command(const char* params_json, std::string* response_out);
bool do_execute_scripted_effect(const char* params_json, std::string* response_out);
bool do_execute_engine_effect(const char* params_json, std::string* response_out);
// break-alliance (re_break_alliance_impl.md). The literal CBreakAllowAllianceEffect
// break via the gated break_alliance_cmd (Path B). BUILD-READY but observer-save-
// GATED: the direct-cmd arg unwrap + packed-ref masking are unverified live
// (MEDIUM), so the real engine call only runs behind an explicit
// confirm_observer_save=true param. Default returns an honest gated refusal and
// points the agent at the safe-now `conqueror`-variable scripted-effect lever.
bool do_break_alliance(const char* params_json, std::string* response_out);
// ck-kkaxx / ck-zvu5l.3: generalised script-effect primitive. Parses the
// outer `<effect> = { … }` block structurally and routes known effects to
// existing direct-call handlers (today: `start_war` → call_war_create_core).
// Other outer effects return `not_implemented_pending_compiler_re` until the
// PDX-script compiler entry point (ck-zvu5l.1 / .2) lands.
// See re/findings/re_script_block_compiler_quest.md.
bool do_execute_script_block(const char* params_json, std::string* response_out);
// ck-eyfb: direct WAR_CREATE_CORE invocation, bypassing CStartWarEffect.
// ck-j3vb0: `action_label` overrides the response's "action" string so
// declare_war's force-path branch can surface a unified action name to
// callers. When `include_path_marker` is true, the response also carries
// a "path":"force_player_path" field for caller routing diagnostics.
bool do_call_war_create_core(const char* params_json, std::string* response_out,
                              const char* action_label = "call_war_create_core",
                              bool include_path_marker = false);

// War Declaration Observer (war_declaration_hook.cpp)
bool do_query_war_declarations(const char* params_json, std::string* out);
bool do_query_scheme_inits(const char* params_json, std::string* out);

// AI_CB_SELECTOR Path B' override (ai_cb_selector_hook.cpp, ck-8weg)
bool do_set_cb_selector_override(const char* params_json, std::string* response_out);
void set_cb_selector_override(uint64_t cb_ref,
                              uint64_t cb_titles_ref,
                              uint64_t target_ref,
                              int32_t  score,
                              uint8_t  extra_roll,
                              uint64_t titles_data     = 0,
                              int32_t  titles_count    = 0,
                              uint32_t claimant_handle = 0xFFFFFFFFu);
void clear_cb_selector_override();
bool cb_selector_override_active();

// Diagnostic: hook on DECLARE_WAR_SPECIAL_HANDLER (0x065db970) — logs every
// engine-dispatched declare_war command's WCC args (atk/def/claimant/cb_type).
void init_war_decl_handler_hook();

// Marriage Observation Observer (marriage_observation_hook.cpp)
bool do_query_marriage_observations(const char* params_json, std::string* out);

// Phase-3 Wave-2 probes (phase3_probes.cpp, ck-e58l.8)
bool do_probe_interaction_cascade(const char* params_json, std::string* response_out);
bool do_probe_gate_4_slots(const char* params_json, std::string* response_out);
bool do_list_pending_interactions(const char* params_json, std::string* response_out);
bool do_tick_diff(const char* params_json, std::string* response_out);
bool do_probe_war_orphans(const char* params_json, std::string* response_out);
// Game-thread processor for probe_interaction_cascade; called from drain_and_execute_intents.
// Returns true if there was a pending cascade query that was processed.
void process_cascade_query();
// Non-blocking check — true if a cascade-probe request is pending on the game thread.
bool cascade_query_pending();

// Council Task Assignment Observer (council_task_hook.cpp)
bool do_query_council_tasks(const char* params_json, std::string* out);

// Per-ruler Council-AI Suppression (council_suppress_hook.cpp, L20 stickiness)
// Installs an inline hook on AI_COUNCIL_TASKS_PER_RULER (FUN_04dca2a0). While a
// ruler id is in g_council_locked, the native per-ruler council-task
// re-assignment routine is skipped for that ruler so a bridge-set council task
// sticks. Default empty → AI keeps full control. Fails OPEN (runs the original)
// on any unexpected state. council_lock_add is called when do_set_council_task
// successfully applies a task; council_lock_remove on release_character.
void init_council_suppress_hook();
void council_lock_add(uint32_t char_id);
void council_lock_remove(uint32_t char_id);

// Per-character OFF-GOAL routine-war veto (offgoal_war_veto_hook.cpp,
// re_offgoal_veto_FINAL.md). Installs an entry hook on AI_WAR_POST_DECL. While
// g_offgoal_veto_enabled is true, a bridge-controlled ruler on the ROUTINE engine
// path (bypass==0) declares no routine offensive war (the hook returns without
// the original). bypass==1 (the bridge's own steered war) is always exempt; every
// other realm is untouched. DEFAULT OFF (ships inert). Fails OPEN on any doubt.
// set_offgoal_veto {enabled:bool} flips the flag at runtime.
extern std::atomic<bool> g_offgoal_veto_enabled;
void init_offgoal_war_veto_hook();
bool do_set_offgoal_veto(const char* params_json, std::string* out);
bool do_pause_game(const char* params_json);
bool do_unpause_game(const char* params_json);
bool do_set_speed(const char* params_json);
bool do_inject_mouse_click(const char* params_json, std::string* response_out);
bool do_select_observe_default(const char* params_json, std::string* response_out);
bool do_enter_observe_mode(const char* params_json, std::string* response_out);
bool do_toggle_ai_subsystem(const char* params_json);
bool do_set_character_ai(const char* params_json);

// =============================================================================
// Personality Handlers (personality.cpp)
// =============================================================================

bool do_get_ai_personality(const char* params_json, std::string* out);
bool do_set_ai_personality(const char* params_json, std::string* out);

// =============================================================================
// AI Eval Hook Handlers (ai_eval_hook.cpp)
// =============================================================================

bool execute_direct_dispatch(Intent* intent, void* character, void* interaction);

// Console command execution on the game thread (ai_eval_hook.cpp)
void execute_console_intent(Intent* intent);

// Scripted-effect invocation on the game thread (intent_dispatch.cpp, ck-7ejs).
// Looks up a CJominiScriptedEffectTemplate by name in the user-scripted DB and
// runs its compiled effect tree on the supplied character as ROOT scope.
bool execute_scripted_effect_intent(Intent* intent);

// ck-eyfb: direct WAR_CREATE_CORE invocation (intent_dispatch.cpp).
// Mirrors WAR_DECLARE_SPECIAL_HANDLER (FUN_065db970): resolves CB by name,
// builds a minimal CB-params struct with a single-title array, and invokes
// WAR_CREATE_CORE on the game thread. Sets intent->executed/rejected and
// stores war_id + a small JSON blob in intent->console_output.
bool execute_war_create_core_intent(Intent* intent);
bool execute_faction_intent(Intent* intent);
// A8.2 (re_raise_armies.md): raise the chosen AI's levies via the guarded
// g_PlayerCharacterID global-swap primitive. Game-thread only (drained from
// on_tick). NEEDS-LIVE-HARNESS-VALIDATION. Queued by do_raise_armies.
bool execute_raise_armies_intent(Intent* intent);
bool do_raise_armies(const char* params_json, std::string* out);
// #96 (re_raid_system.md): toggle the chosen AI's raised armies into RAIDING
// mode via the guarded g_PlayerCharacterID global-swap primitive. Game-thread
// only (drained from on_tick). Queued by do_raise_raiders.
bool execute_raise_raiders_intent(Intent* intent);
bool do_raise_raiders(const char* params_json, std::string* out);
// #111 (re_create_title_action.md): LEGITIMATE de-jure create-title. Builds a
// CCreateTitleCommand for the AI ruler, runs its CanExecute gate, and on PASS
// dispatches it via GAME_COMMAND_DISPATCH(0xe). Game-thread only (drained from
// on_tick). Queued by do_create_title.
bool execute_create_title_intent(Intent* intent);
bool do_create_title(const char* params_json, std::string* out);
bool do_dispatch_faction_intent(const char* params_json, std::string* out);
// #132 (docs/notes/hostile-intrigue-research.md): LEGITIMATE start-a-scheme
// (murder / fabricate_hook / claim_throne) via CreateActiveScheme forward, which
// runs the engine's own scheme-start gates internally (self-gating). Game-thread
// only (drained from on_tick). Queued by do_dispatch_scheme_intent. The 5th/6th
// CreateActiveScheme args are captured from real engine schemes by the
// CREATE_ACTIVE_SCHEME observer hook (scheme_get_arg_template, scheme_init_hook.cpp).
bool execute_scheme_intent(Intent* intent);
bool execute_scheme_cancel_intent(Intent* intent);  // #scheme_cancel
// break-alliance (re_break_alliance_FINAL.md, Path B). Game-thread executor for
// IntentType::BREAK_ALLIANCE: resolves actor+ally CCharacter* via lookup_character,
// pre-gates on IsAlliedTo, and calls the gated break_alliance_cmd(actor, ally, 0)
// under fake-RNG + watchdog + sigsetjmp, then reads back IsAlliedTo to confirm.
// Drained from on_tick. Default-OFF behind g_enable_break_alliance (do_break_alliance).
bool execute_break_alliance_intent(Intent* intent);
// Runtime flag (default false) gating the real break-alliance write. While false,
// do_break_alliance returns the honest observer-save-gated refusal + the safe-now
// `conqueror`-variable alternative; only when armed does it queue the BREAK_ALLIANCE
// intent. Settable via set_break_alliance_enabled {enabled:bool}.
extern std::atomic<bool> g_enable_break_alliance;
bool do_set_break_alliance_enabled(const char* params_json, std::string* out);

// CAIWarCoordinator offensive-stance drive — Fix B1a (re_caiwarcoordinator_FINAL.md,
// re_steered_war_offensive_root.md). Makes a FORCE-DECLARED steered war get
// prosecuted OFFENSIVELY by driving the engine's OWN per-coordinator stance tick
// (COORD_STANCE_TICK) on our war's record — a LEGITIMATE engine re-eval, NOT a
// memory poke (the engine re-selects + writes rec+0x70 via its real selector +
// strength condition). Game-thread executor, drained from WAR_FORCE_OFFENSIVE_STANCE
// under fake-RNG + watchdog + sigsetjmp. DEFAULT-OFF behind g_force_offensive_stance;
// NEEDS-LIVE-VALIDATION (attempt-25). actor_id = AI attacker; title_id = war_id.
extern std::atomic<bool> g_force_offensive_stance;
bool execute_war_force_offensive_stance_intent(Intent* intent);
bool do_force_offensive_stance(const char* params_json, std::string* response_out);
bool do_set_force_offensive_stance(const char* params_json, std::string* out);

// REMOVED: move_army/order_siege mechanism deleted per design (agent does not drive
// army micro) — do_move_army / do_set_move_army_enabled / execute_move_army_intent /
// g_enable_move_army.

bool do_dispatch_scheme_intent(const char* params_json, std::string* out);
bool do_move_capital(const char* params_json, std::string* out);
bool do_set_obligation_level(const char* params_json, std::string* out);
bool do_appoint_councillor(const char* params_json, std::string* out);
// #79: default-OFF flag gating the vacant-seat fill path in do_appoint_councillor.
// While false, a vacant seat returns vacant_fill_disabled; when armed, the fill calls
// COUNCIL_SEAT_ASSIGN with the candidate's FULL 32-bit handle. Settable via
// set_vacant_fill_enabled {enabled:bool}.
extern std::atomic<bool> g_enable_vacant_fill;
bool do_set_vacant_fill_enabled(const char* params_json, std::string* out);
bool do_maa_recruit(const char* params_json, std::string* out);
bool do_maa_station(const char* params_json, std::string* out);
bool do_maa_unstation(const char* params_json, std::string* out);
bool do_hire_mercenary(const char* params_json, std::string* out);
bool do_set_doctrine(const char* params_json, std::string* out);
bool do_set_character_faith(const char* params_json, std::string* out);
bool do_set_character_culture(const char* params_json, std::string* out);
bool do_set_title_law(const char* params_json, std::string* out);
bool do_query_joinable_factions(const char* params_json, std::string* out);
bool do_query_hireable_mercenaries(const char* params_json, std::string* out);
const char* get_interaction_name(void* interaction);
uint32_t get_interaction_hash(void* interaction);

// =============================================================================
// Deferred Acceptance Query (ai_eval_hook.cpp)
// =============================================================================
// Acceptance queries must run on the game thread (TLS + engine functions).
// The IPC thread fills in the request fields, the game thread evaluates during
// drain_and_execute_intents, and the IPC thread polls for the result.

struct AcceptanceQuery {
    // --- Request fields (written by IPC thread) ---
    uint32_t actor_id;
    uint32_t target_id;
    char interaction_type[128];

    // --- Result fields (written by game thread) ---
    bool valid;              // interaction is valid for this actor
    bool would_accept;       // AI_INTERACTION_DECISION returned true (target accepts)
    char error[128];         // error message if evaluation failed

    // --- Synchronization ---
    std::atomic<int> state;  // 0=empty, 1=pending, 2=done, 3=error
};

// Global deferred acceptance query slot.
// Only one query can be in flight at a time (sufficient for single-agent).
extern AcceptanceQuery g_acceptance_query;

// Process the pending acceptance query on the game thread.
// Called from drain_and_execute_intents().
void process_acceptance_query();

// =============================================================================
// Deferred CB Validity Query (ai_eval_hook.cpp)
// =============================================================================
// CB validity queries must run on the game thread (trigger evaluator reads game
// state). The IPC thread fills in actor_id/target_id, the game thread iterates
// the CB type database and calls CB_VALIDITY_CHECK for each enabled CB type,
// storing results. The IPC thread polls for the result.

struct CBQueryResult {
    uint32_t cb_index;
    char     cb_name[128];
    bool     valid;
    int32_t  target_mode;  // 0=none, 6=claim, other=title
    uint64_t eval_us;      // CB_VALIDITY_CHECK wall time, microseconds (ck-wc9w)

    // Strict-mode per-CB outer/inner verdicts (ck-5qpg).
    // Populated only when CBQuery::strict_mode==true. In non-strict mode the
    // outer fields are left as their default-initialised state ("" / pass /
    // unrun) and only `valid` carries inner-pass status, matching the legacy
    // contract for cb_query_count_valid_sync callers.
    //
    // outer_status: "pass"     — Stage 1+2 outer filter accepted the CB
    //               "dropped"  — outer filter dropped the CB; inner unrun
    //               ""         — non-strict mode (legacy)
    // outer_reason: short tag describing the dropout (e.g. "group_disabled",
    //               "stage2_flag", "no_group", "cb_disabled"). Empty on pass.
    // inner_status: "pass" / "fail" — CB_VALIDITY_CHECK ran
    //               "unrun"         — inner skipped because outer dropped
    char     outer_status[16];
    char     outer_reason[64];
    char     inner_status[16];
};

struct CBQuery {
    // --- Request fields (written by IPC thread) ---
    uint32_t actor_id;
    uint32_t target_id;
    // Strict mode: when true, process_cb_query applies AI_CB_EVALUATE_ALL Stage 2
    // group-level outer-trigger flag filters (cb_group::DISABLED_FLAG +
    // cb_group::STAGE2_FLAG) before invoking CB_VALIDITY_CHECK on each cb_type.
    // Synchronized through `state` — set before publishing 0→1, read after 1→4.
    bool strict_mode;

    // --- Result fields (written by game thread) ---
    std::vector<CBQueryResult> results;
    char error[128];

    // ck-9fbs: per-CB ENABLED_FLAG enumeration. Populated by process_cb_query
    // alongside `results`. Disabled CBs are skipped before any per-pair
    // evaluation, so they never appear in `results`; counting them here
    // surfaces the DLC-disabled / has_game_rule subset for the current install
    // (BuildID-pinned). `disabled_cb_names` mirrors cb_type names for CBs
    // whose ENABLED_FLAG byte == 0; sort happens at JSON emit time.
    int enabled_count;
    int disabled_count;
    std::vector<std::string> disabled_cb_names;

    // D8 guard (probe P4, 2026-08-01): number of CB_VALIDITY_CHECK calls in the
    // last serviced query whose option enumeration FILLED the shared output
    // buffer to capacity — i.e. one entry away from the engine's CDynArray
    // growth path through the invalid CB_OPTION_ALLOCATOR_VTABLE (the 07-31 /
    // 08-01 SIGSEGV class). Reset per service by process_cb_query; copied into
    // CBStrictSummary::option_capacity_hits.
    int option_capacity_hits;

    // --- Synchronization ---
    // 0=empty, 1=pending, 2=done, 3=error, 4=in-progress
    std::atomic<int> state{0};

    // --- Servicing instrumentation (ck-wc9w) ---
    // steady_clock ns at state transitions — set by submit/service sides
    // respectively, read on timeout to classify race vs hang vs empty.
    std::atomic<uint64_t> submit_ns{0};         // IPC thread: set when state 0→1
    std::atomic<uint64_t> service_start_ns{0};  // game thread: set when state 1→4
    std::atomic<uint64_t> service_end_ns{0};    // game thread: set when state 4→2/3
    std::atomic<uint64_t> submit_tick{0};       // tick counter at submit (0 if unknown)
    std::atomic<uint64_t> service_tick{0};      // tick counter at service start
};

extern CBQuery g_cb_query;

// H3 (re_warplanner_lifecycle.md §1.4): count of cb_query_busy returns since
// bridge load. Instrumentation only — surfaced in bridge() health so we can tell
// whether H1 actually reduced slot collisions in the live run. Zero behavioural
// change. Incremented at each cb_query_busy return in the sync entry points.
extern std::atomic<uint64_t> g_cb_query_busy_count;

// Process the pending CB validity query on the game thread.
// Called from drain_and_execute_intents().
void process_cb_query();

// Synchronous CB validity probe for IPC-thread callers (e.g. do_pre_validate_war).
// Reuses the deferred g_cb_query slot: waits briefly for any in-flight query to
// drain, submits a new one for (actor, target), and polls until the game thread
// reports a result or the timeout expires.
//
// Returns:
//   >= 0 — count of CBs that pass the engine validity pipeline (skip_gui=1)
//   -1   — slot stayed busy beyond the busy_wait window (could not submit)
//   -2   — game thread reported an error (e.g. attacker/defender lookup miss)
//   -3   — timed out waiting for the game thread to service the query
//
// On non-negative return, *out_total receives the total CBs evaluated (for
// distinguishing "no enabled CBs" vs "all enabled CBs rejected"). On negative
// return, *out_error (capacity error_size) receives a short tag.
int cb_query_count_valid_sync(uint32_t actor_id,
                              uint32_t target_id,
                              int      timeout_ms,
                              int*     out_total,
                              char*    out_error,
                              size_t   error_size);

// Strict variant of cb_query_count_valid_sync: applies AI_CB_EVALUATE_ALL's
// Stage 1 bilateral pre-filter (target IsRuler + IsAlive + actor!=target)
// on the IPC thread, then submits the deferred query in strict mode so the
// game thread additionally applies the Stage 2 group-level outer-trigger
// flag filter (cb_group::DISABLED_FLAG + STAGE2_FLAG) before each
// CB_VALIDITY_CHECK call. Mirrors ck-39e1 Stage 1+2 (Stage 4 deferred).
//
// Returns:
//   >= 0 — count of CBs valid under both bridge and engine outer filters
//   -1   — slot busy
//   -2   — game thread error
//   -3   — timeout
//   -4   — bilateral pre-filter rejected the pair (target dead/non-ruler/self)
//          — `*out_total` is set to 0 and `*out_error` carries the reject reason.
int cb_query_count_valid_strict(uint32_t actor_id,
                                uint32_t target_id,
                                int      timeout_ms,
                                int*     out_total,
                                char*    out_error,
                                size_t   error_size);

// Aggregate verdict counts populated by cb_query_strict_full_sync (ck-5qpg).
// Mirrors the spec's `summary` block for query_cb_validity_strict.
struct CBStrictSummary {
    int total_cbs;            // total CBs visited (incl. disabled-skipped)
    int outer_filter_dropped; // outer (Stage 1+2) dropped — inner not run
    int candidates_post_outer;// outer-pass CBs (inner ran on these)
    int inner_valid;          // CB_VALIDITY_CHECK pass
    int inner_invalid;        // CB_VALIDITY_CHECK fail
    // ck-9fbs: ENABLED_FLAG enumeration of the per-install CB DB. Lets agents
    // deterministically enumerate the DLC-disabled subset without heuristic
    // name-prefix matching (fp1_, ep3_, mpo_, …). `disabled_cb_names` is
    // unsorted as captured; do_query_cb_validity_strict sorts before emit.
    int enabled_count;
    int disabled_count;
    std::vector<std::string> disabled_cb_names;
    // D8 guard: see CBQuery::option_capacity_hits. >0 ⇒ option data for at
    // least one CB was truncated at the bridge buffer capacity (and the fatal
    // engine growth path was one entry away) — surfaced by
    // do_query_cb_validity_strict as `option_capacity_hits` +
    // `cb_enumeration_truncated`.
    int option_capacity_hits;
};

// Strict variant that surfaces the full per-CB result vector (with outer
// reason strings) alongside the aggregate count. Same Stage-1 bilateral
// pre-filter as cb_query_count_valid_strict; same Stage-2 game-thread
// filter inside process_cb_query. Per-CB outer triggers
// (allowed_for_character / allowed_against_character / valid_to_start) are
// not yet evaluated here — they are deferred to ck-39e1 and will populate
// additional outer_reason values when shipped.
//
// Returns: same return-code shape as cb_query_count_valid_strict.
//          >= 0 — inner_valid count; *out_summary populated; *out_results
//                 receives a copy of the per-CB result vector.
//          -1 / -2 / -3 / -4 — same semantics as the count-only variant.
int cb_query_strict_full_sync(uint32_t actor_id,
                              uint32_t target_id,
                              int      timeout_ms,
                              CBStrictSummary*           out_summary,
                              std::vector<CBQueryResult>* out_results,
                              char*    out_error,
                              size_t   error_size);

// =============================================================================
// Deferred Trigger Evaluation Query (ai_eval_hook.cpp)
// =============================================================================
// Evaluates individual script trigger slots on an interaction definition.
// Uses the TLS pipeline to construct a proper scope context with actor/target,
// then calls the universal trigger evaluator (FUN_07628500) on each requested
// slot. This lets us pinpoint exactly which trigger rejects an interaction.
//
// The IPC thread fills in the request fields, the game thread evaluates during
// drain_and_execute_intents, and the IPC thread polls for the result.

struct TriggerSlotResult {
    int      slot;           // 0-6
    char     name[48];       // e.g. "is_shown"
    uint32_t offset;         // offset within interaction_def
    bool     present;        // trigger block exists (non-null)
    bool     result;         // evaluation result (true=pass, false=fail/absent)
    // Loc key from the loud-failure evaluator (ck-wtk7). Populated only when
    // TriggerQuery::extract_loc_keys is set AND the slot was present+failed.
    // Empty string when extraction is disabled, the slot passed, or the loud
    // evaluator produced no SCREAMING_SNAKE_CASE identifier.
    char     loc_key[96];
};

struct TriggerQuery {
    // --- Request fields (written by IPC thread) ---
    uint32_t actor_id;
    uint32_t target_id;
    char interaction_type[128];
    int  trigger_slot;       // -1 = evaluate all 7 slots, 0-6 = specific slot
    // Opt-in loc-key extraction via FUN_0763ddc0 on failing slots (ck-wtk7).
    // Requires live CK3 with bridge loaded; off by default.
    bool extract_loc_keys;

    // --- Result fields (written by game thread) ---
    TriggerSlotResult results[7];
    int  result_count;       // number of populated results
    char error[256];

    // --- Synchronization ---
    // 0=empty, 1=pending, 2=done, 3=error, 4=in-progress
    std::atomic<int> state{0};
};

extern TriggerQuery g_trigger_query;

void process_trigger_query();

// Deferred decision-eval query (#64). The annotated decision eval runs the
// engine's trigger evaluator, which must run on the GAME thread (the watchdog
// is bound there; the IPC thread blocks SIGALRM). IPC handler submits + polls;
// process_decision_eval_query() services it from drain_and_execute_intents.
struct DecisionEvalQuery {
    uint32_t char_id;          // 0 → controlled/player resolved in the eval
    bool     valid_only;       // true = only available decisions
    // #64 list-mode: annotate ONLY these decisions (by name and/or name-hash). Bounded
    // → fast + freeze-safe. Empty → caller serves the light name+hash list instead.
    std::vector<uint32_t>    req_hashes;
    std::vector<std::string> req_names;
    std::string result_json;   // written by game thread
    char     error[128];
    std::atomic<int> state{0}; // 0=empty 1=pending 2=done 3=error 4=in-progress
};
extern DecisionEvalQuery g_decision_eval_query;
void process_decision_eval_query();

// =============================================================================
// Controlled Character / Bridge State (dispatch_router.cpp)
// =============================================================================

bool do_set_controlled_character(const char* params_json, std::string* out);
bool do_release_character(const char* params_json, std::string* out);
bool do_get_bridge_state(const char* params_json, std::string* out);

// =============================================================================
// Trigger Query Handlers (query_handlers.cpp)
// =============================================================================

bool do_query_trigger_result(const char* params_json, std::string* out);

// Synchronous: reads trigger slot structure from memory (IPC-thread safe)
// Returns RTTI class names, children count, child types, label strings (ck-vqth)
bool do_query_trigger_slots(const char* params_json, std::string* out);

// Synchronous: reads current debug flag state from DEBUG_FLAGS_BASE (ck-oaqw)
bool do_query_debug_flags(const char* params_json, std::string* out);

// =============================================================================
// Named Trigger Evaluation (named_trigger_eval.cpp, ck-fo7x.4)
// =============================================================================
// Evaluates a named trigger class against a scope dict. Resolves the class
// via the catalogue at re/rtti/trigger_subclasses.csv, builds a stub trigger
// node, constructs scope via the existing TLS pipeline, and dispatches via
// FUN_07628500. Used by discover(what="evaluate_trigger") in the agent.
bool do_evaluate_named_trigger(const char* params_json, std::string* out);

} // namespace bridge
