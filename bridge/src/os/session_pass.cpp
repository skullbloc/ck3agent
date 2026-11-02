// os/session_pass.cpp — os::session_pass_*, ONE cell for Linux and Windows
// (L14; m-16 made the two wrappers one). The session-pass hook through a
// data-pointer swap: re/findings/re_frame_update_hook.md (Linux, L13) and
// re/findings/windows/re_session_pass_hook_win.md (the MSVC build).
//
// The game's session thread runs `for (;;) { fn = *slot; if (!fn(arg)) break; }`
// and re-reads the slot on every pass, so a compare-exchange of the slot takes
// effect on the next pass and a compare-exchange back un-hooks it. The slot is
// in the game's writable data: no protection change, no code patched, no
// trampoline.
//
// The engine takes its execute lock with a TRY-lock around ProcessCommandQueue;
// the wrapper takes the same lock with the same primitive and skips the drain on
// contention (the main thread holds it during a load or hot-join). The primitive
// is the platform's: an SRWLOCK on Windows, a pthread mutex on Linux, and the
// layout must name it (a kind this platform cannot take is refused).
//
// Everything else is shared: the preconditions and refusals, the CAS install,
// the CAS remove that leaves a slot it no longer owns, the in-flight count and
// its bounded wait. The platform section below is the only #ifdef.
//
// Who builds it: ck3_os on Windows (the session_pass cell, test
// test_os_win_session_pass), ck3_os and libck3_bridge.so on Linux (test
// test_os_linux_session_pass). Both tests are src/os/test/test_os_session_pass.cpp.

#include "os/os.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include "bridge.h"  // safe_read_bytes (process_vm_readv): libck3_bridge.so has no os::safe_read
#endif

namespace bridge {
namespace os {

namespace {

// ── the platform section ────────────────────────────────────────────────────
#ifdef _WIN32
#define CK3_SP "windows: "
constexpr SessionLock kTheLock = SessionLock::srwlock_exclusive;
bool lock_try(std::uintptr_t a) { return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(a)) != 0; }
void lock_release(std::uintptr_t a) { ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(a)); }
std::uintptr_t slot_cas(std::uintptr_t slot, std::uintptr_t expect, std::uintptr_t desired) {
    return reinterpret_cast<std::uintptr_t>(InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(desired),
        reinterpret_cast<void*>(expect)));
}
void sleep_ms(int ms) { Sleep(static_cast<DWORD>(ms)); }
bool read_bytes(void* dst, std::uintptr_t src, std::size_t n) { return succeeded(safe_read(dst, src, n)); }
#else
#define CK3_SP "linux: "
constexpr SessionLock kTheLock = SessionLock::pthread_mutex;
bool lock_try(std::uintptr_t a) { return pthread_mutex_trylock(reinterpret_cast<pthread_mutex_t*>(a)) == 0; }
void lock_release(std::uintptr_t a) { pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(a)); }
std::uintptr_t slot_cas(std::uintptr_t slot, std::uintptr_t expect, std::uintptr_t desired) {
    __atomic_compare_exchange_n(reinterpret_cast<std::uintptr_t*>(slot), &expect, desired, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expect;  // the slot's value before: == expect on success
}
void sleep_ms(int ms) { usleep(static_cast<useconds_t>(ms) * 1000u); }
bool read_bytes(void* dst, std::uintptr_t src, std::size_t n) { return bridge::safe_read_bytes(src, dst, n); }
#endif

Result ok() { return Result{Status::ok, 0, ""}; }
Result failed(const char* d) { return Result{Status::failed, 0, d}; }

// ── the cell ────────────────────────────────────────────────────────────────
using LoopFn = bool (*)(void* session);

// One installer at a time, from the claim to a remove that restored the slot:
// the globals below are written only by the claim's holder, so an install that
// loses its slot CAS resets state nobody else is using (m-18).
std::atomic<bool>             g_claimed{false};
std::atomic<bool>             g_live{false};
std::atomic<std::uintptr_t>   g_orig{0};       // the loop fn the slot held
std::atomic<std::uintptr_t>   g_slot{0};       // &session+fn_off while installed
std::atomic<SessionPassWork>  g_work{nullptr};
std::atomic<int>              g_inflight{0};
std::size_t                   g_execute_off = 0;
std::size_t                   g_lock_off = 0;

bool session_pass_wrapper(void* session) {
    g_inflight.fetch_add(1, std::memory_order_acq_rel);
    const auto orig = reinterpret_cast<LoopFn>(g_orig.load(std::memory_order_acquire));
    // The engine's own pass: network update, ProcessCommandQueue under the lock,
    // the 5 ms sleep when idle. Its value is the run loop's continue bit.
    const bool r = orig(session);
    const auto s = reinterpret_cast<std::uintptr_t>(session);
    SessionPassWork work = g_work.load(std::memory_order_acquire);
    if (work && g_live.load(std::memory_order_acquire) &&
        *reinterpret_cast<volatile std::uint8_t*>(s + g_execute_off) == 1) {
        const std::uintptr_t lock = s + g_lock_off;
        if (lock_try(lock)) {
            work();
            lock_release(lock);
        }
    }
    g_inflight.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}

void put(SessionPassReport* r, SessionPassState st, std::uintptr_t slot, std::uintptr_t v,
         std::uintptr_t arg, std::uintptr_t wrapper) {
    if (r) { r->state = st; r->slot = slot; r->slot_value = v; r->arg_value = arg; r->wrapper = wrapper; }
}

Result not_yet(SessionPassReport* r, std::uintptr_t slot, std::uintptr_t v, std::uintptr_t arg,
               const char* why) {
    put(r, SessionPassState::not_yet, slot, v, arg, 0);
    return failed(why);
}

Result refused(SessionPassReport* r, std::uintptr_t slot, std::uintptr_t v, std::uintptr_t arg,
               const char* why) {
    put(r, SessionPassState::refused, slot, v, arg, 0);
    return failed(why);
}

// Releases the claim on every return of an install that did not install.
struct Claim {
    bool kept = false;
    ~Claim() { if (!kept) g_claimed.store(false, std::memory_order_release); }
};

}  // namespace

