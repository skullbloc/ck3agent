// Bridge DLL initialization — loaded via LD_PRELOAD into CK3 process.
// Resolves ASLR base, installs hooks, starts IPC server.

#include "thread_registry.h"
#include "claim_event_log.h"   //: ck-jij9
#include "primary_cause.h"   //: ck-6xi6 cause register
#include "fault_site.h"
#include <cerrno>
#include "log_rotate.h"
#include "log_archive.h"
#include "skip_held_heir.h"
#include "ai_enabled_watch.h"
#include "drain_provenance.h"
#include "log_stamp.h"
#include "bridge.h"
#include "reply_record.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "ck3_offsets.h"
#include "lobby_view_hook.h"
#include "name_tables.h"
#include "snapshot.h"
#include "pipeline.h"
#include "drain_action_record_frame.h"   //: ck-td8m
#include "drain_write_verdict.h"
#include "board.h"
#include "instance_paths.h" // ck-qn03
#include "budget_ledger.h"
#include "drain_registry.h"
#include "world_epoch.h"   // ck-hxh7
#include "runtime_resolver.h" // ck-nkyc leg (e): observe_build_id
#include "offset_version.h"   // the supported build, for the strict refusal message
#include "build_id_witness.h"
#include "drain_activation.h"   // L13 D2

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include "os/os.h"   // os::session_pass_* (both platforms, L14 m-16)
#ifndef _WIN32
#include <link.h>
#else
#include <windows.h>
#include <condition_variable>
#include "windows/ck3_offsets_win.h"   // ck3_win::funcs: the MSVC CAIManager vtables (P6)
#endif
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <ucontext.h>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <unordered_map>
#include <string>

// Log to a file that we control, since stderr may be swallowed by pressure-vessel
namespace bridge { FILE* g_logfile = nullptr; }
using bridge::g_logfile;

// Process-lifetime trace gate (CK3_BRIDGE_TRACE=1). Default OFF — gates the
// high-frequency per-query BT_TRACE diagnostics (ck-xm08) that otherwise drive
// ~190MB/session of log growth.
namespace bridge { bool g_trace = false; }

static void blog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void blog(const char* fmt, ...) {
    // ck-55dx: one spelling of the prefix, shared with the reply records so
    // both land on the same timeline. Was inline here, where nothing outside
    // this file could reach it.
    char tbuf[32];
    bridge::monotonic_stamp(tbuf, sizeof tbuf);

    va_list ap;
    va_start(ap, fmt);
    if (g_logfile) {
        fputs(tbuf, g_logfile);
        vfprintf(g_logfile, fmt, ap);
        fflush(g_logfile);
    }
    va_end(ap);
}

namespace bridge {

// --- SIGSEGV crash recovery ---
thread_local sigjmp_buf g_recovery_point;
thread_local bool g_in_protected_region = false;

// ck-e3dp: previous SIGSEGV disposition captured when the safety net is
// RE-ARMED post-startup (see rearm_sigsegv_safety_net below). CK3 installs its
// own crash handler (CrashReporter) during engine startup, AFTER bridge_init's
// dlopen-time sigaction — so until the re-arm, the bridge net is decorative
// (P4.5: zero "safety net triggered" lines ever; both D8 crashes went to
// CrashReporter). When chaining is valid, a fault OUTSIDE a protected region is
// forwarded to this saved handler so engine crash dumps keep working exactly as
// before; only g_in_protected_region faults are stolen for siglongjmp recovery.
static struct sigaction g_prev_sigsegv_action = {};
static volatile bool g_sigsegv_chain_valid = false;

static void bridge_sigsegv_handler(int sig, siginfo_t* info, void* ctx) {
    // ── ck-duv5: THE FAULTING INSTRUCTION, CAPTURED ON *EVERY* PATH ─────────
    // This extraction already existed — for the PROTECTED-region branch only.
    // The UNPROTECTED path, which is the one that takes the process out, logged
    // a fault address and nothing else.
    //
    // WHAT THAT COST, MEASURED RATHER THAN ASSERTED: the arm-A SIGSEGV of
    // 2026-09-21 gave one number, 0x1a. Three seats spent a night on it and
    // produced THREE theories, all of which died — a stale target, a
    // time-of-check/time-of-use window, and an elimination over guarded steps.
    // None died to a fact about the crash; each died to a question about the
    // log. rip_off would have ended it in one line at any point, because it is
    // the Ghidra address of the faulting instruction.
    //
    // SO IT IS HOISTED ABOVE THE BRANCH. The protected path's message is
    // unchanged; the forwarded path now carries the same three numbers. Same
    // asymmetry as ck-cg3b (one call site guarded, its twin not) and as the
    // acceptance query's missing target check: the capability was present, one
    // branch had it, and the branch that needed it most did not.
    void* rip = nullptr;
    if (ctx) {
        auto* uc = static_cast<ucontext_t*>(ctx);
        rip = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
    }

    // ck-65lv: rsp beside rip, ABOVE THE BRANCH, for the same reason the rip
    // extraction was hoisted here — all three paths need it, and a value
    // computed inside one branch is a value the other two cannot have.
    const uintptr_t fault_rsp =
        ctx ? static_cast<uintptr_t>(
                  static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RSP])
            : 0;
    // The unresolved case must not look like an answer: see fault_site.h, which
    // holds the one spelling and is graded by bridge/test/fault_location_probe.cpp.
    auto fmt_rip = [&](char* b, size_t n) {
        return format_fault_site(b, n, rip, g_base_addr);
    };

    // ck-duv5: WHICH THREAD. rip names the instruction; without this the reader
    // supplies the thread from assumption, and the engine's AI workers, the
    // engine's own tick and five bridge threads are all candidates. An absent
    // registry entry prints UNREGISTERED — never a guessed role. See
    // thread_registry.h. g_in_protected_region is thread_local, so the
    // protected/unprotected branch above is a fact about THIS thread only, and
    // saying which thread is what makes that branch interpretable at all.
    const int fault_tid = current_tid();
    auto fmt_thread = [&](char* b, size_t n) {
        return format_fault_thread(b, n, fault_tid);
    };

    if (g_in_protected_region) {
        // SAFETY NET: This should no longer trigger if pointer validation is
        // working correctly. Log an error so we can find missed validation sites.
        // Capture the faulting instruction (RIP) so an engine-call crash can be
        // pinpointed to an exact offset: rip - g_base_addr = the raw code offset
        // (maps to the specific WCC/engine sub-call in Ghidra at offset+0x100000).
        // ── ck-65lv: THE RECOVERED PATH GETS THE FULL CAPTURE TOO ──────────
        // This branch had rip and thread and NO registers. I wired the register
        // dump into the two FATAL paths only, on the reasoning that this one
        // recovers so it can keep its one-liner — and then this is the path that
        // fired, five times, on ck-duv5's first live outing, resolving to
        // GetPrestige with a NULL character. Rich instrumentation on the failure
        // I was hunting, a one-liner on the failure that happened.
        //
        // The return address is what the five captures could not give: the IP
        // was at GetPrestige's first instruction and that function has 16
        // callers, so the instruction was named and the CALLER was not. At an
        // entry IP the return address is at [rsp] — and safe_read_bytes reads it
        // through process_vm_readv, which cannot fault the process even though
        // rsp during a SIGSEGV can be anything.
        char ripbuf[192], thrbuf[160], regbuf[512], retbuf[192];
        if (g_logfile) {
            fprintf(g_logfile,
                "[CK3-Bridge] WARNING: SIGSEGV safety net triggered: fault_addr=%p %s %s"
                " — pointer validation missed this case!\n",
                info ? info->si_addr : nullptr, fmt_rip(ripbuf, sizeof ripbuf),
                fmt_thread(thrbuf, sizeof thrbuf));
            fprintf(g_logfile, "[CK3-Bridge] WARNING: %s %s\n",
                    format_fault_regs(regbuf, sizeof regbuf, ctx),
                    format_frame_word(retbuf, sizeof retbuf, fault_rsp,
                                      &safe_read_bytes, g_base_addr, g_image_end));
            // ck-65lv ladder: the eight words above rsp, each probed on its own.
            char ladbuf[768];
            fprintf(g_logfile, "[CK3-Bridge] WARNING: %s\n",
                    format_stack_ladder(ladbuf, sizeof ladbuf, fault_rsp,
                                        &safe_read_bytes, g_base_addr, g_image_end));
            fflush(g_logfile);
        }
        g_in_protected_region = false;
        siglongjmp(g_recovery_point, 1);
    }
    // Not in a protected region — this fault is NOT ours to absorb (ck-e3dp).
    // Forward to the engine handler (CrashReporter) saved at re-arm time so
    // crash reporting/minidumps behave exactly as if the bridge net were not
    // installed. Fall through to default+re-raise when there is nothing real
    // to chain to (pre-re-arm faults, or the engine registered SIG_DFL).
    // NEVER `return` from here. Returning from a SIGSEGV handler resumes at the
    // faulting instruction, which faults again — an unkillable 100%-CPU spin.
    //
    // 2026-08-22 (ck-26fm): that is exactly what happened, three times. The
    // chained CrashReporter handler RETURNED instead of terminating, we returned
    // too, and CK3's main thread livelocked: fault -> handler -> return -> fault.
    // eu-stack on the hung thread showed the whole story in three frames —
    // bridge_sigsegv_handler over __restore_rt over engine code, pegged at 99.9%.
    //
    // The damage was not the crash, it was hiding it. The IPC thread lives in its
    // own server_loop, so the bridge kept answering every probe perfectly while
    // the simulation was dead; the game clock stopped, no health check noticed,
    // and ck3_game_selfheal could never fire because it waits for the process to
    // die and the process could no longer die. A clean crash was recoverable in
    // ~4 minutes by the existing heal. This turned it into a 12-hour dark stream.
    //
    // So: give the engine handler its chance to dump and exit, and if it hands
    // control back, fall through and die properly.
    if (g_sigsegv_chain_valid) {
        if ((g_prev_sigsegv_action.sa_flags & SA_SIGINFO) &&
            g_prev_sigsegv_action.sa_sigaction != nullptr) {
            g_prev_sigsegv_action.sa_sigaction(sig, info, ctx);
        } else if (!(g_prev_sigsegv_action.sa_flags & SA_SIGINFO) &&
                   g_prev_sigsegv_action.sa_handler != SIG_DFL &&
                   g_prev_sigsegv_action.sa_handler != SIG_IGN) {
            g_prev_sigsegv_action.sa_handler(sig);
        }
        // Reached only if the chained handler returned, i.e. it did NOT resolve
        // the fault and did NOT terminate. Say so loudly — this line is the tell
        // that the engine crashed and the bridge declined to hide it.
        if (g_logfile) {
            char ripbuf[192], thrbuf[160];
            fprintf(g_logfile,
                    "[CK3-Bridge] FATAL: chained SIGSEGV handler returned without "
                    "terminating (fault_addr=%p %s %s) — re-raising with SIG_DFL "
                    "so the process dies and ck3_game_selfheal can recover it. "
                    "Returning here would livelock the game thread (ck-26fm).\n",
                    info ? info->si_addr : nullptr,
                    fmt_rip(ripbuf, sizeof ripbuf),
                    fmt_thread(thrbuf, sizeof thrbuf));
                    // ck-65lv: the frame word on the FATAL paths too. These take the
                    // process out and get ONE shot, so a fatal fault that cannot name
                    // its caller is the exact gap ck-duv5 exists to close. Emitting it
                    // only on the recovered path repeats, in the other direction, the
                    // asymmetry this file's rip hoist was written to end.
                    char regbuf[512], retbuf[192], ladbuf[768];
                    fprintf(g_logfile, "[CK3-Bridge] FATAL: %s %s\n",
                            format_fault_regs(regbuf, sizeof regbuf, ctx),
                            format_frame_word(retbuf, sizeof retbuf, fault_rsp,
                                              &safe_read_bytes, g_base_addr, g_image_end));
                    // ck-65lv ladder: the eight words above rsp, each probed on its own.
                    fprintf(g_logfile, "[CK3-Bridge] FATAL: %s\n",
                            format_stack_ladder(ladbuf, sizeof ladbuf, fault_rsp,
                                                &safe_read_bytes, g_base_addr, g_image_end));
            fflush(g_logfile);
        }
    }
    // ck-duv5: AND THE NO-CHAIN PATH LOGGED NOTHING AT ALL. A fault before the
    // re-arm, or with the engine on SIG_DFL, died completely silently — not even
    // a fault address. That is the worst of the three branches and it was the
    // one nobody had looked at, because it is the one that never fired in an
    // episode anybody diagnosed.
    if (!g_sigsegv_chain_valid && g_logfile) {
        char ripbuf[192], thrbuf[160];
        fprintf(g_logfile,
                "[CK3-Bridge] FATAL: SIGSEGV with NO chained handler "
                "(fault_addr=%p %s %s) — re-raising with SIG_DFL. This is either "
                "a pre-re-arm fault or the engine registered SIG_DFL.\n",
                info ? info->si_addr : nullptr,
                fmt_rip(ripbuf, sizeof ripbuf),
                fmt_thread(thrbuf, sizeof thrbuf));
                // ck-65lv: the frame word on the FATAL paths too. These take the
                // process out and get ONE shot, so a fatal fault that cannot name
                // its caller is the exact gap ck-duv5 exists to close. Emitting it
                // only on the recovered path repeats, in the other direction, the
                // asymmetry this file's rip hoist was written to end.
                char regbuf[512], retbuf[192], ladbuf[768];
                fprintf(g_logfile, "[CK3-Bridge] FATAL: %s %s\n",
                        format_fault_regs(regbuf, sizeof regbuf, ctx),
                        format_frame_word(retbuf, sizeof retbuf, fault_rsp,
                                          &safe_read_bytes, g_base_addr, g_image_end));
                // ck-65lv ladder: the eight words above rsp, each probed on its own.
                fprintf(g_logfile, "[CK3-Bridge] FATAL: %s\n",
                        format_stack_ladder(ladbuf, sizeof ladbuf, fault_rsp,
                                            &safe_read_bytes, g_base_addr, g_image_end));
        fflush(g_logfile);
    }

    // No chained handler — restore default handler and re-raise
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, nullptr);
    raise(SIGSEGV);
}

// ck-e3dp: re-install the bridge SIGSEGV safety net AFTER CK3's startup and
// chain to whatever handler the engine installed in between. bridge_init()'s
// dlopen-time sigaction runs before CK3's own crash handler registration, so
// the engine "wins" the race and every sigsetjmp wrap in the bridge was
// decorative for the whole session (P4.5, probes-round3.md). Called from the
// first on_tick (game fully started) and re-checked periodically from the
// same tick loop so a late or re-registered engine handler (save reload)
// cannot silently displace the net again. Idempotent and cheap (one sigaction
// read when already armed). NOT async-signal code — runs on the game thread.
static void rearm_sigsegv_safety_net(const char* when) {
#ifdef _WIN32
    // Windows: the fault net is the shim's first-position vectored handler
    // (posix_shim.cpp); CK3's crash reporter may have been added in front of it.
    const int rc = ck3_win_rearm_fault_recovery();
    if (strcmp(when, "first_on_tick") == 0 || rc != 0)
        blog("[CK3-Bridge] fault net re-arm (vectored handler, %s): %s\n", when, rc == 0 ? "ok" : "FAILED");
    return;
#endif
    struct sigaction cur = {};
    if (sigaction(SIGSEGV, nullptr, &cur) != 0) {
        blog("[CK3-Bridge] SIGSEGV safety net re-arm FAILED: sigaction read errno=%d (%s)\n",
             errno, when);
        return;
    }
    if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == bridge_sigsegv_handler) {
        // Already ours — armed. Log only on the explicit first-tick call so
        // Verify has a deterministic line either way, without per-check spam.
        if (strcmp(when, "first_on_tick") == 0) {
            blog("[CK3-Bridge] SIGSEGV safety net already armed at %s "
                 "(engine did not displace it); chain=%s\n",
                 when, g_sigsegv_chain_valid ? "valid" : "none");
        }
        return;
    }
    // A different handler is installed (the engine's CrashReporter, or SIG_DFL).
    // Save it for chaining, then take the slot back.
    g_sigsegv_chain_valid = false;  // order matters vs a concurrent fault
    g_prev_sigsegv_action = cur;
    g_sigsegv_chain_valid = true;
    struct sigaction sa;
    sa.sa_sigaction = bridge_sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    // Run on the engine's alternate stack if its handler used one, so a
    // forwarded fault executes in the environment CrashReporter expects.
    if (cur.sa_flags & SA_ONSTACK) sa.sa_flags |= SA_ONSTACK;
    if (sigaction(SIGSEGV, &sa, nullptr) != 0) {
        g_sigsegv_chain_valid = false;
        blog("[CK3-Bridge] SIGSEGV safety net re-arm FAILED: sigaction install errno=%d (%s)\n",
             errno, when);
        return;
    }
    blog("[CK3-Bridge] SIGSEGV safety net RE-ARMED (%s): displaced handler %p saved for "
         "chaining (prev_flags=0x%x, onstack=%d) — protected-region faults recover via "
         "siglongjmp, all others forward to the engine handler\n",
         when,
         (cur.sa_flags & SA_SIGINFO) ? reinterpret_cast<void*>(cur.sa_sigaction)
                                     : reinterpret_cast<void*>(cur.sa_handler),
         (unsigned)cur.sa_flags, (cur.sa_flags & SA_ONSTACK) ? 1 : 0);
}

// --- SIGTRAP handler for INT3-first hook installation ---
// During hook installation, byte 0 is temporarily set to INT3 (0xCC).
// If another thread hits the function during this window, it gets SIGTRAP.
// We simply retry the instruction — by the time the handler returns,
// the hook will be fully installed and byte 0 will be the real opcode.
static void bridge_sigtrap_handler(int /*sig*/, siginfo_t* /*info*/, void* ctx) {
    // The INT3 instruction is 1 byte. The CPU advances RIP past it before
    // delivering the signal. Rewind RIP by 1 so the thread re-executes
    // the same byte, which will now be the real first byte of the hook JMP.
    auto* uc = static_cast<ucontext_t*>(ctx);
    uc->uc_mcontext.gregs[REG_RIP] -= 1;
    // Return from signal handler — thread retries the instruction
}

// --- BUG-8: SIGALRM watchdog for dispatch timeout ---
// Prevents infinite hangs (e.g. invite_to_court_interaction) by aborting
// dispatch after DISPATCH_TIMEOUT_SECS seconds via longjmp.
static constexpr unsigned DISPATCH_TIMEOUT_SECS = 10;

static void bridge_sigalrm_handler(int /*sig*/) {
    if (g_in_protected_region) {
        g_in_protected_region = false;
        siglongjmp(g_recovery_point, 2);  // 2 = timeout (vs 1 = SIGSEGV)
    }
}

#ifdef _WIN32
// --- Windows dispatch watchdog (ADR 0001 P6) ---
// No SIGALRM to deliver into the game thread: one watchdog thread polls the
// deadline and, once it passes, interrupts the thread that armed it through the
// shim (ck3_win_watchdog_interrupt: suspend, check, redirect to siglongjmp with
// 2, the Linux handler's value). Lock-free on purpose: the watched thread may be
// suspended at any instruction, so it must never hold anything this thread
// waits for. The interrupt only lands while the thread runs game or bridge code
// (see the shim); elsewhere the thread is left alone and retried next poll.
static std::atomic<uint64_t> g_ww_gen{0};          // 0 = disarmed; else the arm's generation
static std::atomic<uint64_t> g_ww_next_gen{0};
static std::atomic<unsigned long> g_ww_tid{0};
static std::atomic<const bool*> g_ww_region{nullptr};
static std::atomic<int64_t> g_ww_deadline_ms{0};
static HANDLE g_ww_wake = nullptr;
static uintptr_t g_self_lo = 0, g_self_hi = 0;      // this DLL's image

