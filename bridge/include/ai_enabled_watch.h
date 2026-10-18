// ai_enabled_watch.h — ck-6xi6 diagnostic: WHO writes ai_ctrl+0x28?
//
// DIAGNOSTIC ONLY. Env-gated on CK3_AI_WATCH=1, never merged to main, rides its
// own non-counting tag. Off, it costs one relaxed atomic load per tick.
//
// TWO INSTRUMENTS, NOT A CHOICE. They answer different questions and the bead
// needs both:
//   sampler     WHEN. The real period. The agent's set_character_ai calls cannot
//               give it — those are NOTICING times, so their gaps bound the true
//               period from above only (ck-zgh4).
//   watchpoint  WHO.  The writer's RIP as a literal. A sampler sees that a value
//               changed between two reads and nothing about who changed it —
//               asking it to name the writer is a misplaced instrument.
//
// AND THE PAIR DISCRIMINATES A THIRD THING FOR FREE: a transition the SAMPLER
// sees while the WATCHPOINT stayed silent means the writer is on a thread the
// breakpoint is not armed on. Hardware breakpoints are per-task, so this arms on
// the GAME thread (it is called from the tick hook). Sampler-saw / watchpoint-
// silent is therefore a positive reading, not a failure: an off-thread writer.
#pragma once

#include "primary_cause.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <ucontext.h>

namespace bridge {

//: SIGRTMIN is __libc_current_sigrtmin() on glibc, so it is NOT a constant
//: expression — a function, not a constexpr variable.
inline int ai_watch_sig() { return SIGRTMIN + 4; }

struct AiWatchHit {          //: filled IN A SIGNAL HANDLER — POD only
    //: ORDER ACROSS KINDS. tick is too coarse: several writes, a switch and a
    //: move can share one tick, and the hypothesis is about what happened
    //: BETWEEN two of them. One monotonic counter, taken by the handler and by
    //: the game thread from the same atomic, makes the ordering of the READOUT
    //: checkable instead of asserted — seq must increase down the log.
    uint64_t seq;
    uint64_t tick;
    uint64_t rip;
    uint32_t tid;
    //: THE SUBJECT AT THE TIME OF THE WRITE. The drain used to label every hit
    //: with the LIVE g_ai_watch_char, which was harmless while the watch could
    //: never change subject and becomes an attribution bug the moment it can:
    //: writes to the old character would be printed under the new one's id.
    uint32_t char_id;
    uint8_t  value;
    uint8_t  from_selftest;  //: the INSTRUMENT'S own write, not the subject's
};

//: SPSC ring: producer is the signal handler, consumer is the tick hook. Atomic
//: indices from the start — a plain int here is the ck-yg1s defect, and this one
//: has a signal handler for a producer, which is worse than a second thread.
struct AiWatchRing {
    static constexpr int kSlots = 64;
    //: ck-zs2x: the USABLE capacity is kSlots - 1, not kSlots. push() refuses when
    //: (tail+1)%kSlots == head, so one slot is permanently reserved to keep "empty"
    //: (head == tail) distinguishable from "full". 64 is the array; 63 is what the
    //: ring can ever hold. Named because the difference is exactly the case that
    //: drops: a burst of 64 loses one, and a reader reasoning from kSlots would
    //: call that a bug in the counter rather than the capacity.
    static constexpr int kCapacity = kSlots - 1;
    AiWatchHit slot[kSlots];
    std::atomic<int> head{0}, tail{0};
    std::atomic<unsigned long long> dropped{0};

