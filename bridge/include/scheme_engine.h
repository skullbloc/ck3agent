#pragma once
// The scheme reads and the engine's own start check, in one place (l3-schemes,
// re/findings/re_schemes.md). Every handler that reports a scheme -- the owned list,
// the incoming list, the push observation -- and every scheme write -- start, cancel,
// check -- goes through these, so a field means the same thing wherever it appears.

#include <cstdint>
#include <string>

namespace bridge {

class ResponseBuilder;
struct Intent;

// The scheme executors report their verdict as a JSON object in
// Intent::console_output; the handlers splice it into the reply only when it is one.
inline bool is_json_object(const std::string& s) { return !s.empty() && s[0] == 0x7B; }

// CSchemeType* for a script key from the engine's scheme type database, or 0.
uintptr_t scheme_type_by_name(const std::string& key);

// Why the bridge does not start this type, or nullptr: "scheme_type_needs_
// contract_or_artifact" for a contract scheme (scheme_type::CATEGORY 1: the game
// creates those from a task contract the ruler accepted) and for a type the engine
// has only ever started with a real contract or artifact this process.
const char* scheme_bridge_refusal(uintptr_t type_ptr, const std::string& key);

// "scheme_type_targets_a_title" when the type's target is not a character
// (scheme_type::TARGET_TYPE != 0: a title, culture, faith or nothing), else
// nullptr. The bridge starts and checks character-target schemes only; the start
// (execute_scheme_intent) and the check (execute_scheme_check_intent) refuse
// through this one reader, so do(scheme start) and discover(scheme_targets) name
// the same door (L18 round 2: generate_claim's start was refused
// engine_refused_can_start_scheme, its check scheme_type_targets_a_title).
const char* scheme_target_refusal(uintptr_t type_ptr);

// The engine's scheme_success_chance (funcs::SCHEME_GET_SUCCESS_CHANCE), in percent
// (its int64 fixed-point / 100000). False when the call could not be made or
// faulted; every emitter then writes success_chance: null.
bool scheme_success_chance(uintptr_t scheme_ptr, double* out_pct);

// The fields every scheme row carries: type, category, secrecy, success, progress,
// breaches, exposure, agents (with names and contributions). The caller emits the
// ids that differ between the owned and the incoming view.
void scheme_emit_state(ResponseBuilder& r, uintptr_t scheme_ptr);

// The script name of a category value (scheme_type::CATEGORY), "" when unknown.
const char* scheme_category_name(int32_t category);

// Game thread (the intent drain): run CanStartScheme for one owner and a target or a
// scan of targets; Intent::console_output gets the JSON verdict.
bool execute_scheme_check_intent(Intent* intent);

// Game thread: the engine's start check for one owner/type/target. Returns 1 when the
// engine allows it, 0 when it refuses (reasons_json then holds a JSON array of the
// engine's reasons), -1 when the check could not run (reasons_json holds the cause).
int scheme_can_start(uintptr_t type_ptr, void* owner, void* target, bool want_reasons,
                     std::string* reasons_json);

}  // namespace bridge
