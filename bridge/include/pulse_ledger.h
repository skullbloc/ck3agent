// pulse_ledger.h — R1c: the pulse ledger (ck-6kvq.10).
//
// Design of record: docs/design/event-pipeline-conductor.md §2.9.
//
// A PULSED GLOBAL is process-wide engine state the bridge changes for the
// duration of ONE engine call and restores afterwards. `AI_WAR_SUPPRESS` is the
// archetype: a war intent saves the byte, clears it, runs the AI war pipeline,
// and puts it back (intent_dispatch.cpp, ai_eval_hook.cpp).
//
// THE HAZARD IS NOT INTERLEAVING, IT IS THE CRASH PATH.
// Serialised execution within a flush already keeps two pulses from overlapping
// (§2.9). What nothing protects against is a `siglongjmp` out of the middle of a
// pulse: the outer recovery in `bridge_init.cpp` jumps clean out of
// `drain_and_execute_intents`, past every `catch(...)`, past every destructor,
// and past the restore. `AI_WAR_SUPPRESS` is then left CLEARED FOR THE REST OF
// THE PROCESS — the world's AI stops pacing its wars, the episode's condition
// has silently changed, and the bundle it produces looks clean and is a lie.
//
// A C++ RAII guard CANNOT fix this. `siglongjmp` does not run destructors. So
// the ledger is not a guard: it is an out-of-band record of what is currently
// pulsed, written before the global is touched and cleared after it is put back,
// which SURVIVES the longjmp precisely because it lives in neither the stack nor
// a destructor. A sweep then asserts the ledger is EMPTY at two anchors and
// heals + reports anything left behind.
//
// PURITY. This translation unit has no engine dependencies: memory access goes
// through the abstract `PulseMemory`, so the whole ledger — including the
// crash-path behaviour — is host-tested with no CK3 binary (see
// bridge/test/test_pulse_ledger.cpp, which reproduces the leak with a real
// `siglongjmp`).
//
// LOCK-FREE ON PURPOSE. The ledger must be usable from a frame that is about to
// be longjmp'd out of. A mutex held at the moment of the jump would stay locked
// forever and the sweep would deadlock — strictly worse than the leak it exists
// to catch. Slots are therefore claimed with a CAS and released with a store.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bridge {

// ============================================================================
// Memory — abstract so the ledger host-tests with no engine.
// ============================================================================
class PulseMemory {
public:
    virtual ~PulseMemory() = default;
    virtual bool     mapped(uintptr_t addr) const = 0;
    virtual uint64_t read(uintptr_t addr, uint8_t width) const = 0;
    virtual void     write(uintptr_t addr, uint8_t width, uint64_t value) = 0;
};

// ============================================================================
// Sweep scope — WHERE the assertion is taken, which decides how strict it is.
// ============================================================================
//
//  Drain — taken on the GAME THREAD immediately after `drain_and_execute_intents`
//          returns (both the normal and the sigsetjmp-recovery path). The drain
//          pass is over, so any pulse THIS THREAD entered and has not exited is
//          unambiguously leaked. No grace, no timer. This is the tight anchor
//          and the one that actually catches the §2.9 hazard.
//
//  Flush — taken by the CONDUCTOR THREAD at the end of every flush. This is
//          §2.9's literal assertion ("asserted empty at the end of every
//          flush"). It is generation-guarded: a pulse entered under flush F is
//          only called a leak when the flush ENDING is strictly later than F.
//          Without that guard the conductor would race the game thread and
//          report a pulse that is still legitimately in flight — a false
//          `pulse_leak` is an episode abort, so this guard is load-bearing.
//          The engine call a pulse wraps is watchdog-bounded to 8s and the
//          default flush period is 10s, so surviving a whole flush boundary is
//          never legitimate.
enum class PulseScope { Drain, Flush };

const char* pulse_scope_name(PulseScope s);

// ============================================================================
// One leak, as reported and as published (§2.9: `pulse_leak {global, saved,
// observed}`; the rest is provenance for the human reading the bundle).
// ============================================================================
struct PulseLeak {
    std::string global;         // "AI_WAR_SUPPRESS"
    std::string site;           // "execute_war_intent"
    uintptr_t   addr     = 0;
    uint8_t     width    = 1;
    uint64_t    saved    = 0;   // what the pulse promised to put back
    uint64_t    observed = 0;   // what the world was actually left holding
    bool        restored = false;  // observed != saved and `saved` was written back
    bool        mapped   = true;   // false => could not even read it back
    uint64_t    flush_id = 0;   // the flush the pulse was ENTERED under
    uint64_t    age_ms   = 0;   // how long it had been live
    PulseScope  scope    = PulseScope::Drain;

    // Compact, single-line, no newlines — safe for the log and for the
    // key=value flattening the abort ladder does on the telemetry block.
    std::string to_json() const;
};

// ============================================================================
// PulseLedger
// ============================================================================
class PulseLedger {
public:
    // 32 slots. Pulses are serialised within a flush, so the live count is
    // normally 0 or 1; the headroom exists so a burst can never make `enter`
    // fail silently. An overflow is COUNTED and reported, never dropped.
    static constexpr size_t CAPACITY = 32;

    // ---- the pulse itself --------------------------------------------------

