// test_os_session_pass.cpp — os::session_pass_* (src/os/session_pass.cpp),
// engine-free, ONE row set for both platforms: test_os_win_session_pass (built
// for Windows, run under wine by the gate) and test_os_linux_session_pass.
//
// A fake session block on its own page stands in for jomini's CSession, at the
// offsets the bridge itself reads (ck3_win::struct_layout::SESSION_* on Windows,
// ck3::dispatch_mgr::* on Linux), and a thread runs pdx_thread_run_loop's body
// over it (re/findings/windows/re_session_pass_hook_win.md §1: re-read the slot,
// call it with the argument, stop when it returns false or the stop byte is set).
//
// Rows: latch 0 / slot 0 -> not_yet; a wrong fn, a wrong argument or a lock kind
// the platform cannot take -> refused, slot untouched. The swap itself, made
// observable by write-protecting the session page so the install's slot write
// faults into a handler that acts and then lets it through: a slot written by
// someone else between the check and the swap -> refused, the foreign value
// stays; live is already set when the swap happens; a second installer racing
// the first is refused and the first one's state survives. Then over a running
// loop: install -> the slot holds the wrapper, the original still runs every
// pass; work runs with the session lock held; execute byte 0 -> no work; the
// lock held elsewhere -> no work, the pass still runs; a pass blocked inside the
// wrapper holds remove until it leaves, and does not drain once remove began;
// one that never leaves -> remove gives up at 2 s with 1 in flight; a slot
// overwritten before remove -> refused, the slot untouched, nothing drained;
// remove -> the original is back, nothing in flight, no more work; the
// original's return value reaches the loop.

#include "os/os.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "ck3_offsets_win.h"
#else
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include "ck3_offsets.h"
#endif

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
    std::fflush(stdout);
}

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── the platform section: the layout the bridge reads, the lock, the page ───
#ifdef _WIN32
namespace L = ck3_win::struct_layout;
constexpr std::size_t kFn = L::SESSION_THREAD_FN, kArg = L::SESSION_THREAD_ARG,
                      kStop = L::SESSION_THREAD_STOP, kLock = L::SESSION_EXEC_MUTEX,
                      kStarted = L::SESSION_THREAD_STARTED, kExecute = L::SESSION_EXECUTE_COMMANDS;
constexpr os::SessionLock kKind = os::SessionLock::srwlock_exclusive, kOtherKind = os::SessionLock::pthread_mutex;
#else
namespace L = ck3::dispatch_mgr;
constexpr std::size_t kFn = L::THREAD_FN, kArg = L::THREAD_ARG, kStop = L::THREAD_STOP,
                      kLock = L::EXEC_MUTEX, kStarted = L::THREAD_STARTED, kExecute = L::EXECUTE_COMMANDS;
constexpr os::SessionLock kKind = os::SessionLock::pthread_mutex, kOtherKind = os::SessionLock::srwlock_exclusive;
#endif
constexpr std::size_t kPage = 0x1000;
static_assert(kExecute < kPage && kLock + 64 < kPage, "the fake session fits one page");

static std::uint8_t* g_session = nullptr;  // page-aligned, its own page
static std::uintptr_t S() { return reinterpret_cast<std::uintptr_t>(g_session); }
static std::uintptr_t* slot() { return reinterpret_cast<std::uintptr_t*>(g_session + kFn); }
static volatile std::uintptr_t* vslot() { return reinterpret_cast<volatile std::uintptr_t*>(g_session + kFn); }

#ifdef _WIN32
static void alloc_session() {
    g_session = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}
static void protect(bool ro) {
    DWORD old = 0;
    VirtualProtect(g_session, kPage, ro ? PAGE_READONLY : PAGE_READWRITE, &old);
}
static void init_lock() { InitializeSRWLock(reinterpret_cast<PSRWLOCK>(g_session + kLock)); }
static bool lock_try() { return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_session + kLock)) != 0; }
static void lock_take() { AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_session + kLock)); }
static void lock_give() { ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_session + kLock)); }
#else
static void alloc_session() {
    void* p = mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_session = p == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(p);
}
static void protect(bool ro) { mprotect(g_session, kPage, ro ? PROT_READ : PROT_READ | PROT_WRITE); }
static pthread_mutex_t* mu() { return reinterpret_cast<pthread_mutex_t*>(g_session + kLock); }
static void init_lock() { pthread_mutex_init(mu(), nullptr); }
static bool lock_try() { return pthread_mutex_trylock(mu()) == 0; }
static void lock_take() { pthread_mutex_lock(mu()); }
static void lock_give() { pthread_mutex_unlock(mu()); }
#endif