    //: SIGNAL-HANDLER SIDE. No allocation, no locks, no fprintf.
    void push(const AiWatchHit& h) {
        const int t = tail.load(std::memory_order_relaxed);
        const int n = (t + 1) % kSlots;
        if (n == head.load(std::memory_order_acquire)) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        slot[t] = h;
        tail.store(n, std::memory_order_release);
    }
    bool pop(AiWatchHit* out) {
        const int h = head.load(std::memory_order_relaxed);
        if (h == tail.load(std::memory_order_acquire)) return false;
        *out = slot[h];
        head.store((h + 1) % kSlots, std::memory_order_release);
        return true;
    }
};

inline AiWatchRing        g_ai_watch_ring;
inline std::atomic<bool>  g_ai_watch_on{false};
inline std::atomic<int>   g_ai_watch_fd{-1};
inline std::atomic<uintptr_t> g_ai_watch_addr{0};
inline std::atomic<uint32_t>  g_ai_watch_char{0};
inline std::atomic<bool>      g_ai_watch_selftesting{false};
//: TERMINAL. Once the watched controller is GONE the watch does not resume — see
//: ai_watch_disarm for why re-arming would silently change the subject.
inline std::atomic<bool>      g_ai_watch_retired{false};
inline std::atomic<uint8_t>   g_ai_watch_last{0xFF};   //: 0xFF = not yet sampled
inline std::atomic<unsigned long long> g_ai_watch_hits{0};
inline std::atomic<unsigned long long> g_ai_watch_transitions{0};
inline std::atomic<uint64_t>* g_ai_watch_tick = nullptr;   //: set at arm time

//: ONE ORDER FOR EVERY RECORD, whoever emits it. fetch_add on a lock-free
//: atomic is async-signal-safe, so the handler and the game thread can share it.
//:
//: seq IS A SORT KEY, NOT AN EMISSION ORDER. I first told the node seat that "seq
//: must strictly increase down the log; if it does not, the log was reordered" —
//: WRONG, and wrong in the dangerous direction: it rejects a correct artefact.
//: Writes reach the log through the ring, so they are emitted when someone
//: DRAINS, while game-thread lines are written immediately. Draining before every
//: game-thread line narrows the gap but cannot close it — a watchpoint can trip
//: at any instruction, including between a drain and the next fprintf.
//:
//: THE READOUT RULE IS: SORT BY seq, THEN READ. That is what seq is for, and it
//: is strictly better than hoping emission order is already right.
//:
//: AND DISCARD THE CONTROL-FLIP PAIR AFTER EACH **SELFTEST**, NOT AFTER EACH
//: ARMED. I told the node seat "after each ARMED" and that rule DESTROYS DATA:
//: ai_watch_rearm_if_moved re-arms without running a selftest, so a MOVED emits an
//: ARMED with no flips behind it — and a reader discarding two lines after that
//: ARMED would throw away the next two ORGANIC writes, which are the only thing
//: the run is looking for. The selftest is what produces the pair, so the selftest
//: is what marks it. Corrected by bt-bbd from run 2's own artefact.
//: A HARDWARE WATCHPOINT IS PER-TASK, AND THE TICK HOOK IS NOT ONE TASK.
//:
//: perf_event_open(&pe, 0, ...) scopes the breakpoint to the CALLING THREAD. The
//: comment below the arming block used to say "this is THE GAME THREAD" —
//: singular, and wrong. ck-zgh4 measured the node's CK3 running the hook on at
//: least two of five workers ("Primary Task 1".."Primary Task 5") within 36
//: ticks: the two ARMED lines of one known-positive carry different tids.
//:
//: SO A SINGLE ARM SEES ONE WORKER'S SHARE OF THE WRITES, and a zero cannot be
//: read as "nothing wrote". That is not a hypothetical: it retroactively empties
//: the watchpoint column of run 1's 2x2, whose zero came from one arm on one
//: worker across fifteen game-months. (The sampler column survives — it
//: dereferences the byte directly from whichever thread is running, and run 1
//: also had TRANSITION=0.)
//:
//: FIX: cover every thread the hook is seen on. Each thread has its own debug
//: registers, so one byte-watchpoint per worker costs one DR slot each, and five
//: workers fit comfortably in the four-per-thread budget.
struct AiWatchTidFd {
    uint32_t tid = 0;
    int      fd  = -1;
};
inline constexpr int kAiWatchMaxTids = 16;
inline AiWatchTidFd g_ai_watch_tids[kAiWatchMaxTids];
inline std::atomic<int> g_ai_watch_ntids{0};
//: Threads seen running the hook but NOT coverable (table full, or the open
//: failed). A readout that cannot say this number cannot interpret a zero.
inline std::atomic<unsigned long long> g_ai_watch_uncovered{0};
//: WRITES PER THREAD. "Who writes" is the run's question and a total cannot answer
//: it; five workers with one write each and one worker with five are different
//: findings. Parallel to g_ai_watch_tids by index.
inline std::atomic<unsigned long long> g_ai_watch_tid_writes[kAiWatchMaxTids];
//: A TRANSITION NOBODY'S WATCHPOINT REPORTED. The sampler reads the byte directly
//: from whatever thread runs the hook, so it sees a change regardless of which
//: thread caused it. A watchpoint only sees writes on the threads we covered. So
//: a transition with no write behind it is POSITIVE EVIDENCE of a write we could
//: not see — which is the only way to detect the gap that per-task breakpoints
//: create, and it converts "we might have missed some" into a countable number.
inline std::atomic<unsigned long long> g_ai_watch_write_gaps{0};
inline std::atomic<unsigned long long> g_ai_watch_writes_at_last_transition{0};

inline std::atomic<uint64_t> g_ai_watch_seq{0};
inline uint64_t ai_watch_next_seq() {
    return g_ai_watch_seq.fetch_add(1, std::memory_order_relaxed);
}

//: The cause register lives in its own dependency-free header so the router
//: can RECORD a cause without pulling in perf_event_open and the ring.

//: Forward-declared: ai_watch_selftest drains before writing its own line, and
//: is defined above the drain.
inline void ai_watch_drain(FILE* log);

inline bool ai_watch_requested() {
    const char* v = getenv("CK3_AI_WATCH");
    return v && v[0] == '1';
}

//: THE HANDLER. Async-signal-safe: reads the ucontext, pushes POD, returns.
inline void ai_watch_handler(int, siginfo_t*, void* uc) {
    AiWatchHit h{};
    h.seq  = ai_watch_next_seq();
    h.tick = g_ai_watch_tick ? g_ai_watch_tick->load(std::memory_order_relaxed) : 0;
    h.char_id = g_ai_watch_char.load(std::memory_order_relaxed);
    auto* c = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    h.rip = static_cast<uint64_t>(c->uc_mcontext.gregs[REG_RIP]);
#else
    h.rip = 0;
#endif
    h.tid = static_cast<uint32_t>(syscall(SYS_gettid));
    const uintptr_t a = g_ai_watch_addr.load(std::memory_order_relaxed);
    h.value = a ? *reinterpret_cast<volatile uint8_t*>(a) : 0xFF;
    h.from_selftest = g_ai_watch_selftesting.load(std::memory_order_relaxed) ? 1 : 0;
    g_ai_watch_hits.fetch_add(1, std::memory_order_relaxed);
    g_ai_watch_ring.push(h);
}

//: Arm a BYTE-granular hardware write breakpoint. Must be called ON THE GAME
//: THREAD: hardware breakpoints are per-task, so this watches the thread that
//: arms it. mprotect+SIGSEGV was rejected — that byte shares a 4K page with hot
//: character data and would trap on every neighbouring write.
//: ONE WATCHPOINT PER THREAD, AND ANY THREAD MAY ARM ANY OTHER.
//:
//: perf_event_open with pid=0 covers only the CALLING thread, and the tick hook is
//: not one thread: ck-zgh4 measured the node's CK3 running it on two of five
//: "Primary Task" workers within 36 ticks — two ARMED lines, two different tids.
//: A single arm therefore saw ONE WORKER'S SHARE of the writes, which is why run
//: 1's watchpoint column cannot distinguish "nothing wrote" from "the writes were
//: on the other four". (Its sampler column survives: the sampler dereferences the
//: byte from whichever thread runs the hook, and run 1 had TRANSITION=0 too.)
//:
//: WHAT IS ACTUALLY PERMITTED, measured on this host at perf_event_paranoid=2
//: rather than reasoned about:
//:
//:     pid=0 (implicit self)   OK
//:     explicit OWN tid        OK
//:     explicit SIBLING tid    OK
//:
//: I first measured all three as EACCES and concluded that only a thread could arm
//: itself. That was wrong, and the cause was mine: this function omitted
//: exclude_kernel, which ai_watch_arm had always set. Under paranoid=2 an attr
//: without exclude_kernel asks to measure kernel space and is refused — so I read
//: my own missing field as a statement about which tids the policy allows, and
//: nearly shipped the weaker design because of it.
//:
//: SO: enumerate every thread and arm each one. That covers threads that never run
//: the tick hook, which a self-arming scheme could not. Each thread has its own
//: debug registers, so one byte-watchpoint per worker costs one DR slot each.
inline int ai_watch_open_for_tid(uintptr_t byte_addr, uint32_t tid, FILE* log) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof pe);
    pe.type           = PERF_TYPE_BREAKPOINT;
    pe.size           = sizeof pe;
    pe.bp_type        = HW_BREAKPOINT_W;
    pe.bp_addr        = byte_addr;
    pe.bp_len         = HW_BREAKPOINT_LEN_1;
    pe.sample_period  = 1;
    pe.precise_ip     = 2;
    pe.wakeup_events  = 1;
    pe.disabled       = 0;
    //: BOTH REQUIRED under paranoid=2. Omitting them is EACCES, and the EACCES
    //: says nothing about the tid — see the note above, which cost me a design.
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    const int fd = static_cast<int>(
        syscall(__NR_perf_event_open, &pe, static_cast<pid_t>(tid), -1, -1, 0));
    if (fd < 0) {
        g_ai_watch_uncovered.fetch_add(1, std::memory_order_relaxed);
        if (log) fprintf(log, "[CK3-Bridge][aiwatch] THREAD-UNCOVERED seq=%llu "
                              "tid=%u errno=%d — this worker's writes are INVISIBLE; "
                              "a zero result cannot be read as 'nothing wrote'\n",
                         (unsigned long long)ai_watch_next_seq(), tid, errno);
        return -1;
    }
    fcntl(fd, F_SETFL, O_ASYNC | O_NONBLOCK);
    fcntl(fd, F_SETSIG, ai_watch_sig());
    //: Delivered to the thread that TRIPPED it, so the hit's tid is the WRITER'S
    //: thread — which is the run's actual question.
    struct f_owner_ex ow;
    ow.type = F_OWNER_TID;
    ow.pid  = static_cast<pid_t>(tid);
    fcntl(fd, F_SETOWN_EX, &ow);
    return fd;
}

