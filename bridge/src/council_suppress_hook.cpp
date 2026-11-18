// council_suppress_hook.cpp — the per-seat council-task pin.
//
// The native AI re-scores each council seat of an AI ruler on that ruler's
// bucket-3 pass (every 180 days for a duke and up) and switches the task when
// another task of the seat scores above 1.25 x the current one. A task a seat
// set is therefore NOT safe: the next pass can replace it. Full path, guards and
// evidence: re/findings/re_council_ai.md and bridge/include/council_pin.h.
//
// WHAT CHANGED (wave 12): the previous version hooked FUN_04dca2a0 and read the
// ruler id at its first argument +0x18. That argument is the CAIStrategy, whose
// +0x18 is the CCharacter POINTER, so the id it read was a pointer's low 24 bits
// and the lock never matched any ruler (867 dump: 0 of 4593). It also froze
// every seat of a locked ruler by design.
//
// NOW: an inline hook on FUN_04dc9fc0 (AI_COUNCIL_TASK_SELECT_FOR_POSITION, the
// per-seat selector; its only caller is FUN_04dca2a0). Its first argument is the
// ruler's CCharacter* and its second the council position. The call is skipped
// only for a (ruler, position) pin that a seat's own council-task write armed,
// and only while that ruler is a controlled character (g_controlled_chars, the
// roster the session registry derives from its claims; the off-goal war veto
// reads the same one). No pins = the original runs with one atomic load.
// Fails OPEN on any pointer it cannot read. Never writes game memory.
//
// The pass can run on AI worker threads (FUN_04ded5f0's job branch), so every
// shared read here is under a lock or atomic.

#include "action_dispatch_internal.h"
#include "council_pin.h"
#include "init_order.h"   // CK3_EARLY_INIT

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace bridge {

namespace cp = council_pin;

static std::shared_mutex g_council_pin_mutex;
static cp::PinSet        g_council_pins CK3_EARLY_INIT;
static std::atomic<bool> g_council_pins_nonempty{false};

// Trampoline to FUN_04dc9fc0(CCharacter* ruler, position* pos, u32 p3, taskDB* db).
static void* g_orig_council_select = nullptr;
using fn_council_select = void (*)(void* ruler_char, void* position, uint32_t p3, void* task_db);

static bool ruler_is_seated(uint32_t ruler_id) {
    std::lock_guard<std::mutex> lk(g_controlled_mutex);
    return std::any_of(g_controlled_chars.begin(), g_controlled_chars.end(),
                       [&](uint32_t c) { return (c & 0xFFFFFF) == (ruler_id & 0xFFFFFF); });
}

// =============================================================================
// Pin API (action_handlers.cpp arms; dispatch_router.cpp / bridge_init.cpp release)
// =============================================================================

const char* council_pin_arm(uint32_t ruler_id, uint32_t position_id,
                            bool have_position, bool readback_matches) {
    ruler_id &= 0xFFFFFF;
    cp::Arm a = cp::arm_verdict(have_position, readback_matches, ruler_is_seated(ruler_id));
    if (a == cp::Arm::kPinned) {
        std::unique_lock<std::shared_mutex> lk(g_council_pin_mutex);
        if (!g_council_pins.add(ruler_id, position_id)) a = cp::Arm::kRefused;
        g_council_pins_nonempty.store(!g_council_pins.empty(), std::memory_order_release);
    }
    ADLOG("council_pin: ruler %u position 0x%x -> %s\n", ruler_id, position_id, cp::arm_name(a));
    return cp::arm_name(a);
}

// Releases every pin of this ruler (release_character, the dead-ruler handoff,
// the outside-realm unbind). The name is kept from the per-ruler lock it replaced.
void council_lock_remove(uint32_t char_id) {
    size_t n = 0;
    {
        std::unique_lock<std::shared_mutex> lk(g_council_pin_mutex);
        n = g_council_pins.remove_ruler(char_id);
        g_council_pins_nonempty.store(!g_council_pins.empty(), std::memory_order_release);
    }
    if (n) ADLOG("council_pin: released %zu pin(s) of ruler %u\n", n, char_id & 0xFFFFFF);
}

// =============================================================================
// The hook
// =============================================================================

static void on_council_select(void* ruler_char, void* position, uint32_t p3, void* task_db) {
    auto orig = reinterpret_cast<fn_council_select>(g_orig_council_select);
    const bool any = g_council_pins_nonempty.load(std::memory_order_acquire);
    if (!any) { orig(ruler_char, position, p3, task_db); return; }

    const uintptr_t c = reinterpret_cast<uintptr_t>(ruler_char);
    const uintptr_t p = reinterpret_cast<uintptr_t>(position);
    const bool readable = c > 0x10000 && p > 0x10000 &&
        is_mapped_range(c + cp::kRulerIdOffset, sizeof(uint32_t)) &&
        is_mapped_range(p + cp::kPositionIdOffset, sizeof(uint32_t)) &&
        is_mapped_range(c + ck3::character::DEATH_DATE, sizeof(uintptr_t));
    uint32_t ruler_id = 0, position_id = 0;
    bool pinned = false, seated = false, dead = false;
    if (readable) {
        ruler_id = cp::ruler_id_at(ruler_char);
        position_id = cp::position_id_at(position);
        {
            std::shared_lock<std::shared_mutex> lk(g_council_pin_mutex);
            pinned = g_council_pins.has(ruler_id, position_id);
        }
        if (pinned) {
            dead = *reinterpret_cast<volatile uintptr_t*>(c + ck3::character::DEATH_DATE) != 0;
            seated = ruler_is_seated(ruler_id);
        }
    }
    const cp::Pass v = cp::decide(any, readable, pinned, seated, dead);
    if (v == cp::Pass::kNativeRulerDead) council_lock_remove(ruler_id);
    if (cp::skips(v)) {
        if (g_trace) ADLOG("council_pin: held ruler %u position 0x%x (native pass skipped)\n",
                           ruler_id, position_id);
        return;
    }
    orig(ruler_char, position, p3, task_db);
}

// =============================================================================
// Initialization (bridge_init, next to init_council_task_hook)
// =============================================================================

void init_council_suppress_hook() {
    uintptr_t target = bridge::game_fn(ck3::funcs::AI_COUNCIL_TASK_SELECT_FOR_POSITION);
    g_orig_council_select = install_hook(target, reinterpret_cast<void*>(on_council_select));
    if (g_orig_council_select) {
        ADLOG("council_pin_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_council_select);
    } else {
        ADLOG("council_pin_hook: FAILED to install at 0x%lx "
              "(no seat's council task is held; native behaviour unchanged)\n", target);
    }
}

} // namespace bridge
