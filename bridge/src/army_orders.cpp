// army_orders.cpp — a seat's orders to its own ruler's armies.
//
// NONE OF THIS HAS RUN AGAINST A LIVE GAME. Everything here is built from static
// evidence (re/findings/re_army_orders.md) and ships OFF:
//   CK3_BRIDGE_ARMY_ORDERS=1  army_disband / army_move / army_raid may touch the game;
//                             otherwise all three refuse army_orders_off before any
//                             engine contact.
//   CK3_BRIDGE_ARMY_HOLD=1    the GameCommandDispatch hook is installed at the first
//                             drain; otherwise it is never installed and army_hold
//                             refuses army_hold_off.
// The live checklist is live-checklist.md.
//
// army_disband  CDisbandArmyCommand built as the AI's own idle-army disband builds it
//               (FUN_04e5ab50: src 2), CanExecute with a std::string reason (the AI's
//               FUN_04e53060 passes one), then Execute on the game thread (the hire
//               precedent, "strategy B": live-proven for CHireMercenaryCompanyCommand).
// army_move     CMoveUnitCommand, plain form: src 2, the unit, no path, the mode the
//               game's UNIT_MOVE_MODE answers, the target province at +0x28; the
//               plain validator, then Execute, which pathfinds with pather mode 2.
// army_hold     the AI re-issues a stack's move every coordinator tick; a hold drops
//               the AI's (src 2) CMoveUnitCommand for a held unit at GameCommandDispatch,
//               the way the engine's own dispatch gate drops a command.
// emit_army_position (query_armies): where an army is, where its path ends, the next
//               province, a siege it is pressing (all safe_read; HIGH offsets).
#include "bridge.h"
#include "ck3_compiler.h"   // CK3_RETURN_ADDRESS
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"
#include "army_orders.h"
#include "armies_around.h"
#include "siege_read.h"
#include "engine_text.h"
#include "scope_slot_policy.h"   // The scope latch + its one refusal text

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bridge {
bool ai_veto_on_dispatch(void** cmd_pp, uint32_t flags, uintptr_t ra);   // ai_veto.cpp
namespace ao = army_orders;
namespace {

std::atomic<bool> g_orders_on{false};          // CK3_BRIDGE_ARMY_ORDERS, read once at init
std::atomic<bool> g_hold_hook_installed{false};
ao::HoldTable g_holds;
std::atomic<uint64_t> g_hold_drops{0};
void* g_orig_dispatch = nullptr;
uintptr_t g_move_vtable = 0;                   // runtime address of MOVE_UNIT_CMD_VTABLE

using fn_dispatch = uint64_t (*)(void* mgr, void** cmd_pp, uint32_t flags, uint32_t extra);

bool refuse(std::string* out, const char* action, const char* reason, const char* detail = nullptr) {
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string(action);
        r.key("reason"); r.value_string(reason);
        if (detail) { r.key("detail"); r.value_string(detail); }
        r.end_object();
        *out = r.take();
    }
    return false;
}

// The province with this id, through the game's own map (province_map: FUN_06061760 /
// FUN_0632cc20). 0 when the id is out of range or the read faults.
uintptr_t province_by_id(uint32_t id) {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) return 0;
    uintptr_t map_base = safe_read<uintptr_t>(gs + ck3::province_map::FROM_GAME_STATE);
    if (!map_base) return 0;
    uintptr_t map = map_base + ck3::province_map::MAP_OFFSET;
    int32_t count = safe_read<int32_t>(map + ck3::province_map::COUNT);
    uintptr_t arr = safe_read<uintptr_t>(map + ck3::province_map::ARRAY);
    if (!arr || count <= 0 || count > 100000 || id == 0 || static_cast<int32_t>(id) >= count) return 0;
    uintptr_t p = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(id) * 8);
    if (!p || !is_mapped(p)) return 0;
    if (safe_read<uint32_t>(p + ck3::province::PROVINCE_ID) != id) return 0;
    return p;
}

// A province's id (0 for none / the null scope / a faulted read).
uint32_t province_id_of(uintptr_t p) {
    if (!p || !is_mapped(p)) return 0;
    uint32_t id = safe_read<uint32_t>(p + ck3::province::PROVINCE_ID);
    return province_by_id(id) == p ? id : 0;
}

// The barony title key of a province ("" for a sea province or an unread one).
std::string barony_key_of(uintptr_t p) {
    if (!p || !is_mapped(p)) return {};
    uint32_t bid = safe_read<uint32_t>(p + ck3::province::BARONY_TITLE_ID);
    if (bid == 0xFFFFFFFF) return {};
    uintptr_t t = lookup_title_obj(bid);
    if (!t) return {};
    uintptr_t type = safe_read<uintptr_t>(t + ck3::title_obj::TITLE_TYPE_PTR);
    if (!type || !is_mapped(type)) return {};
    return ad_read_std_string(type + ck3::db_object::NAME_KEY);
}

// An army's unit (owner link), the army's full id checked both ways.
uintptr_t unit_of(uintptr_t army) {
    uint32_t link_id = safe_read<uint32_t>(army + ck3::army_obj::OWNER_LINK_ID);
    uintptr_t unit = ad_db_lookup(ck3::globals::OWNER_LINK_DB, link_id);
    if (!unit) return 0;
    if (safe_read<uint32_t>(unit + ck3::army_owner_link::BACK_REF_ARMY_ID) !=
        safe_read<uint32_t>(army + ck3::army_obj::ID)) return 0;
    return unit;
}