inline int ai_watch_open_for_self(uintptr_t byte_addr, FILE* log) {
    return ai_watch_open_for_tid(
        byte_addr, static_cast<uint32_t>(syscall(SYS_gettid)), log);
}

inline void ai_watch_close_all_tids() {
    const int n = g_ai_watch_ntids.exchange(0, std::memory_order_acq_rel);
    for (int i = 0; i < n && i < kAiWatchMaxTids; ++i) {
        if (g_ai_watch_tids[i].fd >= 0) close(g_ai_watch_tids[i].fd);
        g_ai_watch_tids[i].fd = -1;
        g_ai_watch_tids[i].tid = 0;
    }
}

inline bool ai_watch_tid_is_covered(uint32_t tid) {
    const int n = g_ai_watch_ntids.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kAiWatchMaxTids; ++i)
        if (g_ai_watch_tids[i].tid == tid) return true;
    return false;
}

//: COVER EVERY THREAD IN THE PROCESS. Enumerated from /proc/self/task rather than
//: waiting to be called on each one, because a write can land on a worker that
//: never runs the tick hook — and covering only the threads we have SEEN would
//: leave exactly the gap this exists to close.
inline int ai_watch_cover_all_threads(uintptr_t byte_addr, FILE* log) {
    if (!byte_addr) return 0;
    DIR* d = opendir("/proc/self/task");
    if (!d) return 0;
    int added = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const uint32_t tid = static_cast<uint32_t>(strtoul(e->d_name, nullptr, 10));
        if (!tid || ai_watch_tid_is_covered(tid)) continue;
        const int n = g_ai_watch_ntids.load(std::memory_order_acquire);
        if (n >= kAiWatchMaxTids) {
            g_ai_watch_uncovered.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const int fd = ai_watch_open_for_tid(byte_addr, tid, log);
        if (fd < 0) continue;
        //: comm, because "which of the five workers" is how the node seat reads the
        //: log and a bare tid does not say that.
        char comm[64] = {0};
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%u/comm", tid);
        if (FILE* cf = fopen(path, "r")) {
            if (fgets(comm, sizeof comm, cf)) {
                char* nl = strchr(comm, '\n');
                if (nl) *nl = 0;
            }
            fclose(cf);
        }
        g_ai_watch_tids[n].tid = tid;
        g_ai_watch_tids[n].fd  = fd;
        g_ai_watch_ntids.store(n + 1, std::memory_order_release);
        ++added;
        if (log) fprintf(log, "[CK3-Bridge][aiwatch] THREAD-ARMED seq=%llu tid=%u "
                              "comm=%s addr=%p covered=%d\n",
                         (unsigned long long)ai_watch_next_seq(), tid,
                         comm[0] ? comm : "?", reinterpret_cast<void*>(byte_addr),
                         n + 1);
    }
    closedir(d);
    return added;
}

