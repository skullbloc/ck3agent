// ck-laf5: per-CB outer-filter classification table.
//
// Maps the 83 AI-eligible CB names to one of 8 outer-rejection class tags
// derived from the offline PDXScript classification in
// re/findings/re_a1_67cb_rejection_classification_2026-04-25.md (ck-iyhl).
//
// The table is consumed by process_cb_query (bridge/src/ai_eval_hook.cpp)
// to populate `outer_reason` when the per-CB allowed_for_character trigger
// fails. The pass/fail verdict itself comes from the engine's
// SCRIPT_TRIGGER_EVALUATOR — this table only provides a structured reason
// string for diagnostic surface (cycle-2-38 telemetry classification).
//
// Regenerate via:
//   python3 re/specs/casus-belli/gen_outer_filter_table.py \
//       > bridge/include/cb_outer_filter_table.inc
#pragma once

#include <cstring>
#include <cstddef>

struct CbOuterFilterEntry {
    const char* cb_name;
    const char* outer_reason;
};

#include "cb_outer_filter_table.inc"

// Returns the outer_reason tag for a CB name, or nullptr if the CB is not in
// the AI-eligible classification table (e.g. script-only / debug groups, or
// CBs added after the table was generated). Linear scan; the table is small
// (83 entries) and called once per CB per query.
inline const char* cb_outer_filter_lookup(const char* cb_name) {
    if (!cb_name || !cb_name[0]) return nullptr;
    for (size_t i = 0; i < kCbOuterFilterTableSize; ++i) {
        if (strcmp(cb_name, kCbOuterFilterTable[i].cb_name) == 0) {
            return kCbOuterFilterTable[i].outer_reason;
        }
    }
    return nullptr;
}