struct UnitPath { bool ok = false; int32_t count = 0; uint32_t next = 0; uint32_t last = 0; };
UnitPath unit_path(uintptr_t unit) {
    UnitPath p;
    int32_t n = safe_read<int32_t>(unit + ck3::army_owner_link::PATH_COUNT);
    if (n < 0 || n > 4096) return p;
    p.count = n;
    p.ok = true;
    if (n == 0) return p;
    uintptr_t nodes = safe_read<uintptr_t>(unit + ck3::army_owner_link::PATH_NODES);
    if (!nodes || !is_mapped(nodes)) { p.ok = false; return p; }
    uintptr_t first = safe_read<uintptr_t>(nodes);
    uintptr_t last = safe_read<uintptr_t>(nodes + static_cast<uintptr_t>(n - 1) * 8);
    if (!first || !last || !is_mapped(first) || !is_mapped(last)) { p.ok = false; return p; }
    p.next = safe_read<uint32_t>(first);
    p.last = safe_read<uint32_t>(last);
    return p;
}

int32_t army_men(uintptr_t army) { return army_men_and_tier(army, nullptr); }   // siege_read.cpp

// The army `army_id` (24-bit or full) if it is the actor's: its pointer, full id and unit.
struct OwnArmy { uintptr_t army = 0; uint32_t full_id = 0; uintptr_t unit = 0; uint32_t unit_id = 0; };
const char* own_army(uint32_t actor_id, int army_param, OwnArmy* o) {
    if (army_param <= 0) return "army_id_required";
    uintptr_t army = ad_db_lookup(ck3::globals::ARMY_DB, static_cast<uint32_t>(army_param));
    if (!army) return "army_not_found";
    uint32_t full = safe_read<uint32_t>(army + ck3::army_obj::ID);
    if ((full & 0xFFFFFF) != (static_cast<uint32_t>(army_param) & 0xFFFFFF)) return "army_not_found";
    uintptr_t unit = unit_of(army);
    if (!unit) return "army_unit_unread";
    uint32_t owner = safe_read<uint32_t>(unit + ck3::army_owner_link::OWNER_CHAR_ID);
    if ((owner & 0xFFFFFF) != (actor_id & 0xFFFFFF)) return "not_your_army";
    o->army = army; o->full_id = full; o->unit = unit;
    o->unit_id = safe_read<uint32_t>(unit + ck3::army_owner_link::ID);
    return nullptr;
}

// The target province: `province_id`, or `title` (a barony or county key; a county is
// read as its first barony, as map_graph reads a county's representative barony).
const char* target_province(const char* params_json, uintptr_t* prov_out, uint32_t* id_out) {
    int pid = json_get_int(params_json, "province_id");
    const char* key = json_get_string(params_json, "title");
    if (pid > 0) {
        uintptr_t p = province_by_id(static_cast<uint32_t>(pid));
        if (!p) return "province_not_found";
        *prov_out = p; *id_out = static_cast<uint32_t>(pid);
        return nullptr;
    }
    if (!key || !key[0]) return "province_required";
    uint32_t tid = resolve_title_id_by_key(key);
    if (tid == 0xFFFFFFFFu) return "province_not_found";
    uintptr_t t = lookup_title_obj(tid);
    if (!t) return "province_not_found";
    int tier = get_title_tier(t);
    if (tier == 2) {
        int32_t n = safe_read<int32_t>(t + ck3::title_obj::SUB_TITLE_COUNT);
        uintptr_t subs = safe_read<uintptr_t>(t + ck3::title_obj::SUB_TITLE_ARRAY);
        if (n <= 0 || !subs) return "province_not_found";
        t = lookup_title_obj(safe_read<uint32_t>(subs) & 0xFFFFFF);
        if (!t) return "province_not_found";
        tier = get_title_tier(t);
    }
    if (tier != 1) return "province_not_a_barony_or_county";
    uintptr_t p = safe_read<uintptr_t>(t + ck3::title_obj::PROVINCE_PTR);
    uint32_t id = province_id_of(p);
    if (!id) return "province_not_found";
    *prov_out = p; *id_out = id;
    return nullptr;
}

void emit_province(ResponseBuilder& r, const char* id_key, const char* name_key, uintptr_t p, uint32_t id) {
    r.key(id_key); r.value_uint(id);
    std::string k = barony_key_of(p);
    if (!k.empty()) { r.key(name_key); r.value_string(k.c_str(), k.size()); }
}

