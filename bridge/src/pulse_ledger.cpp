// pulse_ledger.cpp — R1c (ck-6kvq.10). See pulse_ledger.h for the why.
//
// No engine dependencies on purpose: memory goes through PulseMemory, so this
// whole file — crash path included — is host-compiled by test_pulse_ledger.

#include "pulse_ledger.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <pthread.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace bridge {

const char* pulse_scope_name(PulseScope s) {
    switch (s) {
        case PulseScope::Drain: return "drain";
        case PulseScope::Flush: return "flush";
    }
    return "unknown";
}

// ============================================================================
// PulseLeak::to_json
// ============================================================================
namespace {

void append_escaped(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
}

std::string hex(uintptr_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
    return std::string(b);
}

uint64_t self_tag() {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
        reinterpret_cast<void*>(pthread_self())));
}

}  // namespace

std::string PulseLeak::to_json() const {
    std::string o = "{\"global\":\"";
    append_escaped(o, global);
    o += "\",\"site\":\"";
    append_escaped(o, site);
    o += "\",\"addr\":\"" + hex(addr) + "\"";
    o += ",\"width\":" + std::to_string((unsigned)width);
    o += ",\"saved\":" + std::to_string((unsigned long long)saved);
    o += ",\"observed\":" + std::to_string((unsigned long long)observed);
    o += ",\"restored\":";
    o += restored ? "true" : "false";
    o += ",\"mapped\":";
    o += mapped ? "true" : "false";
    o += ",\"flush_id\":" + std::to_string((unsigned long long)flush_id);
    o += ",\"age_ms\":" + std::to_string((unsigned long long)age_ms);
    o += ",\"scope\":\"";
    o += pulse_scope_name(scope);
    o += "\"}";
    return o;
}

