#pragma once
// ck-duv5: WHICH THREAD FAULTED.
//
// The crash instrument reports the faulting instruction, its offset, the Ghidra
// VA and the register file — and, without this, not which thread was running.
// That is the highest-yield field in the whole capture: CK3's AI tick launches
// parallel workers, the bridge runs five threads of its own, and the engine's
// own tick is unguarded by design. If a fault is on an engine worker, every
// bridge-step hypothesis dies in one line. If it is on the game thread, the
// unguarded per-tick steps (ck-cg3b) are in scope. Nothing else in the capture
// separates those.
//
// WHY A REGISTRY AND NOT AN INFERENCE. A role guessed at fault time from
// whatever is observable is an interpretation wearing a measurement's clothes.
// Each bridge thread records its OWN identity at its OWN entry point, once, and
// the handler does a lookup. That makes the role a recorded fact, and makes the
// classifier drivable on the host with no game (see fault_location_probe).
//
// WHAT AN ABSENT ENTRY MEANS, AND WHAT IT MUST NOT SAY. Threads the bridge did
// not create — the engine's main thread, its AI workers, its loaders — are
// never in this table. That is a USEFUL signal, and it is still not a licence to
// print "engine worker": a tid absent from the table is a tid we have no record
// for, which is also what a registration that failed or never ran looks like.
// So an unknown tid reports UNREGISTERED and the reader draws the conclusion.
// Same rule as the unresolved rip offset in fault_site.h: a fallback that is
// well-formed and credible is indistinguishable from an answer.
//
// ASYNC-SIGNAL-SAFETY. Lookup runs inside a SIGSEGV handler: a fixed-size array
// of atomics, a linear scan, no lock, no allocation, no syscall. Registration
// happens on ordinary thread-entry paths.
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <unistd.h>
#include <sys/syscall.h>

namespace bridge {

enum class ThreadRole {
    Unregistered = 0,   // never a default that gets PRINTED as a role
    GameThread,         // the engine's AI-tick thread; where on_tick runs
    IpcServer,
    IpcReader,
    PipelineConductor,
    HookInstaller,
};

inline const char* thread_role_name(ThreadRole r) {
    switch (r) {
        case ThreadRole::GameThread:        return "game_thread";
        case ThreadRole::IpcServer:         return "ipc_server";
        case ThreadRole::IpcReader:         return "ipc_reader";
        case ThreadRole::PipelineConductor: return "pipeline_conductor";
        case ThreadRole::HookInstaller:     return "hook_installer";
        case ThreadRole::Unregistered:      break;
    }
    return "UNREGISTERED";
}

inline constexpr size_t kMaxRegisteredThreads = 16;

struct ThreadSlot {
    std::atomic<int>  tid{0};
    std::atomic<int>  role{static_cast<int>(ThreadRole::Unregistered)};
};

inline ThreadSlot* thread_table() {
    static ThreadSlot slots[kMaxRegisteredThreads];
    return slots;
}

// Count of registrations that found no free slot. If this is ever non-zero the
// table is too small and a real thread is reporting UNREGISTERED — which would
// make an absent entry ambiguous in exactly the way this header refuses to be.
// The handler prints it, so a crash log carries its own table-integrity witness.
inline std::atomic<int>& thread_table_overflow() {
    static std::atomic<int> n{0};
    return n;
}

inline int current_tid() { return static_cast<int>(syscall(SYS_gettid)); }

// Called once at each bridge thread's entry point. Idempotent per tid.
inline void register_thread(ThreadRole role) {
    const int me = current_tid();
    ThreadSlot* t = thread_table();
    for (size_t i = 0; i < kMaxRegisteredThreads; ++i) {
        int cur = t[i].tid.load(std::memory_order_acquire);
        if (cur == me) {                       // already registered: refresh role
            t[i].role.store(static_cast<int>(role), std::memory_order_release);
            return;
        }
    }
    for (size_t i = 0; i < kMaxRegisteredThreads; ++i) {
        int expected = 0;
        if (t[i].tid.compare_exchange_strong(expected, me,
                                             std::memory_order_acq_rel)) {
            t[i].role.store(static_cast<int>(role), std::memory_order_release);
            return;
        }
    }
    thread_table_overflow().fetch_add(1, std::memory_order_relaxed);
}

// Async-signal-safe. Returns Unregistered for any tid with no recorded entry.
inline ThreadRole role_of_tid(int tid) {
    const ThreadSlot* t = thread_table();
    for (size_t i = 0; i < kMaxRegisteredThreads; ++i) {
        if (t[i].tid.load(std::memory_order_acquire) == tid)
            return static_cast<ThreadRole>(t[i].role.load(std::memory_order_acquire));
    }
    return ThreadRole::Unregistered;
}

// One spelling, shared by the handler's three log sites and by the probe.
//
// Note what the UNREGISTERED line says and does not say: it reports that the
// bridge has no record of the thread, and names the table's state so a reader
// can tell "not a bridge thread" from "the table overflowed". It does NOT say
// "engine worker" — that is the reader's inference and the log has no basis
// for it.
inline const char* format_fault_thread(char* buf, size_t n, int tid) {
    const ThreadRole r = role_of_tid(tid);
    const int overflow = thread_table_overflow().load(std::memory_order_relaxed);
    if (r != ThreadRole::Unregistered) {
        snprintf(buf, n, "tid=%d thread=%s", tid, thread_role_name(r));
    } else {
        snprintf(buf, n,
                 "tid=%d thread=UNREGISTERED (no bridge record for this tid; "
                 "table_overflow=%d)", tid, overflow);
    }
    return buf;
}

}  // namespace bridge
