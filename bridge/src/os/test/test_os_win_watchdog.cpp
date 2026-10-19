// test_os_win_watchdog.cpp — os::watchdog_arm / watchdog_disarm on Windows,
// engine-free, under wine. The callback sets an atomic flag; the test polls
// (never sleeps on the callback thread) so a hung watchdog shows as a timeout,
// not a deadlock.

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

static std::atomic<int> g_fires{0};
static void on_fire(void* ctx) {
    g_fires.fetch_add(1);
    if (ctx) *static_cast<int*>(ctx) = 7;
}

// Spin up to `ms` waiting for the fire count to reach `want`.
static bool wait_fires(int want, int ms) {
    for (int i = 0; i < ms; i += 5) {
        if (g_fires.load() >= want) return true;
        Sleep(5);
    }
    return g_fires.load() >= want;
}

int main() {
    // 1. It fires after the deadline, once, with the ctx passed through.
    int marker = 0;
    g_fires = 0;
    check(os::succeeded(os::watchdog_arm(30, on_fire, &marker)), "watchdog_arm: ok");
    check(wait_fires(1, 2000), "watchdog fired after its deadline");
    check(marker == 7, "the callback received its ctx");
    Sleep(60);
    bool fired = false;
    check(os::succeeded(os::watchdog_disarm(&fired)) && fired, "disarm after a fire reports fired");
    check(g_fires.load() == 1, "it fired exactly once, not repeatedly");

    // 2. Disarm before the deadline: it never fires.
    g_fires = 0;
    os::watchdog_arm(1000, on_fire, nullptr);
    fired = true;
    check(os::succeeded(os::watchdog_disarm(&fired)) && !fired, "disarm before the deadline: not fired");
    check(!wait_fires(1, 200), "a disarmed watchdog does not fire");

    // 3. Re-arm replaces the deadline; the later arm is the one that fires.
    g_fires = 0;
    os::watchdog_arm(1000, on_fire, nullptr);
    os::watchdog_arm(30, on_fire, nullptr);   // sooner; replaces the first
    check(wait_fires(1, 2000) && g_fires.load() == 1, "re-arm: the new, sooner deadline fires once");
    os::watchdog_disarm(nullptr);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
