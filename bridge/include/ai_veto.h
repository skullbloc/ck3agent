// ai_veto.h — a seat's veto over what the game's AI decides for its ruler.
//
// The game's AI posts each of the four decisions below as a typed command through
// GameCommandDispatch(mgr, &clone, 7, 0) (re/findings/re_ai_veto.md §1-§2, decompiles):
//   create_title  CCreateTitleCommand          CAIGoalCreateTitle::Execute 0x04de1ac0
//   law           CAddLawCommand               the Laws pass 0x04dbde20
//   mercenary     CHireMercenaryCompanyCommand 0x04e571d0 (its primary vtable is 0x0bb5d728;
//                                              the header had it swapped with the +0x18 one once),
//                                              and CSendCharacterInteractionCommand whose interaction is
//                                              hire_laamp_mercenaries_interaction sent by the watched ruler
//   construction  CAddConstructionCommand      the Development pass 0x04dd3ad0 and
//                                              CAIGoalConstructHolding::Execute 0x04de2400
//   ally_call     CSendCharacterInteractionCommand whose interaction is call_ally_interaction
//                                              (AI_INTERACTION_CORE 0x04dc4540 and siblings;
//                                              re/findings/re_interaction_command.md)
//   perk          CAddLifestylePerkCommand     the Lifestyles pass's pick FUN_04dbc620
//                                              (re/findings/re_lifestyle_perks.md §2)
//   decision      CExecuteDecisionCommand      the Decisions pass FUN_04dd1b80, the decision goal
//                                              FUN_04de28b0 (CAIGoalDecision), and
//                                              AI_TRY_DECISION_CORE 0x04dd2190 (re_ai_veto.md §8)
//   war           CSendCharacterInteractionCommand whose interaction is declare_war_interaction
//                                              sent by the watched ruler's AI (ai_war_post_decl
//                                              0x04e55c10: InteractionLookupByIndex(db, 0xd), the
//                                              special interaction declare_war_interaction (00_war.txt),
//                                              BuildInteractionCmd(actor = the ruler), dispatch 7;
//                                              re/findings/re_ai_veto.md §11)
//   court_type    CSetCourtTypeSettingCommand  the AI's court pass FUN_04dc0770 (500 prestige
//                                              each, posted the same day as crown_authority_1 at West Francia 867;
//                                              ctor FUN_067d42d0: ruler +0x20, CCourtTypeSetting* +0x28)
//   faction       CCreateFactionCommand and CJoinFactionCommand (the AI's faction pass FUN_04ddea80
//                                              creates, FUN_04dde690 joins; both post with flags 7, the ruler at
//                                              cmd+0x24, not +0x20), and CSendCharacterInteractionCommand whose
//                                              interaction is a faction one (kFactionInteractionKeys) sent by the
//                                              watched ruler: a claimant faction is created by its type's interaction
//                                              (FUN_04ddf6d0, CFactionType+0xFB8). re_ai_veto.md section 14
//   legacy        CAddDynastyPerkCommand       the AI's legacy pass FUN_04dbc7e0 (only for a ruler
//                                              who heads his dynasty; a weighted pick among the legacies the game's
//                                              check FUN_06e00780 allows, renown included; ctor FUN_067ca890: the
//                                              character +0x20, the CDynastyLegacyPerk* +0x28). The player's legacy
//                                              window FUN_046515f0 posts it with 0xe; the bridge never posts it.
//                                              Not the `perk` kind (a lifestyle perk). re_ai_veto.md section 15
//   vassal_offer  CSendCharacterInteractionCommand whose interaction is
//                                              offer_vassalization_interaction posted by ANOTHER
//                                              ruler's AI with the watched ruler as RECIPIENT
//                                              (a seated game's 1075 vassalage: no war; the offer has no
//                                              reply delay, so the recipient's AI answers inside
//                                              the send; dropped, the offer never reaches him)
//   call_in       CSendCharacterInteractionCommand whose interaction is call_ally_interaction (or the house or
//                                              dynasty member's call, which share its special interaction)
//                                              posted by ANOTHER ruler's AI with the watched ruler as RECIPIENT
//                                              (a test run's ally 36358 called Murchad into two overseas
//                                              wars, 1076-79 and 1082-85; the poster is FUN_04dbc010, which builds
//                                              the call with actor = the war leader, recipient = the ally, target
//                                              scope = the war, and dispatches it with flags 7 at 0x04dbc294 for
//                                              every AI ruler alike; ally_call's live drops return to the same
//                                              site 0x4dbc2ac. 00_alliance.txt: ai_accept and no reply delay, so
//                                              an AI recipient answers inside the send, as vassal_offer's;
//                                              dropped, the call never reaches him and no decline runs)
// Each carries the acting ruler's full character id at cmd+0x20, except the interaction,
// whose actor is at cmd+0x2f8 (the interaction context copied whole to cmd+0x20), and the two faction
// commands (cmd+0x24). The bridge's own faction calls (factions.cpp) run the engine's join/create/leave
// functions directly and dispatch nothing. The dispatch hook
// (army_orders.cpp, shared with the army hold) asks decide() below; a drop does what the
// engine's own dispatch gate does to a refused command (deleting dtor, slot nulled,
// return 0). The bridge's own writes of these classes never post them with flags 7
// (law: 0xe; title, mercenary, construction, perk: a direct Execute), so they are never dropped.
// The decision class is posted by the bridge too: the seat's `do take_decision` runs the game's
// own AI_TRY_DECISION_CORE, which posts it with flags 7 from inside the game image, so the
// return address cannot tell it from the AI's: for this kind the bridge's marker alone says
// who posted (decision_origin below).
// The interaction class is different: the bridge's own sends (the TLS pipeline in
// ai_eval_hook.cpp, a call_ally the seat sends) post it with flags 7 exactly as the AI does.
// Who posted is read from the return address of the GameCommandDispatch call (a game post
// returns into the game image, the bridge's into libck3_bridge.so: classify_origin) and
// cross-checked by a marker the bridge sets around its one flags-7 call (origin_agrees). A
// post that either says is the bridge's is recorded, never dropped (decide's bridge_own).
//
// Header-only and engine-free: test_ai_veto.cpp checks the table, the decision and the
// attempt ring on a host with no game. Every doubt passes the command to the game.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace bridge::ai_veto {