static int64_t win_now_ms() { return static_cast<int64_t>(GetTickCount64()); }

static void win_watchdog_loop() {
    uint64_t logged_outside = 0;
    for (;;) {
        const uint64_t gen = g_ww_gen.load(std::memory_order_acquire);
        if (!gen) { WaitForSingleObject(g_ww_wake, INFINITE); continue; }
        const int64_t left = g_ww_deadline_ms.load(std::memory_order_acquire) - win_now_ms();
        if (left > 0) { WaitForSingleObject(g_ww_wake, static_cast<DWORD>(left < 50 ? left : 50)); continue; }
        const uintptr_t lo[2] = {g_base_addr, g_self_lo}, hi[2] = {g_image_end, g_self_hi};
        uintptr_t rip = 0;
        // The arm may have been replaced or disarmed since gen was read: the
        // interrupt re-reads nothing, so check the generation again right before.
        if (g_ww_gen.load(std::memory_order_acquire) != gen) continue;
        const int rc = ck3_win_watchdog_interrupt(g_ww_tid.load(), g_ww_region.load(), lo, hi, 2, &rip);
        if (rc == 2) {
            if (logged_outside != gen) {
                logged_outside = gen;
                blog("[CK3-Bridge] Watchdog: deadline passed, tid=%lu outside game code (rip=0x%llx); retrying\n",
                     g_ww_tid.load(), static_cast<unsigned long long>(rip));
            }
            Sleep(20);
            continue;
        }
        // 0 interrupted, 1 already out of the region, -1 failed: this arm is spent.
        uint64_t spent = gen;
        g_ww_gen.compare_exchange_strong(spent, 0);
        blog("[CK3-Bridge] Watchdog: tid=%lu %s (rip=0x%llx)\n", g_ww_tid.load(),
             rc == 0 ? "interrupted (timeout, siglongjmp 2)" : rc == 1 ? "had left the region" : "interrupt FAILED",
             static_cast<unsigned long long>(rip));
    }
}

static void win_watchdog_start() {
    if (g_ww_wake) return;
    g_ww_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&win_watchdog_start), &self) && self) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<uintptr_t>(self) + dos->e_lfanew);
        g_self_lo = reinterpret_cast<uintptr_t>(self);
        g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    }
    std::thread(win_watchdog_loop).detach();
}

static void win_watchdog_arm(unsigned secs) {
    g_ww_gen.store(0, std::memory_order_release);   // no stale arm fires mid-update
    g_ww_tid.store(GetCurrentThreadId());
    g_ww_region.store(&g_in_protected_region);
    g_ww_deadline_ms.store(win_now_ms() + static_cast<int64_t>(secs) * 1000);
    g_ww_gen.store(g_ww_next_gen.fetch_add(1) + 1, std::memory_order_release);
    if (g_ww_wake) SetEvent(g_ww_wake);
}

// ScopedWritable's Windows half (bridge.h): the old protection comes back from
// VirtualProtect itself.
bool win_make_writable(void* addr, size_t len, unsigned long* old_prot) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD prev = 0;
    if (!VirtualProtect(addr, len, (mbi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &prev))
        return false;
    *old_prot = prev;
    return true;
}
void win_restore_protection(void* addr, size_t len, unsigned long old_prot) {
    DWORD prev = 0;
    VirtualProtect(addr, len, static_cast<DWORD>(old_prot), &prev);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
}
#endif

// --- Per-thread SIGALRM timer (replaces process-wide alarm()) ---
// Uses SIGEV_THREAD_ID to guarantee delivery to the game thread,
// preventing the "SIGALRM hits wrong thread" hang bug.
static timer_t g_watchdog_timer = nullptr;
static bool g_watchdog_initialized = false;

void watchdog_init() {
    // ck-duv5: this runs ON the game thread (that is the whole reason the
    // watchdog uses SIGEV_THREAD_ID here), so it is the natural place to record
    // the game thread's identity. The tid was already being read one line down
    // and thrown away after the sigevent — now it is kept.
    register_thread(ThreadRole::GameThread);

    // L13: the game thread can change ONCE — from an AI-tick worker (drains run
    // from on_tick until the session-pass hook is installed, e.g. a save that
    // loaded running) to the session thread. The timer must follow it, or it
    // would interrupt a worker while the session thread hangs unguarded.
    // (Windows arms per calling thread already: nothing to follow.)
#ifndef _WIN32
    static int s_watchdog_tid = 0;
    const int me = static_cast<int>(syscall(SYS_gettid));
    if (g_watchdog_initialized && s_watchdog_tid != me) {
        timer_delete(g_watchdog_timer);
        g_watchdog_initialized = false;
        blog("[CK3-Bridge] Watchdog timer retargeted: tid %d -> %d\n", s_watchdog_tid, me);
    }
    s_watchdog_tid = me;
#endif
    if (g_watchdog_initialized) return;
#ifdef _WIN32
    win_watchdog_start();
    g_watchdog_initialized = true;
    blog("[CK3-Bridge] Watchdog: Windows thread interrupt (tid=%lu; armed per calling thread)\n",
         GetCurrentThreadId());
    return;
#endif

    struct sigevent sev = {};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGALRM;
    sev._sigev_un._tid = static_cast<int>(syscall(SYS_gettid));

    if (timer_create(CLOCK_MONOTONIC, &sev, &g_watchdog_timer) == 0) {
        g_watchdog_initialized = true;
        blog("[CK3-Bridge] Watchdog timer created (SIGEV_THREAD_ID, tid=%d)\n",
             sev._sigev_un._tid);
    } else {
        blog("[CK3-Bridge] ERROR: timer_create failed (errno=%d), falling back to alarm()\n", errno);
    }
}

void watchdog_arm(unsigned secs) {
#ifdef _WIN32
    win_watchdog_arm(secs);
    return;
#endif
    // ck-4ot67: unblock SIGALRM on the calling thread before arming. The engine
    // may have inherited or set a thread-level SIGALRM mask elsewhere during a
    // tick (parallel worker setup, allocator critical sections, etc.). If the
    // signal is blocked when the timer fires, the kernel queues it but the
    // longjmp never runs and the watchdog appears silent. Re-asserting the
    // unblock on every arm is cheap (one syscall) and idempotent.
    sigset_t alrm_mask;
    sigemptyset(&alrm_mask);
    sigaddset(&alrm_mask, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &alrm_mask, nullptr);

    if (g_watchdog_initialized) {
        struct itimerspec its = {};
        its.it_value.tv_sec = secs;
        timer_settime(g_watchdog_timer, 0, &its, nullptr);
    } else {
        alarm(secs);
    }
}

void watchdog_disarm() {
#ifdef _WIN32
    g_ww_gen.store(0, std::memory_order_release);
    return;
#endif
    if (g_watchdog_initialized) {
        struct itimerspec its = {};  // all zeros = disarm
        timer_settime(g_watchdog_timer, 0, &its, nullptr);
    } else {
        alarm(0);
    }
}

uintptr_t g_base_addr = 0;
// ck-65lv: one past the last byte of the CK3 image (max PT_LOAD end), captured
// ONCE at init beside the base so the fault formatters can tell an in-image
// word from a stack or heap address without reading /proc in a handler.
uintptr_t g_image_end = 0;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_agent_mode{false};
std::atomic<uint64_t> g_tick_id{0};
std::atomic<bool> g_strict_mode{false};

void bridge_strict_check_abort(const char* site) {
    if (!g_strict_mode.load(std::memory_order_relaxed)) return;
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge] STRICT: CK3_BRIDGE_STRICT=1 set — aborting on SIGSEGV "
            "in %s (pre-validation missed this case; investigate the bridge log "
            "and extend the relevant validator)\n",
            site ? site : "(unknown)");
        fflush(g_logfile);
    }
    abort();
}

// --- /proc/self/maps validation ---
std::vector<MemRange> g_valid_ranges CK3_EARLY_INIT;
std::shared_mutex g_ranges_mutex;

void refresh_memory_map() {
    std::vector<MemRange> ranges;
    ranges.reserve(256);
#ifdef _WIN32
    // Committed, accessible regions of this process (the /proc/self/maps
    // equivalent), merged where contiguous. PAGE_GUARD and PAGE_NOACCESS
    // regions are not readable and are left out.
    {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t a = 0x10000;
        while (a < 0x7FFFFFFFFFFF && VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t end = start + mbi.RegionSize;
            const bool readable = mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && mbi.Protect != 0;
            if (readable) {
                if (!ranges.empty() && ranges.back().end == start) ranges.back().end = end;
                else ranges.push_back({start, end});
            }
            if (end <= a) break;
            a = end;
        }
        std::unique_lock<std::shared_mutex> lock(g_ranges_mutex);
        g_valid_ranges = std::move(ranges);
        return;
    }
#endif
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            // Merge with previous range if contiguous (reduces search time)
            if (!ranges.empty() && ranges.back().end == start) {
                ranges.back().end = end;
            } else {
                ranges.push_back({start, end});
            }
        }
    }
    fclose(f);
    // Exclusive lock for the single writer (once per tick)
    std::unique_lock<std::shared_mutex> lock(g_ranges_mutex);
    g_valid_ranges = std::move(ranges);
}

// --- Session lifecycle state machine ---
static std::atomic<SessionState> g_session_state{SessionState::DORMANT};
static void* g_last_ai_manager = nullptr;      // detect save reloads (pointer changes)
static uint64_t g_last_tick_id_at_active = 0;   // tick when we last entered ACTIVE
static constexpr int RELOAD_DETECT_TICKS = 3;   // ticks before RELOADING→ACTIVE settles

SessionState get_session_state() {
    return g_session_state.load(std::memory_order_acquire);
}

const char* session_state_string() {
    switch (g_session_state.load(std::memory_order_acquire)) {
        case SessionState::DORMANT:   return "dormant";
        case SessionState::ACTIVE:    return "active";
        case SessionState::RELOADING: return "reloading";
    }
    return "unknown";
}

// Trampoline to the original AI main tick function
static void* g_orig_main_tick = nullptr;
// Track which hook method was used (for proper cleanup in bridge_shutdown)
static bool g_used_inline_hook = false;
// If vtable hook was used, track which slot and where
static uintptr_t g_vtable_addr = 0;
static int g_vtable_tick_slot = -1;
// Second vtable hook (internal vtable for thunk dispatch)
static uintptr_t g_vtable2_addr = 0;
static int g_vtable2_tick_slot = -1;
static void* g_orig_main_tick_thunk = nullptr;
// Whether the active hook is on the internal vtable (this pointer needs +8 adjustment)
static bool g_hooked_internal_vtable = false;

// Check if we're running inside the actual CK3 process (not Steam or a helper).
// Returns true only if /proc/self/exe basename is exactly "ck3".
static bool is_ck3_process() {
#ifdef _WIN32
    // The proxy DLL's identity gate has already matched ck3.exe by CodeView
    // GUID + SizeOfImage before anything here runs; this is the name check only.
    char exe_path[1024] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (n == 0 || n >= sizeof(exe_path)) return false;
    const char* b = exe_path;
    for (const char* q = exe_path; *q; ++q) if (*q == '\\' || *q == '/') b = q + 1;
    return _stricmp(b, "ck3.exe") == 0;
#else
    // Check /proc/self/exe — the most reliable indicator
    char exe_path[512] = {};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        // Match "ck3" at end of path (e.g. .../binaries/ck3)
        const char* basename = strrchr(exe_path, '/');
        if (basename && strcmp(basename + 1, "ck3") == 0) {
            return true;
        }
    }
    return false;
#endif
}

// Forward declaration
static bool is_addr_readable(uintptr_t addr);

// ─────────────────────────────────────────────────────────────────────────
// Game-ready predicate (ck-crm5: defer hooks until game-state is constructed)
//
// GAME_STATE_ROOT holds a pointer-to-pointer: at the fixed base-relative
// address we find a pointer that is null on binary boot / main menu /
// character-select, and becomes non-null once the engine has fully
// constructed the in-game world (at which point hooking CAIManager::MainTick
// is safe). This is the same predicate the engine's own GAME_IS_LOADED_FN
// uses (`cmp qword [GAME_STATE_ROOT], 0`).
// ─────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_main_tick_hooks_installed{false};

static bool is_game_ready() {
    if (g_base_addr == 0) return false;
    uintptr_t pp_addr = g_base_addr + ck3::globals::GAME_STATE_ROOT;
    if (!is_addr_readable(pp_addr)) return false;
    void* gs = *reinterpret_cast<void* volatile*>(pp_addr);
    return gs != nullptr;
}

// Find the base address of the main CK3 executable in memory.
// With ASLR, the binary is loaded at a random base address.
// All our offsets are relative to this base.
static uintptr_t find_binary_base() {
#ifdef _WIN32
    {
        // The main module's base and SizeOfImage (the PE equivalent of the
        // PT_LOAD span); MZ/PE is checked by the identity gate already.
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        if (!base) return 0;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_image_end = base + nt->OptionalHeader.SizeOfImage;
        blog("[CK3-Bridge] PE base: 0x%llx end: 0x%llx (image %llu MB)\n",
             (unsigned long long)base, (unsigned long long)g_image_end,
             (unsigned long long)((g_image_end - base) >> 20));
        return base;
    }
#else
    // Method 1 (most reliable): dl_iterate_phdr gives the actual load address
    struct CallbackData {
        uintptr_t base;
        uintptr_t end;      // ck-65lv: max over PT_LOAD of addr + p_vaddr + p_memsz
        char name[256];
    };
    CallbackData data{0, 0, {}};

    auto image_end_of = [](const struct dl_phdr_info* info) -> uintptr_t {
        uintptr_t end = 0;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD) continue;
            const uintptr_t e = info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
            if (e > end) end = e;
        }
        return end;
    };
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* userdata) -> int {
        auto* d = static_cast<CallbackData*>(userdata);
        auto end_of = [](const struct dl_phdr_info* in) -> uintptr_t {
            uintptr_t end = 0;
            for (int i = 0; i < in->dlpi_phnum; ++i) {
                const auto& ph = in->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD) continue;
                const uintptr_t e = in->dlpi_addr + ph.p_vaddr + ph.p_memsz;
                if (e > end) end = e;
            }
            return end;
        };
        // The main executable typically has an empty name
        if (info->dlpi_name[0] == '\0') {
            d->base = info->dlpi_addr; d->end = end_of(info);
            snprintf(d->name, sizeof(d->name), "(main exe, empty name)");
            return 1; // stop iteration
        }
        // Also match by name
        if (strstr(info->dlpi_name, "/ck3")) {
            d->base = info->dlpi_addr; d->end = end_of(info);
            snprintf(d->name, sizeof(d->name), "%s", info->dlpi_name);
            return 1;
        }
        return 0;
    }, &data);
    (void)image_end_of;

    if (data.base != 0) {
        g_image_end = data.end;
        blog("[CK3-Bridge] dl_iterate_phdr base: 0x%lx end: 0x%lx (%s; image %lu MB)\n",
             data.base, data.end, data.name,
             (unsigned long)((data.end > data.base ? data.end - data.base : 0) >> 20));
    }

    // Method 2: Parse /proc/self/maps — useful for diagnostics
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        uintptr_t first_map = 0, first_rxp = 0;
        uintptr_t first_file_offset = 0;

        while (fgets(line, sizeof(line), maps)) {
            // Match the exact binary name (ends with /ck3 or /ck3\n)
            char* ck3pos = strstr(line, "/ck3");
            if (!ck3pos) continue;
            // Ensure it ends at the binary name (next char is \n, \0, or space)
            char after = ck3pos[4];
            if (after != '\n' && after != '\0' && after != ' ') continue;

            char* dash = strchr(line, '-');
            if (!dash) continue;
            *dash = '\0';
            uintptr_t addr = strtoull(line, nullptr, 16);

            // Parse the file offset (5th field): addr-end perms offset dev inode path
            // After *dash='\0', rest is: end perms offset ...
            char* rest = dash + 1;
            // Skip "end "
            char* space = strchr(rest, ' ');
            if (space) {
                space++; // skip space after end
                // Skip "perms "
                space = strchr(space, ' ');
                if (space) {
                    space++; // skip space after perms
                    uintptr_t file_off = strtoull(space, nullptr, 16);
                    if (first_map == 0) {
                        first_map = addr;
                        first_file_offset = file_off;
                    }
                }
            }

            // Restore dash for permission check
            *dash = '-';
            if (!first_rxp && strstr(line, "r-xp")) {
                first_rxp = addr;
            }
        }
        fclose(maps);

        blog("[CK3-Bridge] maps: first=/0x%lx (file_off=0x%lx), first_rxp=0x%lx\n",
                first_map, first_file_offset, first_rxp);

        // If dl_iterate_phdr gave 0 (shouldn't happen), use maps-based base
        // The correct base = first_map - first_file_offset
        // (because the first mapping may not start at file offset 0)
        if (data.base == 0 && first_map != 0) {
            data.base = first_map - first_file_offset;
            blog("[CK3-Bridge] Using maps-derived base: 0x%lx\n", data.base);
        }
    }

    // Validate: the ELF magic (\x7fELF) should be at the base address
    if (data.base != 0 && is_addr_readable(data.base)) {
        auto* magic = reinterpret_cast<const uint8_t*>(data.base);
        if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
            blog("[CK3-Bridge] ELF magic verified at base 0x%lx\n", data.base);
        } else {
            blog("[CK3-Bridge] WARNING: No ELF magic at base 0x%lx (got %02x %02x %02x %02x)\n",
                    data.base, magic[0], magic[1], magic[2], magic[3]);
        }
    }

    return data.base;
#endif
}

// Safely check if a memory address is readable (mapped) without segfaulting.
static bool is_addr_readable(uintptr_t addr) {
#ifdef _WIN32
    unsigned char b;
    return bridge::os::succeeded(bridge::os::safe_read(&b, addr, 1));
#endif
    // Method 1: write() to /dev/null — returns -1 with EFAULT if unmapped
    int fd = open("/dev/null", O_WRONLY);
    if (fd < 0) return false;
    bool ok = (write(fd, reinterpret_cast<void*>(addr), 1) == 1);
    close(fd);
    return ok;
}

// Forward declarations
void init_action_dispatch();
void init_name_tables();
void init_tls_offsets();
void init_runtime_discovery();
void init_event_handler();
void install_score_hook();
// ck-6kvq.5 — defined in personality.cpp (declared in action_dispatch_internal.h,
// which this TU deliberately does not pull in; see the note below).
void install_steering_purge_hook();
void map_graph_init();   // map_graph.cpp — mmaps the baked ck3map.bin (self-disables if absent)

// From dispatch_router.cpp / action_dispatch_internal.h
void* get_controlled_character();
bool is_sentinel(void* ptr);
extern fn_char_is_alive g_char_is_alive;

// Helpers used by the dead-ruler control-handoff check (defined in
// dispatch_router.cpp / query_handlers.cpp). Narrow forward decls so we don't
// pull the whole action_dispatch_internal.h into the init TU.
void*     lookup_character(uint32_t char_id);
uintptr_t lookup_title_obj(uint32_t title_id);
uintptr_t resolve_title_realm(uintptr_t char_addr);
uint32_t  char_get_id(void* character);
int       get_title_tier(uintptr_t title_ptr);
uint32_t  get_player_character_id();   // rc2 B6: PLAYER_CHAR_ID (dispatch_router.cpp)
// TASK B: monthly telemetry emitter (defined in query_handlers.cpp).
void      emit_monthly_telemetry(int32_t raw_date);
extern fn_get_ai_controller g_get_ai_ctrl;
extern fn_ai_char_toggle    g_ai_char_toggle;

