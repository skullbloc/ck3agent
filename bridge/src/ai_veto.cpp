// ai_veto.cpp — a seat's veto over what the game's AI posts for its ruler.
//
// The game's AI acts for an AI-run ruler between the seat's turns. Four of its decisions
// spend the ruler's purse or standing and are posted as typed commands through
// GameCommandDispatch(mgr, &clone, 7, 0) (re/findings/re_ai_veto.md, bridge/include/ai_veto.h):
// title creation, law changes, mercenary hires, constructions; and the AI's
// ally calls (CSendCharacterInteractionCommand carrying call_ally_interaction) and its
// lifestyle perk picks (CAddLifestylePerkCommand); the AI's decisions
// (CExecuteDecisionCommand: the Decisions pass, the decision goal), optionally only named ones,
// told from the seat's own take_decision by the bridge's marker alone. The dispatch hook
// (army_orders.cpp on_game_command_dispatch, shared with the army hold) calls
// ai_veto_on_dispatch() first: for a watched ruler it records the post and, when the seat
// vetoed that kind, drops it as the engine's own gate drops a refused command.
//
// Watched = every controlled character (synced once per daily tick on the game thread),
// plus any ruler a rule was set for. Records are priced on the game thread in the same
// daily drain: a title's creation price (the engine's create_title_price), a law's pass
// price (LAW_PASS_COST_EVAL, gold / prestige / piety); mercenaries, constructions, ally
// calls and perks carry no price read (an ally call's prestige is the script's, by the
// ally's tier: the toolkit prices it). A dynasty legacy (CAddDynastyPerkCommand,
// the dynasty head's AI) is priced in renown as the game's check computes it (ai_veto.h legacy_price).
//
// Who posted. The interaction class is also what the bridge posts for its own sends
// (the TLS pipeline, ai_eval_hook.cpp, flags 7). The hook is entered by a jmp from the
// detoured entry, so __builtin_return_address(0) in on_game_command_dispatch is the
// poster's return address: inside the game image = the game's post, inside this .so = the
// bridge's (re_interaction_command.md §3). The bridge also marks its own posting thread
// around that one call (OwnDispatchMark). Only a post both call the game's can be dropped;
// a disagreement is counted and logged. Every AI interaction of a watched ruler is also
// counted by type (the census), never dropped.
//
// Kill switch: CK3_BRIDGE_AI_VETO=0 (read once at the first drain) and the hook is not
// installed for the veto; unset, it is installed with an empty table (no rule = no change
// to the game: every post passes). Windows: installed the same way; the hook
// target, the command classes, their fields and the drop are reviewed for MSVC in
// ck3_offsets_win.h (funcs::GAME_COMMAND_DISPATCH and the command vtables after
// CREATE_TITLE_DISPATCH_SITE) and re/findings/windows/re_win2_veto_events.md.
#include "bridge.h"
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"
#include "ai_veto.h"
#include "engine_bool.h"
#include "scope_slot_policy.h"
#include "maa_currency.h"
#include "win_anchor_refusal.h"   // Veto kinds with no Windows anchor refuse by name

#include <atomic>
#include <csetjmp>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#else
#include <windows.h>                      // GetCurrentThreadId: the own-post marker
#include "windows/ck3_offsets_win.h"      // struct_layout::DECISION_TYPE_NAME_KEY
#endif
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

bool ensure_dispatch_hook();   // army_orders.cpp: installs the shared GameCommandDispatch hook once

// The bridge's own-post marker (bridge.h OwnDispatchMark). The posting thread's
// pthread id while the bridge's own flags-7 GameCommandDispatch call runs; 0 otherwise. A
// plain atomic rather than a thread_local: the hook loads it on any AI worker thread and
// must not touch the TLS machinery there. Only the game thread posts for the bridge.
std::atomic<uintptr_t> g_own_dispatch_thread{0};

static uintptr_t this_thread_word() {
#if defined(_WIN32)
    // The posting thread's id (never 0 for a live thread). Before the Windows port this was 0,
    // which would have read every post as the bridge's own (0 == an unset marker).
    return static_cast<uintptr_t>(GetCurrentThreadId());
#else
    return static_cast<uintptr_t>(pthread_self());
#endif
}

OwnDispatchMark::OwnDispatchMark() : prev_(g_own_dispatch_thread.exchange(this_thread_word(), std::memory_order_acq_rel)) {}
OwnDispatchMark::~OwnDispatchMark() { g_own_dispatch_thread.store(prev_, std::memory_order_release); }

namespace av = ai_veto;
namespace {

av::Table g_table;
av::Ring g_ring;
av::Census g_census;
av::Census g_unclassified;                      // AI posts of a class no veto kind classifies (by vtable)
uintptr_t g_vt_interaction = 0;                // CSendCharacterInteractionCommand primary vtable
std::atomic<uintptr_t> g_call_ally_itype{0};    // CCharacterInteractionType* of call_ally_interaction
std::atomic<uintptr_t> g_vassal_offer_itype{0}; // ... of offer_vassalization_interaction
std::atomic<uintptr_t> g_declare_war_itype{0};  // ... of declare_war_interaction (the AI's war)
std::atomic<uintptr_t> g_laamp_hire_itype{0};   // ... of hire_laamp_mercenaries_interaction (a mercenary hire)
std::atomic<uint64_t> g_faction_itypes[av::kFactionInteractionCount] = {};   // the faction kind: av::kFactionInteractionKeys
std::atomic<uint64_t> g_call_in_itypes[av::kCallInCount] = {};              // Av::kCallInKeys (calls TO a ruler)
uintptr_t g_vt_join_faction = 0;                // the faction kind: CJoinFactionCommand (the create command is g_vt[kFaction])
uintptr_t g_self_lo = 0, g_self_hi = 0;         // libck3_bridge.so's mapping (the return-address test)
std::atomic<uint64_t> g_origin_disagree{0};     // posts where the return address and the marker disagree
std::atomic<uint64_t> g_bridge_posts_seen{0};   // flags-7 posts of a watched kind that were the bridge's
constexpr int32_t kDropRecordWindow = 30 * ck3::game_state::TICKS_PER_DAY;
std::mutex g_table_mu;                          // writers of g_table (IPC set, the daily sync)
std::atomic<bool> g_live{false};                // the hook consults the table
std::atomic<bool> g_killed{false};              // CK3_BRIDGE_AI_VETO=0
std::atomic<bool> g_hook_ok{false};
std::atomic<uint64_t> g_priced_through{0};      // records up to this seq were priced
uintptr_t g_vt[av::kWhatCount] = {};            // runtime vtables, by What
// On both builds (Windows: the generated header's measured REGIMENT_OWNER_CHAR_ID)
static_assert(av::kRegimentOwnerCharOffset == ::regiment::OWNER_CHAR_ID && av::kRegimentIdOffset == ::regiment::ID,
              "ai_veto.h's regiment fields are ck3_offsets.h's");
#if !defined(_WIN32)
static_assert(av::kActivityTypeOffset == ck3::start_activity_cmd::PLAN + ck3::activity_plan::TYPE &&
              av::kActivityHostOffset == ck3::start_activity_cmd::PLAN + ck3::activity_plan::HOST,
              "ai_veto.h's activity fields are ck3_offsets.h's");
#endif
uintptr_t g_vt_add_maa = 0;                     // CAddMAAToRegimentCommand primary vtable (men_at_arms)
std::atomic<uint64_t> g_maa_posts_seen{0};      // Flags-7 men-at-arms posts of any ruler the hook classified

// ── hook side: volatile loads under is_mapped_range, no allocation ───────────────
int what_of(uintptr_t vt) {
    for (int i = 0; i < av::kWhatCount; ++i)
        if (g_vt[i] && vt == g_vt[i]) return i;
    return av::kNone;
}

template <typename T>
bool vload(uintptr_t a, T* out) {
    if (!is_mapped_range(a, sizeof(T))) return false;
    *out = *reinterpret_cast<volatile T*>(a);
    return true;
}

// The ruler's gold, prestige and piety now (CharacterDB slot -> core data), as lookup_character
// walks it, with volatile loads.
bool hook_purse(uint32_t ruler_full, av::Record* r) {
    uintptr_t db = 0, table = 0, ch = 0, core = 0;
    uint32_t cap = 0, id = 0;
    const uint32_t idx = ruler_full & 0xFFFFFF;
    if (!vload(g_base_addr + ck3::globals::CHARACTER_DB, &db) || db < 0x10000) return false;
    if (!vload(db + ck3::hash_map::CAPACITY, &cap) || idx >= cap) return false;
    if (!vload(db + ck3::hash_map::TABLE_PTR, &table) || table < 0x10000) return false;
    if (!vload(table + ck3::hash_map::ENTRY_HEADER_SIZE + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE, &ch) ||
        ch < 0x10000) return false;
    if (!vload(ch + ck3::character::ID, &id) || (id & 0xFFFFFF) != idx) return false;
    if (!vload(ch + ck3::character::REALM_DATA, &core) || core < 0x10000) return false;
    return vload(core + ck3::core_data::GOLD, &r->gold_fp) && vload(core + ck3::core_data::PRESTIGE, &r->prestige_fp) &&
           vload(core + ck3::core_data::PIETY, &r->piety_fp);
}

// The owner of a regiment (CRegiment+0x124, the character's full id; 0 when not read or a title owns it), by
// the regiment's full id, as CAddMAAToRegimentCommand's Execute reads it (REGIMENT_DB, the id verified).
uint32_t hook_regiment_owner(uint32_t reg_full) {
    uintptr_t db = 0, table = 0, reg = 0;
    uint32_t cap = 0, id = 0, owner = 0;
    const uint32_t idx = reg_full & 0xFFFFFF;
    if (!vload(g_base_addr + ck3::globals::REGIMENT_DB, &db) || db < 0x10000) return 0;
    if (!vload(db + ck3::hash_map::CAPACITY, &cap) || idx >= cap) return 0;
    if (!vload(db + ck3::hash_map::TABLE_PTR, &table) || table < 0x10000) return 0;
    if (!vload(table + ck3::hash_map::ENTRY_HEADER_SIZE + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE, &reg) ||
        reg < 0x10000) return 0;
    if (!vload(reg + av::kRegimentIdOffset, &id) || id != reg_full) return 0;
    if (!vload(reg + av::kRegimentOwnerCharOffset, &owner) || owner == 0xFFFFFFFFu) return 0;
    return owner;
}

int32_t hook_date() {
    uintptr_t gs = 0;
    int32_t d = 0;
    if (!vload(g_base_addr + ck3::globals::GAME_STATE_ROOT, &gs) || gs < 0x10000) return 0;
    return vload(gs + ck3::game_state::GAME_DATE, &d) ? d : 0;
}

}  // namespace

