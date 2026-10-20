// windows/dll_main.cpp — the entry point of ck3_bridge_win, the Windows bridge
// DLL (built as version.dll; the forwards are in proxy.cpp). Compiled ONLY
// into the DLL target, never into ck3_os or a test exe.
//
// DllMain runs under the loader lock, so it does almost nothing (ADR
// "Platform abstraction", os::entry row): it records the module
// (os::dll_attach_record, proxy::set_self) and starts ONE init thread, which
// the loader lets run only after it releases the lock. No LoadLibrary, no
// file or socket work, no allocation in DllMain.
//
// The init thread is ADR phase P4, and P4 only:
//   1. resolve the forwards to the real system version.dll (proxy.cpp);
//   2. open the log (os::paths bridge_log; %TEMP% if the paths refuse);
//   3. is the host ck3.exe? (os::process_identity). Not the game: inert,
//      forwarding only;
//   4. the identity gate (os::binary_identity, whose check field is the
//      identity_gate verdict against pinned_identity.h), logged with both
//      identities;
//   5. match: log "match" and start NOTHING else (no hooks, no IPC listener:
//      the ADR's P4 exit is "match on 1.18.3.1, REFUSED and no listener on
//      any other build"). Mismatch / not found: log REFUSED with both
//      identities, start nothing; the forwards keep working and the game
//      keeps running;
//   6. evidence only: at kCallReportAtMs (1.5 s and 90 s), log how many calls went
//      through each forward, then return. A SIGKILLed game never detaches, so
//      this line, not a detach record, is the witness that the forwards were
//      used.
// P5 (CK3_WIN_BRIDGE_CORE builds): on match, step 5 starts the bridge core in
// observe-only mode (bridge_init: fault net, offsets check, IPC listener; no
// hooks — those are P6). Builds without the core start nothing, as in P4.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "os/os.h"

#ifdef CK3_WIN_BRIDGE_CORE
extern "C" void ck3_bridge_start(void);   // bridge_init.cpp (Windows)
namespace bridge { void abandon_ipc_threads(); }   // ipc_server.cpp
#endif

#include "identity_gate.h"
#include "pinned_identity.h"
#include "proxy.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace bridge {
namespace os {
void dll_attach_record(void* module);  // windows/entry.cpp
void dll_init_record(EntryPhase phase, std::uint64_t tid, bool is_game, const char* outcome);
}  // namespace os
}  // namespace bridge

namespace {

namespace os = bridge::os;

// When the init thread logs the per-forward call counts (ms after it starts):
// early, then once the game has been up a while. No environment override: a
// knob the bridge reads would be product surface (the env table) for a test.
constexpr DWORD kCallReportAtMs[] = {1500, 90000};

// Set in DllMain: plain scalars only.
HMODULE   g_self = nullptr;
ULONGLONG g_attach_ms = 0;
DWORD     g_attach_tid = 0;

wchar_t g_log[1024];  // set once by the init thread; empty = no log

void log_line(const char* fmt, ...) {
    if (!g_log[0]) return;
    char buf[3072];
    SYSTEMTIME t;
    GetSystemTime(&t);
    int n = std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ +%llums [ck3_bridge_win] ",
                          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                          static_cast<unsigned long long>(GetTickCount64() - g_attach_ms));
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(buf + n, sizeof(buf) - std::size_t(n) - 2, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    std::size_t len = std::strlen(buf);
    buf[len++] = '\n';
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, buf, DWORD(len), &w, nullptr);
    CloseHandle(h);
}

void narrow(const wchar_t* in, char* out, int cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, nullptr, nullptr) <= 0) std::strncpy(out, "?", cap);
}

// The log path: os::paths()'s bridge_log, else %TEMP%\ck3_bridge_win.log.
const char* open_log(os::Paths* p) {
    const os::Result r = os::paths(p);
    if (os::succeeded(r) && p->bridge_log && p->bridge_log[0] &&
        MultiByteToWideChar(CP_UTF8, 0, p->bridge_log, -1, g_log, 1024) > 0)
        return "os::paths bridge_log";
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n + 20 < MAX_PATH) {
        lstrcatW(tmp, L"ck3_bridge_win.log");
        lstrcpyW(g_log, tmp);
        return "%TEMP% (os::paths refused)";
    }
    g_log[0] = L'\0';
    return "none";
}