// Guard against double-fire when both vtables are hooked.
// The game dispatches MainTick through ONE vtable per tick, but we hook both
// to be safe. If somehow both fire, the second one just calls through to the
// original without running our hook logic.
// static std::atomic<bool> g_ontick_running{false};  // unused in pass-through test

// ── Deferred-write verdict contract for Mechanism-B (g_msg_queue) writes ──────
// (re/findings/re_deferred_write_verdicts.md §B.3) After each drained WRITE (and
// on the crash / empty-reply branches) store a consumed Intent {request_id,
// executed/rejected, rejection_reason} in the completed ring the Mechanism-A poll
// surfaces read, so query_intents / query_dispatch_result{dispatch_id} /
// inspect(pending) resolve every deferred write. The record and the decision are
// drain_write_verdict.h's and snapshot.cpp's drain_records_write_verdict: a
// game-thread READ stores nothing.
static void record_b_write_verdict(const char* action_name, const char* req_id,
                                   uint32_t actor_id, const char* reply_json) {
    bridge::record_drain_write_verdict(
        bridge::drain_records_write_verdict(action_name), action_name, req_id,
        actor_id, reply_json,
        [](const Intent& rec) { g_intent_queue.store_completed(rec); });
}

#ifdef _WIN32
// The game's MainTick, from the +8 vtable slot (install_main_tick_hooks), and
// on_tick's orig() for it: MSVC's MainTick takes this = object+8.
static void* g_win_main_tick = nullptr;
static void win_main_tick_orig(void* ai_manager) {
    reinterpret_cast<void(*)(void*)>(g_win_main_tick)(
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ai_manager) + 8));
}
#endif

// Thunk wrapper for internal vtable hook (Linux), and the +8 vtable hook (Windows).
// Adjusts `this` by -8 (secondary base -> real CAIManager) then delegates.
void on_tick_internal(void* secondary_base) {
    void* ai_manager = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(secondary_base) - 8);
    on_tick(ai_manager);
}

// ─────────────────────────────────────────────────────────────────────────
// Install the main-tick vtable hooks. Separated from bridge_init() so it
// can be invoked from a deferred polling thread (ck-crm5): installing at
// dlopen was causing CK3 to crash on boot, because the engine dispatches
// through the patched vtable slot during early initialization against an
// AI manager whose backing state isn't fully constructed yet.
//
// Returns true if at least one vtable was hooked, false otherwise.
// Writes all log output via blog(); safe to call from a background thread
// since the vtable swap uses /proc/self/mem (atomic 8-byte pointer write).
// ─────────────────────────────────────────────────────────────────────────
static bool install_main_tick_hooks() {
#ifdef _WIN32
    {
        // ADR 0001 P6. MSVC puts MainTick in the vtable of CAIManager's +8
        // subobject (the base that introduced it), slot MAIN_TICK_SLOT, called
        // with this = object+8; there is no thunk and no slot in the +0 vtable
        // (ck3_offsets_win.h funcs). One swap: the slot -> on_tick_internal,
        // which takes 8 off as it does for the Linux thunk. on_tick's orig() is
        // win_main_tick_orig, which adds the 8 back for the game's function.
        namespace w = ck3_win::funcs;
        const uintptr_t vt = g_base_addr + w::CAIMAN_VTABLE_SUB8;
        auto* slots = reinterpret_cast<uintptr_t*>(vt);
        const uintptr_t expected = g_base_addr + w::AI_MANAGER_MAIN_TICK;
        const size_t slot = w::MAIN_TICK_SLOT;
        blog("[CK3-Bridge] Installing tick hook (Windows): CAIManager +8 vtable at 0x%llx slot %zu, "
             "MainTick 0x%llx\n", (unsigned long long)vt, slot, (unsigned long long)expected);
        if (!is_addr_readable(vt + slot * sizeof(uintptr_t))) {
            blog("[CK3-Bridge] ERROR: the +8 vtable is not readable; no hook\n");
            return false;
        }
        const uintptr_t cur = slots[slot];
        if (cur != expected) {
            // Not this build's layout, or already swapped by someone: refuse
            // rather than chain to an unknown target.
            blog("[CK3-Bridge] ERROR: +8 vtable[%zu] = 0x%llx, not MainTick; no hook\n", slot,
                 (unsigned long long)cur);
            return false;
        }
        g_win_main_tick = reinterpret_cast<void*>(cur);
        g_orig_main_tick = reinterpret_cast<void*>(&win_main_tick_orig);
        const uintptr_t new_val = reinterpret_cast<uintptr_t>(&on_tick_internal);
        if (!write_to_readonly(&slots[slot], &new_val, sizeof(uintptr_t)) || slots[slot] != new_val) {
            blog("[CK3-Bridge] ERROR: +8 vtable[%zu] swap failed; no hook\n", slot);
            g_orig_main_tick = nullptr;
            return false;
        }
        // Restored by bridge_shutdown's internal-vtable branch.
        g_vtable2_addr = vt;
        g_vtable2_tick_slot = static_cast<int>(slot);
        g_orig_main_tick_thunk = g_win_main_tick;
        g_hooked_internal_vtable = true;
        g_used_inline_hook = false;
        g_main_tick_hooks_installed.store(true, std::memory_order_release);
        blog("[CK3-Bridge] Tick hook installed (Windows): +8 vtable[%zu] (orig=%p -> on_tick_internal)\n",
             slot, g_win_main_tick);
        return true;
    }
#endif
    uintptr_t vtable_addr = bridge::game_fn(ck3::funcs::CAIMAN_VTABLE);
    uintptr_t ivtable_addr = bridge::game_fn(ck3::funcs::CAIMAN_INTERNAL_VTABLE);
    auto* vtable = reinterpret_cast<uintptr_t*>(vtable_addr);
    auto* ivtable = reinterpret_cast<uintptr_t*>(ivtable_addr);
    uintptr_t expected_tick = bridge::game_fn(ck3::funcs::AI_MANAGER_MAIN_TICK);
    uintptr_t expected_thunk = bridge::game_fn(ck3::funcs::AI_MANAGER_MAIN_TICK_THUNK);

    blog("[CK3-Bridge] Installing tick hook via DUAL vtable swap (deferred until game-ready)...\n");
    blog("[CK3-Bridge]   primary vtable at 0x%lx, internal vtable at 0x%lx\n",
         vtable_addr, ivtable_addr);
    blog("[CK3-Bridge]   looking for MainTick=0x%lx, Thunk=0x%lx\n",
         expected_tick, expected_thunk);

    // --- Scan primary vtable ---
    int tick_slot = -1;
    if (is_addr_readable(vtable_addr)) {
        for (int i = 0; i < 20; i++) {
            uintptr_t val = vtable[i];
            if (val == expected_tick) { tick_slot = i; break; }
        }
        if (tick_slot >= 0) {
            blog("[CK3-Bridge]   primary[%d] matches MainTick\n", tick_slot);
        }
    } else {
        blog("[CK3-Bridge]   WARNING: primary vtable not readable!\n");
    }

    // --- Scan internal vtable ---
    int ithunk_slot = -1;
    if (is_addr_readable(ivtable_addr)) {
        for (int i = 0; i < 14; i++) {
            uintptr_t val = ivtable[i];
            if (val == expected_thunk) { ithunk_slot = i; break; }
        }
        if (ithunk_slot >= 0) {
            blog("[CK3-Bridge]   internal[%d] matches Thunk\n", ithunk_slot);
        }
    } else {
        blog("[CK3-Bridge]   WARNING: internal vtable not readable!\n");
    }

    if (tick_slot < 0 && ithunk_slot < 0) {
        blog("[CK3-Bridge] ERROR: Neither MainTick nor Thunk found in either vtable!\n");
        return false;
    }

    // --- Hook primary vtable (MainTick directly) ---
    if (tick_slot >= 0) {
        g_orig_main_tick = reinterpret_cast<void*>(vtable[tick_slot]);
        uintptr_t new_val = reinterpret_cast<uintptr_t>(&on_tick);
        if (!write_to_readonly(&vtable[tick_slot], &new_val, sizeof(uintptr_t))) {
            blog("[CK3-Bridge] ERROR: primary vtable write_to_readonly failed, falling back to ScopedWritable\n");
            ScopedWritable sw(reinterpret_cast<void*>(&vtable[tick_slot]), sizeof(uintptr_t));
            vtable[tick_slot] = new_val;
        }
        if (vtable[tick_slot] != reinterpret_cast<uintptr_t>(&on_tick)) {
            blog("[CK3-Bridge] ERROR: primary vtable swap failed!\n");
            return false;
        }
        g_vtable_addr = vtable_addr;
        g_vtable_tick_slot = tick_slot;
        blog("[CK3-Bridge] Primary vtable[%d] hooked (orig=%p -> on_tick)\n",
             tick_slot, g_orig_main_tick);
    }

    // --- Hook internal vtable (Thunk) ---
    if (ithunk_slot >= 0) {
        g_orig_main_tick_thunk = reinterpret_cast<void*>(ivtable[ithunk_slot]);
        uintptr_t new_ival = reinterpret_cast<uintptr_t>(&on_tick_internal);
        if (!write_to_readonly(&ivtable[ithunk_slot], &new_ival, sizeof(uintptr_t))) {
            blog("[CK3-Bridge] ERROR: internal vtable write_to_readonly failed, falling back to ScopedWritable\n");
            ScopedWritable sw(reinterpret_cast<void*>(&ivtable[ithunk_slot]), sizeof(uintptr_t));
            ivtable[ithunk_slot] = new_ival;
        }
        if (ivtable[ithunk_slot] != reinterpret_cast<uintptr_t>(&on_tick_internal)) {
            blog("[CK3-Bridge] ERROR: internal vtable swap failed!\n");
            if (tick_slot < 0) return false;
        } else {
            g_vtable2_addr = ivtable_addr;
            g_vtable2_tick_slot = ithunk_slot;
            blog("[CK3-Bridge] Internal vtable[%d] hooked (orig=%p -> on_tick_internal)\n",
                 ithunk_slot, g_orig_main_tick_thunk);
        }

        if (tick_slot < 0) {
            g_orig_main_tick = reinterpret_cast<void*>(expected_tick);
            g_hooked_internal_vtable = true;
            blog("[CK3-Bridge] Using internal vtable as primary dispatch path "
                 "(orig_main_tick=0x%lx)\n", expected_tick);
        }
    }

    g_used_inline_hook = false;
    g_main_tick_hooks_installed.store(true, std::memory_order_release);
    blog("[CK3-Bridge] Tick hook installed: primary=%s internal=%s\n",
         tick_slot >= 0 ? "YES" : "no",
         ithunk_slot >= 0 ? "YES" : "no");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// L13 D1: THE SESSION-PASS HOOK — where the bridge's game-thread work runs.
//
// re/findings/re_frame_update_hook.md §1-§3, §6. CK3 executes every engine
// command, the daily tick included, on one dedicated SESSION thread:
// pdx_thread_run_loop calls the function pointer at session+0x78
// (CSession_ThreadLoop) on every pass; the loop trylocks session+0x260, runs
// ProcessCommandQueue, unlocks, and sleeps 5 ms when it executed nothing. It
// runs whether or not the game is paused (the pause flag gates only the
// main-thread clock that ENQUEUES the daily tick), so this is the one point
// that is both pause-proof and serial with the whole daily tick, its parallel
// AI workers included (CAIManager::MainTick, where on_tick runs, is one job of
// that tick's parallel-for).
//
// The hook is a DATA-POINTER swap: the slot is in .bss (RW), and the loop
// re-reads it on every pass (`call [rbx]` at image 0x0824d9c1), so a swap takes
// effect on the next pass and writing the original back un-hooks it. The
// wrapper calls the engine's pass first, then, under the SAME mutex and the
// SAME execute-commands predicate the engine uses, runs
// drain_game_thread_work(). Contention (the main thread holds the mutex during
// a load or hot-join) skips the drain for that pass; it is never waited for.
//
// Refuse rather than chain to an unknown target: the slot must hold
// funcs::SESSION_THREAD_LOOP, its argument must be the session itself, and the
// thread must have been started (+0x408 == 1). The installer polls until the
// thread exists (the session thread may not exist at the main menu, finding §9
// item 3) and refuses for good on a value it does not recognise. Without the
// hook (refused) on_tick runs drain_game_thread_work() after the AI tick, as
// before L13, and the bridge says which in its log.
//
// ONE CELL FOR BOTH PLATFORMS (L14 m-16): os::session_pass_install / _remove /
// _live (src/os/session_pass.cpp; tests test_os_linux_session_pass and, under
// wine, test_os_win_session_pass). The cell owns the swap (a compare-exchange),
// the remove (a compare-exchange back that leaves a slot it no longer owns), the
// in-flight count and its bounded wait; this file gives it the layout and the
// work. The layout is ck3::dispatch_mgr's on both builds; the lock primitive is
// the platform's: on Linux a pthread mutex at +0x260, on Windows
// (re/findings/windows/re_session_pass_hook_win.md) an SRWLOCK at +0x2A0 that the
// engine try-acquires exclusively, with the slot at +0xB8, the latch +0x3F0 and
// execute-commands +0x3F3.
// ─────────────────────────────────────────────────────────────────────────
bool session_pass_hook_live() {
    bool live = false;
    return bridge::os::succeeded(bridge::os::session_pass_live(&live)) && live;
}

// Outcome of one install attempt: NotYet (the session thread does not exist
// yet; poll again), Installed, or Refused (a value this build does not
// recognise; never retried).
enum class SessionHookInstall { NotYet, Installed, Refused };

static SessionHookInstall install_session_pass_hook() {
#ifdef _WIN32
    // The layout comes from the generated ck3::names (the code path Linux reads),
    // and each one is checked against the Windows header's own entry: a field the
    // generator fell back on (its Linux value, or a poisoned RVA) fails the build
    // here instead of swapping a slot this build does not have.
    namespace wg = ck3_win::globals;
    namespace wf = ck3_win::funcs;
    namespace wl = ck3_win::struct_layout;
    static_assert(ck3::globals::COMMAND_DISPATCH_MGR == wg::COMMAND_DISPATCH_MGR, "session global");
    static_assert(ck3::globals::SESSION_THREAD_FN_SLOT == wg::SESSION_THREAD_FN_SLOT, "slot");
    static_assert(ck3::funcs::SESSION_THREAD_LOOP == wf::SESSION_THREAD_LOOP, "loop fn");
    static_assert(ck3::dispatch_mgr::THREAD_FN == wl::SESSION_THREAD_FN, "THREAD_FN");
    static_assert(ck3::dispatch_mgr::THREAD_ARG == wl::SESSION_THREAD_ARG, "THREAD_ARG");
    static_assert(ck3::dispatch_mgr::THREAD_STARTED == wl::SESSION_THREAD_STARTED, "THREAD_STARTED");
    static_assert(ck3::dispatch_mgr::EXECUTE_COMMANDS == wl::SESSION_EXECUTE_COMMANDS, "EXECUTE_COMMANDS");
    static_assert(ck3::dispatch_mgr::EXEC_MUTEX == wl::SESSION_EXEC_MUTEX, "EXEC_MUTEX");
    // CSession_ThreadLoop: TryAcquireSRWLockExclusive / ReleaseSRWLockExclusive
    // around ProcessCommandQueue (finding §3).
    constexpr bridge::os::SessionLock kLock = bridge::os::SessionLock::srwlock_exclusive;
    constexpr const char* kLockName = "SRWLOCK";
#else
    // CSession_ThreadLoop: pthread_mutex_trylock / unlock around ProcessCommandQueue.
    constexpr bridge::os::SessionLock kLock = bridge::os::SessionLock::pthread_mutex;
    constexpr const char* kLockName = "mutex";
#endif
    static_assert(ck3::globals::SESSION_THREAD_FN_SLOT ==
                  ck3::globals::COMMAND_DISPATCH_MGR + ck3::dispatch_mgr::THREAD_FN,
                  "the hook slot is session+THREAD_FN");
    const uintptr_t session = g_base_addr + ck3::globals::COMMAND_DISPATCH_MGR;
    const bridge::os::SessionPassLayout layout{
        session,
        ck3::dispatch_mgr::THREAD_FN,
        ck3::dispatch_mgr::THREAD_ARG,
        ck3::dispatch_mgr::THREAD_STARTED,
        ck3::dispatch_mgr::EXECUTE_COMMANDS,
        ck3::dispatch_mgr::EXEC_MUTEX,
        kLock,
        bridge::game_fn(ck3::funcs::SESSION_THREAD_LOOP),
    };
    bridge::os::SessionPassReport rep{};
    const bridge::os::Result r = bridge::os::session_pass_install(layout, &drain_game_thread_work, &rep);
    if (rep.state == bridge::os::SessionPassState::not_yet) return SessionHookInstall::NotYet;
    if (!bridge::os::succeeded(r) || rep.state != bridge::os::SessionPassState::installed) {
        blog("[CK3-Bridge] Session-pass hook: REFUSED — slot 0x%llx holds 0x%llx "
             "(expected CSession_ThreadLoop 0x%llx), arg 0x%llx (expected the session "
             "0x%llx): %s. Game-thread work drains from the AI tick\n",
             (unsigned long long)rep.slot, (unsigned long long)rep.slot_value,
             (unsigned long long)layout.expected_fn, (unsigned long long)rep.arg_value,
             (unsigned long long)session, r.detail);
        return SessionHookInstall::Refused;
    }
    blog("[CK3-Bridge] Session-pass hook installed: slot 0x%llx (session 0x%llx + 0x%zx) "
         "0x%llx -> session_pass wrapper 0x%llx; drain under %s 0x%llx when "
         "execute_commands (+0x%zx) == 1\n",
         (unsigned long long)rep.slot, (unsigned long long)session, layout.fn_off,
         (unsigned long long)rep.slot_value, (unsigned long long)rep.wrapper, kLockName,
         (unsigned long long)(session + layout.lock_off), layout.execute_off);
    return SessionHookInstall::Installed;
}

// Restore the slot (only if it still holds the wrapper), then wait (bounded)
// until no thread is inside the wrapper: the cell's remove.
static void remove_session_pass_hook() {
    if (!session_pass_hook_live()) return;
    int inflight = 0;
    const bridge::os::Result r = bridge::os::session_pass_remove(&inflight);
    blog("[CK3-Bridge] Session-pass hook removed: %s (in-flight %d)%s%s\n",
         bridge::os::succeeded(r) ? "slot restored" : "NOT clean", inflight,
         bridge::os::succeeded(r) ? "" : ": ", r.detail);
}

void bridge_init() {
    // Step 0: Check if we're actually in the CK3 process.
    // With LD_PRELOAD, we load into Steam and every child process.
    // Only initialize if we're in the actual CK3 binary.
    if (!is_ck3_process()) {
        return;
    }

    // ck-qn03 (R12): resolve THIS instance's private paths before anything is
    // opened. On the default instance every answer is the historical literal,
    // so a bridge launched the old way behaves exactly as it always did.
    bridge::instance_paths_init();
    // rc2 B1: 0600, no symlink follow, fresh inode. Not opened at all when the
    // private dir failed verification; the refusal goes to stderr instead.
    if (bridge::instance_private_dir_ok()) {
        g_logfile = bridge::instance_open_private_log(bridge::instance_bridge_log(), true);
    } else {
        fprintf(stderr, "[CK3-Bridge] instance refused: %s (no log, no socket)\n",
                bridge::instance_private_dir_error());
    }
    blog("[CK3-Bridge][instance] id=%s layout=%s socket=%s telemetry=%s log=%s%s%s\n",
         bridge::instance_id(), bridge::instance_layout(), bridge::instance_socket_path(),
         bridge::instance_telemetry_dir(), bridge::instance_bridge_log(),
         bridge::instance_id_was_refused()
             ? "  (CK3_INSTANCE_ID REFUSED: not a slug — nothing bound)" : "",
         bridge::instance_layout_was_refused()
             ? "  (CK3_INSTANCE_LAYOUT REFUSED: want user|legacy — nothing bound)" : "");

    // ck-6kvq.9 (G11): resolve pipeline_mode ONCE, from CK3_PIPELINE_MODE.
    // DEFAULT legacy — a bridge built with the flush loop in it and no env var
    // set must be indistinguishable from the pre-change bridge.
    bridge::pipeline_init_from_env();
    // rc2 B6: the registry refuses the human player's character on every bind
    // path (claim, swap, succession handoff). It reads PLAYER_CHAR_ID lazily.
    bridge::g_session_registry.set_player_character_source(
        [] { return bridge::get_player_character_id(); });
    // ck-6kvq.10 (§2.7): resolve the EPISODE policy — mode, id, and the two
    // unavailability numbers. FREE PLAY IS THE DEFAULT, and a free-play bridge
    // behaves exactly as the pre-change one: no claim pin, no episode abort on a
    // pulse leak, and the covariate is recorded but reads nothing back.
    bridge::pipeline_episode_init_from_env();

    // ck-6kvq.13 (R6/G7): resolve the per-episode decision-budget ceiling ONCE,
    // from CK3_BUDGET_PER_AGENT_PER_YEAR. DEFAULT no cap — record everything,
    // refuse nothing — because the ceiling (1.5 x the pilot median, design
    // §5.3) does not exist until the pilot episodes have run.
    bridge::budget_init_from_env();

    // ck-6kvq.12 (R4/G5): resolve the ARM and open this episode's board. MUST
    // run after pipeline_episode_init_from_env() — it reads the arm the episode
    // policy just parsed. DEFAULT ABSENT: with no CK3_EPISODE_ARM=b the board
    // is disabled, its two verbs are not in the registry, and a free-play or
    // arm-A bridge has no channel at all (decision G5, option A2).
    bridge::board_init_from_env();
    // ck-6kvq.5: the steering-purge hook.  Installed HERE, explicitly, rather
    // than from a static initialiser — dll_init() runs at .init_array[1] and a
    // constructor-installed hook may not exist yet when the reader thread first
    // touches the registry (ck-22n3 / ck-xqol, the whole init-order bug class).
    bridge::install_steering_purge_hook();

    // Trace gate: per-query BT_TRACE diagnostics (ck-xm08) only emit when
    // CK3_BRIDGE_TRACE=1. Read once at init; toggling mid-run is a no-op.
    {
        const char* e = getenv("CK3_BRIDGE_TRACE");
        bridge::g_trace = (e && e[0] == '1' && e[1] == '\0');
    }

    blog("[CK3-Bridge] Initializing (PID %d)...\n", getpid());

    // Strict mode (ck-nr51.4): if CK3_BRIDGE_STRICT=1, sigsetjmp catch paths
    // abort() instead of swallowing — turning previously-silent pointer
    // validation misses into crash signals for test runs. Read once at init
    // and never re-checked, so toggling the env var mid-run is a no-op.
    {
        const char* strict_env = getenv("CK3_BRIDGE_STRICT");
        if (strict_env && strict_env[0] == '1' && strict_env[1] == '\0') {
            g_strict_mode.store(true, std::memory_order_relaxed);
            blog("[CK3-Bridge] STRICT mode enabled (CK3_BRIDGE_STRICT=1) — "
                 "SIGSEGV inside sigsetjmp-protected regions will abort\n");
        }
    }

    // Install SIGSEGV handler for crash recovery in protected regions
#ifdef _WIN32
    // Windows: protected-region faults recover through the shim's first-position
    // vectored handler; there is no SIGTRAP (no INT3-first hooks before P6) and
    // no SIGALRM watchdog (see watchdog_init).
    if (ck3_win_install_fault_recovery() == 0) {
        blog("[CK3-Bridge] Fault net installed (vectored handler, first position)\n");
    } else {
        blog("[CK3-Bridge] ERROR: fault net (vectored handler) not installed; refusing to start\n");
        return;
    }
#else
    {
        struct sigaction sa;
        sa.sa_sigaction = bridge_sigsegv_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
        blog("[CK3-Bridge] SIGSEGV handler installed\n");
    }

    // Install SIGTRAP handler for INT3-first hook installation safety
    {
        struct sigaction sa;
        sa.sa_sigaction = bridge_sigtrap_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &sa, nullptr);
        blog("[CK3-Bridge] SIGTRAP handler installed (INT3-first hook safety)\n");
    }

    // Install SIGALRM handler for dispatch timeout watchdog (BUG-8)
    {
        struct sigaction sa;
        sa.sa_handler = bridge_sigalrm_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGALRM, &sa, nullptr);
        blog("[CK3-Bridge] SIGALRM watchdog installed (%us timeout)\n", DISPATCH_TIMEOUT_SECS);
    }
