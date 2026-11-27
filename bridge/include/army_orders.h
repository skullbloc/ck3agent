// army_orders.h — a seat's orders to its own ruler's armies.
//
// Three levers, each the game's own machinery (re/findings/re_army_orders.md):
//   disband  CDisbandArmyCommand: ctor(cmd, src 2, army id), CanExecute with a
//            std::string reason (ArmyDisbandCore), Execute. Built exactly as the
//            AI's own idle-army disband builds it (FUN_04e5ab50, src 2).
//   move     CMoveUnitCommand, the plain form: ctor(cmd, src 2, unit id, empty
//            path, mode, 1), then the target province id at +0x28; CanExecute;
//            Execute pathfinds with the same pather mode the AI uses (2).
//   hold     the AI re-issues a stack's order every coordinator tick
//            (FUN_04e42e90 -> FUN_04e2a6c0 -> CMoveUnitCommand src 2 ->
//            GameCommandDispatch(.., 7, 0)). A hold drops exactly those AI move
//            commands for a held unit at GameCommandDispatch, the way the engine's
//            own dispatch gate drops a command (deleting dtor, slot nulled, 0).
//
// SWITCHES, BOTH DEFAULT OFF (nothing here has run against a live game yet):
//   CK3_BRIDGE_ARMY_ORDERS=1  enables disband and move (otherwise refused before any
//                             engine contact: army_orders_off)
//   CK3_BRIDGE_ARMY_HOLD=1    installs the GameCommandDispatch hook at the first drain
//                             (otherwise the hook is never installed and hold refuses:
//                             army_hold_off)
// Any other value, or unset, is off. Exactly "1" is on.
//
// Header-only and engine-free: test_army_orders.cpp checks the switch, the hold
// table and the drop decision on a host with no game.
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace bridge::army_orders {

inline constexpr const char* kOrdersEnv = "CK3_BRIDGE_ARMY_ORDERS";
inline constexpr const char* kHoldEnv = "CK3_BRIDGE_ARMY_HOLD";

//: On only when the variable is exactly "1"; unset, "", "0", "yes", "1 " are off.
inline bool switch_on(const char* env_value) {
    return env_value && env_value[0] == '1' && env_value[1] == '\0';
}

//: The command source the AI stamps on its own army commands (FUN_04e5ab50,
//: FUN_04e53060, FUN_04e2a6e0: ctor(.., 2, ..)). 1 = the player's UI, 3 = unchecked.
inline constexpr uint32_t kSourceAi = 2;

enum class Op : uint8_t { kNone = 0, kDisband, kMove, kHold, kRelease, kHolds, kRaid };
inline Op op_by_name(std::string_view n) {
    if (n == "disband") return Op::kDisband;
    if (n == "raid") return Op::kRaid;
    if (n == "move") return Op::kMove;
    if (n == "hold") return Op::kHold;
    if (n == "release") return Op::kRelease;
    if (n == "holds") return Op::kHolds;
    return Op::kNone;
}
inline const char* op_name(Op o) {
    switch (o) {
    case Op::kDisband: return "disband";
    case Op::kMove: return "move";
    case Op::kHold: return "hold";
    case Op::kRelease: return "release";
    case Op::kHolds: return "holds";
    case Op::kRaid: return "raid";
    case Op::kNone: break;
    }
    return "none";
}
//: The ops that write to the game (and so need CK3_BRIDGE_ARMY_ORDERS).
inline bool op_writes_game(Op o) { return o == Op::kDisband || o == Op::kMove || o == Op::kRaid; }

//: raid: CToggleRaidingCommand FLIPS the army's raiding flag (army_obj +0x1b4;
//: its Execute stops a raiding army and starts one that is not). The seat names the state it
//: wants (on = 1 start, 0 stop), so the toggle is sent only when the army is not already there.
inline bool raid_needs_toggle(int want_on, bool raiding_now) { return (want_on != 0) != raiding_now; }
//: The ops that need the hold hook (CK3_BRIDGE_ARMY_HOLD).
inline bool op_needs_hold(Op o) { return o == Op::kHold || o == Op::kRelease || o == Op::kHolds; }

//: A hold lasts 1..365 game days (a forgotten hold must end: the AI owns the army
//: again when it does). 24 sub-day ticks a day (game_state::TICKS_PER_DAY).
inline constexpr int kMaxHoldDays = 365;
inline constexpr int32_t kTicksPerDay = 24;
inline bool hold_days_ok(int d) { return d >= 1 && d <= kMaxHoldDays; }

//: CMoveUnitCommand's mode (UNIT_MOVE_MODE, FUN_06688fd0): 3 is "this unit cannot be
//: ordered there"; the plain validator FUN_066886d0 refuses it first.
inline constexpr uint32_t kModeRefused = 3;

// ---------------------------------------------------------------------------
// The hold table: fixed slots of atomics. The game thread writes (under the
// handler's own serialisation: the drain runs one message at a time); the hook
// reads from whatever thread posts an AI command, without a lock.
// ---------------------------------------------------------------------------
inline constexpr int kMaxHolds = 16;
inline constexpr uint32_t kEmpty = 0;   // unit id 0 is the null link; never a held unit

struct HoldSlot {
    std::atomic<uint32_t> unit{kEmpty};     // the army's unit (owner-link) id, full 32-bit
    std::atomic<uint32_t> army{0};          // the army id, full 32-bit (for the read)
    std::atomic<uint32_t> owner{0};         // the ruler who set it (24-bit)
    std::atomic<int32_t> until_raw{0};      // game date raw the hold ends at
    std::atomic<uint32_t> dropped{0};       // AI move commands dropped for it
};

struct HoldTable {
    HoldSlot slot[kMaxHolds];
    std::atomic<int> count{0};              // live slots: the hook's fast path

    //: Add or renew. Returns the slot, or -1 when the table is full.
    int set(uint32_t unit, uint32_t army, uint32_t owner, int32_t until_raw) {
        if (unit == kEmpty) return -1;
        int free_slot = -1;
        for (int i = 0; i < kMaxHolds; ++i) {
            uint32_t u = slot[i].unit.load(std::memory_order_acquire);
            if (u == unit) {
                slot[i].until_raw.store(until_raw, std::memory_order_release);
                slot[i].army.store(army, std::memory_order_relaxed);
                slot[i].owner.store(owner, std::memory_order_relaxed);
                return i;
            }
            if (u == kEmpty && free_slot < 0) free_slot = i;
        }
        if (free_slot < 0) return -1;
        HoldSlot& s = slot[free_slot];
        s.army.store(army, std::memory_order_relaxed);
        s.owner.store(owner, std::memory_order_relaxed);
        s.dropped.store(0, std::memory_order_relaxed);
        s.until_raw.store(until_raw, std::memory_order_release);
        s.unit.store(unit, std::memory_order_release);   // published last
        count.fetch_add(1, std::memory_order_acq_rel);
        return free_slot;
    }
    //: Release by army id (full or 24-bit). Returns true when one was held.
    bool release_army(uint32_t army) {
        for (int i = 0; i < kMaxHolds; ++i) {
            if (slot[i].unit.load(std::memory_order_acquire) == kEmpty) continue;
            if ((slot[i].army.load(std::memory_order_relaxed) & 0xFFFFFF) != (army & 0xFFFFFF)) continue;
            slot[i].unit.store(kEmpty, std::memory_order_release);
            count.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
        return false;
    }
    //: Expired slots are cleared by the game thread (prune), never by the hook.
    int prune(int32_t now_raw) {
        int n = 0;
        for (int i = 0; i < kMaxHolds; ++i) {
            if (slot[i].unit.load(std::memory_order_acquire) == kEmpty) continue;
            if (now_raw > 0 && now_raw >= slot[i].until_raw.load(std::memory_order_acquire)) {
                slot[i].unit.store(kEmpty, std::memory_order_release);
                count.fetch_sub(1, std::memory_order_acq_rel);
                ++n;
            }
        }
        return n;
    }
    //: The slot holding this unit (not expired at now_raw), or -1.
    int find_unit(uint32_t unit, int32_t now_raw) const {
        if (unit == kEmpty) return -1;
        for (int i = 0; i < kMaxHolds; ++i) {
            if (slot[i].unit.load(std::memory_order_acquire) != unit) continue;
            int32_t until = slot[i].until_raw.load(std::memory_order_acquire);
            if (now_raw > 0 && now_raw >= until) return -1;   // expired: the AI's again
            return i;
        }
        return -1;
    }
    int find_army(uint32_t army) const {
        for (int i = 0; i < kMaxHolds; ++i) {
            if (slot[i].unit.load(std::memory_order_acquire) == kEmpty) continue;
            if ((slot[i].army.load(std::memory_order_relaxed) & 0xFFFFFF) == (army & 0xFFFFFF)) return i;
        }
        return -1;
    }
};

// ---------------------------------------------------------------------------
// The hook's decision on one dispatched command. Every doubt passes the command
// through (the AI's order stands): a wrong drop strands an army, a wrong pass
// only lets the AI move it.
// ---------------------------------------------------------------------------
enum class Pass : uint8_t {
    kDrop = 0,        // an AI move order for a held unit: dropped
    kNoHolds,         // the table is empty (the fast path)
    kNotMoveCommand,  // another command class
    kNotAiSource,     // src != 2 (the UI's, or unchecked)
    kNotHeld,         // a unit nobody holds
    kExpired,         // held, but the hold has ended
};
inline const char* pass_name(Pass p) {
    switch (p) {
    case Pass::kDrop: return "dropped";
    case Pass::kNoHolds: return "no_holds";
    case Pass::kNotMoveCommand: return "not_move_command";
    case Pass::kNotAiSource: return "not_ai_source";
    case Pass::kNotHeld: return "not_held";
    case Pass::kExpired: return "expired";
    }
    return "?";
}
inline Pass decide(const HoldTable& t, bool is_move_command, uint32_t src, uint32_t unit, int32_t now_raw,
                   int* slot_out = nullptr) {
    if (t.count.load(std::memory_order_acquire) <= 0) return Pass::kNoHolds;
    if (!is_move_command) return Pass::kNotMoveCommand;
    if (src != kSourceAi) return Pass::kNotAiSource;
    for (int i = 0; i < kMaxHolds; ++i) {
        if (t.slot[i].unit.load(std::memory_order_acquire) != unit || unit == kEmpty) continue;
        if (now_raw > 0 && now_raw >= t.slot[i].until_raw.load(std::memory_order_acquire)) return Pass::kExpired;
        if (slot_out) *slot_out = i;
        return Pass::kDrop;
    }
    return Pass::kNotHeld;
}

// ---------------------------------------------------------------------------
// The reads after a write: what the reply may claim.
// ---------------------------------------------------------------------------
//: A move is confirmed when the unit's path (owner link +0x30/+0x3c) now ends at
//: the target. `path_count` 0 with the unit already there is "arrived"; 0 elsewhere
//: is "no path" (the pathfinder found none, or the order did not take).
enum class MoveAfter : uint8_t { kConfirmed = 0, kAlreadyThere, kNoPath, kOtherDestination, kUnread };
inline MoveAfter move_after(bool read_ok, int32_t path_count, uint32_t last_node, uint32_t location,
                            uint32_t target) {
    if (!read_ok) return MoveAfter::kUnread;
    if (path_count <= 0) return location == target ? MoveAfter::kAlreadyThere : MoveAfter::kNoPath;
    return last_node == target ? MoveAfter::kConfirmed : MoveAfter::kOtherDestination;
}
inline const char* move_after_name(MoveAfter m) {
    switch (m) {
    case MoveAfter::kConfirmed: return "path_ends_at_target";
    case MoveAfter::kAlreadyThere: return "already_there";
    case MoveAfter::kNoPath: return "no_path";
    case MoveAfter::kOtherDestination: return "other_destination";
    case MoveAfter::kUnread: return "unread";
    }
    return "?";
}
//: (a test run: `check=true` answered `after: no_path, path_steps 0` for the move
//: that, sent, answered `path_ends_at_target, 13 steps`): a check sends nothing, so the path read after it is the one
//: the unit already had, not the one the order would take. A check says so instead of a path verdict, and the path it
//: read is named for what it is (current_path_steps). The game's own validator answers in `allowed`.
inline constexpr const char* kCheckAfter = "not_sent";
inline const char* move_after_reply(bool check, MoveAfter m) { return check ? kCheckAfter : move_after_name(m); }
inline const char* move_path_key(bool check) { return check ? "current_path_steps" : "path_steps"; }

}  // namespace bridge::army_orders