enum class What : uint8_t { kCreateTitle = 0, kLaw = 1, kMercenary = 2, kConstruction = 3, kAllyCall = 4,
                            kPerk = 5, kDecision = 6, kVassalOffer = 7, kMenAtArms = 8, kWar = 9, kCourtType = 10,
                            kFaction = 11, kLegacy = 12, kCallIn = 13, kActivity = 14 };
inline constexpr int kWhatCount = 15;
inline constexpr int kNone = -1;

inline const char* what_name(int w) {
    switch (w) {
    case 0: return "create_title";
    case 1: return "law";
    case 2: return "mercenary";
    case 3: return "construction";
    case 4: return "ally_call";
    case 5: return "perk";
    case 6: return "decision";
    case 7: return "vassal_offer";
    case 8: return "men_at_arms";
    case 9: return "war";
    case 10: return "court_type";
    case 11: return "faction";
    case 12: return "legacy";
    case 13: return "call_in";
    case 14: return "activity";
    }
    return "?";
}
inline int what_by_name(std::string_view n) {
    for (int i = 0; i < kWhatCount; ++i)
        if (n == what_name(i)) return i;
    return kNone;
}

//: The kill switch: "0" keeps the hook from being installed for the veto (no rule can
//: be set, nothing is recorded). Unset or anything else: on, with an empty table.
inline constexpr const char* kKillSwitchEnv = "CK3_BRIDGE_AI_VETO";
inline bool kill_switch_off(const char* v) { return v && v[0] == '0' && v[1] == '\0'; }

//: The flags every AI post of these commands uses (the four decompiles).
inline constexpr uint32_t kAiDispatchFlags = 7;
//: Where a game command's deleting destructor sits in its primary vtable. Itanium
//: (Linux): vt[1], the deleting destructor, (this). MSVC (Windows): vt[0], the scalar deleting
//: destructor, (this, flags), freeing when flags & 1 (ck3_offsets_win.h, the command vtables:
//: 0x928670 and its siblings). The drop calls it as GameCommandDispatch's own refusal does.
#if defined(_WIN32)
inline constexpr size_t kCmdDeletingDtorSlot = 0x0;
#else
inline constexpr size_t kCmdDeletingDtorSlot = 0x8;
#endif
//: The acting ruler's full character id in each of the four commands.
inline constexpr size_t kCmdRulerOffset = 0x20;
//: An AI post of a class the veto does not classify is counted by its vtable when the word at
//: kCmdRulerOffset is a watched ruler's id (the acting ruler's place in every class re_ai_veto.md §2 names;
//: MEDIUM for a class it does not name). Counted, never dropped: query_ai_veto `unclassified_commands`.
inline uint32_t unclassified_ruler(uint32_t word_at_0x20, bool watched) {
    return (word_at_0x20 != 0xFFFFFFFFu && watched) ? (word_at_0x20 & 0xFFFFFFu) : 0u;
}
//: CSendCharacterInteractionCommand: the interaction context is copied whole to cmd+0x20, so its
//: actor (ctx+0x2d8) is at cmd+0x2f8 (re_interaction_command.md §1, HIGH).
inline constexpr size_t kInteractionActorOffset = 0x2f8;
//: Its recipient (ctx+0x2dc) at cmd+0x2fc: the watched ruler of a vassal_offer is the one OFFERED to.
inline constexpr size_t kInteractionRecipientOffset = 0x2fc;
//: The kind of an interaction the WATCHED ruler's own AI sends (the actor is watched): call_ally_interaction is
//: an ally_call; hire_laamp_mercenaries_interaction (06_ep3_laamp_interactions.txt: the actor pays
//: ep3_hire_laamp_mercs_cost_value gold with pay_short_term_gold for a landless adventurer's band) is a mercenary
//: hire, under the same veto as the hire command. 0 = that type is not resolved yet.
//: declare_war_interaction sent by the watched ruler's own AI is a war (the seat's own declare_war
//: never posts it: the script layer's start_war and the steered WAR_CREATE_CORE dispatch nothing).
inline int actor_interaction_what(uint64_t itype, uint64_t call_ally, uint64_t laamp_hire, uint64_t declare_war = 0) {
    if (call_ally && itype == call_ally) return static_cast<int>(What::kAllyCall);
    if (laamp_hire && itype == laamp_hire) return static_cast<int>(What::kMercenary);
    if (declare_war && itype == declare_war) return static_cast<int>(What::kWar);
    return kNone;
}
//: The interactions by which another ruler calls the watched one into a war (call_in). The three share
//: special_interaction = call_ally_interaction (00_alliance.txt, 00_house_head_interactions.txt,
//: 00_dynast_interactions.txt); resolved by key on the game thread, one not in the database stays 0.
inline constexpr int kCallInCount = 3;
inline constexpr const char* kCallInKeys[kCallInCount] = {
    "call_ally_interaction", "call_house_member_to_war_interaction", "call_dynasty_member_to_war_interaction"};