// Called first by the shared dispatch hook, on whatever thread the AI posts from. True =
// the command was dropped (deleting dtor run, the caller's slot nulled): return 0. `ra` is
// the return address of the GameCommandDispatch call (the poster's).
bool ai_veto_on_dispatch(void** cmd_pp, uint32_t flags, uintptr_t ra) {
    if (!g_live.load(std::memory_order_acquire)) return false;
    if (flags != av::kAiDispatchFlags || g_table.count.load(std::memory_order_acquire) <= 0) return false;
    uintptr_t pp = reinterpret_cast<uintptr_t>(cmd_pp), cmd = 0, vt = 0;
    if (!vload(pp, &cmd) || cmd < 0x10000 || !is_mapped_range(cmd, 0x30)) return false;
    if (!vload(cmd, &vt)) return false;
    int what = what_of(vt);
    uint32_t ruler = 0;
    uint64_t itype = 0;
    uint32_t call_in_which = 0;                                 // Which kCallInKeys entry matched
    const bool interaction = g_vt_interaction && vt == g_vt_interaction;
    const bool decision = what == static_cast<int>(av::What::kDecision);
    const bool activity = what == static_cast<int>(av::What::kActivity);
    if (activity && !is_mapped_range(cmd, av::kActivityHostOffset + 4)) return false;
    if (interaction) {
        if (!is_mapped_range(cmd, ck3::send_interaction_cmd::SIZE)) return false;
        if (!vload(cmd + ck3::send_interaction_cmd::INTERACTION_TYPE, &itype)) return false;
        if (!vload(cmd + av::kInteractionActorOffset, &ruler)) return false;
        if (g_table.find(ruler & 0xFFFFFF) < 0) {
            // Another ruler's offer of vassalage TO a watched ruler (the recipient is the watched one);
            // Another ruler's call TO ARMS to a watched ruler (call_in)
            uint64_t ci[av::kCallInCount];
            for (int i = 0; i < av::kCallInCount; ++i) ci[i] = g_call_in_itypes[i].load(std::memory_order_acquire);
            const int rw = av::recipient_interaction_what(itype, g_vassal_offer_itype.load(std::memory_order_acquire),
                                                          ci, av::kCallInCount, &call_in_which);
            uint32_t to = 0;
            if (rw == av::kNone || !vload(cmd + av::kInteractionRecipientOffset, &to) ||
                g_table.find(to & 0xFFFFFF) < 0)
                return false;
            what = rw;
            ruler = to;
        } else {
            what = av::actor_interaction_what(itype, g_call_ally_itype.load(std::memory_order_acquire),
                                              g_laamp_hire_itype.load(std::memory_order_acquire),
                                              g_declare_war_itype.load(std::memory_order_acquire));
            if (what == av::kNone) {   // the faction kind: his AI starting or joining a faction by interaction
                uint64_t fi[av::kFactionInteractionCount];
                for (int i = 0; i < av::kFactionInteractionCount; ++i) fi[i] = g_faction_itypes[i].load(std::memory_order_acquire);
                if (av::faction_interaction(itype, fi, av::kFactionInteractionCount)) what = static_cast<int>(av::What::kFaction);
            }
        }
    } else if (g_vt_add_maa && vt == g_vt_add_maa) {
        // More men of an existing regiment: the command names the regiment, its owner is the ruler
        uint32_t reg = 0;
        g_maa_posts_seen.fetch_add(1, std::memory_order_relaxed);
        if (!vload(cmd + av::kMaaAddRegimentOffset, &reg)) return false;
        ruler = hook_regiment_owner(reg);
        if (!ruler) return false;
        what = static_cast<int>(av::What::kMenAtArms);
    } else {
        if (what == av::kNone && g_vt_join_faction && vt == g_vt_join_faction)
            what = static_cast<int>(av::What::kFaction);   // the faction kind: a join; the ruler at +0x24 as in a create
        if (what == av::kNone) {
            // A class no veto kind classifies: counted by its vtable for a watched ruler, never dropped
            uint32_t w = 0;
            if (vload(cmd + av::kCmdRulerOffset, &w)) {
                const uint32_t r24 = av::unclassified_ruler(w, w != 0xFFFFFFFFu && g_table.find(w & 0xFFFFFF) >= 0);
                if (r24)
                    g_unclassified.add(vt >= g_base_addr ? vt - g_base_addr : vt, r24, hook_date(),
                                       ra >= g_base_addr ? ra - g_base_addr : 0);
            }
            return false;
        }
        if (!vload(cmd + av::ruler_offset(what), &ruler)) return false;
        // A regiment for a title (no character, +0x28 = -1) is no watched ruler's
        if (what == static_cast<int>(av::What::kMenAtArms)) {
            g_maa_posts_seen.fetch_add(1, std::memory_order_relaxed);
            if (ruler == 0xFFFFFFFFu) return false;
        }
    }
    const av::Origin ra_origin = av::classify_origin(ra, g_base_addr, g_image_end, g_self_lo, g_self_hi);
    const bool marker = g_own_dispatch_thread.load(std::memory_order_acquire) == this_thread_word();
    // A decision is dispatched from inside the game image whoever asked; the marker decides
    const av::Origin origin = decision ? av::decision_origin(ra_origin, marker) : ra_origin;
    const bool own = decision ? marker : av::bridge_own(ra_origin, marker);
    const bool agrees = decision ? av::decision_agrees(ra_origin) : av::origin_agrees(ra_origin, marker);
    if (!agrees) g_origin_disagree.fetch_add(1, std::memory_order_relaxed);
    if (interaction && !own && what != static_cast<int>(av::What::kVassalOffer) &&
        what != static_cast<int>(av::What::kCallIn))
        g_census.add(itype, ruler & 0xFFFFFF, hook_date(), ra >= g_base_addr ? ra - g_base_addr : 0);
    if (what == av::kNone) return false;
    // A decision veto may name its decisions: read this one's key (no allocation)
    uint64_t dec_ptr = 0;
    char dec_key[av::kDecisionKeyCap] = {};
    size_t dec_len = 0;
    bool key_vetoed = true;
    if (decision) {
        vload(cmd + av::kDecisionTypeOffset, &dec_ptr);
        const int at = g_table.find(ruler & 0xFFFFFF);
        if (at >= 0 && (g_table.slot[at].mask.load(std::memory_order_acquire) & (1u << what))) {
            uintptr_t data = 0;
            uint64_t len = 0;
#if defined(_WIN32)
            // CDecisionType's key is an MSVC std::string at +0x18 (ck3_offsets_win.h
            // struct_layout::DECISION_TYPE_NAME_KEY): size +0x10, capacity +0x18, the characters
            // inline while the capacity is 15 or less, else at the pointer in the first word.
            const uintptr_t ks = static_cast<uintptr_t>(dec_ptr) + ck3_win::struct_layout::DECISION_TYPE_NAME_KEY;
            uint64_t cap = 0;
            const bool key_ok = dec_ptr >= 0x10000 && vload(ks + 0x10, &len) && vload(ks + 0x18, &cap) &&
                                cap >= len && (cap <= 15 ? (data = ks, true) : vload(ks, &data));
            if (key_ok && len > 0 &&
#else
            if (dec_ptr >= 0x10000 && vload(static_cast<uintptr_t>(dec_ptr) + av::kDbKeyData, &data) &&
                vload(static_cast<uintptr_t>(dec_ptr) + av::kDbKeyLen, &len) && len > 0 &&
#endif
                len < static_cast<uint64_t>(av::kDecisionKeyCap) && is_mapped_range(data, static_cast<size_t>(len))) {
                for (uint64_t i = 0; i < len; ++i) dec_key[i] = *reinterpret_cast<volatile const char*>(data + i);
                dec_len = static_cast<size_t>(len);
            }
            // Whether its cost block names gold, read only when the slot's gold rule is set
            bool costs_gold = false;
#if !defined(_WIN32)
            if (dec_ptr >= 0x10000 && g_table.slot[at].gold_cost.load(std::memory_order_acquire)) {
                uint32_t prim[av::kCostSlots] = {};
                uint32_t fb = 0;
                bool read_ok = true;
                for (int i = 0; i < av::kCostSlots && read_ok; ++i)
                    read_ok = vload(static_cast<uintptr_t>(dec_ptr) + ck3::decision_type::CAN_TAKE_PRIMARY +
                                    ck3::scripted_cost::SLOT_BASE + static_cast<uintptr_t>(i) * ck3::scripted_cost::SLOT_STRIDE +
                                    ck3::scripted_cost::SLOT_COUNT, &prim[i]);
                read_ok = read_ok && vload(static_cast<uintptr_t>(dec_ptr) + ck3::decision_type::CAN_TAKE_FALLBACK +
                                           ck3::scripted_cost::SLOT_BASE + ck3::scripted_cost::SLOT_COUNT, &fb);
                costs_gold = read_ok && av::decision_costs_gold(prim, fb);
            }
#endif
            key_vetoed = av::decision_key_matches(g_table.slot[at], dec_len ? dec_key : nullptr, dec_len,
                                                  costs_gold) == av::KeyMatch::kYes;
        }
    } else if (activity) {
        // The activity's type and its key (the db object's std::string, as a decision's); a type the seat put to
        // its AI with host_activity (the slot's spared types) passes, recorded
        vload(cmd + av::kActivityTypeOffset, &dec_ptr);
        const int at = g_table.find(ruler & 0xFFFFFF);
        uintptr_t data = 0;
        uint64_t len = 0;
#if !defined(_WIN32)
        if (dec_ptr >= 0x10000 && vload(static_cast<uintptr_t>(dec_ptr) + av::kDbKeyData, &data) &&
            vload(static_cast<uintptr_t>(dec_ptr) + av::kDbKeyLen, &len) && len > 0 &&
            len < static_cast<uint64_t>(av::kDecisionKeyCap) && is_mapped_range(data, static_cast<size_t>(len))) {
            for (uint64_t i = 0; i < len; ++i) dec_key[i] = *reinterpret_cast<volatile const char*>(data + i);
            dec_len = static_cast<size_t>(len);
        }
#endif
        if (at >= 0 && av::activity_spared(g_table.slot[at], dec_len ? dec_key : nullptr, dec_len)) key_vetoed = false;
    } else if (what == static_cast<int>(av::What::kCreateTitle)) {
        // The one title the create_title veto spares (the seat's goal title) passes, recorded
        const int at = g_table.find(ruler & 0xFFFFFF);
        uint64_t tid = 0;
        if (at >= 0 && vload(cmd + 0x24, &tid) && av::title_spared(g_table.slot[at], tid)) key_vetoed = false;
    }
    int slot = -1;
    const av::Verdict v = av::decide(g_table, what, flags, ruler, &slot, own, key_vetoed);
    if (v == av::Verdict::kPass || slot < 0) return false;
    if (own) g_bridge_posts_seen.fetch_add(1, std::memory_order_relaxed);

    av::Record r;
    r.ruler = ruler & 0xFFFFFF;
    r.what = static_cast<int8_t>(what);
    r.date_raw = hook_date();
    r.origin = static_cast<uint8_t>(origin);
    r.origin_agrees = agrees;
    r.site = (origin == av::Origin::kGame) ? ra - g_base_addr : (origin == av::Origin::kBridge ? ra - g_self_lo : 0);
    switch (static_cast<av::What>(what)) {
    case av::What::kCreateTitle: vload(cmd + 0x24, &r.object_id); break;   // CCreateTitleCommand title id
    case av::What::kLaw: vload(cmd + 0x28, &r.object_ptr); break;          // CAddLawCommand CLaw*
    case av::What::kMercenary:
        if (interaction) {                                                 // The adventurer hired
            vload(cmd + ck3::send_interaction_cmd::RECIPIENT, &r.object_id);
            r.object_extra = av::kMercLaampFlag;
        } else {
            vload(cmd + 0x24, &r.object_id);                               // company id
        }
        break;
    case av::What::kConstruction:                                          // holding/province, building type
        vload(cmd + 0x24, &r.object_id);
        vload(cmd + 0x2c, &r.object_extra);
        break;
    case av::What::kAllyCall: {                                            // the ally, the war
        vload(cmd + ck3::send_interaction_cmd::RECIPIENT, &r.object_id);
        uint16_t scope_type = 0;
        vload(cmd + ck3::send_interaction_cmd::TARGET_SCOPE_TYPE, &scope_type);
        r.object_extra = scope_type;
        vload(cmd + ck3::send_interaction_cmd::TARGET_SCOPE_VALUE, &r.object_ptr);
        break;
    }
    case av::What::kWar: vload(cmd + ck3::send_interaction_cmd::RECIPIENT, &r.object_id); break;   // the target
    case av::What::kPerk: vload(cmd + ck3::add_lifestyle_perk_cmd::PERK, &r.object_ptr); break;
    case av::What::kCourtType: vload(cmd + ck3::funcs::SET_COURT_TYPE_CMD_SETTING, &r.object_ptr); break;   // the setting
    case av::What::kLegacy: vload(cmd + ck3::funcs::ADD_DYNASTY_PERK_CMD_PERK, &r.object_ptr); break;   // the legacy kind
    case av::What::kVassalOffer: vload(cmd + av::kInteractionActorOffset, &r.object_id); break;   // who offers
    case av::What::kCallIn: {                                              // Who called, the war, which call
        vload(cmd + av::kInteractionActorOffset, &r.object_id);
        vload(cmd + ck3::send_interaction_cmd::TARGET_SCOPE_VALUE, &r.object_ptr);
        r.object_extra = call_in_which;
        break;
    }
    case av::What::kFaction:                                               // the faction kind: what the AI posted
        if (interaction) {                                                 // the interaction's type; its recipient
            r.object_extra = av::kFactionInteraction;
            r.object_ptr = itype;
            vload(cmd + ck3::send_interaction_cmd::RECIPIENT, &r.object_id);
        } else {                                                           // a type index or a faction id; the target
            r.object_extra = vt == g_vt_join_faction ? av::kFactionJoin : av::kFactionCreate;
            vload(cmd + av::kFactionCmdObjectOffset, &r.object_id);
            if (r.object_extra == av::kFactionCreate) {
                uint32_t tgt = 0;
                vload(cmd + av::kFactionCreateTargetOffset, &tgt);
                r.object_ptr = tgt;
            }
        }
        break;
    case av::What::kDecision:                                              // the CDecisionType*, its key
    case av::What::kActivity:                                              // The CActivityType*, its key
        r.object_ptr = dec_ptr;
        if (dec_len) av::copy_text(r.key, sizeof(r.key), dec_key);
        break;
    case av::What::kMenAtArms:                                             // The type index; the regiment
        if (vt == g_vt_add_maa) {
            vload(cmd + av::kMaaAddTypeOffset, &r.object_id);
            uint32_t reg = 0;
            vload(cmd + av::kMaaAddRegimentOffset, &reg);
            r.object_ptr = reg;
            r.object_extra = av::kMaaAddFlag;
        } else {
            vload(cmd + av::kMaaCreateTypeOffset, &r.object_id);
        }
        break;
    }
    r.purse_read = hook_purse(ruler, &r);

    bool drop = v == av::Verdict::kDrop;
    uintptr_t dtor = 0;
    if (drop && !vload(vt + av::kCmdDeletingDtorSlot, &dtor)) drop = false;   // cannot drop cleanly: let it through
    if (drop && !is_mapped_range(dtor, 1)) drop = false;
    r.dropped = drop;
    if (!drop) {
        g_ring.push(r);
        g_table.slot[slot].passed[what].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // a retrying AI (a perk pick each pass, an ally called again): one record a window per object
    const uint64_t obj = r.object_ptr ? r.object_ptr : r.object_id;
    av::Slot& sl = g_table.slot[slot];
    if (av::record_drop(sl.last_obj[what].load(std::memory_order_relaxed), sl.last_date[what].load(std::memory_order_relaxed),
                        obj, r.date_raw, kDropRecordWindow)) {
        sl.last_obj[what].store(obj, std::memory_order_relaxed);
        sl.last_date[what].store(r.date_raw, std::memory_order_relaxed);
        g_ring.push(r);
    }
#if defined(_WIN32)
    reinterpret_cast<void* (*)(uintptr_t, uint32_t)>(dtor)(cmd, 1);   // scalar deleting dtor, free
#else
    reinterpret_cast<void (*)(uintptr_t)>(dtor)(cmd);
#endif
    *reinterpret_cast<volatile uintptr_t*>(pp) = 0;
    sl.dropped[what].fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ai_veto_hook_installed() { return g_hook_ok.load(std::memory_order_acquire); }

// First drain (bridge_init): read the switch, resolve the vtables, install the shared hook.
void init_ai_veto() {
    g_killed.store(av::kill_switch_off(getenv("CK3_BRIDGE_AI_VETO")), std::memory_order_relaxed);
    if (g_killed.load()) {
        ADLOG("ai_veto: disabled by %s=0 (set_ai_veto refuses; the hook is not installed for it)\n", av::kKillSwitchEnv);
        return;
    }
    // The same on Windows; every vtable below is an RTTI-anchored primary vtable there
    // (ck3_offsets_win.h, the command classes after CREATE_TITLE_DISPATCH_SITE)
    g_vt[static_cast<int>(av::What::kCreateTitle)] = g_base_addr + ck3::funcs::CREATE_TITLE_CMD_VTABLE;
    g_vt[static_cast<int>(av::What::kLaw)] = g_base_addr + ck3::funcs::ADD_LAW_CMD_VTABLE;
    g_vt[static_cast<int>(av::What::kMercenary)] = g_base_addr + ck3::globals::HIRE_MERC_CMD_VTABLE_PRIMARY;
    g_vt[static_cast<int>(av::What::kConstruction)] = g_base_addr + ck3::funcs::ADD_CONSTRUCTION_CMD_VTABLE;
    g_vt[static_cast<int>(av::What::kPerk)] = g_base_addr + ck3::add_lifestyle_perk_cmd::VTABLE;
    g_vt[static_cast<int>(av::What::kDecision)] = g_base_addr + ck3::funcs::EXECUTE_DECISION_CMD_VTABLE;
    // The AI's court-type change (CSetCourtTypeSettingCommand, ruler at +0x20)
    g_vt[static_cast<int>(av::What::kCourtType)] = g_base_addr + ck3::funcs::SET_COURT_TYPE_CMD_VTABLE;
    // The dynasty head's AI buying a legacy with renown (CAddDynastyPerkCommand, ruler at +0x20)
    g_vt[static_cast<int>(av::What::kLegacy)] = g_base_addr + ck3::funcs::ADD_DYNASTY_PERK_CMD_VTABLE;
    // The AI's faction create and join (ruler at +0x24)
    g_vt[static_cast<int>(av::What::kFaction)] = g_base_addr + ck3::faction_write::CREATE_FACTION_CMD_VTABLE;
    g_vt_join_faction = g_base_addr + ck3::faction_write::JOIN_FACTION_CMD_VTABLE;
    // A new men-at-arms regiment; more men of one is the other class (its owner read from the regiment)
    g_vt[static_cast<int>(av::What::kMenAtArms)] = g_base_addr + ck3::funcs::CREATE_MAA_REGIMENT_CMD_VTABLE;
    g_vt_add_maa = g_base_addr + ck3::funcs::ADD_MAA_TO_REGIMENT_CMD_VTABLE;
    // An activity the AI's HostActivities pass starts for him (CStartActivityCommand, host at cmd+0x28)
    g_vt[static_cast<int>(av::What::kActivity)] = g_base_addr + ck3::start_activity_cmd::VTABLE;
    // kAllyCall has no vtable of its own: the interaction class, then its type (the hook)
    g_vt_interaction = g_base_addr + ck3::funcs::SEND_INTERACTION_CMD_VTABLE;
    // this .so's mapping: the return-address test's "bridge" range
#if defined(_WIN32)
    // version.dll's image: its base (the module handle of an address inside it)
    // and SizeOfImage from its own PE header
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&init_ai_veto), &self) && self) {
            const auto base = reinterpret_cast<uintptr_t>(self);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
            if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE) {
                g_self_lo = base;
                g_self_hi = base + nt->OptionalHeader.SizeOfImage;
            }
        }
    }