#endif

    // Step 1: Find ASLR base and determine Ghidra image base offset
    g_base_addr = find_binary_base();
    if (g_base_addr == 0) {
        blog("[CK3-Bridge] ERROR: Could not find CK3 binary base address\n");
        return;
    }
    blog("[CK3-Bridge] Binary base: 0x%llx\n", static_cast<unsigned long long>(g_base_addr));

    // The game-function pointers the action handlers call (g_char_is_alive,
    // g_get_ai_ctrl, ...): address arithmetic only, so set them here, before the
    // IPC listener opens. Set on the first tick alone, a claim made while the
    // loaded game was still paused called a null pointer (P6 smoke4:
    // set_controlled_character -> memory_access_fault at tick 0, either platform).
    // The first-tick call below repeats it, harmlessly.
    init_action_dispatch();

    // Step 2: Verify we're in the right binary
    // Use safe read: check that game_state_root address is accessible
    blog("[CK3-Bridge] Step 2: Verifying binary...\n");

    // ck-nkyc leg (e). WHICH CK3 BINARY IS THIS, OBSERVED RATHER THAN ASSUMED.
    // The offsets in ck3_offsets.h are valid against exactly one binary, and
    // until this call nothing in a sealed bundle recorded which one ran: the
    // code that reads the ELF BuildID lives in runtime_resolver.cpp, and its
    // only caller — run_resolver() — is never invoked by anything in this
    // library (confirmed against real bridge.logs, which carry the three Zydis
    // discovery lines and no resolver banner). So the check existed and had
    // never executed. It publishes into bridge::build_id_witness(), which the
    // handshake reports as `observed_build_id`; it does NOT abort, because the
    // refusal belongs to the driver at episode start.
    ck3_resolver::observe_build_id(g_base_addr);

    // RELEASE BEHAVIOUR. The offsets in ck3_offsets.h are valid against exactly
    // one binary (offset_version.h). When strictness is in force — the CMake
    // option CK3_STRICT_BUILD_ID, compiled ON in public release builds and OFF
    // in the research checkout, overridable either way by the env var of the
    // same name — and the loaded binary is not that one, REFUSE: no hooks, no
    // IPC server, no memory read through offsets that belong to another build.
    // The research checkout keeps warn-and-continue because its refusal lives
    // in the episode driver (ck-nkyc), which can say which leg failed; the
    // witness still records the mismatch for the handshake either way.
    {
        const bridge::BuildIdWitness& w = bridge::build_id_witness();
        if (w.strict_effective && w.status != bridge::BuildIdCheck::MATCH) {
            const char* found = w.observed_hex.empty() ? "<no ELF BuildID found>"
                                                       : w.observed_hex.c_str();
            blog("[CK3-Bridge] REFUSED: unsupported CK3 build.\n");
            blog("[CK3-Bridge]   this bridge supports CK3 %s (%s), BuildID %s\n",
                 ck3_version::GAME_VERSION, ck3_version::GAME_PLATFORM,
                 ck3_version::BUILD_ID_HEX);
            blog("[CK3-Bridge]   the running binary has BuildID %s\n", found);
            blog("[CK3-Bridge]   nothing was hooked and no socket was opened. "
                 "Install the ck3agent release matching your CK3 version, or run "
                 "with CK3_STRICT_BUILD_ID=0 to continue unsupported (may crash "
                 "or misread the game).\n");
            fprintf(stderr, "[CK3-Bridge] REFUSED: unsupported CK3 build (have %s, "
                    "need %s = CK3 %s). See the bridge log.\n",
                    found, ck3_version::BUILD_ID_HEX, ck3_version::GAME_VERSION);
            return;
        }
    }
    if (!is_addr_readable(g_base_addr + ck3::globals::AI_ENABLED_FLAG)) {
        blog("[CK3-Bridge] ERROR: AI_ENABLED_FLAG (0x%lx) not readable. Wrong binary or version?\n",
                g_base_addr + ck3::globals::AI_ENABLED_FLAG);
        return;
    }
    blog("[CK3-Bridge] Binary verified OK (AI flag readable)\n");

    // Step 2b: Offset regression validation — smoke test critical offsets
    blog("[CK3-Bridge] Step 2b: Running offset validation suite...\n");
    auto validation = validate_offsets();
    if (validation.degraded) {
        blog("[CK3-Bridge] WARNING: Offset validation failed (>30%% groups bad) — "
             "entering DEGRADED mode (read-only, no vtable hooks)\n");
        // Still start IPC server for diagnostics, but skip hook installation
        g_initialized = true;
        if (!start_ipc_server(bridge::instance_socket_path())) {
            blog("[CK3-Bridge] ERROR: IPC server failed to start in degraded mode!\n");
        } else {
            blog("[CK3-Bridge] IPC server started in DEGRADED mode\n");
        }
        return;
    }
    if (!validation.degraded)
    blog("[CK3-Bridge] Offset validation passed (%d/%d groups, %d/%d checks)\n",
         validation.groups_passed, validation.total_groups,
         validation.checks_passed, validation.total_checks);

    // Step 3: AI parallelism — no longer forcing serial mode.
    // With serial intent dispatch from on_tick, parallel AI is safe.
    blog("[CK3-Bridge] Step 3: Parallel AI mode preserved (serial intent dispatch)\n");

    // Step 3b (ck-dq7ym.8): install the CJominiMultiplayerLobbyView constructor
    // hook NOW (before game-ready), so the singleton is captured the moment
    // the engine creates the lobby view. The lobby is only constructed once
    // the game starts opening multiplayer/observer UI — well after dlopen —
    // but installing early avoids races with that path. install_hook only
    // touches .text bytes; it does not depend on game state.
#ifdef _WIN32
    // ADR 0001 P6: the same path as Linux from here (IPC listener, the deferred
    // main-tick hook on game-ready, then the session-pass hook (L14), which
    // drains in both clock states). Windows adds one thread: the fault ring is
    // drained to the log once a second (the vectored handler may not do I/O),
    // and the readable-range table is refreshed while no tick runs (paused, or
    // before the hook is in; the drain refreshes it after).
    std::thread([]() {
        for (;;) {
            if (g_session_state.load(std::memory_order_acquire) == SessionState::DORMANT || is_paused())
                refresh_memory_map();
            ck3_win_drain_faults(g_logfile);
            usleep(1000000);
        }
    }).detach();
#endif
    if (::ck3_bridge::install_lobby_view_hook()) {
        blog("[CK3-Bridge] Step 3b: LobbyView constructor hook installed\n");
    } else {
        blog("[CK3-Bridge] Step 3b: LobbyView constructor hook FAILED (singleton "
             "capture disabled; observer mode entry will be unavailable)\n");
    }

    // Steps 4-5 deferred to first tick (C++ runtime may not be fully init during constructor)

    // Step 6: Defer main-tick hook installation until the game reports ready.
    // ck-crm5: Installing the vtable hook at dlopen crashes CK3 on boot because
    // the engine dispatches through the patched vtable slot during early init
    // (before AI manager backing state is constructed). A background thread
    // polls is_game_ready() and installs the hooks atomically when it flips.
    blog("[CK3-Bridge] Step 6: Main-tick hook install DEFERRED until game-ready signal\n");
    blog("[CK3-Bridge]   gating on *GAME_STATE_ROOT != 0 (base-rel 0x%lx)\n",
         static_cast<uintptr_t>(ck3::globals::GAME_STATE_ROOT));

    g_initialized = true;

    // Start IPC server early so test suites can connect while game is still paused/loading.
    // The IPC server is just a socket listener — it doesn't need game state.
    // Tier 1 queries (get_bridge_state, get_tick_info) work immediately;
    // game-state queries return "game_not_loaded" until DORMANT->ACTIVE transition.
    if (!start_ipc_server(bridge::instance_socket_path())) {
        blog("[CK3-Bridge] ERROR: IPC server failed to start during init!\n");
    } else {
        blog("[CK3-Bridge] IPC server started (session=DORMANT, accepting connections)\n");
    }

    // ck-6kvq.9: start the bridge-owned flush timer. NOT the game thread — the
    // game thread does not run while paused, and a flush must be able to END a
    // pause. The loop idles until the session goes ACTIVE, and pipeline_start()
    // is a no-op in any mode but `pipeline`.
    bridge::pipeline_start();

    blog("[CK3-Bridge] Initialization complete. Waiting for game-ready to install hooks...\n");

    // Deferred hook installer (ck-crm5). Polls is_game_ready() every 500ms.
    // When the predicate flips (GAME_STATE_ROOT pointer becomes non-null,
    // i.e. the engine has finished constructing the in-game world), install
    // the main-tick vtable hooks from this thread. Between dlopen and that
    // flip, the bridge is inert — no hook touches any game-side function
    // pointer, so the engine's own early-boot dispatches run untouched.
    //
    // After hooks are installed, this thread also serves as the legacy
    // bootstrap monitor (logging when DORMANT->ACTIVE transitions happen).
    std::thread([]() {
        register_thread(ThreadRole::HookInstaller);   // ck-duv5
        blog("[CK3-Bridge] Hook-installer thread: polling for game-ready...\n");

        // Phase 1: wait for game-ready, then install hooks.
        for (int i = 0; ; i++) {
            usleep(500000);  // 500ms
            if (is_game_ready()) {
                blog("[CK3-Bridge] Hook-installer: game-ready signal detected at %ds\n", i / 2);
                // Refresh memory map so is_addr_readable/is_mapped work during install
                refresh_memory_map();
                if (!install_main_tick_hooks()) {
                    blog("[CK3-Bridge] ERROR: install_main_tick_hooks() failed after game-ready\n");
                    return;
                }
                break;
            }
            if (i > 0 && i % 120 == 0) {  // Every 60s
                blog("[CK3-Bridge] Hook-installer: still waiting for game-ready (%ds)...\n", i / 2);
            }
        }

        // Phase 2 (L13 D1/D2): the session-pass hook goes in once the session
        // thread exists (+0x408 == 1) and its slot holds CSession_ThreadLoop;
        // polled because the thread may start after game-ready. The first
        // drain takes the bridge DORMANT -> ACTIVE with no tick, so a loaded
        // save stays paused: the bridge never unpauses to wake itself (the
        // ck-w809/ck-7kbi auto-unpause existed only because ACTIVE needed a
        // tick, and is gone with that need).
        SessionHookInstall session_hook = install_session_pass_hook();
        for (int iter = 1; ; iter++) {
            if (session_hook == SessionHookInstall::NotYet) {
                session_hook = install_session_pass_hook();
            }
            if (g_session_state.load(std::memory_order_acquire) != SessionState::DORMANT &&
                session_hook != SessionHookInstall::NotYet) {
                blog("[CK3-Bridge] Hook-installer: session %s, session-pass hook %s; done\n",
                     session_state_string(),
                     session_hook == SessionHookInstall::Installed ? "installed" : "refused");
                return;
            }
            // Until the first drain, nothing else refreshes the readable-range
            // table, and the save's objects are allocated after game-ready: every
            // is_mapped() check on them failed, so reads at the load date
            // answered character_not_found (measured: P5 parity run,
            // query_alliances / query_call_to_arms). The drain refreshes it from
            // then on.
            refresh_memory_map();
            if (iter % 120 == 0) {  // Every 60s
                blog("[CK3-Bridge] Hook-installer: still waiting (%ds): session=%s, "
                     "session-pass hook %s\n", iter / 2, session_state_string(),
                     session_hook == SessionHookInstall::NotYet ? "not yet installable"
                     : session_hook == SessionHookInstall::Installed ? "installed" : "refused");
            }
            usleep(500000);  // 500ms
        }
    }).detach();
}

void bridge_shutdown() {
    blog("[CK3-Bridge] Shutting down...\n");

    // L13: no drain may start once shutdown begins.
    remove_session_pass_hook();

    // Reset session state
    g_session_state.store(SessionState::DORMANT, std::memory_order_release);
    g_last_ai_manager = nullptr;

    // Shutdown event handler
    shutdown_event_handler();

    // Clear pending intents
    clear_pending_intents();
    g_intent_queue.clear();

    // Restore original AI tick function(s)
    if (g_orig_main_tick) {
        if (g_used_inline_hook) {
            // Inline hook: remove by restoring original bytes via remove_hook
            remove_hook(bridge::game_fn(ck3::funcs::AI_MANAGER_MAIN_TICK));
            blog("[CK3-Bridge] Restored inline hook (original bytes restored)\n");
        } else if (g_vtable_tick_slot >= 0 && g_vtable_addr != 0) {
            // Primary vtable hook: restore the original vtable entry
            auto* vtable = reinterpret_cast<uintptr_t*>(g_vtable_addr);
            uintptr_t orig_val = reinterpret_cast<uintptr_t>(g_orig_main_tick);
            if (!write_to_readonly(&vtable[g_vtable_tick_slot], &orig_val, sizeof(uintptr_t))) {
                ScopedWritable sw(reinterpret_cast<void*>(&vtable[g_vtable_tick_slot]), sizeof(uintptr_t));
                vtable[g_vtable_tick_slot] = orig_val;
            }
            blog("[CK3-Bridge] Restored primary vtable[%d] to original tick function\n", g_vtable_tick_slot);
        }
    }
    // Restore internal vtable hook
    if (g_orig_main_tick_thunk && g_vtable2_tick_slot >= 0 && g_vtable2_addr != 0) {
        auto* ivtable = reinterpret_cast<uintptr_t*>(g_vtable2_addr);
        uintptr_t orig_ival = reinterpret_cast<uintptr_t>(g_orig_main_tick_thunk);
        if (!write_to_readonly(&ivtable[g_vtable2_tick_slot], &orig_ival, sizeof(uintptr_t))) {
            ScopedWritable sw(reinterpret_cast<void*>(&ivtable[g_vtable2_tick_slot]), sizeof(uintptr_t));
            ivtable[g_vtable2_tick_slot] = orig_ival;
        }
        blog("[CK3-Bridge] Restored internal vtable[%d] to original thunk\n", g_vtable2_tick_slot);
    }

    // Stop IPC server
    // ck-6kvq.9: stop the conductor BEFORE the IPC server, so no flush is
    // mid-emit when the sockets go away. No-op in legacy mode.
    bridge::pipeline_stop();

    stop_ipc_server();

    g_initialized = false;
    blog("[CK3-Bridge] Shutdown complete\n");
}

// =============================================================================
// Build B: Per-tick logic + deferred init (action_dispatch, name_tables)
//
// BUILD SEQUENCE (incremental crash isolation):
//   Build A: IPC server + set_ai_manager + tick counter (DONE - stable 60s)
//   Build B: + refresh_memory_map/tick, deferred init (action_dispatch, name_tables),
//            delayed unpause, IPC msg queue drain, auto_respond_pending_events, DIAG/50 (current)
//   Build C: + init_event_handler, init_tls_offsets
//   Build D: + personality hooks, AI disable/enable, intent drain (full bridge)
//
// The FULL on_tick is preserved below as on_tick_full() for reference.
// =============================================================================