// ── the hold hook ───────────────────────────────────────────────────────────────
// GameCommandDispatch(mgr, cmd**, flags, extra). The AI posts every army move as a
// heap clone through it (flags 7). For an AI-sourced CMoveUnitCommand of a held unit
// the hook does what the engine's own dispatch gate does to a refused command (its
// disassembly at 0x076603e0): call the command's deleting dtor (vt[1]), null the
// caller's slot, return 0. The callers already handle a nulled slot (they delete it
// only when non-null). Everything else, and every doubt, goes to the original.
// No allocation, no lock, no safe_read syscall: is_mapped_range and volatile loads.
uint64_t on_game_command_dispatch(void* mgr, void** cmd_pp, uint32_t flags, uint32_t extra) {
    auto orig = reinterpret_cast<fn_dispatch>(g_orig_dispatch);
    // Per ai_veto.cpp: a seat's veto of the AI's create_title / law / mercenary /
    // construction / ally_call / perk posts. One atomic load while no ruler is watched; true =
    // dropped. This hook is entered by a jmp from the detoured entry, so its own return
    // address is the poster's (the game image or this .so: who posted).
    if (ai_veto_on_dispatch(cmd_pp, flags, CK3_RETURN_ADDRESS())) return 0;
    if (g_holds.count.load(std::memory_order_acquire) <= 0)
        return orig(mgr, cmd_pp, flags, extra);
    uintptr_t pp = reinterpret_cast<uintptr_t>(cmd_pp);
    if (!is_mapped_range(pp, sizeof(uintptr_t))) return orig(mgr, cmd_pp, flags, extra);
    uintptr_t cmd = *reinterpret_cast<volatile uintptr_t*>(pp);
    if (cmd < 0x10000 || !is_mapped_range(cmd, ck3::move_unit_cmd::UNIT_ID + 4))
        return orig(mgr, cmd_pp, flags, extra);
    uintptr_t vt = *reinterpret_cast<volatile uintptr_t*>(cmd);
    const bool is_move = g_move_vtable && vt == g_move_vtable;
    uint32_t src = is_move ? *reinterpret_cast<volatile uint32_t*>(cmd + ck3::move_unit_cmd::SOURCE) : 0;
    uint32_t unit = is_move ? *reinterpret_cast<volatile uint32_t*>(cmd + ck3::move_unit_cmd::UNIT_ID) : 0;
    int32_t now = 0;
    uintptr_t gsa = g_base_addr + ck3::globals::GAME_STATE_ROOT;
    if (is_move && is_mapped_range(gsa, sizeof(uintptr_t))) {
        uintptr_t gs = *reinterpret_cast<volatile uintptr_t*>(gsa);
        if (gs > 0x10000 && is_mapped_range(gs + ck3::game_state::GAME_DATE, 4))
            now = *reinterpret_cast<volatile int32_t*>(gs + ck3::game_state::GAME_DATE);
    }
    int slot = -1;
    if (ao::decide(g_holds, is_move, src, unit, now, &slot) != ao::Pass::kDrop)
        return orig(mgr, cmd_pp, flags, extra);
#if defined(_WIN32)
    // MSVC's scalar deleting destructor is vt[0] (this, flags); the hold is not
    // installed on Windows (init_army_orders), this keeps the shared hook's drop correct there
    if (!is_mapped_range(vt, sizeof(uintptr_t))) return orig(mgr, cmd_pp, flags, extra);
    reinterpret_cast<void* (*)(uintptr_t, uint32_t)>(*reinterpret_cast<volatile uintptr_t*>(vt))(cmd, 1);
#else
    if (!is_mapped_range(vt + 8, sizeof(uintptr_t))) return orig(mgr, cmd_pp, flags, extra);
    auto deleting_dtor = reinterpret_cast<void (*)(uintptr_t)>(*reinterpret_cast<volatile uintptr_t*>(vt + 8));
    deleting_dtor(cmd);
#endif
    *reinterpret_cast<volatile uintptr_t*>(pp) = 0;
    if (slot >= 0) g_holds.slot[slot].dropped.fetch_add(1, std::memory_order_relaxed);
    g_hold_drops.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

std::atomic<int> g_dispatch_hook_state{0};      // 0 not tried, 1 installed, 2 failed

}  // namespace

// The one GameCommandDispatch hook, shared by the army hold and the AI veto (ai_veto.cpp):
// installed at most once, on the game thread's first drain, by whichever asks first.
bool ensure_dispatch_hook() {
    // Windows installs it too (hooks.cpp's reviewed list; ck3_offsets_win.h
    // funcs::GAME_COMMAND_DISPATCH's hook review). Only the AI veto asks for it there: the
    // hold stays refused on Windows (init_army_orders).
    const int st = g_dispatch_hook_state.load(std::memory_order_acquire);
    if (st != 0) return st == 1;
    g_move_vtable = g_base_addr + ck3::globals::MOVE_UNIT_CMD_VTABLE;
    uintptr_t target = game_fn(ck3::funcs::GAME_COMMAND_DISPATCH);
    g_orig_dispatch = install_hook(target, reinterpret_cast<void*>(on_game_command_dispatch));
    g_dispatch_hook_state.store(g_orig_dispatch ? 1 : 2, std::memory_order_release);
    if (g_orig_dispatch)
        ADLOG("dispatch hook installed at 0x%lx (army hold and AI veto; inert until one is set)\n", target);
    else
        ADLOG("dispatch hook FAILED to install at 0x%lx\n", target);
    return g_orig_dispatch != nullptr;
}

// Read the switches; install the hold hook only when CK3_BRIDGE_ARMY_HOLD=1 (bridge_init,
// first drain). Off: nothing is installed for the hold and nothing here touches the game.
void init_army_orders() {
    g_orders_on.store(ao::switch_on(getenv("CK3_BRIDGE_ARMY_ORDERS")), std::memory_order_relaxed);
    ADLOG("army_orders: %s=%s (disband and move %s)\n", ao::kOrdersEnv,
          getenv("CK3_BRIDGE_ARMY_ORDERS") ? getenv("CK3_BRIDGE_ARMY_ORDERS") : "(unset)",
          g_orders_on.load() ? "ENABLED" : "refused: army_orders_off");
#if defined(_WIN32)
    ADLOG("army_orders: the hold hook is not reviewed for Windows; not installed\n");
#else
    if (!ao::switch_on(getenv("CK3_BRIDGE_ARMY_HOLD"))) {
        ADLOG("army_orders: %s is not 1: the hold hook is NOT installed (army_hold refuses)\n", ao::kHoldEnv);
        return;
    }
    const bool ok = ensure_dispatch_hook();
    g_hold_hook_installed.store(ok, std::memory_order_release);
    if (ok)
        ADLOG("army_orders: hold hook installed (inert until a hold is set)\n");
    else
        ADLOG("army_orders: hold hook FAILED to install (army_hold refuses)\n");
#endif
}

