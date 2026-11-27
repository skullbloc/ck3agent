// siege_read.cpp — one siege's reads. All safe_read; no engine call.
//
// What the game does, decompiled (siege_view.h has the formulas): the progress so far is
// CSiege+0x110; the need is 100 + 75 x the province's fort level (x the garrison's fill);
// the daily verdict is CSiege+0x18; an army counts toward the besiegers' men when its map
// unit stands on the province, is not locked (+0x14 == 0) or busy (+0x168 < 1), is not
// marching on (a path, +0x168 != 1) and is not in battle (FUN_063136d0 strict; the hostility
// test FUN_068349e0 is not read: every army on the holding passing the rest is counted,
// which matched the game's verdict on 64/64 sieges of the 1097 dump).
#include "bridge.h"
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"
#include "siege_read.h"

#include <cstdint>

namespace bridge {

int32_t army_men_and_tier(uintptr_t army, int* siege_tier_out) {
    if (siege_tier_out) *siege_tier_out = 0;
    uintptr_t reg_vec = safe_read<uintptr_t>(army + ck3::army_obj::REG_VEC);
    int32_t reg_count = safe_read<int32_t>(army + ck3::army_obj::REG_COUNT);
    if (!reg_vec || reg_count <= 0 || reg_count > 5000) return 0;
    int32_t men = 0;
    int tier = 0;
    for (int32_t j = 0; j < reg_count; ++j) {
        uint32_t rid = safe_read<uint32_t>(reg_vec + static_cast<uintptr_t>(j) * 4);
        if (rid == 0 || rid == 0xFFFFFFFF) continue;
        uintptr_t reg = ad_db_lookup(ck3::globals::ARMY_REGIMENT_DB, rid);
        if (!reg || safe_read<uint32_t>(reg + ::army_regiment::ID) != rid) continue;
        men += safe_read<int32_t>(reg + ::army_regiment::MEN_CURRENT);
        uintptr_t type = safe_read<uintptr_t>(reg + ::army_regiment::TYPE_PTR);
        if (type && is_mapped(type)) {
            int t = safe_read<int32_t>(type + ::maa_type::SIEGE_TIER);
            if (t > tier && t < 16) tier = t;
        }
    }
    if (siege_tier_out) *siege_tier_out = tier;
    return men;
}

namespace {

// The garrison's men now: the holding's garrison CRegiment, its first chunk (FUN_06363c50).
int garrison_men(uintptr_t prov) {
    uint32_t gid = safe_read<uint32_t>(prov + ck3::province_holding::HOLDING
                                       + ck3::province_holding::GARRISON_REGIMENT);
    if (gid == 0xFFFFFFFFu) return 0;
    uintptr_t reg = ad_db_lookup(ck3::globals::REGIMENT_DB, gid);
    if (!reg || safe_read<uint32_t>(reg + ::regiment::ID) != gid) return -1;
    uintptr_t chunk = reg + ::regiment::CHUNK_BASE;
    int32_t allocated = safe_read<int32_t>(chunk + 0x00);
    int32_t current   = safe_read<int32_t>(chunk + 0x04);
    int32_t status    = safe_read<int32_t>(chunk + 0x18);
    return (status != 3 || current != 0) ? current : allocated;
}

bool in_battle(uintptr_t army) {
    uint32_t cid = safe_read<uint32_t>(army + ck3::army_obj::COMBAT_REF_ID);
    if (cid == 0xFFFFFFFFu) return false;
    uintptr_t c = ad_db_lookup_keyed(ck3::globals::COMBAT_DB, cid, 0x08);
    return c && safe_read<uint32_t>(c + 0x08) == cid;
}

}  // namespace

bool read_siege(uintptr_t siege, siege_view::Reads* out) {
    if (!out || !siege || !is_mapped(siege)) return false;
    siege_view::Reads s;
    uintptr_t prov = safe_read<uintptr_t>(siege + ck3::siege_obj::PROVINCE_PTR);
    if (!prov || !is_mapped(prov)) return false;
    s.progress = static_cast<double>(safe_read<int64_t>(siege + ck3::siege_obj::PROGRESS)) / 100000.0;
    s.phase_days = static_cast<double>(safe_read<int64_t>(siege + ck3::siege_obj::PHASE_DAYS)) / 100000.0;
    s.ticked = s.phase_days > 0;
    s.can_progress = safe_read<uint8_t>(siege + ck3::siege_obj::CAN_PROGRESS) != 0;
    s.breach = safe_read<int32_t>(siege + ck3::siege_obj::ACTION_LEVELS);
    if (s.breach < 0 || s.breach > 2) s.breach = 0;
    s.day_in_phase = safe_read<int32_t>(siege + ck3::siege_obj::DAY_IN_PHASE);
    s.assault = safe_read<uint8_t>(siege + ck3::siege_obj::ASSAULT) != 0;
    int fort = safe_read<int32_t>(prov + ck3::province_holding::FORT_LEVEL);
    s.fort_level = (fort >= 0 && fort < 100) ? fort : -1;
    s.garrison = garrison_men(prov);

    // The armies on the holding (province::UNIT_IDS: owner-link ids of the map units there).
    uintptr_t ids = safe_read<uintptr_t>(prov + ck3::province::UNIT_IDS);
    int32_t n = safe_read<int32_t>(prov + ck3::province::UNIT_COUNT);
    if (n < 0 || n > 256) n = 0;
    int men = 0, tier = 0;
    for (int32_t i = 0; ids && i < n; ++i) {
        uint32_t lid = safe_read<uint32_t>(ids + static_cast<uintptr_t>(i) * 4);
        uintptr_t link = ad_db_lookup(ck3::globals::OWNER_LINK_DB, lid);
        if (!link || safe_read<uint32_t>(link + ck3::army_owner_link::ID) != lid) continue;
        if (safe_read<uintptr_t>(link + ck3::army_owner_link::LOCATION_PROV) != prov) continue;
        uint32_t aid = safe_read<uint32_t>(link + ck3::army_owner_link::BACK_REF_ARMY_ID);
        uintptr_t army = ad_db_lookup(ck3::globals::ARMY_DB, aid);
        if (!army || safe_read<uint32_t>(army + ck3::army_obj::ID) != aid) continue;
        int t = 0;
        int m = army_men_and_tier(army, &t);
        const int32_t busy = safe_read<int32_t>(link + ck3::army_owner_link::TROOP_COUNT);   // +0x168, misnamed
        const bool moving = safe_read<int32_t>(link + ck3::army_owner_link::PATH_COUNT) != 0 && busy != 1;
        if (in_battle(army)) { s.men_in_battle += m; continue; }
        if (moving) { s.men_moving += m; continue; }
        if (safe_read<int32_t>(link + ck3::army_owner_link::ORDER_LOCK) != 0 || busy >= 1) continue;
        men += m;
        if (t > tier) tier = t;
    }
    s.men = men;
    s.siege_tier = tier;
    *out = s;
    return true;
}

void emit_siege_view(ResponseBuilder& r, uintptr_t siege) {
    siege_view::Reads s;
    if (!read_siege(siege, &s)) { r.key("progress_unread"); r.value_bool(true); return; }
    const siege_view::Verdict v = siege_view::judge(s);
    r.key("status"); r.value_string(v.status);
    if (v.blocker) { r.key("blocker"); r.value_string(v.blocker); }
    r.key("progress"); r.value_double(static_cast<double>(static_cast<int64_t>(s.progress * 10 + 0.5)) / 10);
    if (v.needed_max > 0) {
        r.key("progress_needed_max"); r.value_double(v.needed_max);
        r.key("progress_pct"); r.value_int(static_cast<int64_t>(v.pct + 0.5));
        r.key("fort_level"); r.value_int(s.fort_level);
        r.key("tier_needed"); r.value_int(v.tier_needed);
        r.key("fort_speed"); r.value_double(static_cast<double>(static_cast<int64_t>(v.speed * 100 + 0.5)) / 100);
    }
    if (s.garrison >= 0) { r.key("garrison"); r.value_int(s.garrison); }
    r.key("men"); r.value_int(s.men);
    if (s.men_in_battle) { r.key("men_in_battle"); r.value_int(s.men_in_battle); }
    if (s.men_moving) { r.key("men_moving"); r.value_int(s.men_moving); }
    r.key("siege_tier"); r.value_int(s.siege_tier);
    r.key("walls"); r.value_int(s.breach);
    if (v.next_event_days >= 0) { r.key("next_event_days"); r.value_int(v.next_event_days); }
    if (v.eta_days >= 0) { r.key("eta_days"); r.value_int(v.eta_days); }
    if (s.assault) { r.key("assault"); r.value_bool(true); }
}

}  // namespace bridge