#else
    Dl_info di{};
    if (dladdr(reinterpret_cast<void*>(&init_ai_veto), &di) && di.dli_fbase) {
        struct Find { uintptr_t base, hi; } f{reinterpret_cast<uintptr_t>(di.dli_fbase), 0};
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* u) -> int {
            auto* x = static_cast<Find*>(u);
            if (info->dlpi_addr != x->base) return 0;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD) continue;
                const uintptr_t e = info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
                if (e > x->hi) x->hi = e;
            }
            return 1;
        }, &f);
        if (f.hi > f.base) { g_self_lo = f.base; g_self_hi = f.hi; }
    }
#endif
    ADLOG("ai_veto: origin ranges game [0x%llx,0x%llx) bridge [0x%llx,0x%llx)\n", (unsigned long long)g_base_addr,
          (unsigned long long)g_image_end, (unsigned long long)g_self_lo, (unsigned long long)g_self_hi);
    const bool ok = ensure_dispatch_hook();
    g_hook_ok.store(ok, std::memory_order_release);
    g_live.store(ok, std::memory_order_release);
    ADLOG("ai_veto: %s (empty table: every post passes until a rule is set)\n",
          ok ? "dispatch hook live" : "dispatch hook FAILED to install (set_ai_veto refuses)");
}