// query_armies: where the army is and where it is going (re_army_orders.md §4).
// ── Other rulers' armies where the game looks (armies_around.h) ─────────
namespace {
namespace aa = armies_around;

// One unit (owner link id) as a row, when it is another ruler's: its army, owner, men, province.
bool unit_row(uint32_t link_id, uint32_t me, aa::Row* row) {
    uintptr_t link = ad_db_lookup(ck3::globals::OWNER_LINK_DB, link_id);
    if (!link || safe_read<uint32_t>(link + ck3::army_owner_link::ID) != link_id) return false;
    uint32_t owner = safe_read<uint32_t>(link + ck3::army_owner_link::OWNER_CHAR_ID);
    if (!aa::foreign(owner, me)) return false;
    row->owner_id = owner & 0xFFFFFF;
    uint32_t army_id = safe_read<uint32_t>(link + ck3::army_owner_link::BACK_REF_ARMY_ID);
    uintptr_t army = ad_db_lookup(ck3::globals::ARMY_DB, army_id);
    if (army && safe_read<uint32_t>(army + ck3::army_obj::ID) == army_id) {
        row->army_id = army_id & 0xFFFFFF;
        row->men = army_men(army);
    }
    uintptr_t loc = safe_read<uintptr_t>(link + ck3::army_owner_link::LOCATION_PROV);
    row->province_id = province_id_of(loc);
    uintptr_t cd = row->province_id ? safe_read<uintptr_t>(loc + ck3::province::COUNTY_DATA_PTR) : 0;
    if (cd && is_mapped(cd)) row->county_id = safe_read<uint32_t>(cd + ck3::county::COUNTY_TITLE_ID) & 0xFFFFFF;
    return true;
}

void emit_rows(ResponseBuilder& r, const char* key, std::vector<aa::Row>& rows) {
    aa::order(&rows);
    r.key(key); r.begin_array();
    for (int i = 0; i < static_cast<int>(rows.size()) && i < aa::kRowsMax; ++i) {
        const aa::Row& x = rows[i];
        r.begin_object();
        if (x.army_id) { r.key("army_id"); r.value_uint(x.army_id); }
        r.key("owner_id"); r.value_uint(x.owner_id);
        if (x.men >= 0) { r.key("men"); r.value_int(x.men); } else { r.key("men"); r.value_string("unread"); }
        if (x.province_id) emit_province(r, "province_id", "barony", province_by_id(x.province_id), x.province_id);
        if (x.county_id) { r.key("county_id"); r.value_uint(x.county_id); }
        r.end_object();
    }
    r.end_array();
    r.key((std::string(key) + "_total").c_str()); r.value_int(static_cast<int>(rows.size()));
}
}  // namespace

// The units of other rulers in the county of `province` and the counties next to it: the set the game's
// disband check walks (ArmyDisbandCore). "unread" when the province's county is not read.
void emit_armies_around(ResponseBuilder& r, uintptr_t province, uint32_t me) {
    uintptr_t cd = province && is_mapped(province) ? safe_read<uintptr_t>(province + ck3::province::COUNTY_DATA_PTR) : 0;
    if (!cd || !is_mapped(cd)) { r.key("around"); r.value_string("unread"); return; }
    std::vector<uintptr_t> counties{cd};
    uintptr_t nb = safe_read<uintptr_t>(cd + ck3::county::NEIGHBOR_ARRAY);
    int32_t nn = safe_read<int32_t>(cd + ck3::county::NEIGHBOR_COUNT);
    if (nb && is_mapped(nb) && nn > 0)
        for (int32_t i = 0; i < nn && static_cast<int>(counties.size()) < aa::kCountiesMax; ++i) {
            uintptr_t c = safe_read<uintptr_t>(nb + static_cast<uintptr_t>(i) * 8);
            if (c && is_mapped(c)) counties.push_back(c);
        }
    std::vector<aa::Row> rows;
    for (uintptr_t c : counties) {
        uintptr_t pa = safe_read<uintptr_t>(c + ck3::county::PROVINCE_ARRAY);
        int32_t pn = safe_read<int32_t>(c + ck3::county::PROVINCE_COUNT);
        if (!pa || !is_mapped(pa) || pn <= 0) continue;
        for (int32_t j = 0; j < pn && j < aa::kProvincesMax; ++j) {
            uintptr_t p = safe_read<uintptr_t>(pa + static_cast<uintptr_t>(j) * 8);
            if (!p || !is_mapped(p)) continue;
            uintptr_t ids = safe_read<uintptr_t>(p + ck3::province::UNIT_IDS);
            int32_t un = safe_read<int32_t>(p + ck3::province::UNIT_COUNT);
            if (!ids || !is_mapped(ids) || un <= 0) continue;
            for (int32_t k = 0; k < un && k < aa::kUnitsMax; ++k) {
                aa::Row row;
                if (unit_row(safe_read<uint32_t>(ids + static_cast<uintptr_t>(k) * 4), me, &row)) rows.push_back(row);
            }
        }
    }
    r.key("around_counties"); r.value_int(static_cast<int>(counties.size()));
    emit_rows(r, "around", rows);
}

// query_armies `others_in_counties` ("<county title id>,..."): every other ruler's army standing in one of
// those counties. False (nothing emitted) when the list does not parse.
bool emit_others_in_counties(ResponseBuilder& r, const char* list, uint32_t me) {
    std::vector<uint32_t> want;
    if (!list || !aa::parse_ids(list, &want)) return false;
    std::vector<aa::Row> rows;
    using fn_army_enum = uintptr_t (*)(uintptr_t);
    auto army_enum = reinterpret_cast<fn_army_enum>(game_fn(ck3::funcs::ARMY_ENUM_SUB));
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::ARMY_ENUM_ENTRY);
    uintptr_t sub = gs ? army_enum(gs) : 0;
    uintptr_t id_array = sub ? safe_read<uintptr_t>(sub + ck3::army_enum::ID_ARRAY_PTR) : 0;
    int32_t id_count = sub ? safe_read<int32_t>(sub + ck3::army_enum::ID_ARRAY_COUNT) : 0;
    for (int32_t i = 0; id_array && i < id_count && i < 4000; ++i) {
        uint32_t army_id = safe_read<uint32_t>(id_array + static_cast<uintptr_t>(i) * 4);
        uintptr_t army = ad_db_lookup(ck3::globals::ARMY_DB, army_id);
        if (!army) continue;
        uintptr_t link = unit_of(army);
        if (!link) continue;
        uintptr_t loc = safe_read<uintptr_t>(link + ck3::army_owner_link::LOCATION_PROV);
        uintptr_t cd = loc && is_mapped(loc) ? safe_read<uintptr_t>(loc + ck3::province::COUNTY_DATA_PTR) : 0;
        if (!cd || !is_mapped(cd) || !aa::listed(want, safe_read<uint32_t>(cd + ck3::county::COUNTY_TITLE_ID))) continue;
        aa::Row row;
        if (unit_row(safe_read<uint32_t>(link + ck3::army_owner_link::ID), me, &row)) rows.push_back(row);
    }
    r.key("others_counties"); r.value_int(static_cast<int>(want.size()));
    emit_rows(r, "others", rows);
    return true;
}