// --- Build B on_tick: IPC + observation + per-tick logic ---
// Adds to Build A: refresh_memory_map, deferred init (action_dispatch, name_tables),
// delayed unpause, IPC message queue processing, auto_respond_pending_events, DIAG logging.
// Each addition is wrapped in its own try/catch or SIGSEGV protection.
// =============================================================================
// Dead-ruler control handoff (multi-reign play)
//
// When the controlled primary dies, control must follow the dynasty across the
// succession or the agent steers a corpse indefinitely (observed: Halfdan died
// ~0886, control stuck on dead char 12491 for ~10 game-years until manual
// recovery). The most reliable successor anchor is the dead ruler's PRIMARY
// TITLE's CURRENT holder: the engine has already executed succession and moved
// the title to the live heir, and the title's HOLDER_CHAR_ID field is the
// authoritative live-holder pointer (campaign lesson: trust
// title_succession.current_holder_id).
//
// resolve_controlled_successor(): given the dead controlled char, returns the
//   live successor id (current holder of its highest-tier held title), or 0.
//   PURE READS ONLY (lookup → resolve_title_realm → held-title array →
//   lookup_title_obj → HOLDER_CHAR_ID), every hop is_mapped/safe_read guarded,
//   bounded by the held-title count (clamped). No engine calls.
//
// Crash-safety: runs inside the existing on_tick sigsetjmp protected region.
// All memory access is safe_read<T> (process_vm_readv, faults caught) + is_mapped
// gating; no engine function is invoked except the optional AI re-enable
// (g_ai_char_toggle), which is the same call set_controlled_character already
// makes on the game thread. Bounded: at most one title-array scan (<= 64 entries)
// per dead-ruler detection, and the detection only fires on the tick the death
// is first observed (state is latched on g_controlled_primary).
// =============================================================================
static uint32_t resolve_controlled_successor(uint32_t dead_id) {
    void* dead = lookup_character(dead_id);
    if (!dead || is_sentinel(dead)) return 0;

    uintptr_t tr = resolve_title_realm(reinterpret_cast<uintptr_t>(dead));
    if (!tr || !is_mapped(tr)) return 0;

    uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t count = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (!arr || !is_mapped(arr)) return 0;
    if (count < 0) count = 0;
    if (count > 64) count = 64;  // bound the scan

    // Pick the highest-tier held title (the primary) and read its CURRENT holder.
    // Note: held-title array entries can be stale across the succession tick, but
    // the title OBJECT's HOLDER_CHAR_ID (lookup_title_obj) is live — that is what
    // we read. We choose the highest-tier title the dead ruler held so we follow
    // the realm crown, not an incidental low-tier title.
    int best_tier = -1;
    uint32_t successor = 0;
    for (int32_t i = 0; i < count; ++i) {
        uint32_t tid = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
        if (tid == 0) continue;
        uintptr_t tobj = lookup_title_obj(tid);
        if (!tobj || !is_mapped(tobj)) continue;
        int tier = get_title_tier(tobj);
        if (tier <= best_tier) continue;
        uint32_t holder = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
        if (holder == 0 || holder == 0xFFFFFF || holder == dead_id) continue;
        best_tier = tier;
        successor = holder;
    }
    return successor;
}

// Cache of the controlled ruler's current PRIMARY (highest-tier) held title id,
// refreshed every tick while they are ALIVE. On death the dead ruler's own
// held-title array is emptied (titles transfer to the heir), so resolving the
// successor from the dead char fails — we must have captured the title while
// alive and then read THAT title object's new holder (which is the heir).
static std::atomic<uint32_t> g_controlled_primary_title_cache{0};

// highest_tier_held_title(): highest-tier held title id of a (live) char, or 0.
// Pure reads, bounded scan (<=64). Mirrors resolve_controlled_successor's scan
// but returns the title id (to cache) rather than the holder.
static uint32_t highest_tier_held_title(void* char_obj) {
    uintptr_t tr = resolve_title_realm(reinterpret_cast<uintptr_t>(char_obj));
    if (!tr || !is_mapped(tr)) return 0;
    uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t count = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (!arr || !is_mapped(arr)) return 0;
    if (count < 0) count = 0;
    if (count > 64) count = 64;
    int best_tier = -1; uint32_t best_tid = 0;
    for (int32_t i = 0; i < count; ++i) {
        uint32_t tid = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
        if (tid == 0) continue;
        uintptr_t tobj = lookup_title_obj(tid);
        if (!tobj || !is_mapped(tobj)) continue;
        int tier = get_title_tier(tobj);
        if (tier <= best_tier) continue;
        best_tier = tier; best_tid = tid;
    }
    return best_tid;
}

// title_current_holder(): live holder of a title id, excluding a given id (the
// dead ruler) and sentinels. The title OBJECT persists across the succession
// tick and its HOLDER_CHAR_ID updates to the heir — that is the reliable anchor.
static uint32_t title_current_holder(uint32_t tid, uint32_t exclude_id) {
    if (tid == 0) return 0;
    uintptr_t tobj = lookup_title_obj(tid);
    if (!tobj || !is_mapped(tobj)) return 0;
    uint32_t holder = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
    if (holder == 0 || holder == 0xFFFFFF || holder == exclude_id) return 0;
    return holder;
}

// check_controlled_handoff(): per-session primary-character liveness check.  While
// a control session's primary is alive, cache its highest-tier title so we can find
// the heir after death.  When it dies, resolve the heir via that cached title's
// current holder, move the session's claim to the heir, and rebuild the global
// legacy mirrors.  Re-checks every tick until the heir is resolvable.
// MUST run inside a protected region.
static void check_controlled_handoff() {
    if (!g_char_is_alive) return;

    // ── ck-handoff-swap: ONE WALK PER STATE, NOT ONE PER TICK ────────────────
    // The 64-candidate walk answers a COLLISION question and the answer does not
    // change between adjacent ticks. Re-walking every tick turned ONE
    // unresolvable handoff into 2,574 indistinguishable watchlist_full lines in
    // diag25. The state machine lives in skip_held_heir.h so a fixture can drive
    // it (ck-7rk6's reason for extracting handoff_subject: inline in the tick
    // loop it is a condition a test can only grep for).
    //
    // TICK-THREAD ONLY, so no lock: check_controlled_handoff has exactly one
    // caller (on_tick, below) and runs inside the protected region. A second
    // caller would need one, which is why the count is stated rather than
    // assumed.
    static std::unordered_map<std::string, bridge::HandoffBackoff> s_handoff_backoff;

    std::vector<std::string> sids = bridge::g_session_registry.ids();
    bool any_change = false;

    for (const std::string& sid : sids) {
        bridge::Session* session = bridge::g_session_registry.find(sid);
        if (!session || session->is_observe()) continue;

        uint32_t ctrl = 0;
        uint32_t cached_tid = 0;
        {
            std::lock_guard<std::mutex> lk(session->mutex);
            ctrl = session->primary_character_id;
            cached_tid = session->primary_title_id;
            // ck-7rk6: an agent that releases its own CORPSE zeroes
            // primary_character_id (session.cpp release path), and this loop
            // used to `continue` on ctrl == 0 — so the handoff never ran, the
            // skip-held-heir redirect never fired, and the seat was lost with
            // the counter reading 0 for a collision that happened. Fall back to
            // the last character this session steered so the DEATH is still
            // seen.
            //
            // GUARDED ON DEATH, not merely on the fallback being set: a session
            // that deliberately released a LIVE character chose to stop
            // steering it and must not be handed an heir. Only a corpse means
            // the seat is being lost to the succession this feature exists for.
            // ck-7yrp: READ THE RECORDED STATE, do not re-derive it.
            //
            // This used to call lookup_character(last) and ask whether it is dead
            // NOW. That is a different question from the one handoff_subject
            // documents — "was it alive when the agent let it go" — and the two
            // agree only while nothing dies in between. ep-03 released 36552 alive
            // at tick ~3200, it died, and at tick ~6300 this branch seized it.
            const uint32_t last = session->last_steered_character_id;
            const bridge::ReleaseSubjectState rel_state = session->last_release_state;
            const uint32_t before = ctrl;
            ctrl = bridge::handoff_subject(ctrl, last, rel_state);
            // LOG THE BRANCH AND THE STATE IT TURNED ON, once per decision, so a
            // later reader can tell a recovery from a refusal-to-recover without
            // re-deriving either. Two whole format strings, one per outcome.
            if (before == 0 && last != 0) {
                if (ctrl != 0) {
                    blog("[CK3-Bridge] HANDOFF_RECOVERY: session %s last=%u "
                         "state_at_release=%s — recovering a corpse release\n",
                         sid.c_str(), last,
                         bridge::release_subject_state_name(rel_state));
                } else {
                    blog("[CK3-Bridge] HANDOFF_NO_RECOVERY: session %s last=%u "
                         "state_at_release=%s — NOT a succession; the agent chose "
                         "to stop steering, or nobody recorded what it released\n",
                         sid.c_str(), last,
                         bridge::release_subject_state_name(rel_state));
                }
            }
        }
        if (ctrl == 0) continue;

        void* cc = lookup_character(ctrl);
        if (!cc || is_sentinel(cc)) continue;

        if (g_char_is_alive(cc)) {
            // ALIVE: refresh cached primary title.
            uint32_t pt = highest_tier_held_title(cc);
            if (pt != 0) {
                std::lock_guard<std::mutex> lk(session->mutex);
                session->primary_title_id = pt;
            }
            continue;
        }

        // DEAD: resolve heir via cached primary title's current holder.
        uint32_t heir = title_current_holder(cached_tid, ctrl);
        if (heir == 0) heir = resolve_controlled_successor(ctrl);  // legacy fallback
        if (heir == 0) continue;  // not resolvable yet — retry next tick

        // Clear stale council lock for the dead ruler.
        council_lock_remove(ctrl);

        // rc2 B6: THE HUMAN INHERITED. The handoff must not bind the player:
        // unbind the seat (typed reason heir_is_player, published in
        // sessions_json and get_tick_info) and let the agent pick another
        // ruler. The registry refuses the player on every bind path too; this
        // is where the refusal becomes an unbound seat rather than a seat left
        // pointing at a corpse.
        const uint32_t player = bridge::g_session_registry.player_character_id();
        if (player != 0 && (heir & 0xFFFFFFu) == player) {
            bridge::g_session_registry.unbind_for_player_heir(session, ctrl, heir);
            s_handoff_backoff.erase(sid);
            any_change = true;
            continue;
        }

        // Transfer session ownership: SWAP corpse -> heir, then promote.
        //
        // ck-handoff-swap. This was `claim_character(heir)` followed by
        // `release_character(ctrl)` further down, and the ORDER is the defect:
        // the session still holds the corpse while it asks for a slot, so at a
        // full roster it is asking for room while occupying the only room there
        // is. That is not unlucky, it is UNSATISFIABLE BY CONSTRUCTION — every
        // candidate returns watchlist_full — and it is what spun this function
        // 2,574 times in diag25 while the seat stayed pointed at a corpse.
        //
        // The swap is net zero against the cap (ck-swap, Ruling Three): one slot
        // out, one in, under one lock, claim_seq unmoved. It also SUBSUMES the
        // release, which is why the release below is gone rather than moved.
        std::string claim_reason;
        // ck-6xi6: stamped AT THE HANDOFF SITE, not inside swap_character —
        // swap_character also serves the control path, so stamping there would
        // label a control switch as a handoff. Set before the call so the cause
        // is in place by the time the tick hook observes the new primary; the
        // id-pairing in primary_change_cause_for is what makes a stale stamp
        // report `unknown` instead of lying.
        bridge::note_primary_change(heir, bridge::kPrimaryCauseHandoff);
        if (!bridge::g_session_registry.swap_character(session, ctrl, heir,
                                                       &claim_reason)) {
            // ── ck-7rk6 SKIP-HELD-HEIR ─────────────────────────────────────
            // This `continue` used to end the seat: arma6's six->five. The
            // collision IS this claim failure — the game's heir is already held
            // by another participant session.
            //
            // Walk the GAME'S OWN ordered heir array and take the first
            // candidate nobody holds. Staying inside SUCCESSION_HEIRS is
            // bt-bbd's explicit constraint: the search must not widen past what
            // the game itself would do, so no character is invented and no
            // heuristic is introduced.
            //
            // THE REGISTRY IS THE AUTHORITY ON "HELD", so this ATTEMPTS the
            // claim rather than consulting a separate list first. A g_controlled
            // _chars read could disagree with the registry, and then two answers
            // to one question would exist — the duplication that has cost this
            // repo two lanes tonight already.
            const uint32_t intended_heir = heir;

            // Backing off from an identical, already-walked state: pay nothing.
            // A state CHANGE falls straight through, so a real succession is
            // never delayed by a stale entry.
            if (bridge::handoff_should_skip(s_handoff_backoff[sid], ctrl,
                                            intended_heir)) {
                continue;
            }

            uint32_t redirected = 0;
            int      redirect_index = -1;
            int32_t  heir_count = 0;

            uintptr_t tobj = lookup_title_obj(cached_tid);
            if (tobj && is_mapped(tobj)) {
                uintptr_t harr = safe_read<uintptr_t>(tobj + ck3::title_obj::SUCCESSION_HEIRS);
                heir_count = safe_read<int32_t>(tobj + ck3::title_obj::SUCCESSION_HEIR_COUNT);
                if (heir_count < 0) heir_count = 0;
                if (heir_count > 64) heir_count = 64;   // bound the scan
                if (harr && is_mapped(harr)) {
                    for (int32_t hi = 0; hi < heir_count; ++hi) {
                        uint32_t cand = safe_read<uint32_t>(harr + hi * 4) & 0xFFFFFF;
                        if (cand == 0 || cand == 0xFFFFFF) continue;
                        if (cand == intended_heir || cand == ctrl) continue;
                        if (player != 0 && cand == player) continue;   // rc2 B6
                        void* cand_cc = lookup_character(cand);
                        if (!cand_cc || is_sentinel(cand_cc)) continue;
                        if (!g_char_is_alive(cand_cc)) continue;
                        std::string why;
                        // A SWAP HERE TOO. The corpse is still held on this
                        // path as well, so leaving it a plain claim would fix
                        // the heir case and leave ck-7rk6's collision case
                        // failing at a full roster — half the fix, passing an
                        // acceptance written around the other half.
                        bridge::note_primary_change(
                            cand, bridge::kPrimaryCauseHandoff);
                        if (bridge::g_session_registry.swap_character(session, ctrl,
                                                                     cand, &why)) {
                            redirected    = cand;
                            redirect_index = static_cast<int>(hi);
                            break;
                        }
                    }
                }
            }

            if (redirected == 0) {
                // REFUSE rather than paper over. No unheld heir exists; the seat
                // ends. Inventing a character would make the bridge the author
                // of a succession the game did not perform.
                //
                // THE REASON TRAVELS WITH THE RECORD, and it is reported ONCE
                // PER STATE CHANGE. The old call took no reason and its message
                // asserted an exhausted succession order whatever the cause —
                // false when the cause is watchlist_full, which at a full roster
                // was the whole population. 2,574 copies of that line said
                // nothing the first did not, and the volume is what hid it.
                if (bridge::handoff_note_failure(s_handoff_backoff[sid], ctrl,
                                                 intended_heir,
                                                 claim_reason.c_str())) {
                    bridge::record_skip_held_heir_exhausted(
                        sid.c_str(), ctrl, intended_heir,
                        static_cast<int>(heir_count), claim_reason.c_str());
                    //: ck-jij9: the SWAP leg is a claim-set event too, and this is
                    //: the line that reports one FAILING — the case a corroborator
                    //: is most needed for, since the ledger has no row for a swap
                    //: that did not happen.
                    char cse_fail[64];
                    bridge::claim_event_stamp(
                        cse_fail, sizeof cse_fail,
                        g_tick_id.load(std::memory_order_relaxed));
                    blog("[CK3-Bridge] CLAIM_SET_EVENT CONTROLLED_HANDOFF: %u dead "
                         "-> heir %u swap failed (%s) %s\n",
                         ctrl, heir, claim_reason.c_str(), cse_fail);
                }
                continue;
            }

            bridge::record_skip_held_heir(sid.c_str(), ctrl, intended_heir,
                                          redirected, redirect_index);
            // THE INVARIANT ck-9rto's WITNESS READS: heir_character_id must be
            // the character ACTUALLY CLAIMED. Reassigning `heir` here keeps the
            // single variable that is both claimed and recorded below, so the
            // two cannot disagree. Recording the PRIMARY heir instead grades as
            // `change` and INVALIDATES the episode — they drove both cases.
            heir = redirected;
        }
        // NO release_character(ctrl) HERE. The swap above already took the corpse
        // out of the session and out of ownership_, in the same locked step that
        // put the heir in — that is what makes it net zero. Calling release now
        // would be a no-op (release_character is ownership-checked and the corpse
        // is no longer owned), but a no-op that reads like the mechanism is worse
        // than no line: the next person would take it for the thing that frees
        // the slot, and the ORDER is exactly what was wrong here. The probe
        // MEASURES that it is a no-op rather than asserting it.
        //
        // ck-7rk6 is unaffected. Its recovery path needs last_steered_character_
        // id, which release_character sets and the swap deliberately does not —
        // but handoff_subject() only consults it when primary_character_id == 0,
        // and after a successful swap the primary IS the heir. The case ck-7rk6
        // protects is an agent releasing its own corpse, which is still a release
        // and still sets the field.
        s_handoff_backoff.erase(sid);
        {
            std::lock_guard<std::mutex> lk(session->mutex);
            session->primary_character_id = heir;
            session->heir_character_id = heir;
            session->primary_title_id = 0;  // refresh next alive tick
        }
        any_change = true;

        // ck-48z8: THE HANDOFF PRESERVES THE AI STATE THE CLAIM HAD.
        //
        // This block used to read: "Re-enable agent steering of the heir if
        // agent_mode is active", and then call g_ai_char_toggle(ai_ctrl, 0) — which
        // DISABLES the heir's per-character AI tick. The comment described the
        // opposite of the effect, and the condition was wrong as well:
        // g_agent_mode is rebuilt as `primary != 0`
        // (SessionRegistry::rebuild_legacy_mirrors), so it is TRUE in every arm
        // episode that has any control session, whatever that session asked for.
        //
        // The registered design is STEER mode — "the last personality steer
        // persists; the native AI executes as it always does" — and
        // do_set_controlled_character defaults disable_native_ai=false. So every
        // succession in set28 (ep-01 x3, ep-02 x3, ep-03 x2, ep-04 x1) promoted its
        // heir into Drive mode: no self-declared wars, no sieges, occupation stuck
        // at 0, the M-003 class. ep-03's own agent said so after the handoff to
        // 47368 — "the bridge has disabled his native AI, freezing his tick" — and
        // then spent nine set_character_ai calls trying to turn it back on.
        //
        // g_agent_mode CANNOT answer this question. It says "is anyone steering
        // anything"; what a handoff needs is "did THIS session's claim disable the
        // native AI", which the session now records at claim time. For a default
        // take those differ, and every take in set28 was default.
        bool claim_disabled = false;
        {
            std::lock_guard<std::mutex> lk(session->mutex);
            claim_disabled = session->claim_disabled_native_ai;
        }
        int heir_ai_enabled = -1;   //: -1 = not observed, NOT "disabled"
        if (g_get_ai_ctrl) {
            void* heir_cc = lookup_character(heir);
            if (heir_cc && !is_sentinel(heir_cc)) {
                void* ai_ctrl = g_get_ai_ctrl(heir_cc);
                if (ai_ctrl) {
                    //: ONLY if the dying character's session had explicitly taken
                    //: control WITH disable_native_ai=true. Never on g_agent_mode.
                    if (bridge::handoff_should_disable_heir_ai(
                            claim_disabled,
                            g_agent_mode.load(std::memory_order_relaxed))
                        && g_ai_char_toggle) {
                        g_ai_char_toggle(ai_ctrl, 0);
                    }
                    //: READ IT BACK. The bead asks for the heir's ai_enabled AFTER
                    //: the swap, and a value we wrote is not a value we observed —
                    //: if the toggle silently failed, or something else moved the
                    //: byte, the log has to show what is actually there.
                    heir_ai_enabled = *reinterpret_cast<volatile uint8_t*>(
                        reinterpret_cast<uintptr_t>(ai_ctrl) + ck3::ai_ctrl::AI_ENABLED);
                }
            }
        }

        //: TWO WHOLE FORMAT STRINGS, one per branch, so `strings` finds both claims
        //: in the binary — a %s for the state would leave neither in .rodata for the
        //: blind-rebuild witness the bead asks for.
        // ck-jij9: and the TICK + GAME DATE, so a handoff is locatable exactly
        // rather than to the nearest 50-tick DIAG marker. blog() already supplies
        // the monotonic wall-clock stamp; what was missing is the GAME's clock,
        // which is the one a vacancy is measured in.
        char cse[64];
        bridge::claim_event_stamp(cse, sizeof cse,
                                  g_tick_id.load(std::memory_order_relaxed));
        if (heir_ai_enabled < 0) {
            blog("[CK3-Bridge] CLAIM_SET_EVENT CONTROLLED_HANDOFF: session %s %u "
                 "dead -> heir %u heir_ai_enabled=unobserved "
                 "claim_disabled_native_ai=%d %s\n",
                 sid.c_str(), ctrl, heir, claim_disabled ? 1 : 0, cse);
        } else {
            blog("[CK3-Bridge] CLAIM_SET_EVENT CONTROLLED_HANDOFF: session %s %u "
                 "dead -> heir %u heir_ai_enabled=%d "
                 "claim_disabled_native_ai=%d %s\n",
                 sid.c_str(), ctrl, heir, heir_ai_enabled,
                 claim_disabled ? 1 : 0, cse);
        }
    }

    if (any_change) {
        bridge::g_session_registry.rebuild_legacy_mirrors();
    }
}