//: The kind of an interaction ANOTHER ruler's AI sends to the watched ruler (the recipient is watched):
//: offer_vassalization_interaction is a vassal_offer, a call to war is a call_in; kNone for any other. `which`
//: (call_in): the index of the matched key in kCallInKeys.
inline int recipient_interaction_what(uint64_t itype, uint64_t vassal_offer, const uint64_t* calls, int n,
                                      uint32_t* which = nullptr) {
    if (!itype) return kNone;
    if (vassal_offer && itype == vassal_offer) return static_cast<int>(What::kVassalOffer);
    for (int i = 0; calls && i < n; ++i)
        if (calls[i] && itype == calls[i]) {
            if (which) *which = static_cast<uint32_t>(i);
            return static_cast<int>(What::kCallIn);
        }
    return kNone;
}
//: A mercenary record's object: the company id (the hire command), or the adventurer hired (the interaction).
inline constexpr uint32_t kMercLaampFlag = 1;   // Record::object_extra for a hire_laamp_mercenaries_interaction

//: (a mangonel regiment appeared for Murchad 1074-1078 with no call of the seat's; step 28 read
//: -84.2 gold unexplained): the men-at-arms commands the AI posts with flags 7 (FUN_04dfe030, image). Layouts from
//: the ctors' stores (objdump, binary md5 09ba163b...) and the Execute bodies (re_ai_veto.md section 10):
//: CCreateMAARegimentCommand (a new regiment): +0x24 an owning title (-1 for a character's own), +0x28 the owning
//: character's full id (-1 for a title's), +0x2c the type's index in the MaA type DB, +0x30 the origin, +0x34 a flag.
inline constexpr size_t kMaaCreateOwnerOffset = 0x28;
inline constexpr size_t kMaaCreateTypeOffset = 0x2c;
//: CAddMAAToRegimentCommand (more of an existing regiment): +0x24 the regiment's full id, +0x28 the type's index.
//: The command names no character: its Execute reads the regiment's owner (FUN_0635c620: CRegiment+0x124, a
//: character's full id, -1 when a title owns it), and so does the hook.
inline constexpr size_t kMaaAddRegimentOffset = 0x24;
inline constexpr size_t kMaaAddTypeOffset = 0x28;
//: On Windows the regiment's owner is at +0x12C (ck3_offsets_win.h struct_layout::
//: REGIMENT_OWNER_CHAR_ID: 16,621 of 16,621 regiments of a paired 1.18.3.1 save agree with the Linux
//: +0x124); its id stays at +0x10. ai_veto.cpp asserts both against the build's ck3_offsets.h.
#if defined(_WIN32)
inline constexpr size_t kRegimentOwnerCharOffset = 0x12c;
#else
inline constexpr size_t kRegimentOwnerCharOffset = 0x124;
#endif
inline constexpr size_t kRegimentIdOffset = 0x10;
//: A men-at-arms record's object_extra: 0 a new regiment, 1 more of an existing one (object_ptr = its id).
inline constexpr uint32_t kMaaAddFlag = 1;

//: (1075.10-1076.02 and 1085: about 280 gold spent by activities the game's AI started for the seat's
//: ruler, under no veto): CStartActivityCommand, the command the AI's HostActivities pass (image 0x04da9f40) posts
//: with flags 7 after its plan validates (re_activity_command.md section 1, HIGH, decompiled again here): the plan is
//: copied whole to cmd+0x20 (ctor FUN_05b9b5d0, clone FUN_05b9fa40), so the plan's CActivityType* (plan+0x0) is at
//: cmd+0x20 and its host's full id (plan+0x8, ActivityPlan_ctor FUN_05b709a0) at cmd+0x28. The type's key is the db
//: object's std::string at +0x10 (the 867 dump: "activity_feast"). The post site deletes the command through its
//: vt[1] when the slot is not null after the dispatch, and builds nothing after it.
inline constexpr size_t kActivityTypeOffset = 0x20;
inline constexpr size_t kActivityHostOffset = 0x28;
//: The activity types the activity veto spares per ruler (the ones the seat put to its AI with host_activity).
inline constexpr int kMaxActivitySpares = 4;

//: CCreateFactionCommand (ctor FUN_05f99b80: +0x20 the faction type's index, +0x24 the actor, +0x28
//: the target, his liege) and CJoinFactionCommand (ctor FUN_05f99440: +0x20 the faction's id, +0x24 the character)
//: carry the ruler at +0x24: the +0x20 word is a type index or a faction id, so the census never saw them.
inline constexpr size_t kFactionCmdActorOffset = 0x24;
inline constexpr size_t kFactionCmdObjectOffset = 0x20;
inline constexpr size_t kFactionCreateTargetOffset = 0x28;
//: A faction record's object_extra: what the AI posted.
inline constexpr uint32_t kFactionCreate = 0, kFactionJoin = 1, kFactionInteraction = 2;
//: The interactions by which a ruler starts or joins a faction (common/factions: claimant_faction and
//: ceremonial_claimant_faction name the first two as their character_interaction; 00_vassal_interactions.txt the
//: other two). Resolved by key on the game thread; the watched ruler is the actor.
inline constexpr int kFactionInteractionCount = 4;
inline constexpr const char* kFactionInteractionKeys[kFactionInteractionCount] = {
    "create_claimant_faction_against_interaction", "create_ceremonial_claimant_faction_against_interaction",
    "start_independence_faction_interaction", "join_independence_faction_interaction"};
inline bool faction_interaction(uint64_t itype, const uint64_t* resolved, int n) {
    for (int i = 0; itype && i < n; ++i)
        if (resolved[i] == itype) return true;
    return false;
}

//: A legacy's renown price, as the game's check reads it (FUN_06e00780 compares the dynasty's
//: spendable renown with FUN_05eaab70 = PERK_COST_MULTIPLIER x the dynasty's legacy count + PERK_COST_BASE; the
//: defines read 250 and 500, the core dump's globals too). `held`: the dynasty already holds this legacy (the post
//: passed and ran before the drain read the count), so the count includes it. -1: no price (a count that cannot be
//: one, or globals that did not read).
inline int64_t legacy_price(int32_t base, int32_t per, int32_t count, bool held) {
    if (base <= 0 || per < 0 || count < 0 || count > 128 || (held && count == 0)) return -1;
    return static_cast<int64_t>(per) * (held ? count - 1 : count) + base;
}