void emit_army_position(ResponseBuilder& r, uintptr_t army) {
    uintptr_t unit = unit_of(army);
    if (!unit) { r.key("position"); r.value_string("unread"); return; }
    uintptr_t loc = safe_read<uintptr_t>(unit + ck3::army_owner_link::LOCATION_PROV);
    uint32_t loc_id = province_id_of(loc);
    if (loc_id) emit_province(r, "location_province_id", "location_barony", loc, loc_id);
    UnitPath p = unit_path(unit);
    if (p.ok) {
        r.key("path_steps"); r.value_int(p.count);
        if (p.count > 0) {
            emit_province(r, "destination_province_id", "destination_barony", province_by_id(p.last), p.last);
            r.key("next_province_id"); r.value_uint(p.next);
        }
    }
    // A siege the army is pressing: the siege on its location province whose attacker
    // is this army (province_siege_link::SIEGE_REF_0x368, siege_obj::ATTACKER_ARMY_ID).
    if (loc_id) {
        uint32_t sid = safe_read<uint32_t>(loc + ck3::province_siege_link::SIEGE_REF_0x368);
        if (sid != 0xFFFFFFFF) {
            uintptr_t s = ad_db_lookup_keyed(ck3::globals::SIEGE_DB, sid, ck3::siege_obj::ID);
            uint32_t army_id = safe_read<uint32_t>(army + ck3::army_obj::ID);
            if (s && safe_read<uint32_t>(s + ck3::siege_obj::ID) == sid &&
                safe_read<uint32_t>(s + ck3::siege_obj::ATTACKER_ARMY_ID) == army_id) {
                r.key("sieging"); r.value_bool(true);
                r.key("siege_id"); r.value_uint(sid);
                r.key("siege"); r.begin_object();
                emit_siege_view(r, s);   // (siege_read.cpp)
                r.end_object();
            }
        }
    }
    int slot = g_holds.find_army(safe_read<uint32_t>(army + ck3::army_obj::ID));
    if (slot >= 0) event_rules_emit_date(r, "held_until", g_holds.slot[slot].until_raw.load());
    // The raiding flag (ARMY_IS_RAIDING, HIGH live) the raid toggle flips
    r.key("raiding"); r.value_bool(safe_read<uint8_t>(army + ck3::funcs::ARMY_IS_RAIDING) != 0);
}

// army_disband {actor_id, army_id, check}: check=true asks the game (CanExecute with
// its reason) and writes nothing.
bool do_army_disband(const char* params_json, std::string* response_out) {
    FaultScope fault_scope("army_disband", response_out);
#if defined(_WIN32)
    return refuse(response_out, "army_disband", "unsupported_os");
#endif
    if (!g_orders_on.load(std::memory_order_relaxed))
        return refuse(response_out, "army_disband", "army_orders_off",
                      "the bridge's army orders are switched off (CK3_BRIDGE_ARMY_ORDERS is not 1); nothing was sent");
    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "army_disband", response_out)) return false;
    if (const char* why = actor_refusal(actor)) {
        if (response_out) *response_out = actor_refusal_reply("army_disband", why);
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID) & 0xFFFFFF;
    const bool check = json_get_bool(params_json, "check", false);
    OwnArmy a;
    if (const char* why = own_army(actor_id, json_get_int(params_json, "army_id"), &a))
        return refuse(response_out, "army_disband", why);
    const int32_t men_before = army_men(a.army);

    auto ctor = reinterpret_cast<void (*)(void*, uint32_t, uint32_t)>(game_fn(ck3::funcs::DISBAND_ARMY_CMD_CTOR));
    auto canexec = reinterpret_cast<uint8_t (*)(void*, std::string*)>(game_fn(ck3::funcs::DISBAND_ARMY_CMD_CANEXEC));
    auto execute = reinterpret_cast<void (*)(void*)>(game_fn(ck3::funcs::DISBAND_ARMY_CMD_EXECUTE));
    auto base_dtor = reinterpret_cast<void (*)(void*)>(game_fn(ck3::funcs::CJOMINI_COMMAND_BASE_DTOR));
    alignas(16) uint8_t cmd[0x40];   // the struct is 0x28 (disband_army_cmd::SIZE)
    memset(cmd, 0, sizeof(cmd));
    std::string reason;
    uint8_t can = 0;
    bool executed = false;

    // (G-commit): a read that faulted decides before the engine is touched.
    if (safe_read_had_fault()) return answer_memory_access_fault("army_disband", response_out);
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("army_disband");
        return false;
    }
    uint64_t saved_rng = 0; const bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false; watchdog_disarm();
        return refuse(response_out, "army_disband", "apply_crashed");
    }
    ctor(cmd, ao::kSourceAi, a.full_id);
    can = canexec(cmd, &reason);
    if (can && !check) { execute(cmd + ck3::disband_army_cmd::EXEC_THIS); executed = true; }
    base_dtor(cmd);
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    // The read back: is the army still in the army database under its full id?
    uintptr_t after = ad_db_lookup(ck3::globals::ARMY_DB, a.full_id);
    const bool present_after = after && safe_read<uint32_t>(after + ck3::army_obj::ID) == a.full_id;
    std::string why_text = clean_engine_text(reason);
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(can != 0);
        r.key("action"); r.value_string("army_disband");
        r.key("army_id"); r.value_uint(a.full_id & 0xFFFFFF);
        r.key("men"); r.value_int(men_before);
        r.key("check"); r.value_bool(check);
        r.key("allowed"); r.value_bool(can != 0);
        if (!can) {
            r.key("reason"); r.value_string("refused_by_game");
            r.key("game_reason"); r.value_string(why_text.c_str(), why_text.size());
            // (1068.03-12: twelve disbands refused "hostile Armies are around", no read naming
            // whose): the other rulers' units where the game's check looks, and where this army stands
            uintptr_t loc = safe_read<uintptr_t>(a.unit + ck3::army_owner_link::LOCATION_PROV);
            if (uint32_t lid = province_id_of(loc)) emit_province(r, "location_province_id", "location_barony", loc, lid);
            emit_armies_around(r, loc, actor_id);
        }
        r.key("applied"); r.value_bool(executed);
        r.key("army_after"); r.value_string(present_after ? "present" : "absent");
        r.key("note"); r.value_string(check ? "check only: the game's own disband check, nothing was sent"
                                            : (executed ? "disband executed; army_after is the army database read back"
                                                        : "the game refused; nothing was sent"));
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("army_disband: actor=%u army=0x%x check=%d can=%d executed=%d after=%s reason='%s'\n",
          actor_id, a.full_id, check ? 1 : 0, can, executed ? 1 : 0, present_after ? "present" : "absent",
          why_text.c_str());
    return can != 0;
}