// The drain's thread rule. With the session-pass hook live the game thread IS
// the session thread: the first drain records it and every later call from any
// other thread is refused (logged once), because the drain's guarantees —
// serial with every engine command and with the whole daily tick, under the
// session mutex — hold only there. Without the hook the drain runs from on_tick
// on whichever AI-tick worker took CAIManager's job, as before L13, and that
// thread is noted as the game thread for the call. The SITE is passed, never
// inferred from session_pass_hook_live(): the hook can go live between
// on_tick's check and this one, and an AI-tick worker must never claim the
// session thread's slot (every later session-pass drain would be refused).
enum class DrainSite { SessionPass, AiTick };

static bool claim_drain_thread(DrainSite site) {
    if (site == DrainSite::AiTick) {
        if (session_pass_hook_live()) return false;   // raced the install: the session pass drains
        bridge::note_game_thread();
        return true;
    }
    static std::atomic<int> s_session_tid{0};
    const int me = bridge::current_tid();
    int expected = 0;
    if (s_session_tid.compare_exchange_strong(expected, me, std::memory_order_acq_rel)) {
        bridge::note_game_thread();
        watchdog_init();   // the timer targets the thread that drains
        blog("[CK3-Bridge] SESSION-PASS: game thread is the session thread tid=%d\n", me);
        return true;
    }
    if (expected == me) return true;
    static std::atomic<bool> s_refusal_logged{false};
    if (!s_refusal_logged.exchange(true, std::memory_order_acq_rel)) {
        blog("[CK3-Bridge] SESSION-PASS: REFUSED a drain on tid=%d — the game thread is "
             "the session thread tid=%d (logged once)\n", me, expected);
    }
    return false;
}


// The readable-range table is_mapped() consults is refreshed once per 100
// ticks (parsing /proc/self/maps is not free), so storage the engine allocated
// since — up to ~100 game days of it, or anything a command executed while the
// clock is stopped — is outside it (ff-gl3-r1, tick 27: a scheme started through
// the bridge sat in a new incoming-ref array that is_mapped() refused, and
// query_incoming_schemes listed nothing). Before a pass serves reads, refresh
// when a tick ran since the last such refresh or a second has passed.
static void refresh_ranges_for_reads(uint64_t tick) {
    static uint64_t s_tick_at = ~0ull;
    static auto s_at = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (tick != s_tick_at || now - s_at >= std::chrono::seconds(1)) {
        refresh_memory_map();
        s_tick_at = tick;
        s_at = now;
    }
}