inline constexpr size_t ruler_offset(int what) {
    return what == static_cast<int>(What::kAllyCall) || what == static_cast<int>(What::kWar) ? kInteractionActorOffset
         : what == static_cast<int>(What::kVassalOffer) || what == static_cast<int>(What::kCallIn)
               ? kInteractionRecipientOffset
         : what == static_cast<int>(What::kMenAtArms) ? kMaaCreateOwnerOffset
         : what == static_cast<int>(What::kFaction) ? kFactionCmdActorOffset
         : what == static_cast<int>(What::kActivity) ? kActivityHostOffset : kCmdRulerOffset;
}

//: Who posted a command, from the return address of its GameCommandDispatch call: the game's
//: image, this bridge (libck3_bridge.so), or neither (a range not read, a tail call through a
//: third library). Only kGame is ever dropped.
enum class Origin : uint8_t { kGame = 0, kBridge = 1, kOther = 2 };
inline const char* origin_name(Origin o) {
    return o == Origin::kGame ? "game" : o == Origin::kBridge ? "bridge" : "other";
}
inline Origin classify_origin(uintptr_t ra, uintptr_t game_lo, uintptr_t game_hi, uintptr_t self_lo,
                              uintptr_t self_hi) {
    if (game_hi > game_lo && ra >= game_lo && ra < game_hi) return Origin::kGame;
    if (self_hi > self_lo && ra >= self_lo && ra < self_hi) return Origin::kBridge;
    return Origin::kOther;
}
//: The cross-check: the bridge sets a marker (its own posting thread) around its flags-7
//: call. A game post has no marker on its thread, a bridge post has it; anything else
//: disagrees and is logged, never resolved silently.
inline bool origin_agrees(Origin o, bool own_marker) {
    return (o == Origin::kGame && !own_marker) || (o == Origin::kBridge && own_marker);
}
//: The veto acts only on a post both tests call the game's.
inline bool bridge_own(Origin o, bool own_marker) { return o != Origin::kGame || own_marker; }
//: A decision is posted from inside the game image whoever asked (the bridge's own
//: send_decision calls AI_TRY_DECISION_CORE, which dispatches), so the return address is the
//: game's both times. The marker decides; the address only has to be the game's (anything
//: else is a disagreement). The recorded origin is the marker's answer.
inline bool decision_agrees(Origin o) { return o == Origin::kGame; }
inline Origin decision_origin(Origin o, bool own_marker) { return own_marker ? Origin::kBridge : o; }

inline constexpr double kFpScale = 100000.0;   // the game's fixed point
//: What=decision may name the decisions it drops (none named = every decision).
inline constexpr int kMaxDecisionKeys = 8;
inline constexpr int kDecisionKeyCap = 64;     // bytes, with the terminator
//: CExecuteDecisionCommand: the CDecisionType* (FUN_067cc8c0 stores it at cmd+0x28); its key is
//: the db object's std::string at +0x10 (data pointer, then the length at +0x18).
inline constexpr size_t kDecisionTypeOffset = 0x28;
inline constexpr size_t kDbKeyData = 0x10;
inline constexpr size_t kDbKeyLen = 0x18;
inline constexpr int kMaxRulers = 16;
//: The titles a create_title veto may spare per ruler: the goal title and the titles its creation still
//: needs (the game's tier rule: the de-jure duchies of a kingdom not yet held), set by the toolkit
inline constexpr int kMaxSpareTitles = 8;
inline constexpr int kRingCap = 128;
inline constexpr uint32_t kEmpty = 0;

//: One watched ruler: every AI post of the four commands for him is recorded; those
//: whose bit is set in `mask` are dropped.
struct Slot {
    std::atomic<uint32_t> id{kEmpty};        // 24-bit character id; 0 = free
    std::atomic<uint32_t> mask{0};           // bit (1 << what): vetoed
    std::atomic<uint64_t> dropped[kWhatCount] = {};
    std::atomic<uint64_t> passed[kWhatCount] = {};
    // the last dropped post put in the ring, per kind (record_drop): its object and date
    std::atomic<uint64_t> last_obj[kWhatCount] = {};
    std::atomic<int32_t> last_date[kWhatCount] = {};
    // The decision keys the decision veto names (none = every decision). A seqlock: the
    // writer makes keys_ver odd, writes, makes it even; a reader that sees it odd or changed
    // reads no list and passes the post (every doubt passes).
    std::atomic<uint32_t> keys_ver{0};
    std::atomic<int> key_count{0};
    char keys[kMaxDecisionKeys][kDecisionKeyCap] = {};
    // (0869.05.07: the create_title veto dropped the AI's creation of k_sweden, the title the
    // seat's task counts when the game's AI creates it): the titles (24-bit ids) the create_title veto never
    // drops; spare[0] the goal title. (1071.05-1072.05: the veto dropped the AI's d_leinster
    // three times with gold in hand, the duchy k_ireland's tier rule needed): up to kMaxSpareTitles, the goal's
    // chain too. Their posts are recorded and let through. Written under the table's mutex: ids first, then
    // the count (a reader sees at most an id that was spared a moment ago).
    std::atomic<uint32_t> spare[kMaxSpareTitles] = {};
    std::atomic<int> spare_count{0};
    // (the spending preset left decisions to the AI, which took a vow of poverty, -20% income for
    // life, at -86 gold): the decision veto also drops every decision whose cost block names gold (decision_costs_gold)
    std::atomic<uint32_t> gold_cost{0};
    // The activity types the activity veto lets through (the seat's own host_activity goes through the game's AI
    // pass, the same post); a seqlock as keys_ver
    std::atomic<uint32_t> act_ver{0};
    std::atomic<int> act_count{0};
    char act_keys[kMaxActivitySpares][kDecisionKeyCap] = {};
};