//: Kept as a cheap per-tick top-up: a thread created between rescans still covers
//: itself the first time the hook lands on it.
inline bool ai_watch_cover_this_thread(FILE* log) {
    if (!g_ai_watch_on.load(std::memory_order_acquire)) return false;
    const uint32_t tid = static_cast<uint32_t>(syscall(SYS_gettid));
    if (ai_watch_tid_is_covered(tid)) return false;
    const uintptr_t a = g_ai_watch_addr.load(std::memory_order_relaxed);
    if (!a) return false;
    return ai_watch_cover_all_threads(a, log) > 0;
}

//: RE-SCAN on a cadence, so threads created after arming are covered. One opendir
//: every kAiWatchRescanTicks ticks, not per tick.
inline constexpr uint64_t kAiWatchRescanTicks = 256;
inline void ai_watch_rescan_threads(uint64_t tick, FILE* log) {
    if (!g_ai_watch_on.load(std::memory_order_acquire)) return;
    if (tick % kAiWatchRescanTicks != 0) return;
    const uintptr_t a = g_ai_watch_addr.load(std::memory_order_relaxed);
    if (a) ai_watch_cover_all_threads(a, log);
}

inline bool ai_watch_arm(uintptr_t byte_addr, uint32_t char_id,
                         std::atomic<uint64_t>* tick_src, FILE* log) {
    if (!byte_addr) return false;
    g_ai_watch_tick = tick_src;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = [](int s, siginfo_t* si, void* uc) {
        ai_watch_handler(s, si, uc);
    };
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(ai_watch_sig(), &sa, nullptr) != 0) return false;

    struct perf_event_attr pe;
    memset(&pe, 0, sizeof pe);
    pe.type           = PERF_TYPE_BREAKPOINT;
    pe.size           = sizeof pe;
    pe.bp_type        = HW_BREAKPOINT_W;
    pe.bp_addr        = byte_addr;
    pe.bp_len         = HW_BREAKPOINT_LEN_1;
    pe.sample_period  = 1;
    pe.precise_ip     = 2;
    pe.wakeup_events  = 1;
    pe.disabled       = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;

    const int fd = static_cast<int>(
        syscall(__NR_perf_event_open, &pe, 0 /*this thread*/, -1, -1, 0));
    if (fd < 0) {
        if (log) fprintf(log, "[CK3-Bridge][aiwatch] ARM-FAILED seq=%llu errno=%d "
                              "for char %u at %p — no watchpoint, so a zero result "
                              "would be UNINTERPRETABLE\n",
                         (unsigned long long)ai_watch_next_seq(),
                         errno, char_id, reinterpret_cast<void*>(byte_addr));
        return false;
    }
    fcntl(fd, F_SETFL, O_ASYNC | O_NONBLOCK);
    fcntl(fd, F_SETSIG, ai_watch_sig());
    fcntl(fd, F_SETOWN_EX, nullptr);            //: set below via owner_ex
    struct f_owner_ex ow;
    ow.type = F_OWNER_TID;
    ow.pid  = static_cast<pid_t>(syscall(SYS_gettid));
    fcntl(fd, F_SETOWN_EX, &ow);

    g_ai_watch_fd.store(fd, std::memory_order_relaxed);
    g_ai_watch_addr.store(byte_addr, std::memory_order_relaxed);
    g_ai_watch_char.store(char_id, std::memory_order_relaxed);
    g_ai_watch_on.store(true, std::memory_order_release);
    //: THE ARMING THREAD IS ONE OF FIVE. Record it in the table, then cover every
    //: other thread in the process — the single-arm version saw one worker's share
    //: and its zero was unreadable.
    {
        const int n = g_ai_watch_ntids.load(std::memory_order_acquire);
        if (n < kAiWatchMaxTids) {
            g_ai_watch_tids[n].tid = static_cast<uint32_t>(ow.pid);
            g_ai_watch_tids[n].fd  = fd;
            g_ai_watch_ntids.store(n + 1, std::memory_order_release);
        }
    }
    //: seq ON EVERY LINE, including this one. A readout rule of "discard the
    //: CONTROL-FLIP pair after each ARMED" needs ARMED to be placeable in the
    //: same order as the writes it precedes.
    if (log) fprintf(log, "[CK3-Bridge][aiwatch] ARMED seq=%llu char=%u addr=%p tid=%u\n",
                          (unsigned long long)ai_watch_next_seq(),
                     char_id, reinterpret_cast<void*>(byte_addr), ow.pid);
    ai_watch_cover_all_threads(byte_addr, log);
    return true;
}

