// script_block_parser.h — minimal PDX-script effect-block parser.
//
// Recognises the structural shape `<ident> = { <key>=<value> ... }` used by
// CK3's scripted_effect and effect blocks. Values may be identifiers
// (incl. `prefix:suffix` references like `title:d_guelders`), integers,
// quoted strings, or nested {…} blocks (captured as raw text for sub-parsing
// by the caller).
//
// This is NOT a full PDX-script parser. It exists so the bridge's
// `execute_script_block` handler can route well-known outer effects
// (currently: `start_war`) to existing direct-call handlers without going
// through the unfinished script-compiler entry-point quest (ck-zvu5l.1).
//
// Lifecycle context: standalone, no game-object reads — safe to call on the
// IPC thread. Pure string in → struct out.

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace bridge {

struct ScriptBlock {
    std::string effect_name;
    std::unordered_map<std::string, std::string> params;
    bool valid = false;
    std::string error;
    std::size_t error_offset = 0;
};

ScriptBlock parse_script_block(const char* text);

std::string strip_ref_prefix(const std::string& s, const char* prefix);

bool parse_int_strict(const std::string& s, int* out);

}  // namespace bridge
