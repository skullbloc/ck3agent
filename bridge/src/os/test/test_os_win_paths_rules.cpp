// test_os_win_paths.cpp — the portable Windows instance-path rules
// (os/win_paths.h), over a table of fake environments. Engine-free; runs on
// Linux, windows-latest and macos-14.
//
// The slug rule and the refusal cases are the same ones instance_paths.cpp
// and instance.py enforce; test_os_win_paths_pin (Linux) drives the same ids
// through instance_paths.cpp and requires the same accept/refuse verdict, so
// the fourth transcription cannot drift from the third unnoticed.

#include "os/win_paths.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

using Env = std::map<std::string, std::string>;
static const char* env_lookup(const char* key, void* user) {
    auto& e = *static_cast<Env*>(user);
    auto it = e.find(key);
    return (it == e.end() || it->second.empty()) ? nullptr : it->second.c_str();
}
static os::WinPaths resolve(Env e) { return os::resolve_win_paths(&env_lookup, &e); }

int main() {
    check(os::instance_id_is_slug("default") && os::instance_id_is_slug("e2.arm-3") &&
              os::instance_id_is_slug("A"),
          "slug rule accepts the valid ids");
    check(!os::instance_id_is_slug("") && !os::instance_id_is_slug(".dot") &&
              !os::instance_id_is_slug("has space") && !os::instance_id_is_slug("a/b") &&
              !os::instance_id_is_slug(std::string(49, 'a').c_str()),
          "slug rule rejects empty, leading punct, space, slash, over-48");

    {
        os::WinPaths p = resolve({{"LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local"}});
        check(p.config_error.empty() && !p.id_refused, "default/user: accepted");
        check(p.instance_id == "default" && p.base == "C:\\Users\\u\\AppData\\Local\\ck3agent",
              "default: base under %LOCALAPPDATA%\\ck3agent");
        check(p.socket == "C:\\Users\\u\\AppData\\Local\\ck3agent\\agent.sock" &&
                  p.bridge_log.substr(p.bridge_log.size() - 10) == "bridge.log" &&
                  p.state_dir == p.base && p.log_archive_dir.empty(),
              "default: members directly under base, no archive");
    }
    {
        os::WinPaths p = resolve({{"LOCALAPPDATA", "C:\\LA"}, {"CK3_INSTANCE_ID", "arm3"}});
        check(p.instance_id == "arm3" &&
                  p.socket == "C:\\LA\\ck3agent\\inst\\arm3\\agent.sock" &&
                  p.state_dir == "C:\\LA\\ck3agent\\inst\\arm3" &&
                  p.log_archive_dir == "C:\\LA\\ck3agent\\inst\\arm3\\log-archive",
              "named instance: under <base>\\inst\\<id>, with a log-archive");
    }
    {
        os::WinPaths p = resolve({{"USERPROFILE", "C:\\Users\\u"}});
        check(p.base == "C:\\Users\\u\\AppData\\Local\\ck3agent",
              "no LOCALAPPDATA: base derived from %USERPROFILE%");
    }
    {
        os::WinPaths p = resolve({{"LOCALAPPDATA", "C:\\LA"},
                                  {"CK3_BRIDGE_SOCKET", "D:\\explicit.sock"},
                                  {"CK3_INSTANCE_ID", "arm3"}});
        check(p.socket == "D:\\explicit.sock",
              "an explicit per-path override wins over the instance id");
    }
    {
        os::WinPaths p = resolve({{"LOCALAPPDATA", "C:\\LA"}, {"CK3_INSTANCE_ID", "bad id"}});
        check(p.id_refused && !p.config_error.empty() && p.instance_id == "default",
              "a non-slug id is refused, falls back to default, reports the error");
    }
    {
        os::WinPaths p = resolve({{"LOCALAPPDATA", "C:\\LA"}, {"CK3_INSTANCE_LAYOUT", "legacy"}});
        check(!p.config_error.empty() && p.config_error.find("legacy") != std::string::npos,
              "CK3_INSTANCE_LAYOUT=legacy: a config refusal (no /tmp on Windows)");
    }
    {
        os::WinPaths p = resolve({{"LOCALAPPDATA", "C:\\LA"}, {"CK3_INSTANCE_LAYOUT", "user"}});
        check(p.config_error.empty(), "CK3_INSTANCE_LAYOUT=user: accepted");
    }
    {
        os::WinPaths p = resolve({});
        check(!p.config_error.empty(), "no base dir at all: a config refusal");
    }
    {
        os::WinPaths p = resolve({{"LOCALAPPDATA", "C:\\LA"},
                                  {"CK3_INSTANCE_ROOT", "E:\\runs"},
                                  {"CK3_INSTANCE_ID", "arm3"}});
        check(p.socket == "E:\\runs\\arm3\\agent.sock",
              "CK3_INSTANCE_ROOT overrides <base>\\inst for a named instance");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