DWORD WINAPI init_thread(LPVOID) {
    const std::uint64_t tid = GetCurrentThreadId();
    os::dll_init_record(os::EntryPhase::init_running, tid, false, "init thread running");
    os::set_thread_name("ck3bridge-init");

    // 1. forwards first, so a game call never waits on anything below.
    os::proxy::Resolution res;
    std::memset(&res, 0, sizeof(res));
    const bool published = os::proxy::resolve("init_thread", &res);

    // 2. the log.
    os::Paths paths;
    std::memset(&paths, 0, sizeof(paths));
    const char* log_from = open_log(&paths);

    wchar_t wself[MAX_PATH];
    char self[MAX_PATH * 3];
    GetModuleFileNameW(g_self, wself, MAX_PATH);
    narrow(wself, self, sizeof(self));
    log_line("attach pid=%lu dllmain_tid=%lu init_tid=%llu self=%p (%s) log=%s",
             GetCurrentProcessId(), g_attach_tid, static_cast<unsigned long long>(tid),
             static_cast<void*>(g_self), self, log_from);
    if (!published) {
        log_line("forward ERROR: the resolution was not published within the bounded wait");
    } else {
        log_line("forward real=%s module=%p err=%lu resolved=%d/%d missing=[%s] first=%s%s%s",
                 res.real_path, reinterpret_cast<void*>(res.real_module), res.error, res.resolved,
                 res.total, res.missing, res.trigger, res.refused[0] ? " REFUSED: " : "", res.refused);
    }

    // 3. the host.
    os::ProcessIdentity pid;
    std::memset(&pid, 0, sizeof(pid));
    const os::Result pr = os::process_identity(&pid);
    log_line("host exe=%s is_game=%d%s%s", pid.exe_path[0] ? pid.exe_path : "?", pid.is_game ? 1 : 0,
             os::succeeded(pr) ? "" : " error=", os::succeeded(pr) ? "" : pr.detail);
    if (!pid.is_game) {
        log_line("host is not ck3.exe: inert (forwarding only, no identity check)");
        os::dll_init_record(os::EntryPhase::init_done, tid, false,
                            "not the game: forwarding only, nothing started");
    } else {
        // 4. the identity gate.
        const ULONGLONG t0 = GetTickCount64();
        os::BinaryIdentity id;
        std::memset(&id, 0, sizeof(id));
        const os::Result ir = os::binary_identity(&id);
        const ULONGLONG t1 = GetTickCount64();
        const os::PinnedIdentity& pin = os::pinned_identity();
        const os::GateOutcome g = os::identity_gate(id, pin);
        log_line("identity observed format=%s key=%s size_of_image=%llu sha256=%s (%llums)%s%s",
                 os::identity_format_string(id.format), id.observed_hex[0] ? id.observed_hex : "-",
                 static_cast<unsigned long long>(id.image_size), id.image_sha256[0] ? id.image_sha256 : "-",
                 static_cast<unsigned long long>(t1 - t0), os::succeeded(ir) ? "" : " error=",
                 os::succeeded(ir) ? "" : ir.detail);
        log_line("identity pinned   format=pe_codeview_rsds key=%s size_of_image=%llu sha256=%s (%s)",
                 pin.codeview_hex, static_cast<unsigned long long>(pin.size_of_image), pin.image_sha256,
                 os::pinned::kLabel);
        const bool match = std::strcmp(g.check, "match") == 0;
        // 5. the verdict. One check: id.check (the cell) and g.check agree by
        // construction; a disagreement is logged rather than resolved.
        if (std::strcmp(id.check, g.check) != 0)
            log_line("identity INTERNAL: cell check=%s gate check=%s", id.check, g.check);
        if (match) {
#ifdef CK3_WIN_BRIDGE_CORE
            log_line("identity match key=%s sha256_witness=%s: starting the bridge core (observe-only, P5)",
                     g.key, g.sha_witness);
            ck3_bridge_start();
            log_line("bridge core start returned (see the bridge log for its outcome)");
            os::dll_init_record(os::EntryPhase::init_done, tid, true,
                                "identity match: bridge core started (observe-only, P5)");
#else
            log_line("identity match key=%s sha256_witness=%s: nothing started (ADR P4: no hooks, no IPC listener)",
                     g.key, g.sha_witness);
            os::dll_init_record(os::EntryPhase::init_done, tid, true,
                                "identity match: nothing started yet (P4)");
#endif
        } else {
            log_line("identity REFUSED check=%s key=%s why=%s sha256_witness=%s: nothing started, "
                     "forwarding only; the game keeps running",
                     g.check, g.key, g.why, g.sha_witness);
            os::dll_init_record(os::EntryPhase::init_done, tid, true,
                                "identity REFUSED: forwarding only, nothing started");
        }
    }

    // 6. evidence: the forwards in use.
    DWORD slept = 0;
    for (DWORD at : kCallReportAtMs) {
        Sleep(at - slept);
        slept = at;
        char calls[1024] = "";
        for (int i = 0; i < os::proxy::export_count(); ++i) {
            const long c = os::proxy::call_count(i);
            if (!c) continue;
            char one[96];
            std::snprintf(one, sizeof(one), " %s=%ld", os::proxy::export_name(i), c);
            if (std::strlen(calls) + std::strlen(one) < sizeof(calls)) std::strcat(calls, one);
        }
        log_line("calls after %lums:%s", at, calls[0] ? calls : " none");
    }
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        g_attach_ms = GetTickCount64();
        g_attach_tid = GetCurrentThreadId();
        os::dll_attach_record(inst);
        os::proxy::set_self(inst);
        // The one thing DllMain starts. It runs after the loader lock is
        // released. If it cannot be created, the forwards still resolve on
        // first call; nothing else runs and nothing is logged.
        HANDLE t = CreateThread(nullptr, 0, &init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
#ifdef CK3_WIN_BRIDGE_CORE
    else if (reason == DLL_PROCESS_DETACH) {
        // Before the CRT destroys this DLL's statics (it does so after DllMain
        // returns): a joinable std::thread there is std::terminate -> abort.
        bridge::abandon_ipc_threads();
    }
#endif
    return TRUE;
}