// ============================================================================
// PulseLedger
// ============================================================================
uint64_t PulseLedger::enter(const char* global, const char* site, uintptr_t addr,
                            uint8_t width, uint64_t saved, uint64_t flush_id,
                            uint64_t now_ms) {
    if (!enabled_.load(std::memory_order_relaxed)) return 0;
    if (width != 1 && width != 2 && width != 4 && width != 8) width = 1;

    for (size_t i = 0; i < CAPACITY; ++i) {
        uint32_t expect = SLOT_FREE;
        if (!slots_[i].state.compare_exchange_strong(
                expect, SLOT_FILLING, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            continue;
        }
        Slot& s = slots_[i];
        const uint64_t h = next_handle_.fetch_add(1, std::memory_order_relaxed);
        s.handle     = h;
        s.global     = global ? global : "?";
        s.site       = site ? site : "?";
        s.addr       = addr;
        s.width      = width;
        s.saved      = saved;
        s.flush_id   = flush_id;
        s.entered_ms = now_ms;
        s.thread_tag = self_tag();
        // Publish LAST: the slot is only observable (and only sweepable) once
        // every field is written. Release pairs with the sweep's acquire load.
        s.state.store(SLOT_LIVE, std::memory_order_release);
        entered_total_.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

    // Full. Counted, never silent — that was the intent ring's own incident.
    overflow_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void PulseLedger::exit(uint64_t handle) {
    if (handle == 0) return;
    for (size_t i = 0; i < CAPACITY; ++i) {
        if (slots_[i].state.load(std::memory_order_acquire) != SLOT_LIVE) continue;
        if (slots_[i].handle != handle) continue;
        slots_[i].state.store(SLOT_FREE, std::memory_order_release);
        return;
    }
    // A handle that is not live: a double-exit, or an exit after a sweep
    // already healed it. Counted so it shows up rather than being folded away.
    stale_exit_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t PulseLedger::exit_newest_for_addr(uintptr_t addr) {
    const uint64_t me = self_tag();
    size_t best_i = CAPACITY;
    uint64_t best_h = 0;
    for (size_t i = 0; i < CAPACITY; ++i) {
        if (slots_[i].state.load(std::memory_order_acquire) != SLOT_LIVE) continue;
        if (slots_[i].addr != addr) continue;
        if (slots_[i].thread_tag != me) continue;
        if (slots_[i].handle > best_h) { best_h = slots_[i].handle; best_i = i; }
    }
    // NOT counted as a stale exit. Address-exit is used by pulses whose restore
    // is deliberately idempotent — several sites restore the same global on more
    // than one path, and a sweep may already have healed it — so "nothing live
    // for this address" is the normal, healthy answer, not an anomaly. The
    // handle-based exit() keeps the strict accounting.
    if (best_i == CAPACITY) return 0;
    slots_[best_i].state.store(SLOT_FREE, std::memory_order_release);
    return best_h;
}

size_t PulseLedger::sweep(PulseScope scope, uint64_t current_flush_id,
                          uint64_t now_ms, PulseMemory& mem,
                          std::vector<PulseLeak>* out, bool self_thread_only) {
    if (!enabled_.load(std::memory_order_relaxed)) return 0;

    const uint64_t me = self_tag();
    size_t found = 0;

    for (size_t i = 0; i < CAPACITY; ++i) {
        if (slots_[i].state.load(std::memory_order_acquire) != SLOT_LIVE) continue;
        Slot& s = slots_[i];

        if (self_thread_only && s.thread_tag != me) continue;

        // GENERATION GUARD (§2.9, and see the header). A flush-scope sweep only
        // convicts a pulse that has already survived a whole flush boundary; a
        // pulse entered under the flush that is ending may still be legitimately
        // running on the game thread, and a false pulse_leak aborts the episode.
        if (scope == PulseScope::Flush && s.flush_id >= current_flush_id) continue;

        PulseLeak leak;
        leak.global   = s.global ? s.global : "?";
        leak.site     = s.site ? s.site : "?";
        leak.addr     = s.addr;
        leak.width    = s.width;
        leak.saved    = s.saved;
        leak.flush_id = s.flush_id;
        leak.age_ms   = now_ms >= s.entered_ms ? now_ms - s.entered_ms : 0;
        leak.scope    = scope;
        leak.mapped   = mem.mapped(s.addr);

        if (leak.mapped) {
            leak.observed = mem.read(s.addr, s.width);
            if (leak.observed != s.saved) {
                // HEAL. §2.9: "Non-empty => restore the saved values".
                mem.write(s.addr, s.width, s.saved);
                leak.restored = true;
            }
        } else {
            leak.observed = 0;
        }

        // Free the slot BEFORE reporting, so a sweep can never convict the same
        // pulse twice and a late `exit()` for it becomes a counted stale exit
        // rather than freeing a slot someone else has claimed.
        s.state.store(SLOT_FREE, std::memory_order_release);

        leaks_.fetch_add(1, std::memory_order_relaxed);
        ++found;
        if (out) out->push_back(leak);
    }

    return found;
}

size_t PulseLedger::live() const {
    size_t n = 0;
    for (size_t i = 0; i < CAPACITY; ++i)
        if (slots_[i].state.load(std::memory_order_acquire) == SLOT_LIVE) ++n;
    return n;
}

uint64_t PulseLedger::entered_total() const {
    return entered_total_.load(std::memory_order_relaxed);
}
uint64_t PulseLedger::overflow_count() const {
    return overflow_.load(std::memory_order_relaxed);
}
uint64_t PulseLedger::stale_exit_count() const {
    return stale_exit_.load(std::memory_order_relaxed);
}
uint64_t PulseLedger::leak_count() const {
    return leaks_.load(std::memory_order_relaxed);
}

void PulseLedger::set_enabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
}
bool PulseLedger::enabled() const {
    return enabled_.load(std::memory_order_relaxed);
}

void PulseLedger::reset_for_test() {
    for (size_t i = 0; i < CAPACITY; ++i)
        slots_[i].state.store(SLOT_FREE, std::memory_order_release);
    next_handle_.store(1, std::memory_order_relaxed);
    entered_total_.store(0, std::memory_order_relaxed);
    overflow_.store(0, std::memory_order_relaxed);
    stale_exit_.store(0, std::memory_order_relaxed);
    leaks_.store(0, std::memory_order_relaxed);
    enabled_.store(true, std::memory_order_relaxed);
}

// ============================================================================
// Process-wide ledger + latched verdict
// ============================================================================
PulseLedger& pulse_ledger() {
    static PulseLedger l;
    return l;
}

namespace {
std::atomic<bool>     g_verdict_leaked{false};
std::atomic<bool>     g_verdict_aborted{false};
std::atomic<uint64_t> g_verdict_count{0};
std::mutex            g_verdict_mu;
PulseLeak             g_verdict_first CK3_EARLY_INIT;
}  // namespace

void pulse_latch_verdict(const PulseLeak& leak, bool experiment_mode) {
    g_verdict_count.fetch_add(1, std::memory_order_relaxed);
    bool expect = false;
    if (g_verdict_leaked.compare_exchange_strong(expect, true,
                                                 std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lk(g_verdict_mu);
        g_verdict_first = leak;
    }
    if (experiment_mode) g_verdict_aborted.store(true, std::memory_order_release);
}

bool pulse_episode_aborted() {
    return g_verdict_aborted.load(std::memory_order_acquire);
}

const char* pulse_abort_reason() {
    return pulse_episode_aborted() ? "pulse_leak" : "";
}

PulseVerdict pulse_verdict() {
    PulseVerdict v;
    v.leaked = g_verdict_leaked.load(std::memory_order_acquire);
    v.count  = g_verdict_count.load(std::memory_order_relaxed);
    v.episode_aborted = g_verdict_aborted.load(std::memory_order_acquire);
    v.abort_reason    = v.episode_aborted ? "pulse_leak" : "";
    if (v.leaked) {
        std::lock_guard<std::mutex> lk(g_verdict_mu);
        v.first = g_verdict_first;
    }
    return v;
}

void pulse_reset_verdict_for_test() {
    g_verdict_leaked.store(false, std::memory_order_release);
    g_verdict_aborted.store(false, std::memory_order_release);
    g_verdict_count.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_verdict_mu);
    g_verdict_first = PulseLeak();
}

}  // namespace bridge
