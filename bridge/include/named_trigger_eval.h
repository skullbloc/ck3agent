// named_trigger_eval.h — Bridge handler for evaluating a named trigger
// against a scope dict, dispatched via FUN_07628500 (SCRIPT_TRIGGER_EVALUATOR).
//
// Background: Closes ck-fo7x.4 (ADR-0001 §). The trigger catalogue from
// ck-fo7x.1 lives at `re/rtti/trigger_subclasses.csv` and maps every
// CJominiTrigger descendant to its vtable+evaluator. This module loads that
// catalogue on first use and lets callers ask "evaluate trigger X against
// this scope" without going through an interaction definition.
//
// Architecture: same deferred-query pattern as TriggerQuery / CBQuery — the
// IPC thread fills a slot, the game thread evaluates during
// drain_and_execute_intents(), and the IPC thread polls for the result.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace bridge {

// One row from `re/rtti/trigger_subclasses.csv`. We only keep the columns
// we need at runtime; the CSV file remains the canonical source of truth.
struct TriggerCatalogueEntry {
    std::string class_name;        // Itanium-mangled (matches CSV `class_name`)
    std::string demangled;         // best-effort demangle (CSV `demangled`)
    uint64_t    vtable_vfunc_rva;  // = vtable_start + 0x10; trigger_node[0]
    uint64_t    evaluator_rva;     // vtable[0xd0] slot — convenience only
    bool        evaluator_in_text; // false → MEDIUM/LOW (pure-virtual / abstract)
    char        confidence;        // 'H' / 'M' / 'L'
};

// Inputs/outputs for one evaluation request. Single-slot, like the other
// deferred queries — adequate for the single-agent design today.
struct NamedTriggerQuery {
    // --- Request (written by IPC thread) ---
    // Caller-supplied trigger identifier. Tried first as the mangled
    // `class_name` from the catalogue, then as a script keyword via
    // resolve_keyword_to_class().
    char     trigger_name[160];
    // Character ID for the actor/scope object. Maps onto the
    // interaction-context actor slot when scope is built via the TLS
    // pipeline. 0 → controlled-character fallback.
    uint32_t character_id;
    // Optional second character for triggers with a `target` scope
    // (e.g. CWarContributionTrigger). 0 → same as character_id.
    uint32_t target_id;

    // --- Result (written by game thread) ---
    bool     resolved;           // catalogue lookup succeeded
    // TRUE only when the leaf trigger's evaluate vfunc demonstrably ran
    // (shadow-vtable thunk observed the dispatch). A pre-dispatch scope
    // rejection by FUN_07628790 reports evaluated=false with
    // error="scope_rejected" instead — it is NOT a false verdict (D2,
    // fix-plan FIX 4.1).
    bool     evaluated;
    bool     result;             // trigger bool result (only meaningful if evaluated)
    char     resolved_class[160];// canonical mangled class on success
    // Which evaluation root serviced the call (D2 cure diagnostics, ck-ay9o):
    // "borrowed:<slot>" when a real compiled trigger-block head was
    // byte-copied from the sentinel interaction (the cure path — carries the
    // compiled scope-typing instance state a synthetic root lacks);
    // "synthetic_and_root" when the round-3 fallback was used (known to
    // scope-reject; kept so replies stay typed).
    char     root_source[64];
    // RTTI class of the root actually used (mangled, e.g. "11CAndTrigger").
    char     root_class[96];
    uint64_t evaluator_rva;      // copied from catalogue entry
    uint64_t vtable_vfunc_rva;   // copied from catalogue entry
    char     confidence;         // copied from catalogue entry
    char     error[256];         // populated on resolve/setup/eval failure

    // --- Synchronization ---
    // 0=empty, 1=pending, 2=done, 3=error, 4=in-progress
    std::atomic<int> state{0};
};

// Singleton queue slot — mirrors g_trigger_query / g_cb_query / g_acceptance_query.
extern NamedTriggerQuery g_named_trigger_query;

// IPC-thread entry point. Submits the query and polls until the game thread
// services it (or times out). Always sets *out to a JSON envelope.
bool do_evaluate_named_trigger(const char* params_json, std::string* out);

// Game-thread processor — invoked from drain_and_execute_intents() once per
// tick. No-op when no query is pending. Builds a real scope_ctx via the
// existing TLS pipeline, builds a stub leaf node (catalogued vtable,
// shadow-copied so its evaluate slots record dispatch), grafts it as the
// only child of a BORROWED real trigger-block head (a byte-copy of one of
// the sentinel interaction's embedded 0xE0 heads — the objects the proven
// process_trigger_query path hands to the evaluator; carries the compiled
// scope-typing instance state a synthetic root lacks — D2 cure, ck-ay9o),
// and calls FUN_07628500 on the root. Falls back to the round-3 synthetic
// CAndTrigger root (typed scope_rejected expected) if no head passes the
// sanity gate.
void process_named_trigger_query();

// Catalogue access (mostly for diagnostics / future query handlers).
// Returns nullptr if the name is unknown. Lookup tries both the raw
// `class_name` (mangled) and a script-keyword alias table.
const TriggerCatalogueEntry* lookup_trigger_entry(const char* name);

// Convert a script keyword (e.g. "is_alive") to a canonical mangled class
// name from the catalogue. Returns nullptr if no mapping exists. Internal
// rules: an explicit alias table for the smoke-test keywords, plus a
// mechanical strip-C/strip-Trigger/CamelCase→snake fallback when the alias
// table misses. Cross-validated against the catalogue at load time.
const char* resolve_keyword_to_class(const char* keyword);

// Stats / introspection — useful for the discover() surface and tests.
size_t trigger_catalogue_size();
bool trigger_catalogue_loaded();

} // namespace bridge