//: THE ARMING CONTROL, and it does TWO jobs. It proves the watchpoint fires at
//: all, AND it proves 0x28 is the byte the engine writes — because the toggle
//: used here is the engine's own AI_ToggleCharacterAI. Without it a clean zero
//: is indistinguishable from a watchpoint that was never armed, and from an
//: offset that is wrong. It must pass BEFORE the real run.
inline bool ai_watch_selftest(void (*toggle)(void*, unsigned char), void* ai_ctrl,
                              uintptr_t byte_addr, FILE* log) {
    if (!toggle || !ai_ctrl || !byte_addr) return false;
    const unsigned long long before = g_ai_watch_hits.load(std::memory_order_relaxed);
    const uint8_t orig = *reinterpret_cast<volatile uint8_t*>(byte_addr);
    //: THE INSTRUMENT'S OWN WRITES MUST NOT LOOK LIKE THE SUBJECT'S. ck-zgh4
    //: nearly reported these two as data on run 1: the flip and the restore land
    //: real WRITE hits, with a game-image rip because the selftest calls the
    //: ENGINE's toggle, so they are indistinguishable from an organic write
    //: except by origin. Flagged here and emitted under a literal that is NOT a
    //: substring of the organic one — "SELFTEST-WRITE" would still match a grep
    //: for "WRITE tick=", which is the containment trap.
    g_ai_watch_selftesting.store(true, std::memory_order_relaxed);
    toggle(ai_ctrl, orig ? 0 : 1);          //: flip
    toggle(ai_ctrl, orig);                  //: restore
    g_ai_watch_selftesting.store(false, std::memory_order_relaxed);
    const unsigned long long after = g_ai_watch_hits.load(std::memory_order_relaxed);
    const bool ok = after > before;
    //: DRAIN FIRST. The flips this selftest just caused are sitting in the ring;
    //: without this the SELFTEST line is fprintf'd immediately and prints BEFORE
    //: the CONTROL-FLIP lines it is reporting on. ck-zgh4 saw exactly that live:
    //: SELFTEST seq=3 above CONTROL-FLIP seq=1 and seq=2.
    ai_watch_drain(log);
    if (log) fprintf(log, "[CK3-Bridge][aiwatch] SELFTEST seq=%llu %s hits %llu -> %llu "
                          "(proves the watchpoint arms AND that +0x28 is the byte "
                          "the engine's own toggle writes)\n",
                     (unsigned long long)ai_watch_next_seq(),
                     ok ? "PASS" : "FAIL", before, after);
    return ok;
}

//: THE CONTROLLER CAN ALSO BE DESTROYED, WHICH IS WORSE AND LIKELIER (ck-zgh4).
//:
//: Relocation is not the only way the byte goes away. Unlanding, camp
//: dissolution or death can DESTROY a landless adventurer's controller outright,
//: and then there is no new address to re-arm on — so the move path is never
//: reached and nothing disarms. Left alone the sampler keeps dereferencing a
//: stale pointer every tick, and a FREED PAGE CAN BE REUSED: garbage flipping
//: between two reads is reported as a TRANSITION. That is a FALSE POSITIVE in
//: the sampler column, and it reads exactly like the off-thread-writer cell
//: (transition, watchpoint silent — because the fd watches an address nothing
//: writes any more). A null for an instrument reason costs a run; a POSITIVE for
//: an instrument reason sends the hypothesis the wrong way.
//:
//: TERMINAL, and this is the part that is not obvious: the arming block at the
//: top of the tick hook fires whenever `!g_ai_watch_on` and a controlled primary
//: exists. So a plain disarm would SILENTLY RE-ARM on whatever character is
//: primary now — possibly a different one — while hits and transitions keep
//: accumulating in the same global counters, mixing two subjects in one column.
//: The retired latch makes the disarm final; resuming is a second state and
//: wants its own decision, not a side effect.
//: DISARM IS NOT RETIREMENT ANY MORE (ck-6xi6 run-2 recut).
//:
//: It used to set g_ai_watch_retired unconditionally, which was right while the
//: watch followed ONE character for the whole run: that character's controller
//: going away ended the subject. Now the watch follows the SESSION'S PRIMARY, and
//: a character dying is an ordinary event mid-run — ep-03's own path reaches its
//: subject 850 ticks AFTER the first watched character dies. Retiring there would
//: produce a log with ARMED, SELFTEST PASS and CONTROLLER-GONE that measured
//: nothing, which is a null for an instrument reason wearing the shape of a real
//: one. Terminal retirement is now only for a failure that cannot be recovered.
inline void ai_watch_disarm(uint32_t char_id, FILE* log, bool terminal = false) {
    if (!g_ai_watch_on.load(std::memory_order_acquire)) return;
    const uintptr_t armed = g_ai_watch_addr.load(std::memory_order_relaxed);
    g_ai_watch_on.store(false, std::memory_order_release);   //: stops the sampler first
    ai_watch_close_all_tids();
    const int fd = g_ai_watch_fd.exchange(-1, std::memory_order_relaxed);
    if (fd >= 0) close(fd);
    g_ai_watch_addr.store(0, std::memory_order_relaxed);
    g_ai_watch_last.store(0xFF, std::memory_order_relaxed);
    if (terminal) g_ai_watch_retired.store(true, std::memory_order_release);
    //: TWO WHOLE FORMAT STRINGS, one per branch, because the sentences make
    //: OPPOSITE claims about whether anything more will be measured and a %s
    //: would leave neither claim in .rodata for the pre-deploy witness to find.
    if (log && terminal) {
        fprintf(log, "[CK3-Bridge][aiwatch] CONTROLLER-GONE-TERMINAL char=%u "
                     "armed=%p seq=%llu — unrecoverable; DISARMED and RETIRED. "
                     "The watch does not resume for the rest of the run.\n",
                char_id, reinterpret_cast<void*>(armed),
                (unsigned long long)ai_watch_next_seq());
    } else if (log) {
        fprintf(log, "[CK3-Bridge][aiwatch] CONTROLLER-GONE char=%u armed=%p "
                     "seq=%llu — controller destroyed or character no longer "
                     "resolvable; DISARMED. The window up to here is COMPLETE "
                     "for this character, not void: the watch re-arms if the "
                     "session takes another primary.\n",
                char_id, reinterpret_cast<void*>(armed),
                (unsigned long long)ai_watch_next_seq());
    }
}

