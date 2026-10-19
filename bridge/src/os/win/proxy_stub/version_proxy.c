/*
 * Stub proxy version.dll (ADR 0001, phase P4 load check; runbook step 4).
 *
 * Not the bridge. It proves one thing: that ck3.exe, started under Proton with
 * WINEDLLOVERRIDES=version=n,b, loads an application-directory version.dll
 * (native) and that every call it makes still reaches the real system DLL.
 *
 *  - Every named export of the real version.dll (version_exports.h, generated
 *    from that DLL's export table by gen_exports.py) is a 2-instruction thunk:
 *    count the call, then jump through a pointer resolved on attach.
 *  - On DLL_PROCESS_ATTACH it loads <GetSystemDirectoryW>\version.dll by full
 *    path, resolves every export with GetProcAddress, and appends one record
 *    to C:\ck3_proxy_stub.log (under Proton: $PFX/pfx/drive_c/). An export
 *    that does not resolve keeps a fallback that fails the call
 *    (ERROR_PROC_NOT_FOUND, returns 0) and is named in the log; nothing is
 *    defaulted to "worked".
 *  - If the full-path load hands back this module itself (a loader that
 *    matched on the base name), the stub refuses to forward and says so in
 *    the log, instead of recursing into itself.
 *  - DLL_PROCESS_DETACH appends the per-export call counts. A SIGKILLed game
 *    never detaches, so a missing detach record means "not observed", not
 *    "not called".
 *
 * LoadLibraryW inside DllMain runs under the loader lock. It is tolerated here
 * because the target is a system DLL with no DllMain dependencies on us; the
 * real bridge must not do this (ADR: loader lock follows Wine's loader under
 * Proton, not Windows').
 *
 * Build: build.sh (x86_64-w64-mingw32-gcc or llvm-mingw clang).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH L"C:\\ck3_proxy_stub.log"

static DWORD WINAPI fallback_fail(void) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

/* Pointer table and call counters, one per export. Global (not static) so the
 * top-level asm thunks can name them. */
#define X(name, ord) void *real_##name = (void *)fallback_fail; volatile LONG calls_##name;
#include "version_exports.h"
#undef X

/* Thunks: exported under the real names via version_proxy.def. Registers and
 * stack are untouched; only flags are clobbered, which no caller relies on
 * across a call. */
#define X(name, ord)                                   \
    __asm__(".text\n"                                  \
            ".globl proxy_" #name "\n"                 \
            ".p2align 4\n"                             \
            "proxy_" #name ":\n"                       \
            "  lock incl calls_" #name "(%rip)\n"      \
            "  jmp *real_" #name "(%rip)\n");
#include "version_exports.h"
#undef X

static HMODULE g_self;
static HMODULE g_real;

static void log_line(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    HANDLE h = CreateFileW(LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

static void narrow(const wchar_t *in, char *out, int cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, NULL, NULL) <= 0) {
        strncpy(out, "?", (size_t)cap);
    }
}

static void on_attach(void) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    wchar_t wself[MAX_PATH], wexe[MAX_PATH], wsys[MAX_PATH];
    char self[MAX_PATH * 3], exe[MAX_PATH * 3], sys[MAX_PATH * 3];
    GetModuleFileNameW(g_self, wself, MAX_PATH);
    GetModuleFileNameW(NULL, wexe, MAX_PATH);
    UINT n = GetSystemDirectoryW(wsys, MAX_PATH);
    if (n == 0 || n + 13 >= MAX_PATH) {
        log_line("attach pid=%lu ERROR GetSystemDirectoryW n=%u; not forwarding",
                 GetCurrentProcessId(), n);
        return;
    }
    lstrcatW(wsys, L"\\version.dll");
    narrow(wself, self, sizeof self);
    narrow(wexe, exe, sizeof exe);
    narrow(wsys, sys, sizeof sys);

    g_real = LoadLibraryW(wsys);
    DWORD err = g_real ? 0 : GetLastError();
    int total = 0, ok = 0;
    char missing[1024] = "";
    if (g_real && g_real == g_self) {
        log_line("attach pid=%lu REFUSED: LoadLibraryW(%s) returned this module %p; not forwarding",
                 GetCurrentProcessId(), sys, (void *)g_self);
        g_real = NULL;
        return;
    }
#define X(name, ord)                                                       \
    do {                                                                   \
        total++;                                                           \
        FARPROC p = g_real ? GetProcAddress(g_real, #name) : NULL;         \
        if (p) { real_##name = (void *)p; ok++; }                          \
        else if (strlen(missing) + sizeof(#name) + 2 < sizeof missing) {   \
            strcat(missing, " " #name);                                    \
        }                                                                  \
    } while (0);
#include "version_exports.h"
#undef X
    log_line("attach %04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu tid=%lu self=%p (%s) exe=%s "
             "real=%p (%s) err=%lu resolved=%d/%d missing=[%s]",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             GetCurrentProcessId(), GetCurrentThreadId(), (void *)g_self, self, exe,
             (void *)g_real, sys, err, ok, total, missing);
}

static void on_detach(void) {
    char buf[1536] = "";
#define X(name, ord)                                                         \
    if (calls_##name) {                                                      \
        char one[96];                                                        \
        snprintf(one, sizeof one, " %s=%ld", #name, (long)calls_##name);     \
        if (strlen(buf) + strlen(one) < sizeof buf) strcat(buf, one);        \
    }
#include "version_exports.h"
#undef X
    log_line("detach pid=%lu calls:%s", GetCurrentProcessId(), buf[0] ? buf : " none");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        on_attach();
    } else if (reason == DLL_PROCESS_DETACH) {
        on_detach();
    }
    return TRUE;
}