//: A CDecisionType's cost blocks (ck3_offsets.h decision_type CAN_TAKE_PRIMARY / CAN_TAKE_FALLBACK, both
//: CScriptedCost: 10 slots of SLOT_STRIDE from SLOT_BASE, each with its entry count at SLOT_COUNT; slot 0 is gold,
//: 1 prestige, 2 piety, as list_decisions_annotated reads them live: start_city_construction_decision gold 400,
//: lunatic_building_decision prestige 150). The game uses the primary block when any slot of it has an entry, else
//: the fallback (AI_TRY_DECISION_CORE's IS_EMPTY test, query_handlers.cpp dv_read_cost). An entry counts whatever
//: its value (a script value computed in play has an entry too).
inline constexpr int kCostSlots = 10;
inline bool decision_costs_gold(const uint32_t primary[kCostSlots], uint32_t fallback_gold) {
    bool any = false;
    for (int i = 0; i < kCostSlots; ++i) any = any || primary[i] != 0;
    return any ? primary[0] != 0 : fallback_gold != 0;
}

//: Is this create_title post's title one the slot's create_title veto spares?
inline bool title_spared(const Slot& s, uint64_t title_id) {
    int n = s.spare_count.load(std::memory_order_acquire);
    if (n < 0 || n > kMaxSpareTitles) n = 0;
    const uint32_t want = static_cast<uint32_t>(title_id & 0xFFFFFF);
    for (int i = 0; want != 0 && i < n; ++i)
        if (s.spare[i].load(std::memory_order_acquire) == want) return true;
    return false;
}
//: The first spared title (the goal's), 0 when none.
inline uint32_t first_spare(const Slot& s) {
    return s.spare_count.load(std::memory_order_acquire) > 0 ? s.spare[0].load(std::memory_order_acquire) : 0u;
}

//: Split "a,b,c" (decimal title ids; spaces ignored) into at most kMaxSpareTitles ids. -1 when an id is empty,
//: not a number, 0, over 24 bits, or there are too many.
inline int split_title_ids(const char* csv, uint32_t out[kMaxSpareTitles]) {
    if (!csv) return 0;
    int n = 0;
    uint64_t v = 0;
    bool digit = false, any = false;
    for (const char* p = csv;; ++p) {
        const char c = *p;
        if (c == ' ') continue;
        if (c == ',' || c == '\0') {
            if (!digit) { if (c == '\0' && !any) return 0; return -1; }
            if (n >= kMaxSpareTitles || v == 0 || v > 0xFFFFFF) return -1;
            out[n++] = static_cast<uint32_t>(v);
            v = 0; digit = false;
            if (c == '\0') return n;
            continue;
        }
        if (c < '0' || c > '9') return -1;
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > 0xFFFFFF) return -1;
        digit = any = true;
    }
}

struct Table {
    Slot slot[kMaxRulers];
    std::atomic<int> count{0};               // occupied slots: the hook's fast path

