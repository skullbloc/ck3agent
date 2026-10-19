#pragma once
// os/win_paths.h — the Windows instance-path rules, portable C++17.
//
// The FOURTH transcription of the instance naming rules (authority:
// ck3agent/server/instance.py; then scripts/ck3_instance.sh and
// bridge/src/instance_paths.cpp). Kept portable so it runs on every OS:
// test_os_win_paths_rules checks it on Linux, windows-latest and macos-14,
// and on Linux test_os_win_paths_pin drives the same CK3_INSTANCE_ID values
// through instance_paths.cpp and requires the same accept/refuse verdict.
//
// Windows layout (ADR 0001 "os::paths"): %LOCALAPPDATA%\ck3agent, else
// %USERPROFILE%\AppData\Local\ck3agent. Named instances live under
// <base>\inst\<id>\ (CK3_INSTANCE_ROOT overrides <base>\inst). Same members as
// Linux: agent.sock, bridge.log, telemetry, log-archive, and the same
// override precedence (explicit per-path variable > instance id > layout
// default). CK3_INSTANCE_LAYOUT=legacy is a config refusal here: its /tmp
// literals have no Windows meaning.

#include <string>

namespace bridge {
namespace os {

// Returns the variable's value, or nullptr when unset. The caller's storage.
using EnvLookup = const char* (*)(const char* key, void* user);

struct WinPaths {
    std::string base;             // <...>\ck3agent; empty when no base could be formed
    std::string instance_id;      // "default" or the validated slug
    std::string socket;
    std::string bridge_log;
    std::string telemetry_dir;
    std::string log_archive_dir;  // "" unless configured or a named instance
    std::string state_dir;
    bool        id_refused;       // CK3_INSTANCE_ID was not a slug (fell back to default)
    std::string config_error;     // "" when the configuration was accepted
};

// The slug rule shared with instance.py: ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$
bool instance_id_is_slug(const char* s);

// Resolve from the environment. Never throws; a refusal is reported in
// config_error with the paths still resolved as user/default (for a banner),
// exactly as instance_paths.cpp does.
WinPaths resolve_win_paths(EnvLookup lookup, void* user);

}  // namespace os
}  // namespace bridge