static void drain_game_thread_work_at(DrainSite site) {
    // ck-crm5: never touch bridge state before the world exists.
    if (!is_game_ready()) return;
    if (!claim_drain_thread(site)) return;

    // --- D2: DORMANT -> ACTIVE on the first drain (no tick needed) ---
    // Populate the memory map first so is_mapped() works for the IPC queries
    // the transition admits.
    if (g_session_state.load(std::memory_order_acquire) == SessionState::DORMANT) {
        refresh_memory_map();
        if (bridge::activate_on_first_drain(g_session_state)) {
            blog("[CK3-Bridge] SESSION-PASS: first drain on the game thread (tid=%d, hook=%s, "
                 "tick=%llu) — DORMANT -> ACTIVE\n",
                 bridge::current_tid(), session_pass_hook_live() ? "session_pass" : "ai_tick",
                 static_cast<unsigned long long>(g_tick_id.load(std::memory_order_relaxed)));
            blog("[CK3-Bridge] BUILD-B: Session DORMANT -> ACTIVE\n");
        }
    }

    // --- First drain: deferred init (once only) ---
    // Runs on the game thread (the session thread when the hook is live), paused
    // or running. Every step below is address arithmetic, a code scan or a hook
    // install: none needs a tick. watchdog_init must run HERE because its timer
    // targets the calling thread (SIGEV_THREAD_ID) and the drain is what it
    // guards. init_tls_offsets reads AI_INTERACTION_CORE's prologue for two
    // FS-relative offsets; locate_game_tls applies them to the CALLING thread's
    // FS base at each use, so the direct dispatch resolves the session thread's
    // own TLS block when it runs from this drain.
    static bool s_first_pass_init_done = false;
    if (!s_first_pass_init_done) {
        s_first_pass_init_done = true;

        // Init watchdog timer — must be called from the game thread so
        // SIGEV_THREAD_ID targets this thread's TID
        watchdog_init();

        // ck-e3dp: re-arm the SIGSEGV safety net now that CK3's startup is
        // complete. The engine's CrashReporter registration during startup
        // displaced the handler bridge_init() installed at dlopen time, so
        // every sigsetjmp wrap was decorative until this point (P4.5). This
        // takes the slot back and chains non-protected-region faults to the
        // displaced engine handler. Logged so Verify can assert the re-arm.
        rearm_sigsegv_safety_net("first_drain");

        // Init action dispatch (enables queue_intent, set_personality, etc.)
        try {
            blog("[CK3-Bridge] BUILD-B: Initializing action dispatch...\n");
            init_action_dispatch();
            blog("[CK3-Bridge] BUILD-B: Action dispatch initialized OK\n");
        } catch (...) {
            blog("[CK3-Bridge] BUILD-B: WARNING: init_action_dispatch() threw exception\n");
        }

        // Load the baked map-graph artifact (ck3map.bin) for query_map_graph.
        // Pure file mmap + bounds-check; self-disables to map_artifact_missing if
        // absent. No game state required, but grouped here after g_base_addr is set.
        try {
            map_graph_init();
        } catch (...) {
            blog("[CK3-Bridge] BUILD-B: WARNING: map_graph_init() threw exception\n");
        }

        // Init name tables (enables name resolution for characters, cultures, etc.)
        try {
            blog("[CK3-Bridge] BUILD-B: Initializing name tables...\n");
            init_name_tables();
            blog("[CK3-Bridge] BUILD-B: Name tables initialized OK\n");
        } catch (...) {
            blog("[CK3-Bridge] BUILD-B: WARNING: init_name_tables() threw exception\n");
        }

#ifdef _WIN32
        // ADR 0001 P6: both read the LINUX binary's code shapes (the FS-based TLS
        // block, the send-command ctor's lea pattern). On ck3.exe a shape match
        // would be a different value, not a failure, so they are not run. The
        // trigger query and the direct dispatch reach the TLS context through the
        // Windows TLS block (ai_eval_hook.cpp locate_game_tls); the command
        // vtables and dispatch manager keep their header names, which the
        // Windows header poisons where no Windows value is mapped.
        blog("[CK3-Bridge] BUILD-C: TLS offsets and runtime discovery not run on Windows "
             "(Linux code shapes; P6)\n");
#else
        // Init TLS offsets (enables send_interaction's execute_direct_dispatch)
        try {
            blog("[CK3-Bridge] BUILD-C: Initializing TLS offsets...\n");
            init_tls_offsets();
            blog("[CK3-Bridge] BUILD-C: TLS offsets initialized OK\n");
        } catch (...) {
            blog("[CK3-Bridge] BUILD-C: WARNING: init_tls_offsets() threw exception\n");
        }

        // Runtime discovery of vtable/dispatch manager from instruction decoding
        try {
            blog("[CK3-Bridge] BUILD-C: Running runtime vtable/global discovery...\n");
            init_runtime_discovery();
            blog("[CK3-Bridge] BUILD-C: Runtime discovery OK\n");
        } catch (...) {
            blog("[CK3-Bridge] BUILD-C: WARNING: init_runtime_discovery() threw exception\n");
        }
#endif

        // Init event handler (enables event observation and response)
        try {
            blog("[CK3-Bridge] BUILD-C: Initializing event handler...\n");
            init_event_handler();
            blog("[CK3-Bridge] BUILD-C: Event handler initialized OK\n");
        } catch (...) {
            blog("[CK3-Bridge] BUILD-C: WARNING: init_event_handler() threw exception\n");
        }

        // CALL-site scoring hook — patches the CALL instruction inside
        // AI_INTERACTION_CORE that invokes GetAiWillDoScore, instead of
        // modifying GetAiWillDoScore's prologue (which corrupted .eh_frame FDE).
        try {
            install_score_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: install_score_hook() threw exception\n");
        }

        // War score breakdown hook — post-hooks WAR_SCORE_UPDATE to call
        // sub-functions (imprisonment, battle, occupation, ticking, landless)
        // and cache per-component breakdown for IPC queries.
        try {
            init_war_score_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_war_score_hook() threw exception\n");
        }

        // War declaration observer hook — post-hooks WAR_CREATE_CORE to log
        // every war declaration with full parameters for agent observation.
        try {
            init_war_declaration_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_war_declaration_hook() threw exception\n");
        }

        // War TERMINATION observer hook (ck-sd4d) — hooks WAR_END_CORE, which
        // sets IS_ENDED and then destroys the CWar in the same call. Without
        // this, an ended war's outcome and end date are unreadable by any
        // client: the object is gone before anything can poll for it.
        try {
            init_war_history_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_war_history_hook() threw exception\n");
        }

        // Scheme initiation observer hook — post-hooks CREATE_ACTIVE_SCHEME to log
        // every scheme creation (murder, fabricate_hook, etc.) with full parameters.
        try {
            init_scheme_init_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_scheme_init_hook() threw exception\n");
        }

        // AI CB selector observer hook — post-hooks AI_CB_SELECTOR to capture the
        // S1 verdict (cb_ref/target/score/extra_roll) gated to bridge-initiated
        // execute_war_intent. Replaces the ck-2x8w replay probe (ck-10f5 crash).
        try {
            init_ai_cb_selector_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_ai_cb_selector_hook() threw exception\n");
        }

        // Diagnostic: DECLARE_WAR_SPECIAL_HANDLER hook — logs every engine-dispatched
        // declare_war command's WCC args (reveals whether the AI-path command reaches
        // WCC in the engine's scoped context, + the engine's own valid declarations).
        try {
            init_war_decl_handler_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_war_decl_handler_hook() threw exception\n");
        }

        // Marriage observation hook — post-hooks INTERACTION_TRY_SEND to observe
        // AI marriage/alliance interactions with full parameters.
        try {
            init_marriage_observation_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_marriage_observation_hook() threw exception\n");
        }

        // Council task assignment observer hook — post-hooks SET_COUNCIL_TASK
        // to log every council task assignment with full parameters.
        try {
            init_council_task_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_council_task_hook() threw exception\n");
        }

        // Per-ruler council-AI suppression hook (L20 stickiness) — hooks
        // AI_COUNCIL_TASKS_PER_RULER so a bridge-set council task is not
        // overwritten by the native AI on the next tick for locked rulers.
        // Fail-safe: install failure logs and continues (global behavior
        // unchanged); the hook itself fails open (runs the original) on any
        // unexpected state and never writes council memory.
        try {
            init_council_suppress_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_council_suppress_hook() threw exception\n");
        }

        // Per-character OFF-GOAL routine-war veto (re_offgoal_veto_FINAL.md) —
        // hooks AI_WAR_POST_DECL at entry. Ships INERT (g_offgoal_veto_enabled
        // default false): the hook installs but is a pure pass-through until the
        // agent arms it via set_offgoal_veto. ⚠ ORDERING: intent_dispatch calls
        // war_post_decl BY ADDRESS with bypass=1 (steered war) — that call now
        // routes through the entry hook too, which is FINE: bypass==1 short-
        // circuits to orig before any veto. Fail-safe: install failure logs and
        // continues; the hook fails OPEN on any doubt.
        try {
            init_offgoal_war_veto_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_offgoal_war_veto_hook() threw exception\n");
        }

        // The liege's answer to a faction demand (factions.cpp) — hooks
        // AI_PICK_EVENT_OPTION, the AI's only event-option chooser. A pure
        // pass-through until set_faction_demand_response names a controlled
        // liege; then it returns the FACTION_DEMAND_ACCEPT/REFUSE option for
        // that liege's demand events only, and fails open on any doubt.
        try {
            init_faction_demand_hook();
        } catch (...) {
            blog("[CK3-Bridge] WARNING: init_faction_demand_hook() threw exception\n");
        }
    }


    const uint64_t tick = g_tick_id.load(std::memory_order_relaxed);

    // --- Once per daily tick (g_tick_id moved since the last drain) ---
    // These ran from on_tick once per tick before L13 and keep that cadence: the
    // event paths log per call, and the maps refresh parses /proc/self/maps.
    // This block also runs on the FIRST drain, so the war-DB monitor takes its
    // baseline at ACTIVE (a war declared before the first tick is a new war,
    // witness FF2 m-2). What must not run at load is the auto-respond: a save
    // loaded paused with an event pending and no client is not auto-answered
    // (and unpaused) before anything has run the clock — so it waits for the
    // first tick (s_ticked).
    static uint64_t s_last_tick_seen = ~0ull;
    static bool s_ticked = false;
    if (tick != s_last_tick_seen) {
        if (s_last_tick_seen != ~0ull) s_ticked = true;
        s_last_tick_seen = tick;
        // Refresh memory map periodically (not every tick — parsing /proc/self/maps is expensive)
        if (tick % 100 == 0 || tick <= 3) {
            try {
                refresh_memory_map();
            } catch (...) {
                if (tick <= 3) blog("[CK3-Bridge] BUILD-B: WARNING: refresh_memory_map() threw exception\n");
            }
        }

        // Log rotation. fopen("w") only truncates at process start, so a long single
        // session grows unbounded; this bounds it at the tick boundary.
        //
        // ck-otj9: IT USED TO TRUNCATE IN PLACE — freopen(path, "w") on the same
        // inode, no copy retained, ever. The only survivor was line 1 of the new
        // file. That was written for an untraced log, where discarding the old
        // window is harmless.
        //
        // IT IS NOT HARMLESS WITH CK3_BRIDGE_TRACE ARMED, AND THAT IS THE ONLY TIME
        // IT FIRES. Measured by ck-zgh4 across 11 sealed bundles: an untraced
        // bridge.log is 250KB-1.1MB and NOT ONE had ever rotated. With the trace on
        // it is ~0.148 MB/tick, so a single episode blows a 64MB cap several times.
        // The trace exists to tell "died inside the bridge" from "written then
        // lost" for a timed-out request — and the truncation deleted exactly that
        // evidence, including an observed Backpressure drop line in arma8. A
        // diagnostic that erases its own output is worse than no diagnostic,
        // because the short log that remains reads like a quiet run.
        //
        // Now: RENAME, then reopen. The old inode survives under .1/.2/.3 and the
        // new file's first line NAMES where the previous window went, so a reader
        // of a rotated log cannot mistake it for a complete one. Ring of 3 archives
        // + live ≈ 256MB, which covers a whole traced episode at the rate above.
        if (tick % 100 == 0 && bridge::g_logfile) {
            static constexpr long LOG_MAX_BYTES = 64L * 1024 * 1024;  // 64MB
            static constexpr int  LOG_ARCHIVES  = 3;
            static bridge::LogCoverage s_log_coverage;
            long sz = ftell(bridge::g_logfile);
            if (sz > LOG_MAX_BYTES) {
                const char* path = bridge::instance_bridge_log();
                const bridge::RotateResult rr =
                    bridge::rotate_log(path, &bridge::g_logfile,
                                       LOG_MAX_BYTES, LOG_ARCHIVES,
                                       bridge::instance_layout_is_legacy() ? 0644 : 0600);
                if (rr.archived) {
                    s_log_coverage.archives = LOG_ARCHIVES;
                    const unsigned long long win_start =
                        s_log_coverage.window_start[0];
                    s_log_coverage.note_rotation(static_cast<unsigned long long>(tick));

                    // ck-bwz1: HAND THE RETIRED WINDOW TO THE ARCHIVER, O(1).
                    // The bytes are at <path>.1 right now and will stay there for
                    // ~1300 ticks before the ring drops them; the accept thread
                    // compresses them off this thread. Doing it here would stall
                    // the simulation for a second or more, twelve times a run,
                    // and these episodes MEASURE game-time progression — a stall
                    // is a perturbation of the subject, not a free diagnostic.
                    bridge::ArchiveWindow w;
                    w.start_tick = win_start;
                    w.end_tick   = static_cast<unsigned long long>(tick);
                    snprintf(w.src, sizeof w.src, "%s.1", path);
                    if (!bridge::g_log_archive_q.push(w)) {
                        blog("[CK3-Bridge] log archive queue FULL — the window "
                             "[t%llu,t%llu) at %s.1 was NOT queued and will be lost "
                             "when the ring drops it (ck-bwz1)\n",
                             w.start_tick, w.end_tick, path);
                    }
                    if (s_log_coverage.head_of_run_lost()) {
                        // ck-zgh4: do not let the file count imply coverage. The
                        // ring is full, so the eldest was overwritten and the head
                        // of the run is gone — a reader who counts .1/.2/.3 and
                        // assumes a complete record would be wrong, in the greener
                        // direction.
                        blog("[CK3-Bridge] log rotated (exceeded %ldMB cap) — kept at "
                             "%s.1/.2/.3, but THE RING IS FULL: the eldest window was "
                             "DISCARDED and the retained set now starts at tick %llu, "
                             "NOT at the start of the run (ck-otj9)\n",
                             LOG_MAX_BYTES >> 20, path,
                             s_log_coverage.earliest_retained());
                    } else {
                        blog("[CK3-Bridge] log rotated (exceeded %ldMB cap) — the "
                             "PREVIOUS WINDOW IS NOT LOST, it is at %s.1; older "
                             "windows at .2/.3. Retained set starts at tick %llu "
                             "(the start of the run) (ck-otj9)\n",
                             LOG_MAX_BYTES >> 20, path,
                             s_log_coverage.earliest_retained());
                    }
                } else if (rr.rotated) {
                    // Say so rather than let a truncated log look archived.
                    blog("[CK3-Bridge] log rotated (exceeded %ldMB cap) but the "
                         "ARCHIVE RENAME FAILED (errno=%d) — the previous window "
                         "WAS DISCARDED and this file is not a complete record of "
                         "the run (ck-otj9)\n", LOG_MAX_BYTES >> 20, rr.err);
                }
            }
        }

        // ck-e3dp: keep the SIGSEGV safety net armed. A save reload or late engine
        // re-registration can displace the handler again after the first-tick
        // re-arm; this periodic check (one sigaction read syscall per 100 ticks —
        // no-op when already armed) takes the slot back and re-saves the displaced
        // handler for chaining. Each actual re-arm logs "RE-ARMED (periodic_check)".
        if (tick % 100 == 0) {
            rearm_sigsegv_safety_net("periodic_check");
        }

        // Monthly telemetry (TASK B) — emit one compact JSONL line per game month
        // for the controlled ruler. The emitter self-gates on a month rollover, so
        // calling it every tick is cheap (an int compare on the no-change path).
        // Guarded with SIGSEGV recovery + try in case any read faults.
        g_in_protected_region = true;
        if (sigsetjmp(g_recovery_point, 1) == 0) {
            uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
            int32_t raw_date = gs ? safe_read<int32_t>(gs + ck3::game_state::GAME_DATE) : 0;
            if (raw_date != 0) emit_monthly_telemetry(raw_date);
        } else {
            blog("[CK3-Bridge] WARNING: SIGSEGV in emit_monthly_telemetry at tick=%lu\n", tick);
        }
        g_in_protected_region = false;

        // War DB count-change monitor — detect new wars by polling WAR_DB each tick
        g_in_protected_region = true;
        if (sigsetjmp(g_recovery_point, 1) == 0) {
            check_war_db_tick();
        } else {
            blog("[CK3-Bridge] BUILD-B: WARNING: SIGSEGV in check_war_db_tick\n");
            bridge_strict_check_abort("check_war_db_tick");
        }
        g_in_protected_region = false;

        // Dead-ruler control handoff — if the controlled primary died, re-point to
        // the dynasty successor (primary-title current-holder) so the agent never
        // steers a corpse across a reign. Pure guarded reads + the same AI-toggle
        // set_controlled_character uses; bounded title-array scan. (multi-reign play)
        g_in_protected_region = true;
        if (sigsetjmp(g_recovery_point, 1) == 0) {
            check_controlled_handoff();
        } else {
            blog("[CK3-Bridge] BUILD-B: WARNING: SIGSEGV in check_controlled_handoff at tick=%lu\n", tick);
            bridge_strict_check_abort("check_controlled_handoff");
        }
        g_in_protected_region = false;

        // Event handling — two mutually exclusive paths.
        //
        // Agent connected → push pending events to the agent; the bridge does not
        // pause (L13 D3: the engine's own event pause holds the game where an
        // event demands it). No bridge action answers an event (L16 R3 deleted
        // respond_event, which had no caller).
        //
        // No agent → fall back to auto-dismiss so events don't leave the game stuck
        // on a paused popup forever: always on (L16 deleted its switch,
        // set_auto_respond_events, which had no caller), and only once a tick has
        // run since the bridge came ACTIVE (m-2: never at load); auto_respond
        // unpauses only a game that is paused (m-1).
        if (agent_connected()) {
            g_in_protected_region = true;
            if (sigsetjmp(g_recovery_point, 1) == 0) {
                int notified = poll_and_notify_events();
                if (notified > 0) {
                    blog("[CK3-Bridge] BUILD-B: Notified agent of %d new event(s) at tick=%lu\n",
                         notified, tick);
                }
            } else {
                blog("[CK3-Bridge] BUILD-B: WARNING: SIGSEGV in poll_and_notify_events\n");
                bridge_strict_check_abort("poll_and_notify_events");
            }
            g_in_protected_region = false;
        } else if (s_ticked) {
            g_in_protected_region = true;
            if (sigsetjmp(g_recovery_point, 1) == 0) {
                int dismissed = auto_respond_pending_events();
                if (dismissed > 0) {
                    blog("[CK3-Bridge] BUILD-B: Auto-responded to %d pending events at tick=%lu\n",
                         dismissed, tick);
                }
            } else {
                blog("[CK3-Bridge] BUILD-B: WARNING: SIGSEGV in auto_respond_pending_events\n");
                bridge_strict_check_abort("auto_respond_pending_events");
            }
            g_in_protected_region = false;
        }

    }

    // Check delayed unpause (handles game auto-pausing for events)
    //
    // ck-cg3b: sigsetjmp, not try/catch alone. This step DEREFERENCES ENGINE
    // MEMORY RAW: event_handler.cpp:242 reads *(int32_t*)(idler + PLAYER_ID)
    // with no safe_read and no is_mapped — the only check is `if (idler)`,
    // which is a null test, not a validity test — and then hands the result to
    // the engine's PAUSE_SET. A stale or torn-down idler that is merely
    // non-null faults here, and `catch (...)` does NOTHING for a SIGSEGV: the
    // fault walks straight past it to the fatal handler and takes the process.
    //
    // Its three immediate neighbours below (emit_monthly_telemetry,
    // check_war_db_tick, check_controlled_handoff) are all sigsetjmp-guarded.
    // This one was not, and nothing about it is safer than they are.
    //
    // The try/catch is KEPT INSIDE, not replaced: sigsetjmp recovers a SIGSEGV
    // and does nothing for a C++ exception, so the two guards cover different
    // failures and dropping either would be a regression.
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        try {
            check_delayed_unpause();
        } catch (...) {
            blog("[CK3-Bridge] BUILD-B: WARNING: check_delayed_unpause() threw exception\n");
        }
    } else {
        blog("[CK3-Bridge] BUILD-B: WARNING: SIGSEGV in check_delayed_unpause at tick=%lu\n", tick);
        bridge_strict_check_abort("check_delayed_unpause");
    }
    g_in_protected_region = false;


    // L13: a per-PASS budget, not per tick. The drain holds the session's command
    // mutex, so its length delays the next engine command (the next daily tick
    // included): at most kDrainActionsPerPass queued envelopes, and none started
    // after kDrainPassBudgetMs. An idle pass comes every ~5 ms, so a burst drains
    // within a few passes instead of waiting a game day per 8 actions.
    static constexpr int kDrainActionsPerPass = 8;
    static constexpr int kDrainPassBudgetMs   = 4;
    const auto pass_start = std::chrono::steady_clock::now();
    auto pass_budget_left = [&] {
        return std::chrono::steady_clock::now() - pass_start <
               std::chrono::milliseconds(kDrainPassBudgetMs);
    };

    // L13 D1: the game-state reads the IPC thread is blocked on (Tier 2), first:
    // a reader is waiting, a queued write's caller polls.
    if (game_thread_jobs_pending() > 0) {
        refresh_ranges_for_reads(tick);
        run_game_thread_jobs(kDrainActionsPerPass, pass_budget_left);
    }

    // IPC message queue processing — drain the actions the IPC thread queued
    // for the game thread (is_game_thread_action and anything unclassified)
    {
        int processed = 0;
        char* msg = nullptr;
        while (processed < kDrainActionsPerPass && pass_budget_left() &&
               (msg = dequeue_action(0)) != nullptr) {
            // In-flight B-action identity, captured per inner-action just before
            // dispatch so the outer SIGSEGV branch can stamp a typed `crashed`
            // verdict into the completed ring (mirrors Mechanism-A's
            // recover_drain_crash hook). Cleared after each action
            // is recorded so a crash between actions can't mis-attribute.
            std::string bflight_action;
            std::string bflight_rid;
            uint32_t bflight_actor = 0;

            // M1: per-message session context.  Saved/restored manually because this
            // block may longjmp out on SIGSEGV; RAII destructors are not guaranteed
            // across a siglongjmp.
            bridge::ClientContext b_ctx;
            bridge::ClientContext* saved_b_ctx = bridge::g_current_client_context;
            b_ctx.slot = nullptr;

            // Parse envelope, extract action, dispatch on game thread.
            g_in_protected_region = true;
            if (sigsetjmp(g_recovery_point, 1) == 0) {
                // Parse the queued message (same format as IPC inline)
                rapidjson::Document envelope;
                envelope.Parse(msg);
                std::string drain_session_id;   // ck-n7n0
                if (!envelope.HasParseError() && envelope.IsObject() &&
                    envelope.HasMember("actions") && envelope["actions"].IsArray()) {

                    // M1: reinstall the originating session context for this envelope.
                    // ck-n7n0: and KEEP the id, so the reply can be routed back
                    // to the client that asked instead of broadcast to all of
                    // them. The identity was already being parsed here and
                    // thrown away for routing purposes.
                    if (envelope.HasMember("session_id") && envelope["session_id"].IsString()) {
                        drain_session_id = envelope["session_id"].GetString();
                        bridge::Session* s = bridge::g_session_registry.find(
                            drain_session_id.c_str());
                        if (s) {
                            b_ctx.session = s;
                            bridge::g_current_client_context = &b_ctx;
                        }
                    }

                    // ck-6kvq.19: the conductor stamps the ticket it is
                    // waiting on onto the ENVELOPE (never onto the inner
                    // action, so no handler sees a key its client did not
                    // write). 0 means this envelope came from somewhere else.
                    const uint64_t pipeline_ticket =
                        bridge::DrainRegistry::ticket_from_envelope(envelope);

                    const rapidjson::Value& actions = envelope["actions"];
                    for (rapidjson::SizeType i = 0; i < actions.Size(); i++) {
                        const rapidjson::Value& inner = actions[i];
                        if (!inner.IsObject() || !inner.HasMember("action") || !inner["action"].IsString())
                            continue;

                        const char* action_type = inner["action"].GetString();

                        // Serialize inner action to JSON string
                        rapidjson::StringBuffer sb;
                        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
                        inner.Accept(writer);

                        // Extract request_id from inner action for response injection
                        const char* req_id = nullptr;
                        if (inner.HasMember("request_id") && inner["request_id"].IsString()) {
                            req_id = inner["request_id"].GetString();
                        }

                        // Capture B-action identity for the verdict record + the
                        // outer crash hook: the actor the call names, top level or
                        // under params (drain_write_verdict.h drain_actor_id).
                        const uint32_t b_actor = bridge::drain_actor_id(inner);
                        // The Mechanism-B deferred-write set (re_deferred_write_verdicts.md
                        // §B.2): the is_game_thread_action writes, never a game-thread
                        // read (drain_records_write_verdict, snapshot.cpp).
                        const bool b_write = bridge::drain_records_write_verdict(action_type);
                        if (b_write) {
                            bflight_action = action_type;
                            bflight_rid = req_id ? req_id : "";
                            bflight_actor = b_actor;
                        }

                        std::string response_out;
                        dispatch_action_with_response(action_type, sb.GetString(), &response_out);

                        // ck-6kvq.9: a ticket handed to this drain by the flush
                        // loop is settled BY this drain — never by the mere fact
                        // of having been enqueued. No-op in legacy mode, and a
                        // no-op for any envelope the conductor did not send.
                        bridge::pipeline_note_drain_result(pipeline_ticket, req_id,
                                                           action_type, response_out);

                        // ── Uniform deferred-write verdict (re_deferred_write_verdicts.md §B.3):
                        // surface every Mechanism-B write's verdict in the completed ring
                        // so query_intents / query_dispatch_result{dispatch_id} resolve it.
                        // (Empty reply → typed game_thread_no_reply, never a silent loss.)
                        if (b_write) {
                            record_b_write_verdict(action_type, req_id, b_actor,
                                                   response_out.empty() ? nullptr : response_out.c_str());
                            // Recorded → clear the in-flight identity so a crash on the
                            // NEXT action (or between actions) is not mis-attributed here.
                            bflight_action.clear(); bflight_rid.clear(); bflight_actor = 0;
                        }

                        // ck-dup0: COUNT THE PROVENANCE UNCONDITIONALLY. The
                        // trace below is gated on CK3_BRIDGE_TRACE because it is
                        // per-action volume, and that is right — but "did any
                        // game-thread action take the raw push?" is a question
                        // whose answer is usually NO, and a diagnostic behind a
                        // flag cannot support a negative claim. The counts ride
                        // the heartbeat so an UNTRACED run answers it too.
                        // ck-0lph: RESOLVE THE SPINE ONCE, HERE — above both
                        // the trace that reports it and the injection that
                        // ships it. Computing it twice would let the trace
                        // describe something other than what went on the wire,
                        // and this trace is the instrument the bead is being
                        // decided with. The ticket is already complete:
                        // pipeline_note_drain_result stamped it just above.
                        //
                        // drain_attr: the fragment to splice, or empty.
                        // drain_attr_says: what the trace prints — a named
                        // state in every case, never a blank that reads as
                        // "nothing to say".
                        std::string drain_attr;
                        std::string drain_attr_says = "none (ticket=0)";
                        if (pipeline_ticket) {
                            if (bridge::pipeline_attribution_fragment(pipeline_ticket,
                                                                      &drain_attr)) {
                                drain_attr_says = drain_attr;
                            } else {
                                drain_attr.clear();
                                drain_attr_says = "UNRESOLVED (ticket did not resolve)";
                            }
                        }
                        bridge::note_drain_provenance(pipeline_ticket);

                        // ck-td8m: THE RECORD, BROADCAST, BESIDE THE REPLY.
                        //
                        // ck-0lph put the spine on the reply below, calling it
                        // "the only reply such a call ever gets". Seventeen
                        // hours earlier ck-n7n0 had made that reply UNICAST to
                        // the asking session — the `||` at the send site short-
                        // circuits, so the broadcast leg never runs when the
                        // asker is connected. Both changes are right; the
                        // second removed the premise the first was built on,
                        // and the driver's ArmActionRelay (which keys every row
                        // off `bridge_record` and whose own docstring asserted
                        // these frames are broadcast) stopped seeing a whole
                        // action class. ck-9vgu priced it: construct_building
                        // in bridge.log 168 times, execute=1 167 times, rows in
                        // actions.jsonl ZERO.
                        //
                        // So the RECORD is emitted here as its own frame and
                        // BROADCAST — ck-h4pn's rule, one path over: a record
                        // is not a reply, whoever emits it. `broadcast_
                        // observation` and not `send_observation`, even though
                        // this is the game thread where the thread-local target
                        // is unset and send_observation would broadcast today:
                        // the defect IS a delivery narrowing, and a record
                        // whose audience depends on which thread reached the
                        // emitter is the same bug waiting for the next
                        // refactor.
                        //
                        // BEFORE the reply is built, and on `response_out` as
                        // the HANDLER left it: the injection below rewrites
                        // response_out in place to add `request_id`, and a
                        // record carrying that key would resolve some other
                        // agent's pending future on a broadcast frame. The
                        // caller's id rides as `caller_request_id`.
                        //
                        // An unresolved ticket emits NOTHING (the builder
                        // returns ""): the reply below already carries the
                        // named absence for that case, and a second frame with
                        // a hollow spine would be a row the relay drops in
                        // silence.
                        if (pipeline_ticket && !drain_attr.empty()) {
                            const std::string drain_record =
                                bridge::build_drain_action_record_frame(
                                    action_type, req_id, response_out,
                                    drain_attr, build_envelope_clock_fragment());
                            if (!drain_record.empty()) {
                                bridge::broadcast_observation(
                                    drain_record.c_str(), drain_record.size());
                            }
                        }

                        // ck-xm08: trace game-thread drain path for bridge_timeout triage.
                        // ck-dup0: …and WHICH PATH FED IT. Both the reader's raw
                        // push and the flush's ticketed hand-off end in this same
                        // drain and emitted byte-identical lines until now, which
                        // is why ck-0lph was arguable for a day. ticket=0 is the
                        // raw push; ticket=tk_… is the flush hand-off.
                        if (bridge::g_trace && bridge::g_logfile) {
                            char origin[256];
                            fprintf(bridge::g_logfile,
                                "[CK3-Bridge] BT_TRACE drain_dispatch_done: action=%s rid=%s resp_empty=%d resp_size=%zu %s spine=%s\n",
                                action_type, req_id ? req_id : "(none)",
                                response_out.empty() ? 1 : 0, response_out.size(),
                                bridge::format_drain_origin(origin, sizeof origin,
                                                            pipeline_ticket,
                                                            drain_session_id.c_str()),
                                drain_attr_says.c_str());
                            fflush(bridge::g_logfile);
                        }

                        // Inject request_id + live tick/game_date (#123) into
                        // response (match inline path in ipc_server.cpp).
                        if (!response_out.empty() && response_out[0] == '{') {
                            {
                                std::string injected = "{";
                                if (req_id && req_id[0]) {
                                    injected += "\"request_id\":\"";
                                    injected += req_id;
                                    injected += "\",";
                                }
                                injected += build_envelope_clock_fragment();
                                // ck-0lph: THE ATTRIBUTION SPINE, ON THE ONLY
                                // REPLY THIS CALL EVER GETS. Every game-thread
                                // verb settles here and pipeline_note_drain_result
                                // deliberately emits no second frame, so until now
                                // the spine was minted for inline-serviced calls
                                // and for nothing else. A bundle recorded 140
                                // get_tick_info and 7 set_ai_personality rows and
                                // ZERO set_council_task rows while the council task
                                // was being applied correctly every time — the
                                // driver keys its record off `bridge_record`, and
                                // there was none to key off.
                                //
                                // The ticket is already COMPLETE here:
                                // pipeline_note_drain_result ran above and stamped
                                // flush_id, character_id, tick and game date into
                                // it before this line reads them.
                                //
                                // Ticket 0 is legacy mode or a non-conductor
                                // envelope: nothing is attached and the frame is
                                // byte-identical to before. A ticket that does not
                                // resolve gets a NAMED ABSENCE, never a spine of
                                // empty strings — that would be indistinguishable
                                // from a real record for a session with no id.
                                if (pipeline_ticket) {
                                    if (!drain_attr.empty()) injected += drain_attr;
                                    else injected += ",\"bridge_record_absent\":"
                                                     "\"ticket_did_not_resolve\"";
                                }
                                injected += ",";
                                injected += response_out.substr(1);
                                response_out = std::move(injected);
                            }
                        }

                        // Send response back to agent via IPC
                        if (!response_out.empty()) {
                            // ck-n7n0: target the asker when we know them and
                            // they are still connected; broadcast otherwise, so
                            // engine events and disconnected callers behave
                            // exactly as before. Narrowing delivery must never
                            // become dropping it.
                            bool sent_ok =
                                (!drain_session_id.empty() &&
                                 send_to_session_id(drain_session_id.c_str(),
                                                    response_out.c_str(),
                                                    response_out.size()))
                                || send_observation(response_out.c_str(), response_out.size());
                            bridge::record_reply_drain(
                                action_type, req_id ? req_id : "(none)",
                                sent_ok, response_out.size(),
                                !drain_session_id.empty());
                        } else {
                            bridge::record_empty_reply(
                                action_type, req_id ? req_id : "(none)");
                        }
                    }
                } else {
                    blog("[CK3-Bridge] BUILD-B: WARNING: Failed to parse queued action msg\n");
                }
                // M1: restore caller context before leaving the sigsetjmp-protected block.
                bridge::g_current_client_context = saved_b_ctx;
            } else {
                bridge::g_current_client_context = saved_b_ctx;
                blog("[CK3-Bridge] BUILD-B: WARNING: SIGSEGV during queued action dispatch\n");
                // Crash recovery for the verdict contract: if a Mechanism-B write
                // faulted mid-dispatch, stamp a typed `crashed` verdict into the
                // completed ring keyed by its request_id so query_intents /
                // query_dispatch_result resolve "crashed" instead of not_found —
                // the agent never sees a bare socket timeout. (mirror Mechanism-A's
                // recover_drain_crash hook.)
                if (!bflight_rid.empty()) {
                    record_b_write_verdict(
                        bflight_action.empty() ? "game_thread_action" : bflight_action.c_str(),
                        bflight_rid.c_str(), bflight_actor,
                        R"({"success":false,"reason":"game_thread_crash_during_drain_SIGSEGV"})");
                }
                bridge_strict_check_abort("queued_action_dispatch");
            }
            g_in_protected_region = false;
            free(msg);
            processed++;
        }
        static uint64_t s_batches = 0;
        if (processed > 0 && (++s_batches <= 5 || s_batches % 50 == 0)) {
            blog("[CK3-Bridge] BUILD-B: Processed %d queued actions at tick=%lu (batch %llu)\n",
                 processed, tick, (unsigned long long)s_batches);
        }
    }


    // Drain and execute queued intents and the game-thread query slots (serial:
    // no AI worker runs, so FUN_06b3d770 deep copy cannot deadlock on allocator locks)
    // The client context the drain is entered with: the per-intent guard's
    // destructor does not run on the siglongjmp below, so recovery puts this
    // back (witness m11). Written before sigsetjmp and never after.
    bridge::ClientContext* const drain_entry_ctx = bridge::g_current_client_context;
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        drain_and_execute_intents();
    } else {
        blog("[CK3-Bridge] BUILD-D: WARNING: SIGSEGV in drain_and_execute_intents at tick=%lu\n", tick);
        // 2026-06-09: attribute the crash to the in-flight intent (by seq) so
        // its IPC poll gets a real rejection instead of timing out with no result.
        const uint64_t marked = bridge::recover_drain_crash(
            g_intent_queue, drain_entry_ctx, "game_thread_crash_during_drain");
        blog("[CK3-Bridge] BUILD-D: outer-recovery: client context restored; in-flight intent %s\n",
             marked ? ("seq " + std::to_string(marked) + " rejected").c_str() : "none");
        bridge_strict_check_abort("drain_and_execute_intents");
    }
    g_in_protected_region = false;

    // R1c (ck-6kvq.10, design §2.9): ASSERT THE PULSE LEDGER IS EMPTY.
    //
    // THIS IS THE ANCHOR THE DESIGN IS ABOUT. The `else` branch above is the
    // siglongjmp that jumps clean out of the drain — past every catch(...), past
    // every destructor, and past the restore half of whatever pulse was in
    // flight. A pulse that cleared AI_WAR_SUPPRESS and never put it back leaves
    // the world's AI war pacing changed FOR THE REST OF THE PROCESS, and the
    // episode goes on to produce a clean-looking bundle that is a lie.
    //
    // Running it on BOTH paths (not only after a crash) is deliberate: a leak
    // that happens without a signal — an early `return` a future edit adds
    // between a clear and its restore — is exactly as damaging and exactly as
    // invisible. The sweep restores the saved value, logs pulse_leak{global,
    // saved, observed}, and in experiment mode latches the episode abort.
    pulse_sweep_drain();

    // Resolve character names queued by reader-thread queries (cache misses in
    // get_cached_character_name schedule the id). This runs on the GAME THREAD
    // post-tick (parallel workers done) — the only safe context for
    // CHAR_FIRST_NAME_RESOLVED, which can fault on a null localization scope
    // (see MEMORY: WCC name-gen crash). Guarded by sigsetjmp + watchdog;
    // populate_name_cache() drains its pending set once and no-ops when empty.
    g_in_protected_region = true;
    watchdog_arm(2);
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        populate_name_cache();
    } else {
        blog("[CK3-Bridge] WARNING: SIGSEGV/timeout in populate_name_cache at tick=%lu\n", tick);
    }
    watchdog_disarm();
    g_in_protected_region = false;

    // Clean up consumed/expired intents and update pending counter
    {
        // tick_maintenance moves the consumed/expired intents into the completed
        // ring itself, under the queue's lock (L13 FF2, witness R3).
        std::vector<Intent> completed;
        g_intent_queue.tick_maintenance(tick, completed);
    }

}