Result session_pass_live(bool* live) {
    if (!live) return Result{Status::failed, 0, "live is null"};
    *live = g_live.load(std::memory_order_acquire);
    return ok();
}

Result session_pass_install(const SessionPassLayout& l, SessionPassWork work,
                            SessionPassReport* report) {
    put(report, SessionPassState::refused, 0, 0, 0, 0);
    if (g_claimed.exchange(true, std::memory_order_acq_rel))
        return refused(report, 0, 0, 0,
                       CK3_SP "a session-pass hook is already installed, or another install is "
                       "in progress");
    Claim claim;
    if (l.lock != kTheLock)
        return refused(report, 0, 0, 0,
                       CK3_SP "the layout's session lock is not this platform's primitive; the "
                       "wrapper cannot take it the way the game does");
    if (!work || !l.session || !l.expected_fn)
        return refused(report, 0, 0, 0, CK3_SP "no work, session or expected loop fn");
    const std::uintptr_t slot = l.session + l.fn_off;

    std::uint8_t started = 0;
    if (!read_bytes(&started, l.session + l.started_off, 1))
        return not_yet(report, slot, 0, 0, CK3_SP "the session's started latch is not readable yet");
    if (started != 1)
        return not_yet(report, slot, 0, 0, CK3_SP "the session thread is not started (latch != 1)");
    std::uintptr_t cur = 0, arg = 0;
    if (!read_bytes(&cur, slot, sizeof cur) || !read_bytes(&arg, l.session + l.arg_off, sizeof arg))
        return not_yet(report, slot, 0, 0, CK3_SP "the session's thread block is not readable yet");
    if (cur == 0) return not_yet(report, slot, cur, arg, CK3_SP "the loop fn slot is still 0");
    if (cur != l.expected_fn || arg != l.session)
        return refused(report, slot, cur, arg,
                       CK3_SP "the slot does not hold the expected loop fn, or its argument is "
                       "not the session (not this build's layout, or already swapped): no chain "
                       "to an unknown target");
    std::uint8_t probe = 0;
    if (!read_bytes(&probe, l.session + l.execute_off, 1) ||
        !read_bytes(&probe, l.session + l.lock_off, 1))
        return refused(report, slot, cur, arg, CK3_SP "the execute byte or the lock is not readable");

    g_execute_off = l.execute_off;
    g_lock_off = l.lock_off;
    g_work.store(work, std::memory_order_release);
    g_orig.store(cur, std::memory_order_release);
    const auto mine = reinterpret_cast<std::uintptr_t>(&session_pass_wrapper);
    // Live BEFORE the swap: the first pass through the wrapper must drain.
    g_live.store(true, std::memory_order_release);
    const std::uintptr_t prev = slot_cas(slot, cur, mine);
    if (prev != cur) {
        // Someone outside the bridge wrote the slot between the read and the
        // swap. The claim makes every global above ours, so resetting them
        // clobbers nothing.
        g_live.store(false, std::memory_order_release);
        g_work.store(nullptr, std::memory_order_release);
        g_orig.store(0, std::memory_order_release);
        return refused(report, slot, prev, arg,
                       CK3_SP "the slot changed between the check and the swap; not swapped");
    }
    g_slot.store(slot, std::memory_order_release);
    claim.kept = true;
    put(report, SessionPassState::installed, slot, cur, arg, mine);
    return ok();
}

Result session_pass_remove(int* inflight) {
    if (inflight) *inflight = 0;
    const std::uintptr_t slot = g_slot.load(std::memory_order_acquire);
    if (!slot) return ok();
    const std::uintptr_t orig = g_orig.load(std::memory_order_acquire);
    const auto mine = reinterpret_cast<std::uintptr_t>(&session_pass_wrapper);
    g_live.store(false, std::memory_order_release);
    const bool restored = slot_cas(slot, mine, orig) == mine;
    // The in-flight count misses one case: a pass that loaded the slot (the
    // wrapper) before the exchange and has not reached the wrapper's first
    // instruction. That gap is the loop's load-and-call, a few instructions, not
    // a pass (the 5 ms idle sleep is inside the original, already counted), so
    // it stays open only while the session thread is preempted there. The 20 ms
    // gives such a thread a scheduling quantum to enter and be counted; it is a
    // heuristic, not a bound, and nothing observes a thread still parked there.
    sleep_ms(20);
    int waited = 0;
    while (g_inflight.load(std::memory_order_acquire) != 0 && waited < 2000) {
        sleep_ms(1);
        ++waited;
    }
    const int left = g_inflight.load(std::memory_order_acquire);
    if (inflight) *inflight = left;
    if (!restored) {
        // Someone else's value is in the slot now: leave it (it may chain to us,
        // so the wrapper keeps passing through to the original, without
        // draining) and keep the claim, so a remove can be tried again.
        return failed(CK3_SP "the slot no longer held the wrapper; not restored");
    }
    g_slot.store(0, std::memory_order_release);
    g_work.store(nullptr, std::memory_order_release);
    g_claimed.store(false, std::memory_order_release);
    if (left != 0) return failed(CK3_SP "a pass was still inside the wrapper after 2 s");
    return ok();
}

}  // namespace os
}  // namespace bridge
