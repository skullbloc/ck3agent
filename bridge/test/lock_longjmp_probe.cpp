// ck-1v1t — does siglongjmp out of a scope holding a lock leave it LOCKED?
//
// This is the whole argument for the bead, so it is demonstrated rather than
// asserted from language-lawyering. apply_personality_locks() took
// unique_lock<shared_mutex> in its own frame; all three call sites recover from
// a SIGSEGV with siglongjmp to a recovery point OUTSIDE that frame. If
// siglongjmp skips the destructor, the mutex is leaked locked — and that
// function runs 3x per on_tick on the game thread, so the next call blocks on a
// mutex nothing will ever release.
//
// try_lock is used throughout, never lock(), so a failure REPORTS instead of
// hanging the test. A probe for a deadlock must not be able to deadlock.
//
// Exit 0 pass / 1 FAIL.
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>

static std::shared_mutex g_mu;
static sigjmp_buf g_jmp;
static int failures = 0;

static void row(const char* name, bool ok, const char* detail) {
    printf("%-52s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) failures++;
}

// THE OLD SHAPE: the lock guard lives in the frame we jump out of.
static void locked_then_faults() {
    std::unique_lock<std::shared_mutex> lock(g_mu);
    siglongjmp(g_jmp, 1);              // stands in for the recovered SIGSEGV
}

// THE NEW SHAPE: the lock is released before the section that can fault.
static void snapshot_then_faults() {
    {
        std::shared_lock<std::shared_mutex> lock(g_mu);
        // ... copy what we need under the lock ...
    }                                   // released HERE
    siglongjmp(g_jmp, 1);
}

int main() {
    // 1. Precondition: the mutex starts free, so a later failure means
    //    something took it and did not give it back.
    bool got = g_mu.try_lock();
    row("mutex_starts_unlocked", got, "precondition");
    if (got) g_mu.unlock();

    // 2. THE DEFECT. Jump out while holding it, then see if anyone can have it.
    if (sigsetjmp(g_jmp, 1) == 0) {
        locked_then_faults();
        row("old_shape_reached_the_jump", false, "siglongjmp did not fire");
    } else {
        const bool reacquired = g_mu.try_lock();
        row("longjmp_over_unique_lock_LEAKS_the_mutex", !reacquired,
            reacquired ? "mutex was free — destructor apparently ran"
                       : "mutex still held with no owner: the game thread would "
                         "block here forever, 3x per on_tick");
        if (reacquired) g_mu.unlock();
    }

    // The leaked mutex cannot be unlocked by us (we are not the owner and there
    // is no owner), so the rest of the test uses a SECOND mutex. That this is
    // necessary at all is the point.
    static std::shared_mutex g_mu2;
    struct Swap { Swap() {} } s; (void)s;

    // 3. THE FIX. Same jump, lock released first.
    if (sigsetjmp(g_jmp, 1) == 0) {
        {
            std::shared_lock<std::shared_mutex> l(g_mu2);
        }
        // mirror snapshot_then_faults() against the second mutex
        siglongjmp(g_jmp, 1);
    } else {
        const bool reacquired = g_mu2.try_lock();
        row("lock_released_before_the_jump_survives", reacquired,
            reacquired ? "mutex free after the jump — the fix holds"
                       : "still held: the narrowing did not work");
        if (reacquired) g_mu2.unlock();
    }

    (void)snapshot_then_faults;   // referenced for documentation of the shape

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