    // Call BEFORE writing the global. `saved` is the value the caller has just
    // read and promises to put back. Returns a nonzero handle, or 0 when the
    // ledger is full (which bumps overflow_count()).
    //
    // `global` and `site` must be string LITERALS (or otherwise outlive the
    // pulse): the slot stores the pointers, because a std::string assignment in
    // this path could allocate, and this path runs on the game thread inside a
    // protected region.
    uint64_t enter(const char* global, const char* site, uintptr_t addr,
                   uint8_t width, uint64_t saved, uint64_t flush_id,
                   uint64_t now_ms);

    // Call AFTER the restore, on every exit path. Idempotent; a handle that is
    // not live is counted (stale_exit_count()) and otherwise ignored, so a
    // double-exit can never free a slot another pulse has since claimed.
    void exit(uint64_t handle);

    // Exit by ADDRESS instead of by handle: frees the most recently entered live
    // slot for `addr` on the CALLING thread. This exists for the pulse helpers
    // whose restore function is handed only the saved value and never sees a
    // handle (arm_/restore_fake_random_scope). It is exact for the LIFO nesting
    // those helpers actually have, and — unlike a caller-side handle stack — it
    // SELF-CORRECTS after a sweep: once the sweep has healed a leaked pulse
    // there is no live slot left, so the late restore becomes a counted stale
    // exit rather than silently freeing some other pulse's slot.
    //
    // "Nothing live for this address" is NOT counted as a stale exit: several
    // pulse sites restore the same global on more than one path on purpose, so
    // a redundant address-exit is healthy. exit(handle) keeps strict accounting.
    // Returns the handle it freed, or 0.
    uint64_t exit_newest_for_addr(uintptr_t addr);

    // ---- the assertion -----------------------------------------------------

    // Assert the ledger is empty. Every slot that counts as leaked for this
    // scope is read back, restored if it does not already hold `saved`,
    // appended to *out, and freed.
    //
    // `self_thread_only` restricts a Drain sweep to pulses entered by the
    // CALLING thread, which is what makes the drain anchor exact: the sweeper
    // is the pulser, so there is no in-flight pulse it could steal.
    //
    // Returns the number of leaks found (0 is the healthy answer).
    size_t sweep(PulseScope scope, uint64_t current_flush_id, uint64_t now_ms,
                 PulseMemory& mem, std::vector<PulseLeak>* out,
                 bool self_thread_only);

    // ---- introspection -----------------------------------------------------
    size_t   live() const;
    uint64_t entered_total() const;
    uint64_t overflow_count() const;
    uint64_t stale_exit_count() const;
    uint64_t leak_count() const;

    // Test seam ONLY: when disabled, `enter` records nothing and `sweep` finds
    // nothing. This exists so the unit test can run its crash scenario as a
    // NEGATIVE CONTROL — with the ledger off the global must stay clobbered,
    // which proves the test reproduces the live symptom and that the ledger is
    // what fixes it. Never called from bridge code.
    void set_enabled(bool on);
    bool enabled() const;

    void reset_for_test();

private:
    enum : uint32_t { SLOT_FREE = 0, SLOT_FILLING = 1, SLOT_LIVE = 2 };

    struct Slot {
        std::atomic<uint32_t> state{SLOT_FREE};
        uint64_t    handle   = 0;
        const char* global   = nullptr;
        const char* site     = nullptr;
        uintptr_t   addr     = 0;
        uint8_t     width    = 1;
        uint64_t    saved    = 0;
        uint64_t    flush_id = 0;
        uint64_t    entered_ms = 0;
        uint64_t    thread_tag = 0;
    };

    Slot slots_[CAPACITY];
    std::atomic<uint64_t> next_handle_{1};
    std::atomic<uint64_t> entered_total_{0};
    std::atomic<uint64_t> overflow_{0};
    std::atomic<uint64_t> stale_exit_{0};
    std::atomic<uint64_t> leaks_{0};
    std::atomic<bool>     enabled_{true};
};

// ============================================================================
// The process-wide ledger + the verdict it latches.
// ============================================================================
PulseLedger& pulse_ledger();

// A latched leak verdict. Once set it is NEVER cleared for the life of the
// process: §2.9 says the episode's validity is already gone, so a later clean
// flush must not be allowed to erase the evidence. Published as
// `pipeline.pulse_leak` on get_tick_info (see pipeline.cpp).
struct PulseVerdict {
    bool        leaked = false;
    uint64_t    count  = 0;   // total leaks observed, not just the first
    PulseLeak   first;        // the first one, which is the one that broke it

    // §2.9: "in experiment mode abort the episode with that typed reason".
    // The bridge cannot terminate an episode — that is ck-6kvq.1's abort ladder
    // (reason `pulse_leak`, exit 13). What it does is latch this, publish it as
    // `pipeline.pulse_leak`, and refuse further control-session work so nothing
    // else contaminates a run whose validity is already gone. In FREE-PLAY a
    // leak is logged and healed and play continues: there is no episode to void.
    bool        episode_aborted = false;
    const char* abort_reason    = "";   // "" | "pulse_leak"
};

// `experiment_mode` decides the abort half. Latching is one-way for the life of
// the process: a later clean flush must not erase the evidence that the world's
// condition silently changed.
void         pulse_latch_verdict(const PulseLeak& leak, bool experiment_mode);
PulseVerdict pulse_verdict();
bool         pulse_episode_aborted();
const char*  pulse_abort_reason();
void         pulse_reset_verdict_for_test();

}  // namespace bridge