// army_raid {actor_id, army_id, on, check} (re/findings/re_raid_toggle.md):
// the army window's START_RAIDING / END_RAIDING button. CToggleRaidingCommand built as the
// UI builds it (FUN_043093e0: ctor(cmd, src, character, army)) but with the AI's source (2,
// the disband precedent: the seat's ruler is AI-run); CanExecute with a std::string reason
// (the button's tooltip FUN_04309510 passes one), then Execute on the game thread. The
// command FLIPS the flag, so it is sent only when the army is not already in the state
// asked (on=1 raid, on=0 stop); the reply reads army_obj +0x1b4 (ARMY_IS_RAIDING) back.
// NOT RUN AGAINST A LIVE GAME; off with the other army orders (CK3_BRIDGE_ARMY_ORDERS).
bool do_army_raid(const char* params_json, std::string* response_out) {
    FaultScope fault_scope("army_raid", response_out);
#if defined(_WIN32)
    return refuse(response_out, "army_raid", "unsupported_os");
#endif
    if (!g_orders_on.load(std::memory_order_relaxed))
        return refuse(response_out, "army_raid", "army_orders_off",
                      "the bridge's army orders are switched off (CK3_BRIDGE_ARMY_ORDERS is not 1); nothing was sent");
    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "army_raid", response_out)) return false;
    if (const char* why = actor_refusal(actor)) {
        if (response_out) *response_out = actor_refusal_reply("army_raid", why);
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID) & 0xFFFFFF;
    const uint32_t actor_full = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID);
    const bool check = json_get_bool(params_json, "check", false);
    if (!json_has_key(params_json, "on"))
        return refuse(response_out, "army_raid", "raid_on_required", "on: 1 to raid, 0 to stop raiding");
    const int want_on = json_get_int(params_json, "on") != 0 ? 1 : 0;
    OwnArmy a;
    if (const char* why = own_army(actor_id, json_get_int(params_json, "army_id"), &a))
        return refuse(response_out, "army_raid", why);
    const bool raiding_before = safe_read<uint8_t>(a.army + ck3::funcs::ARMY_IS_RAIDING) != 0;
    if (!ao::raid_needs_toggle(want_on, raiding_before))
        return refuse(response_out, "army_raid", "already_in_that_state",
                      raiding_before ? "the army is already raiding; nothing was sent"
                                     : "the army is not raiding; nothing was sent");

    auto ctor = reinterpret_cast<void (*)(void*, uint32_t, uint32_t, uint32_t)>(
        game_fn(ck3::funcs::TOGGLE_RAIDING_CMD_CTOR));
    auto canexec = reinterpret_cast<uint8_t (*)(void*, std::string*)>(game_fn(ck3::funcs::TOGGLE_RAIDING_CMD_CANEXEC));
    auto execute = reinterpret_cast<void (*)(void*)>(game_fn(ck3::funcs::TOGGLE_RAIDING_CMD_EXECUTE));
    auto base_dtor = reinterpret_cast<void (*)(void*)>(game_fn(ck3::funcs::CJOMINI_COMMAND_BASE_DTOR));
    alignas(16) uint8_t cmd[0x40];   // the struct is 0x30 (toggle_raiding_cmd::SIZE)
    static_assert(sizeof(cmd) >= ck3::toggle_raiding_cmd::SIZE, "command buffer");
    memset(cmd, 0, sizeof(cmd));
    std::string reason;
    uint8_t can = 0;
    bool executed = false;

    if (safe_read_had_fault()) return answer_memory_access_fault("army_raid", response_out);
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("army_raid");
        return false;
    }
    uint64_t saved_rng = 0; const bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false; watchdog_disarm();
        return refuse(response_out, "army_raid", "apply_crashed");
    }
    // CanExecute resolves the ordering character from +0x24 by slot and compares the WHOLE
    // stored id (engine_id.h): he goes in as his whole id (+0x18), not its low 24 bits.
    ctor(cmd, ao::kSourceAi, actor_full, a.full_id);
    can = canexec(cmd, &reason);
    if (can && !check) { execute(cmd + ck3::toggle_raiding_cmd::EXEC_THIS); executed = true; }
    base_dtor(cmd);
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    uintptr_t after = ad_db_lookup(ck3::globals::ARMY_DB, a.full_id);
    const bool present_after = after && safe_read<uint32_t>(after + ck3::army_obj::ID) == a.full_id;
    const int raiding_after = present_after ? (safe_read<uint8_t>(after + ck3::funcs::ARMY_IS_RAIDING) != 0 ? 1 : 0) : -1;
    std::string why_text = clean_engine_text(reason);
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(can != 0);
        r.key("action"); r.value_string("army_raid");
        r.key("army_id"); r.value_uint(a.full_id & 0xFFFFFF);
        r.key("on"); r.value_bool(want_on != 0);
        r.key("check"); r.value_bool(check);
        r.key("allowed"); r.value_bool(can != 0);
        if (!can) {
            r.key("reason"); r.value_string("refused_by_game");
            r.key("game_reason"); r.value_string(why_text.c_str(), why_text.size());
        }
        r.key("applied"); r.value_bool(executed);
        r.key("raiding_before"); r.value_bool(raiding_before);
        r.key("raiding_after");
        if (raiding_after < 0) r.value_string("army_absent"); else r.value_bool(raiding_after != 0);
        r.key("note"); r.value_string(check ? "check only: the game's own raid-toggle check, nothing was sent"
                                            : (executed ? "raid toggle executed; raiding_after is the army's flag read back"
                                                        : "the game refused; nothing was sent"));
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("army_raid: actor=%u army=0x%x on=%d check=%d can=%d executed=%d before=%d after=%d reason='%s'\n",
          actor_id, a.full_id, want_on, check ? 1 : 0, can, executed ? 1 : 0, raiding_before ? 1 : 0,
          raiding_after, why_text.c_str());
    return can != 0;
}