//: THE CONTROLLER CAN MOVE, AND RUN 2 IS WHERE IT WILL.
//:
//: A hardware watchpoint is armed on ONE ADDRESS. If the engine destroys and
//: recreates a character's AI controller — plausible for a LANDLESS ADVENTURER
//: on landing/unlanding or a camp state change, which is exactly the population
//: run 2 targets — then the live AI_ENABLED byte moves to a new allocation and
//: the watchpoint is left watching FREED MEMORY. Two consequences, and the
//: second is worse than the first:
//:
//:   * the run returns a null for an INSTRUMENT reason indistinguishable from
//:     the subject reason. Run 1's null is only interpretable because the
//:     controller demonstrably did not move under a landed king.
//:   * the per-tick sampler would be READING FREED MEMORY every tick, on a live
//:     game, for two game-years.
//:
//: So the address is re-resolved every tick and a move is LOUD. A re-arm also
//: resets the sampler baseline, because a fresh allocation's byte is not a
//: transition of the old one.
inline void ai_watch_rearm_if_moved(uintptr_t now_addr, uint32_t char_id,
                                    std::atomic<uint64_t>* tick_src, FILE* log) {
    if (!g_ai_watch_on.load(std::memory_order_acquire)) return;
    const uintptr_t armed = g_ai_watch_addr.load(std::memory_order_relaxed);
    if (now_addr == armed) return;
    //: The sentence follows the branch. It used to say "re-arming"
    //: unconditionally while the re-arm is `if (now_addr)` — accurate on every
    //: path reachable today, and a claim about an action the function does not
    //: always take, which a future caller passing 0 would turn into a lie.
    //: seq HERE ABOVE ALL. The contract is "sort by seq, then read", and this is
    //: the line whose meaning is "everything above me is VOID". Without a seq it
    //: has no defined position in the ordering a reader is told to use — ck-zgh4
    //: sorted a live run, had to place it arbitrarily, and it landed next to an
    //: ARMED it long postdates, which would have condemned the first window. MOVED
    //: and SWITCH make OPPOSITE claims about the window above them, so an
    //: unsortable MOVED is worse than a missing line.
    if (log) fprintf(log, "[CK3-Bridge][aiwatch] CONTROLLER-MOVED seq=%llu char=%u "
                          "armed=%p now=%p — the watchpoint was on stale memory; "
                          "%s. Any window before this line is VOID for this "
                          "character.\n",
                     (unsigned long long)ai_watch_next_seq(),
                     char_id, reinterpret_cast<void*>(armed),
                     reinterpret_cast<void*>(now_addr),
                     now_addr ? "re-arming" : "NOT re-arming, no new address");
    //: ALL of them: one fd per covered thread, and the old address is stale
    //: for every one of them.
    ai_watch_close_all_tids();
    const int old_fd = g_ai_watch_fd.exchange(-1, std::memory_order_relaxed);
    if (old_fd >= 0) close(old_fd);
    g_ai_watch_on.store(false, std::memory_order_release);
    g_ai_watch_last.store(0xFF, std::memory_order_relaxed);   //: not a transition
    if (now_addr) ai_watch_arm(now_addr, char_id, tick_src, log);
}

