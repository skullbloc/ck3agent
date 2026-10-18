// speed_read.h — ck-k57a. ABSENCE IS A DIFFERENT TYPE THAN A VALUE.
//
// THE DEFECT THIS HEADER EXISTS TO REMOVE. `get_tick_info` used to emit the
// game speed as:
//
//     int32_t speed_raw = safe_read<int32_t>(gs + ck3::game_state::GAME_SPEED);
//     r.key("game_speed"); r.value_int(speed_raw + 1);
//
// `safe_read` returns `T{}` — ZERO — when `process_vm_readv` fails
// (bridge.h). So a FAILED read produced `speed_raw == 0` and the handler
// emitted `0 + 1 == 1`. **One is a valid speed.** A memory fault inside a
// healthy world was therefore indistinguishable on the wire from a world
// genuinely running at speed 1, and under pre-registration §7.3 that turns a
// transient read fault into `speed_mismatch` — a real, typed, archived claim
// about EXPERIMENTAL CONDUCT, manufactured by an arithmetic accident.
//
// THE GENERAL SHAPE, worth naming because it is unlikely to be unique here:
// READ RAW → ADD AN OFFSET → EMIT. Any zero-returning failed read that is then
// arithmetically shifted becomes a valid-looking datum. The `+ 1` is what
// converts an abstention into a lie. Its two cousins in this codebase
// (ck-xqol's `fd = 0` from raw .bss, ck-6kvq.5's `world_epoch() == 0`) at
// least produced a SENTINEL, which a careful consumer can route to abstain.
// This one produced a value in the same range as the data, so there was
// nothing to route and no consumer-side fix existed: the information was
// destroyed inside the bridge.
//
// THE RULE HERE. A speed and the ABSENCE of a speed are different TYPES, not
// different values of one type. `Reading::has_speed` is the only thing that
// licenses reading `Reading::speed`, and the wire mirrors that: `game_speed`
// is a JSON number when and only when a speed was actually read, and JSON
// `null` otherwise, with `speed_read_status` naming WHICH absence it is.
//
// ENGINE-FREE ON PURPOSE. No offsets, no `safe_read`, no CK3. The classifier
// is a pure function of three facts the caller already has, so the same code
// that runs in the live handler also runs in `bridge/test/test_speed_read` and
// in `bridge/test/speed_read_probe` — including the pre-fix negative control,
// which is a separate BINARY rather than a re-typed python model of what the
// bridge used to do.
#pragma once

#include <cstdint>

namespace bridge {
namespace speed {

// Why the read did or did not produce a speed. Three states, not two, because
// "the game state is not loaded" and "the game state is loaded and the speed
// field would not read" are different facts about the world: the first is the
// ordinary condition between save loads, the second is a memory fault worth
// noticing. Collapsing them would hide a bridge problem inside a world state.
enum class ReadStatus {
    Ok = 0,              // a speed was read
    RootUnreadable = 1,  // game-state root null or unreadable — no speed exists to read
    ReadFaulted = 2,     // root resolved, the GAME_SPEED read itself faulted
};

// The wire spelling. This is the `speed_read_status` enum consumers switch on;
// `scripts/episode_validity.py` carries the same three names.
inline const char* status_name(ReadStatus s) {
    switch (s) {
        case ReadStatus::Ok:             return "ok";
        case ReadStatus::RootUnreadable: return "root_unreadable";
        case ReadStatus::ReadFaulted:    return "read_faulted";
    }
    // Unreachable for any declared enumerator. Abstaining beats inventing: an
    // unnamed status must never fall through to "ok".
    return "read_faulted";
}

struct Reading {
    ReadStatus status    = ReadStatus::RootUnreadable;
    // MEANINGLESS unless `has_speed`. Deliberately not defaulted to something
    // plausible: a caller that reads this without checking should get a value
    // that is obviously not a speed rather than one that is.
    int        speed     = 0;
    bool       has_speed = false;

    bool ok() const { return status == ReadStatus::Ok; }

    // THE COMPAT SHAPE, in one place. The pre-ck-k57a contract was "an integer,
    // 1..5 for a speed and 0 for no reading" — and the whole bug was that the
    // fault case did not honour it, emitting 1. A legacy integer consumer gets
    // that contract here, with the fault case FIXED to the 0 it always should
    // have been. It is a MIRROR of `has_speed`/`speed`, never a second source:
    // nothing in this codebase decides anything from it.
    int legacy_int() const { return has_speed ? speed : 0; }
};

// THE CLASSIFIER. Pure.
//
//   root_ok        — did the game-state root pointer resolve?
//   speed_faulted  — did the read of GAME_SPEED itself fault? (the caller
//                    samples `safe_read_fault_count()` around the read; the
//                    counter already existed and the emission site simply
//                    never consulted it)
//   raw            — the 0-indexed stored value; MEANINGLESS when either flag
//                    says there was no read, and this function never touches
//                    it in those cases. That is the fix, stated as code: the
//                    `+ 1` happens on the OK path only.
inline Reading classify(bool root_ok, bool speed_faulted, int32_t raw) {
    Reading r;
    if (!root_ok) {
        r.status = ReadStatus::RootUnreadable;
        return r;
    }
    if (speed_faulted) {
        r.status = ReadStatus::ReadFaulted;
        return r;
    }
    r.status    = ReadStatus::Ok;
    r.speed     = static_cast<int>(raw) + 1;   // 0-4 stored, 1-5 displayed
    r.has_speed = true;
    return r;
}

// The three wire fields, emitted ONCE, by whoever is asking.
//
// Templated on the builder so the live handler (`ResponseBuilder`) and the
// host probe (a twenty-line stub) run the SAME emission. A probe that re-typed
// this would be testing the re-typing.
//
// WIRE CONTRACT:
//   game_speed         int 1..5 when a speed was read, else JSON null.
//                      NEVER 0 and NEVER `raw + 1` on a fault.
//   speed_read_status  "ok" | "root_unreadable" | "read_faulted".
//   game_speed_legacy  the pre-ck-k57a integer contract (0 == no reading), for
//                      consumers that predate `speed_read_status`. Documented
//                      in docs/ci.md; nothing in-tree decides from it.
template <typename Builder>
inline void emit(Builder& b, const Reading& r) {
    b.key("game_speed");
    if (r.has_speed) b.value_int(r.speed);
    else             b.value_null();
    b.key("speed_read_status"); b.value_string(status_name(r.status));
    b.key("game_speed_legacy"); b.value_int(r.legacy_int());
}

}  // namespace speed
}  // namespace bridge