// army_move {actor_id, army_id, province_id | title, check}: check=true asks the game's
// move validator and writes nothing.
bool do_army_move(const char* params_json, std::string* response_out) {
    FaultScope fault_scope("army_move", response_out);
#if defined(_WIN32)
    return refuse(response_out, "army_move", "unsupported_os");
#endif
    if (!g_orders_on.load(std::memory_order_relaxed))
        return refuse(response_out, "army_move", "army_orders_off",
                      "the bridge's army orders are switched off (CK3_BRIDGE_ARMY_ORDERS is not 1); nothing was sent");
    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "army_move", response_out)) return false;
    if (const char* why = actor_refusal(actor)) {
        if (response_out) *response_out = actor_refusal_reply("army_move", why);
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID) & 0xFFFFFF;
    const bool check = json_get_bool(params_json, "check", false);
    OwnArmy a;
    if (const char* why = own_army(actor_id, json_get_int(params_json, "army_id"), &a))
        return refuse(response_out, "army_move", why);
    uintptr_t target = 0; uint32_t target_id = 0;
    if (const char* why = target_province(params_json, &target, &target_id))
        return refuse(response_out, "army_move", why);
    uintptr_t here = safe_read<uintptr_t>(a.unit + ck3::army_owner_link::LOCATION_PROV);
    const uint32_t here_id = province_id_of(here);
    if (here_id == target_id) return refuse(response_out, "army_move", "already_there");

    auto in_combat = reinterpret_cast<bool (*)(uintptr_t)>(game_fn(ck3::funcs::ARMY_IN_COMBAT));
    auto move_mode = reinterpret_cast<uint8_t (*)(uintptr_t, uintptr_t, int)>(game_fn(ck3::funcs::UNIT_MOVE_MODE));
    auto ctor = reinterpret_cast<void (*)(void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t)>(
        game_fn(ck3::funcs::MOVE_UNIT_CMD_CTOR));
    auto canexec = reinterpret_cast<uint8_t (*)(void*, void*)>(game_fn(ck3::funcs::MOVE_UNIT_CMD_CANEXEC));
    auto execute = reinterpret_cast<void (*)(void*)>(game_fn(ck3::funcs::MOVE_UNIT_CMD_EXECUTE));
    auto dtor = reinterpret_cast<void (*)(void*)>(game_fn(ck3::funcs::MOVE_UNIT_CMD_DTOR));
    alignas(16) uint8_t cmd[0x200];                       // the struct is 0x168 (move_unit_cmd::SIZE)
    alignas(16) uint8_t empty_path[0x140];                // the ctor reads +0x00, +0x0C, +0x128
    memset(cmd, 0, sizeof(cmd));
    memset(empty_path, 0, sizeof(empty_path));
    bool fighting = false;
    uint8_t mode = ao::kModeRefused;
    uint8_t can = 0;
    bool executed = false;

    if (safe_read_had_fault()) return answer_memory_access_fault("army_move", response_out);
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        if (response_out) *response_out = bridge::scope_latched_refusal_json("army_move");
        return false;
    }
    uint64_t saved_rng = 0; const bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false; watchdog_disarm();
        return refuse(response_out, "army_move", "apply_crashed");
    }
    fighting = in_combat(a.army);
    if (!fighting) {
        mode = move_mode(a.unit, target, 1);              // as the AI asks (FUN_04e53060)
        if (mode != ao::kModeRefused) {
            ctor(cmd, ao::kSourceAi, a.unit_id, empty_path, mode, 1);
            *reinterpret_cast<uint32_t*>(cmd + ck3::move_unit_cmd::TARGET_PROVINCE) = target_id;
            can = canexec(cmd, nullptr);
            if (can && !check) { execute(cmd); executed = true; }
            dtor(cmd);
        }
    }
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    const int32_t before_faults = safe_read_fault_count();
    UnitPath p = unit_path(a.unit);
    uint32_t loc_after = province_id_of(safe_read<uintptr_t>(a.unit + ck3::army_owner_link::LOCATION_PROV));
    const bool read_ok = p.ok && safe_read_fault_count() == before_faults;
    ao::MoveAfter after = ao::move_after(read_ok, p.count, p.last, loc_after, target_id);
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        const bool ok = !fighting && mode != ao::kModeRefused && can;
        r.key("success"); r.value_bool(ok);
        r.key("action"); r.value_string("army_move");
        r.key("army_id"); r.value_uint(a.full_id & 0xFFFFFF);
        emit_province(r, "from_province_id", "from_barony", here, here_id);
        emit_province(r, "target_province_id", "target_barony", target, target_id);
        r.key("check"); r.value_bool(check);
        if (fighting) { r.key("reason"); r.value_string("in_combat"); }
        else if (mode == ao::kModeRefused) { r.key("reason"); r.value_string("cannot_be_ordered_there"); }
        else if (!can) { r.key("reason"); r.value_string("refused_by_game"); }
        r.key("allowed"); r.value_bool(ok);
        r.key("applied"); r.value_bool(executed);
        r.key("after"); r.value_string(ao::move_after_reply(check, after));   // A check sent nothing
        if (p.ok) { r.key(ao::move_path_key(check)); r.value_int(p.count); }
        r.key("held"); r.value_bool(g_holds.find_army(a.full_id) >= 0);
        r.key("note"); r.value_string(check ? "check only: the game's own move validator, nothing was sent"
                                            : "the game's AI re-orders a stack it commands within days unless the army is held (army_hold)");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("army_move: actor=%u army=0x%x unit=0x%x %u->%u check=%d fighting=%d mode=%u can=%d executed=%d after=%s\n",
          actor_id, a.full_id, a.unit_id, here_id, target_id, check ? 1 : 0, fighting ? 1 : 0, mode, can,
          executed ? 1 : 0, ao::move_after_name(after));
    return !fighting && mode != ao::kModeRefused && can;
}