namespace {

// The game thread prices one record. Engine calls under the protected region.
void price_record(av::Record* r) {
    // g_recovery_point / g_in_protected_region: bridge:: (action_dispatch_internal.h). A local
    // extern here would name a symbol in this anonymous namespace that nothing defines.
    void* chp = lookup_character(r->ruler);
    if (!chp || is_sentinel(chp)) { r->priced = 2; av::copy_text(r->why, sizeof(r->why), "ruler_not_found"); return; }
    const uintptr_t ch = reinterpret_cast<uintptr_t>(chp);
    const uint32_t full = safe_read<uint32_t>(ch + ck3::character::ID);
    uintptr_t title_obj = 0, law = 0;
    if (r->what == static_cast<int8_t>(av::What::kCreateTitle)) {
        title_obj = lookup_title_obj(r->object_id & 0xFFFFFF);
        const uintptr_t type = title_obj ? safe_read<uintptr_t>(title_obj + ck3::title_obj::TITLE_TYPE_PTR) : 0;
        if (type && is_mapped(type)) {
            std::string k = ad_read_std_string(type + ck3::db_object::NAME_KEY);
            av::copy_text(r->key, sizeof(r->key), k.c_str());
        }
        if (!title_obj) { r->priced = 2; av::copy_text(r->why, sizeof(r->why), "title_not_found"); return; }
    } else if (r->what == static_cast<int8_t>(av::What::kLaw)) {
        law = static_cast<uintptr_t>(r->object_ptr);
        if (!law || !is_mapped(law)) { r->priced = 2; av::copy_text(r->why, sizeof(r->why), "law_unread"); return; }
        std::string k = ad_read_std_string(law + ck3::realm_law_entry::LAW_NAME);
        av::copy_text(r->key, sizeof(r->key), k.c_str());
        if (!is_mapped(game_fn(ck3::funcs::LAW_PASS_COST_EVAL))) {
            r->priced = 2; av::copy_text(r->why, sizeof(r->why), "law_cost_fn_unmapped"); return;
        }
    } else {
        if (r->what == static_cast<int8_t>(av::What::kPerk)) {
            const uintptr_t perk = static_cast<uintptr_t>(r->object_ptr);
            if (perk && is_mapped(perk)) {
                std::string k = ad_read_std_string(perk + ck3::lifestyle_perk::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
        }
        const bool laamp = r->what == static_cast<int8_t>(av::What::kMercenary) && r->object_extra == av::kMercLaampFlag;
        const bool war = r->what == static_cast<int8_t>(av::What::kWar);
        if (r->what == static_cast<int8_t>(av::What::kAllyCall) ||
            r->what == static_cast<int8_t>(av::What::kVassalOffer) || laamp || war) {
            // the interaction's own key, read back (the hook matched its type pointer)
            const uintptr_t it = (laamp ? g_laamp_hire_itype
                                  : war ? g_declare_war_itype
                                  : r->what == static_cast<int8_t>(av::What::kAllyCall) ? g_call_ally_itype
                                                                                        : g_vassal_offer_itype)
                                     .load(std::memory_order_acquire);
            if (it && is_mapped(it)) {
                std::string k = ad_read_std_string(it + ck3::db_object::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
        }
        if (r->what == static_cast<int8_t>(av::What::kCallIn)) {
            // The matched call's key read back; no price (joining costs the war's upkeep, read by the toolkit)
            const uintptr_t it = r->object_extra < static_cast<uint32_t>(av::kCallInCount)
                                     ? static_cast<uintptr_t>(g_call_in_itypes[r->object_extra].load(std::memory_order_acquire))
                                     : 0;
            if (it && is_mapped(it)) {
                std::string k = ad_read_std_string(it + ck3::db_object::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
            r->priced = 2;
            av::copy_text(r->why, sizeof(r->why), "call_in_no_price");
            return;
        }
        if (r->what == static_cast<int8_t>(av::What::kLegacy)) {
            // The legacy's key, and its renown price as the game's check computes it (FUN_05eaab70:
            // PER x the dynasty's legacy count + BASE): the ruler's dynasty (his house's), its count, and whether it
            // already holds this legacy (a passed post that ran before this drain: the count then includes it)
            const uintptr_t perk = static_cast<uintptr_t>(r->object_ptr);
            if (perk && is_mapped(perk)) {
                std::string k = ad_read_std_string(perk + ::dynasty_legacy_perk::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
            const uint32_t house = safe_read<uint32_t>(ch + ck3::character::HOUSE_ID_DIRECT);
            const uintptr_t hobj = (house && house != 0xFFFFFFFFu) ? ad_db_lookup(ck3::globals::HOUSE_DB, house & 0xFFFFFF) : 0;
            const uint32_t did = (hobj && is_mapped(hobj)) ? safe_read<uint32_t>(hobj + ::dynasty_house_obj::DYNASTY_ID) : 0;
            const uintptr_t dyn = (did && did != 0xFFFFFFFFu) ? ad_db_lookup(ck3::globals::DYNASTY_DB, did & 0xFFFFFF) : 0;
            if (!dyn || !is_mapped(dyn) || safe_read<uint32_t>(dyn + ::dynasty_obj::ID) != did) {
                r->priced = 2; av::copy_text(r->why, sizeof(r->why), "dynasty_unread"); return;
            }
            const int32_t count = safe_read<int32_t>(dyn + ::dynasty_obj::LEGACY_PERKS_COUNT);
            const uintptr_t arr = safe_read<uintptr_t>(dyn + ::dynasty_obj::LEGACY_PERKS_ARRAY);
            bool held = false;
            for (int32_t i = 0; perk && arr && is_mapped(arr) && i < count && i < 128; ++i)
                held = held || safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8) == perk;
            const int64_t cost = av::legacy_price(safe_read<int32_t>(g_base_addr + ck3::globals::DYNASTY_PERK_COST_BASE),
                                                  safe_read<int32_t>(g_base_addr + ck3::globals::DYNASTY_PERK_COST_PER),
                                                  count, held);
            if (cost < 0) { r->priced = 2; av::copy_text(r->why, sizeof(r->why), "legacy_price_unread"); return; }
            r->cost_renown_fp = cost * static_cast<int64_t>(av::kFpScale);
            av::copy_text(r->currency, sizeof(r->currency), "renown");
            r->priced = 1;
            return;
        }
        if (r->what == static_cast<int8_t>(av::What::kFaction)) {
            // The key of what was posted: the interaction's, the faction's type (a join), or the type
            // at its index in the faction type DB (a create, CFactionType+0x08 = the index); no price (none is paid)
            uintptr_t key_obj = 0;
            if (r->object_extra == av::kFactionInteraction) {
                key_obj = static_cast<uintptr_t>(r->object_ptr);
            } else if (r->object_extra == av::kFactionJoin) {
                const uintptr_t f = ad_db_lookup(ck3::globals::FACTION_DB, r->object_id);
                if (f && is_mapped(f) && f != g_base_addr + ck3::faction_write::NULL_FACTION_SENTINEL)
                    key_obj = safe_read<uintptr_t>(f + ck3::faction_v2::TYPE_PTR);
            } else {
                const uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::FACTION_TYPE_DB_V2);
                const uintptr_t arr = db ? safe_read<uintptr_t>(db + ck3::game_db::ENTRIES_ARRAY) : 0;
                const uint32_t cnt = db ? safe_read<uint32_t>(db + ck3::game_db::LIVE_COUNT) : 0;
                const uintptr_t t = (arr && r->object_id < cnt && cnt <= 256)
                                        ? safe_read<uintptr_t>(arr + static_cast<uintptr_t>(r->object_id) * 8) : 0;
                if (t && is_mapped(t) && safe_read<uint32_t>(t + 8) == r->object_id) key_obj = t;
            }
            if (key_obj && is_mapped(key_obj)) {
                std::string k = ad_read_std_string(key_obj + ck3::db_object::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
            r->priced = 2;
            av::copy_text(r->why, sizeof(r->why), "faction_no_price");
            return;
        }
        if (r->what == static_cast<int8_t>(av::What::kActivity)) {
            // The type's key read back; no price here: the game charges it when the queued command runs, after
            // the post, and the plan is gone. The toolkit reads it from the purse: the gold at the post (this record)
            // to its next read
            const uintptr_t at = static_cast<uintptr_t>(r->object_ptr);
            if (!r->key[0] && at && is_mapped(at)) {
                std::string k = ad_read_std_string(at + ck3::db_object::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
            r->priced = 2;
            av::copy_text(r->why, sizeof(r->why), "activity_cost_by_purse");
            return;
        }
        if (r->what == static_cast<int8_t>(av::What::kCourtType)) {
            // The court type's key (a db object, its name at NAME_KEY: MEDIUM, as the decision's);
            // its price is the file's cost block (500 prestige for every type), read by the toolkit
            const uintptr_t ct = static_cast<uintptr_t>(r->object_ptr);
            if (ct && is_mapped(ct)) {
                std::string k = ad_read_std_string(ct + ck3::db_object::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
            r->priced = 2;
            av::copy_text(r->why, sizeof(r->why), "court_type_cost_in_files");
            return;
        }
        if (r->what == static_cast<int8_t>(av::What::kDecision) && !r->key[0]) {
            // the hook reads the key only under a decision veto; read it here for every record
            const uintptr_t dec = static_cast<uintptr_t>(r->object_ptr);
            if (dec && is_mapped(dec)) {
                std::string k = ad_read_std_string(dec + ck3::db_object::NAME_KEY);
                av::copy_text(r->key, sizeof(r->key), k.c_str());
            }
        }
        if (r->what == static_cast<int8_t>(av::What::kMenAtArms)) {
            // The type by its index in the MaA type DB (as the command's Execute reads it), and the type's
            // recruit price in gold as the game's database holds it (query_maa_types reads the same field)
            const uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::MAA_TYPE_DB);
            const uintptr_t arr = db ? safe_read<uintptr_t>(db + ck3::globals::MAA_TYPE_DB_ARR_OFF) : 0;
            const uint32_t cnt = db ? safe_read<uint32_t>(db + ck3::globals::MAA_TYPE_DB_LIVE_CNT_OFF) : 0;
            const uintptr_t e = (arr && is_mapped(arr) && r->object_id < cnt && cnt <= 5000)
                                    ? safe_read<uintptr_t>(arr + static_cast<uintptr_t>(r->object_id) * 8) : 0;
            if (!e || !is_mapped(e) || (safe_read<uint32_t>(e + ::maa_type::DB_ID) & 0xFFFFFF) != r->object_id) {
                r->priced = 2; av::copy_text(r->why, sizeof(r->why), "maa_type_unread"); return;
            }
            std::string k = ad_read_std_string(e + ::maa_type::NAME_KEY);
            av::copy_text(r->key, sizeof(r->key), k.c_str());
            // A tribal ruler's government buys with prestige (maa_currency.h)
            namespace mc = bridge::maa_currency;
            const int64_t buy_fp = safe_read<int64_t>(e + ::maa_type::RECRUIT_GOLD_COST);
            const mc::Currency cur = mc::of_government(read_government_key(lookup_character(r->ruler)));
            if (cur == mc::Currency::Unread) {
                r->priced = 2; av::copy_text(r->why, sizeof(r->why), "maa_currency_unread"); return;
            }
            if (cur == mc::Currency::Prestige)
                r->cost_prestige_fp = static_cast<int64_t>(static_cast<double>(buy_fp) * mc::kPrestigeBuyCostMult);
            else
                r->cost_gold_fp = buy_fp;
            av::copy_text(r->currency, sizeof(r->currency), mc::name(cur));
            r->priced = 1;
            return;
        }
        if (war) {
            // A declaration's price (prestige, piety) is its casus belli's, read by cbs before a
            // declaration; the AI's own pick of CB is not in the interaction command at this offset
            r->priced = 2;
            av::copy_text(r->why, sizeof(r->why), "war_cb_not_read");
            return;
        }
        if (r->what == static_cast<int8_t>(av::What::kDecision)) {
            // a decision's price is its script cost block, read from the game's files by the toolkit
            r->priced = 2;
            av::copy_text(r->why, sizeof(r->why), r->key[0] ? "decision_cost_in_files" : "decision_unread");
            return;
        }
        if (r->what == static_cast<int8_t>(av::What::kConstruction)) {
            const uintptr_t bt = ad_db_lookup(ck3::globals::BUILDING_TYPE_DB, r->object_extra);
            const uintptr_t name = bt ? safe_read<uintptr_t>(bt + ck3::building_type::NAME_PTR) : 0;
            if (name && is_mapped(name)) {
                char buf[64] = {};
                for (size_t i = 0; i + 1 < sizeof(buf); ++i) {
                    const char c = safe_read<char>(name + i);
                    if (!c) break;
                    buf[i] = c;
                }
                av::copy_text(r->key, sizeof(r->key), buf);
            }
        }
        r->priced = 2;
        av::copy_text(r->why, sizeof(r->why), "no_price_read");
        return;
    }
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        r->priced = 2; av::copy_text(r->why, sizeof(r->why), "rng_scope_latched"); return;
    }
    volatile bool crashed = false;
    CreateTitlePrice price;
    int64_t cost[10] = {};
    uint64_t saved_tmp = 0;
    volatile bool rng_armed = arm_fake_random_scope(&saved_tmp);
    volatile uint64_t saved_rng = saved_tmp;
    watchdog_arm(3);
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) != 0) {
        crashed = true;
    } else {
        try {
            if (title_obj) {
                price = create_title_price(chp, title_obj);
            } else {
                auto eval = reinterpret_cast<void* (*)(int64_t*, uintptr_t, uint32_t)>(
                    game_fn(ck3::funcs::LAW_PASS_COST_EVAL));
                eval(cost, law + ck3::funcs::LAW_PASS_COST, full);
            }
        } catch (...) {
            crashed = true;
        }
    }
    g_in_protected_region = false; watchdog_disarm();
    if (rng_armed) restore_fake_random_scope(saved_rng);
    if (crashed) { r->priced = 2; av::copy_text(r->why, sizeof(r->why), "price_read_crashed"); return; }
    if (title_obj) {
        if (!price.known) { r->priced = 2; av::copy_text(r->why, sizeof(r->why), price.why); return; }
        av::copy_text(r->currency, sizeof(r->currency), price.currency);
        if (std::strcmp(price.currency, "piety") == 0) r->cost_piety_fp = price.cost;
        else r->cost_gold_fp = price.cost;
    } else {
        // slot 7 is added to the gold slot when the ruler does not pay from a treasury
        // (FUN_06b8f850); the law gate's own currency test, read the same way here.
        auto treasury = reinterpret_cast<bridge::EngineBoolFn<void*>>(game_fn(ck3::funcs::CREATE_TITLE_PAYS_TREASURY));
        const bool pays_treasury = is_mapped(reinterpret_cast<uintptr_t>(treasury)) && treasury(chp);
        r->cost_gold_fp = cost[0] + (pays_treasury ? 0 : cost[7]);
        r->cost_prestige_fp = cost[1];
        r->cost_piety_fp = cost[2];
        av::copy_text(r->currency, sizeof(r->currency), pays_treasury ? "treasury" : "gold");
    }
    r->priced = 1;
}

}  // namespace

// Once per daily tick on the game thread (bridge_init's drain): every controlled character
// is watched (and a watched one that is no longer controlled and has no rule is dropped);
// then the records the hook added since the last pass are priced and logged.
// call_ally_interaction's type pointer (the interaction hash table), checked by its
// key; 0 until the database answers.
uintptr_t resolve_call_ally_itype() {
    uintptr_t it = g_call_ally_itype.load(std::memory_order_acquire);
    if (it) return it;
    void* p = lookup_interaction("call_ally_interaction");
    const uintptr_t c = reinterpret_cast<uintptr_t>(p);
    if (!c || !is_mapped(c)) return 0;
    if (ad_read_std_string(c + ck3::db_object::NAME_KEY) != "call_ally_interaction") {
        ADLOG("ai_veto: call_ally_interaction resolved to 0x%lx but its key does not read back; not used\n",
              (unsigned long)c);
        return 0;
    }
    g_call_ally_itype.store(c, std::memory_order_release);
    ADLOG("ai_veto: call_ally_interaction type at 0x%lx (key read back)\n", (unsigned long)c);
    return c;
}

// offer_vassalization_interaction's type pointer, resolved and checked the same way.
uintptr_t resolve_vassal_offer_itype() {
    uintptr_t it = g_vassal_offer_itype.load(std::memory_order_acquire);
    if (it) return it;
    void* p = lookup_interaction("offer_vassalization_interaction");
    const uintptr_t c = reinterpret_cast<uintptr_t>(p);
    if (!c || !is_mapped(c)) return 0;
    if (ad_read_std_string(c + ck3::db_object::NAME_KEY) != "offer_vassalization_interaction") {
        ADLOG("ai_veto: offer_vassalization_interaction resolved to 0x%lx but its key does not read back; not used\n",
              (unsigned long)c);
        return 0;
    }
    g_vassal_offer_itype.store(c, std::memory_order_release);
    ADLOG("ai_veto: offer_vassalization_interaction type at 0x%lx (key read back)\n", (unsigned long)c);
    return c;
}

// declare_war_interaction's type pointer, resolved and checked the same way (the war veto). The
// AI's war post builds the special interaction slot 0xd; the key names that slot's interaction (00_war.txt
// declare_war_interaction: special_interaction = declare_war_interaction).
uintptr_t resolve_declare_war_itype() {
    uintptr_t it = g_declare_war_itype.load(std::memory_order_acquire);
    if (it) return it;
    void* p = lookup_interaction("declare_war_interaction");
    const uintptr_t c = reinterpret_cast<uintptr_t>(p);
    if (!c || !is_mapped(c)) return 0;
    if (ad_read_std_string(c + ck3::db_object::NAME_KEY) != "declare_war_interaction") {
        ADLOG("ai_veto: declare_war_interaction resolved to 0x%lx but its key does not read back; not used\n",
              (unsigned long)c);
        return 0;
    }
    g_declare_war_itype.store(c, std::memory_order_release);
    ADLOG("ai_veto: declare_war_interaction type at 0x%lx (key read back)\n", (unsigned long)c);
    return c;
}

// hire_laamp_mercenaries_interaction's type pointer, resolved and checked the same way (the mercenary veto
// covers the AI's hire of a landless adventurer's band as it covers the hire command).
uintptr_t resolve_laamp_hire_itype() {
    uintptr_t it = g_laamp_hire_itype.load(std::memory_order_acquire);
    if (it) return it;
    void* p = lookup_interaction("hire_laamp_mercenaries_interaction");
    const uintptr_t c = reinterpret_cast<uintptr_t>(p);
    if (!c || !is_mapped(c)) return 0;
    if (ad_read_std_string(c + ck3::db_object::NAME_KEY) != "hire_laamp_mercenaries_interaction") {
        ADLOG("ai_veto: hire_laamp_mercenaries_interaction resolved to 0x%lx but its key does not read back; not "
              "used\n", (unsigned long)c);
        return 0;
    }
    g_laamp_hire_itype.store(c, std::memory_order_release);
    ADLOG("ai_veto: hire_laamp_mercenaries_interaction type at 0x%lx (key read back)\n", (unsigned long)c);
    return c;
}

// The faction interactions' type pointers (av::kFactionInteractionKeys), resolved and checked the
// same way; one not in the database (a DLC's) stays 0 and matches nothing.
void resolve_faction_itypes() {
    for (int i = 0; i < av::kFactionInteractionCount; ++i) {
        if (g_faction_itypes[i].load(std::memory_order_acquire)) continue;
        const char* key = av::kFactionInteractionKeys[i];
        const uintptr_t c = reinterpret_cast<uintptr_t>(lookup_interaction(key));
        if (!c || !is_mapped(c) || ad_read_std_string(c + ck3::db_object::NAME_KEY) != key) continue;
        g_faction_itypes[i].store(c, std::memory_order_release);
        ADLOG("ai_veto: %s type at 0x%lx (key read back)\n", key, (unsigned long)c);
    }
}

// The calls to war another ruler sends (av::kCallInKeys), resolved and checked the same way; one not in the
// database stays 0 and matches nothing.
void resolve_call_in_itypes() {
    for (int i = 0; i < av::kCallInCount; ++i) {
        if (g_call_in_itypes[i].load(std::memory_order_acquire)) continue;
        const char* key = av::kCallInKeys[i];
        const uintptr_t c = reinterpret_cast<uintptr_t>(lookup_interaction(key));
        if (!c || !is_mapped(c) || ad_read_std_string(c + ck3::db_object::NAME_KEY) != key) continue;
        g_call_in_itypes[i].store(c, std::memory_order_release);
        ADLOG("ai_veto: %s type at 0x%lx (key read back; call_in)\n", key, (unsigned long)c);
    }
}

static int call_in_resolved() {
    int n = 0;
    for (int i = 0; i < av::kCallInCount; ++i) n += g_call_in_itypes[i].load(std::memory_order_acquire) != 0;
    return n;
}

// The seat passed from a dead ruler to his heir (bridge_init.cpp, the controlled handoff): the
// heir's AI is vetoed from the same instant, before the game's AI acts for him (crown_authority_1 passed
// for the heir the day after the death, a day before the toolkit's own carry). Returns the mask carried.
uint32_t ai_veto_carry(uint32_t from_id, uint32_t to_id) {
    if (!g_live.load(std::memory_order_acquire)) return 0;
    uint32_t m = 0;
    {
        std::lock_guard<std::mutex> lk(g_table_mu);
        m = g_table.carry(from_id & 0xFFFFFF, to_id & 0xFFFFFF);
    }
    if (m) ADLOG("ai_veto: CARRIED at the handoff %u -> %u mask=0x%x\n", from_id & 0xFFFFFF, to_id & 0xFFFFFF, m);
    return m;
}

void ai_veto_daily() {
    if (!g_live.load(std::memory_order_acquire)) return;
    resolve_call_ally_itype();
    resolve_vassal_offer_itype();
    resolve_laamp_hire_itype();
    resolve_declare_war_itype();
    resolve_faction_itypes();
    resolve_call_in_itypes();
    // the own-post mark is only ever set around one call on this thread; a crash recovered out
    // of that call skips its destructor (siglongjmp does not unwind): clear a stale mark here
    if (const uintptr_t m = g_own_dispatch_thread.load(std::memory_order_acquire)) {
        if (m == this_thread_word()) {
            g_own_dispatch_thread.store(0, std::memory_order_release);
            ADLOG("ai_veto: a stale own-post mark was cleared (a recovered send skipped its scope end)\n");
        }
    }
    std::vector<uint32_t> ctl;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        for (uint32_t c : g_controlled_chars) ctl.push_back(c & 0xFFFFFF);
    }
    {
        std::lock_guard<std::mutex> lk(g_table_mu);
        for (uint32_t c : ctl) g_table.watch(c);
        for (int i = 0; i < av::kMaxRulers; ++i) {
            const uint32_t id = g_table.slot[i].id.load(std::memory_order_acquire);
            if (id == av::kEmpty || g_table.slot[i].mask.load() != 0) continue;
            bool still = false;
            for (uint32_t c : ctl) still = still || c == id;
            if (!still) g_table.unwatch(id);
        }
    }
    const uint64_t last = g_ring.last_seq();
    uint64_t from = g_priced_through.load(std::memory_order_relaxed) + 1;
    const uint64_t first = g_ring.first_seq();
    if (from < first) from = first;
    int budget = 16;   // at most 16 engine price reads per day; the rest wait for tomorrow
    for (uint64_t s = from; s <= last && budget > 0; ++s, --budget) {
        av::Record r;
        if (!g_ring.get(s, &r)) { g_priced_through.store(s, std::memory_order_relaxed); continue; }
        if (r.priced == 0) {
            price_record(&r);
            g_ring.put_priced(r);
            ADLOG("ai_veto: %s %s char=%u object=%u key=%s gold=%.2f prestige=%.2f price_gold=%.2f price_prestige=%.2f "
                  "price_piety=%.2f price_renown=%.2f priced=%d %s origin=%s from_game=%d agrees=%d site=0x%lx war=%lu\n",
                  r.dropped ? "DROPPED" : "passed", av::what_name(r.what), r.ruler,
                  r.object_id, r.key, r.gold_fp / 1e5, r.prestige_fp / 1e5, r.cost_gold_fp / 1e5,
                  r.cost_prestige_fp / 1e5, r.cost_piety_fp / 1e5, r.cost_renown_fp / 1e5, r.priced, r.why,
                  av::origin_name(static_cast<av::Origin>(r.origin)), r.origin == 0 ? 1 : 0, r.origin_agrees ? 1 : 0,
                  (unsigned long)r.site, r.what == static_cast<int8_t>(av::What::kAllyCall) ||
                  r.what == static_cast<int8_t>(av::What::kCallIn) ? (unsigned long)r.object_ptr : 0UL);
        }
        g_priced_through.store(s, std::memory_order_relaxed);
    }
}

namespace {

bool veto_fail(std::string* out, const char* action, const char* reason) {
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string(action);
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *out = r.take();
    }
    return false;
}

void emit_ruler(ResponseBuilder& r, int i) {
    const av::Slot& s = g_table.slot[i];
    const uint32_t m = s.mask.load(std::memory_order_acquire);
    r.begin_object();
    r.key("character_id"); r.value_uint(s.id.load(std::memory_order_acquire));
    r.key("vetoes"); r.begin_array();
    for (int w = 0; w < av::kWhatCount; ++w) if (m & (1u << w)) r.value_string(av::what_name(w));
    r.end_array();
    // The decisions the decision veto names ([] = every decision)
    r.key("decision_keys"); r.begin_array();
    {
        const int n = s.key_count.load(std::memory_order_acquire);
        for (int k = 0; k < n && k < av::kMaxDecisionKeys; ++k) r.value_string(s.keys[k]);
    }
    r.end_array();
    // Whether the decision veto also drops every decision that costs gold
    r.key("decision_gold_cost"); r.value_bool(s.gold_cost.load(std::memory_order_acquire) != 0);
    // The title the create_title veto spares first (the goal's; 0: none); every spared title
    r.key("spare_title_id"); r.value_uint(av::first_spare(s));
    r.key("spare_title_ids"); r.begin_array();
    {
        int n = s.spare_count.load(std::memory_order_acquire);
        if (n < 0 || n > av::kMaxSpareTitles) n = 0;
        for (int k = 0; k < n; ++k) r.value_uint(s.spare[k].load(std::memory_order_acquire));
    }
    r.end_array();
    // The activity types the activity veto lets through
    r.key("activity_spare_keys"); r.begin_array();
    {
        const int n = s.act_count.load(std::memory_order_acquire);
        for (int k = 0; k < n && k < av::kMaxActivitySpares; ++k) r.value_string(s.act_keys[k]);
    }
    r.end_array();
    r.key("dropped"); r.begin_object();
    for (int w = 0; w < av::kWhatCount; ++w) { r.key(av::what_name(w)); r.value_uint(static_cast<uint32_t>(s.dropped[w].load())); }
    r.end_object();
    r.key("passed"); r.begin_object();
    for (int w = 0; w < av::kWhatCount; ++w) { r.key(av::what_name(w)); r.value_uint(static_cast<uint32_t>(s.passed[w].load())); }
    r.end_object();
    r.end_object();
}

}  // namespace

// set_ai_veto {character_id, op: set|clear, what: create_title|law|mercenary|construction|ally_call|perk|
// decision|... (every av::what_name, legacy the thirteenth), keys: "a,b" (what=decision, set only: drop only these decisions; none = every decision),
// spare_title_id: N (what=create_title, set only, the one title whose creation by the AI is never dropped; a set
// without it spares none, a clear forgets it), spare_title_ids: "a,b" (up to kMaxSpareTitles, spare_title_id
// first when both are sent)}.
// Bridge state only: the table the dispatch hook loads.
bool do_set_ai_veto(const char* params_json, std::string* out) {
    static const char* kAction = "set_ai_veto";
    if (g_killed.load(std::memory_order_relaxed)) return veto_fail(out, kAction, "ai_veto_disabled");
    if (!g_hook_ok.load(std::memory_order_acquire)) return veto_fail(out, kAction, "ai_veto_hook_not_installed");
    const int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) return veto_fail(out, kAction, "character_id_required");
    const uint32_t id = static_cast<uint32_t>(cid) & 0xFFFFFF;
    const char* op_s = json_get_string(params_json, "op");
    const std::string op = (op_s && op_s[0]) ? op_s : "set";
    if (op != "set" && op != "clear") return veto_fail(out, kAction, "op_must_be_set_or_clear");
    const char* what_s = json_get_string(params_json, "what");
    if (!what_s || !what_s[0]) return veto_fail(out, kAction, "what_required");
    const int what = av::what_by_name(what_s);
    if (what == av::kNone) return veto_fail(out, kAction, "what_must_be_a_veto_kind");
    // A kind matched by a vtable with no Windows anchor would be an inert rule there
    // (success, never a match); refuse it by name. Never on Linux (no constant reads unmapped).
    if (op == "set") {
        if (const bridge::win_anchor::KindNeed* kn = bridge::win_anchor::veto_kind_missing(av::what_name(what))) {
            ADLOG("set_ai_veto: %s refused: not_on_windows (%s has no Windows anchor)\n", kn->what, kn->anchor);
            if (out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string(kAction);
                r.key("reason"); r.value_string("not_on_windows");
                r.key("what"); r.value_string(kn->what);
                r.key("missing_anchor"); r.value_string(kn->anchor);
                r.key("detail"); r.value_string("this build has no Windows address for the command class this kind "
                                                "matches; a rule would never drop anything, so none was set");
                r.end_object();
                *out = r.take();
            }
            return false;
        }
    }
    // the interaction's type is resolved on the game thread (ai_veto_daily), never here
    if (what == static_cast<int>(av::What::kAllyCall) && !g_call_ally_itype.load(std::memory_order_acquire))
        return veto_fail(out, kAction, "call_ally_interaction_unresolved");
    if (what == static_cast<int>(av::What::kVassalOffer) && !g_vassal_offer_itype.load(std::memory_order_acquire))
        return veto_fail(out, kAction, "offer_vassalization_interaction_unresolved");
    if (what == static_cast<int>(av::What::kWar) && !g_declare_war_itype.load(std::memory_order_acquire))
        return veto_fail(out, kAction, "declare_war_interaction_unresolved");
    if (what == static_cast<int>(av::What::kCallIn) && op == "set" && !g_call_in_itypes[0].load(std::memory_order_acquire))
        return veto_fail(out, kAction, "call_in_interactions_unresolved");   // call_ally_interaction at least
    // The decision veto's keys (set only; a clear forgets them)
    char keys[av::kMaxDecisionKeys][av::kDecisionKeyCap] = {};
    const char* keys_s = json_get_string(params_json, "keys");
    int nkeys = 0;
    if (keys_s && keys_s[0]) {
        if (what != static_cast<int>(av::What::kDecision) || op != "set")
            return veto_fail(out, kAction, "keys_only_with_set_decision");
        nkeys = av::split_decision_keys(keys_s, keys);
        if (nkeys < 0) return veto_fail(out, kAction, "bad_decision_keys");
    }
    // The decision veto's gold rule (set only): every decision whose cost block names gold
#if defined(_WIN32)
    // The rule's read (the decision type's cost block, decision_type::CAN_TAKE_PRIMARY/FALLBACK and
    // scripted_cost::SLOT_*) has no Windows layout, and the dispatch hook compiles it out there: the rule
    // is not stored, so the readback says decision_gold_cost false (only the named keys are dropped) and
    // the reply says why, instead of a rule that reads as set and never matches.
    const bool gold_cost_asked = json_get_int(params_json, "gold_cost") == 1;
    const bool gold_cost = false;
    if (gold_cost_asked && (what != static_cast<int>(av::What::kDecision) || op != "set"))
        return veto_fail(out, kAction, "gold_cost_only_with_set_decision");
#else
    const bool gold_cost = json_get_int(params_json, "gold_cost") == 1;
    if (gold_cost && (what != static_cast<int>(av::What::kDecision) || op != "set"))
        return veto_fail(out, kAction, "gold_cost_only_with_set_decision");
#endif
    // The activity veto's spared types (set only): "a,b", at most kMaxActivitySpares
    char akeys[av::kMaxDecisionKeys][av::kDecisionKeyCap] = {};
    const char* akeys_s = json_get_string(params_json, "spare_keys");
    int nakeys = 0;
    if (akeys_s && akeys_s[0]) {
        if (what != static_cast<int>(av::What::kActivity) || op != "set")
            return veto_fail(out, kAction, "spare_keys_only_with_set_activity");
        nakeys = av::split_decision_keys(akeys_s, akeys);
        if (nakeys < 0 || nakeys > av::kMaxActivitySpares) return veto_fail(out, kAction, "bad_spare_keys");
    }
    // The create_title veto's spared title (set only)
    const int spare_raw = json_get_int(params_json, "spare_title_id");      // -1: absent (json_get_int)
    const int spare_i = spare_raw > 0 ? spare_raw : 0;
    // The goal's chain (spare_title_ids "a,b"), after spare_title_id
    uint32_t spares[av::kMaxSpareTitles] = {};
    int nspare = 0;
    if (spare_i > 0) spares[nspare++] = static_cast<uint32_t>(spare_i) & 0xFFFFFF;
    const char* spares_s = json_get_string(params_json, "spare_title_ids");
    if (spares_s && spares_s[0]) {
        uint32_t more[av::kMaxSpareTitles] = {};
        const int m = av::split_title_ids(spares_s, more);
        if (m < 0) return veto_fail(out, kAction, "bad_spare_title_ids");
        for (int i = 0; i < m; ++i) {
            bool dup = false;
            for (int j = 0; j < nspare; ++j) dup = dup || spares[j] == more[i];
            if (dup) continue;
            if (nspare >= av::kMaxSpareTitles) return veto_fail(out, kAction, "too_many_spare_titles");
            spares[nspare++] = more[i];
        }
    }
    if (nspare > 0 && (what != static_cast<int>(av::What::kCreateTitle) || op != "set"))
        return veto_fail(out, kAction, "spare_only_with_set_create_title");
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        bool controlled = false;
        for (uint32_t c : g_controlled_chars) controlled = controlled || (c & 0xFFFFFF) == id;
        if (!controlled) return veto_fail(out, kAction, "not_a_controlled_character");
    }
    int slot = -1;
    {
        std::lock_guard<std::mutex> lk(g_table_mu);
        slot = g_table.watch(id);
        if (slot < 0) return veto_fail(out, kAction, "too_many_rulers");
        if (what == static_cast<int>(av::What::kDecision)) {
            // set: the list first, then the rule; clear: the rule off first, then the list. A post
            // between the two never reads "every decision" for a rule that names some, or is going.
            const char* kp[av::kMaxDecisionKeys];
            for (int i = 0; i < nkeys; ++i) kp[i] = keys[i];
            if (op == "set") {
                g_table.set_decision_keys(id, kp, nkeys);
                g_table.set_decision_gold(id, gold_cost);
                g_table.set_rule(id, what, true);
            } else {
                g_table.set_rule(id, what, false);
                g_table.set_decision_keys(id, kp, 0);
                g_table.set_decision_gold(id, false);
            }
        } else if (what == static_cast<int>(av::What::kCreateTitle)) {
            // Set: the spared title first, then the rule; clear: the rule off, then the title forgotten
            if (op == "set") {
                g_table.set_spare_titles(id, spares, nspare);
                g_table.set_rule(id, what, true);
            } else {
                g_table.set_rule(id, what, false);
                g_table.set_spare_titles(id, nullptr, 0);
            }
        } else if (what == static_cast<int>(av::What::kActivity)) {
            // Set: the spared types first, then the rule; clear: the rule off, then the types forgotten
            const char* kp[av::kMaxActivitySpares];
            for (int i = 0; i < nakeys; ++i) kp[i] = akeys[i];
            if (op == "set") {
                g_table.set_activity_spares(id, kp, nakeys);
                g_table.set_rule(id, what, true);
            } else {
                g_table.set_rule(id, what, false);
                g_table.set_activity_spares(id, kp, 0);
            }
        } else {
            g_table.set_rule(id, what, op == "set");
        }
    }
    ADLOG("set_ai_veto: %u %s %s%s%s\n", id, op.c_str(), av::what_name(what), nkeys ? " keys=" : "",
          nkeys ? keys_s : "");
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string(kAction);
        r.key("op"); r.value_string(op.c_str(), op.size());
        r.key("what"); r.value_string(av::what_name(what));
#if defined(_WIN32)
        if (gold_cost_asked) {
            r.key("gold_cost_not_set"); r.value_string("not_on_windows");
            r.key("missing_layout"); r.value_string("decision_type::CAN_TAKE_PRIMARY");
        }
#endif
        r.key("ruler"); emit_ruler(r, slot);
        r.end_object();
        *out = r.take();
    }
    return true;
}

// query_ai_veto {character_id (0 = every watched), since_seq}: the table and the records
// after since_seq. Bridge state only.
bool do_query_ai_veto(const char* params_json, std::string* out) {
    FaultScope fault_scope("query_ai_veto", out);   // The unclassified census names a class from its typeinfo
    const int cid = json_get_int(params_json, "character_id");
    const uint32_t want = cid > 0 ? (static_cast<uint32_t>(cid) & 0xFFFFFF) : 0;
    const int since_i = json_get_int(params_json, "since_seq");
    const uint64_t since = since_i > 0 ? static_cast<uint64_t>(since_i) : 0;
    ResponseBuilder r; r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("action"); r.value_string("query_ai_veto");
    r.key("hook_installed"); r.value_bool(g_hook_ok.load(std::memory_order_acquire));
    r.key("disabled_by_env"); r.value_bool(g_killed.load(std::memory_order_relaxed));
    const uint64_t last = g_ring.last_seq();
    uint64_t from = g_ring.first_seq();
    if (from <= since) from = since + 1;
    r.key("last_seq"); r.value_uint(static_cast<uint32_t>(last));
    r.key("origin_disagreements"); r.value_uint(static_cast<uint32_t>(g_origin_disagree.load()));
    r.key("bridge_posts_seen"); r.value_uint(static_cast<uint32_t>(g_bridge_posts_seen.load()));
    r.key("maa_posts_seen"); r.value_uint(static_cast<uint32_t>(g_maa_posts_seen.load()));
    r.key("call_ally_type_resolved"); r.value_bool(g_call_ally_itype.load(std::memory_order_acquire) != 0);
    r.key("laamp_hire_type_resolved"); r.value_bool(g_laamp_hire_itype.load(std::memory_order_acquire) != 0);
    r.key("declare_war_type_resolved"); r.value_bool(g_declare_war_itype.load(std::memory_order_acquire) != 0);
    r.key("call_in_types_resolved"); r.value_uint(static_cast<uint32_t>(call_in_resolved()));   // Of 3
    r.key("ring_cap"); r.value_uint(static_cast<uint32_t>(av::kRingCap));
    r.key("rulers"); r.begin_array();
    for (int i = 0; i < av::kMaxRulers; ++i) {
        const uint32_t id = g_table.slot[i].id.load(std::memory_order_acquire);
        if (id == av::kEmpty || (want && id != want)) continue;
        emit_ruler(r, i);
    }
    r.end_array();
    r.key("records"); r.begin_array();
    for (uint64_t s = from; s <= last; ++s) {
        av::Record x;
        if (!g_ring.get(s, &x) || (want && x.ruler != want)) continue;
        r.begin_object();
        r.key("seq"); r.value_uint(static_cast<uint32_t>(x.seq));
        event_rules_emit_date(r, "date", x.date_raw);
        r.key("character_id"); r.value_uint(x.ruler);
        r.key("what"); r.value_string(av::what_name(x.what));
        r.key("dropped"); r.value_bool(x.dropped);
        if (x.what == static_cast<int8_t>(av::What::kCreateTitle)) { r.key("title_id"); r.value_uint(x.object_id & 0xFFFFFF); }
        if (x.what == static_cast<int8_t>(av::What::kMercenary)) {
            if (x.object_extra == av::kMercLaampFlag) { r.key("laamp_id"); r.value_uint(x.object_id & 0xFFFFFF); }
            else { r.key("company_id"); r.value_uint(x.object_id); }
        }
        if (x.what == static_cast<int8_t>(av::What::kConstruction)) {
            r.key("location_id"); r.value_uint(x.object_id);
            r.key("building_type_index"); r.value_uint(x.object_extra);
        }
        if (x.what == static_cast<int8_t>(av::What::kDecision) && !x.key[0]) {
            r.key("decision_key"); r.value_string("unread");
        }
        if (x.what == static_cast<int8_t>(av::What::kVassalOffer)) {
            r.key("offered_by"); r.value_uint(x.object_id & 0xFFFFFF);
        }
        if (x.what == static_cast<int8_t>(av::What::kCallIn)) {
            r.key("called_by"); r.value_uint(x.object_id & 0xFFFFFF);
            r.key("war_id"); r.value_uint(static_cast<uint32_t>(x.object_ptr));
        }
        if (x.what == static_cast<int8_t>(av::What::kWar)) {
            r.key("target_id"); r.value_uint(x.object_id & 0xFFFFFF);
        }
        if (x.what == static_cast<int8_t>(av::What::kFaction)) {   // the faction kind
            const bool join = x.object_extra == av::kFactionJoin, via = x.object_extra == av::kFactionInteraction;
            r.key("faction_post"); r.value_string(join ? "join" : via ? "interaction" : "create");
            if (join) { r.key("faction_id"); r.value_uint(x.object_id & 0xFFFFFF); }
            else if (via) { r.key("recipient_id"); r.value_uint(x.object_id & 0xFFFFFF); }
            else { r.key("faction_type_index"); r.value_uint(x.object_id); r.key("target_id"); r.value_uint(static_cast<uint32_t>(x.object_ptr) & 0xFFFFFF); }
        }
        if (x.what == static_cast<int8_t>(av::What::kMenAtArms)) {
            r.key("maa_type_index"); r.value_uint(x.object_id);
            r.key("regiment"); r.value_string(x.object_extra == av::kMaaAddFlag ? "existing" : "new");
            if (x.object_extra == av::kMaaAddFlag) { r.key("regiment_id"); r.value_uint(static_cast<uint32_t>(x.object_ptr)); }
        }
        if (x.what == static_cast<int8_t>(av::What::kAllyCall)) {
            r.key("ally_id"); r.value_uint(x.object_id & 0xFFFFFF);
            r.key("war_id"); r.value_uint(static_cast<uint32_t>(x.object_ptr));
            r.key("target_scope_type"); r.value_uint(x.object_extra);
        }
        r.key("origin"); r.value_string(av::origin_name(static_cast<av::Origin>(x.origin)));
        if (!x.origin_agrees) { r.key("origin_agrees"); r.value_bool(false); }
        if (x.site) {
            char site[24];
            snprintf(site, sizeof site, "0x%lx", static_cast<unsigned long>(x.site));
            r.key("site"); r.value_string(site);
        }
        if (x.key[0]) { r.key("key"); r.value_string(x.key); }
        if (x.purse_read) {
            r.key("gold"); r.value_float(static_cast<double>(x.gold_fp) / av::kFpScale);
            r.key("prestige"); r.value_float(static_cast<double>(x.prestige_fp) / av::kFpScale);
            r.key("piety"); r.value_float(static_cast<double>(x.piety_fp) / av::kFpScale);
        } else {
            r.key("gold"); r.value_null();
        }
        r.key("priced"); r.value_string(x.priced == 1 ? "yes" : x.priced == 2 ? "no" : "pending");
        if (x.priced == 1) {
            r.key("price"); r.begin_object();
            r.key("gold"); r.value_float(static_cast<double>(x.cost_gold_fp) / av::kFpScale);
            r.key("prestige"); r.value_float(static_cast<double>(x.cost_prestige_fp) / av::kFpScale);
            r.key("piety"); r.value_float(static_cast<double>(x.cost_piety_fp) / av::kFpScale);
            if (x.what == static_cast<int8_t>(av::What::kLegacy)) {   // the legacy kind: the dynasty's renown
                r.key("renown"); r.value_float(static_cast<double>(x.cost_renown_fp) / av::kFpScale);
            }
            r.key("currency"); r.value_string(x.currency);
            r.end_object();
        } else if (x.priced == 2) {
            r.key("unpriced_why"); r.value_string(x.why);
        }
        r.end_object();
    }
    r.end_array();
    // The census of the AI's interactions for watched rulers (counted, never dropped)
    {
        static av::CensusEntry ce[av::kCensusCap];   // IPC thread only
        const int n = g_census.copy(ce, av::kCensusCap);
        r.key("interactions"); r.begin_array();
        for (int i = 0; i < n; ++i) {
            if (want && ce[i].ruler != want) continue;
            r.begin_object();
            r.key("character_id"); r.value_uint(ce[i].ruler);
            const uintptr_t it = static_cast<uintptr_t>(ce[i].itype);
            std::string k = (it && is_mapped(it)) ? ad_read_std_string(it + ck3::db_object::NAME_KEY) : std::string();
            r.key("interaction"); r.value_string(k.empty() ? "unread" : k.c_str());
            r.key("count"); r.value_uint(ce[i].count);
            event_rules_emit_date(r, "first", ce[i].first_date);
            event_rules_emit_date(r, "last", ce[i].last_date);
            char site[24];
            snprintf(site, sizeof site, "0x%lx", static_cast<unsigned long>(ce[i].site));
            r.key("site"); r.value_string(site);
            r.end_object();
        }
        r.end_array();
        r.key("interactions_overflow"); r.value_uint(static_cast<uint32_t>(g_census.overflow()));
        // Never a silent cut: the pairs the table had no slot for, and whether that count is exact
        r.key("interactions_truncated"); r.value_bool(g_census.overflow() > 0);
        r.key("interactions_pairs_not_shown"); r.value_uint(static_cast<uint32_t>(g_census.pairs_not_shown_for(want)));
        r.key("interactions_pairs_not_shown_exact"); r.value_bool(g_census.pairs_not_shown_exact());
        r.key("interactions_cap"); r.value_uint(static_cast<uint32_t>(av::kCensusCap));
    }
    // The AI's posts of classes no veto kind classifies, by the command's vtable (counted, never dropped)
    {
        static av::CensusEntry ue[av::kCensusCap];   // IPC thread only
        const int n = g_unclassified.copy(ue, av::kCensusCap);
        r.key("unclassified_commands"); r.begin_array();
        for (int i = 0; i < n; ++i) {
            if (want && ue[i].ruler != want) continue;
            r.begin_object();
            r.key("character_id"); r.value_uint(ue[i].ruler);
            char hex[24];
            snprintf(hex, sizeof hex, "0x%lx", static_cast<unsigned long>(ue[i].itype));
            r.key("vtable_rva"); r.value_string(hex);
            std::string cls;
#if !defined(_WIN32)
            // Itanium ABI: vtable[-1] is the typeinfo, typeinfo+8 its mangled name ("26CRazeHoldingCommand")
            const uintptr_t vt = g_base_addr + static_cast<uintptr_t>(ue[i].itype);
            const uintptr_t ti = is_mapped_range(vt - 8, 8) ? safe_read<uintptr_t>(vt - 8) : 0;
            const uintptr_t nm = (ti >= 0x10000 && is_mapped_range(ti + 8, 8)) ? safe_read<uintptr_t>(ti + 8) : 0;
            for (int k = 0; nm >= 0x10000 && k < 80 && is_mapped_range(nm + k, 1); ++k) {
                const char c = safe_read<char>(nm + k);
                if (c == 0) break;
                cls.push_back(c);
            }
#else
            // MSVC x64 RTTI: vtable[-1] is the complete-object locator (an absolute address), its +0xC
            // the TypeDescriptor's image-relative offset, the decorated name at TypeDescriptor+0x10
            // (".?AVCRazeHoldingCommand@@"). The same walk on the 1.18.3.1 file names the classes of
            // the RTTI vtables in ck3_offsets_win.h (CCreateTitleCommand, CSendCharacterInteractionCommand).
            const uintptr_t vt = g_base_addr + static_cast<uintptr_t>(ue[i].itype);
            const uintptr_t col = is_mapped_range(vt - 8, 8) ? safe_read<uintptr_t>(vt - 8) : 0;
            const uint32_t sig = (col >= 0x10000 && is_mapped_range(col, 0x10)) ? safe_read<uint32_t>(col) : 0;
            const uint32_t td = sig == 1 ? safe_read<uint32_t>(col + 0xC) : 0;
            const uintptr_t nm = td ? g_base_addr + td + 0x10 : 0;
            for (int k = 0; nm && k < 80 && is_mapped_range(nm + k, 1); ++k) {
                const char c = safe_read<char>(nm + k);
                if (c == 0) break;
                cls.push_back(c);
            }
#endif
            r.key("class"); r.value_string(cls.empty() ? "unread" : cls.c_str());
            r.key("count"); r.value_uint(ue[i].count);
            event_rules_emit_date(r, "first", ue[i].first_date);
            event_rules_emit_date(r, "last", ue[i].last_date);
            snprintf(hex, sizeof hex, "0x%lx", static_cast<unsigned long>(ue[i].site));
            r.key("site"); r.value_string(hex);
            r.end_object();
        }
        r.end_array();
        r.key("unclassified_overflow"); r.value_uint(static_cast<uint32_t>(g_unclassified.overflow()));
        r.key("unclassified_truncated"); r.value_bool(g_unclassified.overflow() > 0);
        r.key("unclassified_pairs_not_shown"); r.value_uint(static_cast<uint32_t>(g_unclassified.pairs_not_shown_for(want)));
        r.key("unclassified_pairs_not_shown_exact"); r.value_bool(g_unclassified.pairs_not_shown_exact());
    }
    r.end_object();
    if (out) *out = r.take();
    return true;
}

}  // namespace bridge