//: Per-tick: sample for the PERIOD, and drain the handler's ring for the WRITER.
//: THE SESSION CHANGED SUBJECT — a DIFFERENT FACT from the same subject's
//: controller moving, and a reader must not have to infer which from context.
//:
//: CONTROLLER-MOVED says the object was reallocated, so any window before it is
//: VOID: values read at the old address were read from freed memory. A SWITCH
//: says the session moved on, so the window before it is COMPLETE for the old
//: character — everything measured there was measured correctly, of that
//: character. Collapsing the two would throw away good data or trust bad data,
//: depending on which way the reader guessed.
//:
//: Callers MUST drain first (see ai_watch_drain) so this line lands after the
//: writes that preceded it rather than before them.
inline void ai_watch_switch(uint32_t old_id, uint32_t new_id, uintptr_t now_addr,
                            AiPrimaryCause cause,
                            std::atomic<uint64_t>* tick_src, FILE* log) {
    const uintptr_t armed = g_ai_watch_addr.load(std::memory_order_relaxed);
    const uint64_t tick = tick_src ? tick_src->load(std::memory_order_relaxed) : 0;
    if (log) fprintf(log, "[CK3-Bridge][aiwatch] PRIMARY-SWITCHED seq=%llu "
                          "tick=%llu char=%u -> %u cause=%s armed=%p now=%p — the "
                          "session changed subject; the window above is COMPLETE "
                          "for %u, and values below are on a DIFFERENT object.\n",
                     (unsigned long long)ai_watch_next_seq(),
                     (unsigned long long)tick, old_id, new_id,
                     ai_primary_cause_name(cause),
                     reinterpret_cast<void*>(armed),
                     reinterpret_cast<void*>(now_addr), old_id);
    //: ALL of them: one fd per covered thread, and the old address is stale
    //: for every one of them.
    ai_watch_close_all_tids();
    const int old_fd = g_ai_watch_fd.exchange(-1, std::memory_order_relaxed);
    if (old_fd >= 0) close(old_fd);
    g_ai_watch_on.store(false, std::memory_order_release);
    g_ai_watch_addr.store(0, std::memory_order_relaxed);
    g_ai_watch_last.store(0xFF, std::memory_order_relaxed);   //: not a transition
    //: NOT RETIRED: this is the whole point of the recut.
    if (now_addr) ai_watch_arm(now_addr, new_id, tick_src, log);
}

//: DRAIN ONLY — no sampling, no transition check. Exists so a caller can flush
//: the handler's pending writes IMMEDIATELY BEFORE emitting a game-thread line,
//: which is what makes the single output stream correctly ordered. Before this,
//: ai_watch_tick ran at the END of the hook while MOVED and GONE printed in the
//: middle, so a write that happened BEFORE a move was printed AFTER it — and
//: "was there a MOVED between the disable and this value" is exactly the question
//: the run exists to answer.
//: ck-zs2x: the last `dropped` total this process has PUBLISHED. The ring's own
//: counter is the producer's; this is the consumer's memory of what a reader has
//: already been told, so each loss is announced once rather than re-announced on
//: every drain.
inline std::atomic<unsigned long long> g_ai_watch_dropped_published{0};

inline void ai_watch_drain(FILE* log) {
    //: THE RAW COUNT FIRST, AND BEFORE THE BATCH IT AFFECTS.
    //:
    //: This ring DROPS on full; it does not overwrite. So a producer outrunning the
    //: consumer leaves no trace in the data at all — the hits that were lost were
    //: never written to a slot, and the surviving hits look like a complete batch.
    //: `dropped` is the ONLY evidence a wrap happened, and until ck-zs2x nothing
    //: published it: a formatter (ai_watch_counters, deleted by ck-arto) built an
//: aiwatch_dropped= string and was called
    //: from nowhere, so the literal was absent from the .so (strings = 0) and a
    //: wrap was invisible by construction.
    //:
    //: Announced BEFORE the lines it affects, not after, so a reader learns the
    //: batch is incomplete before reading it. And published as a RAW COUNT with the
    //: capacity beside it — never as a rate. A rate divides the loss by a window and
    //: hands back a small number for a burst that lost everything; the count and the
    //: window are two facts and a reader is entitled to both. If a rate is ever
    //: added it goes on a LATER line, so it cannot displace this one.
    {
        const unsigned long long total =
            g_ai_watch_ring.dropped.load(std::memory_order_relaxed);
        const unsigned long long seen =
            g_ai_watch_dropped_published.load(std::memory_order_relaxed);
        if (log && total > seen) {
            fprintf(log, "[CK3-Bridge][aiwatch] DROPPED seq=%llu new=%llu total=%llu "
                         "capacity=%d slots=%d\n",
                    (unsigned long long)ai_watch_next_seq(),
                    total - seen, total,
                    AiWatchRing::kCapacity, AiWatchRing::kSlots);
            g_ai_watch_dropped_published.store(total, std::memory_order_relaxed);
        }
    }
    AiWatchHit h;
    while (g_ai_watch_ring.pop(&h)) {
        //: RIP raw. Resolve offline: IMAGE VA = (rip - base) + 0x100000.
        //: TWO WHOLE FORMAT STRINGS, NOT ONE WITH %s FOR THE ORIGIN. A runtime
        //: choice leaves "WRITE" and "CONTROL-FLIP" as bare words with no
        //: literal in .rodata, so `strings` finds neither and the build-witness
        //: check ck-zgh4 runs before a deploy would report the binary wrong.
        //: The log output is identical either way — it is the WITNESS that
        //: differs, which is the same defect as ck-7ndj's twelve runtime error
        //: values: a value assembled at runtime cannot be seen statically.
        //: h.char_id, NOT the live g_ai_watch_char: the subject can change now,
        //: and a hit must be reported under the character it was actually taken
        //: on. Reading the live value here would relabel the old character's
        //: writes as the new one's the moment a switch happened.
        if (!h.from_selftest) {
            const int n = g_ai_watch_ntids.load(std::memory_order_acquire);
            for (int i = 0; i < n && i < kAiWatchMaxTids; ++i)
                if (g_ai_watch_tids[i].tid == h.tid) {
                    g_ai_watch_tid_writes[i].fetch_add(1, std::memory_order_relaxed);
                    break;
                }
        }
        if (log && h.from_selftest) {
            fprintf(log, "[CK3-Bridge][aiwatch] CONTROL-FLIP seq=%llu tick=%llu "
                         "char=%u rip=0x%llx tid=%u value=%u\n",
                    (unsigned long long)h.seq, (unsigned long long)h.tick,
                    h.char_id, (unsigned long long)h.rip, h.tid, h.value);
        } else if (log) {
            fprintf(log, "[CK3-Bridge][aiwatch] WRITE seq=%llu tick=%llu "
                         "char=%u rip=0x%llx tid=%u value=%u\n",
                    (unsigned long long)h.seq, (unsigned long long)h.tick,
                    h.char_id, (unsigned long long)h.rip, h.tid, h.value);
        }
    }
}