// ── the swap probe: the install's write to the slot faults on the read-only
// page, the handler runs `g_on_swap` once (on the first faulting thread), then
// makes the page writable and the write is retried. Any other fault is not ours.
enum class OnSwap { none, overwrite, record_live, race_second };
static std::atomic<OnSwap> g_on_swap{OnSwap::none};
static std::atomic<bool> g_armed{false};
static std::atomic<int> g_swap_faults{0};
static std::atomic<bool> g_live_at_swap{false};
static std::atomic<bool> g_second_go{false}, g_second_done{false};
static std::atomic<std::uint64_t> g_first_tid{0};
static std::uintptr_t g_other_fn_addr = 0;

static void handler_sleep_1ms() {
#ifdef _WIN32
    Sleep(1);
#else
    timespec ts{0, 1000000};
    nanosleep(&ts, nullptr);
#endif
}
static std::uint64_t this_tid() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<std::uint64_t>(pthread_self());
#endif
}

// true: the fault was the slot write and it may be retried.
static bool on_slot_fault(std::uintptr_t addr) {
    if (!g_armed.load() || addr != S() + kFn) return false;
    g_swap_faults.fetch_add(1);
    std::uint64_t none = 0;
    const bool first = g_first_tid.compare_exchange_strong(none, this_tid()) || g_first_tid.load() == this_tid();
    if (first) {
        bool live = false;
        os::session_pass_live(&live);
        g_live_at_swap = live;
        const OnSwap act = g_on_swap.exchange(OnSwap::none);
        if (act == OnSwap::overwrite) {
            protect(false);
            *vslot() = g_other_fn_addr;
        } else if (act == OnSwap::race_second) {
            g_second_go = true;
            for (int i = 0; i < 3000 && !g_second_done.load(); ++i) handler_sleep_1ms();
        }
    }
    protect(false);
    return true;
}