    int find(uint32_t id24) const {
        if (id24 == kEmpty) return -1;
        for (int i = 0; i < kMaxRulers; ++i)
            if (slot[i].id.load(std::memory_order_acquire) == id24) return i;
        return -1;
    }
    //: Writers (the IPC thread, the game thread) hold one mutex around these; the hook
    //: only loads. A new slot is published mask first, id last.
    int watch(uint32_t id24) {
        int at = find(id24);
        if (at >= 0 || id24 == kEmpty) return at;
        for (int i = 0; i < kMaxRulers; ++i) {
            if (slot[i].id.load(std::memory_order_acquire) != kEmpty) continue;
            slot[i].mask.store(0, std::memory_order_relaxed);
            slot[i].key_count.store(0, std::memory_order_relaxed);
            slot[i].spare_count.store(0, std::memory_order_relaxed);
            slot[i].gold_cost.store(0, std::memory_order_relaxed);
            slot[i].act_count.store(0, std::memory_order_relaxed);
            for (int w = 0; w < kWhatCount; ++w) {
                slot[i].dropped[w].store(0, std::memory_order_relaxed);
                slot[i].passed[w].store(0, std::memory_order_relaxed);
                slot[i].last_obj[w].store(0, std::memory_order_relaxed);
                slot[i].last_date[w].store(0, std::memory_order_relaxed);
            }
            slot[i].id.store(id24, std::memory_order_release);
            count.fetch_add(1, std::memory_order_acq_rel);
            return i;
        }
        return -1;
    }
    bool unwatch(uint32_t id24) {
        int at = find(id24);
        if (at < 0) return false;
        slot[at].mask.store(0, std::memory_order_release);
        slot[at].id.store(kEmpty, std::memory_order_release);
        count.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    //: Set or clear one what for a watched ruler. False when he is not watched.
    bool set_rule(uint32_t id24, int what, bool on) {
        if (what < 0 || what >= kWhatCount) return false;
        int at = find(id24);
        if (at < 0) return false;
        const uint32_t bit = 1u << what;
        if (on) slot[at].mask.fetch_or(bit, std::memory_order_acq_rel);
        else slot[at].mask.fetch_and(~bit, std::memory_order_acq_rel);
        return true;
    }
    //: (1069.09.27: the AI passed crown_authority_1 for the heir on his first day, -143
    //: prestige; the toolkit set the vetoes again only at its next read, a day later): at the seat's handoff
    //: from a dead ruler to his heir, the heir's slot takes the dead ruler's rules (mask and decision keys),
    //: OR-ed into any the heir already had. Returns the mask carried (0: nothing to carry, the dead ruler not
    //: watched, or no slot free for the heir). The dead ruler's slot is left as it was (records keep his id).
    uint32_t carry(uint32_t from24, uint32_t to24) {
        const int src = find(from24);
        if (src < 0 || to24 == kEmpty || to24 == from24) return 0;
        const uint32_t m = slot[src].mask.load(std::memory_order_acquire);
        if (m == 0) return 0;
        const int dst = watch(to24);
        if (dst < 0) return 0;
        const int bit_decision = static_cast<int>(What::kDecision);
        if (m & (1u << bit_decision)) {
            const char* kp[kMaxDecisionKeys];
            int n = slot[src].key_count.load(std::memory_order_acquire);
            if (n < 0 || n > kMaxDecisionKeys) n = 0;
            for (int i = 0; i < n; ++i) kp[i] = slot[src].keys[i];
            set_decision_keys(to24, kp, n);
            if (slot[src].gold_cost.load(std::memory_order_acquire))                 // The gold rule too
                slot[dst].gold_cost.store(1, std::memory_order_release);
        }
        if (slot[dst].spare_count.load(std::memory_order_acquire) == 0) {        // The spared titles too
            uint32_t ids[kMaxSpareTitles];
            int n = slot[src].spare_count.load(std::memory_order_acquire);
            if (n < 0 || n > kMaxSpareTitles) n = 0;
            for (int i = 0; i < n; ++i) ids[i] = slot[src].spare[i].load(std::memory_order_acquire);
            set_spare_titles(to24, ids, n);
        }
        if (slot[dst].act_count.load(std::memory_order_acquire) == 0) {          // The spared activities too
            const char* kp[kMaxActivitySpares];
            int n = slot[src].act_count.load(std::memory_order_acquire);
            if (n < 0 || n > kMaxActivitySpares) n = 0;
            for (int i = 0; i < n; ++i) kp[i] = slot[src].act_keys[i];
            set_activity_spares(to24, kp, n);
        }
        slot[dst].mask.fetch_or(m, std::memory_order_acq_rel);
        return m;
    }
    //: The title the create_title veto of a watched ruler spares (0: none). False when he is not watched.
    bool set_spare_title(uint32_t id24, uint32_t title24) {
        const uint32_t t = title24 & 0xFFFFFF;
        return set_spare_titles(id24, &t, t ? 1 : 0);
    }
    //: The titles it spares (n = 0: none; zeros skipped). False when he is not watched or n is over the cap.
    bool set_spare_titles(uint32_t id24, const uint32_t* ids, int n) {
        int at = find(id24);
        if (at < 0 || n < 0 || n > kMaxSpareTitles) return false;
        Slot& s = slot[at];
        s.spare_count.store(0, std::memory_order_release);                 // shrink first: no stale id is read
        int k = 0;
        for (int i = 0; i < n; ++i)
            if (ids[i] & 0xFFFFFF) s.spare[k++].store(ids[i] & 0xFFFFFF, std::memory_order_release);
        s.spare_count.store(k, std::memory_order_release);
        return true;
    }
    //: Whether a watched ruler's decision veto also drops every decision that costs gold. False when he is not
    //: watched.
    bool set_decision_gold(uint32_t id24, bool on) {
        int at = find(id24);
        if (at < 0) return false;
        slot[at].gold_cost.store(on ? 1 : 0, std::memory_order_release);
        return true;
    }
    //: The activity types a watched ruler's activity veto lets through (n = 0: none). False when he is not
    //: watched, n is over kMaxActivitySpares, or a key is empty or too long.
    bool set_activity_spares(uint32_t id24, const char* const* k, int n) {
        int at = find(id24);
        if (at < 0 || n < 0 || n > kMaxActivitySpares) return false;
        for (int i = 0; i < n; ++i)
            if (!k[i] || !k[i][0] || std::strlen(k[i]) >= static_cast<size_t>(kDecisionKeyCap)) return false;
        Slot& s = slot[at];
        char tmp[kMaxActivitySpares][kDecisionKeyCap] = {};
        for (int i = 0; i < n; ++i) std::memcpy(tmp[i], k[i], std::strlen(k[i]));   // k may point into this slot
        s.act_ver.fetch_add(1, std::memory_order_acq_rel);           // odd: writing
        for (int i = 0; i < kMaxActivitySpares; ++i) std::memcpy(s.act_keys[i], tmp[i], kDecisionKeyCap);
        s.act_count.store(n, std::memory_order_release);
        s.act_ver.fetch_add(1, std::memory_order_acq_rel);           // even: done
        return true;
    }
    //: The decision keys of a watched ruler's decision veto (n = 0: every decision). False
    //: when he is not watched, n is over kMaxDecisionKeys, or a key is empty or too long.
    bool set_decision_keys(uint32_t id24, const char* const* k, int n) {
        int at = find(id24);
        if (at < 0 || n < 0 || n > kMaxDecisionKeys) return false;
        for (int i = 0; i < n; ++i)
            if (!k[i] || !k[i][0] || std::strlen(k[i]) >= static_cast<size_t>(kDecisionKeyCap)) return false;
        Slot& s = slot[at];
        s.keys_ver.fetch_add(1, std::memory_order_acq_rel);          // odd: writing
        for (int i = 0; i < kMaxDecisionKeys; ++i) std::memset(s.keys[i], 0, kDecisionKeyCap);
        for (int i = 0; i < n; ++i) std::memcpy(s.keys[i], k[i], std::strlen(k[i]));
        s.key_count.store(n, std::memory_order_release);
        s.keys_ver.fetch_add(1, std::memory_order_acq_rel);          // even: done
        return true;
    }
};

//: Does a decision post with this key fall under the slot's decision veto? Every
//: decision when no key is named; else only a named key. A list being rewritten (seqlock odd
//: or changed under the read) or an unread key answers false: the post passes.
//: With the slot's gold rule set, a decision that costs gold (`costs_gold`, decision_costs_gold) falls under it
//: too, and no key named then means only those (not every decision).
enum class KeyMatch : uint8_t { kNo = 0, kYes = 1 };
inline KeyMatch decision_key_matches(const Slot& s, const char* key, size_t len, bool costs_gold = false) {
    const bool gold = s.gold_cost.load(std::memory_order_acquire) != 0;
    if (gold && costs_gold) return KeyMatch::kYes;
    const uint32_t v0 = s.keys_ver.load(std::memory_order_acquire);
    if (v0 & 1u) return KeyMatch::kNo;
    const int n = s.key_count.load(std::memory_order_acquire);
    if (n <= 0) return !gold && s.keys_ver.load(std::memory_order_acquire) == v0 ? KeyMatch::kYes : KeyMatch::kNo;
    if (!key || len == 0 || len >= static_cast<size_t>(kDecisionKeyCap)) return KeyMatch::kNo;
    bool hit = false;
    for (int i = 0; i < n && i < kMaxDecisionKeys && !hit; ++i)
        hit = std::strlen(s.keys[i]) == len && std::memcmp(s.keys[i], key, len) == 0;
    if (s.keys_ver.load(std::memory_order_acquire) != v0) return KeyMatch::kNo;
    return hit ? KeyMatch::kYes : KeyMatch::kNo;
}

//: Is this activity type one the slot's activity veto lets through? A list being rewritten answers true (every
//: doubt passes), an unread key false (a post whose type did not read is dropped under the veto like any other).
inline bool activity_spared(const Slot& s, const char* key, size_t len) {
    const uint32_t v0 = s.act_ver.load(std::memory_order_acquire);
    if (v0 & 1u) return true;
    const int n = s.act_count.load(std::memory_order_acquire);
    if (n <= 0 || !key || len == 0 || len >= static_cast<size_t>(kDecisionKeyCap)) return false;
    bool hit = false;
    for (int i = 0; i < n && i < kMaxActivitySpares && !hit; ++i)
        hit = std::strlen(s.act_keys[i]) == len && std::memcmp(s.act_keys[i], key, len) == 0;
    if (s.act_ver.load(std::memory_order_acquire) != v0) return true;
    return hit;
}

//: Split the comma-separated key list set_ai_veto takes ("a,b"; spaces ignored) into at most
//: kMaxDecisionKeys keys of [a-z0-9_]. -1 when a key is empty, too long or carries another
//: character, or there are too many.
inline int split_decision_keys(const char* csv, char out[kMaxDecisionKeys][kDecisionKeyCap]) {
    if (!csv) return 0;
    int n = 0;
    size_t len = 0;
    bool any = false;
    for (const char* p = csv;; ++p) {
        const char c = *p;
        if (c == ' ') continue;
        if (c == ',' || c == '\0') {
            if (len == 0) { if (c == '\0' && !any) return 0; return -1; }
            if (n >= kMaxDecisionKeys) return -1;
            out[n][len] = '\0';
            ++n; len = 0;
            if (c == '\0') return n;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok || len + 1 >= static_cast<size_t>(kDecisionKeyCap) || n >= kMaxDecisionKeys) return -1;
        out[n][len++] = c;
        any = true;
    }
}

enum class Verdict : uint8_t { kPass = 0, kRecord, kDrop };

//: The hook's question. `what` is the command's class (kNone: none of the kinds), `flags`
//: the dispatch flags, `ruler_full` the acting ruler (ruler_offset). kRecord: a watched ruler's
//: post, let through; kDrop: his AI's post of a vetoed what. `own`: the bridge posted it
//: (bridge_own): recorded, never dropped.
inline Verdict decide(const Table& t, int what, uint32_t flags, uint32_t ruler_full, int* slot_out,
                      bool own = false, bool key_vetoed = true) {
    *slot_out = -1;
    if (what < 0 || what >= kWhatCount) return Verdict::kPass;
    if (flags != kAiDispatchFlags) return Verdict::kPass;
    if (t.count.load(std::memory_order_acquire) <= 0) return Verdict::kPass;
    const uint32_t id24 = ruler_full & 0xFFFFFF;
    const int at = t.find(id24);
    if (at < 0) return Verdict::kPass;
    const uint32_t m = t.slot[at].mask.load(std::memory_order_acquire);
    if (t.slot[at].id.load(std::memory_order_acquire) != id24) return Verdict::kPass;   // reused meanwhile
    *slot_out = at;
    // key_vetoed: for a decision, whether its key falls under the slot's list
    return (m & (1u << what)) && !own && key_vetoed ? Verdict::kDrop : Verdict::kRecord;
}

//: A vetoed AI retries: the Lifestyles pass picks a perk again at each pass and the
//: interactions pass calls the ally again. A drop of the same object for the same ruler
//: within `window` (raw date units) of the last one put in the ring is counted (Slot::dropped)
//: but not recorded again, so one retrying kind cannot push every other record out of the
//: ring. `last_date` 0 = nothing recorded yet; an unread date (0) is always recorded.
inline bool record_drop(uint64_t last_obj, int32_t last_date, uint64_t obj, int32_t now, int32_t window) {
    if (last_date == 0 || now == 0 || obj != last_obj) return true;
    return now - last_date >= window || now < last_date;
}

//: The AI's interactions for watched rulers, counted by (ruler, interaction type) and never
//: dropped: what a ruler's AI sends in a decade (which of them spend his purse is the
//: question a new veto kind answers). Fixed size, a spin lock, no allocation in the hook.
struct CensusEntry {
    uint64_t itype = 0;      // CCharacterInteractionType*
    uint32_t ruler = 0;      // 24-bit
    uint32_t count = 0;
    int32_t first_date = 0, last_date = 0;
    uint64_t site = 0;       // the last post's return address minus the image base
};
//: (another session: 10 claimed rulers filled 96 pairs in five game-years, and the posts past them were only a
//: count): 512 pairs (20 KB a table), and the pairs that found no slot are kept by key (up to kOverflowKeys) so the
//: reply says how many pairs it does not show; past that the count is a floor (pairs_not_shown_exact false).
inline constexpr int kCensusCap = 512;
inline constexpr int kOverflowKeys = 256;
class Census {
public:
    //: Count one post; false when the table is full (counted in overflow(), its pair in pairs_not_shown()).
    bool add(uint64_t itype, uint32_t ruler24, int32_t date, uint64_t site) {
        lock();
        int at = -1;
        for (int i = 0; i < n_; ++i)
            if (e_[i].itype == itype && e_[i].ruler == ruler24) { at = i; break; }
        if (at < 0 && n_ < kCensusCap) {
            at = n_++;
            e_[at] = CensusEntry{};
            e_[at].itype = itype; e_[at].ruler = ruler24; e_[at].first_date = date;
        }
        if (at >= 0) { ++e_[at].count; e_[at].last_date = date; e_[at].site = site; }
        else {
            ++overflow_;
            bool seen = false;
            for (int i = 0; i < nov_ && !seen; ++i) seen = ov_[i].itype == itype && ov_[i].ruler == ruler24;
            if (!seen) {
                if (nov_ < kOverflowKeys) { ov_[nov_].itype = itype; ov_[nov_].ruler = ruler24; ++nov_; }
                else ov_full_ = true;
            }
        }
        unlock();
        return at >= 0;
    }
    //: The distinct (ruler, type) pairs that found no slot: exact while exact() (else a floor).
    int pairs_not_shown() { lock(); int k = nov_; unlock(); return k; }
    bool pairs_not_shown_exact() { lock(); bool x = !ov_full_; unlock(); return x; }
    //: Of those, the ones of one ruler (0: every ruler).
    int pairs_not_shown_for(uint32_t ruler24) {
        lock();
        int k = 0;
        for (int i = 0; i < nov_; ++i) k += (!ruler24 || ov_[i].ruler == ruler24) ? 1 : 0;
        unlock();
        return k;
    }
    int copy(CensusEntry* out, int cap) {
        lock();
        const int k = n_ < cap ? n_ : cap;
        for (int i = 0; i < k; ++i) out[i] = e_[i];
        unlock();
        return k;
    }
    uint64_t overflow() { lock(); uint64_t o = overflow_; unlock(); return o; }

private:
    void lock() { while (flag_.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { flag_.clear(std::memory_order_release); }
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    int n_ = 0;
    uint64_t overflow_ = 0;
    CensusEntry e_[kCensusCap];
    struct Key { uint64_t itype = 0; uint32_t ruler = 0; };
    Key ov_[kOverflowKeys];
    int nov_ = 0;
    bool ov_full_ = false;
};

//: One AI post the hook saw for a watched ruler. Plain data: the hook fills the first
//: block; the game thread's drain fills the price block (`priced` 0 -> 1 or 2).
struct Record {
    uint64_t seq = 0;
    int32_t date_raw = 0;
    uint32_t ruler = 0;                      // 24-bit
    int8_t what = kNone;
    bool dropped = false;
    uint32_t object_id = 0;                  // title id / company id / holding id / ally_call: the ally /
                                             // call_in: the ruler who called
    uint32_t object_extra = 0;               // construction: building type index
    uint64_t object_ptr = 0;                 // law: the CLaw* at cmd+0x28; perk: the CLifestylePerk*;
                                             // ally_call, call_in: the war's full id (cmd+0x318);
                                             // decision: the CDecisionType* at cmd+0x28;
                                             // legacy: the CDynastyLegacyPerk* at cmd+0x28
    uint8_t origin = 0;                      // Origin (classify_origin of the dispatch's return address)
    bool origin_agrees = true;               // origin_agrees() with the bridge's own-post marker
    uint64_t site = 0;                       // the return address minus the image base (game posts)
    bool purse_read = false;
    int64_t gold_fp = 0, prestige_fp = 0, piety_fp = 0;   // the ruler's, at the post
    // filled by the drain
    uint8_t priced = 0;                      // 0 pending, 1 priced, 2 no price read
    int64_t cost_gold_fp = 0, cost_prestige_fp = 0, cost_piety_fp = 0;
    int64_t cost_renown_fp = 0;              // legacy: the dynasty's renown price
    char currency[12] = {};                  // create_title: gold | treasury | piety
    char key[64] = {};                       // title / law / building / decision key ("" unread)
    char why[40] = {};                       // priced == 2: why not
};

inline void copy_text(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = src ? std::strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) std::memcpy(dst, src, n);
    dst[n] = '\0';
}

//: The attempt ring. A spin lock (no allocation, held for one copy) serialises the hook
//: (any AI worker thread), the drain and the readers.
class Ring {
public:
    uint64_t push(Record r) {
        lock();
        r.seq = ++next_seq_;
        buf_[static_cast<size_t>((r.seq - 1) % kRingCap)] = r;
        unlock();
        return r.seq;
    }
    uint64_t last_seq() {
        lock();
        uint64_t s = next_seq_;
        unlock();
        return s;
    }
    //: Copy the record with this seq; false when it was overwritten or never written.
    bool get(uint64_t seq, Record* out) {
        if (seq == 0) return false;
        lock();
        const Record& r = buf_[static_cast<size_t>((seq - 1) % kRingCap)];
        const bool ok = r.seq == seq;
        if (ok) *out = r;
        unlock();
        return ok;
    }
    //: Write back a priced record only if its slot still holds that seq.
    bool put_priced(const Record& r) {
        lock();
        Record& at = buf_[static_cast<size_t>((r.seq - 1) % kRingCap)];
        const bool ok = r.seq != 0 && at.seq == r.seq;
        if (ok) at = r;
        unlock();
        return ok;
    }
    //: The oldest seq still held.
    uint64_t first_seq() {
        lock();
        uint64_t s = next_seq_ > static_cast<uint64_t>(kRingCap) ? next_seq_ - kRingCap + 1 : 1;
        unlock();
        return s;
    }

private:
    void lock() { while (flag_.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { flag_.clear(std::memory_order_release); }
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    uint64_t next_seq_ = 0;
    Record buf_[kRingCap];
};

}  // namespace bridge::ai_veto