// The contract's drain entry (L13): the session-pass hook's call.
void drain_game_thread_work() {
    drain_game_thread_work_at(DrainSite::SessionPass);
}

void on_tick(void* ai_manager) {
    auto orig = reinterpret_cast<void(*)(void*)>(g_orig_main_tick);

    // ck-crm5: safety-net gate. Even though hook installation is deferred
    // until is_game_ready() first flips true, we re-check here to defend
    // against save-reload windows and to make early-return cheap if the
    // engine ever dispatches the patched vtable slot against a partially-
    // torn-down AI manager. If not ready, pass through to the original
    // without touching any bridge state.
    if (!is_game_ready()) {
        if (orig) orig(ai_manager);
        return;
    }
    // The game thread is the drain's (claim_drain_thread); this AI-tick worker is
    // noted only when the drain runs from here (no session-pass hook).

    // --- ck-hxh7: world-reload detection (BEGIN, 1 line) ---
    // Samples a world fingerprint and drops every world-scoped cache when a save
    // is loaded into this live process. DORMANT->ACTIVE (the first drain) is NOT
    // sufficient: it fires once after dlopen, and a later save load never
    // leaves ACTIVE. See world_epoch.cpp for the full rationale.
    world_epoch_on_tick();
    // --- ck-hxh7 (END) ---

    // --- Every tick ---

    // Cache the AI manager pointer so observation queries can use it
    set_ai_manager(ai_manager);

    // Increment tick counter
    g_tick_id.fetch_add(1, std::memory_order_relaxed);
    uint64_t tick = g_tick_id.load(std::memory_order_relaxed);

    // ── ck-6xi6 DIAGNOSTIC (CK3_AI_WATCH=1 only; off = one relaxed load) ──────
    //
    // ARMED FROM HERE, because the engine writes we are hunting happen on the
    // task pool this hook runs on — but NOT on one thread. The comment here
    // used to say "this is THE GAME THREAD", singular, and that was the
    // assumption behind arming once: ck-zgh4 measured the hook on two of five
    // "Primary Task" workers within 36 ticks. A per-task watchpoint armed on
    // one of them returns a clean zero for the other four, which is the
    // reading we established cannot be interpreted.
if (bridge::ai_watch_requested()) {
        // DRAIN BEFORE ANY GAME-THREAD LINE. ai_watch_tick used to run at the END
        // of this block while MOVED and GONE printed in the middle, so a write
        // that happened BEFORE a move was printed AFTER it. "Was there a MOVED or
        // a SWITCH between the disable and this value" is the question the run
        // exists to answer, so the readout's order has to be the real order.
        bridge::ai_watch_drain(g_logfile);

        // COVER THIS WORKER. The hook is NOT one thread: ck-zgh4 measured the
        // node's CK3 running it on two of five "Primary Task" workers within 36
        // ticks. A hardware watchpoint is per-task, so a single arm sees one
        // worker's share of the writes — which is why run 1's watchpoint column
        // cannot distinguish "nothing wrote" from "the writes were elsewhere".
        //
        // Each thread arms ITSELF, because it is the only thing that works:
        // perf_event_open refuses an explicit tid with EACCES under
        // perf_event_paranoid=2 even when the tid is the caller's own, so one
        // thread cannot arm the others. Measured, not assumed.
        bridge::ai_watch_cover_this_thread(g_logfile);
        // And re-scan periodically, so a worker created after arming is covered
        // rather than silently absent from the watchpoint column.
        bridge::ai_watch_rescan_threads(tick, g_logfile);

        // FOLLOW THE SESSION'S PRIMARY (ck-6xi6 run-2 recut). The watch used to
        // read g_controlled_primary ONCE, in the arming condition below, and then
        // track that one character for the whole run — so ep-03's control path
        // (30853 -> 36375 -> 32642 -> 36552) would arm on the first, sit through
        // every switch, and retire at the first death 850 ticks before the real
        // subject was ever controlled. Measured nothing, looked clean.
        if (bridge::g_ai_watch_on.load(std::memory_order_acquire) && g_get_ai_ctrl) {
            const uint32_t prim = g_controlled_primary.load(std::memory_order_relaxed);
            const uint32_t wc = bridge::g_ai_watch_char.load(std::memory_order_relaxed);
            if (prim != 0 && prim != wc) {
                // The cause is READ FROM THE REGISTER the originating action
                // wrote, and only trusted when its id matches the primary we
                // actually observed — otherwise it is reported as unknown rather
                // than guessed from what else happened this tick.
                const bridge::AiPrimaryCause cause =
                    bridge::primary_change_cause_for(prim);
                void* ncc = lookup_character(prim);
                void* nctrl = (ncc && !is_sentinel(ncc)) ? g_get_ai_ctrl(ncc) : nullptr;
                const uintptr_t naddr = nctrl
                    ? reinterpret_cast<uintptr_t>(nctrl) + ck3::ai_ctrl::AI_ENABLED
                    : 0;
                bridge::ai_watch_switch(wc, prim, naddr, cause, &g_tick_id, g_logfile);
                // The selftest re-runs on the NEW address, because "the byte the
                // engine's toggle writes" is a claim about THIS controller and has
                // not been established for it. The readout's rule is therefore
                // "discard the CONTROL-FLIP pair after each ARMED", not "after
                // the first one".
                if (naddr && g_ai_char_toggle &&
                    bridge::g_ai_watch_on.load(std::memory_order_acquire)) {
                    bridge::ai_watch_selftest(
                        reinterpret_cast<void(*)(void*, unsigned char)>(
                            g_ai_char_toggle),
                        nctrl, naddr, g_logfile);
                }
            }
        }

        if (!bridge::g_ai_watch_on.load(std::memory_order_acquire) &&
            !bridge::g_ai_watch_retired.load(std::memory_order_acquire)) {
            const uint32_t prim = g_controlled_primary.load(std::memory_order_relaxed);
            if (prim != 0 && g_get_ai_ctrl) {
                void* cc = lookup_character(prim);
                if (cc && !is_sentinel(cc)) {
                    void* ai_ctrl = g_get_ai_ctrl(cc);
                    if (ai_ctrl) {
                        const uintptr_t byte =
                            reinterpret_cast<uintptr_t>(ai_ctrl) + ck3::ai_ctrl::AI_ENABLED;
                        if (bridge::ai_watch_arm(byte, prim, &g_tick_id, g_logfile) &&
                            g_ai_char_toggle) {
                            // The control runs ONCE, immediately, and before the
                            // observation window. It proves the watchpoint arms
                            // AND that +0x28 is the byte the engine's own toggle
                            // writes — two claims, one flip-and-restore.
                            bridge::ai_watch_selftest(
                                reinterpret_cast<void(*)(void*, unsigned char)>(
                                    g_ai_char_toggle),
                                ai_ctrl, byte, g_logfile);
                        }
                    }
                }
            }
        }
        // Re-resolve every tick: a controller that moved leaves the watchpoint
        // on freed memory, which would make run 2 null for an instrument reason
        // and would have the sampler reading a dead allocation for two
        // game-years.
        if (bridge::g_ai_watch_on.load(std::memory_order_acquire) && g_get_ai_ctrl) {
            const uint32_t wc = bridge::g_ai_watch_char.load(std::memory_order_relaxed);
            void* wcc = wc ? lookup_character(wc) : nullptr;
            void* now_ctrl = (wcc && !is_sentinel(wcc)) ? g_get_ai_ctrl(wcc) : nullptr;
            if (now_ctrl) {
                bridge::ai_watch_rearm_if_moved(
                    reinterpret_cast<uintptr_t>(now_ctrl) + ck3::ai_ctrl::AI_ENABLED,
                    wc, &g_tick_id, g_logfile);
            } else {
                // ck-zgh4: the controller was DESTROYED, not relocated — there is
                // no new address, so the move path cannot run and without this
                // else NOTHING disarms. The sampler would keep dereferencing a
                // stale pointer, and a reused freed page reports garbage as a
                // TRANSITION: a false positive that reads exactly like the
                // off-thread-writer cell.
                // NON-TERMINAL. In ep-03 the first watched character dies 850
                // ticks before the subject is controlled, so retiring here is
                // what made gate 0.5 fail. The window above is COMPLETE for this
                // character; the arming block re-arms when a primary appears.
                bridge::ai_watch_disarm(wc, g_logfile, /*terminal=*/false);
            }
        }
        bridge::ai_watch_tick(tick, g_logfile);
    }

    // DIAG logging every 50 ticks
    if (tick <= 5 || tick % 50 == 0) {
        blog("[CK3-Bridge] BUILD-B: DIAG tick=%lu ai_mgr=%p session=%s tid=%d\n",
             tick, ai_manager, session_state_string(), bridge::current_tid());
    }

    // Re-apply personality locks BEFORE the engine tick too (A7 fix). The
    // post-tick re-write alone is too late: the AI evaluator reads personality
    // bytes (and unhooked parametric/dot-product paths) DURING orig() this
    // same tick, so the steered values must already be in place at tick start.
    // ck-cg3b: personality.cpp:410 WRITES engine memory raw —
    // *(int8_t*)(personality_ptr + i) = axis — after a safe_read chain and an
    // is_mapped() check. The validation is real, which makes this lower risk
    // than check_delayed_unpause above, but is_mapped is a time-of-check test
    // and the store is the time-of-use: a page unmapped in between faults, and
    // catch(...) does nothing for that. All THREE call sites were unguarded,
    // so this is not a judgement that one of them is safe — it is the same
    // omission repeated. try/catch kept inside; the two guards catch different
    // failures.
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        try {
            apply_personality_locks();
        } catch (...) {
            blog("[CK3-Bridge] BUILD-D: WARNING: pre-tick apply_personality_locks() threw at tick=%lu\n", tick);
        }
    } else {
        blog("[CK3-Bridge] BUILD-D: WARNING: SIGSEGV in pre-tick apply_personality_locks at tick=%lu\n", tick);
        bridge_strict_check_abort("pre_tick_apply_personality_locks");
    }
    g_in_protected_region = false;

    // Call original AI tick (launches parallel workers, waits for completion)
    if (orig) orig(ai_manager);

    // === POST-TICK: All parallel workers are done, locks released ===

    // Re-apply personality locks after engine tick (Build D) — belt-and-suspenders
    // re-write in case the engine recomputed any axis from traits during the tick.
    // ck-cg3b: personality.cpp:410 WRITES engine memory raw —
    // *(int8_t*)(personality_ptr + i) = axis — after a safe_read chain and an
    // is_mapped() check. The validation is real, which makes this lower risk
    // than check_delayed_unpause above, but is_mapped is a time-of-check test
    // and the store is the time-of-use: a page unmapped in between faults, and
    // catch(...) does nothing for that. All THREE call sites were unguarded,
    // so this is not a judgement that one of them is safe — it is the same
    // omission repeated. try/catch kept inside; the two guards catch different
    // failures.
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        try {
            apply_personality_locks();
        } catch (...) {
            blog("[CK3-Bridge] BUILD-D: WARNING: apply_personality_locks() threw at tick=%lu\n", tick);
        }
    } else {
        blog("[CK3-Bridge] BUILD-D: WARNING: SIGSEGV in post-tick apply_personality_locks at tick=%lu\n", tick);
        bridge_strict_check_abort("post_tick_apply_personality_locks");
    }
    g_in_protected_region = false;

    // Without the session-pass hook (refused, or not yet in) the game-thread work
    // drains here, after the AI tick, as before L13: only while the clock runs.
    if (!session_pass_hook_live()) drain_game_thread_work_at(DrainSite::AiTick);

    // THIRD apply point (run-2 fix: re_personality_steering_persistence).
    // The pre-tick (:1206) and post-tick (:1219) applies bracket only the
    // CAIManager AI tick (orig(ai_manager)). The pre-tick apply is what
    // guarantees the AI evaluator reads locked axes — it reads +0x308 DURING
    // orig(), immediately after the pre-tick re-assert, so the AI DECISION is
    // already correctly steered.
    //
    // But the engine's per-character trait→personality RECOMPUTE fires on the
    // separate monthly/character-update pipeline (re_county_conversion_mechanics
    // open-q #2: a distinct entry point in the day-processing pipeline, NOT the
    // AI tick). For high-activity chars (e.g. 13568, a tier-5 emperor at war)
    // that recompute can land LATE in the engine's day-processing — after the
    // post-tick apply at :1219 but still within the dispatch that the engine
    // sequences alongside this CAIManager tick. The post-tick apply at :1219
    // therefore misses it, and external probes / the observer UI catch the
    // native (trait-derived) bytes in the inter-tick gap.
    //
    // Re-asserting here, at the very end of on_tick (after intent drain + name
    // resolution, i.e. as late as the bridge can run on the game thread this
    // dispatch), collapses that residual window: any recompute that ran after
    // :1219 this tick is overwritten before control returns to the engine, and
    // any recompute that runs after on_tick returns is in turn overwritten by
    // the next tick's pre-tick apply (:1206) BEFORE the next AI read.
    //
    // Crash-safety: reuses the same fully-guarded apply_personality_locks
    // (lookup→sentinel→alive→core_data→personality_ptr→is_mapped, then ≤9 byte
    // writes). Pure memory read/write, no engine calls, no RNG, bounded by the
    // lock-map size (≤ a handful of chars). Identical safety profile to :1219.
    // ck-cg3b: personality.cpp:410 WRITES engine memory raw —
    // *(int8_t*)(personality_ptr + i) = axis — after a safe_read chain and an
    // is_mapped() check. The validation is real, which makes this lower risk
    // than check_delayed_unpause above, but is_mapped is a time-of-check test
    // and the store is the time-of-use: a page unmapped in between faults, and
    // catch(...) does nothing for that. All THREE call sites were unguarded,
    // so this is not a judgement that one of them is safe — it is the same
    // omission repeated. try/catch kept inside; the two guards catch different
    // failures.
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        try {
            apply_personality_locks();
        } catch (...) {
            blog("[CK3-Bridge] BUILD-D: WARNING: end-of-tick apply_personality_locks() threw at tick=%lu\n", tick);
        }
    } else {
        blog("[CK3-Bridge] BUILD-D: WARNING: SIGSEGV in end-of-tick apply_personality_locks at tick=%lu\n", tick);
        bridge_strict_check_abort("end_of_tick_apply_personality_locks");
    }
    g_in_protected_region = false;
}



} // namespace bridge

// =============================================================================
// DLL entry points (called by dynamic linker)
// =============================================================================

#ifdef _WIN32
// Windows: no ELF constructor (it would run under the loader lock). The proxy
// DLL's init thread calls this after the identity gate matched (dll_main.cpp).
extern "C" void ck3_bridge_start(void) {
    bridge::bridge_init();
}
#else
__attribute__((constructor))
static void dll_init() {
    // Diagnostic-first marker (ck-fj23): proves whether the .so constructor
    // even fires before bridge_init() is called. Append-mode boot log avoids
    // racing the per-PID g_logfile (which is opened later, only in CK3) and
    // captures every host process LD_PRELOAD attaches to (Steam launcher,
    // pressure-vessel helpers, CK3 itself). NO C++ runtime calls — async-safe
    // syscalls only, in case static initializers downstream are the regression.
    {
        int fd = bridge::instance_open_boot_log();   // rc2: only with CK3_BRIDGE_BOOT_LOG=1
        if (fd >= 0) {
            char buf[512];
            char exe[256] = {0};
            ssize_t elen = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (elen < 0) elen = 0;
            exe[elen] = '\0';
            int n = snprintf(buf, sizeof(buf),
                             "[boot] dll_init pid=%d exe=%s\n",
                             getpid(), exe);
            if (n > 0) {
                ssize_t written = write(fd, buf, (size_t)n);
                (void)written;
            }
            close(fd);
        }
    }
    bridge::bridge_init();
}

__attribute__((destructor))
static void dll_fini() {
    // Only shutdown if we actually initialized (skip for Steam/helper processes)
    if (bridge::g_initialized.load(std::memory_order_acquire)) {
        bridge::bridge_shutdown();
    }
}
#endif