inline void ai_watch_tick(uint64_t tick, FILE* log) {
    if (!g_ai_watch_on.load(std::memory_order_acquire)) return;
    const uintptr_t a = g_ai_watch_addr.load(std::memory_order_relaxed);
    if (a) {
        const uint8_t now = *reinterpret_cast<volatile uint8_t*>(a);
        const uint8_t prev = g_ai_watch_last.exchange(now, std::memory_order_relaxed);
        if (prev != 0xFF && prev != now) {
            g_ai_watch_transitions.fetch_add(1, std::memory_order_relaxed);
            //: DID ANY WATCHPOINT ACCOUNT FOR IT? Drain first, so a write in the
            //: ring is already counted, then compare the running total against the
            //: total at the previous transition. No movement means the byte changed
            //: and no covered thread wrote it — so an uncovered thread did.
            ai_watch_drain(log);
            const unsigned long long seen =
                g_ai_watch_hits.load(std::memory_order_relaxed);
            const unsigned long long prev_seen =
                g_ai_watch_writes_at_last_transition.exchange(
                    seen, std::memory_order_relaxed);
            if (seen == prev_seen) {
                g_ai_watch_write_gaps.fetch_add(1, std::memory_order_relaxed);
                if (log) fprintf(log, "[CK3-Bridge][aiwatch] WRITE-GAP seq=%llu "
                                      "tick=%llu char=%u %u -> %u — the byte CHANGED "
                                      "and no covered thread reported a write, so an "
                                      "UNCOVERED thread wrote it. This is the "
                                      "per-task breakpoint's blind spot, counted.\n",
                                 (unsigned long long)ai_watch_next_seq(),
                                 (unsigned long long)tick,
                                 g_ai_watch_char.load(std::memory_order_relaxed),
                                 prev, now);
            }
            if (log) fprintf(log, "[CK3-Bridge][aiwatch] TRANSITION seq=%llu "
                                  "tick=%llu char=%u %u -> %u\n",
                             (unsigned long long)ai_watch_next_seq(),
                             (unsigned long long)tick,
                             g_ai_watch_char.load(std::memory_order_relaxed),
                             prev, now);
        }
    }
    ai_watch_drain(log);
}

//: ck-arto: `ai_watch_counters` WAS HERE AND IS DELETED, because nothing read it.
//:
//: It formatted `aiwatch_hits= aiwatch_transitions= aiwatch_dropped=` into a caller's
//: buffer and had ZERO callers, on every branch in the repository, for its whole life.
//: So the literals never reached the .so and no log line ever carried them — which is
//: exactly why a ring wrap was invisible until ck-zs2x published `dropped` from the
//: drain instead. Exists / is called / produces are three claims; only the first was
//: ever true of it.
//:
//: DELETED RATHER THAN WIRED, ruled by bt-bbd, and the reason is worth keeping: a
//: formatter that LOOKS like the publication path and is not is the same trap ck-zs2x
//: closed. Leaving it would mean the next reader asking "so the counters are
//: published?" and having to re-derive that they are not. The DROPPED line in
//: ai_watch_drain is the publication path; there is now only one candidate for it.
//:
//: THE SIGNED TEXT REFERS TO THIS FUNCTION, so here is the forwarding address. The
//: frozen pre-registration's AMENDMENT 2, COMPONENT A records it by name — that
//: `ai_watch_counters()` formats `aiwatch_dropped=`, is called from nowhere, and has a
//: strings count of 0 — and names its disposition: "Left open and named:
//: ai_watch_counters() still has zero callers — bead ck-arto (delete unless read)". The
//: deploy-31 annotation and the journal say the same, and no citation anywhere
//: describes it as an available diagnostic or a planned instrument. Nothing read it, so
//: ck-arto deleted it, which is the branch of that sentence the signed text anticipated.
//: A reader who arrives from amendment 2 looking for the referent should find this note
//: rather than an absence.
//:
//: Item 7's REGISTERED WITNESS is the DROPPED line in ai_watch_drain, which is
//: untouched. That is the published counter; this was never one.
//:
//: If a per-drain counters line is ever wanted, add it AT a call site with a reader,
//: not as a formatter waiting for one.

}  // namespace bridge
