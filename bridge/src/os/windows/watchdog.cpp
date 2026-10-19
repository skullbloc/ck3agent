// windows/watchdog.cpp — os::watchdog_arm / watchdog_disarm on Windows.
//
// Windows has no SIGALRM, so the watchdog cannot interrupt the watched thread
// the way the Linux dispatch watchdog does (ADR: "a timer thread + flag
// polling"). One background thread sleeps until the deadline; if the deadline
// passes without a disarm, it runs the callback ONCE, on its own thread. The
// callback sets the flag the watched code polls between steps -- it does not
// unwind anyone. Arming while armed replaces the deadline and callback.
//
// The thread is process-lifetime and never joined, like the Linux watchdog.

#include "win_internal.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bridge {
namespace os {

namespace {

struct Watchdog {
    std::mutex               m;
    std::condition_variable  cv;
    bool                     armed = false;
    bool                     fired = false;
    bool                     started = false;
    std::chrono::steady_clock::time_point deadline;
    WatchdogFn               fn = nullptr;
    void*                    ctx = nullptr;
};

Watchdog& wd() {
    static Watchdog w;
    return w;
}

void loop() {
    Watchdog& w = wd();
    std::unique_lock<std::mutex> lk(w.m);
    for (;;) {
        if (!w.armed) {
            w.cv.wait(lk);
            continue;
        }
        if (w.cv.wait_until(lk, w.deadline) == std::cv_status::timeout) {
            // Re-check under the lock: a disarm or re-arm may have raced in.
            if (w.armed && std::chrono::steady_clock::now() >= w.deadline) {
                WatchdogFn fn = w.fn;
                void* ctx = w.ctx;
                w.armed = false;
                w.fired = true;
                lk.unlock();
                if (fn) fn(ctx);
                lk.lock();
            }
        }
    }
}

}  // namespace

Result watchdog_arm(std::uint32_t ms, WatchdogFn fn, void* ctx) {
    Watchdog& w = wd();
    std::lock_guard<std::mutex> lk(w.m);
    w.fn = fn;
    w.ctx = ctx;
    w.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    w.armed = true;
    w.fired = false;
    if (!w.started) {
        w.started = true;
        std::thread(loop).detach();
    }
    w.cv.notify_all();
    return win::ok();
}

Result watchdog_disarm(bool* fired) {
    Watchdog& w = wd();
    std::lock_guard<std::mutex> lk(w.m);
    if (fired) *fired = w.fired;
    w.armed = false;
    w.cv.notify_all();
    return win::ok();
}

}  // namespace os
}  // namespace bridge
