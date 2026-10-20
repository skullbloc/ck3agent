// test_os_win_posix_recovery.cpp — the Windows POSIX shim's fault recovery
// (ADR 0001 P5): sigsetjmp(g_recovery_point) + a protected region + a fault in a
// callee must come back through siglongjmp to the second return of sigsetjmp, with
// the caller's frame intact. This is the bridge's inline-query shape
// (ipc_server.cpp): a std::string reply declared before sigsetjmp, written by the
// handler through a pointer, assigned an error after recovery.
// And the dispatch watchdog's interrupt (P6, ck3_win_watchdog_interrupt): a thread
// spinning in a protected region, in this exe's code, is redirected to the second
// return of sigsetjmp with 2 (the Linux SIGALRM value); one waiting inside a system
// DLL, or one outside the region, is left alone.
#include <setjmp.h>   // the shim's sigsetjmp / siglongjmp
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bridge {
thread_local sigjmp_buf g_recovery_point;
thread_local bool g_in_protected_region = false;
}
using bridge::g_in_protected_region;
using bridge::g_recovery_point;

static int g_fail = 0;
#define CHECK(c, msg) do { if (c) std::printf("ok:   %s\n", msg); else { std::printf("FAIL: %s\n", msg); ++g_fail; } } while (0)

// A handler that builds its reply in pieces, then reads through a poisoned
// address (base + CK3_WIN_UNMAPPED), as a query does through an unmapped RVA.
__declspec(noinline) static void handler(std::string* out, std::uintptr_t poison) {
    std::string local(200, 'x');            // heap-allocated: a callee resource abandoned on fault
    *out = "{\"partial\":";                   // the reply is mid-build when the fault comes
    out->append(local);
    volatile std::uint64_t v = *reinterpret_cast<volatile std::uint64_t*>(poison);
    *out += std::to_string(v);
}

__declspec(noinline) static int one_round(std::uintptr_t poison, std::string* seen) {
    std::string response_out;
    volatile int recovered = 0;
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        handler(&response_out, poison);
    } else {
        recovered = 1;
        response_out = "{\"error\":\"memory_access_fault\"}";
    }
    g_in_protected_region = false;
    *seen = response_out;
    return recovered;
}

// ── watchdog interrupt ─────────────────────────────────────────────────────
static std::atomic<int> g_ready{0}, g_stop{0}, g_result{-1};
static std::atomic<const bool*> g_region{nullptr};
static volatile unsigned long long g_spins = 0;

__declspec(noinline) static DWORD WINAPI spinner(LPVOID) {
    g_region.store(&g_in_protected_region);
    std::string reply = "{";
    volatile int got = 0;
    g_in_protected_region = true;
    const int sj = sigsetjmp(g_recovery_point, 1);
    if (sj == 0) {
        g_ready.store(1);
        for (;;) ++g_spins;            // this exe's code, never leaves
    } else {
        got = sj;
        reply += "\"error\":\"timeout\"}";
    }
    g_in_protected_region = false;
    g_result.store(reply == "{\"error\":\"timeout\"}" ? got : -2);
    return 0;
}

__declspec(noinline) static DWORD WINAPI sleeper(LPVOID) {
    g_region.store(&g_in_protected_region);
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        g_ready.store(1);
        while (!g_stop.load()) Sleep(5);   // inside kernelbase/ntdll most of the time
        g_in_protected_region = false;
        g_ready.store(2);
        while (g_stop.load() < 2) Sleep(5);   // alive, outside the region
        g_result.store(0);
    } else {
        g_in_protected_region = false;
        g_result.store(99);                // must not happen
    }
    return 0;
}

static void image_range(std::uintptr_t* lo, std::uintptr_t* hi) {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    *lo = base;
    *hi = base + nt->OptionalHeader.SizeOfImage;
}

static void watchdog_cases() {
    std::uintptr_t lo, hi, rip = 0;
    image_range(&lo, &hi);

    g_ready = 0; g_result = -1;
    DWORD tid = 0;
    HANDLE t = CreateThread(nullptr, 0, spinner, nullptr, 0, &tid);
    while (!g_ready.load()) Sleep(1);
    Sleep(20);
    const int rc = ck3_win_watchdog_interrupt(tid, g_region.load(), &lo, &hi, 1, &rip);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    CHECK(rc == 0, "a spinning thread in the region, in game code, is interrupted");
    CHECK(rip >= lo && rip < hi, "the RIP it was seen at lies in the given range");
    CHECK(g_result.load() == 2, "it resumes at sigsetjmp's second return with 2, its frame intact");

    g_ready = 0; g_stop = 0; g_result = -1;
    t = CreateThread(nullptr, 0, sleeper, nullptr, 0, &tid);
    while (!g_ready.load()) Sleep(1);
    Sleep(20);
    int outside = 0, other = 0;
    for (int i = 0; i < 20; ++i) {
        const int r = ck3_win_watchdog_interrupt(tid, g_region.load(), &lo, &hi, 1, &rip);
        if (r == 2) ++outside; else if (r != 0) ++other;
        if (r == 0) { other += 100; break; }
        Sleep(3);
    }
    CHECK(outside > 0 && other == 0, "a thread waiting in a system DLL is never interrupted (rc 2: not now)");
    g_stop = 1;
    while (g_ready.load() != 2) Sleep(1);
    const int after = ck3_win_watchdog_interrupt(tid, g_region.load(), &lo, &hi, 1, &rip);
    CHECK(after == 1, "a thread that has left the region is left alone (rc 1)");
    g_stop = 2;
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    CHECK(g_result.load() == 0, "and it runs on to its own end");
    CHECK(ck3_win_watchdog_interrupt(GetCurrentThreadId(), &g_in_protected_region, &lo, &hi, 1, &rip) == -1,
          "the calling thread is refused (it cannot suspend itself)");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("platform layer: windows (posix shim fault recovery)\n");
    CHECK(ck3_win_install_fault_recovery() == 0, "vectored handler installed");
    std::string seen;
    const std::uintptr_t poison = 0x140000000ull + 0x7FF0000000000000ull;
    CHECK(one_round(poison, &seen) == 1, "a non-canonical read recovered to sigsetjmp's second return");
    CHECK(seen == "{\"error\":\"memory_access_fault\"}", "the caller's reply string is intact and assigned after recovery");
    CHECK(one_round(0x10, &seen) == 1, "a null-page read recovered too");
    CHECK(seen == "{\"error\":\"memory_access_fault\"}", "and the reply again");
    int all = 1;
    for (int i = 0; i < 1000; ++i) all &= one_round(poison, &seen);
    CHECK(all == 1 && seen == "{\"error\":\"memory_access_fault\"}", "1000 recoveries in a row (no stack drift)");
    watchdog_cases();
    std::printf("%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