// army_hold {actor_id, op: hold|release|holds, army_id, days}: the hold table the
// dispatch hook reads. No engine call: reads, and atomics the hook loads.
bool do_army_hold(const char* params_json, std::string* response_out) {
    FaultScope fault_scope("army_hold", response_out);
    const char* op_s = json_get_string(params_json, "op");
    ao::Op op = ao::op_by_name(op_s ? op_s : "");
    if (!ao::op_needs_hold(op)) return refuse(response_out, "army_hold", "unknown_op", "hold, release or holds");
    if (!g_hold_hook_installed.load(std::memory_order_acquire))
        return refuse(response_out, "army_hold", "army_hold_off",
                      "the hold hook is not installed (CK3_BRIDGE_ARMY_HOLD is not 1, or the install failed); nothing was held");
    void* actor = resolve_actor(params_json);
    if (reject_player_actor(actor, "army_hold", response_out)) return false;
    if (const char* why = actor_refusal(actor)) {
        if (response_out) *response_out = actor_refusal_reply("army_hold", why);
        return false;
    }
    uint32_t actor_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID) & 0xFFFFFF;
    const int32_t now = event_rules_game_date_raw();
    if (safe_read_had_fault()) return answer_memory_access_fault("army_hold", response_out);
    g_holds.prune(now);
    if (op == ao::Op::kHolds) {
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(true);
            r.key("action"); r.value_string("army_hold");
            r.key("holds"); r.begin_array();
            for (int i = 0; i < ao::kMaxHolds; ++i) {
                const ao::HoldSlot& s = g_holds.slot[i];
                if (s.unit.load() == ao::kEmpty || (s.owner.load() & 0xFFFFFF) != actor_id) continue;
                r.begin_object();
                r.key("army_id"); r.value_uint(s.army.load() & 0xFFFFFF);
                event_rules_emit_date(r, "until", s.until_raw.load());
                r.key("ai_orders_dropped"); r.value_uint(s.dropped.load());
                r.end_object();
            }
            r.end_array();
            r.key("ai_orders_dropped_total"); r.value_uint(g_hold_drops.load());
            r.end_object();
            *response_out = r.take();
        }
        return true;
    }
    OwnArmy a;
    if (const char* why = own_army(actor_id, json_get_int(params_json, "army_id"), &a))
        return refuse(response_out, "army_hold", why);
    if (op == ao::Op::kRelease) {
        const bool was = g_holds.release_army(a.full_id);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(true);
            r.key("action"); r.value_string("army_hold");
            r.key("op"); r.value_string("release");
            r.key("army_id"); r.value_uint(a.full_id & 0xFFFFFF);
            r.key("was_held"); r.value_bool(was);
            r.end_object();
            *response_out = r.take();
        }
        return true;
    }
    if (!json_has_key(params_json, "days")) return refuse(response_out, "army_hold", "days_required", "1..365");
    const int days = json_get_int(params_json, "days");
    if (!ao::hold_days_ok(days)) return refuse(response_out, "army_hold", "bad_days", "1..365");
    if (now <= 0) return refuse(response_out, "army_hold", "date_unread");
    const int32_t until = now + days * ao::kTicksPerDay;
    if (g_holds.set(a.unit_id, a.full_id, actor_id, until) < 0)
        return refuse(response_out, "army_hold", "hold_table_full", "16 armies at most; release one");
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("army_hold");
        r.key("op"); r.value_string("hold");
        r.key("army_id"); r.value_uint(a.full_id & 0xFFFFFF);
        event_rules_emit_date(r, "until", until);
        r.key("note"); r.value_string("the AI's move orders for this army are dropped until then; "
                                      "it still fights, retreats, splits, merges and can be disbanded by the AI; "
                                      "it sieges an enemy holding it stands on, but will not march to one");
        r.end_object();
        *response_out = r.take();
    }
    ADLOG("army_hold: actor=%u army=0x%x unit=0x%x days=%d\n", actor_id, a.full_id, a.unit_id, days);
    return true;
}

}  // namespace bridge
