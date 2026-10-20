// test_os_win_proxy_load.cpp — the built ck3_bridge_win DLL (version.dll),
// loaded the way a host loads it, engine-free, under wine.
//
// Built twice by CMake from this one source, each beside its own copy of the
// DLL (named version.dll):
//   test_os_win_proxy_load            argv[1] = "inert":   the host is not
//                                     ck3.exe, so the DLL forwards and starts
//                                     nothing, with no identity check
//   proxyhost/ck3.exe                 argv[1] = "refused": the host IS named
//                                     ck3.exe but is not the pinned build, so
//                                     the gate must log REFUSED
// (The `match` branch needs the real 1.18.3.1 ck3.exe; the gate's match logic
// is covered on synthetic identities by test_os_win_identity_gate, and the
// real match by the Proton run in re/findings/windows/re_p4_proxy_gate.md.)
//
// Witnesses that share no code with the DLL: the real system version.dll,
// loaded here by full path and called directly, and the DLL's log read back as
// text. Under wine, CTest sets WINEDLLOVERRIDES=version=n,b (as the Proton
// recipe does), or the loader would hand back the builtin for the base name.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

static const char* const kNames[] = {
#define X(name, ord) #name,
#include "../windows/version_exports.h"
#include "../windows/version_exports_extra.h"
#undef X
};

static std::string read_file(const wchar_t* path) {
    std::string s;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) s.append(buf, n);
    CloseHandle(h);
    return s;
}

using SizeW = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
using InfoW = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
using QueryW = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

// dwFileVersionMS of `file` through the three functions of one module.
static DWORD file_version_ms(HMODULE m, const wchar_t* file) {
    auto sz = reinterpret_cast<SizeW>(GetProcAddress(m, "GetFileVersionInfoSizeW"));
    auto info = reinterpret_cast<InfoW>(GetProcAddress(m, "GetFileVersionInfoW"));
    auto query = reinterpret_cast<QueryW>(GetProcAddress(m, "VerQueryValueW"));
    if (!sz || !info || !query) return 0;
    DWORD h = 0;
    const DWORD n = sz(file, &h);
    if (!n) return 0;
    std::string blob(n, '\0');
    if (!info(file, 0, n, &blob[0])) return 0;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!query(blob.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || !ffi || len < sizeof(*ffi)) return 0;
    return ffi->dwFileVersionMS ? ffi->dwFileVersionMS : 1;  // 0 means "failed" here
}

int main(int argc, char** argv) {
    const bool want_refused = argc > 1 && std::strcmp(argv[1], "refused") == 0;
    const bool want_inert = argc > 1 && std::strcmp(argv[1], "inert") == 0;
    check(want_refused || want_inert, "argv[1] is inert or refused");

    wchar_t tmp[MAX_PATH], logw[MAX_PATH], dir[MAX_PATH], dll[MAX_PATH], sys[MAX_PATH], k32[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(logw, MAX_PATH, L"%sck3_bridge_win_test_%lu.log", tmp, GetCurrentProcessId());
    DeleteFileW(logw);
    SetEnvironmentVariableW(L"CK3_BRIDGE_LOG", logw);

    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = L'\0';
    swprintf(dll, MAX_PATH, L"%s\\version.dll", dir);
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(k32, MAX_PATH, L"%s\\kernel32.dll", sys);
    lstrcatW(sys, L"\\version.dll");

    const ULONGLONG t0 = GetTickCount64();
    HMODULE ours = LoadLibraryW(dll);
    check(ours != nullptr, "LoadLibraryW(<exe dir>\\version.dll): the proxy loads");
    if (!ours) {
        std::printf("GetLastError=%lu\n", GetLastError());
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    wchar_t got[MAX_PATH];
    GetModuleFileNameW(ours, got, MAX_PATH);
    check(lstrcmpiW(got, dll) == 0, "the module loaded is the file beside the exe (native), not the builtin");

    HMODULE real = LoadLibraryW(sys);
    check(real != nullptr && real != ours, "the real system version.dll is a different module");

    int exported = 0;
    for (const char* n : kNames)
        if (GetProcAddress(ours, n)) ++exported;
    check(exported == int(sizeof(kNames) / sizeof(kNames[0])),
          "every listed export (generated 16 + extra) is exported by the proxy");
    check(GetProcAddress(ours, "ck3proxy_GetFileVersionInfoA") == nullptr,
          "the internal wrapper names are not exported");

    const DWORD v_real = file_version_ms(real, k32);
    const DWORD v_ours = file_version_ms(ours, k32);
    check(v_real != 0, "witness: the real version.dll reads kernel32.dll's version");
    check(v_ours == v_real, "forward: GetFileVersionInfoSizeW/GetFileVersionInfoW/VerQueryValueW through the "
                            "proxy give the real DLL's answer");

    std::string log;
    for (int i = 0; i < 200; ++i) {
        log = read_file(logw);
        if (log.find("calls after 1500ms") != std::string::npos) break;
        Sleep(100);
    }
    std::printf("---- DLL log (%llu ms after load) ----\n%s----\n",
                static_cast<unsigned long long>(GetTickCount64() - t0), log.c_str());
    auto has = [&](const char* s) { return log.find(s) != std::string::npos; };
    check(has("[ck3_bridge_win] attach pid="), "log: attach line (the init thread ran)");
    check(has("forward real=") && has("first=") && !has("REFUSED: "),
          "log: the forwards resolved against the real DLL");
    check(has("resolved=16/17") || has("resolved=17/17"),
          "log: 16 of 17 resolve (Wine's builtin lacks GetFileVersionInfoByHandle) or 17 of 17 (Windows)");
    check(!has("identity match"), "log: never `identity match` in a test exe");
    if (want_inert) {
        check(has("is_game=0") && has("inert (forwarding only"), "inert: not ck3.exe, nothing checked or started");
        check(!has("identity observed"), "inert: no identity check ran");
    } else {
        check(has("is_game=1"), "refused: the host is named ck3.exe");
        check(has("identity observed") && has("identity pinned"), "refused: both identities are logged");
        check(has("identity REFUSED check=mismatch") && has("nothing started"),
              "refused: REFUSED, nothing started");
        check(!has("identity INTERNAL"), "refused: the cell's check and the gate agree");
    }
    check(has("calls after") && has("GetFileVersionInfoSizeW=1") && has("VerQueryValueW=1"),
          "log: the call counters saw the forwards this test made");

    DeleteFileW(logw);
    // No FreeLibrary: the DLL is never unloaded in a host either.
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