#ifdef _WIN32
static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 &&
        er->ExceptionInformation[0] == 1 && on_slot_fault(static_cast<std::uintptr_t>(er->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
static void install_fault_handler() { AddVectoredExceptionHandler(1, veh); }
#else
static void segv(int sig, siginfo_t* si, void*) {
    if (on_slot_fault(reinterpret_cast<std::uintptr_t>(si->si_addr))) return;
    signal(sig, SIG_DFL);  // not ours: the retried access dies with the default action
}
static void install_fault_handler() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
#endif

// ── the fake loop ───────────────────────────────────────────────────────────
static std::atomic<int> g_passes{0};
static std::atomic<bool> g_loop_returns{true};
static std::atomic<bool> g_block{false}, g_entered{false}, g_release{false};
static bool fake_loop(void* s) {  // CSession_ThreadLoop's stand-in
    if (s == g_session) g_passes.fetch_add(1);
    if (g_block.exchange(false)) {  // one pass parks here until released
        g_entered = true;
        while (!g_release.load()) sleep_ms(1);
    }
    sleep_ms(1);
    return g_loop_returns.load();
}
static bool other_fn(void*) { sleep_ms(1); return true; }
// Someone else's hook that chains to whatever it replaced (here: the wrapper).
static std::atomic<std::uintptr_t> g_chain_to{0};
static bool chain_fn(void* s) { return reinterpret_cast<bool (*)(void*)>(g_chain_to.load())(s); }

static std::atomic<int> g_work{0};
static std::atomic<int> g_work_unlocked{0};
static void work() {
    g_work.fetch_add(1);
    // Held by the wrapper: a second try-lock must fail.
    if (lock_try()) { g_work_unlocked.fetch_add(1); lock_give(); }
}

static std::atomic<bool> g_loop_exited{false};
static void run_loop() {  // pdx_thread_run_loop's body
    using Fn = bool (*)(void*);
    bool r = true;
    while (*reinterpret_cast<volatile std::uint8_t*>(g_session + kStop) == 0 && r) {
        const auto fn = reinterpret_cast<Fn>(*vslot());
        r = fn(*reinterpret_cast<void* volatile*>(g_session + kArg));
    }
    g_loop_exited = true;
}

static bool wait_for(const std::atomic<int>& c, int at_least, int ms) {
    for (int i = 0; i < ms; i += 2) { if (c.load() >= at_least) return true; sleep_ms(2); }
    return c.load() >= at_least;
}
static bool wait_flag(const std::atomic<bool>& f, int ms) {
    for (int i = 0; i < ms; i += 2) { if (f.load()) return true; sleep_ms(2); }
    return f.load();
}

static os::SessionPassLayout layout() {
    return os::SessionPassLayout{S(), kFn, kArg, kStarted, kExecute, kLock, kKind,
                                 reinterpret_cast<std::uintptr_t>(&fake_loop)};
}

static bool is_live() {
    bool live = false;
    return os::succeeded(os::session_pass_live(&live)) && live;
}

int main() {
    alloc_session();
    if (!g_session) { std::printf("FAIL: no page for the fake session\n"); return 1; }
    install_fault_handler();
    g_other_fn_addr = reinterpret_cast<std::uintptr_t>(&other_fn);
    os::SessionPassReport rep{};
    const auto fake = reinterpret_cast<std::uintptr_t>(&fake_loop);
    int inflight = -1;

    // 1. Latch 0: not yet, nothing written.
    std::memset(g_session, 0, kPage);
    init_lock();
    *slot() = fake;
    *reinterpret_cast<void**>(g_session + kArg) = g_session;
    os::Result r = os::session_pass_install(layout(), work, &rep);
    check(!os::succeeded(r) && rep.state == os::SessionPassState::not_yet, "latch 0: not_yet");
    check(*slot() == fake, "latch 0: slot untouched");

    // 2. Latch 1, slot 0: not yet.
    g_session[kStarted] = 1;
    *slot() = 0;
    r = os::session_pass_install(layout(), work, &rep);
    check(!os::succeeded(r) && rep.state == os::SessionPassState::not_yet, "slot 0: not_yet");

    // 3. A different fn in the slot: refused, untouched.
    *slot() = g_other_fn_addr;
    r = os::session_pass_install(layout(), work, &rep);
    check(!os::succeeded(r) && rep.state == os::SessionPassState::refused && rep.slot_value == g_other_fn_addr,
          "wrong fn: refused, the report names what the slot held");
    check(*slot() == g_other_fn_addr, "wrong fn: slot untouched");

    // 4. The argument is not the session: refused.
    *slot() = fake;
    *reinterpret_cast<void**>(g_session + kArg) = &rep;
    r = os::session_pass_install(layout(), work, &rep);
    check(!os::succeeded(r) && rep.state == os::SessionPassState::refused, "wrong arg: refused");
    check(*slot() == fake, "wrong arg: slot untouched");
    *reinterpret_cast<void**>(g_session + kArg) = g_session;

    // 5. A lock the platform cannot take with the game's primitive: refused.
    os::SessionPassLayout pl = layout();
    pl.lock = kOtherKind;
    r = os::session_pass_install(pl, work, &rep);
    check(!os::succeeded(r) && rep.state == os::SessionPassState::refused && *slot() == fake,
          "the other platform's lock kind: refused, slot untouched");

    // 6. An unreadable session: not yet, no fault.
    os::SessionPassLayout bad = layout();
    bad.session = 0x10;
    r = os::session_pass_install(bad, work, &rep);
    check(!os::succeeded(r) && rep.state == os::SessionPassState::not_yet, "unreadable session: not_yet");

    // S1. The slot is written by someone else between the check and the swap:
    // the swap must be a compare-exchange (a plain store would overwrite it).
    g_on_swap = OnSwap::overwrite;
    g_first_tid = 0;
    g_armed = true;
    protect(true);
    r = os::session_pass_install(layout(), work, &rep);
    g_armed = false;
    protect(false);
    check(g_swap_faults.load() >= 1, "swap probe: the install's slot write reached the handler");
    check(!os::succeeded(r) && rep.state == os::SessionPassState::refused && rep.slot_value == g_other_fn_addr,
          "slot written before the swap: refused, the report names the foreign value");
    check(*slot() == g_other_fn_addr, "slot written before the swap: the foreign value stays");
    check(!is_live(), "slot written before the swap: not live");
    if (rep.state == os::SessionPassState::installed) os::session_pass_remove(&inflight);
    *slot() = fake;

    // S2. Live is set BEFORE the swap: the first pass through the wrapper drains.
    g_on_swap = OnSwap::record_live;
    g_first_tid = 0;
    g_live_at_swap = false;
    g_armed = true;
    protect(true);
    r = os::session_pass_install(layout(), work, &rep);
    g_armed = false;
    protect(false);
    check(os::succeeded(r) && *slot() == rep.wrapper, "after a lost swap the next install works");
    check(g_live_at_swap.load(), "live was already true when the slot was swapped");
    r = os::session_pass_remove(&inflight);
    check(os::succeeded(r) && *slot() == fake, "S2 remove: restored");

    // S3. A second installer racing the first (it runs while the first is at
    // its swap): refused, and the first one's install and live state survive.
    os::SessionPassReport rep_b{};
    os::Result r_b{os::Status::ok, 0, ""};
    g_second_go = false;
    g_second_done = false;
    std::thread second([&] {
        if (!wait_flag(g_second_go, 5000)) return;
        r_b = os::session_pass_install(layout(), work, &rep_b);
        g_second_done = true;
    });
    g_on_swap = OnSwap::race_second;
    g_first_tid = 0;
    g_armed = true;
    protect(true);
    r = os::session_pass_install(layout(), work, &rep);
    g_armed = false;
    protect(false);
    g_second_go = true;  // never parked if the probe did not fire
    second.join();
    check(!os::succeeded(r_b) && rep_b.state == os::SessionPassState::refused,
          "racing installer: refused");
    check(os::succeeded(r) && rep.state == os::SessionPassState::installed && *slot() == rep.wrapper,
          "racing installer: the first install stands");
    check(is_live(), "racing installer: still live (the loser clobbered nothing)");
    r = os::session_pass_remove(&inflight);
    check(os::succeeded(r) && *slot() == fake, "S3 remove: restored");
    *slot() = fake;

    // 7. Install over a running loop.
    g_session[kExecute] = 1;
    std::thread th(run_loop);
    check(wait_for(g_passes, 3, 2000), "the fake loop runs before the install");
    r = os::session_pass_install(layout(), work, &rep);
    check(os::succeeded(r) && rep.state == os::SessionPassState::installed, "install: ok, installed");
    check(*slot() == rep.wrapper && rep.wrapper != fake && rep.slot == S() + kFn,
          "install: the slot holds the wrapper");
    check(is_live(), "install: live");
    int p0 = g_passes.load();
    check(wait_for(g_work, 5, 2000), "work runs after the passes");
    check(g_passes.load() > p0, "the original pass still runs under the wrapper");
    check(g_work_unlocked.load() == 0, "work runs with the session lock held");
    os::SessionPassReport rep2{};
    r = os::session_pass_install(layout(), work, &rep2);
    check(!os::succeeded(r) && rep2.state == os::SessionPassState::refused, "a second install: refused");

    // 8. Execute byte 0: passes go on, no work.
    g_session[kExecute] = 0;
    sleep_ms(30);
    int w0 = g_work.load();
    p0 = g_passes.load();
    sleep_ms(60);
    check(g_passes.load() > p0 && g_work.load() == w0, "execute 0: passes run, no work");
    g_session[kExecute] = 1;

    // 9. The lock held elsewhere: passes go on, no work.
    lock_take();
    sleep_ms(30);
    w0 = g_work.load();
    p0 = g_passes.load();
    sleep_ms(60);
    check(g_passes.load() > p0 && g_work.load() == w0, "lock held elsewhere: passes run, work skipped");
    lock_give();
    w0 = g_work.load();
    check(wait_for(g_work, w0 + 3, 2000), "lock released: work resumes");

    // B1. A pass parked inside the wrapper (in the original) holds remove: remove
    // does not return before the pass leaves, and the pass does not drain once
    // remove began.
    g_release = false;
    g_entered = false;
    g_block = true;
    check(wait_flag(g_entered, 2000), "B1: a pass is parked inside the wrapper");
    const int wb = g_work.load();
    std::atomic<bool> removed{false};
    os::Result r_rm{os::Status::ok, 0, ""};
    int inflight_rm = -1;
    std::thread remover([&] { r_rm = os::session_pass_remove(&inflight_rm); removed = true; });
    sleep_ms(300);
    check(!removed.load(), "B1: remove has not returned at 300 ms with the pass still inside");
    g_release = true;
    const bool rm_done = wait_flag(removed, 3000);
    remover.join();
    check(rm_done && os::succeeded(r_rm) && inflight_rm == 0, "B1: remove returns ok once the pass left, none in flight");
    check(g_work.load() == wb, "B1: the parked pass did not drain after remove began");
    check(*slot() == fake && !is_live(), "B1: the slot holds the original, not live");

    // B2. A pass that never leaves: remove gives up at 2 s and says 1 in flight.
    r = os::session_pass_install(layout(), work, &rep);
    w0 = g_work.load();
    check(os::succeeded(r) && wait_for(g_work, w0 + 1, 2000), "B2: re-installed, a pass went through the wrapper");
    g_release = false;
    g_entered = false;
    g_block = true;
    check(wait_flag(g_entered, 2000), "B2: a pass is parked inside the wrapper");
    const long long t0 = now_ms();
    r = os::session_pass_remove(&inflight);
    const long long took = now_ms() - t0;
    check(!os::succeeded(r) && inflight == 1, "B2: remove fails with 1 in flight when the pass never leaves");
    check(took >= 1900, "B2: remove waited the full 2 s bound before giving up");
    g_release = true;
    sleep_ms(30);

    // R1. The slot overwritten by someone else's hook before remove: refused,
    // the slot untouched, and the wrapper (which that hook chains to) passes
    // through without draining.
    r = os::session_pass_install(layout(), work, &rep);
    w0 = g_work.load();
    const bool r1_in = os::succeeded(r) && rep.wrapper != 0;
    check(r1_in && wait_for(g_work, w0 + 1, 2000), "R1: re-installed, work runs");
    if (r1_in) {  // a chain to a wrapper that was never installed would call 0
        g_chain_to = rep.wrapper;
        *vslot() = reinterpret_cast<std::uintptr_t>(&chain_fn);
        sleep_ms(30);
        r = os::session_pass_remove(&inflight);
        check(!os::succeeded(r), "R1: remove over a foreign slot value: refused");
        check(*slot() == reinterpret_cast<std::uintptr_t>(&chain_fn), "R1: the foreign value stays in the slot");
        sleep_ms(30);
        w0 = g_work.load();
        p0 = g_passes.load();
        sleep_ms(60);
        check(g_passes.load() > p0 && g_work.load() == w0, "R1: passes chain through the wrapper, nothing drained");
        *vslot() = rep.wrapper;  // the foreign hook unhooks itself
        r = os::session_pass_remove(&inflight);
        check(os::succeeded(r) && *slot() == fake, "R1: a second remove restores the original");
    }
    *vslot() = fake;

    // 10. Remove: the original is back, nothing in flight, no more work.
    r = os::session_pass_install(layout(), work, &rep);
    w0 = g_work.load();
    check(os::succeeded(r) && wait_for(g_work, w0 + 1, 2000), "re-install: work runs");
    r = os::session_pass_remove(&inflight);
    check(os::succeeded(r) && inflight == 0, "remove: ok, none in flight");
    check(*slot() == fake, "remove: the slot holds the original again");
    check(!is_live(), "remove: not live");
    w0 = g_work.load();
    p0 = g_passes.load();
    sleep_ms(60);
    check(g_passes.load() > p0 && g_work.load() == w0, "after remove: passes run, no work");

    // 11. Re-install, then the original's false return stops the loop through the wrapper.
    r = os::session_pass_install(layout(), work, &rep);
    check(os::succeeded(r) && *slot() == rep.wrapper, "re-install after remove: ok");
    // Flip the original's value only once a pass has gone through the wrapper:
    // a pass that loaded the slot before the swap would otherwise hand the
    // original's false straight to the loop and prove nothing about the wrapper.
    w0 = g_work.load();
    check(wait_for(g_work, w0 + 1, 2000), "re-install: a pass went through the wrapper");
    g_loop_returns = false;
    check(wait_flag(g_loop_exited, 2000), "the original's return value reaches the loop (it exits)");
    if (!g_loop_exited.load()) g_session[kStop] = 1;  // never leave a joinable loop running
    th.join();
    r = os::session_pass_remove(&inflight);
    check(os::succeeded(r) && *slot() == fake, "final remove: restored");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
