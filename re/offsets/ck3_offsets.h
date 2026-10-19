// CK3 Binary Offsets — Derived from RE of CK3 Linux binary
// BuildID: e61ff2642f07094defd140fcc98e078d42541275
// These WILL change with game patches. See CLAUDE.md Phase 4 for re-mapping strategy.
//
// ═══════════════════════════════════════════════════════════════════════════
// ADDRESS CONVENTION (canonical — audited ck-e58l.20, 2026-04-21)
// ═══════════════════════════════════════════════════════════════════════════
// All uintptr_t values in this header are RAW (ASLR-base-relative /
// file-offset-relative), NOT Ghidra virtual addresses. Ghidra's image base
// for this binary is 0x00100000. To convert between forms:
//   raw       = ghidra_va - 0x00100000
//   ghidra_va = raw + 0x00100000
//
// The CoreReader (re/coredump/core_reader.py) and the bridge code both
// assume raw: CoreReader.resolve(val) = base_addr + val. When adding a new
// entry, always convert from the Ghidra VA via `val - 0x00100000`.
//
// ck-e58l.20 swept the entire header and converted 189 Ghidra-VA-as-raw
// leaks to their raw form; entries carrying that audit tag were corrected
// in this pass. See re/findings/re_offset_header_convention_sweep.md.
// ═══════════════════════════════════════════════════════════════════════════
//
// Confidence levels:
//   HIGH   = 3+ independent accessor functions confirm, or decompilation-verified
//   MEDIUM = 2 accessor functions or structural inference from related offsets
//
// Round 3 update: traits, family, claims, succession, events, factions,
//                 realm verification, military, interaction/decision resolution
// Round 4 update: opinion accessor, trait names, title chain, war declaration pipeline
// Round 5 update: schemes, war battles, council positions, house/dynasty DBs,
//                 interaction DB array, db_object name key, character house ID
// Dynasty RE update: CDynasty struct (prestige/renown, prestige_level, legacy perks),
//                    CDynastyHouse struct, dynasty functions, perk cost globals
// Round 5c-f update: CWar full struct, CWarParticipant, CCasusBelli,
//                    culture/faith IDs on char, faction list, law container,
//                    faith DB, realm law DB
// Round 6b update: army DB/enumeration, council task direct setter,
//                  vassal contract obligation, army owner link
// Round 7 update: stress/dread vitals, scheme manager singleton access,
//                 relation manager, lifestyle/perk, scheme manager flags
// Round 9 update: opinion modifier struct, income utility functions,
//                 focus DB singleton, CActiveFocus/SLifestyleXpData RTTI
// Round 9 update: opinion system architecture (GetOpinionOf, COpinionModifier struct),
//                 income_utilities.cpp functions, focus/lifestyle DB singleton,
//                 CActiveFocus/SLifestyleXpData RTTI types
// Round 10 update: income calculators (domain/vassal/tributary/modifier/monthly),
//                  legitimacy accessor, CB validation pipeline, trigger evaluator,
//                  innovation type DB + struct, culture struct, realm title/domain funcs
// Round 10-8 update: CActiveScheme struct corrected from CreateActiveScheme/InitializeScheme
//                    decompilation — scheme_type is ptr at +0x18 (not owner), target is
//                    split into target_type(+0x28)/target_ref(+0x2C), progress at +0x70,
//                    added freeze/timing/agent fields, RefDB validates at +0x10 not +0x18
// Round 10-3 update: CDiplomaticRelation struct (0x1F0 bytes), pairwise truce storage,
//                    relation container sorted array at core_data+0x20, truce sentinel/null
//                    globals, truce/relation function addresses
// Round 10-4 update: CHookTypeDatabase singleton, EHookStrength enum (5 values),
//                    hook/secret RTTI addresses, hook trigger/effect function addresses,
//                    secret crime status enum
// Round secret-RE:   CActiveSecret full struct layout (15 fields, 0xA8 bytes) from
//                    constructor decompilation + 16-instance runtime validation,
//                    CSecretManager access chain (GSR→+0xa0+0xCED0→RefDB),
//                    CSecretType key fields (18 types enumerated),
//                    CORRECTION: char+0x448 is heritage not secrets
// Round 10-2 update: alliance storage (CActiveRelation alliance fields, forced allies,
//                    SAllianceReason struct, alliance reason enum, IsAlliedTo/GetAllianceReasons/
//                    CreateAlliance function addresses)
// Round 10-5 update: income pipeline (domain/vassal/tributary/expense/net), holding income,
//                    tributary array at ruler+0x248, MaA regiment refs at ruler+0x278,
//                    barter accumulator at ruler+0x488, modifier indices, expense functions
// R8V/R9 update: focus system (CActiveFocus), scheme agent slots, scheme progress UI offset,
//                 council instance rename, dread secondary, lifestyle getter functions
// Round 10-6 update: CRegiment DB + struct (chunks/type/max_count/source_flag),
//                    CMenAtArmsType cost fields, CMaaOwnerLink array, military stats result,
//                    personal/merc/holy_order/title MaA arrays on ruler_data, regiment functions
// R8V-6 update: succession window verification — designated heir ref on ruler_data,
//               SSuccessionRule extended fields (scope ptrs, gov types), law DB entry stride,
//               succession law/heir resolution function addresses
// Round 11 update: CB validity pipeline (CCasusBelliType struct, CB enumeration functions),
//                  war declaration command flow (WAR_CREATE_CORE, interaction indices),
//                  succession functions (BuildSuccessionOrder, heir title arrays),
//                  military 8-category breakdown (corrected quality semantics, holy order/merc DBs)
// Round 12 update: AI Watch Window system (CAIWatchWindow struct, populate/update functions,
//                  AI evaluation sub-functions, CPdxArray layout, allocator vtable globals)
// R25 Phase 4 update: Title-side claimant storage (CLandedTitle+0x2A8/0x2B0/0x2B4/0x2B8),
//                     AddClaimant/RemoveClaimant/GetClaimState decompiled,
//                     CCharacter::GetClaimsArray confirmed at core_data+0x1E8
// Council voting update: CSuccessionElectionState struct (title+0x280), SVoteEntry layout,
//                        election pipeline functions (GetVoteTarget, EvalVoteScore, AIElectionVote),
//                        CLaw/CLawGroup structs, title_realm_data law container (+0x200/+0x208),
//                        crown_authority = council power (levels 0-3)
// Vassal contract update: CSubjectContract full struct (0xD8 bytes), obligation types/levels,
//                         CSubjectContractType key names, 8 contract classes mapped,
//                         serializer/deserializer verified, misnamed Round 6b functions corrected
// R-partition-2 update: scoring weights confirmed (own=6, other=1), 5 helper functions decoded
//                       (FindQualifyingDeJureLiege, PrunePartitionCandidate, AddCandidateToList,
//                       IsInDomain, CheckDynastyPartitionPermission), fair share algorithm traced,
//                       BuildSuccessionForRuler title grouping logic, CLandedTitle flag semantics
// Contract negotiation scoring (ck-x91l): 15 function addresses for AI acceptance scoring,
//                         contract_scoring_globals namespace (10 configuration addresses),
//                         contract_manager struct (15 fields), contract_entry struct (12 fields),
//                         hook leverage mechanics (4 hook types), 8-category obligation scoring
// Effect executor RE (ck-n18o, corrected ck-e58l.15): CJominiEffect base vtable is 25 slots
//                     (not 16). Execute is slot [23] at +0xB8 (not slot [7]); slot [7] is
//                     GetScopeMask. CStartWarEffect::CreateWar at slot [23] is the actual
//                     executor — ck-n18o's slot-[7] claim and ck-1ps.5's slot-[9] claim were
//                     both wrong. Stale slot-[7]-as-Execute constants removed ck-e58l.30.
// Pending interaction consumer RE (ck-1ps.5): CPendingCharacterInteractionManager tick function,
//                     consumer re-validates with CanSend before processing — silent discard on failure
#pragma once
#include <cstddef>
#include <cstdint>

namespace ck3 {

// =============================================================================
// Global Variable Addresses (absolute, relative to binary base)
// These need ASLR adjustment at runtime: actual = base + offset
// =============================================================================
namespace globals {
    // AI system
    constexpr uintptr_t AI_ENABLED_FLAG     = 0x0beacf71;  // CONFIRMED: byte, checked in tick loop
    constexpr uintptr_t AI_GLOBAL_TOGGLE    = 0x0beacf72;  // byte: set by AI.Enable/Disable
    constexpr uintptr_t AI_RNG_COUNTER      = 0x0bea2018;  // uint32: global AI random counter
    constexpr uintptr_t AI_WAR_SUPPRESS     = 0x0beafb48;  // byte: suppresses war eval
    constexpr uintptr_t AI_EVENT_EVAL_FLAG  = 0x0c5ce5f0;  // HIGH: byte, AI event evaluation enabled

    // Database singletons
    constexpr uintptr_t GAME_STATE_ROOT     = 0x0c2b40f0;  // CONFIRMED: ptr, game state root
    // Process-global singleton ptr → the constructed CAIWarStanceDatabase (0x1020
    // bytes). *(this) = CAIWarStanceDatabase*; setter STANCE_DB_CTOR (FUN_06ed25b0)
    // does _DAT_0c19e380 = new char[0x1020]. Embedded SAIWarStance* slots at +0xfb0
    // (attacker_offensive) .. +0x1010 (ghw_defender). (re_caiwarcoordinator_FINAL.md)
    constexpr uintptr_t STANCE_DB_GLOBAL    = 0x0c09e380;  // HIGH
    constexpr uintptr_t CHARACTER_DB        = 0x0becf088;  // CONFIRMED: character hash map (Round 5c-f verified)
    constexpr uintptr_t INTERACTION_DB      = 0x0bed7748;  // ptr: interaction database singleton
    constexpr uintptr_t TRAIT_DB            = 0x0bee3d60;  // HIGH: CTraitDatabase singleton, hash table at +0x1b0
    constexpr uintptr_t BUILDING_TYPE_DB    = 0x0bedf588;  // MEDIUM: building type database
    constexpr uintptr_t CULTURE_DB          = 0x0bf29938;  // CONFIRMED: culture database
    constexpr uintptr_t FAITH_DB            = 0x0bedad78;  // HIGH: faith database (ID-indexed array)
    constexpr uintptr_t NULL_FAITH_SENTINEL = 0x0bedad80;  // HIGH: default null faith (ck-pj01)
    constexpr uintptr_t RELIGION_TEMPLATE_DB = 0x0beda798; // HIGH: religion template lookup DB (ck-pj01)
    constexpr uintptr_t NULL_RELIGION_SENTINEL = 0x0beda7a0; // HIGH: default null religion template (ck-pj01)
    constexpr uintptr_t NULL_CULTURE_SENTINEL  = 0x0beda7b0; // HIGH: default null culture (ck-pj01)
    constexpr uintptr_t REALM_LAW_DB        = 0x0bedad50;  // HIGH: realm law DB (+0x88=array, +0x94=count)
    constexpr uintptr_t FACTION_TYPE_DB_V2  = 0x0bee10b8;  // HIGH: faction type DB (+0x88=array, +0x94=count)

    // Siege system (ck-mol-6uaq: RTTI + coredump + Ghidra decompilation)
    constexpr uintptr_t SIEGE_DB             = 0x0bed8928;  // HIGH: TPdxRefDatabase<CSiege, 8> singleton
    constexpr uintptr_t SIEGE_NULL           = 0x0bed8930;  // HIGH: TPdxNullObject<CSiege> sentinel

    // Modifier system (Round 9-4)
    constexpr uintptr_t MODIFIER_DEF_DB    = 0x0bf32e98;  // HIGH: CModifierDefinitionDatabase singleton (~1944 definitions) (ck-e58l.20)
    constexpr uintptr_t FAITH_DOCTRINE_DB_BAD = 0x0bfdaf88;  // WRONG: Ghidra VA not debiased (+0x100000 off) — live read = 0/unmapped
    constexpr uintptr_t FAITH_DOCTRINE_DB  = 0x0bedaf88;  // HIGH (CORRECTED 2026-06-18, live PID 898009): CFaithDoctrineDatabase singleton (game_db layout: entries@0x88, count@0x90, name@db_object::NAME_KEY); 243 entries live
    constexpr uintptr_t FAITH_DOCTRINE_GROUP_DB = 0x0bedaf88; // MEDIUM: CFaithDoctrineGroupDatabase (corrected with FAITH_DOCTRINE_DB)
    constexpr uintptr_t NULL_FAITH_DOCTRINE = 0x0c0950c0;  // MEDIUM: TPdxNullObject<CFaithDoctrine> (null sentinel)
    constexpr uintptr_t INNOVATION_TYPE_DB   = 0x0bee09e8;  // HIGH: CCultureInnovationTypeDatabase hash-table DB
    constexpr uintptr_t NULL_INNOVATION_TYPE = 0x0c0950c0;  // MEDIUM: TPdxNullObject<CCultureInnovationType> (null sentinel)
    // Round 14: Research speed globals (BSS — populated at runtime, not in static binary)
    constexpr uintptr_t SPREAD_SPEED_MULT     = 0x0c2acac8;  // MEDIUM: int64, spread speed multiplier (ck-e58l.20)
    constexpr uintptr_t FASCINATION_SPEED_MULT = 0x0c2acae0; // MEDIUM: int64, fascination speed multiplier (ck-e58l.20)
    constexpr uintptr_t ERA_TYPE_DB          = 0x0bee09e8;  // HIGH: CCultureEraTypeDatabase hash-table DB (ck-e58l.20)
    constexpr uintptr_t NULL_ERA_TYPE        = 0x0c0950c8;  // MEDIUM: TPdxNullObject<CCultureEraType> (null sentinel)

    // Pause system — CJominiIdler singleton (HIGH, 6-way cross-validated)
    constexpr uintptr_t IDLER_SINGLETON     = 0x0c5b53e8;  // CONFIRMED: CJominiIdler* singleton

    // AI toggle flags (one byte each, 38 subsystems contiguous)
    // These control which AI subsystems are active per-tick
    // R14 GROUND TRUTH: derived from console command registration analysis
    constexpr uintptr_t AI_TOGGLE_BASE      = 0x0bea5c78;
    // Individual subsystem toggle offsets (relative to AI_TOGGLE_BASE)
    // +0x00 DAT_0bfa5c78 = Decisions           (AI.ToggleDecisions)
    // +0x01 DAT_0bfa5c79 = GrantRevokeTitles   (AI.ToggleGrantRevokeTitles)
    // +0x02 DAT_0bfa5c7a = SetRealmCapital      (AI.ToggleSetRealmCapital)
    // +0x03 DAT_0bfa5c7b = Lifestyles           (AI.ToggleLifestyles)
    // +0x04 DAT_0bfa5c7c = DynastyLegacies      (AI.ToggleDynastyLegacies)
    // +0x05 DAT_0bfa5c7d = HousePower           (AI.ToggleHousePower)
    // +0x06 DAT_0bfa5c7e = CultureTraditions    (AI.ToggleCultureTraditions)
    // +0x07 DAT_0bfa5c7f = DeclareWar           (AI.ToggleDeclareWar)
    // +0x08 DAT_0bfa5c80 = Faction              (AI.ToggleFaction)
    // +0x09 DAT_0bfa5c81 = Marriages            (AI.ToggleMarriages)
    // +0x0A DAT_0bfa5c82 = Council              (AI.ToggleCouncil)
    // +0x0B DAT_0bfa5c83 = CouncilTasks         (AI.ToggleCouncilTasks)
    // +0x0C DAT_0bfa5c84 = SchemeCountermeasures (AI.ToggleSchemeCountermeasures)
    // +0x0D DAT_0bfa5c85 = CallAllies           (AI.ToggleCallAllies)
    // +0x0E DAT_0bfa5c86 = Development          (AI.ToggleDevelopment)
    // +0x0F DAT_0bfa5c87 = ReligiousReformation (AI.ToggleReligiousReformation)
    // +0x10 DAT_0bfa5c88 = SuccessionElection   (AI.ToggleSuccessionElection)
    // +0x11 DAT_0bfa5c89 = SuccessionAppointment (AI.ToggleSuccessionAppointment)
    // +0x12 DAT_0bfa5c8a = Goals                (AI.ToggleGoals)
    // +0x13 DAT_0bfa5c8b = Interactions         (AI.ToggleInteractions)
    // +0x14 DAT_0bfa5c8c = Laws                 (AI.ToggleLaws)
    // +0x15 DAT_0bfa5c8d = PrimaryTitle         (AI.TogglePrimaryTitle)
    // +0x16 DAT_0bfa5c8e = Capital              (AI.ToggleCapital)
    // +0x17 DAT_0bfa5c8f = ScriptedGui          (AI.ToggleScriptedGui)
    // +0x18 DAT_0bfa5c90 = Artifacts            (AI.ToggleArtifacts)
    // +0x19 DAT_0bfa5c91 = CourtAmenities       (AI.ToggleCourtAmenities)
    // +0x1A DAT_0bfa5c92 = CourtTypes           (AI.ToggleCourtTypes)
    // +0x1B DAT_0bfa5c93 = CourtPositions       (AI.ToggleCourtPositions)
    // +0x1C DAT_0bfa5c94 = CourtLanguage        (AI.ToggleCourtLanguage)
    // +0x1D DAT_0bfa5c95 = HostActivities       (AI.ToggleHostActivities)
    // +0x1E DAT_0bfa5c96 = AttendOpenActivities (AI.ToggleAttendOpenActivities)
    // +0x1F DAT_0bfa5c97 = VassalStance         (AI.ToggleVassalStance)
    // +0x20 DAT_0bfa5c98 = Accolades            (AI.ToggleAccolades)
    // +0x21 DAT_0bfa5c99 = StationMenAtArms     (AI.ToggleStationMenAtArms)
    // +0x22 DAT_0bfa5c9a = TaxCollector         (AI.ToggleTaxCollector)
    // +0x23 DAT_0bfa5c9b = Legends              (AI.ToggleLegends)
    // +0x24 DAT_0bfa5c9c = Situations           (AI.ToggleSituations)
    // +0x25 DAT_0bfa5c9d = GreatProjects        (AI.ToggleGreatProjects)

    // Threading flags (per-subsystem, controls direct vs job-queue execution)
    constexpr uintptr_t THREAD_FLAG_STRATEGY = 0x0bea5e20;  // 0=serial, non-0=parallel
    constexpr uintptr_t THREAD_FLAG_MARRIAGE = 0x0bea5e27;
    constexpr uintptr_t THREAD_FLAG_INTERACT = 0x0bea5e2c;

    // Character DB sentinel (null character pointer)
    constexpr uintptr_t CHAR_DB_SENTINEL     = 0x0becf090;

    // Game state
    constexpr uintptr_t GAME_IS_LOADED       = 0x05f94f20;

    // AI null controller sentinel
    constexpr uintptr_t AI_NULL_CONTROLLER   = 0x0c086150;

    // Player character ID global (Ghidra DAT_0bfad8c0 - 0x100000)
    // Contains the player's character ID (e.g. 36010 = Matilda).
    // Both FUN_062642d0 and FUN_062642e0 read from this address.
    // Previously mislabeled as "CACHED_GAME_DATE" -- it is NOT a game date.
    // The actual game date is at *(*(GAME_STATE_ROOT) + game_state::GAME_DATE).
    constexpr uintptr_t PLAYER_CHAR_ID       = 0x0bead8c0;  // int32: player character ID
    constexpr uintptr_t CACHED_GAME_DATE     = 0x0bead8c0;  // DEPRECATED alias — this is PLAYER_CHAR_ID, not game date

    // Decision database singleton
    constexpr uintptr_t DECISION_DB          = 0x0bf087f8;  // decision type hash table at +0x1b0

    // Max game speed global
    constexpr uintptr_t MAX_GAME_SPEED       = 0x0c2a821c;  // CONFIRMED: int32

    // Event manager singleton (also used for pause_on functionality)
    constexpr uintptr_t EVENT_MANAGER        = 0x0bf542c0;  // MEDIUM: CEventManager singleton ptr
    constexpr uintptr_t EVENT_QUEUE_MUTEX    = 0x0c28e628;  // MEDIUM: event queue mutex/lock

    // Succession
    constexpr uintptr_t MAX_HEIRS            = 0x0c2a86c8;  // MEDIUM: max succession heirs displayed

    // Game speed/pause
    constexpr uintptr_t FAST_TICK_TOGGLE     = 0x0becd5f8;  // byte: fast/slow tick toggle
    constexpr uintptr_t SPEED_INCREMENT_FN   = 0x038847e0;  // INCREASE_GAME_SPEED handler (1515B)
    constexpr uintptr_t SPEED_DECREMENT_FN   = 0x03884fc0;  // DECREASE_GAME_SPEED handler (1508B)

    // Null/sentinel objects for hash table lookup misses
    constexpr uintptr_t NULL_INTERACTION     = 0x0beda700;  // HIGH: null interaction sentinel
    constexpr uintptr_t NULL_DECISION        = 0x0bf1ba28;  // HIGH: null decision sentinel

    // House/Dynasty database singletons (Round 5)
    constexpr uintptr_t HOUSE_DB             = 0x0bed26c0;  // HIGH: house DB singleton
    constexpr uintptr_t HOUSE_DB_SENTINEL    = 0x0bed26c8;  // HIGH: null house sentinel
    constexpr uintptr_t DYNASTY_DB           = 0x0bed7788;  // HIGH: dynasty DB singleton
    constexpr uintptr_t DYNASTY_DB_SENTINEL  = 0x0bed7790;  // HIGH: null dynasty sentinel

    // Dynasty legacy perk cost globals (from FUN_05eaab70 decompilation)
    constexpr uintptr_t DYNASTY_PERK_COST_PER   = 0x0c2ac5f8;  // MEDIUM-HIGH: int, renown cost multiplier per unlocked perk
    constexpr uintptr_t DYNASTY_PERK_COST_BASE  = 0x0c2ac5e0;  // MEDIUM-HIGH: int, base renown cost for first perk

    // Null name sentinel (Round 5)
    constexpr uintptr_t NULL_NAME_SENTINEL   = 0x0c3d6870;  // HIGH: fallback name for null core_data

    // Landed title database (Round 3: +0x20=array, +0x2c=count)
    constexpr uintptr_t LANDED_TITLE_DB      = 0x0bed2700;
    constexpr uintptr_t TITLE_DB             = LANDED_TITLE_DB;  // ALIAS: same as LANDED_TITLE_DB
    constexpr uintptr_t TITLE_DB_NULL        = 0x0bed2708;  // HIGH: null title sentinel

    // Opinion clamp globals (Round 4)
    constexpr uintptr_t OPINION_MAX_CAP      = 0x0c2a9b80;  // HIGH: int32, opinion max (+100)
    constexpr uintptr_t OPINION_MIN_FLOOR    = 0x0c2a9b98;  // HIGH: int32, opinion min (-100)

    // Faction system (Round 3, R17 confirmed)
    constexpr uintptr_t FACTION_DB           = 0x0bf087c8;  // TPdxRefDatabase<CFaction, 8>  (DAT_0c0087c8 - 0x100000)
    constexpr uintptr_t FACTION_TYPE_DB      = 0x0bee39f8;  // DEPRECATED: mislabeled in R3; this IS CCasusBelliTypeDatabase (= CASUS_BELLI_DB). Use FACTION_TYPE_DB_V2 for factions.
    constexpr uintptr_t FACTION_DISCONTENT_THRESHOLD = 0x0c2ac9d8; // R17: int64 fp/100k, demand threshold (DAT_0c3ac9d8 - 0x100000)

    // Casus belli database (Round 5, R11-25 confirmed: CGameDatabase<CCasusBelliTypeDatabase, CCasusBelliType>)
    constexpr uintptr_t CASUS_BELLI_DB       = 0x0bee39f8;  // HIGH: CCasusBelliTypeDatabase singleton (+0x88=array, +0x94=count)

    // Casus belli GROUP database (ck-e58l.3: CCasusBelliGroupDatabase, distinct from CASUS_BELLI_DB)
    // Layout: +0x88=group array, +0x94=group count. 19 groups in save.
    // Each group: +0x30 CBType** array, +0x3C count, +0x48 is_valid trigger (inline),
    //             +0x208 disabled flag, +0x209 ai_allowed flag.
    // Iterated by has_any_display_cb_on (FUN_066d6b20). CB's +0x30 field points back to its group.
    constexpr uintptr_t CB_GROUP_DB          = 0x0c09b070;  // HIGH: CCasusBelliGroupDatabase singleton (core-verified, 19 groups, ck-e58l.3)

    // Council position database (Round 5)
    constexpr uintptr_t COUNCIL_POSITION_DB       = 0x0bef9320;  // HIGH: council position DB (also used as POSITION_ASSIGN_DB, R10-1)
    constexpr uintptr_t COUNCIL_POSITION_SENTINEL = 0x0bef9328;  // HIGH: council position null sentinel

    // Council TASK-TYPE database — RESOLVED & RUNTIME-VALIDATED 20260612.
    // Singleton ptr; *(this) = instance. Instance is a flat string-hash map:
    //   table @ inst+0x1b0, mask(u32) @ inst+0x1bc, bucket stride 0x18,
    //   bucket: ctrl byte @ +4 (0xff=empty), name-hash(u32) @ +8, value ptr @ +0x10.
    //   value = CCouncilTaskType*, its key std::string @ +0x10 (e.g. "task_fabricate_claim").
    // Resolve by LINEAR NAME SCAN over buckets (pure reads — no hash call needed).
    // Live: 72 task types (task_increase_control, task_fabricate_claim, ...).
    constexpr uintptr_t COUNCIL_TASK_TYPE_DB      = 0x0bed76d8;  // HIGH: CCouncilTaskTypeDatabase singleton ptr
    constexpr uintptr_t NO_TARGET_SCOPE           = 0x0befa4a0;  // HIGH: DAT_0bffa4a0 — global "no target" scope singleton (BUILD_TARGET_SCOPE default)

    // Council position type database (R10-1: CCouncilPositionType hash DB)
    constexpr uintptr_t POSITION_TYPE_DB          = 0x0bf32ea0;  // HIGH: CCouncilPositionType hash DB (R10-1)

    // Council INSTANCE walk — HIGH, runtime-validated 20260612 (audit B1).
    // The reliable per-ruler council walk (mirrors do_set_council_task, which
    // resolves positions correctly where the old query_council DB-scan failed):
    //   council        = *(char + 0x1b8)
    //   pos id array   = *(council + 0x230)      // u32[] position ids
    //   pos id count   = *(council + 0x23c)      // u32
    //   posdb          = *(g_base + COUNCIL_POSITION_DB); pdb_arr = *(posdb + 0x20)
    //   pos_obj        = *(pdb_arr + 8 + (pid & 0xffffff) * 0x10)
    // Position-object layout (CCouncilPosition):
    //   +0x10 key (pid; lo 24 bits = bucket idx)   +0x18 current task obj
    //   +0x30 holder char id (u32; 0xFFFFFF = vacant)
    // Current task name: std::string @ (current_task_obj + 0x10) = CTT_NAME.
    constexpr uintptr_t CHAR_COUNCIL_INSTANCE     = 0x1b8;   // char -> council instance ptr
    constexpr uintptr_t COUNCIL_POS_ID_ARRAY      = 0x230;   // council -> u32[] position ids
    constexpr uintptr_t COUNCIL_POS_ID_COUNT      = 0x23c;   // council -> u32 count
    constexpr uintptr_t COUNCIL_POS_KEY           = 0x10;    // pos_obj -> u32 key (pid)
    constexpr uintptr_t COUNCIL_POS_CURRENT_TASK  = 0x18;    // pos_obj -> current task obj
    constexpr uintptr_t COUNCIL_POS_HOLDER_ID     = 0x30;    // pos_obj -> u32 DISPLAY holder char id
                                                             //   UNRELIABLE: 0xFFFFFF even for FILLED seats
                                                             //   (always blank for chaplain/consort; for some
                                                             //   AI realms blank on EVERY seat). NOT authoritative.
    constexpr uintptr_t COUNCIL_POS_COUNCILLOR_ID = 0x38;    // pos_obj -> u32 AUTHORITATIVE seated councillor id
                                                             //   (= do_set_council_task ctx_a; +0x3c = ruler id).
                                                             //   HIGH (live-probed 20260619, 8 rulers/45 seats):
                                                             //   real seated councillor; query_council uses this as
                                                             //   the holder, falling back from +0x30. 0xFFFFFF=vacant.
    constexpr uintptr_t COUNCIL_POS_CTX_RULER     = 0x3c;    // pos_obj -> u32 ruler id (= ctx_b). HIGH (live-probed).
    constexpr uintptr_t COUNCIL_POS_TASK_TARGET_TAG = 0x40;  // pos_obj -> u32 task target-scope tag; 8 = title target,
                                                             //   0 = taskless. HIGH (core-dump ck3_20260421_195552,
                                                             //   8 seats; FUN_05c84350 scope-prep writes tag@scope+8).
    constexpr uintptr_t COUNCIL_POS_TASK_TARGET_ID  = 0x48;  // pos_obj -> u32 target title id when TAG==8; resolve via
                                                             //   lookup_title_obj. HIGH (core-dump verified:
                                                             //   6694->c_irbil, 2167->c_cosenza).

    // Scheme system sentinels (Round 5)
    constexpr uintptr_t NULL_SCHEME_TYPE     = 0x0bedaf70;  // HIGH: null scheme type sentinel (= SCHEME_TYPE_DB_SENTINEL)
    constexpr uintptr_t SCHEME_TYPE_DB_REAL  = 0x0bedaf68;  // HIGH: actual CSchemeTypeDatabase ptr (Jomini pattern: DB at N, sentinel at N+8)
    constexpr uintptr_t SCHEME_EVENT_DISPATCH = 0x0becd550; // HIGH: scheme event dispatch table

    // War/truce null sentinel (Round 5)
    constexpr uintptr_t NULL_WAR_TRUCE_SENTINEL = 0x0c3d6bf0; // HIGH: null war/truce sentinel

    // Opinion UI accessor (registered in _INIT_1043)
    constexpr uintptr_t OPINION_UI_ACCESSOR  = 0x017b85e0;  // "GetOpinionOf" handler

    // Command dispatch manager (Round 6)
    constexpr uintptr_t COMMAND_DISPATCH_MGR = 0x0c59dd68;  // HIGH: command dispatch singleton (R26: DAT_0c69dd68 - 0x100000)
    constexpr uintptr_t COMMAND_DISPATCH_GATE = 0x0c59dfb8; // Dispatch gate flags (DAT_0c69dfb8 - 0x100000). Check (gate & 0xfd)==0 before dispatch.

    // Culture/faith indexed DBs (Round 6)
    constexpr uintptr_t CULTURE_INDEXED_DB   = 0x0beda7a8;  // HIGH: culture ID -> obj indexed DB
    constexpr uintptr_t CULTURE_NULL_SENTINEL = 0x0beda7b0;  // HIGH: null culture sentinel
    constexpr uintptr_t FAITH_INDEXED_DB     = 0x0bedadd8;  // HIGH: faith/religion indexed DB

    // Law system (Round 6, corrected by law-change investigation)
    constexpr uintptr_t LAW_DB_NULL          = 0x0bedad58;  // HIGH: null law sentinel
    // LAW_DB_ENTRY_STRIDE (0x2D8) DEPRECATED — DB uses CLaw** pointer array, not inline.
    // Actual CLaw object size = 0x21F8. See realm_law_entry::OBJECT_SIZE.
    // LAW_ENTRY_NAME (0x28) DEPRECATED — use realm_law_entry::LAW_NAME (0x10) on dereferenced CLaw*.

    // MaA type database (Round 6)
    constexpr uintptr_t MAA_TYPE_DB          = 0x0bedaf98;  // HIGH: MaA type DB (same layout as BUILDING_TYPE_DB)
    constexpr uintptr_t MAA_TYPE_NULL        = 0x0bedafa0;  // HIGH: null MaA type sentinel
    // MAA_TYPE_DB layout (core-dump verified, 2026 dump 0xbedaf98, 109 types):
    //   db = read_ptr(resolve(MAA_TYPE_DB)); arr = read_ptr(db+0x88); count = read_u32(db+0x90);
    //   for i in 0..count: type_obj = read_ptr(arr + i*8); name = std::string @ type_obj+0x10;
    //   type DB id = read_u32(type_obj+0x8)  (sequential 0..count-1; this is what the recruit ctor
    //   reads via *(maa_type_obj+8)). Resolve-by-name mirrors action_handlers::resolve_building_obj.
    constexpr size_t    MAA_TYPE_DB_ARR_OFF  = 0x88;  // HIGH: ptr to type-obj array
    constexpr size_t    MAA_TYPE_DB_CNT_OFF  = 0x90;  // HIGH: u32 type count

    // Secret type database (Round 6)
    constexpr uintptr_t SECRET_TYPE_DB       = 0x0bee4e50;  // HIGH: secret type hash-table DB
    constexpr uintptr_t SECRET_TYPE_NULL     = 0x0bf1cbe0;  // HIGH: null secret type sentinel

    // Army system databases (Round 6b)
    constexpr uintptr_t ARMY_DB              = 0x0bed7670;  // HIGH: CArmy database (same lookup pattern as char DB)
    constexpr uintptr_t ARMY_NULL            = 0x0bed7678;  // HIGH: null army sentinel
    constexpr uintptr_t OWNER_LINK_DB        = 0x0bed76c8;  // HIGH: CArmyOwnerLink database (army→owner link)
    constexpr uintptr_t OWNER_LINK_NULL      = 0x0bed76d0;  // HIGH: null owner link sentinel
    constexpr uintptr_t ARMY_THIRD_DB        = 0x0bed7778;  // HIGH: third army DB (disband validation)
    constexpr uintptr_t ARMY_THIRD_NULL      = 0x0bed7780;  // HIGH: null sentinel for above
    constexpr uintptr_t ARMY_ENUM_ENTRY      = 0x0c2b40f0;  // HIGH: army enumeration entry point (= GAME_STATE_ROOT)

    // Vassal contract database (Round 7: track1 confirmed)
    constexpr uintptr_t VASSAL_CONTRACT_DB   = 0x0bed7700;  // HIGH: vassal contract DB singleton
    constexpr uintptr_t VASSAL_CONTRACT_NULL = 0x0bed7708;  // HIGH: null vassal contract sentinel

    // Council member database (Round 6b)
    constexpr uintptr_t COUNCIL_MEMBER_DB    = 0x0becef08;  // HIGH: council member database
    constexpr uintptr_t COUNCIL_MEMBER_NULL  = 0x0becef10;  // HIGH: null council member sentinel

    // Council type database (R7-2: DAT_0bfcef08 — hash table for council_type_id lookup)
    constexpr uintptr_t COUNCIL_TYPE_DB      = 0x0becef08;  // HIGH: council type DB (R7-2: DAT_0bfcef08 - 0x100000)

    // Localization system (Round 7: track2 confirmed)
    constexpr uintptr_t LOCALIZATION_DB      = 0x0c6f5948;  // HIGH: localization database singleton

    // Round 7: Game subsystem accessors (via *(DAT_0c3b40f0 + 0xa0) + offset)
    constexpr uintptr_t SCHEME_MANAGER_OFFSET = 0xa4d8;     // *(*(root+0xa0)) + 0xa4d8
    constexpr uintptr_t RELATION_MANAGER_OFFSET = 0xa370;   // *(*(root+0xa0)) + 0xa370

    // Round 7: Databases
    constexpr uintptr_t RELATION_TYPE_DB     = 0x0bed8878;
    constexpr uintptr_t RELATION_TYPE_NULL   = 0x0bed8880;
    constexpr uintptr_t LIFESTYLE_PERK_DB    = 0x0bee2900;
    constexpr uintptr_t LIFESTYLE_PERK_NULL  = 0x0bf1cb80;
    constexpr uintptr_t LIFESTYLE_DB         = 0x0bee2fc8;
    constexpr uintptr_t LIFESTYLE_NULL       = 0x0bf1cb70;
    constexpr uintptr_t SCHEME_NULL          = 0x0bf1cb20;  // HIGH: SLOT holding the null CActiveScheme* (DB/null pair with SCHEME_REFDB); CreateActiveScheme returns *(base+SCHEME_NULL), id 0xFFFFFF (lane-scheme live 2026-10-18; caller loads it at 0x051ba9ac)
    constexpr uintptr_t SCHEME_REFDB         = 0x0bf1cb18;  // HIGH: TPdxRefDatabase<CActiveScheme,8> (R7-3, DAT_0c01cb18 - 0x100000)
    constexpr uintptr_t SCHEME_TYPE_DB       = 0x0bedaf68;  // HIGH: CSchemeTypeDatabase (R8V5 fix: was 0x0bedaf70 = sentinel, real DB is at sentinel-8)
    // lane-scheme 2026-10-18 (re/findings/re_scheme_start_args.md): the two OPTIONAL
    // refs CreateActiveScheme takes as p5/p6. The ONLY engine caller (start_scheme
    // effect, call @ image 0x052baa73) builds them by RefDB lookup of an optional
    // script scope and falls back to the DB's null object when the scope is absent:
    //   p5 = FUN_05f75740(scope type 0x38) -> *(TASK_CONTRACT_DB) slot | *(TASK_CONTRACT_NULL)
    //   p6 = FUN_05f73f00(scope type 0x1f) -> *(ARTIFACT_DB) slot      | *(ARTIFACT_NULL)
    // RTTI of the null objects (core dump ivar867): TPdxNullObject<CTaskContract> and
    // TPdxNullObject<CArtifact>; null ids are -1 (contract +0x8, artifact +0x10), which
    // is what InitializeScheme copies to scheme+0x34 / +0x38. HIGH (decompile x2 +
    // RTTI + core dump + live murder readback, see the finding).
    constexpr uintptr_t TASK_CONTRACT_DB     = 0x0bef9358;  // HIGH: TPdxRefDatabase<CTaskContract> (FUN_05f75740)
    constexpr uintptr_t TASK_CONTRACT_NULL   = 0x0bef9360;  // HIGH: TPdxNullObject<CTaskContract>* (id @+0x8 == -1)
    constexpr uintptr_t ARTIFACT_DB          = 0x0bed8938;  // HIGH: TPdxRefDatabase<CArtifact,8> (FUN_05f73f00; re_siege_mechanics.md)
    constexpr uintptr_t ARTIFACT_NULL        = 0x0bed8940;  // HIGH: TPdxNullObject<CArtifact>* (id @+0x10 == -1)

    // War database (canonical war lookup)
    constexpr uintptr_t WAR_DB               = 0x0bed8888;
    constexpr uintptr_t WAR_DB_SENTINEL      = 0x0bed8890;

    // CCasusBelli / CCasusBelliType vtables (ck-s0qu: validated via 867 + 1097 dumps).
    // CCASUSBELLI is the embedded vtable at war+0x98; CCASUSBELLITYPE is the type
    // vtable shared by every entry of CASUS_BELLI_DB (pointed to by war+0xA0).
    constexpr uintptr_t CCASUSBELLI_VTABLE     = 0x0baa7158;  // HIGH: CCasusBelli embedded vtable (file-rel; Ghidra = 0x0bba7158)
    constexpr uintptr_t CCASUSBELLITYPE_VTABLE = 0x0bd78300;  // HIGH: CCasusBelliType vtable, all CB-DB entries share it (war+0xA0 deref target)

    // Game date globals for age computation (R8V-1: FUN_0674c0c0)
    // Age birth-date function reads from DAT_0c3a9770[DAT_0c3a977c] when char+0x6C >= 0
    constexpr uintptr_t GAME_DATE_ARRAY      = 0x0c2a9770;  // HIGH: game date array base (R8V-1)
    constexpr uintptr_t GAME_DATE_INDEX      = 0x0c2a977c;  // HIGH: int32, current index into date array (R8V-1)

    // Opinion modifier database (Round 9: COpinionModifierDatabase)
    constexpr uintptr_t OPINION_MODIFIER_DB  = 0x0c03f388;  // HIGH: COpinionModifierDatabase singleton

    // Focus/lifestyle database (Round 9: R9-7b)
    constexpr uintptr_t FOCUS_DB             = 0x0bedaf78;  // HIGH: CFocusDatabase singleton
    constexpr uintptr_t FOCUS_DB_SENTINEL    = 0x0bedaf80;  // HIGH: null focus sentinel

    // Innovation system (Round 10) — moved to globals near FAITH_DOCTRINE_DB

    // Regiment database (Round 10-6)
    constexpr uintptr_t REGIMENT_DB          = 0x0bed76e0;  // HIGH: TPdxRefDatabase<CRegiment, 8> — recruitment/MaA regiments (chunk-summed)
    constexpr uintptr_t REGIMENT_DB_SENTINEL = 0x0bed76e8;  // HIGH: null CRegiment sentinel

    // CArmyRegiment database (ck-warstrength) — the DEPLOYED sub-units listed in CArmy+0x18.
    // DISTINCT from REGIMENT_DB: that is CRegiment (a ruler's standing MaA), this is
    // CArmyRegiment (a regiment mobilized into a marching army). RTTI: TPdxRefDatabase<CArmyRegiment,8>.
    // Resolves Army+0x18 regiment IDs; the per-regiment current/max soldier ints live at +0x38/+0x3c.
    // HIGH: CoreReader ck3_20260421_195552 — resolved IDs across 407 armies; .bss DB global,
    // image VA 0xc0298e8 (header 0xbf298e8), neighbours g_CultureDB.
    constexpr uintptr_t ARMY_REGIMENT_DB     = 0x0bf298e8;  // HIGH: TPdxRefDatabase<CArmyRegiment, 8>

    // Diplomatic relation system (Round 10-3)
    constexpr uintptr_t RELATION_NULL_SENTINEL = 0x0c329ca0; // HIGH: null CDiplomaticRelation* (binary search miss)
    constexpr uintptr_t TRUCE_SENTINEL_DATE    = 0x0c333b18; // HIGH: sentinel date value ("no truce" / expired)
    constexpr uintptr_t TRUCE_LIST_FALLBACK    = 0x0c3d6bf0; // HIGH: fallback for truce list accessor (= NULL_WAR_TRUCE_SENTINEL)

    // Hook/secret system databases (Round 10-4)
    constexpr uintptr_t HOOK_TYPE_DB           = 0x0c0c7ff8; // HIGH: CHookTypeDatabase* singleton (common/hook_types)
    constexpr uintptr_t HOOK_TYPE_NULL         = 0x0c0c7ff0; // HIGH: SHookType* null/sentinel hook type
    constexpr uintptr_t SECRET_EXPOSER_SCOPE   = 0x0becd998; // MEDIUM: "secret_exposer" scope string singleton
    constexpr uintptr_t USE_HOOK_STRONG_STR    = 0x0c0fb0a8; // MEDIUM: "use_hook_strong" global string registration

    // Holy order / mercenary databases (R11-9: military 8-category breakdown)
    constexpr uintptr_t HOLY_ORDER_DB          = 0x0bf087e8;  // HIGH: TPdxRefDatabase holy order DB (R11-9)
    constexpr uintptr_t HOLY_ORDER_DB_NULL     = 0x0bf087f0;  // HIGH: null/sentinel holy order (R11-9)
    constexpr uintptr_t MERCENARY_DB           = 0x0bf087d8;  // HIGH: TPdxRefDatabase mercenary DB (R11-9)
    constexpr uintptr_t MERCENARY_DB_NULL      = 0x0bf087e0;  // HIGH: null/sentinel mercenary (R11-9)

    // ── LABEL CORRECTION (re_hire_mercenary_command_pinned.md, 2026-06-18) ──
    // The CHireMercenaryCompanyCommand Execute (0x610a460) AND CanExecute
    // (0x610a530) BOTH look up the company_id (cmd+0x24) in the DB at 0x0bf087e8
    // — and in live proc 858006 that DB holds 557 objects whose entry-vtable
    // RTTI typeinfo resolves to "17CMercenaryCompany". The DB at 0x0bf087d8 is
    // EMPTY. So the two R11-9 labels above are SWAPPED: the real mercenary
    // company DB (the one the engine hire path enumerates) is 0x0bf087e8, NOT
    // 0x0bf087d8. Use MERC_COMPANY_DB below for the hire/discover path.
    // Evidence: decompile (Execute+CanExecute RIP-rel) + RTTI entry-vtable name
    // + runtime DB walk (3 independent sources) → HIGH.
    constexpr uintptr_t MERC_COMPANY_DB        = 0x0bf087e8;  // HIGH: real CMercenaryCompany DB (hire path); id@entry+0x10, stride 0x70 def-nodes
    // (HOLY_ORDER_DB at the same 0x0bf087e8 is the stale/mis-swapped label —
    //  do not rely on it for holy-order enumeration until re-verified.)

    // ── CHireMercenaryCompanyCommand (re_hire_mercenary_command_pinned.md) ──
    // Dual-vtable game command (CGameCommand pattern), struct size 0x30:
    //   +0x00 primary vtable (offset_to_top=-24) | +0x08..0x17 CGameCommand base
    //   +0x18 CGameCommandHelper vtable          | +0x20 hirer char id (u32, &0xFFFFFF→CHARACTER_DB)
    //   +0x24 company id (u32, &0xFFFFFF→MERC_COMPANY_DB) | +0x28 contract field (u32)
    // All VAs verified by reading RESOLVED vtable slots from /proc/858006/mem
    // (RELRO) + capstone disasm of the function bodies. HIGH.
    constexpr uintptr_t HIRE_MERC_CMD_VTABLE_PRIMARY = 0x0bb5d7d8; // HIGH: cmd+0x00 primary vtable start (typeinfo 0x0bb5d9d0)
    constexpr uintptr_t HIRE_MERC_CMD_VTABLE_HELPER  = 0x0bb5d728; // HIGH: cmd+0x18 CGameCommandHelper<CHireMercenaryCompanyCommand> vtable start
    constexpr uintptr_t HIRE_MERC_CMD_CTOR     = 0x0600a410; // HIGH: ctor(this, u32 hirer_char_id, u32 company_id, u32 contract_len)
    constexpr uintptr_t HIRE_MERC_CMD_EXECUTE  = 0x0600a460; // HIGH: Execute body (primary vtable; helper-thunk at 0x0600a520 adjusts -0x18 then jmps here)
    constexpr uintptr_t HIRE_MERC_CMD_CANEXEC  = 0x0600a530; // HIGH: per-type CanExecute/Validate (helper vtable[7]); validates both DB lookups + calls hireable-precheck 0x0664f390 + contract validate 0x06177300
    constexpr uintptr_t HIRE_MERC_CMD_CLONE_DEFAULT = 0x0600b720; // MEDIUM: default-construct (alloc 0x30, fields=-1/contract=1)
    constexpr uintptr_t HIRE_MERC_COST_COMPUTE = 0x06178f60; // MEDIUM: cost/RNG-mix from (merc_company,char), scale 0x186a0=100000 (the hire price)

    // Military quality modifier globals (R11-9)
    constexpr uintptr_t MIL_QUALITY_MOD_A      = 0x0c2aa3a0;  // HIGH: global combat quality modifier A (R11-9)
    constexpr uintptr_t MIL_QUALITY_MOD_B      = 0x0c2aa388;  // HIGH: global combat quality modifier B (R11-9)
    constexpr uintptr_t MIL_ALLY_QUALITY_A     = 0x0c2aa598;  // HIGH: ally quality modifier compound A (R11-9)
    constexpr uintptr_t MIL_ALLY_QUALITY_B     = 0x0c2aa580;  // HIGH: ally quality modifier compound B (R11-9)
    constexpr uintptr_t MIL_ALLY_QUALITY_C     = 0x0c2aa568;  // HIGH: ally quality modifier compound C (R11-9)
    constexpr uintptr_t MIL_SINGLETON          = 0x0bedaf98;  // HIGH: military singleton (garrison quality at +0xF58+0x270) (R11-9, = MAA_TYPE_DB)

    // AI Watch Window system (Round 12)
    constexpr uintptr_t AI_WATCH_DEBUG_FLAG    = 0x0bed75d4;  // HIGH: byte, 1=enabled 0=disabled (Ghidra DAT_0bfd75d4)
    // Note: SCHEME_TYPE_DB already defined above at 0x0bedaf68
    constexpr uintptr_t AIWATCH_VTABLE         = 0x0b758128;  // HIGH: CAIWatchWindow vtable (Ghidra 0x0b858128)

    // Allocator vtable globals used by CAIWatchWindow (Round 12, Ghidra PTR_vftable_*)
    constexpr uintptr_t ALLOC_VT_23E8          = 0x0bea23e8;  // Ghidra PTR_vftable_0bfa23e8
    constexpr uintptr_t ALLOC_VT_23F0          = 0x0bea23f0;  // Ghidra PTR_vftable_0bfa23f0
    constexpr uintptr_t ALLOC_VT_23F8          = 0x0bea23f8;  // Ghidra PTR_vftable_0bfa23f8
    constexpr uintptr_t ALLOC_VT_2400          = 0x0bea2400;  // Ghidra PTR_vftable_0bfa2400
    constexpr uintptr_t ALLOC_VT_2408          = 0x0bea2408;  // Ghidra PTR_vftable_0bfa2408
    constexpr uintptr_t ALLOC_VT_2410          = 0x0bea2410;  // Ghidra PTR_vftable_0bfa2410
    constexpr uintptr_t ALLOC_VT_2418          = 0x0bea2418;  // Ghidra PTR_vftable_0bfa2418
    constexpr uintptr_t ALLOC_VT_2420          = 0x0bea2420;  // Ghidra PTR_vftable_0bfa2420
}

// =============================================================================
// Game State Root Offsets (from *(GAME_STATE_ROOT))
// =============================================================================
namespace game_state {
    constexpr size_t GAME_DATE              = 0x08;   // CONFIRMED: int32, sub-day ticks (24 per day) since epoch
    // ── THE TWO DATE STRUCTS (ck-hwbb, 2026-09-13) ─────────────────────────
    // 0x0E and 0xCE were SWAPPED. Both sit in a {int32 raw; …; uint16 year}
    // struct and the two structs are 0xC0 apart. 0x0E was graded CONFIRMED as
    // START_DATE_YEAR; ck-hxh7's world-epoch fingerprint therefore read the
    // CURRENT clock as a campaign identity and declared a world reload on
    // every 1 January — quarantining the agents' personality locks mid-episode
    // and refusing session resume across a New Year.
    //
    // WHY A CONFIRMED ENTRY IS OVERWRITTEN HERE (CLAUDE.md forbids it on one
    // session's say-so): this is a DISPROOF from a re-readable artefact, not a
    // competing reading. In core dump ck3_20260421_195552.core (same BuildID
    // e61ff264), GAME_DATE raw 53418504 decodes to 1098.01.02 and gs+0x0E
    // reads 1098. CK3 ships no 1098 bookmark, so 0x0E cannot be a campaign
    // start year in any campaign. Corroborated live: one node boot running
    // 1066.09.15 → 1084.02.20 logged exactly 18 start_date_year_changed
    // reload lines — one per New Year crossed.
    constexpr size_t CURRENT_YEAR           = 0x0E;   // CONFIRMED: uint16, the YEAR of GAME_DATE — advances every 1 Jan, never a campaign start
    constexpr size_t GAME_SPEED             = 0x70;   // CONFIRMED: int32, 0-indexed
    constexpr size_t DAY_COUNTER            = 0x9C;   // MEDIUM: uint32, scheduling
    // ── THE CAMPAIGN-START PAIR (0xC8/0xCE) ────────────────────────────────
    // These two were labelled END_DATE_RAW / END_DATE_YEAR, both CONFIRMED.
    // The STRUCTURE was right and the LABEL was wrong: it is an int32 raw date
    // followed by its uint16 year — exactly the shape the retired pair
    // described — but it is the campaign START, not an end date. 0xCE is
    // simply the YEAR OF THE DATE AT 0xC8, not an independent field. The
    // END_DATE_* names are RETIRED; the GENUINE end-date fields (the game's
    // configured campaign end) remain UNIDENTIFIED — no offset in this header
    // points at them, and that is an honest gap.
    //
    // WHY THE WRONG LABEL SURVIVED, which is the generalisable part: nothing
    // in the tree consumed either name, so nothing ever contradicted them. A
    // WRONG LABEL ON AN UNCONSUMED PAIR IS UNFALSIFIABLE BY USE — it can only
    // be falsified by reading the field, which nobody had done.
    //
    // EVIDENCE (offline, archived core dumps, no game running). Decoded with
    // this header's own ck-05mg calendar below — DATE_EPOCH, 24 ticks/day, a
    // fixed 365-day year and the REAL month table, never twelve 30-day months:
    //   ck3_20260421_195552.core  gs+0x08 = 53418504 -> 1098.01.02  (current)
    //                             gs+0xC8 = 53144328 -> 1066.09.15  (start)
    //   william_1066_baseline.core gs+0xC8 = 53144328 -> 1066.09.15 — the SAME
    //     raw value 32 game-years earlier in the same campaign: it does not
    //     move with the clock. 1066.09.15 is the Fate of England bookmark
    //     start, to the day.
    //   GROUND-TRUTH CONTROL for the conversion itself: william_1066_baseline
    //     gs+0x08 = 53144904 -> 1066.10.09, which is the meta_date CK3 wrote
    //     into the save at that instant. The decoder is checked against a date
    //     the ENGINE produced, not against our own arithmetic.
    //
    // THE DISCRIMINATOR, one dump, both readings side by side — a single
    // reading proves nothing and the pairing is the whole argument:
    //   ck3_20260421_195552.core   gs+0x0E = 1098   gs+0xCE = 1066
    // Same save, same instant, 32 game-years apart. That is what separates a
    // start-year field from a current-year field WITHIN one save, and is why
    // 0x0E's half of this change carries no caveat. A scan of gs+0x000..0x400
    // in that dump for any bookmark year (867 / 1066 / 1178) returns exactly
    // one hit: +0xCE = 1066.
    //
    // WHY HIGH AND NOT CONFIRMED FOR THE PAIR: both readable dumps are
    // 1066-START campaigns, so this is "matches the 1066 bookmark exactly on
    // every dump we can read", not "proven to track the start across
    // bookmarks". THE TEST NOT YET RUN: read this pair in an 867 or 1178
    // campaign. ONE such campaign SETTLES BOTH FIELDS OR FALSIFIES BOTH — it
    // is a single test for the pair, not two tests. The existing 867 dump
    // (ck3_20260625_152901_ivar867) CANNOT settle it: GAME_STATE_ROOT is
    // unmapped in that core.
    constexpr size_t START_DATE_RAW         = 0xC8;   // HIGH: int32, raw ticks — the campaign's START date (1066.09.15 on both 1066 dumps)
    constexpr size_t START_DATE_YEAR        = 0xCE;   // HIGH: uint16 — the YEAR of START_DATE_RAW, not an independent field
    constexpr size_t DATE_PAUSE_CAP         = 0xE0;   // CONFIRMED: int32, date-pause list capacity
    constexpr size_t DATE_PAUSE_COUNT       = 0xE4;   // CONFIRMED: int32, date-pause list count
    constexpr size_t EVENT_PAUSE_CAP        = 0xF8;   // CONFIRMED: int32, event-pause list capacity
    constexpr size_t EVENT_PAUSE_COUNT      = 0xFC;   // CONFIRMED: int32, event-pause list count

    // Date conversion: (game_date - DATE_EPOCH) / 24 = days since year 0, Jan 1
    // Raw unit is 1/24th of a day (confirmed via Ghidra: / 0x18 and / 6 >> 2 patterns)
    // Epoch = 5000 * 365 * 24 = 43,800,000
    constexpr int32_t DATE_EPOCH            = 43800000;

    // ── THE CALENDAR (ck-05mg, 2026-09-12) ─────────────────────────────────
    // CK3 uses a FIXED 365-day year with REAL month lengths and NO leap years:
    //     31 28 31 30 31 30 31 31 30 31 30 31   (= 365)
    // Rendering with twelve 30-day months (360) was wrong by up to 4 days and
    // produced an impossible MONTH 13 for days-of-year 360..364. HIGH — four
    // independent sources, see re/findings/re_game_calendar.md:
    //   (1) GROUND TRUTH. william_1066_baseline.core raw 53144904 ->
    //       total_days 389371 -> 1066.10.09 == the meta_date of the save CK3
    //       wrote at that instant (Duke_William_II_of_Normandy_1066_10_09.ck3).
    //   (2) the engine's own constant tables, ANCHORS below.
    //   (3) no leap table ({31,29,...} or {0,31,60,91,...}) exists anywhere in
    //       the binary — there is no Feb-29 path.
    //   (4) DATE_EPOCH is exactly 5000*365*24, and every date literal in
    //       game/history/ respects the table above.
    // The bridge renders ONLY via bridge/include/game_date.h (ck3::game_date).
    constexpr int32_t DAYS_PER_YEAR         = 365;   // HIGH: never 366
    constexpr int32_t TICKS_PER_DAY         = 24;    // HIGH: sub-day tick granularity
    // Days elapsed before the 1st of month N (1-based N indexes [N-1]); [12]=365.
    inline constexpr int32_t MONTH_CUM_DAYS[13] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365
    };

    // Anchors: the engine's own month tables in .rodata (base-relative; Ghidra
    // image VA = these + 0x100000). Byte-verified in the CK3_new project via
    // the pyghidra read server. Note: Ghidra records NO xrefs to any of them
    // (RIP-relative loads into undisassembled data) — the decode function that
    // consumes them is still unidentified, which is why confidence rests on
    // the ground-truth pair (1) and not on a decompile.
    constexpr uintptr_t MONTH_LENGTHS_U8_A   = 0x084dd26a;  // HIGH: u8[12] 1f 1c 1f 1e 1f 1e 1f 1f 1e 1f 1e 1f
    constexpr uintptr_t MONTH_LENGTHS_U8_B   = 0x08d1cb3f;  // HIGH: u8[12], second copy of the same table
    constexpr uintptr_t MONTH_CUM_DAYS_I32   = 0x08e93be0;  // HIGH: i32[12] 0,31,59,...,334 == MONTH_CUM_DAYS

    // Active-wars array fields written by WAR_CREATE_CORE Block K (ck-e58l.5
    // decomp). The decomp reads/writes these at offsets 0x28/0x30/0x34/0x38 of
    // its local `lStack_1208 = param_1`. ck-e58l.5 inferred param_1 ==
    // *(GAME_STATE_ROOT), but ck-ltsh (2026-04-21) DISPROVED that inference:
    // on both the 867 AD core dump and the live 1097 AD session, the field at
    // *(GAME_STATE_ROOT)+0x20 is a small int (0x24) not a pointer, while the
    // decomp reads *(param_1+0x20) as a pointer-arg to WAR_DB_ALLOCATE. So
    // param_1 is some other struct (likely a CWarManager) and the offsets
    // below are correct relative to THAT struct, not to *(GAME_STATE_ROOT).
    // Reading them from *(GAME_STATE_ROOT) yields garbage: ptr=0xffffffff,
    // cap=0, count=32575. Demoted to LOW until the true parent is located.
    //
    // Consumers (phase3_probes::read_active_wars_array) must either find the
    // true parent struct or treat these as unresolvable. Orphan detection via
    // probe_war_orphans is disabled until then. See bead ck-ltsh.
    constexpr size_t ACTIVE_WARS_PTR        = 0x28;   // LOW: ptr to int32[] of war IDs — parent struct unknown
    constexpr size_t ACTIVE_WARS_CAPACITY   = 0x30;   // LOW: int32 — parent struct unknown
    constexpr size_t ACTIVE_WARS_COUNT      = 0x34;   // LOW: int32 — parent struct unknown
    constexpr size_t ACTIVE_WARS_ALLOCATOR  = 0x38;   // LOW: ptr (vtable[2]=alloc, [3]=free) — parent struct unknown

    // WAR_DB root: WAR_CREATE_CORE reads *(game_state+0x20)+8 as the argument to
    // WAR_DB_ALLOCATE (ck-e58l.5).
    constexpr size_t WAR_DB_ALLOC_ROOT      = 0x20;   // HIGH: ptr; *(gs+0x20)+8 → WAR_DB allocator arg
}

// =============================================================================
// CJominiIdler Struct Offsets (pause system, CONFIRMED)
// =============================================================================
namespace idler {
    constexpr size_t PAUSE_FLAG             = 0x20;   // CONFIRMED: byte, 0=unpaused 1=paused
    constexpr size_t PLAYER_ID              = 0x24;   // CONFIRMED: uint32
    constexpr size_t TIMESTAMP              = 0x28;   // CONFIRMED: int64
}

// =============================================================================
// Generic Hash-Map Header (CK3 shared container layout, ck-e58l.24 FIX-2)
// =============================================================================
// Every CK3 "DB" singleton (WAR_DB, CHARACTER_DB, SCHEME_REFDB, TITLE_DB, ...)
// points to an object whose first 0x40 bytes are a generic hash-table header.
// A canonical walk is:
//     table    = *(db + TABLE_PTR)        // hash-bucket array
//     capacity = *(db + CAPACITY)         // uint32 bucket count
//     for (idx = 0; idx < capacity; ++idx) {
//         entry    = table + ENTRY_HEADER_SIZE + idx * ENTRY_STRIDE;
//         obj_ptr  = *(entry);            // null if slot empty
//         key_ref  = *(entry + ENTRY_KEY_REF);   // uint32, low 24 bits = idx
//     }
// Moved into its own namespace so the 5+ bridge files that duplicate these
// magic numbers share a single source of truth.
namespace hash_map {
    constexpr size_t TABLE_PTR         = 0x20;  // HIGH: pointer to first bucket
    constexpr size_t CAPACITY          = 0x2C;  // HIGH: uint32 bucket count
    constexpr size_t ENTRY_HEADER_SIZE = 8;     // HIGH: bytes to skip at table start
    constexpr size_t ENTRY_STRIDE      = 0x10;  // HIGH: sizeof(bucket_entry)
    constexpr size_t ENTRY_KEY_REF     = 0x10;  // HIGH: uint32 key ref at entry+0x10
                                                //       (lo 24 bits = bucket idx)
}

// =============================================================================
// Key Function Addresses (absolute, need ASLR adjustment)
// =============================================================================
namespace funcs {
    // CAIManager vtable function pointer array (static, in .data.rel.ro)
    // Ghidra labels vtable metadata at 0x0b987068 (offset_to_top + typeinfo),
    // but the object vptr points to the first function slot at +0x10:
    // Ghidra VA 0x0b987078, adjusted -0x100000 = 0x0b887078
    // MainTick at slot [10], PostExecute at [12], Destructor at [0]
    constexpr uintptr_t CAIMAN_VTABLE              = 0x0b887078;  // CONFIRMED: CAIManager vptr target (first vfn slot)

    // CAIManager internal vtable function pointer array (secondary base at object+0x08)
    // Ghidra VA 0x0b9870f0 (first function slot, after offset_to_top and typeinfo at 0x0b9870e0)
    // Adjusted -0x100000 = 0x0b8870f0
    // MainTick thunk at slot [2] (raw index from function pointer start)
    constexpr uintptr_t CAIMAN_INTERNAL_VTABLE     = 0x0b8870f0;  // Internal vtable function pointer start

    // CAIManager vtable functions (slot indices verified from binary vtable scan)
    constexpr uintptr_t AI_MANAGER_MAIN_TICK       = 0x04ceb5e0;  // vtable[10], 3221B
    constexpr uintptr_t AI_MANAGER_MAIN_TICK_THUNK = 0x04cec870;  // internal vtable[2], 9B thunk: this-=8, tail-call MainTick
    constexpr uintptr_t AI_MANAGER_POST_EXECUTE    = 0x04cedfd0;  // vtable[12], 1972B
    constexpr uintptr_t AI_MANAGER_DESTRUCTOR      = 0x04ce72e0;  // vtable[0]

    // Per-character AI control
    constexpr uintptr_t AI_CHAR_TOGGLE             = 0x04d1f190;  // 5 bytes! *(p+0x28) = v
    constexpr uintptr_t AI_CHAR_EVAL               = 0x04d1f6b0;  // per-char tick category update
    // ck-2u2a: THIS COMMENT IS WRONG AND THE VALUE IS UNVERIFIED. 0x04d22cb0 is
    // is_entry=false — it lands inside FUN_04d22c80 (291 bytes) — and it is NOT
    // the ai_ctrl per-tick setup, which is FUN_04e22cb0 -> FUN_04e52690 (see the
    // ai_ctrl namespace). The VALUE is left alone deliberately: changing an offset
    // value needs its own bead and a control, and nothing here establishes what
    // this constant was meant to point at. Do not trust the label.
    constexpr uintptr_t AI_CHAR_EVAL_SECONDARY     = 0x04d22cb0;  // SEE ABOVE - label unverified
    constexpr uintptr_t AI_IS_ELIGIBLE             = 0x04d19160;  // (state>1)&&(flags&2)&&!suspended
    constexpr uintptr_t AI_IS_SUSPENDED            = 0x04d1f2e0;  // returns *(ai_ctrl+0x2A)
    constexpr uintptr_t AI_IS_ACTIVE               = 0x0574f860;  // char is landed + in active DB

    // AI dispatch functions (per-character subsystem dispatchers)
    // R14: 11 dispatchers total, we currently hook 2
    constexpr uintptr_t AI_MARRIAGES_DISPATCH      = 0x04c88fe0;  // R14: checks +0x02,+0x13,+0x00,+0x17,+0x18,+0x1D,+0x08,+0x20,+0x25
    constexpr uintptr_t AI_INTERACTIONS_DISPATCH    = 0x04c89360;  // R14: checks +0x12,+0x0F,+0x08,+0x0B,+0x0C,+0x14,+0x15,+0x16,+0x19,+0x1A,+0x1B,+0x1C,+0x10,+0x11,+0x1F,+0x23
    // THE +0x1A FIELD IS UNCHANGED AND WAS NEVER IN DOUBT: it comes from the R14
    // static field list above, which is the only evidence for it.
    //
    // A LIVE-FAULT "CORROBORATION" WAS CLAIMED HERE ON 2026-09-21 AND IS
    // WITHDRAWN (same day). The claim was that an arm-A SIGSEGV with
    // fault_addr=0x1a was this dispatch reading +0x1A off a null character
    // handed to it by a deferred acceptance query. It does not hold:
    //   - the fatal line lands BEFORE evaluate_acceptance_query logs its first
    //     line, and that ADLOG fflushes, so the absence is real, not buffering.
    //     The code I wrote a guard for never ran.
    //   - the target id in question was healthy: same path, different actor,
    //     clean REJECT, and it answered later queries.
    //   - a second arm-A crash (1067.10.12) had fault_addr=(nil) after a
    //     different call with no acceptance query near it, which excludes the
    //     acceptance path as the common factor from a second incident.
    //
    // WHY IT LOOKED LIKE EVIDENCE, since that is the part worth keeping: two
    // numbers agreed (a static +0x1A read, a 0x1a fault address) and were
    // produced months apart without each other in mind. Independence makes an
    // agreement worth INVESTIGATING; it does not make it a finding. 0x1a is a
    // small displacement that many structures read, and nobody counted how
    // often this dispatch runs WITHOUT faulting before believing the match.
    // The fault address alone could not name the instruction — which is the
    // gap ck-duv5 exists to close.
    //
    // See re/findings/re_ai_interactions_dispatch_null_target.md (retraction
    // stamped above the original).

    // R14: Additional dispatch functions (not yet hooked)
    constexpr uintptr_t AI_LIFESTYLE_DISPATCH      = 0x04c89110;  // checks +0x03,+0x04,+0x05,+0x06,+0x0E,+0x18,+0x24,+0x25
    constexpr uintptr_t AI_MARRIAGE_WAR_DISPATCH   = 0x04c89240;  // checks +0x09,+0x10,+0x12
    constexpr uintptr_t AI_WAR_COUNCIL_DISPATCH    = 0x04c892d0;  // checks +0x07,+0x0A,+0x0E,+0x21
    constexpr uintptr_t AI_CALL_ALLIES_DISPATCH    = 0x04c891e0;  // checks +0x0D
    constexpr uintptr_t AI_COURT_ACTIVITY_DISPATCH = 0x04c88eb0;  // checks +0x19,+0x1E,+0x22
    constexpr uintptr_t AI_GRANT_REVOKE_1          = 0x04c89530;  // checks +0x01
    constexpr uintptr_t AI_GRANT_REVOKE_2          = 0x04c89580;  // checks +0x01
    constexpr uintptr_t AI_TAX_DISPATCH            = 0x04c895d0;  // checks +0x22

    // AI personality getter functions (each ~26 bytes, takes CCharacter*, returns int)
    // Verified via binary byte dump: file offset = ELF VA (code segment offset==VA)
    // Pattern: mov rax,[rdi+0x1a8]; test rax,rax; je; mov rax,[rax+0x308]; movsx eax,byte [rax+N]; ret
    // 9 axes only (axis 9 at +0x20*9 is a different function, not a getter)
    constexpr uintptr_t AI_PERSONALITY_HONOR       = 0x0664dca0;  // axis 0, byte [rax+0]
    constexpr uintptr_t AI_PERSONALITY_GREED       = 0x0664dcc0;  // axis 1, byte [rax+1]
    constexpr uintptr_t AI_PERSONALITY_RATIONALITY = 0x0664dce0;  // axis 2, byte [rax+2]
    constexpr uintptr_t AI_PERSONALITY_ENERGY      = 0x0664dd00;  // axis 3, byte [rax+3]
    constexpr uintptr_t AI_PERSONALITY_BOLDNESS    = 0x0664dd20;  // axis 4, byte [rax+4]
    constexpr uintptr_t AI_PERSONALITY_ZEAL        = 0x0664dd40;  // axis 5, byte [rax+5]
    constexpr uintptr_t AI_PERSONALITY_VENGEFULNESS= 0x0664dd60;  // axis 6, byte [rax+6]
    constexpr uintptr_t AI_PERSONALITY_COMPASSION  = 0x0664dd80;  // axis 7, byte [rax+7]
    constexpr uintptr_t AI_PERSONALITY_SOCIABILITY = 0x0664dda0;  // axis 8, byte [rax+8]

    // War score update — called per war during war update cycle.
    // void FUN_066b6020(void* war_ptr, ...) — hooked to cache computed scores.
    constexpr uintptr_t WAR_SCORE_UPDATE           = 0x065b6020;  // FUN_066b6020 - 0x100000 image base

    // War score computation — int FUN_066ba0d0(void* war_ptr, void* tooltip_ctx) -> int in [-100,+100]
    // NOT safe to call from arbitrary context, but safe from within the war update hook
    // (called immediately after the original WAR_SCORE_UPDATE returns).
    constexpr uintptr_t WAR_COMPUTE_SCORE          = 0x065ba0d0;  // FUN_066ba0d0 - 0x100000 image base

    // War score sub-component functions (called by WAR_COMPUTE_SCORE).
    // TWO different calling conventions — see re/findings/re_war_score_subfunctions.md:
    //
    // Per-side (3-param): int fn(void* war_ptr, int side_index, void* tooltip_ctx)
    //   side: 0=attacker, 1=defender.  tooltip: nullptr for no tooltip.
    constexpr uintptr_t WAR_SCORE_IMPRISONMENT     = 0x065d8530;  // HIGH: FUN_066d8530 - 3-param per-side
    constexpr uintptr_t WAR_SCORE_OCCUPATION       = 0x065d95a0;  // HIGH: FUN_066d95a0 - 3-param per-side
    //
    // Net-score (2-param): int fn(void* war_ptr, void* tooltip_ctx)
    //   No side param — computes both sides internally, returns net score.
    //   WARNING: Do NOT pass side_index — misinterpreted as tooltip ptr, SIGSEGV on side=1.
    constexpr uintptr_t WAR_SCORE_BATTLE           = 0x065d8ca0;  // HIGH: FUN_066d8ca0 - 2-param net score
    constexpr uintptr_t WAR_SCORE_TICKING          = 0x065da7b0;  // HIGH: FUN_066da7b0 - 2-param net score
    constexpr uintptr_t WAR_SCORE_LANDLESS_THUNK   = 0x065d9440;  // HIGH: FUN_066d9440 - 2-param net (attacker only)
    constexpr uintptr_t WAR_SCORE_LANDLESS_IMPL    = 0x065d9080;  // HIGH: FUN_066d9080 - impl called by thunk

    // Core decision functions
    constexpr uintptr_t AI_INTERACTION_DECISION    = 0x04cc4b90;  // char, interaction, rng (R23 confirmed: FUN_04dc4b90 - 0x100000 image base)
    constexpr uintptr_t AI_INTERACTION_CORE        = 0x04cc4540;  // FUN_04dc4540 - 0x100000: inner loop of AI_INTERACTION_DECISION, calls GetAiWillDoScore
    constexpr uintptr_t AI_MARRIAGE_EVAL           = 0x04cc3a50;  // FUN_04dc3a50 - 0x100000: evaluates candidates for interaction array — HIGH
    constexpr uintptr_t AI_ADDITIONAL_VALIDATION   = 0x04cc38f0;  // FUN_04dc38f0 - 0x100000: AI-specific post-validation checks — MEDIUM
    constexpr uintptr_t AI_WAR_TARGET_EVAL         = 0x04d22820;
    constexpr uintptr_t AI_WAR_DECLARE_EXEC        = 0x04d50660;
    constexpr uintptr_t AI_WAR_POST_DECL           = 0x04d55c10;  // 3095B
    constexpr uintptr_t AI_WAR_ELIGIBILITY_CHECK   = 0x04d19160;  // HIGH: rank>1 + AI-controlled gate
    constexpr uintptr_t AI_CB_SELECTOR             = 0x04d980d0;  // HIGH: picks best CB via weighted random (0x908-byte entries, score at +0xB0)
    constexpr uintptr_t AI_CB_EVALUATE_ALL         = 0x04d99190;  // HIGH: scores all valid CBs for target — called by AI_CB_SELECTOR

    // AI_CB_SELECTOR result-struct offsets (stack-allocated by caller; ~0xC0 bytes).
    // These are the fields the S1 compound-if in AI_WAR_POST_DECL (ck-a7gr) reads
    // to decide whether to dispatch WAR_CREATE_CORE. When AI_CB_EVALUATE_ALL
    // returns 0 viable pairs, AI_CB_SELECTOR zeroes offsets 0x00/0x08/0x10 and
    // sets +0x18 = 0xFFFFFFFF; otherwise it copies the weighted-random winner.
    // See re/findings/re_ai_war_post_decl_early_exits.md.
    //
    // ⚠️  AI_CB_SELECTOR is NOT side-effect-free for replay. The ck-a7gr claim
    // that it "writes only to its output buffer" was refuted on ck-10f5: a
    // replay inside execute_war_intent (b32de6c) reliably crashed CK3 via heap
    // corruption on the first call. Read these offsets only from a FIRST-CALL
    // capture (entry/exit hook), never from a synthetic second invocation. See
    // re/findings/re_ai_cb_selector_probe.md post-mortem and ck-2x2z.
    constexpr uint64_t CB_RESULT_CB_REF       = 0x00;  // HIGH: CB type/data ref (0 if no CB) — ck-a7gr
    constexpr uint64_t CB_RESULT_CB_TITLES    = 0x08;  // HIGH: misnamed — gate-probes a CHARACTER-like obj (vtable[2]); NOT a titles list. Target CCharacter* satisfies it. — ck-tcxr/ck-6h1v
    constexpr uint64_t CB_RESULT_TARGET_REF   = 0x10;  // HIGH: target character ref — ck-a7gr
    constexpr uint64_t CB_RESULT_SENTINEL_U32 = 0x18;  // HIGH: MISNAMED — this is the CLAIMANT char handle (u32; 0xFFFFFFFF=none), NOT a sentinel. WarCmd_SetTitleList copies it to war-goal+0x28 → WAR_CREATE_CORE claimant arg. NOT read by the G1 gate. — re_claimant_is_cb_result_0x18_20260611
    constexpr uint64_t CB_RESULT_SCORE        = 0xb0;  // HIGH: int32 score (score > 0 required to dispatch) — ck-a7gr
    constexpr uint64_t CB_RESULT_EXTRA_ROLL   = 0xb4;  // HIGH: u8 "requires additional roll" flag — ck-a7gr

    // CB result struct sub-CDynArray header at +0x20 (titles list payload).
    // AI_WAR_POST_DECL → WarCmd_SetTitleList (0x066dbcc0) reads data ptr at +0x20
    // and count at +0x2c, copies into cmd+0x10 — this is the actual title-list
    // material that flows into the dispatched war command. Path B' override
    // (ck-8weg) does NOT touch these fields; engine NO_VIABLE_PAIR init zeroes
    // them at LAB_04e98358 (XORPS [R13+0x20]). See re_ai_cb_selector_consumer.md.
    constexpr uint64_t CB_RESULT_TITLES_DATA  = 0x20;  // HIGH: u32* — title-handle array data ptr (ck-6h1v)
    constexpr uint64_t CB_RESULT_TITLES_CAP   = 0x28;  // HIGH: int32 capacity (ck-6h1v)
    constexpr uint64_t CB_RESULT_TITLES_COUNT = 0x2c;  // HIGH: int32 count — WarCmd_SetTitleList reads param_2[5] (ck-6h1v)
    // Allocator-holder for the +0x20 titles CDynArray. AI_WAR_POST_DECL's epilog
    // (raw 0x4d567f1) frees the +0x20 data via (*(result+0x30))->vtable[0x18]
    // (holder, data, 4) — but ONLY when +0x20 data != null. A Path B' override
    // that writes a non-null +0x20 data MUST also point +0x30 at an allocator
    // whose vtable[0x18] free is safe for the override's buffer (the bridge uses
    // a fake no-op-free allocator). See re_path_b_prime_titles_array_20260610.md.
    constexpr uint64_t CB_RESULT_TITLES_ALLOC = 0x30;  // HIGH: allocator-holder ptr (epilog free site)

    // War cmd builders (called by AI_WAR_POST_DECL inside the S1 dispatch block)
    constexpr uintptr_t WAR_CMD_SET_CB             = 0x066dbcb0;  // HIGH: WarCmd_SetCB(cmd, cb_ref) → *(cmd+8) = cb_ref (ck-6h1v)
    constexpr uintptr_t WAR_CMD_SET_TITLE_LIST     = 0x066dbcc0;  // HIGH: WarCmd_SetTitleList(cmd, &titles_arr_hdr) — copies CDynArray(uint32) into cmd+0x10 (ck-6h1v)

    constexpr uintptr_t AI_WAR_PRESEED_TARGETS     = 0x04d4f400;  // MEDIUM: builds neighbor list for war evaluation (vtable handler only)
    constexpr uintptr_t AI_VTABLE_WAR_HANDLER      = 0x043a25e0;  // MEDIUM: scripted effect war dispatch (complex, not for bridge use)

    // ─────────────────────────────────────────────────────────────────────────
    // CAIWarStance / CAIWarCoordinator — offensive-stance drive (Fix B1)
    // (re_caiwarcoordinator_FINAL.md; header = IMAGE − 0x100000)
    // ─────────────────────────────────────────────────────────────────────────
    // Per-war stance selector — B1 entry. Reads the stance-DB global, computes the
    // attacker/offensive condition, and WRITES the chosen SAIWarStance* to
    // coord_war_record + COORDREC_STANCE_PTR (0x70).
    // ⚠ OFFSET-CONVENTION FIX (2026-06-30, crash ck3_20260630_115545): the four B1a
    // coordinator function offsets below were stored as the RAW Ghidra image VA
    // (0x04e6xxxx) instead of header-offset = ghidra_va − 0x100000. The bridge calls
    // `g_base_addr + <offset>`, which lands at ghidra (offset + 0x100000) — so the OLD
    // 0x04e6xxxx values resolved to ghidra 0x04f6xxxx, the WRONG functions. The bootstrap
    // in particular resolved to FUN_04f69c70, a std::string-builder — calling it with a
    // coordinator record as param_1 faulted the game thread non-recoverably (the observed
    // crash). Convention re-verified against the renamed, working CB_GET_VALID_OPTIONS
    // (ghidra 0x6dce6d0 ↔ header 0x6cce6d0) and ai_cb_selector (ghidra 0x4e980d0 ↔ header
    // 0x4d980d0): header = ghidra_va − 0x100000, uniformly. Corrected 0x04e6→0x04d6 below.
    constexpr uintptr_t STANCE_PERWAR_SELECT       = 0x04d66550;  // HIGH: ghidra FUN_04e66550(coord_war_record, char/realm)  [was 0x04e66550 — +0x100000 bug]
    // Per-coordinator stance-update tick — decrements re-eval cooldowns, walks the
    // war-record list at coord+0x50, calls STANCE_PERWAR_SELECT per record. B1a
    // re-eval entry (zero the record's cooldowns then call this so the engine
    // re-selects now). Touches unit-stack merge/split RNG ⇒ fake-RNG scope MANDATORY.
    constexpr uintptr_t COORD_STANCE_TICK          = 0x04d66220;  // HIGH: ghidra FUN_04e66220(coordinator, …)  [was 0x04e66220 — +0x100000 bug]
    // Objective-sublist BOOTSTRAP seeder — FUN_04e69c70(rec). Unconditionally
    // _Znwm(0x98)→ctor FUN_04e3da00(obj,rec)→push_back into rec+0x50, taking the
    // objective count rec+0x5c from 0→1 (and bumps rec+0x1b50). NO +0x5c gate, so it
    // bootstraps a record from zero — UNLIKE COORD_STANCE_TICK (which early-returns at
    // rec+0x5c==0) and the in-tick maintainer FUN_04e669e0 (which only REFINES an
    // existing sublist). A FRESH steered war's record has rec+0x5c==0 / rec+0x50==NULL
    // (core-dump ground-truth, re_steered_war_offensive_root.md §7), so this MUST run
    // before COORD_STANCE_TICK or the stance tick does nothing. Vtable method (only
    // INDIRECTION/DATA xrefs); the engine invokes it from the realm-AI war-PLANNING
    // phase (FUN_04dedfd0→FUN_04de8cc0→FUN_04def300→FUN_04def140) which runs BEFORE the
    // stance tick FUN_04dedd40 in the same AI tick. Touches engine allocator only (no
    // RNG), but call under the same fake-RNG scope + watchdog for uniform crash-net.
    constexpr uintptr_t COORD_OBJ_BOOTSTRAP        = 0x04d69c70;  // HIGH(decomp 20260630): ghidra FUN_04e69c70(rec) seeds rec+0x50 obj sublist 0→1  [was 0x04e69c70 — +0x100000 bug → resolved to string-builder FUN_04f69c70, crash ck3_20260630_115545]
    // In-tick objective MAINTAINER — FUN_04e669e0(rec). Reached inside COORD_STANCE_TICK
    // at the rec+0x98<2 branch; refines/extends the EXISTING rec+0x50 sublist (gated on
    // rec+0x5c!=0). Cannot bootstrap. Listed for completeness; not called directly.
    constexpr uintptr_t COORD_OBJ_MAINTAIN         = 0x04d669e0;  // HIGH(decomp): ghidra FUN_04e669e0(rec) refines existing sublist  [was 0x04e669e0 — +0x100000 bug]
    // Stance-DB member-init (vfunc[2]); fills the 7 embedded stances at +0xfb0..+0x1010.
    constexpr uintptr_t STANCE_DB_INIT             = 0x04e922a0;  // HIGH: CAIWarStanceDatabase init
    constexpr uintptr_t STANCE_DB_CTOR             = 0x06ed25b0;  // HIGH: constructs+publishes the singleton (new char[0x1020])
    constexpr uintptr_t STANCE_GET_ATK_OFF         = 0x04e92a30;  // HIGH: return *(DB+0xfb0) — attacker_offensive getter (siblings a40..a90)
    // Record→war resolver (the live-probe match key — see COORDREC notes below).
    // CORRECTION (re_query_war_stance.md §2b): FUN_04e60920 returns the PRIMARY
    // CHARACTER of the record's side (resolving the war internally from REC_WAR_ID),
    // NOT a war handle. The war-id match key is the plain field rec+0x18 (REC_WAR_ID)
    // — read it directly; no call needed. Suggested rename: CAIWarCoordRec_ResolvePrimaryChar.
    constexpr uintptr_t COORD_REC_TO_WAR           = 0x04e60920;  // HIGH(addr): FUN_04e60920(rec) → PRIMARY CHAR (not a war)

    // ─────────────────────────────────────────────────────────────────────────
    // war_data front-buffer layout (the candidate list AI_CB_EVALUATE_ALL reads)
    // ─────────────────────────────────────────────────────────────────────────
    // Accessed via FUN_04e219e0(ai_ctrl) which returns *(ai_ctrl+0x20) + 0x98.
    // Populated by AI_WAR_TARGET_SELECTOR → FUN_04e22330 swap (copies from
    // wd+0x118 with stride 4 bytes).
    //
    // AI_CB_EVALUATE_ALL iterates with stride +1 uint32 (line 659 of decomp,
    // re_ai_war_post_decl_early_exits.md confirms count-in-uint32s semantics).
    // Each entry = 1 uint32 holding the canonical character handle:
    //   - low 24 bits: character ID (used to index CharacterDB)
    //   - high 8 bits: generation/type tag (must match char struct +0x18)
    // Mismatched high byte → goto LAB_04e99310 → silent skip (NO crash).
    //
    // ck-7dop Path C pre-seed: overwrite entry[0] at FRONT_BUF_BEGIN with
    // bridge-vouched target's canonical handle (read from char struct +0x18)
    // before calling AI_WAR_POST_DECL.
    constexpr uint64_t AI_CTRL_WAR_DATA_PTR     = 0x20;  // HIGH: ai_ctrl→war_data ptr
    constexpr uint64_t WAR_DATA_FRONT_BUF       = 0x98;  // HIGH: candidate buffer header (see fields below)
    constexpr uint64_t FRONT_BUF_BEGIN          = 0x00;  // HIGH: uint32* to entries (relative to wd+0x98)
    constexpr uint64_t FRONT_BUF_COUNT          = 0x0C;  // HIGH: int32 entry count (uint32s; entry stride = 1)
    constexpr uint64_t CHAR_CANONICAL_HANDLE    = 0x18;  // HIGH: uint32 canonical handle (CB-eval ID match key)

    // Console command handlers (can be called directly)
    constexpr uintptr_t CMD_AI_ENABLE              = 0x04100770;
    constexpr uintptr_t CMD_AI_DISABLE             = 0x041010d0;
    constexpr uintptr_t CMD_AI_DECLARE_WAR         = 0x04122760;
    constexpr uintptr_t CMD_AI_TRY_INTERACTION     = 0x04127e90;
    constexpr uintptr_t CMD_AI_TRY_DECISION        = 0x04128300;
    constexpr uintptr_t CMD_AI_INTERACTION_VALUES   = 0x0411fc40;
    constexpr uintptr_t CMD_AI_TOGGLE_ALL          = 0x04134500;
    constexpr uintptr_t CMD_SET_IS_AI              = 0x04105500;
    constexpr uintptr_t CMD_YESMEN                 = 0x040fce10;
    constexpr uintptr_t CMD_NOMEN                  = 0x040fd020;
    constexpr uintptr_t CMD_PRINT_AI_TIMINGS       = 0x04d03b80;

    // Character utilities
    constexpr uintptr_t CHAR_IS_ALIVE              = 0x0664b2c0;  // 12 bytes
    constexpr uintptr_t CHAR_IS_RULER              = 0x0574f390;  // 12B, *(char+0x1b8)!=0 (Ghidra 0x0674f390)
    constexpr uintptr_t GET_FACTION_LIST           = 0x06689a10;  // 29B, *(*(char+0x1b8)+0x120)
    constexpr uintptr_t CHAR_GET_AI_CONTROLLER     = 0x06680040;  // 32 bytes
    constexpr uintptr_t CHAR_ITERATOR_START        = 0x05f5a1f0;  // 13 bytes
    constexpr uintptr_t CHAR_ITERATOR_NEXT         = 0x06708010;  // 5 bytes (ret p+0x20)

    // Hash function used for interaction lookup
    constexpr uintptr_t STRING_HASH                = 0x08113140;  // MurmurHash variant

    // Strategy/goal processing
    constexpr uintptr_t STRATEGY_GOAL_PROCESS      = 0x04d64160;  // 1126B, computes geographic bounds
    constexpr uintptr_t STRATEGY_EXECUTE           = 0x04d66220;  // 683B, timer mgmt + goal dispatch
    constexpr uintptr_t SELECT_GOALS               = 0x04d66550;  // HIGH: goal pool selection (financial state) (ck-e58l.20)
    constexpr uintptr_t SELECT_BEST_GOAL           = 0x04d84740;  // HIGH: core priority scoring loop (ck-rk3r) (ck-e58l.20)
    constexpr uintptr_t CHECK_GOAL_ELIGIBILITY     = 0x04d91b50;  // HIGH: eval "potential" trigger (ck-rk3r) (ck-e58l.20)
    constexpr uintptr_t SCORE_GOAL_PRIORITY        = 0x04d91cd0;  // HIGH: eval "weight" script (ck-rk3r) (ck-e58l.20)
    constexpr uintptr_t GET_GOAL_CANDIDATE_LIST    = 0x04d92aa0;  // HIGH: goal list by mode+tier (ck-rk3r) (ck-e58l.20)
    constexpr uintptr_t CREATE_NEW_GOALS           = 0x04d669e0;  // MEDIUM: goal creation on timer B expiry (ck-e58l.20)

    // UpdatePostExecute dispatch functions (aliases for the AI dispatch functions above)
    constexpr uintptr_t DISPATCH_MARRIAGES         = AI_MARRIAGES_DISPATCH;     // R14: 9 flags
    constexpr uintptr_t DISPATCH_INTERACTIONS      = AI_INTERACTIONS_DISPATCH;   // R14: 16 flags
    constexpr uintptr_t DISPATCH_SCHEMES           = AI_CALL_ALLIES_DISPATCH;   // R14: +0x0D (CallAllies)

    // Personality accessors (sequential, stride 0x20)
    // Each takes CCharacter* -> returns int (sign-extended from signed char)
    // Pattern: if(*(char+0x1a8)!=0) return *(signed char*)(*(*(char+0x1a8)+0x308) + axis_index)
    constexpr uintptr_t GET_AI_HONOR               = 0x0664dca0;  // CONFIRMED: axis 0
    constexpr uintptr_t GET_AI_GREED               = 0x0664dcc0;  // CONFIRMED: axis 1
    constexpr uintptr_t GET_AI_RATIONALITY         = 0x0664dce0;  // CONFIRMED: axis 2
    constexpr uintptr_t GET_AI_ENERGY              = 0x0664dd00;  // CONFIRMED: axis 3
    constexpr uintptr_t GET_AI_BOLDNESS            = 0x0664dd20;  // CONFIRMED: axis 4
    constexpr uintptr_t GET_AI_ZEAL                = 0x0664dd40;  // CONFIRMED: axis 5
    constexpr uintptr_t GET_AI_VENGEFULNESS        = 0x0664dd60;  // CONFIRMED: axis 6
    constexpr uintptr_t GET_AI_COMPASSION          = 0x0664dd80;  // CONFIRMED: axis 7
    constexpr uintptr_t GET_AI_SOCIABILITY         = 0x0664dda0;  // CONFIRMED: axis 8

    // Character ID → pointer lookup
    constexpr uintptr_t CHAR_ID_TO_PTR             = 0x037df190;

    // Player character ID getter (returns DAT_0bfad8c0)
    constexpr uintptr_t GET_PLAYER_CHAR_ID         = 0x061642e0;

    // AI personality evaluator (926KB, full weight logic)
    constexpr uintptr_t AI_PERSONALITY_EVAL        = 0x061b8cf0;
    // Parametric personality getter: int GetAIValue(CCharacter*, int axis_index)
    // Same logic as the 9 fixed getters but takes axis as parameter — MUST be hooked
    constexpr uintptr_t AI_PERSONALITY_PARAMETRIC  = 0x06688310;

    // AI decision core functions
    constexpr uintptr_t AI_TRY_DECISION_CORE       = 0x04cd2190;  // (result, char, decision_obj, rng)

    // Game command dispatch
    constexpr uintptr_t GAME_COMMAND_DISPATCH      = 0x07560390;  // FUN_07660390 - 0x100000 image base

    // Character scope resolver (from console commands)
    constexpr uintptr_t RESOLVE_CHAR_SCOPE         = 0x05b7e6d0;  // (param_list, index) -> char*

    // CAIWatchWindow (confirmed Round 5)
    constexpr uintptr_t AIWATCH_VTABLE             = 0x0b758128;  // CAIWatchWindow vtable
    constexpr uintptr_t AIWATCH_TYPEINFO           = 0x0b758190;  // CAIWatchWindow typeinfo
    constexpr uintptr_t CMD_AIWATCH                = 0x040fd440;  // "aiwatch" console handler (488B)
    constexpr uintptr_t AIWATCH_TOGGLE_FLAG        = 0x0bed75d4;  // byte: toggled by aiwatch cmd
    constexpr uintptr_t DEBUG_FLAGS_BASE           = 0x0bed75d0;  // byte array: +0=yesmen, +1=nomen, +2=instant, +3=ai_watch_sub, +4=aiwatch, +5=warwatch, +8=instabuild(int32), +0x10=bypass(byte)
    constexpr size_t    DEBUG_BYPASS_OFFSET        = 0x10;        // bypass flag: skips interaction/scheme/decision preconditions

    // --- CONFIRMED Round 2 accessor functions ---

    // Resource accessors (return int64 fixed-point, divide by 100,000 for float)
    // Gold: fn(char, resource_type) — 0=gold, 3=treasury, 4=barter
    constexpr uintptr_t GOLD_GET                   = 0x06655990;  // CONFIRMED: reads *(*(char+0x1a8)+0x100)
    constexpr uintptr_t PIETY_GET                  = 0x0664b8c0;  // CONFIRMED: reads *(*(char+0x1a8)+0x110)
    constexpr uintptr_t PRESTIGE_GET               = 0x0664b8e0;  // CONFIRMED: reads *(*(char+0x1a8)+0x130)

    // Skill accessors (CONFIRMED: int32, skill_index 0-5 = dip/mar/stew/int/learn/prowess)
    constexpr uintptr_t SKILL_GET_BASE             = 0x0666c7c0;  // CONFIRMED: fn(char, skill_index) → base skill
    constexpr uintptr_t SKILL_GET_CALC             = 0x0666c800;  // CONFIRMED: fn(char, skill_index) → calculated skill
    constexpr uintptr_t SKILL_SET                  = 0x0666c720;  // CONFIRMED: fn(char, skill_index, value)

    // Military strength accessors
    constexpr uintptr_t MIL_CURRENT                = 0x0664beb0;  // CONFIRMED: reads *(*(char+0x1b8)+0x2F0), int32
    constexpr uintptr_t MIL_MAX                    = 0x0664bed0;  // CONFIRMED: reads *(*(char+0x1b8)+0x2F4), int32

    // Pause system functions (CONFIRMED, 6-way cross-validated)
    constexpr uintptr_t PAUSE_SET                  = 0x075fd240;  // CONFIRMED: fn(idler, pause_val, player_id)
    constexpr uintptr_t PAUSE_TOGGLE               = 0x075fd2d0;  // CONFIRMED: debounced toggle
    constexpr uintptr_t IS_MANUALLY_PAUSED         = 0x03883f10;  // CONFIRMED: reads *(DAT_0c6b53e8 + 0x20)

    // --- Round 3: Trait system functions ---
    constexpr uintptr_t ADD_TRAIT                  = 0x066c88c0;  // HIGH: CAddOrRemoveTraitCommand ctor
    constexpr uintptr_t TRAIT_GET_LIST             = 0x0664f4f0;  // HIGH: fn(char) -> struct{+0=array, +0xC=count}
    constexpr uintptr_t TRAIT_ADD                  = 0x066746a0;  // HIGH: sorted insert + side effects
    constexpr uintptr_t TRAIT_REMOVE               = 0x06676150;  // HIGH: sorted remove + side effects
    constexpr uintptr_t TRAIT_BSEARCH              = 0x037fe8c0;  // HIGH: binary search on sorted trait array
    // Trait pipeline functions (from trait gain/loss investigation)
    constexpr uintptr_t TRAIT_ADD_GATE             = 0x06575000;  // HIGH: validate + add trait (exclusion check, notify) (ck-e58l.20)
    constexpr uintptr_t TRAIT_ADD_INNER            = 0x06574630;  // HIGH: permission check + call core insert (ck-e58l.20)
    constexpr uintptr_t TRAIT_INSERT_CORE          = 0x07528790;  // HIGH: bitmask exclusion + vfunc dispatch (ck-e58l.20)
    constexpr uintptr_t ADD_TRAIT_EFFECT_F         = 0x03af2050;  // HIGH: CAddTraitEffect<false>::Execute (ck-e58l.20)
    constexpr uintptr_t ADD_TRAIT_EFFECT_T         = 0x03afa9a0;  // HIGH: CAddTraitEffect<true>::Execute (ck-e58l.20)

    // --- Round 3: Claim & succession functions ---
    constexpr uintptr_t CLAIM_GET_STATE            = 0x03d725b0;  // HIGH: GetClaimStateFor (1027B)
    constexpr uintptr_t CLAIM_BUILD_LIST           = 0x03e25c10;  // R25 confirmed: FUN_03f25c10 - 0x100000 BuildClaimsList (425B)
    constexpr uintptr_t CLAIM_REMOVE               = 0x06684b40;  // HIGH: RemoveExplicitClaim
    constexpr uintptr_t CLAIM_CHANGE_STRENGTH      = 0x06684e60;  // HIGH: 0=make strong, 1=make weak
    // R25 Phase 4: Title-side claimant functions (CLandedTitle claimant array at title+0x2A8)
    constexpr uintptr_t TITLE_ADD_CLAIMANT         = 0x051ceb80;  // HIGH: CLandedTitle::AddClaimant(title, char_obj) — adds to title+0x2A8
    constexpr uintptr_t TITLE_REMOVE_CLAIMANT      = 0x051cee60;  // HIGH: CLandedTitle::RemoveClaimant(title, char_obj) — removes from title+0x2A8
    constexpr uintptr_t CHAR_GET_CLAIMS_ARRAY      = 0x05784ac0;  // HIGH: CCharacter::GetClaimsArray(char) — returns core_data+0x1E8
    // SUSPECT 2026-06-20: image 0x05884ac0 resolves +0x50 into FUN_05884a70, a whitespace/string
    // parser (memchr over " \t\r\n"), NOT a claims getter. UNUSED in bridge/ (no refs) so no impact.
    // Re-RE before any consumer wires it.
    constexpr uintptr_t CLAIM_GET_STATE_FOR_TITLE  = 0x05875b80;  // HIGH: CClaim::GetClaimState(out, char, title) — searches char claims + parent fallback
    // REMOVED: SUCCESSION_GET_LINE at 0x0664ae20 is INVALID (no function at that address).
    // R11-7: Real succession functions are in the Round 11-7 section below.

    // --- Round 3: Event system functions ---
    constexpr uintptr_t EVENT_QUEUE_ENUM           = 0x05e5ace0;  // MEDIUM: pending event enumeration (365B)
    constexpr uintptr_t EVENT_OPTION_COUNT         = 0x05ecf3f0;  // MEDIUM: available_option_count (883B)
    constexpr uintptr_t AI_EVENT_EVAL              = 0x05e4d570;  // HIGH: AI event option evaluator

    // --- Round 3: Building functions ---
    constexpr uintptr_t BUILDING_TOTAL_SLOTS       = 0x0601c600;  // MEDIUM: total_building_slots
    constexpr uintptr_t BUILDING_FREE_SLOTS        = 0x0601c610;  // MEDIUM: free_building_slots
    constexpr uintptr_t BUILDING_USED_SLOTS        = 0x0601c6b0;  // MEDIUM: used_building_slots

    // --- Round 3: Interaction/decision resolution ---
    constexpr uintptr_t INTERACTION_RESOLVE        = 0x066ef340;  // MEDIUM: name-to-object resolver
    constexpr uintptr_t DECISION_EVAL              = 0x04cd2190;  // HIGH: decision evaluation (= AI_TRY_DECISION_CORE)

    // Opinion system
    constexpr uintptr_t ADD_OPINION_CMD            = 0x04ec3860;  // add_opinion console cmd handler
    constexpr uintptr_t OPINION_TRIGGER            = 0x05eb6df0;  // opinion_of_ trigger (NOT for direct use)
    // Round 4 confirmed: simple int(char_a, char_b), 84 bytes, decompilation-verified
    constexpr uintptr_t GET_OPINION_OF             = 0x06678ab0;  // HIGH: clamped opinion score
    constexpr uintptr_t OPINION_BASE_CALC          = 0x061ca540;  // int(char_a, char_b, flags)
    constexpr uintptr_t OPINION_REALM_MOD          = 0x067e3700;  // int(realm_data, char_a, char_b, flags)

    // Trait name resolution (Round 4)
    constexpr uintptr_t TRAIT_GET_DESCRIPTOR        = 0x05bc7c00;  // desc*(CTrait*) → name at desc+0x10
    constexpr uintptr_t TRAIT_INDEX_TO_PTR          = 0x064f9380;  // CTrait*(trait_db, index)

    // Trait effects system (Round 9-4)
    constexpr uintptr_t TRAIT_PARSE_FIELD           = 0x064f2960;  // HIGH: CTrait::ParseField (3816B, main switch) (ck-e58l.20)
    constexpr uintptr_t MOD_COLLECTION_PARSE        = 0x065e8bd0;  // HIGH: CModifierCollection::ParseModifier (860B) (ck-e58l.20)
    constexpr uintptr_t MOD_COLLECTION_GET_SLOT     = 0x065e6ac0;  // HIGH: CModifierCollection::GetSlot(collection, def_idx) (ck-e58l.20)
    constexpr uintptr_t MOD_DEF_LOOKUP_BY_TOKEN     = 0x065e2230;  // HIGH: look up modifier def by token ID (ck-e58l.20)
    constexpr uintptr_t MOD_DEF_GET_BY_INDEX        = 0x065e21c0;  // HIGH: CModifierDefinition* by index (ck-e58l.20)
    constexpr uintptr_t RECALCULATE_MODIFIERS       = 0x067710e0;  // HIGH: recalc all char stat modifiers (ck-e58l.20)
    constexpr uintptr_t APPLY_TRAIT_RULER_EFFECTS   = 0x06675b40;  // HIGH: ruler-specific trait effects (ck-e58l.20)

    // Title chain (Round 4)
    constexpr uintptr_t TITLE_GET_COUNTY            = 0x060c3310;  // county*(barony_title)
    constexpr uintptr_t TITLE_GET_TIER              = 0x060c58f0;  // int(title) → tier value
    constexpr uintptr_t TITLE_GET_HOLDER            = 0x060c3ed0;  // char*(title) via char DB

    // Event system (CONFIRMED)
    constexpr uintptr_t FIRE_EVENT                 = 0x0411b7e0;  // CONFIRMED: fn(param1, param2, is_fire, is_execute)
    constexpr uintptr_t EVENT_OPTION_SELECTION     = 0x02745900;  // CONFIRMED: event option handler (7404B)

    // Name resolution (Round 5)
    constexpr uintptr_t HOUSE_GET_NAME             = 0x05dba4d0;  // HIGH: house_obj → name (return param+0x10)
    constexpr uintptr_t DYNASTY_GET_FROM_HOUSE     = 0x05dbe2f0;  // HIGH: house_obj → dynasty_id (*(obj+0x24))
    constexpr uintptr_t DYNASTY_GET_NAME           = 0x05daa120;  // HIGH: dynasty_obj → name (returns obj+0x80)

    // Dynasty functions (from RE dynasty_legacy_renown investigation)
    constexpr uintptr_t DYNASTY_GET_PRESTIGE_LEVEL = 0x05daa440;  // HIGH: dynasty_obj → clamped prestige level (uint32)
    constexpr uintptr_t DYNASTY_HAS_LEGACY_PERK    = 0x05daa4a0;  // MEDIUM-HIGH: dynasty_obj, perk_id → bool
    constexpr uintptr_t DYNASTY_GET_NEXT_PERK_COST = 0x05daab70;  // MEDIUM-HIGH: dynasty_obj → int (renown cost for next perk)
    constexpr uintptr_t DYNASTY_GET_HOUSES_LIST    = 0x05daacc0;  // MEDIUM: dynasty_obj → ptr to houses array

    // Culture/faith setters (Round 5e-f)
    constexpr uintptr_t SET_CULTURE                = 0x0666ada0;  // HIGH: SetCultureOnCharacter(CCharacter*, CCulture* culture_obj, char flag) — decompile-verified, 1852B. WIRED as set_character_culture action (convert-own-culture lever, ck-#130, 20260624). Takes a culture OBJECT pointer (NOT an id); writes char+0xB0 (CULTURE_ID_DIRECT) from culture_obj+0x10, guards char+0x19c (dead), fires culture recalc. Console handler CMD_SET_CULTURE calls it with flag=0. No scope_ctx (#73) needed.
    constexpr uintptr_t SET_FAITH                  = 0x0666bc40;  // HIGH: SetFaithOnCharacter(char, faith_id, fire_onaction=1, hof_cleanup=1) — decompile-verified, WIRED as set_character_faith action (convert-own-faith lever, 20260624). Takes faith_id INT (no scope_ctx); writes char+0xB4, fires on_faith_conversion (on_action 0x33) + recalc.
    constexpr uintptr_t WAR_PARTICIPATION_SET      = 0x06678dd0;  // HIGH: fn(char, side), 25B, sets atk/def counter

    // War system (Round 5c-d)
    constexpr uintptr_t WAR_CONSTRUCTOR            = 0x065d10a0;  // HIGH: CWar ctor, 382B
    constexpr uintptr_t WAR_PARTICIPANT_SETUP      = 0x065be2d0;  // HIGH: add participants, 355B
    constexpr uintptr_t WAR_CB_SET                 = 0x05a5ab70;  // HIGH: CCasusBelli::Set, 1261B
    constexpr uintptr_t WAR_CB_CONSTRUCTOR         = 0x05a5a820;  // HIGH: CCasusBelli ctor, 304B
    constexpr uintptr_t WAR_SIDE_INSERT            = 0x065c4740;  // HIGH: insert into war side, 521B
    constexpr uintptr_t WAR_PARTICIPANT_SEARCH     = 0x038d77a0;  // HIGH: linear search (4x unrolled), 257B
    constexpr uintptr_t WAR_SET_NAME               = 0x065b6700;  // HIGH: set war name string, 220B
    constexpr uintptr_t WAR_COPY_SCORES            = 0x065b6290;  // HIGH: copy current→cached scores, 48B

    // --- War TERMINATION (ck-sd4d) -------------------------------------------
    // WAR_END_CORE(war_manager, CWar* war, int outcome, bool notify) — Ghidra
    // FUN_066c8480 (image VA 0x066c8480 - 0x100000), 2367B. THE single
    // termination path: every war in the game ends through this function.
    //
    //   entry:  if (war[IS_ENDED]) return;  war[IS_ENDED] = 1;
    //   switch (outcome) -> cb_type + {0x960, 0xa20, 0x9c0, 0xa80}
    //                       message ids   0x56,  0x57,  0x58,  0x59
    //   ...applies truces/notifications, then DESTROYS the war:
    //       (*war->vtable[0])(war);        // dtor
    //       memset(war, 0, 0x300);         // the whole CWar is wiped
    //       war_db.table[slot] = nullptr;  // slot returned to the free list
    //
    // The wipe happens in the SAME call that sets IS_ENDED, so an ended war is
    // NEVER visible to a polling client — confirmed empirically: 0 of 87 wars
    // in ck3_20260421_195552.core (1097 AD, a 230-year game) carry IS_ENDED.
    // Every "skip ended wars" filter in an enumerator is therefore a guard for
    // the deferred-removal case only (war_db+0x48 set while the DB is being
    // iterated), not a way to observe an ended war. Outcome and end date are
    // observable ONLY by hooking here — bridge/src/war_history_hook.cpp.
    constexpr uintptr_t WAR_END_CORE               = 0x065c8480;  // HIGH: ends AND destroys a war (decompile-verified, ck-sd4d)

    // Console handlers (Round 5c-f)
    constexpr uintptr_t CMD_SET_CULTURE            = 0x04124030;  // HIGH: set_culture console handler, 1086B
    constexpr uintptr_t CMD_ADD_REALM_LAW          = 0x04123790;  // HIGH: add_realm_law handler, 635B
    constexpr uintptr_t CMD_FACTION_SPAWN          = 0x0410f110;  // HIGH: faction_spawn handler, 729B

    // Realm law management (Round 5e, extended by law-change investigation)
    constexpr uintptr_t ADD_REALM_LAW              = 0x06672bf0;  // HIGH: AddLawToCharacter(char, law, mode:u8, extra), 201B.
    // CORRECTION (re_write_action_legitimacy_audit.md §4, ck-84): AddLawToCharacter
    // is the RAW law-vector mutator — it does NOT deduct prestige/piety, does NOT
    // set the ~10yr cooldown, and does NOT run the powerful-vassals vote regardless
    // of mode. mode=7 fires the on_pass SCRIPT + notify; mode=4 skips pass effects.
    // The earlier "mode=7 = cost+cooldown+event" claim (re_ck-iglm_add_law_modes.md)
    // was REFUTED. For the COSTED player path use ADD_LAW_COMMAND_CTOR below.

    // ── Costed player Change-Realm-Law command (CAddLawCommand) — ck-84 ──────────
    // The LEGITIMATE path. A CJominiCommandBase-derived typed command (same family
    // as construction/hire-merc). Dispatched via GAME_COMMAND_DISPATCH(mgr,&cmd,0xe,0)
    // → CommandQueuePush → engine runs CanExecute (cost/cooldown/vote/eligibility)
    // then Execute. Execute itself calls AddLawToCharacter(char,law,7,0); the
    // prestige cost + cooldown + powerful-vassals vote + vassal-opinion penalty are
    // applied by the engine's command pipeline (CanExecute gate + on_accept), NOT by
    // AddLawToCharacter. Replicate the do_add_construction dispatch template with an
    // EXPLICIT actor (ctor sets char id @+0x20=0xffffffff, then we overwrite).
    // ── ck-84 CORRECTION (2026-06-21): ALL FIVE offsets below were recorded
    //    0x100000 TOO HIGH. The prior agent stored IMAGE VAs as header offsets, so
    //    the bridge call site `g_base_addr + 0x067cd2f0` resolved to image 0x067cd2f0
    //    — which is FUN_068cd200, a generic hashmap-INSERT routine, NOT the command
    //    ctor. Calling a hashmap insert with command-shaped args corrupted the stack
    //    = the non-SIGSEGV HARD CRASH (the sigsetjmp/SIGSEGV net can't bound a
    //    std::terminate/stack-smash). The real CAddLawCommand functions live at image
    //    0x067cdXXX; their header offsets = image − 0x100000 = 0x066cdXXX. Verified
    //    via func_at (is_entry:true) + decompile + the live vtable read from the core
    //    dump (vt resolves to header 0x066cd410 / 0x066db1e0 / base-dtor 0x0755b650).
    //    This is the same +0x100000 image-VA-vs-header bug flagged in memory
    //    [[gated-write-leaf-strategy-disproven]] — always func_at EVERY offset.
    constexpr uintptr_t ADD_LAW_COMMAND_CTOR       = 0x066cd2f0;  // HIGH: CAddLawCommand ctor(cmd) — CJominiCommandBase_ctor(cmd,0) + vtable 0xbd98ce0 @cmd[0] + 2nd-vtable 0xbd98d90 @cmd+0x18; inits char @+0x20=0xffffffff, law @+0x28=sentinel. FUN_067cd2f0 (image), 52B.
    constexpr uintptr_t ADD_LAW_COMMAND_EXECUTE    = 0x066cd370;  // HIGH: CAddLawCommand::Execute(cmd) — reads char id @cmd+0x20 (&0xffffff), law @cmd+0x28; calls AddLawToCharacter(char,law,7,0). FUN_067cd370 (image), 77B.
    constexpr uintptr_t ADD_LAW_COMMAND_CANEXEC    = 0x06d30000;  // HIGH: CanExecute(law, char, ctx) — FUN_06e30000 (image). IsRuler(char) + law-group-level bitmask (law+0xb08<10 && (0x23d>>level)&1) + !CharacterHasLaw + cost-affordability eval FUN_06e2fd40(law,char,ctx). Returns affordability/eligibility; if !eligible+ctx!=0 it fills the ctx reject-reason. The gate; called by AI law evaluator + UI. Reached via vt[7] wrapper ADD_LAW_COMMAND_CANEXEC_VT.
    constexpr uintptr_t ADD_LAW_COMMAND_CANEXEC_VT = 0x066cd410;  // HIGH: vt[7] (vtable+0x38) CanExecute wrapper FUN_067cd410(cmd,ctx) — resolves char from cmd+0x20 via CharacterDB, then calls CANEXEC(law@+0x28, char, ctx). This is the slot the engine command pipeline invokes.
    constexpr uintptr_t ADD_LAW_COMMAND_PREPARE_VT = 0x066db1e0;  // HIGH: vt[9] (vtable+0x48) clone/prepare FUN_067db1e0(&out,cmd) — _Znwm(0x30) + copies base + both vtables + char@+0x20 + law@+0x28. Byte-for-byte analog of construction's prepare. (Bridge reads vt[9] dynamically; recorded for reference.)
    // CAddLawCommand layout (after ctor): cmd+0x20 = u32 char id (full engine id,
    // low 24 bits = index); cmd+0x28 = CLaw* (law object ptr from REALM_LAW_DB).
    // mode=7 (full effects: cost+cooldown+event) is what `add_realm_law`
    // console + all script-effect / RTTI wrappers pass; mode=4 skips pass
    // effects (only `*_skip_effects` callers). No other values observed.
    // See re/findings/re_ck-iglm_add_law_modes.md (ck-iglm).
    constexpr uintptr_t FIND_REPLACE_LAW           = 0x06664510;  // HIGH: finds existing law in same group, replaces it, 305B
    constexpr uintptr_t REPLACE_IN_CONTAINER       = 0x06664040;  // HIGH: array swap old→new + succession rule check, 782B
    constexpr uintptr_t FINALIZE_LAW_CHANGE        = 0x06664350;  // HIGH: sort array, fire event 0x328A, recalculate, 402B
    constexpr uintptr_t CHARACTER_HAS_LAW          = 0x06883920;  // WARNING(#109): WRONG as a header offset — resolves into FUN_069836e0 (image 0x6983920), not a function entry. Kept for back-compat; do NOT call. Use CHARACTER_HAS_LAW_ENTRY below.
    constexpr uintptr_t CHARACTER_HAS_LAW_ENTRY    = 0x067839a0;  // HIGH(#109): char(char,law) — CharacterHasLaw entry (Ghidra-named, is_entry:true, image 0x68839a0 → header 0x67839a0). Called by CAddLawCommand CanExecute (FUN_06e30000) to detect an already-active law. Verified: header+0x100000 == Ghidra entry.
    constexpr uintptr_t PARSE_ADD_REALM_LAW        = 0x04123aa0;  // HIGH: parse law name, DB lookup, validate ruler

    // ── CCreateTitleCommand (#111) — LEGITIMATE de-jure create-title player command ──
    // re/findings/re_create_title_action.md. Structurally IDENTICAL to CAddLawCommand
    // above (CJominiCommandBase-derived, ruler@+0x20, target@+0x24), dispatched via
    // ⚠ #115 (live-verified): do NOT use GAME_COMMAND_DISPATCH for this command. The
    // game runs in OBSERVER mode (PLAYER_CHAR_ID = 0xFFFFFFFF) and the deferred
    // player-command queue is only drained for the active player, so a dispatched
    // create-title NEVER executes (no mint, no cost deducted) — true for BOTH mode
    // 0xe and mode 7. The WORKING path: run vt[7] IsValid(cmd,NULL) gate, then on
    // PASS call CREATE_TITLE_CMD_EXECUTE (FUN_067cabf0) SYNCHRONOUSLY under a
    // transient PLAYER_CHAR_ID swap to the acting ruler (same gated+synchronous
    // +player-swap shape as execute_raise_armies_intent). The canonical UI handler
    // FUN_04de1ac0 dispatches with mode 7 only because it has a real active player.
    // The gate (controls-de-jure-counties + not-extant + gold cost)
    // lives INSIDE the engine — routing through it is legitimate, not a memory poke.
    // ALL header offsets = image VA − 0x100000 (verified func_at is_entry + decompile +
    // live core-dump vtable read; do NOT store image VAs as header offsets — see
    // [[gated-write-leaf-strategy-disproven]]).
    // Command struct (0x28 bytes): vtable@+0x00, base bookkeeping +0x08/+0x10,
    // 2nd-vtable@+0x18, u32 ruler_id@+0x20 (FULL engine id), u32 title_id@+0x24. Type-id 0x2cc0.
    constexpr uintptr_t CREATE_TITLE_CMD_VTABLE    = 0x0bc98588;  // HIGH: primary vtable @cmd[0] (image 0xbd98588 − 0x100000). vt[7]=IsValid(FUN_067cacf0), vt[9]=clone/prepare(FUN_067da3f0).
    constexpr uintptr_t CREATE_TITLE_CMD_VTABLE2   = 0x0bc98640;  // HIGH: secondary vtable @cmd+0x18 (image 0xbd98640 − 0x100000).
    constexpr uintptr_t CREATE_TITLE_CMD_VALIDATE  = 0x066cacf0;  // HIGH: vt[7] IsValid(cmd,ctx) FUN_067cacf0 — resolves ruler@+0x20 + title@+0x24, cheat-bypass check, then calls CREATE_TITLE_GATE(ruler,title,1,reasons). Returns nonzero iff createable. ctx!=0 fills reject-reason vector.
    constexpr uintptr_t CREATE_TITLE_CMD_EXECUTE   = 0x066cabf0;  // HIGH: Execute(cmd) FUN_067cabf0 — reached by the command pipeline; resolves ruler+title, calls CREATE_TITLE_APPLY_CORE. (We never call directly; the engine runs it on the command-queue tick.)
    constexpr uintptr_t CREATE_TITLE_GATE          = 0x0611a500;  // HIGH: CanCreateTitle(ruler,title,flag=1,reasons_out) FUN_0621a500 (image 0x621a500 − 0x100000) — THE gate (de-jure control + not-extant + cost); shared by 5 AI scorers. Legitimate verdict. (Documentation-only; not called directly — reached via the vt[7] IsValid wrapper above.)
    // #131 (2026-06-24, re_create_title_dejure_integrity_131.md): the engine gate above
    // FAITHFULLY runs CK3's scripted `can_create`. For ~60 power-projection empires
    // (e_scandinavia, e_britannia, e_hre, …) that trigger falls through to
    // `trigger_else = { always = yes }` under the DEFAULT game rule, so it PASSES a
    // ZERO-de-jure ruler (vanilla "power projection" formation). NOT a cheat/bypass:
    // the IsBypassActive flag (DAT_0bfd75d0+0x10, header 0x0bed75d0) is 0 in our observer
    // game and the player-list bypass set is empty (both live-verified). The bridge adds a
    // stricter-than-vanilla de-jure-MAJORITY gate (tier>=4) in execute_create_title_intent.
    constexpr uintptr_t CREATE_TITLE_DEJURE_CORE   = 0x06118550;  // HIGH: can_create_title_dejure_core FUN_06218550(title,ruler,conds,mode,reasons) — enumerates tier-2 de-jure counties (via DEJURE_COUNTY_COLLECTOR), counts realm-held, FAILs held<ceil(threshold*count). (image 0x6218550 − 0x100000)
    constexpr uintptr_t DEJURE_COUNTY_COLLECTOR    = 0x06122620;  // HIGH: dejure_county_collector FUN_06222620(title, target_tier, &out_list) — recursive de-jure descendant collector (image 0x6222620 − 0x100000)
    constexpr uintptr_t DEJURE_CREATE_THRESHOLD_FN = 0x061181e0;  // HIGH: dejure_creation_threshold FUN_062181e0(title.0x10, ruler.0x18) → scripted fraction (FUN_03e29d80 0x4a); ×county_count = required held (image 0x62181e0 − 0x100000)
    constexpr uintptr_t TITLE_CREATION_BYPASS_CHK  = 0x05c298b0;  // HIGH: IsBypassActive FUN_05c398b0(&g_title_creation_bypass, ruler): (*(this+0x10)?player_list_member(ruler):0). Flag 0 in observer ⇒ real gate always runs (image 0x5c398b0 − 0x100000)
    constexpr uintptr_t TITLE_CREATION_BYPASS_OBJ  = 0x0bed75d0;  // HIGH: g_title_creation_bypass singleton (DAT_0bfd75d0, header = image−0x100000). +0x10 byte = bypass flag (0=off). +0x2c float 0x3f666666 confirms this is the real singleton.
    constexpr uintptr_t CREATE_TITLE_APPLY_CORE    = 0x0611b800;  // HIGH: create-title application core FUN_0621b800(ruler,title) (image 0x621b800 − 0x100000) — allocates realm-queue item + grant primitive. Do NOT call directly (bypasses the gate).
    constexpr uintptr_t CREATE_TITLE_CMD_CTOR      = 0x066cabb0;  // HIGH: real CCreateTitleCommand ctor FUN_067cabb0(cmd,ruler_id,title_id) (image 0x67cabb0 − 0x100000) — CJominiCommandBase_ctor(cmd,0)+stamp both vtables+ruler@+0x20+title@+0x24. Bridge builds the POD inline (byte-identical), but this is the canonical ctor. The canonical dispatcher is FUN_04de1ac0 (the UI "Create Title" handler): ctor → vt[7] IsValid(cmd,NULL) → vt[9] prepare → GAME_COMMAND_DISPATCH(mgr,&out,7,0).

    // Title-level succession laws (TITLE-scoped, NOT realm/char-scoped) — re_title_succession_law_elective.md
    // The `title_succession_laws` group (feudal_elective_succession_law, princely_/saxon_/
    // scandinavian_/gaelic_/tribal_elective_succession_law, etc.) is APPLIED TO A TITLE, not
    // a character. AddLawToCharacter (above) writes char+0x1b8→ruler_data+0x200 which NEVER
    // holds title_succession_laws (verified: 0/1515 rulers in core ck3_20260421_195552).
    // Effect class: CAddTitleLawEffect<CLandedTitle, ELawChangeEffects 0=effects/1=skip>.
    // Wire a title-scoped setter (resolve title via lookup_title_obj, NOT a character).
    constexpr uintptr_t CMD_ADD_TITLE_LAW_HANDLER  = 0x0411eb80;  // HIGH: add_title_law console handler (scoped-title law setter). Looks law up in the SAME realm-law DB hash; applies to CLandedTitle. Self-doc: "add law to scoped title, overriding any current law from the same group. DOES apply law change costs and effects."
    // Direct in-engine title-law applier entry NOT byte-pinned this session (gh.py VA map
    // inconsistent) — resolve CAddTitleLawEffect::Execute from its vtable on a code-mapped
    // dump for a HIGH offset. Console handler above is the safe interim entry. — MEDIUM
    // Decision route (player-legible fallback): `adopt_special_succession_decision` exists in
    // DECISION_DB (0x0bf087f8); fire via do_send_decision once decision-option passing is added.

    // Round 6: Interaction dispatch via CSendCharacterInteractionCommand
    constexpr uintptr_t BUILD_SEND_INTERACTION_CMD = 0x06a3d920;  // R23 confirmed: FUN_06b3d920 - 0x100000 (buf, interaction, actor_id, target_id, 0, 1), 923B
    constexpr uintptr_t INTERACTION_LOOKUP_BY_IDX  = 0x06a3ca50;  // R23 confirmed: FUN_06b3ca50 - 0x100000 (db, byte_index) -> interaction*, 27B

    // Round 6: Interaction context helpers
    constexpr uintptr_t INTERACTION_CTX_INIT       = 0x06a45300;  // R23 confirmed: FUN_06b45300 - 0x100000 init context
    constexpr uintptr_t INTERACTION_CTX_RESET      = 0x06a40000;  // R26: FUN_06b40000 - 0x100000 reset context state
    constexpr uintptr_t INTERACTION_CTX_SET_TARGET  = 0x06a3f140;  // R23 confirmed: FUN_06b3f140 - 0x100000 set target
    constexpr uintptr_t INTERACTION_CTX_SET_SECONDARY_ACTOR     = 0x06a3f1b0;  // FUN_06b3f1b0 - 0x100000: writes char ref to ctx+0x2E0
    constexpr uintptr_t INTERACTION_CTX_SET_SECONDARY_RECIPIENT = 0x06a3f1e0;  // FUN_06b3f1e0 - 0x100000: writes char ref to ctx+0x2E4
    constexpr uintptr_t INTERACTION_CTX_SET_TARGET_AND_SEC_RECIP = 0x06a3f210; // FUN_06b3f210 - 0x100000: sets target+0x2DC AND secondary_recipient+0x2E4
    constexpr uintptr_t INTERACTION_CTX_SET_TERTIARY            = 0x06a3f240;  // FUN_06b3f240 - 0x100000: writes char ref to ctx+0x2E8
    constexpr uintptr_t INTERACTION_CTX_SET_RNG     = 0x06a3f500;  // R23 confirmed: FUN_06b3f500 - 0x100000 set RNG seed
    constexpr uintptr_t INTERACTION_CTX_VALIDATE    = 0x06a41880;  // R23 confirmed: FUN_06b41880 - 0x100000 validate
    // InteractionCtxValidate sub-functions (5-layer validation pipeline):
    constexpr uintptr_t VALIDATE_HARDCODED         = 0x06a41e70;  // FUN_06b41e70 - 0x100000: hardcoded eligibility checks — HIGH
    // FUN_06b42310 = full CanSend validation (whitelist→precond→can_interact→triggers→custom)
    // Ghidra: InteractionTrySend_Cascade — canonical entry for the 7-slot trigger pipeline;
    // also used by pending_interaction consumer for CanSend re-validation (former
    // event_weight::CONSUMER_CANSEND; deduped in ck-audit-B).
    constexpr uintptr_t INTERACTION_TRY_SEND_CASCADE = 0x06a42310;  // HIGH: FUN_06b42310 - 0x100000: full 7-slot trigger pipeline (ck-vqth) (ck-audit-B: renamed from VALIDATE_CAN_SEND_FULL, deduped with CONSUMER_CANSEND)
    constexpr uintptr_t VALIDATE_BASIC_ALIVE       = 0x06a41640;  // FUN_06b41640 - 0x100000: actor+target alive check (char+0x1c8==0) — HIGH
    constexpr uintptr_t VALIDATE_RELATIONSHIP_BLOCK = 0x057f0e40; // FUN_067f0e40 - 0x100000: blocking relationship check — HIGH
    constexpr uintptr_t VALIDATE_HOSTILE_GUARD     = 0x06a417b0;  // FUN_06b417b0 - 0x100000: hostile interaction permission — MEDIUM
    // ck-jvyj/ck-go1m: NOT a game-state validator (no game-paused / save-in-progress checks).
    // Duplicate-interaction guard. Returns 1=proceed, 0=block when an equivalent interaction
    // is already in the active interaction registry (matched by FUN_06b444d0 — 5-field equality
    // on actor/target/type/context). Bypassed entirely when interaction_def+0x2A39 is nonzero.
    // See re/findings/re_game_state_validation_062e5450.md.
    constexpr uintptr_t INTERACTION_HAS_NO_DUPLICATE = 0x061e5450; // HIGH: FUN_062e5450 - 0x100000: duplicate interaction guard
    constexpr uintptr_t INTERACTION_CTX_VALIDATE_SCOPES = 0x06a3dd60; // FUN_06b3dd60 - 0x100000: rebuild scope paths after manual ctx edits
    constexpr uintptr_t INTERACTION_FINALIZE        = 0x04cc5ea0;  // R23 confirmed: FUN_04dc5ea0 - 0x100000 finalize before wrap
    // ck-ml3y: Ghidra rename maybe_cb_pre_send_dispatch (MEDIUM). NOT a destructor — it's a
    // pre-send dispatcher reading flags at *(ctx)+0x253c, +0x2a32, +0x2a36 to pick one of three
    // readiness routines (FUN_06b43e70 / FUN_04dc5990 / FUN_04dc5a90). 6 callers.
    constexpr uintptr_t INTERACTION_TRY_SEND        = 0x06a43eb0;  // R23 confirmed: FUN_06b43eb0 - 0x100000 attempt send
    constexpr uintptr_t INTERACTION_TLS_CONSTRUCT   = 0x06a3e970;  // FUN_06b3e970 - 0x100000: construct TLS interaction context
    constexpr uintptr_t INTERACTION_TLS_DESTRUCTOR  = 0x06a3ec30;  // FUN_06b3ec30 - 0x100000: TLS context destructor (for __cxa_thread_atexit)
    // ck-ml3y: Ghidra rename InteractionCmd_Destroy. 158 callers — generic CInteractionEvalContext
    // destructor. Tears down: smart-ptr +0x330, dyn-buffer +0x300/+0x308/+0x30c (allocator
    // +0x310), and FUN_038df310 sub-dtors at +0x170 and +0x8. Idempotent in the empty case
    // (null-checks +0x300 and +0x330 before freeing) — safe to call on a freshly-built ctx
    // that hasn't yet been populated by CB_SCOPE_BUILDER.
    constexpr uintptr_t GET_AI_WILL_DO_SCORE       = 0x06a441c0;  // Ghidra-verified FUN_06b441c0 - 0x100000: GetAiWillDoScore(tls_ctx)->int64, 96B. Bytes: 41 56 53 48 81 ec...
    // Marriage/Alliance scoring functions (ck-zaal investigation)
    constexpr uintptr_t GET_AI_ACCEPT_SCORE        = 0x06a43be0;  // HIGH: FUN_06b43be0, evaluates ai_accept CModValue at itype+0x1B30
    constexpr uintptr_t GET_AI_ACCEPT_NEGOT_SCORE  = 0x06a43c70;  // HIGH: FUN_06b43c70, evaluates ai_accept_negotiation at itype+0x1B90
    constexpr uintptr_t ACCEPTANCE_DECISION        = 0x06a43820;  // HIGH: FUN_06b43820, score->accept/decline decision (returns 0=accept,1=auto,2=decline)
    constexpr uintptr_t INTERACTION_TRY_SEND_V2    = 0x06a43d00;  // HIGH: FUN_06b43d00, full acceptance pipeline — Ghidra: InteractionTrySend_V2 (ck-audit-B: renamed from ACCEPTANCE_EVALUATION)
    constexpr uintptr_t CMODVALUE_EVALUATE         = 0x064713b0;  // HIGH: FUN_075713b0, evaluate CModValue (base + modifier chain) (ck-e58l.20)
    constexpr uintptr_t CMODVALUE_EVAL_INNER       = 0x06571640;  // HIGH: FUN_07571640, inner CModValue loop (accumulator + modifiers)
    constexpr uintptr_t CSCRIPTEDVALUE_EVALUATE    = 0x02812c80;  // HIGH: FUN_03912c80, evaluate CScriptedValue (factor/add sub-values)
    // Modifier type virtual Modify functions (vtable+0x40 dispatches):
    constexpr uintptr_t CTRIGGER_VALUE_MODIFIER_MODIFY   = 0x051ba030; // HIGH: if trigger → acc*=factor, acc+=add
    constexpr uintptr_t COPINION_MTTH_MODIFIER_MODIFY    = 0x051b6a10; // HIGH: acc += opinion*mult/step (clamped)
    constexpr uintptr_t CAI_VALUE_MODIFIER_MODIFY        = 0x051b8ca0; // HIGH: acc += dot(9 AI axes, weights)
    constexpr uintptr_t CJOMINI_SCRIPTED_MODIFIER_MODIFY = 0x06572eb0; // HIGH: compound: iterate sub-modifier array
    // Round 6: Interaction option system (CB options for war, send_options for other interactions)
    // ALL interactions share the same option array at interaction_def+0x2530 (stride 0x7D0).
    // Option entry: +0x000=is_valid, +0x0E0=is_shown, +0x1C0=starts_enabled, +0x3A8=flag_key hash
    // Selection state: cmd+0x300=byte_array_ptr, cmd+0x30C=count (1 byte per option, 0/1)
    // interaction_def+0x2A36: 0=multi-select (send_options), non-0=single-select (CB options)
    constexpr uintptr_t INTERACTION_SELECT_OPTION   = 0x06a40160;  // FUN_06b40160 - 0x100000: sets option_data[index]=1, calls ValidateScopes
    constexpr uintptr_t INTERACTION_DESELECT_OPTION = 0x06a40350;  // FUN_06b40350 - 0x100000: sets option_data[index]=0, calls ValidateScopes
    constexpr uintptr_t INTERACTION_SET_SELECTED    = 0x06a40320;  // FUN_06b40320 - 0x100000: toggles option (select if 0, deselect if 1)
    constexpr uintptr_t INTERACTION_IS_OPTION_VALID = 0x06a406e0;  // FUN_06b406e0 - 0x100000: checks is_valid/is_shown/starts_enabled triggers
    constexpr uintptr_t INTERACTION_TRY_SELECT      = 0x06a400d0;  // FUN_06b400d0 - 0x100000: evaluates option trigger at +0xE0, returns bool
    constexpr uintptr_t INTERACTION_POPULATE_OPTIONS = 0x06a3e070; // FUN_06b3e070 - 0x100000: auto-populates options via trigger evaluation
    constexpr uintptr_t INTERACTION_CHECK_POPULATE  = 0x06a3def0;  // FUN_06b3def0 - 0x100000: validates war_target/claimant before populate
    // ck-e58l.6 — InteractionTrySend_Cost: cost-gate invoked during cascade
    constexpr uintptr_t INTERACTION_TRY_SEND_COST  = 0x06a8f740;  // HIGH: FUN_06b8f740 - 0x100000: CanSend cost gate (gold/prestige/piety/etc.) (ck-e58l.6) (ck-audit-B)

    // Round 6: War pipeline details
    constexpr uintptr_t WAR_BUILD_STATE            = 0x04d980d0;  // HIGH: build war state (CB + titles), 3351B
    constexpr uintptr_t WAR_CHECK_FEASIBILITY      = 0x04d20b60;  // HIGH: check war feasibility, 68B
    constexpr uintptr_t WAR_ATTACH_STATE           = 0x065dbcb0;  // HIGH: WarCmd__AttachCBState(cmd+0x330, cb_type_ref) — trivial: *(ptr+8) = ref
    constexpr uintptr_t WAR_ATTACH_CB              = 0x065dbcc0;  // HIGH: WarCmd__AttachCBTitle(cmd+0x330, title_data) — deep-copies title array + claim_type
    constexpr uintptr_t WAR_FINALIZE_CMD           = 0x04cc5ea0;  // R23 confirmed: FUN_04dc5ea0 - 0x100000, same as INTERACTION_FINALIZE

    // Phase 8: CB option + AI war candidate functions
    constexpr uintptr_t INTERACTION_SELECT_CB_OPTION = 0x06a3f0f0; // FUN_06b3f0f0 - 0x100000: writes CB option index to cmd+0x2DC, then PopulateOptions
    // ck-ml3y: Ghidra rename Interaction_SetCBTypeAndEvaluate(ctx, cb_type, do_eval).
    // The inline scope_ctx in AI_CB_EVALUATE_ALL (auStack_12490, ~0x338 bytes) is
    // structurally a stack-allocated CInteractionEvalContext (same destructor).
    // Per-CB ScriptTriggerEvaluator gating uses this fn to point ctx at next CB.
    constexpr uintptr_t SET_ACTIVE_CB_OPTION         = 0x06a3fee0; // FUN_06b3fee0 - 0x100000: copies option data to ctx+0x300, calls ValidateScopes+PopulateOptions
    constexpr uintptr_t AI_POPULATE_WAR_CANDIDATES   = 0x04d99190; // FUN_04e99190 - 0x100000: builds (target,CB,title) war candidate list (0x908/entry)
    constexpr uintptr_t CB_GET_VALID_OPTIONS         = 0x06cce6d0; // FUN_06dce6d0 - 0x100000: CCasusBelliType__GetValidCBOptions(cb,out,actor,target)
    constexpr uintptr_t WAR_CANDIDATE_LIST_APPEND    = 0x04da0840; // FUN_04ea0840 - 0x100000: appends 0x908-byte entry to candidate list
    constexpr uintptr_t EVAL_AI_WAR_UTILITY          = 0x065cb960; // FUN_066cb960 - 0x100000: scores war candidate for AI

    // Round 6: MaA regiment creation
    // CCreateMAARegimentCommand ctor (CJominiCommandBase subclass; recruit MaA write path).
    // VERIFIED ctor decompile (VA 0x668fce0): CJominiCommandBase_ctor(cmd,0); cmd+0x00=vt1(0xbd4b138);
    //   cmd+0x18=helper_vt(0xbd4b1e8); cmd+0x20=actor_id(u32); cmd+0x24=0xffffffff;
    //   cmd+0x28=char_id(u32, 2nd actor arg); cmd+0x2c=*(maa_type_obj+8) (the MaA-type DB id);
    //   cmd+0x30=param_5(0xffffffff, origin/loc sentinel); cmd+0x34=param_6(u8, count/flag=1).
    // Signature: void(void* cmd, u32 actor_id, u32 char_id, void* maa_type_obj, u32 origin, u8 flag)
    // Dispatch EXACTLY like do_add_construction: prepare=(*cmd[0])[9](&out,cmd); then
    //   GAME_COMMAND_DISPATCH(COMMAND_DISPATCH_MGR, &out, 0xe, 0). See re_maa_write_path.md.
    constexpr uintptr_t CREATE_MAA_REGIMENT        = 0x0658fce0;  // HIGH: CCreateMAARegimentCommand ctor, 102B
    // ── CCreateMAARegimentCommand full command path (re_create_maa_regiment_command.md, 2026-06-19 HIGH).
    // The PROPER apply entry: Execute resolves owner char + MaA type from the DBs (the piece the bare
    // leaf lacked → why the bare leaf hung/crashed), THEN calls the create-leaf with a DB-resolved this.
    // Drive ctor→Execute→base_dtor on the GAME thread under arm_fake_random_scope+watchdog+sigsetjmp.
    constexpr uintptr_t CREATE_MAA_CMD_CTOR        = 0x0658fce0;  // HIGH: 6-arg ctor(cmd,actor_id,char_id,maa_type_obj,origin,flag) — alias of CREATE_MAA_REGIMENT
    constexpr uintptr_t CREATE_MAA_CMD_EXECUTE     = 0x0658fdc0;  // HIGH: Execute body (PRIMARY this) — applies (calls create-leaf 0x062645c0 → gold 0x06f07fa0)
    constexpr uintptr_t CREATE_MAA_CMD_EXEC_THUNK  = 0x0658ff30;  // HIGH: helper->primary adjustor Execute(this-0x18)
    constexpr uintptr_t CREATE_MAA_CMD_BASE_DTOR   = 0x0755b650;  // HIGH: PRIMARY[0] base dtor (same as HIRE base_dtor)
    // ── REAL apply leaves (#62, 2026-06-19): the command queue/dispatch only validates;
    // the actual mutation is the effect leaf, callable directly (like hire). No recompute.
    constexpr uintptr_t MAA_RECRUIT_CREATE_LEAF    = 0x062645c0;  // DISPROVEN as standalone (re_maa_recruit_leaf_hangs.md): needs DB-resolved owner this; use CREATE_MAA_CMD_EXECUTE instead
    constexpr uintptr_t MAA_RECRUIT_GOLD_CHARGE    = 0x06f07fa0;  // MEDIUM: FUN_06f07fa0 gold charge (×100000), called by the leaf when charge==1
    constexpr uintptr_t MAA_STATION_APPLY_LEAF     = 0x065981e0;  // HIGH: station Execute body (Ghidra entry FUN_066981e0; header = gh-0x100000). reads cmd+0x20=regiment_id, cmd+0x24=origin_province_id; never derefs cmd vtable. Build zeroed cmd[0x40] w/ those fields; NO ctor/dtor. (CORRECTED -0x100000 2026-06-19: re_resolution_maa_station.md stored the gh-image VA 0x066981e0 → crash.)
    constexpr uintptr_t MAA_UNSTATION_APPLY_LEAF   = 0x06599050;  // HIGH: unstation Execute body (Ghidra entry FUN_06699050; header = gh-0x100000). reads ONLY cmd+0x20=regiment_id. (CORRECTED -0x100000: was gh-image VA 0x06699050 → froze.)
    constexpr uintptr_t MAA_STATION_ORIGIN_SET     = 0x0635ba00;  // MEDIUM: FUN_0635ba00(province, regiment) — clears/sets the regiment's origin province
    // CORRECTION (re_maa_station_ctor.md, live-proc 858006 verified): the two recruit-vtable
    // values below were +0x100000 too high. The recruit ctor at 0x658fce0 actually leas
    // [0xbc4b138] (primary) and [0xbc4b1e8] (helper) — confirmed by objdump of the ctor AND by
    // reading the live RELRO-resolved slots (offset_to_top=0, typeinfo==CCreateMAARegimentCommand
    // typeinfo @0xbc4c6c8). The header convention is: value == image VA; loaded = load_base + value.
    // The OLD 0x0bd4b138/0x0bd4b1e8 do NOT resolve to this command's typeinfo at runtime.
    constexpr uintptr_t MAA_CMD_VTABLE1_BAD        = 0x0bd4b138;  // WRONG (kept for git-blame): see correction above
    constexpr uintptr_t MAA_CMD_HELPER_VTABLE_BAD  = 0x0bd4b1e8;  // WRONG (kept for git-blame): see correction above
    constexpr uintptr_t MAA_CMD_VTABLE1            = 0x0bc4b138;  // HIGH: CCreateMAARegimentCommand primary vtable (cmd+0x00) — CORRECTED
    constexpr uintptr_t MAA_CMD_HELPER_VTABLE      = 0x0bc4b1e8;  // HIGH: CCreateMAARegimentCommand CGameCommandHelper vtable (cmd+0x18) — CORRECTED

    // MaA STATION / UN-STATION command ctors (re_maa_station_ctor.md, live-proc 858006 verified).
    // Both are CJominiCommandBase subclasses, identical dispatch shape to recruit but with
    // dispatch FLAGS 0x7 (the scripted-effect select_maa_origin_province path uses 0x7, not 0xe).
    // CSetMAAOriginProvince ctor @0x65981a0 (52B):
    //   void(void* cmd /*rdi*/, u32 regiment_id /*esi*/, u32 origin_province_id /*edx*/)
    //   leas [0xbc4c270] (primary, cmd+0x00) + [0xbc4c320] (helper, cmd+0x18) after
    //   CJominiCommandBase_ctor(cmd,0); writes cmd+0x20=regiment_id, cmd+0x24=origin_province_id.
    //   regiment_id source in engine = regiment_obj+0x10; origin id = origin_obj+0x8.
    // CRemoveMAAOriginProvince ctor @0x6599020 (un-station; 46B):
    //   void(void* cmd /*rdi*/, u32 regiment_id /*esi*/)  — writes only cmd+0x20=regiment_id.
    // Primary-vtable slots (resolved live): [1]=dtor, [7]=CanExecute/Validate, [9]=prepare/Clone.
    //   Set CanExecute @0x6598dd0 (reads cmd+0x20, masks &0xffffff, looks up regiment DB @0xbed76e0).
    constexpr uintptr_t SET_MAA_ORIGIN_PROVINCE    = 0x065981a0;  // HIGH: CSetMAAOriginProvince ctor (station)
    constexpr uintptr_t SET_MAA_ORIGIN_VTABLE1     = 0x0bc4c270;  // HIGH: CSetMAAOriginProvince primary vtable (cmd+0x00)
    constexpr uintptr_t SET_MAA_ORIGIN_HELPER_VT   = 0x0bc4c320;  // HIGH: CSetMAAOriginProvince CGameCommandHelper vtable (cmd+0x18)
    constexpr uintptr_t SET_MAA_ORIGIN_CANEXEC     = 0x06598dd0;  // HIGH: Set primary vtable[7] CanExecute/Validate
    constexpr uintptr_t REMOVE_MAA_ORIGIN_PROVINCE = 0x06599020;  // HIGH: CRemoveMAAOriginProvince ctor (un-station)
    constexpr uintptr_t REMOVE_MAA_ORIGIN_VTABLE1  = 0x0bc4c358;  // HIGH: CRemoveMAAOriginProvince primary vtable (cmd+0x00)
    constexpr uintptr_t REMOVE_MAA_ORIGIN_HELPER_VT = 0x0bc4c408; // HIGH: CRemoveMAAOriginProvince CGameCommandHelper vtable (cmd+0x18)

    // Round 6: Secret creation
    constexpr uintptr_t CREATE_SECRET              = 0x06685330;  // HIGH: fn(char, secret_type, target_spec, 0), 1052B

    // Round 6: Council task commands
    constexpr uintptr_t COUNCIL_TASK_CMD           = 0x05b83370;  // HIGH: build council task command, 82B
    constexpr uintptr_t COUNCIL_PROVINCE_TASK      = 0x05b86ef0;  // HIGH: enumerate valid provinces for task, 3105B

    // Round 10: Council config/instance accessors
    constexpr uintptr_t GET_COUNCIL_CONFIG         = 0x0667baf0;  // HIGH: FUN_0677baf0 - 0x100000. fn(char_ptr) -> ruler_data+0x4F0, 372B
    constexpr uintptr_t GET_COUNCIL_INSTANCE       = 0x0667bdf0;  // HIGH: FUN_0677bdf0 - 0x100000. fn(char_ptr) -> *(ruler_data+0x4E8), 314B
    constexpr uintptr_t IS_COUNCILLOR              = 0x04bb5450;  // CONFIRMED: fn(char_id, char_ptr) -> bool. Binary search on GetCouncilConfig+0x40/0x4C
    constexpr uintptr_t GET_HELD_TITLES_ARRAY      = 0x0574f840;  // CONFIRMED: fn(char_ptr) -> landed_data+0x1E0 container. 20 callers. Held title ID array (NOT council!)
    // NOTE: Previously mislabeled as GET_COUNCIL_POSITION_REFS. RE shows this is GetTitleArray (R8V, R7-6).

    // Round 6: Law lookup by name (MurmurHash3)
    // BROKEN: 0x06c332c0 points into destructor FUN_06d331b0, NOT a lookup function.
    // Calling this as fn(db, name, len) executes destructor code → SIGSEGV.
    // Use direct array walk (realm_law_db::LAW_PTR_ARRAY) instead.
    constexpr uintptr_t LAW_LOOKUP_BY_NAME         = 0x06c332c0;  // BROKEN — see comment above

    // Round 6: Culture/faith name accessors
    constexpr uintptr_t CULTURE_GET_NAME           = 0x05c188b0;  // HIGH: fn(culture_obj) -> name ptr, 8B trivial
    constexpr uintptr_t CULTURE_GET_ID             = 0x05c2c530;  // HIGH: fn(game_state, culture_obj) -> culture_id, 58B

    // Round 13: Culture innovation accessors
    constexpr uintptr_t CULTURE_GET_KNOWN_INNOVATIONS = 0x05c208c0; // HIGH: fn(culture_obj) -> CPdxArray<CCultureInnovation*>*, 8B trivial (ck-e58l.20)
    constexpr uintptr_t CULTURE_ADD_KNOWN_INNOVATION  = 0x05c208e0; // HIGH: fn(culture_obj, innovation_type_ptr), adds to known list (ck-e58l.20)
    constexpr uintptr_t CULTURE_GET_DISPLAY_NAME      = 0x05c188b0; // HIGH: fn(culture_obj) -> std::string* (display name) (ck-e58l.20)
    constexpr uintptr_t INNOVATION_CAN_GAIN_PROGRESS  = 0x06b5cc60; // HIGH: fn(innovation_type, culture, 0) -> bool (ck-e58l.20)
    constexpr uintptr_t INNOVATION_LINEAR_SEARCH      = 0x03f55840; // HIGH: fn(begin, end, target) -> iterator (4x unrolled) (ck-e58l.20)
    constexpr uintptr_t INNOVATION_READINIT           = 0x06b5c5a0; // HIGH: CCultureInnovationType ReadInit parser, 1346B (ck-e58l.20)
    constexpr uintptr_t INNOVATION_VALIDATE           = 0x06b5cb50; // HIGH: CCultureInnovationType PostReadInit validation (ck-e58l.20)

    // Round 14: Innovation progress functions
    constexpr uintptr_t ADD_SPREAD_PROGRESS           = 0x06b5fef0; // HIGH: fn(innov_inst, amount) adds to inst+0x18, caps at 10M (ck-e58l.20)
    constexpr uintptr_t ADD_FASCINATION_PROGRESS      = 0x06b54a10; // HIGH: fn(era_entry, amount) adds to era+0x28, caps at 10M (ck-e58l.20)
    constexpr uintptr_t CULTURE_ADD_INNOV_PROGRESS    = 0x05c20a70; // HIGH: fn(culture, innov_type, amount) indexes culture+0x5D0 (ck-e58l.20)
    constexpr uintptr_t CULTURE_ADD_FASCIN_PROGRESS   = 0x05c20a90; // HIGH: fn(culture, amount, year) adds fascination via era array (ck-e58l.20)
    constexpr uintptr_t ON_INNOVATION_COMPLETED       = 0x05c1ff10; // HIGH: fn(culture, innov_inst, flag) moves to known list (ck-e58l.20)
    constexpr uintptr_t MONTHLY_INNOVATION_TICK       = 0x065f4ea0; // HIGH: fn(tick_ctx, date_ptr) main monthly handler (ck-e58l.20)
    constexpr uintptr_t CALC_SPREAD_PROGRESS          = 0x06b601f0; // HIGH: fn(innov_inst, debug) -> bool, research speed formula (ck-e58l.20)
    constexpr uintptr_t INNOVATION_GET_TYPE           = 0x06b5e500; // HIGH: fn(innov_inst) -> CCultureInnovationType*, 8B trivial (ck-e58l.20)

    // Round 13: Faith system functions
    constexpr uintptr_t FAITH_ADD_DOCTRINE            = 0x051ac290; // HIGH: AddDoctrine(faith, doctrine), logs "already has doctrine" (ck-e58l.20)
    constexpr uintptr_t FAITH_REMOVE_DOCTRINE         = 0x051acc20; // HIGH: RemoveDoctrine(faith, doctrine), logs "doesn't have" (ck-e58l.20)
    constexpr uintptr_t FAITH_LOOKUP_BY_NAME          = 0x06278ef0; // HIGH: faith name lookup, 55B (ck-e58l.20)
    constexpr uintptr_t CMD_ADD_DOCTRINE              = 0x04122c30; // HIGH: add_doctrine console handler (ck-e58l.20)
    constexpr uintptr_t CMD_CHANGE_FERVOR             = 0x041244a0; // HIGH: change_fervor console handler (ck-e58l.20)
    constexpr uintptr_t CHANGE_FERVOR                = 0x05ea8820; // HIGH: ChangeFervor(faith, delta, ctx) — R14 decompile-verified (ck-e58l.20)
    constexpr uintptr_t GET_CHAR_FAITH_ID            = 0x06656460; // HIGH: trivial getter → char+0xB4 (FAITH_ID_DIRECT) — R14 decompile (ck-e58l.20)
    constexpr uintptr_t GET_DOCTRINE_MODIFIER        = 0x05eb7f70; // MED: GetDoctrineModifier(container, key, default) — R14 decompile context (ck-e58l.20)

    // Faith reform / doctrine-change write path (re_faith_reform_doctrine.md)
    // set_doctrine is the implementable "change tenets/doctrines" play. The
    // VALIDATED add path is FAITH_ADD_DOCTRINE above; the raw container-add and
    // the doctrine-by-name DB layout are pinned here. A clean doctrine SWAP =
    // FAITH_REMOVE_DOCTRINE(old) then FAITH_ADD_DOCTRINE(new) (AddDoctrine only
    // rejects when the group is FULL; it does not evict the conflicting member).
    // No piety/fervor charge is enforced natively — pre-checks are bridge-side.
    constexpr uintptr_t FAITH_ADD_DOCTRINE_TO_CONTAINER = 0x05ea80a0; // HIGH: raw add — push(faith+0x278,doctrine)+recalc, NO validation (re_faith_reform_doctrine)
    constexpr uintptr_t FAITH_DOCTRINE_CONTAINER_PUSH   = 0x05eb8d40; // HIGH: CPdxArray push-back(container@faith+0x278, CFaithDoctrine*) (re_faith_reform_doctrine)
    constexpr uintptr_t FAITH_CONTAINER_HAS_DOCTRINE    = 0x05eb8120; // HIGH: bool(faith+0x278, int doctrine_id) — membership check (re_faith_reform_doctrine)
    constexpr uintptr_t ADD_DOCTRINE_CONSOLE_PARSER     = 0x04123070; // HIGH: name→object resolver used by CMD_ADD_DOCTRINE (re_faith_reform_doctrine)
    // FAITH_DOCTRINE_DB (0x0bfdaf88) name-lookup layout (MurmurHash3_32, NOT the
    // id-indexed ad_db_lookup layout): table@db+0x1b0, mask@db+0x1bc (hash&mask),
    // entry stride 0x18, hash@entry+8, doctrine ptr@entry+0x10 (when entry+4!=0xFF),
    // null sentinel = FAITH_DOCTRINE_DB+8 (_DAT_0bfdaf90). HIGH (decompile-verified).
    constexpr size_t FAITH_DOCTRINE_DB_TABLE_PTR   = 0x1b0;  // HIGH: ptr to hash table
    constexpr size_t FAITH_DOCTRINE_DB_MASK        = 0x1bc;  // HIGH: u32 hash mask
    constexpr size_t FAITH_DOCTRINE_DB_ENTRY_STRIDE = 0x18;  // HIGH
    constexpr size_t FAITH_DOCTRINE_DB_ENTRY_HASH  = 0x08;   // HIGH: u32 murmur hash in entry
    constexpr size_t FAITH_DOCTRINE_DB_ENTRY_PTR   = 0x10;   // HIGH: CFaithDoctrine* in entry
    // CCreateFaithCommand (full custom-faith reform) — RESOLVED via LIVE process
    // (PID 858006) /proc/mem read of the runtime-relocated .data.rel.ro vtables.
    // The static binary zero-fills RELRO so these were previously BLOCKED; reading
    // the resolved slots from the running game unblocks the pins. RTTI:
    // 19CCreateFaithCommand. Raw offset = (resolved_ptr - load_base); ghidra VA =
    // raw + 0x100000. Command object is 0x460 bytes (operator new(0x460)). See
    // re_faith_create_command.md for the full SCreateFaithParameters layout + the
    // feasibility verdict (BLOCKED on params fabrication, NOT on the Execute pin).
    constexpr uintptr_t CREATE_FAITH_CMD_VTABLE        = 0x0bc98eb0; // HIGH: primary vtable (CCreateFaithCommand), live-read; stored at cmd+0x00
    constexpr uintptr_t CREATE_FAITH_CMD_HELPER_VTABLE = 0x0bc98f60; // HIGH: CGameCommandHelper<> sub-object vtable, live-read; stored at cmd+0x18
    constexpr uintptr_t CREATE_FAITH_CMD_CTOR          = 0x066ce1c0; // HIGH: ctor(cmd, actor_id_u32, params_src_ptr) — sets vtables, cmd+0x20=actor, copy params into cmd+0x28
    constexpr uintptr_t CREATE_FAITH_CMD_VALIDATE      = 0x066ced70; // HIGH: CanExecute/Validate(cmd, sync_ctx)->bool — resolves actor from g_CharacterDB, checks eligibility + (if cmd+0x28 flag) gold/piety cost; safe to call for honest reject
    constexpr uintptr_t CREATE_FAITH_CMD_COST_CALC     = 0x066ceee0; // MED: cost(cmd, 0)->int (piety/fervor breakdown) used by Validate
    constexpr uintptr_t CREATE_FAITH_PARAMS_CTOR       = 0x05fa5340; // HIGH: SCreateFaithParameters default ctor (zeroes 11 strings, doctrine vec, holy-site id) — embedded at cmd+0x28
    constexpr uintptr_t CREATE_FAITH_PARAMS_COPY_CTOR  = 0x05fadad0; // HIGH: SCreateFaithParameters copy ctor (deep-copies all fields from src)
    // CCreateFaithCommand object layout (0x460 bytes):
    constexpr size_t CREATE_FAITH_CMD_ACTOR_ID         = 0x20;   // HIGH: u32 founder/actor character full id (char+0x18)
    constexpr size_t CREATE_FAITH_CMD_PARAMS           = 0x28;   // HIGH: embedded SCreateFaithParameters (serialized as a 0xf0-tag blob)
    // SCreateFaithParameters field offsets (relative to params base = cmd+0x28):
    constexpr size_t CF_PARAMS_STR0                    = 0x08;   // HIGH: std::string (faith tag/key); +0x10=len. 11 std::strings total at strides below
    constexpr size_t CF_PARAMS_STR1                    = 0x30;   // HIGH: std::string (name/adjective set)
    constexpr size_t CF_PARAMS_STR2                    = 0x60;   // HIGH
    constexpr size_t CF_PARAMS_STR3                    = 0x90;   // HIGH
    constexpr size_t CF_PARAMS_STR4                    = 0xc0;   // HIGH
    constexpr size_t CF_PARAMS_STR5                    = 0xf0;   // HIGH
    constexpr size_t CF_PARAMS_STR6                    = 0x118;  // HIGH
    constexpr size_t CF_PARAMS_STR7                    = 0x138;  // HIGH
    constexpr size_t CF_PARAMS_STR8                    = 0x158;  // HIGH
    constexpr size_t CF_PARAMS_STR9                    = 0x178;  // HIGH
    constexpr size_t CF_PARAMS_STR10                   = 0x198;  // HIGH
    constexpr size_t CF_PARAMS_TEMPLATE_OR_SITE_ID     = 0x1b8;  // MED: u32-pair / id at +0x1b8/+0x1c0 (holy-site or template ref), default 0xffffffff at +0x1b8
    constexpr size_t CF_PARAMS_DOCTRINE_VEC_BEGIN      = 0x1d0;  // HIGH: CFaithDoctrine* vector .begin (ptr)
    constexpr size_t CF_PARAMS_DOCTRINE_VEC_COUNT      = 0x1dc;  // HIGH: int element count (one selected doctrine per group)
    constexpr size_t CF_PARAMS_SUBOBJ_1E8              = 0x1e8;  // MED: sub-container (FUN_05faf880)
    constexpr size_t CF_PARAMS_SUBOBJ_420              = 0x420;  // MED: sub-container (FUN_05fb00c0)

    // Round 6: Character name accessors
    constexpr uintptr_t CHAR_DISPLAY_NAME          = 0x06659c30;  // HIGH: fn(output, char, format, 1), 1364B
    // Round 7 track2: Core first-name getter with localization resolution
    // Signature: std::string* fn(std::string* output, CCharacter* char_ptr)
    // Handles override check (char+0x48) AND loc key resolution via localization DB
    // Safe to call from game thread (read-only)
    constexpr uintptr_t CHAR_FIRST_NAME_RESOLVED   = 0x0665b4c0;  // HIGH: FUN_0675b4c0 - 0x100000. 258B, resolves loc keys to display names
    constexpr uintptr_t LOC_KEY_CHECK              = 0x07ed3810;  // HIGH: FUN_07fd3810 - 0x100000. bool fn(string)
    constexpr uintptr_t LOC_RESOLVE                = 0x07ebc5e0;  // HIGH: FUN_07fbc5e0 - 0x100000. fn(key) -> display string

    // Round 6: Build CSendCharacterInteractionCommand from buffer
    // Ghidra-verified: FUN_0619b410 (Ghidra VA), 117 bytes (0x0619b410-0x0619b485).
    // Constructor: base_ctor(FUN_0765b630) + vtable 0x0bc73e38 + secondary vtable 0x0bc73ee8
    //   + deep_copy(FUN_06b3d770) + hash writes to [0x358],[0x360].
    // Called by FUN_04dc4540 (AI interaction core), FUN_04e55c10 (war exec), 25 callers total.
    // NOTE: 0x0619b410 - 0x100000 = 0x0609b410 (NOT 0x0519b410 — that was an arithmetic error).
    constexpr uintptr_t BUILD_SEND_CMD_FROM_BUF    = 0x0609b410;  // FUN_0619b410 - 0x100000

    // Round 6b: Army system functions
    constexpr uintptr_t ARMY_DISBAND               = 0x065891b0;  // HIGH: army disband core (4248B, full validation)
    constexpr uintptr_t ARMY_RAISE_ALL             = 0x03de3e40;  // HIGH: RaiseAllArmiesCore(msg_sink, mode); 2742B. Reads g_PlayerCharacterID ONCE at entry via GET_PLAYER_CHAR_ID, resolves char via CharacterDB, checks *(char+0x18)==full-32-bit-handle else falls to NullCharacter sentinel (safe no-op). mode==0 + msg_sink==null = full raise via ARMY_RAISE_INNER, skips all UI-message branches. A8.2 global-swap target (re_raise_armies.md)
    constexpr uintptr_t ARMY_RAISE_INNER           = 0x03ee0210;  // HIGH: FUN_03ee0210, 305B — the actual raise-all command construction invoked by RaiseAllArmiesCore on the mode==0 path; ALSO reads g_PlayerCharacterID once at entry (same full-32-bit check), so the global swap must span the whole RaiseAllArmiesCore call. No loops/recursion — bounded. (re_raise_armies.md)
    constexpr uintptr_t ARMY_RAISE_ALL_WRAPPER     = 0x03de4b20;  // HIGH: raise all wrapper (UI handler, 295B)
    // #96 Norse raiding (re_raid_system.md): the player-driven "toggle raiding on
    // all eligible armies" dispatch path. Header 0x03de36d0 (IMAGE 0x03ee36d0,
    // FUN_03ee36d0, 1605B). Reads g_PlayerCharacterID via GET_PLAYER_CHAR_ID,
    // resolves the player's army container (FUN_068c9ac0 → +0x158 array / +0x164
    // count), iterates armies filtering raid-eligible ones (FUN_0634c1e0) into a
    // std::vector, builds a CToggleRaidingCommand (FUN_06354710; helper vtable
    // UNK_0bcc95d0, behaviour vtable UNK_0bcc9680), validates it
    // (CToggleRaidingCommand::IsValid / FUN_06354e10), and ONLY-IF-VALID enqueues
    // it via GameCommandDispatch(&g_CommandDispatchManager, cmd, 0xe, 0) →
    // CommandQueuePush (non-blocking; the engine flips army+0x1b4 natively on its
    // own validated/RNG-safe order-apply tick). Takes a single int param = the
    // raid INTENT index (0 = default toggle-raiding; the iStack_12c==2 branch
    // takes a different path, so pass 0). Bounded: one loop over the realm's
    // armies, validator-gated dispatch — safe no-op for an ineligible ruler.
    // A8.3 global-swap target: swap g_PlayerCharacterID → AI handle, call(0),
    // restore. (re_raid_system.md, #96)
    constexpr uintptr_t ARMY_TOGGLE_RAIDING_DISPATCH = 0x03de36d0; // HIGH: raise-all-raiders / toggle-raiding command dispatch (player-global driven, validator-gated)
    constexpr uintptr_t ARMY_IS_RETREATING         = 0x05abe070;  // HIGH: bool(army) — return *(army+0x16c)!=0, 11B
    constexpr uintptr_t ARMY_IN_COMBAT             = 0x05ac2dd0;  // HIGH: InCombat check, 67B
    constexpr uintptr_t ARMY_GET_OWNER_CHAR_ID     = 0x05ac2e20;  // HIGH: GetOwnerCharID, 68B
    constexpr uintptr_t ARMY_GET_COMMANDER         = 0x05ac4cc0;  // HIGH: GetCommander, 66B
    constexpr uintptr_t ARMY_ENUM_SUB              = 0x05f61870;  // HIGH: fn(GAME_STATE_ROOT) → sub-object with army ID array
    constexpr uintptr_t ARMY_CHECK_ENEMIES_NEARBY  = 0x065d55a0;  // HIGH: check enemies nearby, 317B
    // L2 RE (this session): FUN_0634e600 is NOT a mobilizer — it is a read-only
    // UI-availability boolean predicate (raise-button enable). Args:
    // (CCharacter*, listData, holdingsList[+0xc=count], mode, out_vecA, out_vecB);
    // out vecs null => no mutation. Recorded as a predicate, never call as a raise.
    constexpr uintptr_t ARMY_CAN_RAISE_PREDICATE   = 0x0624e600;  // HIGH: read-only bool predicate (UI raise-button enable), NOT a mobilizer
    constexpr uintptr_t REALM_HOLDINGS_LIST_FOR_CHAR = 0x0624b300; // HIGH: FUN_0634b300(charID)->realmObj+0x158 holdings-list, fallback &DAT_0c42cc10
    constexpr size_t    REALM_HOLDINGS_VEC_OFF     = 0x158;       // HIGH: realmObj+0x158 holdings/regiment-id vector ({data @ +0x0, count @ +0xc})
    constexpr uintptr_t ARMY_CHECK_WAR_LEADER      = 0x065d5320;  // HIGH: check if war leader, 159B
    constexpr uintptr_t ARMY_CHECK_HOSTILE_AREA    = 0x065d5b60;  // HIGH: check hostile area, 151B

    // #96 Norse raiding — CArmy raid-state fields + validity predicates
    // (re_raid_system.md, live-verified in core dump). Army-relative offsets
    // (ARMY_DB=0x0bed7670). The toggle dispatch (ARMY_TOGGLE_RAIDING_DISPATCH
    // above) flips ARMY_IS_RAIDING natively; these are for handler-side readback.
    // ARMY_LOCATION_PROV (0xec) — REMOVED 2026-09-13 (ck-meu5). REFUTED.
    // 0xec is army_obj::OWNER_LINK_ID and always was: across 32 live armies in
    // william_1066_baseline.core its value is SEQUENTIAL WITH THE ARMY'S OWN DB
    // INDEX (0x0,0x1,...,0x1000005,...), (v & 0xffffff) == index for 32/32.
    // A province index cannot also be a per-army sequence number. do_query_armies
    // reads this byte as the owner link, resolves it through OWNER_LINK_DB and
    // filters by owner correctly — that handler is the live proof.
    // There is NO confirmed army-location offset. See re/findings/re_army_location.md.
    constexpr size_t    ARMY_IS_RAIDING            = 0x1b4;       // HIGH (live): int8 bool — army is raiding
    constexpr size_t    ARMY_TARGET_PROV           = 0x1c8;       // HIGH (live+decomp): uint32 raid target province (0xFFFFFFFF=none)
    constexpr size_t    ARMY_IS_COUNTER_RAIDING    = 0x1cc;       // HIGH (live): int8 bool — army is counter-raiding (excl. with +0x1b4)
    constexpr size_t    ARMY_RAID_PROGRESS_DATE    = 0x1c0;       // MEDIUM: int64 raid-progress/loot date
    constexpr uintptr_t FN_TOGGLE_RAID_IS_VALID    = 0x06241bc0;  // HIGH: CToggleRaidingCommand::IsValid(self, CCharacter*, errbuf) — ruler raid eligibility (header; IMAGE 0x06341bc0)
    constexpr uintptr_t FN_ARMY_START_RAID_VALIDATE= 0x062491a0;  // HIGH: army-vs-province "can start raid here" (header; IMAGE 0x063491a0)
    constexpr uintptr_t FN_RAID_TARGET_IS_LEGAL    = 0x06249bd0;  // HIGH: char-vs-province "is legal raid target" (header; IMAGE 0x06349bd0)

    // Round 6b: Council task functions
    // SIGNATURE RESOLVED (decompile 20260612): bool SetCouncilTask(council_instance*, uint32 task_type_id, councillor_handle).
    // From CSetCouncilTaskEffect::Execute call site: council = FUN_05b67e70(realm/gamestate)
    // (auto-position) or position-resolved via FUN_04ef23d0; task_type_id = taskobj+0x18;
    // councillor_handle = packed char ref via FUN_081f11a0. (2nd SetCouncilTask exists @ raw 0x677df20.)
    // RESOLVED 20260612: SET_COUNCIL_TASK @ 0x05ab5b00 is NOT a setter — it is an
    // availability CHECK: bool(council*, task_id, councillor_handle) doing std::find
    // over the councillor's allowed-task list (no store). The real mutator is
    // FUN_0687dde0 (raw 0x577df20) which builds a descriptor internally, or the
    // scripted effect CSetCouncilTaskEffect::Execute(effect_obj, scope). Native
    // assignment needs effect_obj (+0x50 councillor charref, +0x54 position int,
    // -1=auto) + a scope object carrying the task type → object construction =
    // crash risk; deferred (re_council_task_setter_20260612.md). Use console for
    // claim fab (add_claim) meanwhile.
    // Council-task TARGET-TYPE discriminator (re_council_task_targets_20260613):
    // *(CCouncilTaskType + 0x38) int32 = 0 none / 1 CHARACTER / 2 TITLE target.
    // fabricate_claim, increase_control, conversion are all 1 (CHARACTER). The
    // command-build (FUN_03ff3930 raw 0x3ef3930) for disc 1 needs a CHARACTER
    // scope; BUILD_TARGET_SCOPE makes only TITLE scopes → targeted tasks reject.
    // Validity oracle: FUN_05cb80e0(tasktype, scope_buf, 0)->bool (Ghidra VA
    // 0x5cb80e0 → raw 0x5bb80e0). Returns 1 if the target passes the task's
    // data-driven potential(+0x630)+valid(+0x710) script triggers.
    // CANONICAL CALL (from CHANGE_COUNCIL_TASK→FUN_03ff3930, verified 20260613):
    //   uVar3 = *(u32*)(position + 0x3c);                       // councillor context B
    //   FUN_05c84350(scope_buf/*32B*/, uVar3, *(u32*)(position+0x38), target_scope)
    //   valid = FUN_05cb80e0(tasktype, scope_buf, 0);
    // where target_scope = BUILD_TARGET_SCOPE(&{i16 8; u32 county_title_id}).
    // scope_buf layout: +0=*(pos+0x38) +4=*(pos+0x3c) +8=packed title handle +0x18=0.
    constexpr uintptr_t TASKTYPE_TARGET_DISC       = 0x38;        // int32 on CCouncilTaskType
    constexpr uintptr_t COUNCIL_TASK_TARGET_VALIDATE = 0x05bb80e0;// FUN_05cb80e0(tasktype,scope_buf,0)->bool (VA 0x5cb80e0-0x100000)
    constexpr uintptr_t COUNCIL_TASK_SCOPE_PREP    = 0x05b84350;  // FUN_05c84350(out32,ctxB,ctxA,target_scope) builds eval scope (VA 0x5c84350-0x100000)
    constexpr uintptr_t COUNCIL_POS_CTX_A          = 0x38;        // u32 on position instance → scope_buf[0]
    constexpr uintptr_t COUNCIL_POS_CTX_B          = 0x3c;        // u32 on position instance → scope_buf[1] (via FUN_05cc86b0)
    constexpr uintptr_t SET_COUNCIL_TASK           = 0x05ab5b00;  // HIGH: task-AVAILABILITY check (not a setter) — corrected 20260612
    constexpr uintptr_t GET_COUNCIL_DATA           = 0x0667bdf0;  // HIGH: GetCouncilInstance(ruler) → *(realm_data+0x4E8) — verified 20260612
    constexpr uintptr_t COUNCIL_TASK_EXECUTE       = 0x04df21f0;  // HIGH: CSetCouncilTaskEffect::Execute, 446B

    // ═══════════════════════════════════════════════════════════════════════
    // COUNCIL-TASK ASSIGNMENT — ⚠ DOES NOT APPLY FOR AI (corrected 20260612)
    // ═══════════════════════════════════════════════════════════════════════
    // ⚠ CHANGE_COUNCIL_TASK below builds CChangeCouncilTaskCommand and dispatches
    // via GAME_COMMAND_DISPATCH(0xe) — the PLAYER-COMMAND QUEUE. It dispatches
    // cleanly but NEVER mutates the task for AI rulers (same player-gate as the
    // building CAddConstructionCommand path). Unlike building there is NO
    // standalone leaf to call instead — the real store (SetCouncilTask v2
    // 0x577df20 / FUN_0688cb20 / CSetCouncilTaskEffect::Execute 0x04df21f0)
    // consumes a constructed command/effect object. Full call graph + the two
    // unlock options (local-player spoof vs object construction) in
    // re/findings/re_council_apply_callgraph_20260612.md. The signature below is
    // accurate for the (non-applying) command builder:
    //   void ChangeCouncilTask(CCharacter* ruler,
    //                          void* position_instance,   // g_CouncilPositionDB entry for the task's slot
    //                          void* task_type_obj,       // from CCouncilTaskTypeDatabase (see below)
    //                          void* target_scope)        // FUN_05f63380(&{i16 type; u32 id@+8}); 0-type → NO_TARGET_SCOPE
    //   - position id = FUN_067e2b20(ruler, *(task_type+0x30), 0); entry = COUNCIL_POSITION_DB[id]
    //   - command advances RNG → MUST wrap in arm_fake_random_scope (like war/focus)
    // Task-type object layout: name std::string @ +0x10, position-type ref @ +0x30,
    //   TARGET-MODE discriminator @ +0x38: 0 = NO target (taskless: foreign/domestic
    //     affairs, justify_rule), 1 = single target via the eval-predicate path
    //     (task_fabricate_claim: +0x3c==1), 2 = title target (conversion/integrate/
    //     develop/increase_control: +0x3c==2). A task with disc != 0 is REJECTED by
    //     FUN_05cb80e0 (target validate, called BEFORE the councillor check) unless a
    //     valid target scope is supplied — and that early reject writes NO error
    //     string. (Corrected 20260622 #118 — was mislabelled "command vs effect path".)
    // ── Building construction (CAddConstructionCommand) — dispatch path RESOLVED 20260612 ──
    // Command (0x30B): +0x20 actor id, +0x24 holding/province id, +0x28 slot,
    // +0x2c building-type index (= ordinal in BUILDING_TYPE_DB @ 0xbedf588).
    // Build via ADD_CONSTRUCTION_CTOR then dispatch (*cmd[0][9])(&out,cmd) +
    // GAME_COMMAND_DISPATCH(mgr,&out,0xe,0); validate first with ADD_CONSTRUCTION_VALIDITY.
    // Dispatch advances RNG → wrap in arm_fake_random_scope. See
    // re/findings/re_building_construction.md (DISPATCH PATH RESOLVED).
    // ── NON-GATED construction builder (via CAddBuildingEffect apply) — RESOLVED 20260612 (DC35-40) ──
    // The scripted-effect path the game AI uses; NO player gate (unlike the UI command).
    //   province = CONSTRUCTION_SCOPE_TO_PROVINCE(&{i16 type=8; u32 title_id@+8})
    //   CONSTRUCTION_START(province+0x1F8 /*slot container*/, CBuildingType*, 0)
    // sets the holding's under-construction building at (slot_container+0x60) +
    // registers with gamestate; building is FREE (effect path, no cost). Verify by
    // reading *(slot_container+0x60) == the CBuildingType*. Wrap in arm_fake_random_scope.
    // CBuildingType category @ +0x1cb58: 0=normal, 1=duchy-capital (uses CONSTRUCTION_START_DUCHY),
    // 2=special. See re/findings/re_building_construction.md.
    // ⚠ CORRECTED 20260612: these 0x611xxxx-region funcs were mis-derived as
    // Ghidra-VA − 0x100000 = 0x511xxxx (ARITHMETIC ERROR; 0x611xxxx − 0x100000 =
    // 0x601xxxx). The wrong 0x511xxxx addresses pointed at garbage mid-function →
    // do_construct_building hard-crashed (the entire "execution-frame assert"
    // saga was a wrong-address bug; the functions/data were correct). Verified by
    // byte-matching the Ghidra entry bytes against runtime g_base+offset.
    constexpr uintptr_t CONSTRUCTION_START         = 0x0601ffa0;  // HIGH: FUN_0611ffa0(slot_container, CBuildingType*, 0) — SPECIAL-building start (cat 2)
    constexpr uintptr_t CONSTRUCTION_SCOPE_TO_PROVINCE = 0x05e63e90;  // HIGH: FUN_05f63e90({i16 8; u32 id@+8})→province (indexes PROVINCE array — needs province id, NOT title id)
    constexpr uintptr_t CONSTRUCTION_PRECHECK      = 0x0601d430;  // HIGH: FUN_0611d430(slot_container, CBuildingType*)→already-present bool
    // NORMAL-building (category 0) start sequence — what CAddBuildingEffect's apply
    // (FUN_05240920) does for cat-0 buildings (building+0x1cb58: 0=normal,1=duchy,2=special):
    constexpr uintptr_t CONSTRUCTION_FIND_SLOT     = 0x0601cbd0;  // HIGH: FUN_0611cbd0(slot_container, CBuildingType*)→slot idx by family (-1 none)
    constexpr uintptr_t CONSTRUCTION_FIND_SLOT_FB  = 0x0601c870;  // HIGH: FUN_0611c870(slot_container)→first free slot idx (-1)
    constexpr uintptr_t CONSTRUCTION_FINALIZE_PEND = 0x0601d090;  // HIGH: FUN_0611d090(slot_container) — finalize pending before new
    constexpr uintptr_t CONSTRUCTION_START_NORMAL  = 0x0601d9b0;  // HIGH: FUN_0611d9b0(slot,building,slot_idx,-1)→FUN_0611da40 normal starter
    constexpr uintptr_t CONSTRUCTION_ADVANCE       = 0x0601e610;  // HIGH: FUN_0611e610(slot_container, 1) — advance/place after start
    constexpr uintptr_t CONSTRUCTION_START_DUCHY   = 0x060201e0;  // HIGH: FUN_061201e0(slot,building) duchy-capital (VA 0x61201e0 - 0x100000)
    constexpr size_t    BUILDING_CATEGORY_OFF      = 0x1cb58;     // int on CBuildingType: 0=normal,1=duchy-capital,2=special

    constexpr uintptr_t ADD_CONSTRUCTION_CTOR      = 0x06033610;  // HIGH: FUN_06133610 CAddConstructionCommand ctor(cmd,actor,loc,slot,bt_ref)
    constexpr uintptr_t ADD_CONSTRUCTION_VALIDITY  = 0x06033800;  // HIGH: FUN_06133800 can-build check (safe, no mutation)
    // #133 build_gate — the REAL buildability conjunction (player-gated wrapper FUN_05c7f110
    // tail-calls FUN_05c7fe70 which bit-43-gates AI actors). BUILDING_OWNERSHIP_CHECK
    // (FUN_05c7ea00) has a ctx-free success return (LAB_05c7efe9→1) and is the AI-safe owner/
    // realm predicate. ALL header = image - 0x100000 (func_at-verified image VAs noted; the
    // recurring +0x100000 bug — spec table had two wrong). MEDIUM until live crash-validated;
    // the engine-call gate is OPT-IN (engine_gate=true), fail-closed, defaulting OFF.
    constexpr uintptr_t BUILDING_BUILDABLE_FULL    = 0x05c7f110;  // image 0x5c7f110 — full conjunction (player-gated; do NOT call for AI)
    constexpr uintptr_t BUILDING_OWNERSHIP_CHECK   = 0x04c7ea00;  // image 0x5c7ea00 — owner/realm buildability(actor_full_id, key, ctx); ctx-free 1 on allowed
    constexpr uintptr_t BUILDING_SLOT_HAS_INDEX    = 0x0620c670;  // image 0x630c670 — *(holding+0x31c)!=-1 (slot present); trivial — inlined as a pure read
    constexpr uintptr_t BUILDING_CONFLICT_CHECK    = 0x06233200;  // image 0x6333200 — conflicting/duplicate building present
    constexpr uintptr_t BUILDING_EXTRACT_KEY       = 0x073ebe90;  // image 0x74ebe90 — *(building_type+0x10) accessor (FUN_074ebe90(bt+8))
    constexpr uintptr_t GAMESTATE_TITLE_SCOPE      = 0x0622cc20;  // image 0x632cc20 — FUN_0632cc20(gsr, key) holding scope
    constexpr uintptr_t GAMESTATE_ROOT_GETTER      = 0x05f61760;  // image 0x6061760 — FUN_06061760(g_GameStateRoot)
    constexpr size_t    HOLDING_SLOT_INDEX_OFF     = 0x31c;       // int32 on holding: slot index (-1 = no valid slot)
    constexpr uintptr_t ADD_CONSTRUCTION_DISPATCH  = 0x046f5570;  // HIGH: FUN_047f5570 build+dispatch wrapper (player-actor; replicate w/ explicit actor)

    constexpr uintptr_t CHANGE_COUNCIL_TASK        = 0x03ef3a70;  // HIGH: FUN_03ff3a70 — single-call council-task setter (ruler,pos,tasktype,target)
    constexpr uintptr_t COUNCIL_POSITION_RESOLVE   = 0x067d2b20;  // HIGH: FUN_067e2b20(ruler,postype_ref,0)→position id (ruler+0x1b8 council)
    constexpr uintptr_t BUILD_TARGET_SCOPE         = 0x05e63380;  // HIGH: FUN_05f63380(&{i16 type@0; u32 id@+8})→scope obj; type!=8/4 returns NO_TARGET_SCOPE (corrected: VA 0x5f63380 - 0x100000)
    constexpr uintptr_t COUNCIL_TASK_CMD_BUILD     = 0x05c83370;  // HIGH: FUN_05c83370 CChangeCouncilTaskCommand ctor (called by CHANGE_COUNCIL_TASK)
    // ── DIRECT-APPLY path (20260614) — bypasses the player command queue ──
    // The council-task command's vtable Execute slot (vt[7] = FUN_05c83430,
    // runtime-resolved from cmd vtable 0xbbcc240). It reads the command FIELDS
    // directly (NO command vtable needed) and applies synchronously:
    //   pos  = CouncilPositionDB[*(cmd+0x20) /*pid*/]; validity (pos+8 vtable+0x10)
    //   ok   = COUNCIL_TASK_TARGET_VALIDATE(*(cmd+0x28)=tasktype, cmd+0x30=scope, 0)
    //   ruler= CharacterDB[*(cmd+0x34)]; councillor = CharacterDB[*(cmd+0x30)]
    //   FUN_05c85b40(ruler, councillor, tasktype, &errstr)  // validate+apply
    //     → FUN_05cb7c40(tasktype, FUN_05c84330(ruler_id,councillor_id)) = STORE
    // Command field layout (build a 0x50 buffer, no vtable):
    //   +0x20 u32 position id (= *(position_instance + COUNCIL_POS_KEY 0x10))
    //   +0x28 u64 tasktype ptr
    //   +0x30 .. +0x50  scope buffer from COUNCIL_TASK_SCOPE_PREP(buf, ctxB, ctxA, target_scope)
    // param_2 (context) = a libstdc++ std::string for the rejection reason (init empty).
    // This is the AI/effect-equivalent store — NOT the player-gated queue path, so
    // it APPLIES for AI rulers in an observer game (the gate-byte dispatch did not).
    constexpr uintptr_t COUNCIL_TASK_CMD_EXECUTE   = 0x05b83430;  // HIGH: FUN_05c83430 cmd Validate/CanAssign (VA 0x5c83430-0x100000); returns bool + reject std::string. NOT a store.
    // ── REAL STORE (20260614) — the command's secondary-vtable apply slot path ──
    // The command's Execute (secondary vtable 0xbbcc2f0 slot 1 = FUN_05c83400)
    // calls FUN_05cc8660(mgr/*ignored*/, pos_id, scope, tasktype) →
    // FUN_05cc9970(position, scope, tasktype) which WRITES *(position+0x18)=tasktype
    // (the field query_council reads) + copies the scope (ctxA/ctxB/target) into
    // position+0x38.. then finalizes (FUN_05cc5e10/FUN_05ccb030/FUN_05cc9c80).
    // NOT player-gated, NOT queued — applies synchronously to the position.
    // Call: FUN_05cc8660(nullptr, pos_id, &scope_buf, tasktype) after the Validate
    // (FUN_05c83430) passes. scope_buf = COUNCIL_TASK_SCOPE_PREP(buf, ctxB, ctxA, target).
    constexpr uintptr_t COUNCIL_TASK_STORE         = 0x05bc8660;  // HIGH: FUN_05cc8660 (VA 0x5cc8660-0x100000) → FUN_05cc9970 store

    // ── PER-RULER COUNCIL-AI re-assignment routine (L20 stickiness target) ──
    // FUN_04dca2a0(CCharacter* char_ptr, undefined4 param2): the per-ruler
    // CouncilTasks AI routine called behind the GLOBAL gate DAT_0bfa5c83 from
    // FUN_04d89360 (per-char AI dispatcher). It loops the ruler's council
    // positions (via COUNCIL_POSITION_DB) and calls FUN_04dc9fc0 per position,
    // which re-assigns each task through the same FUN_05cc8660/FUN_05cc9970 store
    // — overwriting any bridge-set council task every AI tick. The bridge installs
    // a per-ruler suppression hook here (council_suppress_hook.cpp): if the
    // char_ptr's id is in g_council_locked it returns WITHOUT calling the
    // original, freezing only that ruler's council AI (vs. the global toggle
    // which freezes every realm). HIGH — decompile-verified callee behind the
    // DAT_0bfa5c83 gate; VA 0x04dca2a0 - 0x100000.
    constexpr uintptr_t AI_COUNCIL_TASKS_PER_RULER = 0x04cca2a0;  // HIGH: FUN_04dca2a0 per-ruler council-AI re-assign (VA 0x4dca2a0-0x100000)

    // ── COUNCIL APPOINT (L29: seat/replace a councillor, fill a vacant seat) ──
    // re/findings/re_appoint_councillor.md (2026-06-18). The WRITE TARGET and the
    // POSITION RESOLUTION are SOLID (reuse do_set_council_task / do_query_council).
    // The native MUTATOR Execute entry is class-identified but NOT byte-pinned this
    // session (static vtables RELRO-zeroed; core dumps lack rodata vtable pages;
    // live observer was UNPAUSED so paused-gated memory_probe refused). Two candidate
    // native paths — confirm the Execute via a PAUSED memory_probe of the vtable or a
    // fresh rodata-bearing core dump before wiring (see findings "Next step to finish").
    //
    // CCouncilPosition holder/vacant — HIGH, runtime-validated by query_council
    // (method=council_instance_walk). An appoint must make the engine write +0x30.
    constexpr uintptr_t COUNCIL_POS_HOLDER_CHAR_ID = 0x30;  // HIGH: u32 seated councillor char id; 0xFFFFFF/0 = VACANT (query_council reads this)
    constexpr uintptr_t COUNCIL_POS_SCOPE_CHAR_ID  = 0x38;  // HIGH: u32 char id used in task scope-prep (NOT the displayed holder; read by FUN_05c83430 / FUN_05c84420)
    //
    // CAssignCouncillorTypeEffect (RTTI rtti_classes.txt:16682; effect_subclasses.csv:154)
    // — the membership analog of CSetCouncilTaskEffect (whose ::Execute @ 0x04df21f0 is
    // the proven AI-applicable applier).
    //
    // ── EXECUTE PINNED 20260618 (re_appoint_councillor_EXECUTE_20260618.md) ──
    // The Execute IS gh-VA 0x4ef2900 (ck3 offset 0x04df2900). Proof: read the primary
    // vtable (vtable_start gh-VA 0xb917d40) from the FRESH core dump ck3_fresh_re.811520
    // (ck3 module base 0x561f72c99000) and compared SLOT-BY-SLOT (40 slots) against
    // CSetCouncilTaskEffect's primary vtable (0xb917c10). They are byte-for-byte parallel;
    // SetTask vtable slot[9] = 0x4ef21f0 = the CONFIRMED CSetCouncilTaskEffect_Execute
    // (446B, decompiled). ⇒ Assign vtable slot[9] = 0x4ef2900 = the Assign Execute.
    // The earlier "PARSE fn" note was a Ghidra mis-bound: 0x4ef2900 sits INSIDE
    // FUN_04ef28a0 (parse, entry 0x4ef28a0 size 0x153) only because Ghidra never created
    // a function-start at the Execute entry — the live vtable overrides that.
    // Signature (from the SetTask twin): Execute(effect_obj /*param_1*/, scope*
    // /*param_2 = {i16 type=4 CHAR; u32 ruler_char_id @ +8}*/). effect_obj fields:
    //   +0x50 u32 = char id of the councillor to SEAT  (engine packs via FUN_081f11a0)
    //   +0x54 i32 = position-type selector (-1 = default; else the council position type)
    // The Execute SELF-VALIDATES: on a bad councillor/seat it returns 0 and writes a
    // localized rejection std::string into effect_obj (snprintf → FUN_075c35c0(param_1,..)),
    // so no separate Validate call is required (unlike the command-store task path).
    // NOT player-gated / NOT queued → applies synchronously for an AI ruler (same as the
    // SetTask effect). Confidence MEDIUM-HIGH: vtable parallelism is strong structural
    // proof, but the Execute body could not be byte-disassembled (Ghidra .text reads 0;
    // the core dump omits .text pages). Upgrade to HIGH via a PAUSED live memory_probe of
    // the Execute prologue or a text+rodata core dump.
    // ⚠⚠ INVALID — DO NOT CALL. CONFIRMED CRASHER (#62, 2026-06-18, live /proc/877296/mem):
    // 0x04df2900 is +0x60 INTO THE MIDDLE of the parser FUN_04ef28a0 (entry 0x04df28a0) —
    // calling it executes a mid-instruction-stream → SIGSEGV / jomini "empty scope" faults
    // (this crashed the game). Two errors: (a) addr from the unreliable effect_subclasses.csv
    // execute_rva; (b) the "effect has a virtual (effect,scope) Execute at vtable[9]" premise
    // is FALSE — CAssignCouncillorTypeEffect's vtable has NO such Execute (effects' Executes
    // are free funcs), and the WORKING council code (do_set_council_task) uses command+STORE,
    // not the effect. The +0x50/+0x54/type=4-scope layout below was copied from a path the
    // working code never runs. Correct appoint path = CSwapCouncilPositionCommand below.
    constexpr uintptr_t ASSIGN_COUNCILLOR_EFFECT_EXECUTE = 0x04df2900;  // INVALID — mid-parser, crashes (see note above)
    constexpr uintptr_t ASSIGN_COUNCILLOR_EFFECT_OBJ_CHARREF = 0x50;    // INVALID (wrong-path layout)
    constexpr uintptr_t ASSIGN_COUNCILLOR_EFFECT_OBJ_POSTYPE = 0x54;    // INVALID (wrong-path layout)
    constexpr uintptr_t ASSIGN_COUNCILLOR_EFFECT_VTABLE_BAD = 0x0b817d40;  // WRONG (0x817 vs 0x917 digit error)
    constexpr uintptr_t ASSIGN_COUNCILLOR_EFFECT_VTABLE = 0x0b917d40;  // HIGH (CORRECTED): real CAssignCouncillorTypeEffect primary vtable_start (live raw, typeinfo 0xb917e18) — but it has no (effect,scope) Execute slot; informational only
    constexpr uintptr_t ASSIGN_COUNCILLOR_EFFECT_PARSE  = 0x04ef28a0;  // NOTE: PARSE/AddEntry fn (gh-VA), NOT Execute
    //
    // CSwapCouncilPositionCommand (#62, 2026-06-18) — the GUI "drag courtier onto seat" /
    // fill-empty-seat command; the CORRECT native appoint primitive. typeinfo live raw
    // 0xbacc320. Its Execute (FUN_05c827e0) calls the seat mutator FUN_067e2ba0(owner,+0x24,
    // +0x28) UNCONDITIONALLY (only the notification tail is GetCurrentPlayerId-gated) — so it
    // MAY apply for AI rulers (unlike the player-queue command-dispatch). cmd obj fields:
    // +0x20 = council-owner char id (ruler); +0x24 / +0x28 = the two char ids to swap between
    // seats. MEDIUM-HIGH (live-vtable + decompile verified; NOT yet write-tested — confirm the
    // vacant-seat field semantics + that +0x30 updates via a live write-test before trusting).
    constexpr uintptr_t SWAP_COUNCIL_POSITION_CMD_VTABLE  = 0x0bacbf78;  // MEDIUM-HIGH: primary cmd vtable_start (cmd+0x00), live raw
    constexpr uintptr_t SWAP_COUNCIL_POSITION_CMD_EXECUTE  = 0x05b827e0;  // MEDIUM-HIGH: Execute (FUN_05c827e0), unconditional seat mutator
    constexpr uintptr_t SWAP_COUNCIL_POSITION_CMD_BUILDER  = 0x05b82990;  // MEDIUM-HIGH: builder FUN_05c82990 (sets vtable + fields)
    constexpr uintptr_t SWAP_COUNCIL_SEAT_MUTATOR          = 0x066e2ba0;  // CORRECTED -0x100000 (was 0x067e2ba0=image VA): FUN_067e2ba0(owner,a,b) calls the raw leaf THEN COUNCIL_AI_RECOMPUTE (next-tick enqueue, NOT a hang) — DO NOT CALL (use the raw leaf directly)
    // ── CORRECTED appoint leaf (#62, 2026-06-19): the queue never calls Execute; the
    // real apply is the EFFECT leaf, callable directly. FUN_067e2ba0 wedged because it
    // appends FUN_068710e0 (AI council recompute). Call the RAW swap leaf FUN_068b4d10
    // DIRECTLY instead — it does the membership add/remove + swap + notification but NOT
    // the recompute. Wrap in fake-RNG-scope+watchdog (membership writers touch RNG).
    // (FUN_0674dbe0, previously mislabeled "recompute liege-walk", is actually an
    // RNG-scope getter — NOT the wedge. The wedge is FUN_068710e0.) MEDIUM — not yet write-tested.
    constexpr uintptr_t SWAP_COUNCIL_RAW_LEAF  = 0x067b4d10;  // CORRECTED -0x100000 (was 0x068b4d10=IMAGE VA → resolved +0x310 into a logging fn → the crash). HIGH: FUN_068b4d10(council_inst_ptr, owner_char_ptr, u32 char_a_id, u32 char_b_id) — recompute-free 2-occupied seat swap (re_resolution_appoint.md)
    constexpr uintptr_t COUNCIL_AI_RECOMPUTE   = 0x067710e0;  // CORRECTED -0x100000 (was 0x068710e0=image VA): FUN_068710e0 enqueues a next-tick council recompute (array append under mutex) — NOT a hang, but not needed for the raw-leaf path
    constexpr size_t    SWAP_COUNCIL_CMD_OWNER   = 0x20;  // u32 ruler/owner char id
    constexpr size_t    SWAP_COUNCIL_CMD_CHAR_A  = 0x24;  // u32 swap char id A
    constexpr size_t    SWAP_COUNCIL_CMD_CHAR_B  = 0x28;  // u32 swap char id B
    // ── CORRECT appoint primitive (#62, 2026-06-18, live-decompiled PID 898009) ──
    // FUN_05cc9d90(pos_obj, char_ptr) = the clean council-seat HOLDER SETTER — the
    // SIBLING of the proven council-task STORE leaf FUN_05cc9970 (same 0x5cc9xxx
    // cluster). Writes pos_obj+0x38 + pos_obj+0x30 = new holder id, +0x28 = date,
    // fires old-holder removal + new-holder add (sets courtier char+0x1b0[+0xf0]=pid
    // back-link) + the assignment notification + council recompute. NO player gate,
    // and (unlike the swap leaf FUN_067e2ba0) does NOT drive the AI-recompute
    // liege-walk that WEDGED the game. HIGH-confidence safe (parallel to the working
    // set_council_task store). Guards before calling: pos_obj+0x30==0xFFFFFFFF (seat
    // VACANT), pos_obj+POS_OWNER==ruler id, courtier holds no other seat.
    constexpr uintptr_t COUNCIL_SEAT_HOLDER_SET = 0x05bc9d90;  // HIGH: FUN_05cc9d90(pos_obj, char_ptr) — seat setter (writes holder +0x38 via FUN_05ccb030 + recompute enqueue); NO displacement block. CORRECTED -0x100000 (was 0x05cc9d90 = Ghidra-image VA). Use for vacant-fill.
    constexpr uintptr_t COUNCIL_HELD_PID_GETTER = 0x067e2d30;  // HIGH: FUN_067e2d30(char)->u32 pid held, 0xFFFFFFFF if none
    constexpr size_t    COUNCIL_POS_OWNER       = 0x3c;        // HIGH: u32 owner-ruler char id on CCouncilPosition
    constexpr uintptr_t SWAP_COUNCIL_POSITION_NAME_STR = 0x08be5208;  // MEDIUM: rodata "swap_council_position" (gh-VA)
    constexpr uintptr_t FIRE_COUNCIL_POSITION_NAME_STR = 0x08be521e;  // MEDIUM: rodata "fire_council_position" (gh-VA)

    // ── COUNCIL VACANT-SEAT FILL (Angle D, re_council_ai_seat_path.md, 2026-06-19) ──
    // Recompute-FREE seat-ASSIGN that FILLS a vacant seat (and relocates an occupied
    // one). This is the engine's own seat-assign primitive used by the
    // CAssignCouncillorType / explicit-position script effects (FUN_04ff2a70 /
    // FUN_04ff34f0 both call COUNCIL_SEAT_ASSIGN with skip_eligibility=1 as their
    // applier). Unlike SWAP_COUNCIL_RAW_LEAF (two-occupied swap), there is NO
    // both-seats-occupied precondition, so a VACANT target is filled. NO call to
    // COUNCIL_AI_RECOMPUTE (FUN_068710e0) — recompute-free, no hang hazard. Advances
    // RNG via FUN_07848f10, so the fake-random-scope wrap IS required.
    // DISTINCT from SWAP_COUNCIL_SEAT_MUTATOR=0x066e2ba0 (swap leaf + recompute).
    constexpr uintptr_t COUNCIL_SEAT_ASSIGN      = 0x066e2b40;  // HIGH: FUN_067e2b40(owner_char*, u32 char_FULL_HANDLE, u32 position_id, char run_validation/*0=run oracle*/, char skip_eligibility/*1=skip*/) — reads owner+0x1b8 council, null-guards, calls COUNCIL_SEAT_ASSIGN_PRIM. THE vacant-fill entry. #79 ROOT: arg2 = candidate FULL handle (char+0x18, incl. the non-zero generation byte in bits 24-31), NOT a 24-bit id — the prim's worker FUN_068b4180 validates *(cand+0x18)==arg, so a bare 24-bit id fails the match and triggers the null-sentinel→displacement-vcall SIGSEGV (#62/#79 crash).
    constexpr uintptr_t COUNCIL_SEAT_ASSIGN_PRIM = 0x067b4180;  // HIGH: FUN_068b4180(council, owner_char*, u32 char_id, u32 pos_id, char p5, char skip_eligibility) — seat-assign primitive; lands in COUNCIL_SEAT_HOLDER_SET (FUN_05cc9d90, writes pos_obj+0x30); param_6!=0 skips COUNCIL_ELIGIBILITY_GATE.
    constexpr uintptr_t COUNCIL_ELIGIBILITY_GATE = 0x05c84800;  // HIGH: FUN_05c84800(owner, char_ptr, pos_descriptor, &reject_str) — can-this-char-hold-this-position oracle; skipped when skip_eligibility!=0.
    // PRE-EXISTING BUG NOTE: COUNCIL_POSITION_RESOLVE=0x067d2b20 (line ~1365) is WRONG
    // (header+0x100000=0x068d2b20=FUN_068d2b20, unrelated). Real FUN_067e2b20 header is
    // 0x066e2b20 (digit error 0x067e vs 0x067d). Not on the fill path; fix separately.

    // Round 6b: Vassal contract area functions
    // WARNING: Names below are from Round 6b and are MISIDENTIFIED. These functions
    // operate on CUnit/military data (TPdxRefDatabase<CUnit,8> at 0x0bed76c8),
    // NOT on obligation levels. See re/findings/re_vassal_contract.md for corrections.
    // Kept for backwards compatibility — do NOT use for obligation manipulation.
    constexpr uintptr_t CHECK_CONTRACT_VALID       = 0x04d29140;  // MISNAMED: virtual dispatch trampoline via +0x38
    constexpr uintptr_t APPLY_CONTRACT_CHANGE      = 0x04d29130;  // MISNAMED: stores global at param_1+0x38
    constexpr uintptr_t GET_OBLIGATION_LEVEL       = 0x04d29690;  // MISNAMED: iterates CUnit refs, sums via unit DB
    constexpr uintptr_t CREATE_OBLIGATION          = 0x04d4c980;  // MISNAMED: military scoring/selection function
    constexpr uintptr_t GET_NEXT_OBLIGATION        = 0x04d46630;  // MISNAMED: complex iteration with sorting
    constexpr uintptr_t GET_PREV_OBLIGATION        = 0x04d47180;  // MISNAMED: complex iteration with sorting
    constexpr uintptr_t GET_OBLIGATION_REALM       = 0x04d29ec0;  // MISNAMED: unknown, needs re-investigation
    constexpr uintptr_t ASSIGN_OBLIGATION          = 0x04d29100;  // MISNAMED: stores param_2 at param_1+0x38 after validity check

    // Vassal contract: verified functions (from serializer/vtable decompilation)
    constexpr uintptr_t CONTRACT_DESTRUCTOR        = 0x063d35e0;  // HIGH: CSubjectContract::~CSubjectContract (vfunc[0]) (ck-e58l.20)
    constexpr uintptr_t CONTRACT_DESERIALIZE       = 0x063d3820;  // HIGH: CSubjectContract deserialization (vfunc[3]) (ck-e58l.20)
    constexpr uintptr_t CONTRACT_SERIALIZE         = 0x063d3ae0;  // HIGH: CSubjectContract serialization (vfunc[4]) (ck-e58l.20)
    constexpr uintptr_t CONTRACT_CHANGE_EXECUTE    = 0x04d41c50;  // HIGH: CVassalContractChange::Execute (named in Ghidra) (ck-e58l.20)

    // Vassal contract negotiation scoring (from ck-x91l investigation)
    // See: re/findings/re_vassal_contract_scoring.md for full analysis
    constexpr uintptr_t CONTRACT_MGR_TICK           = 0x04d66220;  // HIGH: ContractManager tick, processes all contracts (ck-e58l.20)
    constexpr uintptr_t CONTRACT_MGR_EVAL_LIEGE     = 0x04d66550;  // HIGH: AI liege evaluates whether to modify contracts (ck-e58l.20)
    constexpr uintptr_t CONTRACT_MGR_EVAL_SUBJECT   = 0x04d669e0;  // MEDIUM: AI subject evaluates modifications (ck-e58l.20)
    constexpr uintptr_t CONTRACT_ENTRY_PROCESS      = 0x04d3e7f0;  // HIGH: per-contract 8-step change pipeline (ck-e58l.20)
    constexpr uintptr_t CONTRACT_ENTRY_VALIDATE     = 0x04d3ea00;  // HIGH: pre-change validation, returns 0/1/2 (ck-e58l.20)
    constexpr uintptr_t CONTRACT_AI_ACCEPTANCE      = 0x04d444d0;  // HIGH: AI acceptance eval: 0=reject,1=accept,2=force (ck-e58l.20)
    constexpr uintptr_t COMPUTE_OBLIGATION_DESIRE   = 0x04d49840;  // HIGH: core scoring fn, returns fixed-point 100000 (ck-e58l.20)
    constexpr uintptr_t FILL_CATEGORY_SCORES        = 0x04e48800;  // HIGH: fills 8-category scoring arrays
    constexpr uintptr_t COLLECT_VASSAL_SCORING      = 0x04d488a0;  // HIGH: per-vassal scoring data (hooks/military/dread) (ck-e58l.20)
    constexpr uintptr_t GET_CATEGORY_VALUE          = 0x04d716f0;  // HIGH: primary obligation value per category (ck-e58l.20)
    constexpr uintptr_t GET_CATEGORY_BASE_MOD       = 0x04d71790;  // HIGH: base modifier per category (int32) (ck-e58l.20)
    constexpr uintptr_t GET_CATEGORY_HOOK_MAX       = 0x04d717f0;  // HIGH: hook leverage cap per category (×100000) (ck-e58l.20)
    constexpr uintptr_t GET_CATEGORY_MIN_VALUE      = 0x04d71870;  // HIGH: floor value per category (×100000) (ck-e58l.20)
    constexpr uintptr_t GET_HOOK_TYPE               = 0x04d627d0;  // HIGH: hook type: 0=none,1=weak,3=strong,8=blocked (ck-e58l.20)
    constexpr uintptr_t COMPARE_CONTRACT_TARGET     = 0x04e45450;  // HIGH: opinion-based contract target comparison

    // ── Move-realm-capital command (ck modify-vassal-contract+move-capital lane, 20260618) ──
    // Pinned from the AI SetRealmCapital subsystem FUN_04e0ada0 (gated by AI toggle
    // DAT_0bfa5c7a / AI_TOGGLE_BASE+0x02). It builds a CSetRealmCapitalCommand and
    // dispatches it. ALL VAs below are ck3 raw offsets (Ghidra VA = raw + 0x100000).
    // See re/findings/re_modify_contract_move_capital.md.
    //
    // CSetRealmCapitalCommand layout (built by SET_REALM_CAPITAL_CMD_BUILD):
    //   +0x00 primary vtable (gh-VA 0xbd98a28); +0x18 secondary vtable (gh-VA 0xbd98ad8)
    //   +0x20 byte flag (1 = use FUN_06120400 alt path; 0 = relocate via CAPITAL_RELOCATE)
    //   +0x24 uint32 actor character id;  +0x28 uint32 target COUNTY landed-title id
    // Command primary-vtable slots (read from core ck3_20260421_195552, RELRO populated):
    //   slot[1] +0x08 = dtor/free; slot[7] +0x38 = Execute (CAPITAL_CMD_EXECUTE);
    //   slot[9] +0x48 = clone-into-refcounted-ptr (used by AI handler before dispatch).
    constexpr uintptr_t SET_REALM_CAPITAL_CMD_BUILD = 0x066cc3c0;  // HIGH: FUN_067cc3c0(cmd_buf, char_id, county_title_id) ctor (ck capital lane)
    constexpr uintptr_t CAPITAL_CMD_EXECUTE         = 0x066cc4c0;  // HIGH: command Execute (vtable+0x38): reads cmd+0x24/+0x28, calls CAPITAL_RELOCATE
    constexpr uintptr_t CAPITAL_CMD_CLONE_REFPTR    = 0x066da8a0;  // HIGH: command vtable+0x48: heap-clone 0x30B refcounted copy for dispatch
    constexpr uintptr_t CAPITAL_RELOCATE            = 0x061206b0;  // HIGH: FUN_062206b0(char*, county_title*, err_ctx) — capital relocate apply (err_ctx=0 = AI/no-reason path)
    constexpr uintptr_t CAPITAL_VALIDATE_APPLY      = 0x0611fe70;  // HIGH: FUN_0621fe70(char*, county_title*, mode, err_ctx) — county-tier + capital-barony validate; returns bool
    constexpr uintptr_t CJOMINI_COMMAND_BASE_CTOR   = 0x0755b630;  // HIGH: CJominiCommandBase::ctor(cmd, 0) (ck capital lane)
    // GAME_COMMAND_DISPATCH already defined above (0x07560390); g_CommandDispatchManager = globals::COMMAND_DISPATCH_MGR (0x0c59dd68)

    // Round 7: Vitals
    constexpr uintptr_t GET_AGE                    = 0x06670d30;  // HIGH: GetAge(char) -> int16, age in years (R22 confirmed)
    constexpr uintptr_t IS_ADULT                   = 0x0664ddc0;  // HIGH: IsAdult(char) -> bool (R22 confirmed)
    constexpr uintptr_t GET_STRESS                 = 0x0664b790;  // HIGH: 22B, reads core+0x2F8 (corrected from 0x0664b790) (ck-e58l.20)
    constexpr uintptr_t ADD_STRESS                 = 0x06679080;  // HIGH: 1032B, (char, delta, apply_mods) (corrected from 0x06679080) (ck-e58l.20)
    constexpr uintptr_t GET_DREAD                  = 0x0664ba90;  // HIGH: 23B, reads ruler+0x350
    constexpr uintptr_t ADD_DREAD                  = 0x066891b0;  // HIGH: 262B, (char, delta)
    constexpr uintptr_t SET_DREAD                  = 0x06689160;  // HIGH: 67B, direct set

    // R9-7: Focus/Lifestyle functions
    constexpr uintptr_t FOCUS_GETTER               = 0x067e27b0;  // HIGH: 25B, reads core_data+0x280 (R9-7)
    constexpr uintptr_t FOCUS_SETTER_CORE          = 0x067e27d0;  // HIGH: allocates/sets CActiveFocus (R9-7)
    constexpr uintptr_t FOCUS_LOOKUP_BY_KEY        = 0x038b58f0;  // HIGH: string -> SFocusType* (R9-7)
    constexpr uintptr_t GET_TOTAL_PERK_POINTS      = 0x066f5eb0;  // HIGH: UI getter (R9-7)
    constexpr uintptr_t GET_PERK_POINTS            = 0x066f5ee0;  // HIGH: UI getter (R9-7)
    constexpr uintptr_t GET_PERK_POINTS_USED       = 0x066f60c0;  // HIGH: UI getter (R9-7)
    constexpr uintptr_t GET_LIFESTYLE_XP           = 0x066f6300;  // HIGH: UI getter (R9-7)
    constexpr uintptr_t HAS_PERK                   = 0x066f6320;  // HIGH: UI getter (R9-7)

    // Round 7: Scheme Manager
    constexpr uintptr_t GET_SCHEME_MANAGER         = 0x05f5a230;  // HIGH: 13B, (game_state) -> scheme_mgr
    constexpr uintptr_t SCHEME_KNOW_ALL            = 0x062a9000;  // HIGH: 11B, sets mgr+0x2578
    constexpr uintptr_t SCHEME_END_ALL             = 0x062a9010;  // HIGH: 11B, sets mgr+0x257c
    // #132 (do_dispatch_scheme_intent / execute_scheme_intent): the start-a-scheme
    // entry. Decompile-verified signature (image 0x063a7dd0):
    //   CActiveScheme* CreateActiveScheme(mgr, CSchemeType* type, owner_char,
    //                                     uint64 target_scope, p5, p6)
    //   target_scope: low32 = target_type (0=char,1=title,2=culture,3=faith,4=none),
    //                 high32 = target_ref (FULL canonical handle char+0x18). For murder
    //                 target_type MUST equal type's expected target_type at type+0x1f0
    //                 (param_2[0x3e]) — verified 0 for murder/fabricate_hook/claim_throne.
    //   SELF-GATING: runs type is_shown (vt type+0x10) + ValidateSchemeTarget +
    //   owner/target IsAlive + HasSchemeOnTarget(cooldown) + type can-start (vt
    //   type+0x38) INTERNALLY; returns the null scheme *(base+SCHEME_NULL) on any fail
    //   (the VALUE in the slot, not its address -- lane-scheme 2026-10-18).
    //   p5/p6 are NON-NULLABLE: p5 = CTaskContract*, p6 = CArtifact* (lane-scheme
    //   2026-10-18, re_scheme_start_args.md); a scheme without them gets the DB null
    //   objects *(TASK_CONTRACT_NULL) / *(ARTIFACT_NULL), which execute_scheme_intent
    //   now passes itself (it used to replay a pointer captured by scheme_init_hook).
    //   SCHEME_TYPE_DB iterated like the faction DB: inst=*(base+SCHEME_TYPE_DB),
    //   arr=*(inst+0x88), count=*(u32)(inst+0x94)=76, name=std::string@type+0x10.
    constexpr uintptr_t CREATE_ACTIVE_SCHEME       = 0x062a7dd0;  // HIGH: 2296B, FUN_063a7dd0
    constexpr uintptr_t INITIALIZE_SCHEME          = 0x062a8810;  // HIGH: 516B, sets type/owner/target/dates/name (R10-8)
    constexpr uintptr_t INVALIDATE_SCHEME          = 0x062a8c50;  // HIGH: 364B, sets status=1, cleans up refs (R10-8)
    // #scheme_cancel: SchemeManager::RemoveActiveScheme(mgr, scheme) — unlinks from all
    // manager index lists, runs INVALIDATE_SCHEME teardown + owner/target notify, memsets
    // the scheme over TOTAL_SIZE(0x1F0) and frees the refdb slot. Inverse of
    // CREATE_ACTIVE_SCHEME. Wired by execute_scheme_cancel_intent (gated: actor==OWNER_ID).
    constexpr uintptr_t REMOVE_ACTIVE_SCHEME       = 0x062a6e30;  // HIGH: 671B, removes from manager, frees slot (R10-8)
    constexpr uintptr_t PROCESS_SCHEME_TICK        = 0x062a70f0;  // HIGH: 1107B, handles know/end schemes, expiry (R10-8)
    constexpr uintptr_t UPDATE_SCHEME_PROGRESS     = 0x062a75a0;  // HIGH: 931B, daily progress tick (R10-8)
    constexpr uintptr_t VALIDATE_SCHEME_TARGET     = 0x062a86e0;  // HIGH: 291B, switch on target_type (R10-8)
    constexpr uintptr_t SCHEME_REFDB_LOOKUP        = 0x03784840;  // HIGH: 58B, cleanest RefDB accessor (R10-8)
    constexpr uintptr_t SCHEME_TYPE_NAME_ACCESSOR  = 0x05bb16e0;  // HIGH: 152B, CSchemeType name from "game_scheme_type_struct" (R10-8)

    // Scheme success probability (ck-1nab investigation)
    constexpr uintptr_t SCHEME_COMPUTE_SUCCESS_TRIPLE  = 0x062c64d0;  // HIGH: inner (base,growth,max) formula, 5210B (ck-e58l.20)
    constexpr uintptr_t SCHEME_COMPUTE_SUCCESS_CHANCE  = 0x062c9f90;  // HIGH: inner success chance %, 1653B (ck-e58l.20)
    constexpr uintptr_t SCHEME_EVAL_AGENT_CONTRIBUTION = 0x062caab0;  // HIGH: agent modifier by type, 628B (ck-e58l.20)
    constexpr uintptr_t SCHEME_EVAL_SCRIPTED_MOD       = 0x062ca760;  // HIGH: scripted modifier evaluator (ck-e58l.20)
    constexpr uintptr_t SCHEME_EVAL_CHAR_CULTURE_MOD   = 0x063ca9f0;  // HIGH: character/culture modifier evaluator
    constexpr uintptr_t SCHEME_EVAL_MODIFIER_TREE      = 0x074713b0;  // HIGH: modifier definition block evaluator (ck-e58l.20)
    constexpr uintptr_t SCHEME_EVAL_TYPE_BASE          = 0x04d92e80;  // HIGH: scheme type base value evaluator (ck-e58l.20)
    constexpr uintptr_t SCHEME_EVAL_CHAR_SKILL         = 0x0666c800;  // HIGH: character skill for growth (ck-e58l.20)
    constexpr uintptr_t SCHEME_CHECK_GAME_RULE         = 0x0665f090;  // HIGH: game rule flag check (ck-e58l.20)

    // Scheme success globals (base-relative, core dump validated)
    constexpr uintptr_t SCHEME_GROWTH_FLOOR            = 0x0c2aaf58;  // HIGH: int64=100000 (1.0/day min growth)
    constexpr uintptr_t SCHEME_MAX_CAP_TABLE_BASE      = 0x0c2aaf70;  // HIGH: int32[4], values 95/100/95/95
    constexpr uintptr_t SCHEME_MAX_PROGRESS_FLOOR_G    = 0x0c2aaf40;  // HIGH: int64=10 (min 10 days)

    // Round 7: Relations
    constexpr uintptr_t GET_RELATION_MANAGER       = 0x05f60090;  // HIGH: 13B, (game_state) -> rel_mgr
    constexpr uintptr_t ADD_RELATION               = 0x061daaf0;  // HIGH: (rel_mgr, args_struct)
    constexpr uintptr_t REMOVE_RELATION            = 0x061db2f0;  // HIGH: (rel_mgr, char1_id, char2_id, type)

    // Round 7: Lifestyle
    constexpr uintptr_t ADD_PERK                   = 0x066f4f80;  // HIGH: (char, perk_ptr, 1)
    constexpr uintptr_t ADD_LIFESTYLE_XP           = 0x066f4b00;  // HIGH: (char, lifestyle_ptr, xp, 0, 1)

    // Round 9: Opinion system
    constexpr uintptr_t GET_OPINION_BREAKDOWN      = 0x06678b10;  // HIGH: fn(output_str, char_a, char_b), 96B
    constexpr uintptr_t ACTIVE_OPINION_SUM         = 0x061c5750;  // HIGH: int(active_opinion_entry), 471B

    // Round 9: Income (source: income_utilities.cpp)
    constexpr uintptr_t INCOME_CALC_BUDGET         = 0x06086c10;  // HIGH: 3-cat income breakdown, 911B
    constexpr uintptr_t INCOME_CALC_SUBJECT_TAX    = 0x0607b210;  // HIGH: vassal tax sum, 2861B
    constexpr uintptr_t INCOME_CALC_BARTER_GOODS   = 0x06087be0;  // HIGH: barter goods sum, 2842B
    constexpr uintptr_t INCOME_OPINION_MODIFIER    = 0x060870a0;  // HIGH: treasury opinion modifier, 902B

    // Round 9: Focus/lifestyle
    constexpr uintptr_t SET_FOCUS_CMD              = 0x030ffdb0;  // NOTE: just the "set_focus" string guard-init (FUN_031ffd80), NOT a usable handler — decompile 20260612
    // SIGNATURE RESOLVED (decompile 20260612): void SET_FOCUS_APPLY(CCharacter* c, CFocus* focus_obj).
    // Reads c+0x1a8 (REALM_DATA); validity-checks focus via focus_obj vtable+0x38; then
    // FOCUS_SETTER_CORE(realm_data, focus_obj, c) allocs CActiveFocus at realm_data+0x280.
    // focus_obj = a CFocus* from FOCUS_DB (game_db: entries+0x88, count+0x90; match db_object::NAME_KEY).
    constexpr uintptr_t SET_FOCUS_APPLY            = 0x066f53a0;  // HIGH: SET_FOCUS_APPLY(char*, focus_obj*) — 2-arg, resolved 20260612

    // Development pipeline functions
    constexpr uintptr_t DEV_MONTHLY_TICK           = 0x040fda70;  // HIGH: monthly dev tick — iterates all counties, calc + apply (ck-e58l.20)
    constexpr uintptr_t DEV_CALC_GROWTH            = 0x05bd8230;  // HIGH: CalcMonthlyDevelopmentGrowth(county_data, breakdown) (ck-e58l.20)
                                                                   //   computes spread from neighbors + modifier 0x1e9/0x1ea/0x1eb
    constexpr uintptr_t DEV_APPLY_PROGRESS         = 0x05bd7e90;  // HIGH: ApplyDevelopmentProgress(county_data, growth_fp) (ck-e58l.20)
                                                                   //   accumulates +0x218, triggers level-up at 10M, resets to 0
    constexpr uintptr_t COUNTY_SERIALIZE           = 0x05bd6cf0;  // HIGH: CCountyData save (writes tagged fields) (ck-e58l.20)
    constexpr uintptr_t COUNTY_DESERIALIZE         = 0x05bd6ee0;  // HIGH: CCountyData load (reads tagged fields) (ck-e58l.20)

    // County conversion functions (ck-pj01)
    constexpr uintptr_t SET_COUNTY_FAITH           = 0x05bd7900;  // HIGH: SetCountyFaith(county, faith, update_flag) (ck-e58l.20)
                                                                   //   Writes county+0x208, updates faith+0x4c8 follower counts
                                                                   //   Handles religion tracking when crossing religion boundaries
    constexpr uintptr_t SET_COUNTY_FAITH_PUB       = 0x05bd78a0;  // HIGH: wrapper — calls core + event dispatch (on_faith_conversion) (ck-e58l.20)
    constexpr uintptr_t SET_COUNTY_CULTURE         = 0x05bd76d0;  // HIGH: SetCountyCulture(county, culture, update_flag) (ck-e58l.20)
                                                                   //   Writes county+0x20C, updates culture county tracking
    constexpr uintptr_t SET_COUNTY_CULTURE_PUB     = 0x05bd7660;  // HIGH: wrapper — calls core + event dispatch (ck-e58l.20)
    constexpr uintptr_t FAITH_ADD_COUNTY_FOLLOWERS = 0x05eaa4e0;  // HIGH: faith+0x4c8 += delta (trivial 6B function) (ck-e58l.20)
    constexpr uintptr_t CULTURE_REMOVE_COUNTY      = 0x05c208f0;  // HIGH: remove county from culture's county list (ck-e58l.20)
    constexpr uintptr_t CULTURE_ADD_COUNTY         = 0x05c20950;  // HIGH: add county to culture's county list (ck-e58l.20)
    constexpr uintptr_t RELIGION_REMOVE_COUNTY     = 0x06272d80;  // HIGH: remove county from religion's county tracking (ck-e58l.20)
    constexpr uintptr_t RELIGION_ADD_COUNTY        = 0x06272de0;  // HIGH: add county to religion's county tracking (ck-e58l.20)

    // Round 10: Income calculators
    constexpr uintptr_t INCOME_DOMAIN_CALC         = 0x060874d0;  // HIGH: domain income calculator
    constexpr uintptr_t INCOME_VASSAL_CALC         = 0x060878d0;  // HIGH: vassal income calculator
    constexpr uintptr_t INCOME_TRIBUTARY_CALC      = 0x060887e0;  // HIGH: tributary income calculator
    constexpr uintptr_t INCOME_MODIFIER_CALC       = 0x06088af0;  // HIGH: monthly income total with modifiers
    constexpr uintptr_t MONTHLY_INCOME_GET         = 0x06674290;  // HIGH: monthly income accessor
    constexpr uintptr_t LEGITIMACY_GET             = 0x0577c640;  // HIGH: character legitimacy accessor

    // Round 10: CB validation pipeline
    constexpr uintptr_t CB_PREFILTER               = 0x06cceb70;  // HIGH: fast 3-trigger CB precheck
    constexpr uintptr_t CB_VALIDITY_CHECK          = 0x06cce6d0;  // HIGH: full CB validity wrapper (GetValidCBOptions)
    constexpr uintptr_t CB_VALIDITY_CORE           = 0x06cce220;  // HIGH: core CB validity evaluator
    constexpr uintptr_t TRIGGER_EVAL               = 0x0665a030;  // HIGH: universal scripted trigger evaluator

    // Phase 9: CB subsystem functions (non-vassal war CBs)
    constexpr uintptr_t CB_STANDARD_TITLE_VALID    = 0x06cca050;  // HIGH: StandardTitleValid — enumerates valid target titles for title-targeted CBs
    constexpr uintptr_t CB_TITLE_VALIDATION        = 0x06cc9920;  // HIGH: TitleValidation — 3-way branch on target_mode (0/6/other)
    constexpr uintptr_t CB_EXTENDED_TRIGGER_CHECK  = 0x06ccec00;  // HIGH: ExtendedTriggerCheck — 5-gate AND cascade for CB validity
    // #74 (2026-06-22, re_directional_conquest_enabler.md): CB_EXTENDED_TRIGGER_CHECK's
    // FIRST gate is the GROUP-level is_valid trigger at *(cb_type+0x30)+0x48 — i.e.
    // `subjugation:allowed_for_character` / `conquest:allowed_for_character`. This is
    // why CB_VALIDITY_CHECK legitimately enforces the tribal-government + faith-doctrine
    // + neighbor gates for subjugation/conquest even though those CBs' PER-CB triggers
    // (cb+0x38) are EMPTY. Gates evaluated by CB_TITLE_VALIDATION (0x06cc9920) per
    // target_mode: mode 0 (subjugation/vassalization/tributary) → can_use (cb+0x3b8) +
    // can_use_title (cb+0x498), both empty; mode 6 (claim) → EnumerateClaims; else →
    // StandardTitleValid. So the directional gate lives in the GROUP trigger, not per-CB.
    constexpr uintptr_t CB_BUILD_SCOPES            = 0x06ccee80;  // HIGH: BuildScopes — sets up trigger evaluation scopes
    constexpr uintptr_t CB_ENUMERATE_CLAIMS        = 0x06cd0010;  // HIGH: EnumerateClaims — for claim-based CBs (target_mode=6)
    constexpr uintptr_t CB_POPULATE_CANDIDATE_TITLES = 0x06cd0210; // HIGH: PopulateCandidateTitles — 4-mode switch for de_jure/direct titles
    constexpr uintptr_t CB_FILTER_BY_RELATION      = 0x06ccf020;  // HIGH: FilterByAttackerRelation — vassal-only CB detection
    constexpr uintptr_t CB_VALIDATE_TARGET_CLAIM   = 0x06cc8650;  // HIGH: ValidateTargetClaim — evaluates can_use trigger at cb+0x578

    // ck-e58l.3: has_any_display_cb_on script trigger — "any CB passes" entry-point
    constexpr uintptr_t HAS_ANY_DISPLAY_CB_ON_EVAL = 0x052092b0;  // HIGH: evaluator (vtbl+0xD0 of trigger-instance vtable 0x0bb40620)
    // ck-e58l.3: has_any_display_cb_on outer evaluator (cascade entry from script-trigger dispatch)
    constexpr uintptr_t HAS_ANY_DISPLAY_CB_EVALUATOR = 0x053092b0;  // HIGH: FUN_054092b0 - 0x100000: outer has_any_display_cb_on dispatch (ck-e58l.3) (ck-audit-B)
    constexpr uintptr_t CB_ANY_OR_ENUMERATE_BY_ID  = 0x06cc76a0;  // HIGH: id→ptr wrapper, forces out_list=NULL (126B)
    constexpr uintptr_t CB_CASCADE                 = 0x065d6b20;  // HIGH: full CB-group cascade — iterates CB_GROUP_DB, runs Gates 0-6, short-circuit if out_list=NULL, else collect — Ghidra: CB_Cascade (ck-audit-B: renamed from CB_ANY_OR_ENUMERATE)
    constexpr uintptr_t CB_CHECK_IS_VASSAL_CB      = 0x06ccecf0;  // HIGH: CheckIsVassalCB — cb+0x6b4 and cb+0x1700 flags
    // NOTE: AI_POPULATE_WAR_CANDIDATES (0x04d99190) already defined in Phase 8 section
    // NOTE: AI_SELECT_WAR_TARGET_CB = WAR_BUILD_STATE (0x04d980d0) already defined in Round 6 section

    // Round 10: Realm/domain functions
    constexpr uintptr_t COMPUTE_DOMAIN_LIMIT       = 0x067ba9c0;  // HIGH: ComputeDomainLimit, 1063B (ck-e58l.20)
    constexpr uintptr_t COLLECT_REALM_TITLES       = 0x06779780;  // HIGH: recursive realm size calc, 855B (ck-e58l.20)
    constexpr uintptr_t COUNTS_TOWARD_DOMAIN       = 0x060cf380;  // HIGH: checks if title counts for domain, 275B (ck-e58l.20)

    // Round 10-5: Income/expense pipeline (source: income_utilities.cpp)
    constexpr uintptr_t INCOME_EXPENSE_WRAPPER     = 0x06088ca0;  // HIGH: monthly expense calculator (wrapper), 66B
    constexpr uintptr_t INCOME_EXPENSE_COMPUTE     = 0x067327a0;  // HIGH: expense computation (10-element array), 1236B
    constexpr uintptr_t INCOME_NET_CALC            = 0x06088cf0;  // HIGH: income - expenses (net), 104B
    constexpr uintptr_t INCOME_BALANCE_WRITER      = 0x0665e550;  // HIGH: net income → barter accumulator, 196B
    constexpr uintptr_t AI_INCOME_CALC             = 0x04cd39f0;  // HIGH: AI income data struct (7-element), 221B
    constexpr uintptr_t ARMY_MAINTENANCE_NOBD      = 0x06681d70;  // HIGH: army maintenance (no breakdown), 411B
    constexpr uintptr_t ARMY_MAINTENANCE_BD        = 0x06681f10;  // HIGH: army maintenance (with breakdown), 471B
    constexpr uintptr_t VASSAL_CONTRIB_CALC        = 0x06087be0;  // HIGH: vassal/tributary contribution calc, 2842B
    constexpr uintptr_t HOLDING_INCOME_CALC        = 0x06022330;  // HIGH: holding income sub-calculation, 1627B
    constexpr uintptr_t YEARLY_STATS_SERIALIZER    = 0x066fceb0;  // HIGH: yearly statistics serializer, 1929B

    // Round 10-6: Regiment/MaA functions
    constexpr uintptr_t REGIMENT_ALIVE_COUNT       = 0x0625b0d0;  // HIGH: CalcMenAliveCount, 319B (7 chunks)
    constexpr uintptr_t REGIMENT_MAINT_COST        = 0x0625cfc0;  // HIGH: CalcMaintenanceCost (full w/ ratio), 426B
    constexpr uintptr_t REGIMENT_BASE_COST         = 0x0625cf70;  // HIGH: CalcBaseCost (simple), 34B
    constexpr uintptr_t MILITARY_STATS_COMPUTE     = 0x0619fed0;  // HIGH: ComputeMilitaryStats (8-category), 2853B
    constexpr uintptr_t MILITARY_STRENGTH_GET      = 0x067c3ac0;  // HIGH: GetMilitaryStrength (summary), 287B
    constexpr uintptr_t MIL_SUM_MAX_SOLDIERS       = 0x0619f380;  // HIGH: sum max_soldiers across 8 cats, 37B
    constexpr uintptr_t MIL_SUM_CURRENT_SOLDIERS   = 0x0619f3b0;  // HIGH: sum current_soldiers across 8 cats, 36B
    constexpr uintptr_t REGIMENT_ACCUMULATE_STATS  = 0x061a0b10;  // HIGH: per-regiment stat accumulator, 401B
    constexpr uintptr_t REGIMENT_PROCESS_MERC      = 0x061a0cb0;  // HIGH: process mercenary regiments, 561B
    constexpr uintptr_t REGIMENT_PROCESS_HOLY      = 0x061a0ef0;  // HIGH: process holy order regiments, 561B
    constexpr uintptr_t GET_PERSONAL_REGIMENTS     = 0x06688570;  // HIGH: fn(ruler_data) → ruler_data+0x108, 127B
    constexpr uintptr_t GET_TITLE_REGIMENTS        = 0x06688d00;  // HIGH: fn(ruler_data) → ruler_data+0x2A8, 127B
    constexpr uintptr_t GET_MERC_REGIMENTS         = 0x0668a910;  // HIGH: fn(ruler_data) → ruler_data+0x138, 127B
    constexpr uintptr_t GET_HOLY_ORDER_REGIMENTS   = 0x0668a990;  // HIGH: fn(ruler_data) → ruler_data+0x150, 127B
    constexpr uintptr_t GET_MAA_OWNER_LINKS        = 0x066885f0;  // HIGH: fn(ruler_data) → ruler_data+0x290, 29B

    // Mercenary/Holy Order Hiring RE (ck-mol-p6cm)
    constexpr uintptr_t MERC_COMPANY_CONSTRUCTOR   = 0x039fb060;  // MEDIUM: CMercenaryCompany constructor, inits ~0xF1C bytes (ck-e58l.20)
    constexpr uintptr_t MERC_COMPANY_DESTRUCTOR    = 0x039fb510;  // MEDIUM: CMercenaryCompany destructor (ck-e58l.20)
    constexpr uintptr_t MERC_BASE_DESTRUCTOR       = 0x039fbc30;  // MEDIUM: CMercenaryCompany base class destructor (ck-e58l.20)
    constexpr uintptr_t GAME_CMD_DISPATCH_HELPER   = 0x05edde90;  // HIGH: GameCommand dispatch via helper vtable (void* helper, uint32_t type_hash)
    // Note: 208 game command helper vtables found via marker function 0x5ede1e0
    // Military command cluster at 0xbc97000-0xbca0000 in .data.rel.ro

    // Round 10-2: Alliance system functions
    constexpr uintptr_t ALLIANCE_IS_ALLIED_TO     = 0x06750c50;  // HIGH: bool(char_a, char_b), 150B — checks forced allies then CActiveRelation alliance flag (rel+0x94). #119 FIX: was 0x05750c50 (off by 0x1000000 — resolved to a vector-insert helper FUN_05850b80, NOT IsAlliedTo). IMAGE 0x6850c50 = "IsAlliedTo".
    constexpr uintptr_t ALLIANCE_GET_REASONS      = 0x05752240;  // HIGH: fn(output, char_a, char_b), 1038B — enumerate all SAllianceReason entries
    constexpr uintptr_t ALLIANCE_CREATE           = 0x05169820;  // HIGH: fn(relation, char_a_id, char_b_id, reason_enum), 192B — appends to CActiveRelation+0x88 (ck-e58l.20)

    // Round 10-3: Diplomatic relation / truce functions
    constexpr uintptr_t TRUCE_LIST_ACCESSOR        = 0x0576df20;  // HIGH: fn(char) -> *(*(char+0x1c0)+0x6a0), fallback sentinel
    constexpr uintptr_t GET_DIPLOMATIC_RELATION    = 0x05660fb0;  // HIGH: fn(char_a, char_b) -> CDiplomaticRelation*
    constexpr uintptr_t GET_OR_CREATE_RELATION     = 0x06678a40;  // HIGH: fn(char_a, char_b) -> CDiplomaticRelation* (creates if absent) — ck-ehbi: was 0x05678a40 (off by 0x01000000, resolved to unrelated graphics fn)
    constexpr uintptr_t RELATION_BINARY_SEARCH     = 0x057e4390;  // HIGH: binary search sorted relation array at core+0x20
    constexpr uintptr_t TRUCE_ADD_ENTRY            = 0x06269460;  // HIGH: fn(rel, char_id, date_ptr, reason_str_ptr, reason_type, only_if_later) — ck-ehbi: was 0x05269460 (resolved to wrapper wrong function)
    constexpr uintptr_t TRUCE_REMOVE_ENTRY         = 0x052694d0;  // HIGH: clear truce from one side
    constexpr uintptr_t TRUCE_EXPIRE               = 0x05268b70;  // HIGH: fn(rel, current_date) -- clean up expired truces
    constexpr uintptr_t TRUCE_REASON_LOOKUP        = 0x05264690;  // HIGH: token ID -> enum (0=attacker,1=defender,2=ally,3=invalid)
    constexpr uintptr_t TRUCE_REASON_REVERSE       = 0x052646c0;  // HIGH: flip attacker/defender perspective
    constexpr uintptr_t RELATION_CREATE            = 0x052701a0;  // HIGH: allocate + init + post-init new CDiplomaticRelation
    constexpr uintptr_t RELATION_ALLOC             = 0x052701e0;  // HIGH: operator_new(0x1F0) with free-list pooling
    constexpr uintptr_t RELATION_INIT              = 0x05266850;  // HIGH: set char_id_low/high from two char ptrs
    constexpr uintptr_t RELATION_POST_INIT         = 0x052668c0;  // HIGH: register relation with both characters
    constexpr uintptr_t TRUCE_ADD_ONEWAY           = 0x04ec6000;  // HIGH: script effect: add truce A->B
    constexpr uintptr_t TRUCE_ADD_BOTHWAYS         = 0x04ec6b90;  // HIGH: script effect: add truce A<->B
    constexpr uintptr_t TRUCE_CANCEL_ONEWAY        = 0x04ec7240;  // HIGH: script effect: remove truce A->B
    constexpr uintptr_t TRUCE_CANCEL_BOTHWAYS      = 0x04ec75a0;  // HIGH: script effect: remove truce A<->B

    // Round 10-4: Hook/secret system functions
    constexpr uintptr_t HOOK_TRIGGER_INIT          = 0x0201ed60;  // HIGH: _INIT_2433, registers all hook/secret triggers
    constexpr uintptr_t HOOK_EFFECT_INIT           = 0x020ce6b0;  // HIGH: _INIT_2487, registers all hook/secret effects
    constexpr uintptr_t HOOK_GET_HOOK              = 0x0388e340;  // HIGH: IntrigueWindowHookItem::GetHook getter
    constexpr uintptr_t HOOK_GET_CHARACTER         = 0x0388e380;  // HIGH: IntrigueWindowHookItem::GetCharacter getter
    constexpr uintptr_t HOOK_GET_HOOKS_ON_ME       = 0x0388eef0;  // HIGH: IntrigueWindow::GetHooksOnMe
    constexpr uintptr_t HOOK_STRENGTH_STATE        = 0x05a6fdd0;  // HIGH: GetHookStrengthState
    constexpr uintptr_t HOOK_HAS_EXPIRY            = 0x05a6fee0;  // HIGH: HasExpirationDate
    constexpr uintptr_t HOOK_GET_EXPIRY            = 0x05a6f560;  // MEDIUM: GetExpirationDate
    constexpr uintptr_t HOOK_GET_FRAME             = 0x05a6fe40;  // MEDIUM: GetHookFrame
    constexpr uintptr_t HOOK_STRONG_COUNT          = 0x0388f020;  // MEDIUM: GetMyStrongHooksCount
    constexpr uintptr_t HOOK_DETAIL_INIT           = 0x02fbc260;  // HIGH: _INIT_5098, hook detail window
    constexpr uintptr_t AI_HOOK_EVALUATOR          = 0x04cb8d40;  // HIGH: CUseHookEffect AI evaluator

    // Round 11-25: CB validity pipeline (full CB enumeration + validation)
    // NOTE: CB_TITLE_VALIDATION and CB_ENUMERATE_CLAIMS defined above in Phase 3 section
    constexpr uintptr_t CB_VALIDITY_BY_ID          = 0x06cce190;  // HIGH: fn(cb, attacker_id, defender_id, err), 133B (R11-25)
    constexpr uintptr_t CB_EXTENDED_TRIGGER         = 0x06ccec00;  // HIGH: 5-trigger CB check (is_valid + allowed_for + allowed_against + valid_to_start + defender), (R11-25)
    constexpr uintptr_t CB_SCOPE_BUILDER            = 0x06ccee80;  // HIGH: build scope objects for CB evaluation, (R11-25)
    constexpr uintptr_t CB_ALREADY_AT_WAR           = 0x065d5490;  // HIGH: int(attacker, defender, war) — check if already at war, 114B (R11-25)
    constexpr uintptr_t CB_UI_POPULATE              = 0x044969a0;  // HIGH: CDeclareWarInteractionWindow CB population (R11-25)
    constexpr uintptr_t CB_TITLE_STANDARD_VALID     = 0x06cca050;  // HIGH: standard title validation for title-targeted CB (R11-25)
    constexpr uintptr_t HAS_CB_ON_TRIGGER           = 0x043082e0;  // HIGH: has_cb_on scripting trigger handler (R11-25)

    // Round 11-6: War declaration command pipeline
    constexpr uintptr_t WAR_CREATE_CORE             = 0x065c9360;  // HIGH: FUN_066c9360, allocate CWar + setup (3433B, source: war_manager.cpp) (R11-6)
    // ck-eyfb: helpers used by WAR_DECLARE_SPECIAL_HANDLER to build WAR_CREATE_CORE args.
    // CORRECTED 2026-06-09: ck-eyfb recorded these two as the raw Ghidra FUN_ name
    // hex (= Ghidra addr) instead of subtracting the 0x100000 image base, so both
    // were +0x100000 too high. Verified against the byte-identical on-disk binary
    // (BuildID e61ff264…) by disassembling the real WAR_DECLARE_SPECIAL_HANDLER
    // body at raw 0x065db970: `call 5f60210` (gs-from-root) then `call 65c9360`
    // (WAR_CREATE_CORE), and `call 75facf0` (lock). The old GAME_STATE_FROM_ROOT
    // value 0x06060210 landed mid-std::vector-realloc — calling it wedged/crashed
    // the game thread at force-path step 4 (gs_resolve), which is exactly the
    // bl-vml1lg / ck-4ot67 "wedge after ENTRY, before war creation" symptom.
    constexpr uintptr_t GAME_STATE_FROM_ROOT        = 0x05f60210;  // HIGH: FUN_06060210 (Ghidra) → raw 0x05f60210; *(root+0xa0)+0x299f0. VERIFIED via disasm (mov $0x299f0,eax; add 0xa0(rdi),rax; ret) + WAR_DECLARE_SPECIAL_HANDLER call site. (ck-eyfb corrected 2026-06-09)
    constexpr uintptr_t GAME_LOCK_ACQUIRE           = 0x075facf0;  // HIGH: FUN_076facf0 (Ghidra) → raw 0x075facf0; the engine's lock-acquire stub is a bare `ret` (no-op) in this build — WAR_DECLARE_SPECIAL_HANDLER@0x65db970 calls it as its first instruction. Old value 0x076facf0 was also a `ret` so the no-op was accidentally correct. (ck-eyfb corrected 2026-06-09)
    constexpr uintptr_t WAR_DB_ALLOCATE             = 0x065d2110;  // HIGH: CGameObjectDB slot allocator for CWar, returns (ptr, id), 232B (ck-veeg)
    constexpr uintptr_t WAR_NAME_GENERATE           = 0x065b62c0;  // HIGH: auto-generate war name from CB data → SSO string at war+0x2D8, 985B (ck-veeg)
    constexpr uintptr_t AI_POPULATE_CB_CANDIDATES   = 0x04d99190;  // HIGH: populate CB candidate array for AI war eval (R11-6)
    // ck-39e1: AI_CB_EVALUATE_ALL helpers (Path A outer-filter RE).
    // Image-base 0x100000 already subtracted; values are SBA-relative.
    // Source: re/findings/re_a1_path_a_re_20260425.md, AI_CB_EVALUATE_ALL decomp.
    constexpr uintptr_t AI_CB_CANDIDATE_APPEND      = 0x04da0840;  // HIGH: append entry to 0x908-stride candidate list (Ghidra: AI_CB_CANDIDATE_APPEND, was FUN_04ea0840) — ck-39e1
    constexpr uintptr_t AI_CB_CANDIDATE_ENTRY_DTOR  = 0x041db2d0;  // HIGH: destructor for 0x908-stride candidate entry (frees sub-CDynArrays @+0x20,+0x68,+0xb8,+0x4e0) — ck-39e1
    constexpr uintptr_t AI_GET_WAR_FRONT_BUFFER     = 0x04d219e0;  // HIGH: returns *(ai_ctrl+0x20)+0x98 (war_data front buf); used by Path C entry[0] pre-seed — ck-39e1
    constexpr uintptr_t AI_CB_TITLE_RELATION_MODE   = 0x06ccf020;  // MEDIUM: classifies title.+0x738 → 0/1/2 (gates truce-required pairs in AI_CB_EVALUATE_ALL +0x374) — ck-39e1
    // ck-xdzo: AI_CB_EVALUATE_ALL Stage-1 bilateral helpers (sub-byte filter
    // chain at decomp lines 222-263). All values are SBA-relative
    // (Ghidra address - 0x100000). See re/findings/re_stage1_bilateral_helpers_ck-xdzo.md
    // #119 WARNING — MISNOMER. This returns char+0x1C0+0x7D0 (when +0x7DC flag) or
    // char+0x1C0+0x578, but the +0x578 branch is the WAR-PARTICIPANT list (==
    // diplomatic_data::WAR_LIST_PTR), NOT alliance partners. A bilateral SIMD_FIND of
    // the DEFENDER char-ptr in this list reports the attacker's WAR opponents as
    // "allied" (the #119 false-positive). Core-dump validated: genuine allies are
    // NEVER in +0x578/+0x7D0 — they live in the relation array (diplomatic_relation::
    // HAS_ALLIANCE @ rel+0x94). Use is_allied_via_relation() for the authoritative
    // bilateral alliance check. The engine's AI_CB_EVALUATE_ALL uses this list for a
    // war-co-belligerent test (like-typed scope find), NOT a general alliance gate.
    constexpr uintptr_t MAYBE_GET_ALLIANCE_LIST     = 0x0668b570;  // MEDIUM/MISNOMER: NOT an alliance-partner list — see #119 note above — ck-xdzo
    constexpr uintptr_t GET_TRUCE_LIST              = 0x0666df20;  // HIGH: returns char+0x1C0+0x6A0 (CDynArray of 8-byte char-ptrs, sorted) — already-named "GetTruceList" in Ghidra — ck-xdzo
    constexpr uintptr_t MAYBE_GET_REALM_STEP        = 0x066539c0;  // MEDIUM: 1-step realm/liege resolver. char+0x1B8!=0: chase realm ptr+0x1C0+vtable[2]+0x10; else: lookup char+0x1B0[+200] in CharacterDB — ck-xdzo
    constexpr uintptr_t MAYBE_GET_REALM_TOP         = 0x0665efe0;  // MEDIUM: iterative version of MAYBE_GET_REALM_STEP — walks realm chain to fixed point (top liege) — ck-xdzo
    constexpr uintptr_t MAYBE_REALM_PREDICATE_INV   = 0x0664adf0;  // MEDIUM: returns true iff char+0x1B8!=0 AND vtable[2]+0x10 on (char+0x1B8+0x1C0) returns 0 — ck-xdzo
    constexpr uintptr_t MAYBE_GET_ACTIVE_FAITH      = 0x066538c0;  // MEDIUM: returns char+0x1A8→+0x258→+0x10 (back-ptr-validated) or DAT_0xc4d6ee0 singleton — likely GetActiveFaith — ck-xdzo
    constexpr uintptr_t MAYBE_FAITH_DOCTRINE_CHECK  = 0x065e4fb0;  // MEDIUM: wrapper for MAYBE_DOCTRINE_LOWER_BOUND(struct+0x68, ushort key); called as (faith, 0x247) at line 260 — ck-xdzo
    constexpr uintptr_t MAYBE_DOCTRINE_LOWER_BOUND  = 0x065e77e0;  // MEDIUM: binary-search ushort key in sorted array at struct[0]; map values at struct[3], returns bool: value > 0 — ck-xdzo
    constexpr uintptr_t SIMD_FIND_LONG              = 0x03875f20;  // HIGH: dispatches std::find(begin,end,key) for 8-byte longs (SSE2 cmov path) — ck-xdzo
    constexpr uintptr_t ARE_AT_WAR                  = 0x06731920;  // HIGH: check existing war between two chars (R11-6)
    constexpr uintptr_t ADD_WAR_TO_CHAR             = 0x06677aa0;  // HIGH: register war on character's war list (R11-6)
    constexpr uintptr_t ADD_WAR_PARTICIPANT         = 0x065b6870;  // HIGH: add character to war side (R11-6)

    // ═══════════════════════════════════════════
    // Realm-change command queue (ck-e58l.17.1)
    // ═══════════════════════════════════════════
    // Subsystem distinct from CSendCharacterInteractionCommand::Execute.
    // The ONLY path from queued items to WAR_CREATE_CORE is:
    //   REALM_QUEUE_DISPATCH → REALM_QUEUE_MAIN_DISPATCH → REALM_QUEUE_PREPROC
    //   (unconditional) → REALM_VASSAL_TRANSFER_WAR → WAR_DECLARE_SPECIAL_HANDLER
    //   → WAR_CREATE_CORE.
    // Bridge's CSendCharacterInteractionCommand pipeline is structurally
    // disjoint from this queue — see ck-e58l.17.1 §4.
    constexpr uintptr_t REALM_QUEUE_DISPATCH         = 0x064ea9b0;  // HIGH: queue-item dispatcher, 25 callers enumerated (ck-e58l.17.1) (ck-e58l.20)
    constexpr uintptr_t REALM_QUEUE_ITEM_ALLOC       = 0x064ea3e0;  // HIGH: item allocator(mgr, type) → 632B item, type at +0x268 (ck-e58l.17.1) (ck-e58l.20)
    constexpr uintptr_t REALM_QUEUE_ITEM_SET_TYPE    = 0x064c9fe0;  // HIGH: sets item+0x268 = type (ck-e58l.17.1) (ck-e58l.20)
    constexpr uintptr_t REALM_QUEUE_MAIN_DISPATCH    = 0x064d4f20;  // HIGH: main dispatcher, unconditional fall-through to preproc at 0x065d5737 (ck-e58l.17.1) (ck-e58l.20)
    constexpr uintptr_t REALM_QUEUE_PREPROC          = 0x064d0b80;  // HIGH: pre-processor, type-gated notifications + UNCONDITIONAL call to vassal-transfer (ck-e58l.17.1) (ck-e58l.20)
    constexpr uintptr_t REALM_VASSAL_TRANSFER_WAR    = 0x064ccb90;  // HIGH: iterates target_list, calls WAR_DECLARE_SPECIAL_HANDLER for titleless rulers (ck-e58l.17.1, ck-e58l.5) (ck-e58l.20)
    constexpr uintptr_t WAR_DECLARE_SPECIAL_HANDLER  = 0x065db870;  // HIGH: sole direct caller of WAR_CREATE_CORE (ck-e58l.5, ck-e58l.17.1)
    constexpr uintptr_t REALM_QUEUE_ITEM_QMOVE       = 0x064eab20;  // HIGH: post-dispatch move pending→completed (ck-e58l.17.1) (ck-e58l.20)

    // Queue-item struct offsets (632 bytes, FUN_065ea3e0 allocation)
    constexpr size_t QITEM_VTABLE         = 0x000;  // HIGH: vtable ptr (ck-e58l.17.1)
    constexpr size_t QITEM_FLAG           = 0x260;  // HIGH: byte — dispatcher skips if non-zero (ck-e58l.17.1)
    constexpr size_t QITEM_TYPE           = 0x268;  // HIGH: int32 — type discriminator (ck-e58l.17.1)
    constexpr size_t QITEM_SIZE           = 0x278;  // HIGH: 632 bytes total (ck-e58l.17.1)

    // Queue-manager struct offsets
    constexpr size_t QMGR_PENDING_ARR     = 0x30;   // HIGH (ck-e58l.17.1)
    constexpr size_t QMGR_PENDING_CNT     = 0x3c;   // HIGH (ck-e58l.17.1)
    constexpr size_t QMGR_COMPLETED_ARR   = 0x48;   // HIGH (ck-e58l.17.1)
    constexpr size_t QMGR_COMPLETED_CAP   = 0x50;   // HIGH (ck-e58l.17.1)
    constexpr size_t QMGR_COMPLETED_CNT   = 0x54;   // HIGH (ck-e58l.17.1)
    constexpr size_t QMGR_ALLOCATOR_VT    = 0x58;   // HIGH (ck-e58l.17.1)
    constexpr size_t QMGR_RECURSION_DEPTH = 0x60;   // HIGH: capped at 0x32 (50) (ck-e58l.17.1)

    // Known type codes for QITEM_TYPE (ck-e58l.17.1 §3.1 — per-type semantics inferred from caller context)
    constexpr uint32_t QITEM_TYPE_TITLE_DESTROY       = 6;     // MEDIUM (ck-e58l.17.1)
    constexpr uint32_t QITEM_TYPE_TITLE_GRANT         = 7;     // MEDIUM (ck-e58l.17.1 — most common; 8 call sites)
    constexpr uint32_t QITEM_TYPE_TITLE_PASS          = 8;     // MEDIUM (ck-e58l.17.1)
    constexpr uint32_t QITEM_TYPE_TITLE_CLAIM         = 9;     // MEDIUM (ck-e58l.17.1)
    constexpr uint32_t QITEM_TYPE_FAITH_CULTURE_GOV   = 0xa;   // MEDIUM (ck-e58l.17.1)
    constexpr uint32_t QITEM_TYPE_TITLE_REVOKE        = 0x12;  // MEDIUM (ck-e58l.17.1; primary type for revoke cascade)
    constexpr uint32_t QITEM_TYPE_ACTIVITY_EFFECT     = 0x13;  // LOW (ck-e58l.17.1 — single caller)
    constexpr uint32_t QITEM_TYPE_SUCCESSION          = 0x15;  // MEDIUM (ck-e58l.17.1)
    constexpr uint32_t QITEM_TYPE_UNINITIALIZED       = 0x17;  // HIGH: default after alloc, dispatcher filters these out (ck-e58l.17.1)

    // ═══════════════════════════════════════════
    // WarParticipationRecord — target_list entry (ck-e58l.17.1.2)
    // ═══════════════════════════════════════════
    // 0x160-byte POD struct (NO vtable at +0x00). Consumed by REALM_VASSAL_TRANSFER_WAR
    // (FUN_065ccb90) as the target_list[] the war-materialization code iterates.
    // Populated upstream by REALM_QUEUE_PREPROC (FUN_065d0b80) via FUN_065d3180
    // from the queue-item's 12-byte (title_id, char_id, char_id) sub-vectors at
    // QITEM+0x58 and QITEM+0x70. The target_list itself is a Jomini growable
    // vector {data_ptr, cap_u32, size_u32, alloc_ref} with 0x160-byte stride.
    //
    // Copy-constructor at FUN_065e6120 touches every scalar byte 0x00..0x47
    // plus the two embedded vectors plus two trailing fields — so the layout
    // below is exhaustive on which BYTES the game writes (semantics of some
    // untouched fields are still open, see §Open Questions in findings).
    //
    // Per-entry push_back: FUN_065e5f90 (stride 0x160, growth factor 1.5×).
    constexpr size_t WPR_SIZE                  = 0x160;  // HIGH (ck-e58l.17.1.2)
    constexpr size_t WPR_TARGET_RULER          = 0x00;   // HIGH: CCharacter* — targeted ruler (IsRuler && held_titles.count==0) (ck-e58l.17.1.2)
    constexpr size_t WPR_CONTEXT_TITLE         = 0x08;   // HIGH: CTitle* — kind-dependent context title; fields +0x22c (parent/de-jure title_id), +0x270 (holder char_id) read via this ptr (ck-e58l.17.1.2)
    constexpr size_t WPR_CLAIM_TITLE           = 0x10;   // HIGH: CTitle* — claim title passed to FUN_061c8d70 to resolve CCasusBelliType from +0x490 of the walked-up title (ck-e58l.17.1.2)
    constexpr size_t WPR_SCALAR_18             = 0x18;   // LOW: u64 — copied by FUN_065e6120; semantics undetermined
    constexpr size_t WPR_SCALAR_20             = 0x20;   // LOW: u32+u32 — copied by FUN_065e6120; semantics undetermined
    constexpr size_t WPR_KIND                  = 0x28;   // HIGH: u32 — kind discriminator. Observed values {1,2,3,4,5,6}. Selects how CONTEXT_TITLE resolves (kind==2: use directly; kind==1: walk title+0x250 to liege title). FUN_065d0b80 processes target_list grouped by KIND in reverse order (6→5→4→3→2→1) (ck-e58l.17.1.2)
    constexpr size_t WPR_SCALAR_2C             = 0x2c;   // LOW: u32 — copied by FUN_065e6120; semantics undetermined
    constexpr size_t WPR_AUX_CTX               = 0x30;   // MEDIUM: ptr — passed as 3rd arg to FUN_0680d830 (post-materialization bookkeeping) (ck-e58l.17.1.2)
    constexpr size_t WPR_SCALAR_38             = 0x38;   // LOW: u32+u32 — copied by FUN_065e6120; semantics undetermined
    constexpr size_t WPR_FALLBACK_TITLE        = 0x40;   // MEDIUM: ptr (likely CTitle*) — 2nd arg to FUN_0676c890 when CASUS_BELLI resolution fails for WPR_CLAIM_TITLE (ck-e58l.17.1.2)
    // Embedded vector A (Jomini growable vector of qwords; inline cap 16):
    constexpr size_t WPR_VEC_A_DATA            = 0x48;   // HIGH: qword data_ptr (initially → +0x68 inline buffer) (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_A_CAP             = 0x50;   // HIGH: u32 capacity (initial 0x10) (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_A_SIZE            = 0x54;   // HIGH: u32 size (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_A_ALLOC_REF       = 0x58;   // HIGH: ptr → embedded allocator vtable at +0x60 (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_A_ALLOC_VT        = 0x60;   // HIGH: &UNK_0b89c9f8 — embedded allocator vtable (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_A_INLINE          = 0x68;   // HIGH: 16 × 8 = 0x80 bytes of inline storage (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_A_EXT_ALLOC_REF   = 0xe8;   // HIGH: &DAT_0bf9f268 — external allocator used after inline overflow (ck-e58l.17.1.2)
    // Embedded vector B (Jomini growable vector of qwords; inline cap 7):
    constexpr size_t WPR_VEC_B_DATA            = 0xf0;   // HIGH: qword data_ptr (initially → +0x110 inline buffer) (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_B_CAP             = 0xf8;   // HIGH: u32 capacity (initial 0x7) (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_B_SIZE            = 0xfc;   // HIGH: u32 size (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_B_ALLOC_REF       = 0x100;  // HIGH: ptr → embedded allocator vtable at +0x108 (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_B_ALLOC_VT        = 0x108;  // HIGH: &UNK_0bd2e350 — embedded allocator vtable (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_B_INLINE          = 0x110;  // HIGH: 7 × 8 = 0x38 bytes of inline storage (ck-e58l.17.1.2)
    constexpr size_t WPR_VEC_B_EXT_ALLOC_REF   = 0x148;  // HIGH: &DAT_0bf9f268 — external allocator (ck-e58l.17.1.2)
    constexpr size_t WPR_TRAIL_U64             = 0x150;  // MEDIUM: u64 copied by FUN_065e6120 (semantics undetermined) (ck-e58l.17.1.2)
    constexpr size_t WPR_TRAIL_FLAG            = 0x158;  // MEDIUM: 1 byte — boolean flag copied by FUN_065e6120 (ck-e58l.17.1.2)
    // bytes 0x159..0x15f: padding to 0x160

    // Functions that manage WarParticipationRecord instances:
    constexpr uintptr_t WPR_COPY_CTOR           = 0x064e6120;  // HIGH: 0x160-byte copy constructor; exhaustive field copy (ck-e58l.17.1.2) (ck-e58l.20)
    constexpr uintptr_t WPR_VECTOR_PUSH_BACK    = 0x064e5f90;  // HIGH: push_back into Jomini vector<WarParticipationRecord>; stride 0x160, growth 1.5× (ck-e58l.17.1.2) (ck-e58l.20)
    constexpr uintptr_t WPR_ENTRY_POPULATE_FROM_TITLE_CHAR = 0x064d3180;  // HIGH: appends/updates target_list entries from (title_id, char_id) sub-vec tuples; signature (qitem, {title_id,char_id_src,char_id_dst}*, flag, target_list, &ctx, kind_override) (ck-e58l.17.1.2) (ck-e58l.20)
    constexpr uintptr_t WPR_SIDE_GETTER         = 0x064dec50;  // HIGH: returns entry for (side=0|1, entry, target_list) (ck-e58l.17.1.2) (ck-e58l.20)
    constexpr uintptr_t WPR_VEC_A_ALLOCATOR_VT  = 0x0b89c9f8;  // HIGH: &UNK_0b89c9f8 (ck-e58l.17.1.2)
    constexpr uintptr_t WPR_VEC_B_ALLOCATOR_VT  = 0x0bd2e350;  // HIGH: &UNK_0bd2e350 (ck-e58l.17.1.2)
    constexpr uintptr_t WPR_EXT_ALLOCATOR_REF   = 0x0bf9f268;  // HIGH: &DAT_0bf9f268 (shared by both vectors) (ck-e58l.17.1.2)

    // Round 11-7: Succession system functions (R11-7 corrected addresses)
    // "Who inherits from me" is compute-on-demand via these functions (not cached).
    constexpr uintptr_t BUILD_SUCCESSION_ORDER      = 0x0643d490;  // HIGH: FUN_0653d490 - 0x100000, dispatcher (case 0=inheritance, 1=election, 2=appointment), 2186B (R11-7)
    constexpr uintptr_t EXECUTE_SUCCESSION          = 0x063f7e40;  // DEPRECATED: wrong address, use EXECUTE_SUCCESSION_REAL (R11-7 error)
    constexpr uintptr_t BUILD_INHERITANCE_SUCC      = 0x0643b180;  // HIGH: FUN_0653b180 - 0x100000, BuildInheritanceSuccession, 4886B (R11-7)
    constexpr uintptr_t BUILD_ELECTION_SUCC         = 0x06427450;  // HIGH: FUN_06527450 - 0x100000, ElectionSuccession (R11-7)
    constexpr uintptr_t BUILD_ELECTION_SUCC_CORE   = 0x06427f50;  // HIGH: FUN_06527f50, enumerate candidates + apply votes + sort
    constexpr uintptr_t GET_ELECTION_STATE         = 0x051d1a10;  // HIGH: FUN_061d1a10, returns *(title+0x280), 7B
    constexpr uintptr_t GET_VOTE_TARGET            = 0x06426800;  // HIGH: FUN_06526800, fn(state, voter_id) → candidate_char_ref, 189B
    constexpr uintptr_t EVAL_VOTE_SCORE            = 0x0642f9e0;  // HIGH: FUN_0652f9e0, fn(election_type, candidate, ctx, tooltip?) → int score
    constexpr uintptr_t AI_ELECTION_VOTE_DECISION  = 0x03e36370;  // HIGH: FUN_04e36370, AI picks best candidate for elector, ~1896B
    constexpr uintptr_t ELECTION_STATE_CLEAR       = 0x06428720;  // HIGH: FUN_06528720, clears candidate/elector/vote arrays
    constexpr uintptr_t ELECTION_STATE_SERIALIZE   = 0x06425d40;  // HIGH: FUN_06525d40, serialization (tags 0x3291/0x328d/0x3296)
    constexpr uintptr_t HAS_ELECTION_VOTE_OF       = 0x064275f0;  // HIGH: FUN_065275f0, checks if char has another's vote across titles
    constexpr uintptr_t BUILD_APPOINTMENT_SUCC      = 0x0643c660;  // HIGH: FUN_0653c660 - 0x100000, AppointmentSuccession, 2587B (R11-7)
    constexpr uintptr_t BUILD_TITLE_HEIR_LIST       = 0x06401880;  // HIGH: BuildTitleHeirList (STitleHeirPair list) (R11-7)
    constexpr uintptr_t GET_HEIR_TITLE_FOR_CHAR     = 0x0664dbe0;  // HIGH: FUN_0674dbe0 - 0x100000, returns ruler_data+0x3F0 (182B, 576+ callers) (R11-7)

    // R-partition: Succession partition calculator functions
    // The partition algorithm distributes titles among heirs using tier-descending
    // greedy assignment with de jure scoring. See re/findings/re_succession_partition.md.
    constexpr uintptr_t EXECUTE_SUCCESSION_REAL     = 0x06435f60;  // HIGH: FUN_06535f60, main succession orchestrator, 10163B (R-partition)
    constexpr uintptr_t HANDLE_PARTITION_DISTRIB    = 0x06444b20;  // HIGH: FUN_06544b20, core partition algorithm, 4774B (R-partition)
    constexpr uintptr_t DISTRIBUTE_TITLES_TO_HEIRS  = 0x06438f50;  // HIGH: FUN_06538f50, title grant loop, 2459B (R-partition)
    constexpr uintptr_t ADD_TITLE_TO_HEIR_ENTRY     = 0x06439e10;  // HIGH: FUN_06539e10, group title into heir entry, 830B (R-partition)
    constexpr uintptr_t IS_PARTITION                = 0x06435470;  // HIGH: FUN_06535470, division==1 && order<2, 15B (R-partition)
    constexpr uintptr_t IS_PARTITION_WITH_FAITH_REQ = 0x06435480;  // HIGH: FUN_06535480, partition + faith_req!=0, 21B (R-partition)
    constexpr uintptr_t BUILD_SUCC_FOR_RULER        = 0x0643ddf0;  // HIGH: FUN_0653ddf0, groups titles by rules and dispatches, 1280B (R-partition)
    constexpr uintptr_t DE_JURE_MATCH               = 0x06122410;  // HIGH: FUN_06222410, checks de jure title relationship (R-partition)
    constexpr uintptr_t GRANT_TITLE                 = 0x060c8e00;  // HIGH: FUN_061c8e00, actual title transfer (R-partition)
    constexpr uintptr_t VALIDATE_HEIR_FOR_TITLE     = 0x064023b0;  // HIGH: FUN_065023b0, heir-title validation (R-partition)
    constexpr uintptr_t IS_HEIR_ELIGIBLE            = 0x06402620;  // HIGH: FUN_06502620, heir eligibility check (R-partition)

    // R-partition-2: Additional partition helper functions
    constexpr uintptr_t FIND_QUALIFYING_DEJURE_LIEGE = 0x063f6b30;  // HIGH: FUN_064f6b30, walks title hierarchy checking game define 0x329f (R-partition-2)
    constexpr uintptr_t PRUNE_PARTITION_CANDIDATE    = 0x063f6c20;  // HIGH: FUN_064f6c20, candidate eligibility (IsRuler+tier+flag), returns 1=keep (R-partition-2)
    constexpr uintptr_t ADD_CANDIDATE_TO_LIST        = 0x06445f40;  // HIGH: FUN_06545f40, appends 0x28-stride candidate to partition array (R-partition-2)
    constexpr uintptr_t IS_IN_DOMAIN                 = 0x06121080;  // HIGH: FUN_06221080, checks de jure domain membership (tier 2 always true) (R-partition-2)
    constexpr uintptr_t CHECK_DYNASTY_PARTITION_PERM = 0x060ccbd0;  // HIGH: FUN_061ccbd0, recursive dynasty/law partition permission check (R-partition-2)

    // R-partition: Partition scoring globals (de jure affinity weights)
    // CONFIRMED via core dump: own=6, other=1 → score = own_matches*6 - other_matches*1
    constexpr uintptr_t PARTITION_DEJURE_OWN_WEIGHT  = 0x0c3a9a68; // int32: value=6, positive weight for de jure own matches (R-partition)
    constexpr uintptr_t PARTITION_DEJURE_OTHER_WEIGHT = 0x0c3a9a80; // int32: value=1, negative weight for de jure other conflicts (R-partition)
    constexpr uintptr_t PARTITION_MAX_HEIR_REASSIGN  = 0x0c3a86f8; // int32: value=6, max heirs per title reassignment in DistributeTitlesToHeirs (R-partition-2)

    // R8V-6: Succession window verification functions
    constexpr uintptr_t GET_SUCCESSION_LAW_FROM_TITLE = 0x04a63750; // HIGH: FUN_04b63750 - 0x100000, reads title+0x380 (R8V-6)
    constexpr uintptr_t GET_DESIGNATED_HEIR          = 0x0668d970;  // HIGH: FUN_0678d970 - 0x100000, reads ruler_data+0x1B4 (R8V-6)
    constexpr uintptr_t GET_DESIGNATED_HEIR_REF      = 0x0664a380;  // HIGH: FUN_0674a380 - 0x100000, returns raw uint32 (R8V-6)
    constexpr uintptr_t COMPARE_SUCCESSION_RULES     = 0x06435520;  // HIGH: FUN_06535520 - 0x100000, CompareSuccessionRules (R8V-6)
    constexpr uintptr_t INIT_SUCCESSION_RULE_FROM_TITLE = 0x064356f0; // HIGH: FUN_065356f0 - 0x100000, InitSuccessionRuleFromTitle (R8V-6)
    constexpr uintptr_t BUILD_TITLE_HEIR_LIST_R8V6   = 0x06401880;  // HIGH: FUN_06501880 - 0x100000, BuildTitleHeirList alt (1285B, iterates title array) (R8V-6)

    // Round 11-9: Military 8-category breakdown functions
    constexpr uintptr_t REGIMENT_QUERY              = 0x06734d80;  // HIGH: compute_regiment_query(char, param2, type_mask, category, 0), 364B (R11-9)
    constexpr uintptr_t MIL_QUALITY_GLOBAL          = 0x06570af0;  // HIGH: global combat quality modifier accessor (R11-9)
    constexpr uintptr_t GET_ALLY_CHARACTERS         = 0x060bde00;  // HIGH: collect ally/vassal character refs, 1516B (R11-9)
    constexpr uintptr_t GET_VASSAL_TITLES           = 0x0676c3d0;  // HIGH: collect vassal title pointers, 412B (R11-9)
    constexpr uintptr_t GARRISON_TROOP_COUNT        = 0x06164f00;  // HIGH: garrison troop count for char_id (R11-9)
    constexpr uintptr_t IS_LANDED_CHECK             = 0x06773040;  // HIGH: landed status check via DAT_0bfd7778 (R11-9)
    constexpr uintptr_t IS_IMPRISONED               = 0x0664adf0;  // HIGH: bool(char) — checks ruler_data→court→is_free, 34B (R11-9)
    constexpr uintptr_t IS_CHAR_IN_LIEGE_CHAIN     = 0x06668520;  // HIGH: bool(char*, uint32 target_id) — walks liege chain, 403B (Phase 31)
    constexpr uintptr_t GET_EMPLOYER_RELATION       = 0x0664a2e0;  // HIGH: get ruler's relation/employer object, 148B (R11-9)
    constexpr uintptr_t MIL_MAX_WITHOUT_HIRES       = 0x0619f3e0;  // HIGH: sum max excluding holy orders+mercs (slots 0,1,4,5,6,7), 30B (R11-9)
    constexpr uintptr_t MIL_TOTAL_QUALITY_CURRENT   = 0x0619f430;  // HIGH: sum quality_current (int64) across 8 slots, 45B (R11-9)
    constexpr uintptr_t MIL_TOTAL_QUALITY_MAX       = 0x0619f400;  // HIGH: sum quality_max (int64) across 8 slots, 45B (R11-9)

    // Round 12: AI Watch Window functions
    constexpr uintptr_t AIWATCH_CONSTRUCTOR          = 0x041d54a0;  // HIGH: CAIWatchWindow ctor, 660B (Ghidra FUN_042d54a0 - 0x100000)
    constexpr uintptr_t AIWATCH_DESTRUCTOR           = 0x041d5b60;  // HIGH: CAIWatchWindow dtor, 1062B (Ghidra FUN_042d5b60 - 0x100000)
    constexpr uintptr_t AIWATCH_POPULATE             = 0x041d3670;  // HIGH: main builder, 5875B (Ghidra FUN_042d3670 - 0x100000)
    constexpr uintptr_t AIWATCH_BIG_UPDATE           = 0x041d62a0;  // HIGH: big update, 5089B (Ghidra FUN_042d62a0 - 0x100000)
    constexpr uintptr_t AIWATCH_DISPATCH_CB          = 0x041da1a0;  // HIGH: CB dispatch, 192B (Ghidra FUN_042da1a0 - 0x100000)
    constexpr uintptr_t AIWATCH_ACTIVITY_BUILD       = 0x041d33a0;  // HIGH: activity build, 636B (Ghidra FUN_042d33a0 - 0x100000)

    // Round 12: AI evaluation sub-functions (called by AIWATCH_POPULATE)
    constexpr uintptr_t AI_EVAL_ACTIVITY             = 0x04cb0130;  // HIGH: activity evaluation (Ghidra FUN_04db0130 - 0x100000)
    constexpr uintptr_t AI_EVAL_STRATEGY             = 0x04cb1aa0;  // HIGH: strategy evaluation (Ghidra FUN_04db1aa0 - 0x100000)
    constexpr uintptr_t AI_EVAL_CB                   = 0x05ab8430;  // HIGH: strategy/CB evaluation (Ghidra FUN_05bb8430 - 0x100000)
    constexpr uintptr_t AI_SCOPE_FROM_CHAR           = 0x05ab3d60;  // HIGH: scope from character (Ghidra FUN_05bb3d60 - 0x100000)
    constexpr uintptr_t AI_WAR_DATA                  = 0x05ab8240;  // HIGH: war data accessor (Ghidra FUN_05bb8240 - 0x100000)
}

// =============================================================================
// CAIWarStanceDatabase / CAIWarCoordinator struct offsets (Fix B1)
// (re_caiwarcoordinator_FINAL.md). Slot offsets are INSIDE the 0x1020-byte DB
// instance pointed at by globals::STANCE_DB_GLOBAL. Each slot = {SAIWarStance*,
// u32 refcount}. Record offsets are on a coordinator war-record (coord+0x50[i]).
// =============================================================================
namespace war_stance {
    constexpr size_t DB_SIZE                = 0x1020;  // HIGH: new char[0x1020]
    // Embedded SAIWarStance* slots (offset into the DB instance):
    constexpr size_t OFF_ATK_OFFENSIVE      = 0xfb0;   // HIGH ← FORCE THIS for a steered conquest
    constexpr size_t OFF_ATK_DEFENSIVE      = 0xfc0;   // HIGH (the default a steered war lands on = the bug)
    constexpr size_t OFF_DEF_OFFENSIVE      = 0xfd0;   // HIGH
    constexpr size_t OFF_DEF_DEFENSIVE      = 0xfe0;   // HIGH
    constexpr size_t OFF_DEF_DESPERATE      = 0xff0;   // HIGH
    constexpr size_t OFF_GHW_ATTACKER       = 0x1000;  // HIGH
    constexpr size_t OFF_GHW_DEFENDER       = 0x1010;  // HIGH

    // Coordinator-war-record (the object STANCE_PERWAR_SELECT mutates):
    constexpr size_t REC_ACTIVE_FLAG        = 0x14;    // HIGH(decomp FUN_04dedd40 gate): byte != 0 ⇒ active record
    constexpr size_t REC_WAR_ID             = 0x18;    // HIGH(decomp FUN_04e60920)/PROBE-confirm §6 re_query_war_stance.md:
                                                       //   record↔war match key. low-24 = WAR_DB slot, high-8 = gen tag;
                                                       //   same value query_wars / query_war_score_breakdown return as war_id.
                                                       //   FUN_04e60920 masks &0xffffff → WarDB slot, validates war+8 == rec+0x18.
    constexpr size_t REC_FLAGS              = 0x68;    // HIGH: u16 bitfield; bit2(0x4)=is-attacker side, bit4(0x10)=cond
    constexpr size_t REC_STANCE_PTR         = 0x70;    // HIGH: SAIWarStance* — the LIVE chosen stance for this war ← the slot
    constexpr size_t REC_REEVAL_CD_1        = 0x94;    // MEDIUM: re-eval cooldown counters (decremented each tick)
    constexpr size_t REC_REEVAL_CD_2        = 0x98;    // MEDIUM
    constexpr size_t REC_REEVAL_CD_3        = 0x9c;    // MEDIUM

    // CORRECTION (re_query_war_stance.md §2c/§7): the per-war coordinator RECORD
    // ARRAY lives on the AI-MANAGER root, NOT at "coord+0x50". FUN_04dedd40 walks
    // arr=*(ai_root+0x48), n=*(ai_root+0x54), rec=*(arr+i*8), active gate rec+0x14.
    constexpr size_t AIMGR_REC_ARRAY        = 0x48;    // HIGH(decomp FUN_04dedd40): base of per-realm coordinator records
    constexpr size_t AIMGR_REC_COUNT        = 0x54;    // HIGH(decomp FUN_04dedd40): count of records

    // COORD_WAR_REC_LIST = 0x50 is NOT the war-record list — it is the in-record
    // province-OBJECTIVE sublist (count at rec+0x5c), iterated by FUN_04e66220.
    // Walk AIMGR_REC_ARRAY/+COUNT for enumerate-all-records (the query mirrors that).
    constexpr size_t COORD_WAR_REC_LIST     = 0x50;    // RE-COMMENT: in-record province-objective sublist, NOT war list
    constexpr size_t REC_OBJ_COUNT          = 0x5c;    // HIGH(decomp FUN_04e66220): int32 objective-sublist count; ==0 ⇒ stance tick early-returns
    constexpr size_t AIOBJ_TO_COORD         = 0x10;    // HIGH: coordinator = *(realm_ai_obj + 0x10)

    // Offensive-vs-defensive condition (re_army_control.md §6, HIGH-decompile):
    // FUN_04e843a0 (image) scores army-strength ratio vs thresholds and returns the
    // bit STANCE_PERWAR_SELECT writes into REC_FLAGS bit 0x10 — THE "decides to
    // siege" predicate. Header value = image VA − 0x100000.
    constexpr uintptr_t OFFENSIVE_CONDITION = 0x04d843a0;  // HIGH: FUN_04e843a0 (img−0x100000)

    // Per-war SIDE STRENGTH TOTALS on the coordinator record — the inputs the
    // selector FUN_04e66550 reads (rec+0x1b28, rec+0x1b30) and passes to the
    // OFFENSIVE_CONDITION comparator. HIGH (decomp FUN_04e66550 lVar15/lVar11 +
    // FUN_04e63700 reads). Fixed-point /100000 aggregate of each side's
    // participants' maintained TOTAL fieldable strength (levies+MaA potential,
    // char+0x1b8->+0x308) — NOT a count of currently-mustered regiments.
    //   attacker offensive ⟺ enemy < own AND (enemy/own) < 0.75  (single-objective war).
    constexpr size_t REC_STRENGTH_ENEMY     = 0x1b28;  // HIGH: long, enemy-side total strength (param_2 to FUN_04e843a0)
    constexpr size_t REC_STRENGTH_OWN       = 0x1b30;  // HIGH: long, own-side total strength  (param_3 to FUN_04e843a0)
    constexpr size_t REC_SOLDIERS_OWN       = 0x1b38;  // MEDIUM: int32, own soldier count (FUN_04e63700 reads)
    // Ratio thresholds (fixed-point /100000), core-dump-verified:
    //   RATIO_HI = 0.75 (_DAT_0c16ecf0): at/above this an attacker is NOT offensive
    //   RATIO_LO = 0.50 (_DAT_0c16ed08): only relevant on the >=0.75 / defender paths
    constexpr long OFFENSIVE_RATIO_HI       = 75000;   // HIGH: enemy/own must be < this for attacker offensive
    constexpr long OFFENSIVE_RATIO_LO       = 50000;   // HIGH
}

// =============================================================================
// CCharacter Struct Offsets
// =============================================================================
namespace character {
    constexpr size_t ID                 = 0x18;   // CONFIRMED: uint32, character ID (lower 24 bits)
    constexpr size_t FIRST_NAME         = 0x20;   // HIGH: std::string, birth/default first name (directly on CCharacter)
    constexpr size_t OVERRIDE_NAME      = 0x40;   // HIGH: std::string, custom override name
    constexpr size_t OVERRIDE_FLAG      = 0x48;   // HIGH: ptr, non-NULL means use OVERRIDE_NAME instead of FIRST_NAME
    // Birth date (R8V-1: FUN_0674c0c0 reads char+0x6C as int16, used for age computation)
    // Age = current_game_date - birth_date. Requires globals DAT_0c3a9770 + DAT_0c3a977c.
    // If char+0x6C < 0, birth date comes from *(*(char+0x1a8) + 0x2F0) instead.
    constexpr size_t BIRTH_DATE_SHORT   = 0x6C;   // HIGH: int16, birth date index (R8V-1 confirmed via FUN_0674c0c0)
    constexpr size_t CULTURE_ID_DIRECT  = 0xB0;   // HIGH: uint32, culture ID (Round 5e confirmed via SetCulture)
    constexpr size_t FAITH_ID_DIRECT    = 0xB4;   // HIGH: uint32, faith ID (Round 5f confirmed via SetFaith)
    constexpr size_t IS_AI_FLAG         = 0x199;  // CONFIRMED: byte
    constexpr size_t DEAD_FLAG          = 0x19C;  // HIGH: byte, nonzero=dead (Round 5e, culture/faith setters skip)
    constexpr size_t REALM_DATA         = 0x1a8;  // CONFIRMED: ptr, realm/landed data (null if unlanded)
    constexpr size_t TITLE_REALM_DATA   = 0x1b8;  // CONFIRMED: ptr, title/realm sub-struct (null if unlanded)
    constexpr size_t BIRTH_DATA         = 0x1b8;  // ALIAS: same as TITLE_REALM_DATA (previously mislabeled)
    constexpr size_t DEATH_DATE         = 0x1c8;  // CONFIRMED: ptr, death date (null = alive)

    // === CONFIRMED sub-pointer structure ===
    constexpr size_t TITLE_DATA         = 0x1a0;  // CONFIRMED: ptr, title/primary holding data
    // REALM_DATA already at 0x1a8
    constexpr size_t DYNASTY_DATA       = 0x1b0;  // CONFIRMED: ptr, dynasty/house data
    // VPS memory: char+0x1b0 is courtier/knight data (NULL = not courtier).
    // The label DYNASTY_DATA above may be a mislabel — the VPS character_struct.md
    // says +0x1a0 = Family/Dynasty, +0x1b0 = Courtier/knight data.
    constexpr size_t COURTIER_DATA      = 0x1b0;  // ALIAS: courtier/knight sub-struct (NULL = not courtier)
    // BIRTH_DATA already at 0x1b8
    constexpr size_t DIPLOMATIC_DATA    = 0x1c0;  // CONFIRMED: ptr, diplomatic sub-struct (wars, truces)
    // DEPRECATED alias — old name was a mislabel; this sub-struct contains war/truce lists, not culture
    constexpr size_t CULTURE_DATA       = DIPLOMATIC_DATA;
    // DEATH_DATE already at 0x1c8

    // === Claims (R25 FIX: NOT on CCharacter directly!) ===
    // Claims are at core_data+0x1E8, NOT char+0x170.
    // Access: core_data = *(char+0x1A8); claims_pdx_array = core_data+0x1E8
    // PDX array struct: +0x00=data_ptr, +0x0C=count
    // DEPRECATED: these offsets are WRONG (char+0x170 was never claims).
    // Claims are at core_data+0x1E8 (see core_data::CLAIMS_ARRAY).
    // Kept uncommented only for backwards compat until all callers are fixed.
    constexpr size_t CLAIM_ARRAY     = 0x170;  // WRONG: compat shim, fix callers to use core_data+0x1E8
    constexpr size_t CLAIM_COUNT     = 0x17C;  // WRONG: compat shim, fix callers to use core_data+0x1E8

    // === House ID (Round 5, HIGH confidence) ===
    constexpr size_t HOUSE_ID_DIRECT    = 0x150;  // HIGH: uint32, house ID (24-bit, directly on char)

    // === Traits (Round 3, HIGH confidence) ===
    constexpr size_t TRAIT_ARRAY        = 0xF0;   // CONFIRMED: ptr to sorted uint32[] of trait IDs
    constexpr size_t TRAIT_COUNT        = 0xFC;   // CONFIRMED: int32

    // === Family sub-struct (Round 3, HIGH confidence) ===
    // Accessed via *(char+0x1a0):
    // +0x00=father_id, +0x04=mother_id, +0x08=real_father_id,
    // +0x0C=house_head_id, +0x10=dynasty_id, +0x18=house_id
    constexpr size_t FAMILY_DATA        = 0x1a0;  // CONFIRMED: ptr to family sub-struct

    // === Succession (Round 3, HIGH confidence; R11-7 clarified) ===
    // These are "titles this char is heir TO" (cached title ID array).
    // Access: core_data = *(char+0x1A8); heir_ptr = *(core_data+0x1B8); count = *(core_data+0x1C4)
    // WARNING: char+0x1B8 is the title_realm pointer — must dereference core_data first!
    // See also: core_data::HEIR_TITLE_IDS / HEIR_TITLE_COUNT (same offsets, canonical names).
    constexpr size_t REALM_HEIR_LIST    = 0x1b8;  // sub-offset from REALM_DATA: ptr to uint32[] title IDs this char inherits
    constexpr size_t REALM_HEIR_COUNT   = 0x1c4;  // sub-offset from REALM_DATA: int32 heir-to title count

    // === Spouse (Round 3, HIGH confidence) ===
    // *(*(char+0x1a8) + 0x288) — 15 accessor functions confirm
    constexpr size_t REALM_SPOUSE       = 0x288;  // sub-offset from REALM_DATA: spouse ptr

    // === Children (Round 3, HIGH confidence) ===
    // *(*(char+0x1a8) + 0x4b8) = sorted uint32 array, count at +0x4c4
    constexpr size_t REALM_CHILDREN_LIST  = 0x4b8;  // sub-offset from REALM_DATA: children ID array
    constexpr size_t REALM_CHILDREN_COUNT = 0x4c4;  // sub-offset from REALM_DATA: int32

    // === CONFIRMED Skills (int32, stride 4, direct on CCharacter) ===
    // Verified via accessor functions SKILL_GET_BASE/SKILL_GET_CALC
    constexpr size_t SKILL_DIPLOMACY_BASE   = 0xBC;   // int32 base (without modifiers)
    constexpr size_t SKILL_MARTIAL_BASE     = 0xC0;   // int32 base
    constexpr size_t SKILL_STEWARDSHIP_BASE = 0xC4;   // int32 base
    constexpr size_t SKILL_INTRIGUE_BASE    = 0xC8;   // int32 base
    constexpr size_t SKILL_LEARNING_BASE    = 0xCC;   // int32 base
    constexpr size_t SKILL_PROWESS_BASE     = 0xD0;   // int32 base
    // Calculated (post-modifier) — matches what the UI displays
    constexpr size_t SKILL_DIPLOMACY    = 0xD4;   // int32 calculated
    constexpr size_t SKILL_MARTIAL      = 0xD8;   // int32 calculated
    constexpr size_t SKILL_STEWARDSHIP  = 0xDC;   // int32 calculated
    constexpr size_t SKILL_INTRIGUE     = 0xE0;   // int32 calculated
    constexpr size_t SKILL_LEARNING     = 0xE4;   // int32 calculated
    constexpr size_t SKILL_PROWESS      = 0xE8;   // int32 calculated

    // === Resources: NOT direct fields — accessed via sub-pointers ===
    // Gold:     *(*(char+0x1a8) + 0x100) — int64 fixed-point (÷100,000)
    // Piety:    *(*(char+0x1a8) + 0x110) — int64 fixed-point (÷100,000)
    // Prestige: *(*(char+0x1a8) + 0x130) — int64 fixed-point (÷100,000)
    // Use accessor functions: GOLD_GET, PIETY_GET, PRESTIGE_GET

    // === Military: accessed via BIRTH_DATA sub-pointer ===
    // Current: *(*(char+0x1b8) + 0x2F0) — int32
    // Max:     *(*(char+0x1b8) + 0x2F4) — int32
    // Use accessor functions: MIL_CURRENT, MIL_MAX

    // === Core data sub-offsets (from *(char + CORE_DATA / 0x1a8)) ===
    // See also: core_data namespace below for the complete set.
    constexpr size_t REALM_GOLD         = 0x100;  // HIGH: int64 fixed-point (gold)
    constexpr size_t REALM_PIETY        = 0x110;  // HIGH: int64 fixed-point (piety)
    constexpr size_t REALM_PRESTIGE     = 0x130;  // HIGH: int64 fixed-point (prestige)
    // WARNING: 0x274 on core_data is NOT the liege ID! It was misidentified.
    // The actual liege is at title_realm+0x448 (LIEGE_PTR, a CCharacter*).
    constexpr size_t REALM_LIEGE_ID     = 0x274;  // WRONG: DO NOT USE for liege. Use title_realm::LIEGE_PTR instead
    constexpr size_t REALM_AI_CTRL      = 0x278;  // HIGH: ptr, AI controller object
    constexpr size_t REALM_PERSONALITY_PTR = 0x308;  // HIGH: ptr to signed char[9]

    // === Title/realm sub-offsets (from *(char + TITLE_REALM_DATA / 0x1b8)) ===
    // See also: title_realm namespace below for the complete set.
    constexpr size_t BIRTH_MIL_CURRENT  = 0x2F0;  // HIGH: int32, current military strength
    constexpr size_t BIRTH_MIL_MAX      = 0x2F4;  // HIGH: int32, max military strength

    // === Compatibility aliases for family sub-struct (offsets within *(char+0x1a0)) ===
    // Prefer family:: namespace for new code.
    constexpr size_t FAMILY_FATHER_ID       = 0x00;  // HIGH: uint32, father char ID
    constexpr size_t FAMILY_MOTHER_ID       = 0x04;  // HIGH: uint32, mother char ID
    constexpr size_t FAMILY_REAL_FATHER_ID  = 0x08;  // HIGH: uint32, biological father
    constexpr size_t FAMILY_HOUSE_HEAD_ID   = 0x0C;  // HIGH: uint32, dynasty house head
    constexpr size_t FAMILY_DYNASTY_ID      = 0x10;  // HIGH: uint32, dynasty ID
    constexpr size_t FAMILY_HOUSE_ID        = 0x18;  // HIGH: uint32, house ID

    // === Compatibility aliases for claim entry (offsets within each CClaim entry) ===
    // Prefer claim_entry:: namespace for new code.
    // DEPRECATED: claim compat aliases — claims are now at core_data+0x1E8, not char+0x170
    // Use core_data::CLAIMS_ARRAY and claim_entry:: namespace instead
    constexpr size_t CLAIM_ENTRY_SIZE   = 0x10;   // HIGH: bytes per CClaim entry
    constexpr size_t CLAIM_TITLE_ID     = 0x08;   // HIGH: uint32, title ID (was TITLE_REF at 0x00, WRONG)
    constexpr size_t CLAIM_PRESSED      = 0x0C;   // HIGH: byte, 1=pressed/strong
    constexpr size_t CLAIM_IMPLICIT     = 0x0D;   // HIGH: byte, 1=inherited

    // === Compatibility aliases for title_realm sub-struct ===
    // Prefer title_realm:: namespace for new code.
    constexpr size_t TITLE_VASSAL_PTR   = 0x1E0;  // MISLEADING: actually held title ID array ptr
    constexpr size_t TITLE_VASSAL_CNT   = 0x1EC;  // MISLEADING: actually held title count
    constexpr size_t TITLE_PRIMARY_PTR  = 0x1F8;  // DEPRECATED: actually capital barony ID, not primary title
    constexpr size_t TITLE_TIER_OFFSET  = 0x1D4;  // MEDIUM: int32, realm tier
    constexpr size_t TITLE_DOMAIN_PTR   = 0x218;  // MEDIUM: domain list ptr
    constexpr size_t TITLE_DOMAIN_CNT   = 0x224;  // MEDIUM: domain count
    constexpr size_t TITLE_LIEGE_PTR    = 0x448;  // HIGH: liege ptr

    // === REMOVED offsets (pre-Round 3 guesses, now known WRONG) ===
    // These are kept as stubs returning 0 so code compiles but produces obviously
    // wrong data, forcing the developer to update usage to correct namespaces.
    // REALM_DOMAIN_SIZE(0x30), REALM_DOMAIN_LIMIT(0x34), REALM_SIZE(0x38),
    // REALM_VASSAL_COUNT(0x3c), REALM_MIL_STRENGTH(0x40), REALM_PRIMARY_TITLE(0x48),
    // REALM_VASSAL_LIST(0x58), REALM_VASSAL_LIST_COUNT(0x60), REALM_TITLE_ID(0x110),
    // REALM_WAR_LIST(0x100), REALM_WAR_COUNT(0x108), GOLD_RESERVED(0x120),
    // GOLD_WAR_CHEST(0x124), GOLD_LONG_TERM(0x128), GOLD_SHORT_TERM(0x12c),
    // REALM_COUNCIL_LIST(0x150), REALM_COUNCIL_COUNT(0x158), SCHEME_LIST_PTR(0x130),
    // SCHEME_COUNT(0x138) — all were unverified guesses, many conflict with Round 3 findings.
    //
    // Temporary stubs for compilation (WRONG VALUES -- must be remapped):
    constexpr size_t REALM_DOMAIN_SIZE  = 0x30;   // WRONG: needs re-mapping via title_realm
    constexpr size_t REALM_DOMAIN_LIMIT = 0x34;   // WRONG: needs re-mapping
    constexpr size_t REALM_SIZE         = 0x38;   // WRONG: needs re-mapping
    constexpr size_t REALM_VASSAL_COUNT = 0x3c;   // WRONG: use title_realm::VASSAL_COUNT
    constexpr size_t REALM_MIL_STRENGTH = 0x40;   // WRONG: use title_realm::MIL_CURRENT
    constexpr size_t REALM_PRIMARY_TITLE= 0x48;   // WRONG: use title_realm::PRIMARY_TITLE_PTR
    constexpr size_t REALM_VASSAL_LIST  = 0x58;   // WRONG: use title_realm::VASSAL_LIST
    constexpr size_t REALM_VASSAL_LIST_COUNT = 0x60; // WRONG: use title_realm::VASSAL_COUNT
    constexpr size_t REALM_WAR_LIST     = 0x100;  // WRONG: needs re-mapping
    constexpr size_t REALM_WAR_COUNT    = 0x108;  // WRONG: needs re-mapping
    constexpr size_t GOLD_RESERVED      = 0x120;  // WRONG: needs re-mapping
    constexpr size_t GOLD_WAR_CHEST     = 0x124;  // WRONG: needs re-mapping
    constexpr size_t GOLD_LONG_TERM     = 0x128;  // WRONG: needs re-mapping
    constexpr size_t GOLD_SHORT_TERM    = 0x12c;  // WRONG: needs re-mapping
    constexpr size_t REALM_COUNCIL_LIST = 0x150;  // WRONG: needs re-mapping
    constexpr size_t REALM_COUNCIL_COUNT= 0x158;  // WRONG: needs re-mapping
    constexpr size_t SCHEME_LIST_PTR    = 0x130;  // WRONG: needs re-mapping
    constexpr size_t SCHEME_COUNT       = 0x138;  // WRONG: needs re-mapping

}

// =============================================================================
// Family Sub-Struct (from *(char + 0x1a0))
// HIGH confidence -- multiple independent accessor functions per field
// =============================================================================
namespace family {
    constexpr size_t FATHER_ID          = 0x00;   // HIGH: uint32, father char ID (0xFFFFFFFF=none)
    constexpr size_t MOTHER_ID          = 0x04;   // HIGH: uint32, mother char ID
    constexpr size_t REAL_FATHER_ID     = 0x08;   // HIGH: uint32, biological father (may differ)
    constexpr size_t HOUSE_HEAD_ID      = 0x0C;   // HIGH: uint32, dynasty house head char ID
    constexpr size_t DYNASTY_ID         = 0x10;   // HIGH: uint32, dynasty ID (resolve via CHARACTER_DB)
    constexpr size_t HOUSE_ID           = 0x18;   // HIGH: uint32, house ID (0xFFFFFFFF=none)

    // R8V-1: Canonical children/spouse paths (UI accessors use these, not core_data paths)
    // FUN_06768150 reads *(*(char+0x1a0) + 0x50) for children array struct
    // FUN_0676f820 reads *(*(char+0x1a0) + 0x68) for spouse array struct
    constexpr size_t SPOUSE_ARRAY       = 0x20;   // VERIFIED: PDX array of uint32 char IDs (count=1 for Heinrich). Was 0x68 (wrong).
    constexpr size_t CHILDREN_ARRAY     = 0x38;   // VERIFIED: PDX array of uint32 char IDs (count=3 for Heinrich). Was 0x50 (wrong).
}

// =============================================================================
// Core Data Sub-Struct (from *(char + 0x1a8))
// Largest sub-struct: resources, spouse, children, AI controller, faith, income
// =============================================================================
namespace core_data {
    // Character name (Round 5, HIGH — std::string with SSO)
    constexpr size_t RAW_NAME           = 0x20;   // HIGH: std::string (SSO), null sentinel DAT_0c4d6870

    // Per-character OWNED scheme ref list (R7-3: FUN_067808e0)
    constexpr size_t SCHEME_REF_PTR     = 0x98;   // HIGH: ptr to uint32[] packed scheme ref IDs
    constexpr size_t SCHEME_REF_COUNT   = 0xA4;   // HIGH: int32, number of scheme refs (0x98 + 0x0C)
    // Per-character INCOMING (targeted-by) scheme ref list — structural sibling
    // (+0x18) of the owned list (GET_INCOMING_SCHEME_LIST FUN_06781200 returns
    // core_data+0xB0). Validated decompile + core dump (re_incoming_schemes.md):
    // 1067 entries, every scheme's target == the owning char. These are the
    // plots AGAINST this character (murder/abduct/fabricate_hook/etc).
    constexpr size_t INCOMING_SCHEME_REF_PTR   = 0xB0;  // HIGH: ptr to uint32[] packed incoming scheme ref IDs
    constexpr size_t INCOMING_SCHEME_REF_COUNT = 0xBC;  // HIGH: int32, count (0xB0 + 0x0C)

    // Resources (int64 fixed-point, divide by 100,000 for float)
    constexpr size_t GOLD               = 0x100;  // HIGH: int64
    constexpr size_t PIETY              = 0x110;  // HIGH: int64
    constexpr size_t PIETY_CURRENT      = 0x118;  // MEDIUM: int64 (4 accessors)
    constexpr size_t PIETY_LEVEL_CAP    = 0x120;  // MEDIUM: int32 (3 accessors)
    constexpr size_t PRESTIGE           = 0x130;  // HIGH: int64
    constexpr size_t PRESTIGE_CURRENT   = 0x138;  // MEDIUM: int64 (4 accessors)
    constexpr size_t PRESTIGE_LEVEL_CAP = 0x140;  // MEDIUM: int32 (3 accessors)

    // Succession heir list — "titles this character is heir TO" (cached)
    // WARNING: These are offsets within core_data (= *(char+0x1A8)), NOT direct char offsets!
    // The bridge was previously reading char+0x1B8 (which is the ruler_data pointer!).
    // Correct access: core_data = *(char+0x1A8); then core_data+0x1B8 / core_data+0x1C4.
    constexpr size_t HEIR_TITLE_IDS     = 0x1B8;  // HIGH: ptr to uint32[] title IDs this char will inherit (4-byte stride)
    constexpr size_t HEIR_TITLE_COUNT   = 0x1C4;  // HIGH: int32, count of titles this char is heir to
    // DEPRECATED aliases (old names were misleading — these are title IDs, not heir char IDs)
    constexpr size_t HEIR_LIST          = 0x1B8;  // DEPRECATED: use HEIR_TITLE_IDS
    constexpr size_t HEIR_COUNT         = 0x1C4;  // DEPRECATED: use HEIR_TITLE_COUNT

    // === Claims (R25 CONFIRMED: FUN_06784ac0) ===
    // CPdxArray<CClaim, int> at core_data+0x1E8
    // PDX array: +0x00=data_ptr (CClaim*[]), +0x0C=count (int32)
    // Each CClaim = 0x10 bytes: +0x00=vtable(8B), +0x08=title_id(u32), +0x0C=pressed(u8), +0x0D=implicit(u8)
    constexpr size_t CLAIMS_ARRAY       = 0x1E8;  // HIGH: PDX array of CClaim entries (R25 decompilation)

    // Identity / relationships
    // WARNING: 0x274 is NOT liege ID! On title objects, +0x274 = designated heir.
    // The actual liege is a CCharacter* at title_realm+0x448 (LIEGE_PTR).
    // See title_realm::LIEGE_PTR for the correct path.
    constexpr size_t LIEGE_ID           = 0x274;  // WRONG: was misidentified as liege, DO NOT USE
    constexpr size_t AI_CONTROLLER      = 0x278;  // HIGH: ptr, AI controller object, 3 accessors
    // DEPRECATED/WRONG: 0x288 is NOT the spouse pointer. Disproved live (pid 680049):
    // Robert (37397) has spouse 37576 yet core_data+0x288 == NULL; prisoners' 0x288
    // points to a 16-byte CImprisonerLink (first qword = imprisoner id, not a vtable).
    // The "15 accessors" are imprisoner-link accessors. Use IMPRISONER_LINK below.
    constexpr size_t SPOUSE_PTR         = 0x288;  // DEPRECATED/WRONG: this is IMPRISONER_LINK, not spouse
    constexpr size_t IMPRISONER_LINK    = 0x288;  // HIGH: ptr to CImprisonerLink (null=free). is_imprisoned == (this != 0). *(uint32*)*link = imprisoner char-id. Verified live (pid 680049): FUN_0674b2d0 engine predicate + 8/1024 prisoners diffed, free chars null. SUPERSEDES the mislabeled SPOUSE_PTR=0x288.

    // Income/expense block (5 sequential fields, all int64 fixed-point / 100000)
    constexpr size_t MONTHLY_INCOME     = 0x2B0;  // MEDIUM: total monthly income (3 accessors)
    constexpr size_t GROSS_DOMAIN_INCOME = 0x2B8; // MEDIUM: gross domain income (3 accessors)
    constexpr size_t NET_DOMAIN_INCOME  = 0x2C0;  // MEDIUM: net domain income (3 accessors)
    constexpr size_t TOTAL_EXPENSES     = 0x2C8;  // MEDIUM: total expenses (3 accessors)
    constexpr size_t NET_GOLD_INCOME    = 0x2D0;  // MEDIUM: net gold income, clamped >= 0 (3 accessors)
    // Compat aliases (old names)
    constexpr size_t INCOME_FIELD_2     = 0x2B8;  // DEPRECATED: use GROSS_DOMAIN_INCOME
    constexpr size_t INCOME_FIELD_3     = 0x2C0;  // DEPRECATED: use NET_DOMAIN_INCOME
    constexpr size_t INCOME_FIELD_4     = 0x2C8;  // DEPRECATED: use TOTAL_EXPENSES
    constexpr size_t INCOME_FIELD_5     = 0x2D0;  // DEPRECATED: use NET_GOLD_INCOME
    constexpr size_t INCOME_ACCUMULATOR = 0x2E8;  // MEDIUM: monthly income accumulator (4 accessors)
    constexpr size_t MONTHLY_INCOME_FP  = 0x2E8;  // ALIAS: same as INCOME_ACCUMULATOR, int64 fixed-point /100000
    constexpr size_t PIETY_LEVEL        = 0x118;  // ALIAS: piety level (int32, same addr as PIETY_CURRENT)
    constexpr size_t PIETY_CAP          = 0x120;  // ALIAS: piety cap, int64 fixed-point /100000
    constexpr size_t PRESTIGE_LEVEL     = 0x138;  // ALIAS: prestige level (int32, same addr as PRESTIGE_CURRENT)
    constexpr size_t PRESTIGE_CAP       = 0x140;  // ALIAS: prestige cap, int64 fixed-point /100000

    // Health (VPS MEMORY.md CONFIRMED: core_data+0x2F0 is health, int64 fixed-point /100000)
    // Previously mislabeled as BIRTH_DATE_ALT in R8V-1; corrected per VPS round 13+ analysis.
    constexpr size_t HEALTH             = 0x2F0;  // HIGH: int64, health value (fp /100000, e.g. 500000 = 5.0)
    constexpr size_t BIRTH_DATE_ALT     = 0x2F0;  // DEPRECATED: actually HEALTH, kept for compat

    // Stress / date (R8V-1 CONFIRMED: 0x2F8 is STRESS, not age)
    // AGE_COUNTER was a misidentification — the UI accessor FUN_0674b790 confirms this is stress (uint32).
    constexpr size_t STRESS             = 0x2F8;  // HIGH: uint32, stress value (R8V-1 confirmed via FUN_0674b790)
    constexpr size_t AGE_COUNTER        = 0x2F8;  // DEPRECATED: actually STRESS, kept for compat
    constexpr size_t DATE_TIMESTAMP     = 0x2FC;  // MEDIUM: int32 (3 accessors, set from game date)

    // Income (R8V-1 NEW: FUN_0674b920 reads core_data+0x170, int64 fixed-point /100000)
    // This is the character window's income display value, distinct from MONTHLY_INCOME at 0x2B0.
    constexpr size_t INCOME_DISPLAY     = 0x170;  // HIGH: int64 fp /100000, monthly income (R8V-1 confirmed via FUN_0674b920)

    // Personality (signed char[9], accessed via pointer chain)
    constexpr size_t PERSONALITY_PTR    = 0x308;  // HIGH: ptr to signed char[9] (10 accessor functions)

    // Pregnancy
    constexpr size_t PREGNANCY_FLAG     = 0x3D8;  // MEDIUM: byte (2 accessors)
    constexpr size_t PREGNANCY_PARTNER  = 0x3E0;  // MEDIUM: ptr, CCharacter* (2 accessors, death-checked)

    // War participation counters (Round 5f confirmed via FUN_06778dd0)
    constexpr size_t WAR_ATK_COUNT      = 0x43A;  // HIGH: uint16, wars as attacker
    constexpr size_t WAR_DEF_COUNT      = 0x43C;  // HIGH: uint16, wars as defender

    // Betrothal / heritage
    constexpr size_t BETROTHAL_FLAG     = 0x438;  // MEDIUM: byte (2 accessors)
    // NOTE: +0x448 was previously labeled SECRETS_LIST but runtime probing shows
    // it points to a culture/heritage object (e.g. "asian_japanese"), NOT secrets.
    // Per-character secrets are accessed via CActiveSecret RefDB (OWNER_CHAR_ID field).
    constexpr size_t HERITAGE_PTR       = 0x448;  // MEDIUM: ptr, culture heritage object (RENAMED from SECRETS_LIST)

    // Children list (sorted uint32[] of char IDs)
    constexpr size_t CHILDREN_LIST      = 0x4B8;  // HIGH: ptr to sorted uint32[]
    constexpr size_t CHILDREN_COUNT     = 0x4C4;  // HIGH: int32

    // Council instance pointer (R7 confirmed, R8V renamed from COUNCIL_MEMBER_PTR)
    constexpr size_t COUNCIL_INSTANCE_PTR = 0x4E8;  // HIGH: ptr to council instance obj (nullable)
    constexpr size_t COUNCIL_OWNER_INLINE = 0x4F0; // HIGH: inline council owner struct (fallback if 0x4E8 null)

    // Title / culture / ruler status
    constexpr size_t CULTURE_ID         = 0x548;  // MEDIUM: int32, via DAT_0bfd88a8 (6 accessors)
    constexpr size_t PRIMARY_TITLE_ID   = 0x548;  // MEDIUM: int32, via DAT_0bfd88a8 (6 accessors)
    // NOTE: CULTURE_ID and PRIMARY_TITLE_ID share 0x548; which DB singleton is
    // used for resolution determines the interpretation.
    constexpr size_t IS_RULER           = 0x54C;  // UNRELIABLE: Ghidra found 0 field accesses. Engine uses char+0x1b8!=0.
}

// =============================================================================
// Character Vitals — Stress & Dread (Round 7, HIGH confidence)
// Stress: via core_data (char+0x1a8), uint32
// Dread: via title_realm (char+0x1b8), int64 fixed-point
// =============================================================================
namespace character_vitals {
    constexpr size_t HEALTH = 0x2F0;        // HIGH: via core_data (char+0x1a8), int64 fixed-point /100000
    constexpr size_t STRESS = 0x2F8;        // HIGH: via core_data (char+0x1a8), uint32
    constexpr size_t MODIFIER_PTR = 0x258;  // HIGH: ptr to CCharacterModifier (RTTI confirmed, re_stress_mechanics.md)
    constexpr size_t DREAD  = 0x350;        // HIGH: via title_realm (char+0x1b8), int64 fixed-point
    constexpr size_t DREAD_SECONDARY    = 0x358;  // MEDIUM: int64, secondary dread (R8V-2 flush)
    constexpr size_t SLANDED_CHAR_DATA_SIZE = 0x4A0;  // SLandedCharacterData total struct size (R8V-2 verified)
}

// =============================================================================
// Stress Mechanics Functions (re_stress_mechanics.md)
// Stress pipeline: AddStress → StressModifierCalc → level detection → events
// Level = stress_raw / 100 (levels 0-3, thresholds at 100/200/300)
// =============================================================================
namespace stress_funcs {
    constexpr uintptr_t GET_STRESS_LEVEL          = 0x0664b7b0;  // HIGH: int(char*), stress/100 (ck-e58l.20)
    constexpr uintptr_t GET_STRESS_LEVEL_AFTER    = 0x06678b90;  // HIGH: int(char*, delta), predicted level after delta (ck-e58l.20)
    constexpr uintptr_t STRESS_MODIFIER_CALC      = 0x06678e50;  // HIGH: int(char*, delta), apply gain/loss modifiers (ck-e58l.20)
    constexpr uintptr_t IS_PLAYER_CHARACTER       = 0x06164320;  // HIGH: bool(char*), checks g_PlayerCharacterID (ck-e58l.20)
    constexpr uintptr_t STRESS_IMPACT_TRAIT       = 0x04f020a0;  // MEDIUM: trait-based stress impact calculation
    constexpr uintptr_t ON_STRESS_LEVEL_INCREASE  = 0x061c1c70;  // MEDIUM: event dispatch for level increase (ck-e58l.20)
    // ON_STRESS_LEVEL_DECREASE removed (ck-audit-B): the former 0x061c1b50 entry
    // was a mislabel — that function is the generic on-action-manager dispatch
    // (Ghidra: OnActionManager_Dispatch), not a stress-specific handler. Use
    // event_weight::ON_ACTION_DISPATCH_BY_ID for the canonical reference.
    constexpr uintptr_t TRIGGER_MENTAL_BREAK      = 0x05ecf360;  // MEDIUM: mental break event (player char only) (ck-e58l.20)
}

namespace stress_globals {
    // NOTE: ck-e58l.16 follow-up verified via core-dump read that these WERE Ghidra-VA.
    // Converted to raw below. Previous values (0x0c3a92d0 / 0x0c3a92b8) were Ghidra-VA
    // and read as 1 / 0 respectively — incorrect. Raw values read 100 / 3 — correct.
    constexpr uintptr_t STRESS_PER_LEVEL          = 0x0c2a92d0;  // HIGH: int32 = 100, core-dump verified 2026-04-21
    constexpr uintptr_t MAX_STRESS_LEVEL          = 0x0c2a92b8;  // HIGH: int32 = 3, core-dump verified 2026-04-21
    // NOTE: ck-e58l.16 showed the "stress event" singleton at 0x0c0b0768 is actually
    // the generic on-action manager (same address as on_action_dispatch::
    // ON_ACTION_MGR_WAR_SINGLETON). Removed from this namespace to avoid duplication;
    // use on_action_dispatch::ON_ACTION_MGR_WAR_SINGLETON (raw 0x0bfb0768).
}

namespace stress_modifiers {
    constexpr int STRESS_GAIN_MULT_INDEX = 0x7E;   // HIGH: modifier collection index for stress gain mult
    constexpr int STRESS_LOSS_MULT_INDEX = 0x7F;   // HIGH: modifier collection index for stress loss mult
    constexpr int SKILL_WEIGHT_1_INDEX   = 0x30;   // MEDIUM: additional stress loss weight factor 1
    constexpr int SKILL_WEIGHT_2_INDEX   = 0x31;   // MEDIUM: additional stress loss weight factor 2
}

// =============================================================================
// Lifestyle / Perk Offsets (Round 7, relative to core_data = char+0x1a8)
// Perk objects follow standard DB pattern: name string at +0x10.
// =============================================================================
namespace lifestyle {
    constexpr uintptr_t PERK_LIST_PTR      = 0x220;  // core_data + offset, pointer to array of perk object pointers
    constexpr uintptr_t PERK_LIST_CAPACITY = 0x228;  // core_data + offset
    constexpr uintptr_t PERK_LIST_COUNT    = 0x22C;  // core_data + offset, int32
    constexpr uintptr_t LIFESTYLE_XP_ARRAY = 0x208;  // core_data + offset, ptr to array of 16B entries {lifestyle_ptr(8B), xp_int64(8B)}
    constexpr uintptr_t LIFESTYLE_XP_COUNT = 0x214;  // core_data + offset, int32 count of XP entries
    constexpr size_t ACTIVE_FOCUS_PTR     = 0x280;  // HIGH: ptr to CActiveFocus (nullable, NULL = no focus) (R9-7)
    // Round 9: RTTI types discovered
    // CActiveFocus typeinfo: 0x0bdc5830
    // SLifestyleXpData typeinfo: 0x0bdca1f0
    // UI property init: _INIT_4788 at 0x02f11fe0 (GetSelectedFocus, GetPerkPoints, GetLifestyleXp)
}

// =============================================================================
// CActiveFocus Struct Offsets (R9-7, from focus getter/setter decompilation)
// Accessed via core_data + ACTIVE_FOCUS_PTR (0x280)
// =============================================================================
namespace active_focus {
    constexpr size_t FOCUS_TYPE_PTR = 0x08;  // HIGH: SFocusType* pointer
    constexpr size_t DATE_SET       = 0x10;  // HIGH: int64, date focus was set
    constexpr size_t PROGRESS       = 0x18;  // MEDIUM: int32, progress/count
    constexpr size_t STRUCT_SIZE    = 0x28;  // CActiveFocus total size
}

// =============================================================================
// CLifestylePerk Struct Offsets (core dump validated, RTTI confirmed)
// Total struct size: 0x480 bytes. 162 perks across 6 lifestyles.
// Access via CLifestylePerkDatabase: entries at DB+0x88, count at DB+0x90.
// See re/findings/re_lifestyle_perk_selection.md for full perk tree.
// =============================================================================
namespace lifestyle_perk {
    constexpr size_t VTABLE          = 0x000;  // HIGH: ptr, CLifestylePerk vtable
    constexpr size_t DB_ID           = 0x008;  // HIGH: uint32, database index (0-161)
    constexpr size_t NAME_HASH       = 0x00C;  // HIGH: uint32, hash of name key
    constexpr size_t NAME_KEY        = 0x010;  // HIGH: std::string, perk name (e.g. "thoughtful_perk")
    constexpr size_t TREE_NODE       = 0x050;  // HIGH: std::string, tree node type ("node_*" or "trait_*")
    constexpr size_t EFFECT_DESC_PTR = 0x040;  // MEDIUM: ptr to CDynamicDescription::CSingleDesc
    constexpr size_t SOURCE_FILE     = 0x250;  // HIGH: std::string, source .txt path
    constexpr size_t EFFECT_KEY      = 0x268;  // HIGH: std::string, effect key (e.g. "name:effect")
    constexpr size_t LIFESTYLE_PTR   = 0x300;  // HIGH: SLifestyle*, parent lifestyle object
    constexpr size_t PREREQ_DATA     = 0x308;  // HIGH: ptr, PDX array of CLifestylePerk* prereqs
    constexpr size_t PREREQ_COUNT    = 0x314;  // HIGH: int32, prerequisite count (0 = root perk)
    constexpr size_t STRUCT_SIZE     = 0x480;  // HIGH: total struct size
}

// CGameDatabase common entry access pattern (works for Perk/Lifestyle/Focus DBs)
namespace game_db {
    constexpr size_t ENTRIES_ARRAY   = 0x088;  // HIGH: ptr to typed entry pointer array
    constexpr size_t ENTRY_COUNT     = 0x090;  // HIGH: uint32, number of entries
}

// =============================================================================
// Title/Realm Sub-Struct (from *(char + 0x1b8))
// Only populated for landed characters.
// =============================================================================
namespace title_realm {
    constexpr size_t TITLE_DB_ID        = 0x10;   // HIGH: int32, title DB lookup key (7 accessors)
    constexpr size_t TITLE_LIST         = 0x68;   // HIGH: ptr, title list array (6 accessors)
    constexpr size_t TITLE_COUNT        = 0x74;   // HIGH: int32 (6 accessors)

    // Faction list (Round 5f confirmed)
    constexpr size_t FACTION_LIST       = 0x120;  // HIGH: ptr, faction ID array
    constexpr size_t FACTION_COUNT      = 0x12C;  // HIGH: int32, count (+0xC from list ptr)

    constexpr size_t CAPITAL_BARONY_ID  = 0x1F8;  // HIGH: uint32, capital barony title ID (NOT primary title! -1 = no title)
    // DEPRECATED aliases — 0x1F8 is the capital barony, not the primary (highest-tier) title.
    // Real primary title = first element of TITLE_ARRAY_PTR (held title array).
    constexpr size_t PRIMARY_TITLE_ID   = 0x1F8;  // DEPRECATED: actually capital barony ID, use TITLE_ARRAY_PTR[0] instead
    constexpr size_t PRIMARY_TITLE_PTR  = 0x1F8;  // DEPRECATED: alias for CAPITAL_BARONY_ID
    constexpr size_t SUB_REALM_TITLES   = 0x1C8;  // HIGH: ptr, sub-realm/held titles list (8 accessors)
    constexpr size_t REALM_TIER         = 0x1D4;  // MEDIUM: int32 (7=empire) (3 accessors)
    // Government type (20260614, runtime-resolved — gov is DATA-DRIVEN so no static
    // symbol/string; found by scanning a live ruler's title_realm graph for an
    // object whose key std::string ends in "_government"). title_realm+0x3F0 → a
    // CGovernmentType*; its key std::string is at gov_obj+0x10. Verified across
    // mandala/celestial/administrative/nomad governments. (ck-51)
    constexpr size_t GOVERNMENT_TYPE_PTR = 0x3F0;  // HIGH: ptr to CGovernmentType (name std::string @ +0x10)

    // Law container (Round 5e confirmed)
    constexpr size_t LAW_CONTAINER      = 0x200;  // HIGH: ptr, CLaw array

    // WARNING: Despite the name, these are HELD TITLE ID arrays, NOT vassal char IDs.
    // Confirmed by ANALYSIS_title_chain.md: FUN_0674db20 iterates these as title IDs.
    // For vassal enumeration, use liege-check via core_data::LIEGE_ID instead.
    constexpr size_t VASSAL_LIST        = 0x1E0;  // HIGH: ptr, ACTUALLY held title ID array (MISLEADING NAME)
    constexpr size_t VASSAL_COUNT       = 0x1EC;  // HIGH: int32, ACTUALLY held title count (MISLEADING NAME)

    // NOTE: 0x218 is the VASSAL CONTRACT array (same as VASSAL_CONTRACT_ARRAY below), NOT domain list
    constexpr size_t DOMAIN_LIST        = 0x218;  // DEPRECATED: actually vassal contract array (use VASSAL_CONTRACT_ARRAY)
    constexpr size_t DOMAIN_COUNT       = 0x224;  // DEPRECATED: actually vassal contract count

    // Military strength (cached area, flushed atomically from working area)
    // R8V_3 VERIFIED, R11-9 CORRECTED semantics from compute_military_from_8_categories (FUN_068c3ac0)
    constexpr size_t MIL_CURRENT        = 0x2F0;  // HIGH: int32, total current soldiers (sum 8 categories) (R11-9: param_2+0x60)
    constexpr size_t MIL_MAX            = 0x2F4;  // HIGH: int32, total max soldiers (sum 8 categories) (R11-9: param_2+0x64)
    constexpr size_t MIL_HIREABLE       = 0x2F8;  // HIGH: int32, hireable troops (holy orders if not imprisoned, mercs if hired) (R11-9: param_2+0x68)
    constexpr size_t MIL_WITHOUT_HIRES  = 0x2FC;  // HIGH: int32, max soldiers excluding holy orders+mercs (slots 0,1,4,5,6,7) (R11-9: param_2+0x6C)
    constexpr size_t MIL_OWN_DOMAIN_MAX = 0x300;  // HIGH: int32, slot 0 max (own domain levy max) (R11-9: param_2+0x70)
    constexpr size_t MIL_QUALITY_CURRENT_I64 = 0x308; // HIGH: int64, total quality current (sum 8 categories) (R11-9: param_2+0x78)
    constexpr size_t MIL_QUALITY_MAX_I64 = 0x310; // HIGH: int64, total quality max (sum 8 categories) (R11-9: param_2+0x80)

    // Compatibility aliases (DEPRECATED -- use R11-9 corrected names above)
    constexpr size_t MIL_ATTACKING      = 0x2F4;  // DEPRECATED: use MIL_MAX
    constexpr size_t MIL_LEVIES         = 0x2F8;  // DEPRECATED: use MIL_HIREABLE
    constexpr size_t MIL_CONDITIONAL    = 0x2FC;  // DEPRECATED: use MIL_WITHOUT_HIRES
    constexpr size_t MIL_THIRD_METRIC   = 0x2FC;  // DEPRECATED: use MIL_WITHOUT_HIRES
    constexpr size_t MIL_REDUCED_TOTAL  = 0x300;  // DEPRECATED: use MIL_OWN_DOMAIN_MAX
    constexpr size_t MIL_DIRECT_VALUE   = 0x300;  // DEPRECATED: use MIL_OWN_DOMAIN_MAX
    constexpr size_t MIL_EXT_1_I64      = 0x308;  // DEPRECATED: use MIL_QUALITY_CURRENT_I64
    constexpr size_t MIL_EXT_2_I64      = 0x310;  // DEPRECATED: use MIL_QUALITY_MAX_I64
    constexpr size_t MIL_MAX_I64        = 0x308;  // DEPRECATED: use MIL_QUALITY_CURRENT_I64
    constexpr size_t MIL_CURRENT_I64    = 0x310;  // DEPRECATED: use MIL_QUALITY_MAX_I64

    // Held title array (PDX array: data_ptr at +0x00, count at +0x0C from array base)
    // Round 8: These ARE the held title IDs (uint32), confirmed via title DB cross-ref
    constexpr size_t TITLE_ARRAY_PTR    = 0x1E0;  // HIGH: ptr to uint32[] of held title IDs
    constexpr size_t TITLE_ARRAY_COUNT  = 0x1EC;  // HIGH: int32, DIRECTLY-held title count (ALL tiers).
                                                  // NOT realm size: flat when a vassal grows. For true
                                                  // vassal-inclusive county count use the top-liege walk
                                                  // (count_realm_counties) — re_realm_size_vassal_inclusive.md (ck-realmsize)

    // Direct vassal array (PDX array of int32 char IDs)
    constexpr size_t DIRECT_VASSAL_ARRAY     = 0xA8;   // HIGH: PDX array base (data at +0x00, count at +0x0C)
    // Indirect vassal array (PDX array of int32 char IDs)
    constexpr size_t INDIRECT_VASSAL_ARRAY   = 0xC0;   // HIGH: PDX array base (data at +0x00, count at +0x0C)
    // Fourth vassal-like array (unknown categorization, same PDX int32 pattern)
    constexpr size_t FOURTH_VASSAL_ARRAY     = 0xD8;   // HIGH: PDX array int32 (FUN_0674f680)
    // Vassal contract array (PDX array of int32 contract IDs)
    constexpr size_t VASSAL_CONTRACT_ARRAY   = 0x218;  // HIGH: PDX array base

    // Ruler tier (1=barony..5=empire)
    constexpr size_t TIER                    = 0x1D4;  // HIGH: uint32, ruler tier

    // Cached limits (game recomputes periodically)
    constexpr size_t DOMAIN_LIMIT_CACHED     = 0x3D0;  // HIGH: int32, cached domain limit
    constexpr size_t VASSAL_LIMIT_CACHED     = 0x3D4;  // HIGH: int32, cached vassal limit
    constexpr size_t VASSAL_COUNT_CACHED     = 0x3D8;  // HIGH: int32, vassal count EXCLUDING barons (what player sees)
    constexpr size_t IS_POWERFUL_VASSAL      = 0x1D9;  // HIGH: byte, true if this char is a powerful vassal of their liege

    // Designated heir (R8V-6 verified via GetDesignatedHeir FUN_0678d970)
    constexpr size_t DESIGNATED_HEIR_REF     = 0x1B4;  // HIGH: uint32 character ref, designated heir (R8V-6)

    // Succession: single heir title ref (R11-7)
    // For living rulers: ruler_data+0x3F0 stores a single heir-title reference.
    // For dead chars: death_data+0x88 stores the heir title ref instead.
    // Getter: FUN_0674dbe0 (182B, 576+ callers)
    constexpr size_t SINGLE_HEIR_REF         = 0x3F0;  // HIGH: heir title ref for "who inherits from me" (R11-7)

    // Army list (from disband function FUN_03e00da0)
    // NOTE: ARMY_CAPACITY/ARMY_COUNT at 0x3D0/0x3D4 conflict with DOMAIN_LIMIT_CACHED/VASSAL_LIMIT_CACHED.
    // The Round 8 RE (domain/vassal limits) supersedes the Round 6b army guess for these offsets.
    constexpr size_t ARMY_ARRAY         = 0x3C8;  // MEDIUM: ptr to army pointer array

    // War ref array (CPdxArray<uint32> of war refs, stride 4)
    // Confirmed R7-4: FUN_06757e50 returns *(char+0x1B8)+0x318
    constexpr size_t WAR_REF_ARRAY      = 0x318;  // HIGH: CPdxArray base (data_ptr at +0x00, count at +0x0C)

    // MaA regiment arrays (Round 10-6, CPdxArray<uint32> regiment refs)
    constexpr size_t PERSONAL_MAA_PTR   = 0x108;  // HIGH: CPdxArray<uint32> personal MaA regiment refs (data at +0x00, count at +0x0C)
    constexpr size_t MERC_REGIMENT_PTR  = 0x138;  // HIGH: CPdxArray<uint32> mercenary company regiment refs
    constexpr size_t HOLY_ORDER_REG_PTR = 0x150;  // HIGH: CPdxArray<uint32> holy order regiment refs

    // Tributary array (Round 10-5, CPdxArray<uint32> tributary char refs)
    constexpr size_t TRIBUTARY_ARRAY    = 0x248;  // HIGH: CPdxArray<uint32> tributary refs (data at +0x00, count at +0x0C)

    // MaA regiment ref array for army maintenance calc (Round 10-5)
    constexpr size_t MAA_REGIMENT_REFS  = 0x278;  // HIGH: CPdxArray<uint32> MaA regiment refs (used by army maintenance)

    // MaA owner link array (Round 10-6, CMaaOwnerLink[], stride 0x38)
    constexpr size_t MAA_OWNER_LINKS    = 0x290;  // HIGH: CMaaOwnerLink array (stride 0x38, used by military strength calc)

    // Title MaA regiment array (Round 10-6)
    constexpr size_t TITLE_MAA_REG_PTR  = 0x2A8;  // HIGH: CPdxArray<uint32> title MaA regiment refs

    // Heir-to-my-titles data (R11-7: accessed by FUN_0674dbe0 / GET_HEIR_TITLE_FOR_CHAR)
    // For dead chars, the equivalent is at death_data+0x88 instead.
    constexpr size_t HEIR_TO_MY_TITLES  = 0x3F0;  // DEPRECATED: use SINGLE_HEIR_REF (same offset, better name)

    // Barter goods accumulator (Round 10-5: income balance writer FUN_0675e550)
    constexpr size_t BARTER_ACCUMULATOR = 0x488;  // HIGH: int64 fp?, barter goods accumulator (income balance target)

    // Barter goods (int64 fixed-point /100000)
    constexpr size_t BARTER_GOODS       = 0x490;  // HIGH: int64, barter goods currency

    // Liege
    // WARNING (independence-resolver fix, 2026-06-20): +0x448 is NOT a reliable liege
    // CCharacter* in this build. It is NULL for most landed rulers (including genuine
    // vassals) and, for many independents, a non-null *mapped sentinel/economy* object
    // whose +0x18 char-id reads 0xFFFFFF (occasionally junk e.g. 0x6) — it does NOT point
    // to a liege. The AUTHORITATIVE liege is PATH2: realm_parent(+0x1c0)→holder(+0x28)→
    // id(+0x18); 0xFFFFFF is the engine's no-liege sentinel (normalize to 0). Use
    // get_liege_id_from_addr (dispatch_router.cpp), do NOT read +0x448 raw for independence.
    // FIX #liege_id (2026-06-24): get_liege_id_from_addr now DB-validates the resolved id
    // (validate_liege_id): the candidate's char-DB entry pointer must equal the pointer it was
    // read from, so a small garbage/stale id (e.g. the live liege_id=3 symptom mid-recompute)
    // is rejected on BOTH paths rather than leaking through the reject-set-only gate.
    constexpr size_t LIEGE_PTR          = 0x448;  // MEDIUM: ptr, UNRELIABLE liege signal — see WARNING; use PATH2 realm-hierarchy
    constexpr size_t SECONDARY_LIEGE    = 0x450;  // MEDIUM: ptr (3 accessors)

    // Realm parent object pointer (fallback liege resolution, mirrors FUN_067539c0)
    constexpr size_t REALM_PARENT_OBJ   = 0x1C0;  // ptr to parent realm obj (liege's title data)
}

// Realm parent object (pointed to by title_realm+0x1C0)
namespace realm_parent {
    constexpr size_t HOLDER_CHAR_PTR    = 0x28;   // CCharacter* of the holder
}

// =============================================================================
// Courtier Data Sub-Struct Offsets (from *(char + COURTIER_DATA / 0x1b0))
// VPS character_struct.md: courtier/knight data, NULL for non-courtiers
// =============================================================================
namespace courtier_data {
    constexpr size_t LIEGE_CHAR_REF     = 0xC8;   // HIGH: uint32 packed char ref, liege of this courtier
    constexpr size_t KNIGHT_ARMY_INDEX  = 0xF8;   // HIGH: int32, knight army index (-1 = not raised)
    constexpr size_t KNIGHT_STATUS      = 0x10C;  // HIGH: int32, 0=forced, 1=default, 2=disallowed
}

// =============================================================================
// Diplomatic Data Sub-Struct Offsets (from *(char + DIPLOMATIC_DATA / 0x1c0))
// HIGH confidence — decompiled accessor, Round 5
// =============================================================================
namespace diplomatic_data {
    // Legitimacy (int64 fixed-point /100000)
    constexpr size_t LEGITIMACY         = 0x28;   // HIGH: int64, legitimacy score

    // War list: DEPRECATED — produces duplicate/stale entries (R7-4, R8V4 confirmed)
    // Use title_realm::WAR_REF_ARRAY (char+0x1B8 +0x318) instead, which is CPdxArray<uint32> war refs.
    constexpr size_t WAR_LIST_PTR       = 0x578;  // DEPRECATED: ptr to war ptr array (use title_realm::WAR_REF_ARRAY)
    constexpr size_t WAR_LIST_COUNT     = 0x584;  // DEPRECATED: int32, war count (use title_realm::WAR_REF_ARRAY+0x0C)
    // Truce list: R8V4 confirmed via FUN_0676df20 — returns *(*(char+0x1c0)+0x6a0)
    constexpr size_t TRUCE_LIST_PTR     = 0x6A0;  // HIGH: ptr to truce ptr array
    constexpr size_t TRUCE_LIST_COUNT   = 0x6AC;  // HIGH: int32, truce count
}

// =============================================================================
// CDiplomaticRelation Struct (0x1F0 bytes, Round 10-3)
// Pairwise container tracking ALL diplomatic state between two characters:
// truces, alliances, wars, raids, etc. Allocated via operator_new(0x1F0).
//
// Access pattern:
//   core_data = *(char + 0x1A8)
//   rel_array = *(uint**)(core_data + 0x20)   // sorted by other char ID
//   rel_count = *(int*)(core_data + 0x2C)
//   Each entry: +0x00=key(uint32 other_char_id), +0x08=CDiplomaticRelation*
//   Entry stride = 0x10. Binary search via funcs::RELATION_BINARY_SEARCH.
//   Miss returns globals::RELATION_NULL_SENTINEL.
//
// Char IDs stored sorted: +0x08 = min(id_a, id_b), +0x0C = max(id_a, id_b).
// Truce slots use stride 0x30. Side A at base +0x28, Side B at base +0x58.
// Slot active when expiry_date != sentinel (globals::TRUCE_SENTINEL_DATE).
//
// Truce reason enum (FUN_06364690):
//   0=attacker, 1=defender, 2=ally, 3=invalid/expired
// Tooltip relation types: 0=war_enemy, 1=war_enemy_ally, 2=war_ally,
//   3=war_enemy_enemy, 4=raid_on_me, 5=raid_by_me, 6=self, 7=alliance,
//   8=tributary, 9=truce, 10=liege, 11+=other
// =============================================================================
namespace diplomatic_relation {
    constexpr size_t TOTAL_SIZE         = 0x1F0;  // HIGH: CDiplomaticRelation struct size
    constexpr size_t VTABLE             = 0x00;   // HIGH: ptr, vtable/metadata
    constexpr size_t CHAR_ID_LOW        = 0x08;   // HIGH: uint32, min(char_a_id, char_b_id)
    constexpr size_t CHAR_ID_HIGH       = 0x0C;   // HIGH: uint32, max(char_a_id, char_b_id)

    // --- Side A truce (for CHAR_ID_LOW) ---
    constexpr size_t TRUCE_EXPIRY_A     = 0x28;   // HIGH: int32, CK3 date (8-byte write, hi32 likely zero)
    constexpr size_t TRUCE_REASON_STR_A = 0x30;   // HIGH: std::string (0x20 bytes), war name/CB
    constexpr size_t TRUCE_REASON_TYPE_A = 0x50;  // HIGH: int32, reason enum (0-3)

    // --- Side B truce (for CHAR_ID_HIGH) ---
    constexpr size_t TRUCE_EXPIRY_B     = 0x58;   // HIGH: int32, CK3 date
    constexpr size_t TRUCE_REASON_STR_B = 0x60;   // HIGH: std::string (0x20 bytes)
    constexpr size_t TRUCE_REASON_TYPE_B = 0x80;  // HIGH: int32, reason enum

    constexpr size_t TRUCE_SLOT_STRIDE  = 0x30;   // HIGH: bytes per truce side

    // --- Hook pointers within CActiveRelation (CDiplomaticRelation) ---
    constexpr size_t HOOK_PTR_A         = 0x10;   // HIGH: CActiveHook* for charA (CHAR_ID_LOW holder)
    constexpr size_t HOOK_PTR_B         = 0x18;   // HIGH: CActiveHook* for charB (CHAR_ID_HIGH holder)

    // --- Alliance flag (#119) ---
    // The AUTHORITATIVE bilateral alliance signal. HasAllianceInRelation (engine
    // 0x6369900, RE-VERIFIED) is literally `return *(int*)(rel+0x94) != 0;`, and
    // IsAlliedTo (0x6750c50 header) gates its result on this after binary-searching
    // the relation array. Nonzero == the two characters of this CDiplomaticRelation
    // are allied. Core-dump validated (ck3_20260421_195552): genuine allies found
    // ONLY via this field; the +0x578 "alliance list" the bridge formerly searched
    // contained WAR participants and NEVER the real ally (see re_alliance_false_positive_119.md).
    constexpr size_t HAS_ALLIANCE       = 0x94;   // HIGH: int32, alliance count/flag (!=0 == allied) — HasAllianceInRelation

    // Remaining fields +0x84..+0x1EF: other diplomatic state (wars, alliances, raids)

    // Truce reason enum values
    constexpr int TRUCE_REASON_ATTACKER = 0;
    constexpr int TRUCE_REASON_DEFENDER = 1;
    constexpr int TRUCE_REASON_ALLY     = 2;
    constexpr int TRUCE_REASON_INVALID  = 3;
}

// =============================================================================
// CActiveHook (0x28 bytes) — a hook one character holds on another
// =============================================================================
namespace active_hook {
    constexpr size_t TOTAL_SIZE         = 0x28;   // HIGH: CActiveHook struct size
    constexpr size_t HOOK_TYPE_PTR      = 0x08;   // HIGH: SHookType* (hook type definition)
    constexpr size_t EXPIRY_DATE        = 0x10;   // HIGH: int64, CK3 date when hook expires
    constexpr size_t END_DATE           = 0x18;   // HIGH: int64, CK3 date when hook ends
    constexpr size_t BACKING_SECRET_REF = 0x20;   // HIGH: uint32, ref to backing secret
}

// =============================================================================
// SHookType — static hook type definition
// =============================================================================
namespace hook_type {
    constexpr size_t IS_STRONG          = 0x95;   // HIGH: bool, strong vs weak hook
    constexpr size_t IS_PERPETUAL       = 0x96;   // HIGH: bool, perpetual (never expires)
}

// =============================================================================
// Relation Container (sorted array at core_data+0x20, Round 10-3)
// Binary search by target character ID. Entry stride = 0x10.
// =============================================================================
namespace relation_container {
    constexpr size_t ARRAY_PTR          = 0x20;   // HIGH: ptr to sorted relation entry array (from core_data)
    constexpr size_t CAPACITY           = 0x28;   // HIGH: int32, array capacity
    constexpr size_t COUNT              = 0x2C;   // HIGH: int32, number of relations
    constexpr size_t ENTRY_STRIDE       = 0x10;   // HIGH: bytes per entry
    // Entry layout:
    //   +0x00: uint32 key (other character ID, for binary search)
    //   +0x04: uint32 (unknown/padding)
    //   +0x08: ptr    CDiplomaticRelation* (0x1F0 bytes)
    constexpr size_t ENTRY_KEY          = 0x00;   // HIGH: uint32, other char ID
    constexpr size_t ENTRY_PTR          = 0x08;   // HIGH: ptr, CDiplomaticRelation*
}

// =============================================================================
// CTruceContainer (DEPRECATED aliases -- prefer diplomatic_relation namespace)
// These are a subset of CDiplomaticRelation, kept for backward compatibility.
// =============================================================================
namespace truce_container {
    constexpr size_t CHAR_ID_1          = 0x08;   // HIGH: = diplomatic_relation::CHAR_ID_LOW
    constexpr size_t CHAR_ID_2          = 0x0C;   // HIGH: = diplomatic_relation::CHAR_ID_HIGH
    constexpr size_t SLOT1_EXPIRY       = 0x28;   // HIGH: int32, CK3 date
    constexpr size_t SLOT1_REASON_STR   = 0x30;   // HIGH: std::string (0x20 bytes)
    constexpr size_t SLOT1_REASON_ENUM  = 0x50;   // HIGH: int32, reason enum (0-3)
    constexpr size_t SLOT2_EXPIRY       = 0x58;   // HIGH: int32, CK3 date
    constexpr size_t SLOT2_REASON_STR   = 0x60;   // HIGH: std::string (0x20 bytes)
    constexpr size_t SLOT2_REASON_ENUM  = 0x80;   // HIGH: int32, reason enum (0-3)
    constexpr size_t SLOT_SIZE          = 0x30;   // HIGH: bytes per slot
}

// =============================================================================
// CClaim Entry Offsets (within claim PDX array at core_data + 0x1E8)
// HIGH confidence -- R25 decompilation of FUN_06784ac0 and FUN_05c3a810
// =============================================================================
namespace claim_entry {
    constexpr size_t ENTRY_SIZE         = 0x10;   // HIGH: 16 bytes per claim
    constexpr size_t VTABLE             = 0x00;   // HIGH: 8-byte CClaim::vftable pointer
    constexpr size_t TITLE_ID           = 0x08;   // HIGH: uint32, title ID
    constexpr size_t PRESSED            = 0x0C;   // HIGH: byte, 1=pressed/strong claim
    constexpr size_t IMPLICIT           = 0x0D;   // HIGH: byte, 1=inherited from ancestor
    // DEPRECATED: old name assumed +0x00 was title_ref, but it's actually the vtable
    constexpr size_t TITLE_REF          = 0x00;   // DEPRECATED: this is the vtable, not a title ref
}

// =============================================================================
// CTraitDatabase Offsets (from *(TRAIT_DB))
// HIGH confidence -- decompilation-verified
// =============================================================================
namespace trait_db {
    constexpr size_t ARRAY_PTR          = 0x88;   // HIGH: ptr to CTrait*[] indexed by trait_id
    constexpr size_t ARRAY_COUNT        = 0x94;   // HIGH: int32, max valid trait_id + 1
    constexpr size_t HASH_TABLE         = 0x1B0;  // HIGH: name->CTrait* hash map (0x18-byte entries)
    constexpr size_t HASH_MASK          = 0x1BC;  // HIGH: uint32, AND mask for bucket index
    constexpr size_t LOCK_OBJECT        = 0xED0;  // HIGH: mutex/rwlock
    constexpr size_t LOCK_FLAG          = 0xF10;  // HIGH: byte, 0=needs locking
    // Lookup: *(*(trait_db + ARRAY_PTR) + trait_id * 8) -> CTrait*
    // Bounds: trait_id < *(int*)(trait_db + ARRAY_COUNT)
}

// =============================================================================
// CTrait Struct Offsets (from resolved CTrait*)
// HIGH confidence -- decompilation of FUN_065f2960 (deserializer), Round 9-4 full analysis
// =============================================================================
namespace trait_entry {
    // Identity
    constexpr size_t TRAIT_ID           = 0x08;   // HIGH: uint32, numeric trait index
    constexpr size_t NAME_DESCRIPTOR    = 0x10;   // HIGH: CJominiScriptDbObject desc ptr (name at desc+0x10)
    constexpr size_t MIN_VALUE          = 0x68;   // HIGH: int16, education trait level min
    constexpr size_t MAX_VALUE          = 0x6C;   // HIGH: int16, education trait level max

    // Modifier collection (sub-object at +0x90, stores all stat bonuses)
    // Access: value_array = *(trait + 0xA8), values are 8-byte CFixedPoint, indexed by modifier slot
    // Slot obtained via FUN_066e6ac0(trait+0x90, modifier_def_index)
    constexpr size_t MODIFIER_COLLECTION = 0x90;  // HIGH: CModifierCollection sub-object start
    constexpr size_t MOD_VALUE_ARRAY    = 0xA8;   // HIGH: ptr to CFixedPoint[]; = *(trait+0x90+0x18)
    constexpr size_t MOD_TYPE_MASK      = 0xE8;   // HIGH: uint32, bitmask of modifier categories

    // Triggered modifiers
    constexpr size_t CULTURE_MOD_TRIG   = 0xF0;   // HIGH: culture_modifier trigger sub-object
    constexpr size_t FAITH_MOD_TRIG     = 0x108;  // HIGH: faith_modifier trigger sub-object
    constexpr size_t TRIG_OPINION_ARRAY = 0x120;  // HIGH: ptr to triggered opinion entries (stride ~0x230)
    constexpr size_t TRIG_OPINION_COUNT = 0x12C;  // HIGH: int32, triggered opinion count

    // Compatibility map
    constexpr size_t COMPATIBILITY_MAP  = 0x194;  // HIGH: compatibility = { trait = value ... }

    // Group
    constexpr size_t GROUP_NAME         = 0x260;  // HIGH: std::string, trait group name
    constexpr size_t GROUP_PTR          = 0x280;  // HIGH: ptr to resolved CTraitGroup

    // Level tracks (from ParseField 0x357c + core dump validation)
    constexpr size_t LEVEL_TRACK_ARRAY  = 0x320;  // HIGH: ptr to SLevelTrackEntry[] (16-byte stride)
    constexpr size_t LEVEL_TRACK_PACKED = 0x328;  // HIGH: packed: low32=count, high32=capacity
    // Entry layout: [+0] ptr to related db object, [+8] uint32 value, [+0xC] uint32 type
    constexpr size_t LEVEL_TRACK_ENTRY_STRIDE = 16;

    constexpr size_t GROUP_PTR2         = 0x338;  // HIGH: ptr, resolved trait group (from +0x260 lookup)

    // Flags and category
    constexpr size_t FLAGS              = 0x344;  // HIGH: uint32 bitfield (fully decoded, see trait_flags namespace)
    constexpr size_t CATEGORY           = 0x348;  // HIGH: byte (0=personality,1=edu,2=congenital,3-4=cmdr,5=lifestyle,6=fame,7=court,8=legend,9=genetic)
    constexpr size_t SEX_RELATED        = 0x349;  // HIGH: byte (0=default,1=male_only,2=female_only) — ParseField 0x2a58
    constexpr size_t PARENT_INHERIT_SEX = 0x34A;  // HIGH: byte (0=male,1=female,2=all)
    constexpr size_t CHILD_INHERIT_SEX  = 0x34B;  // HIGH: byte (0=male,1=female,2=all)

    // Genetics / inheritance
    constexpr size_t INHERIT_CHANCE     = 0x34C;  // HIGH: int32, 0-100
    constexpr size_t BOTH_PARENT_CHANCE = 0x350;  // HIGH: int32, 0-100
    constexpr size_t BASTARD_TYPE       = 0x354;  // HIGH: byte (0=none,1=illegit,2=legit) — ParseField 0x3086
    constexpr size_t BASTARD_TYPE_ALT   = 0x355;  // HIGH: byte — ParseField 0x3087

    // Opinion modifiers (CORRECTED from Round 9-4)
    constexpr size_t SAME_OPINION       = 0x388;  // HIGH: int32, same-trait opinion bonus
    constexpr size_t SAME_FAITH_OPINION = 0x38C;  // HIGH: int32, same-trait same-faith opinion
    constexpr size_t OPPOSITE_OPINION   = 0x390;  // HIGH: int32, opposite-trait opinion malus
    constexpr size_t RULER_DESIGNER_COST = 0x394; // HIGH: int32
    constexpr size_t MINIMUM_AGE        = 0x39C;  // HIGH: int32
    constexpr size_t MAXIMUM_AGE        = 0x3A0;  // HIGH: int32
    constexpr size_t VALID_SEX          = 0x3A4;  // HIGH: byte (0=all,1=male,2=female)

    // Name hash and AI personality
    constexpr size_t NAME_HASH          = 0x3AC;  // HIGH: uint32, string hash
    constexpr size_t AI_PERS_HASH_0     = 0x3B0;  // HIGH: uint32, ai_honor attribute key hash — ParseField 0x3123
    constexpr size_t AI_PERS_HASH_1     = 0x3B4;  // HIGH: uint32, ai_greed attribute key hash — ParseField 0x3124
    constexpr size_t AI_PERS_HASH_2     = 0x3B8;  // HIGH: uint32, ai_? attribute key hash — ParseField 0x3125
}

// CTrait Flags Bitfield — fully decoded from ParseField decompilation
namespace trait_flags {
    constexpr uint32_t PHYSICAL               = 0x001;  // ParseField 0x358c
    constexpr uint32_t GENETIC                = 0x002;  // ParseField 0x3088
    constexpr uint32_t INHERIT_REAL_FATHER     = 0x004;  // ParseField 0x3089
    constexpr uint32_t SPECIAL_LEGEND          = 0x008;  // ParseField 0x2852
    constexpr uint32_t DISABLE_COMBAT_LEADER   = 0x010;  // ParseField 0x2d7f
    constexpr uint32_t NO_CHILDREN_INV         = 0x020;  // ParseField 0x2de3
    constexpr uint32_t EDUCATION_TYPE          = 0x040;  // ParseField 0x34ff
    constexpr uint32_t IMMORTAL                = 0x080;  // ParseField 0x29ab
    constexpr uint32_t INCAPACITATING          = 0x100;  // ParseField 0x308a
    constexpr uint32_t GOOD_GENETIC            = 0x200;  // ParseField 0xcd0
    constexpr uint32_t ENCYCLOPEDIA_INV        = 0x400;  // ParseField 0x34b3
    constexpr uint32_t RULER_DESIGNER_INV      = 0x800;  // ParseField 0x31a4
    constexpr uint32_t ENABLES_INBRED          = 0x1000; // ParseField 0x3943
}

// =============================================================================
// CFaction Struct Offsets (MEDIUM-HIGH confidence)
// =============================================================================
namespace faction {
    constexpr size_t ID                 = 0x10;   // MEDIUM: int32, faction ID
    constexpr size_t DATA_PTR           = 0x18;   // MEDIUM: ptr, faction data block
    constexpr size_t TYPE_TARGET        = 0x38;   // MEDIUM: int32, faction type/target ID
    constexpr size_t CB_STRING          = 0xFC0;  // MEDIUM: ptr, casus belli string
    constexpr size_t CB_RESOLVED        = 0xFC8;  // MEDIUM: ptr, resolved CB (null triggers lookup)
    constexpr size_t ELIGIBLE           = 0xFF0;  // MEDIUM: byte, join eligibility flag
    constexpr size_t COUNTY_EXCL_FLAG   = 0xFF1;  // MEDIUM: byte, county exclusion flag
}

// =============================================================================
// Province Key Offsets (MEDIUM confidence, from building/county analysis)
// =============================================================================
namespace province {
    constexpr size_t TEMPLATE_PTR       = 0x008;  // HIGH: CJominiProvinceTemplate* (runtime-verified)
    constexpr size_t BUILDING_SLOT_DATA = 0x1F8;  // MEDIUM: building slot data
    constexpr size_t HOLDING_TYPE_PTR   = 0x200;  // MEDIUM: ptr, holding type
    constexpr size_t HAS_HOLDER_FLAG    = 0x208;  // MEDIUM: byte
    constexpr size_t PARENT_BARONY_ID   = 0x250;  // MEDIUM: int32
    constexpr size_t HOLDER_CHAR_ID     = 0x270;  // MEDIUM: uint32
    constexpr size_t BARONY_TITLE_ID    = 0x318;  // HIGH: uint32, barony title ID (runtime-verified)
    constexpr size_t COUNTY_DATA_PTR    = 0x428;  // HIGH: CCounty* (R19 confirmed via FUN_06312be0 setter)
}

// =============================================================================
// CJominiProvinceTemplate — Static province data (accessed via province::TEMPLATE_PTR)
// Geographic adjacency data is here. Computed from map image at startup.
// =============================================================================
namespace province_template {
    constexpr size_t CENTER_X            = 0x008;  // HIGH: int32, map pixel x coordinate
    constexpr size_t CENTER_Y            = 0x00c;  // HIGH: int32, map pixel y coordinate
    constexpr size_t PROVINCE_ID         = 0x024;  // HIGH: int32, province ID (matches definition.csv)
    constexpr size_t ADJACENCY_ARRAY_PTR = 0x050;  // HIGH: ptr to SAdjacency[] (CPdxArray data_ptr)
    constexpr size_t ADJACENCY_CAPACITY  = 0x058;  // HIGH: int32, CPdxArray capacity
    constexpr size_t ADJACENCY_COUNT     = 0x05c;  // HIGH: int32, number of adjacent provinces
    // Terrain type: CTerrainType* (key std::string at +0x10 → "plains","hills","mountains",
    // "drylands","forest","desert","taiga","jungle","steppe","farmlands","wetlands",
    // "floodplains","desert_mountains","oasis","terraced_hills"). HIGH — core dump
    // ck3_20260421_195552: 300+ provinces clustered into exactly 15 distinct ptrs, all
    // sharing vtable 0x...ce5800 and decoding to valid CK3 terrain keys. (ck-readlane)
    constexpr size_t TERRAIN_TYPE_PTR    = 0x0b8;  // HIGH: ptr, CTerrainType* (key at +0x10, db_object::NAME_KEY)
}

// SAdjacency struct (stride 0x30 = 48 bytes)
// Element of province_template::ADJACENCY_ARRAY_PTR
namespace adjacency {
    constexpr size_t TYPE_FLAGS          = 0x00;   // MEDIUM: int32, 0=land, 2=? (sea/river?)
    constexpr size_t NEIGHBOR_PROV_ID    = 0x04;   // HIGH: int32, neighbor province ID
    constexpr size_t DISTANCE            = 0x10;   // MEDIUM: int32, distance (fixed-point)
    constexpr size_t CROSSING_X          = 0x18;   // MEDIUM: int32, crossing point x pixel
    constexpr size_t CROSSING_Y          = 0x1c;   // MEDIUM: int32, crossing point y pixel
    constexpr size_t STRIDE              = 0x30;   // 48 bytes per adjacency entry
}

// =============================================================================
// Diplomatic Range Constants (from 00_defines.txt, HIGH confidence)
// Used for capital-to-capital Euclidean distance filtering.
// =============================================================================
namespace diplomatic_range {
    constexpr int MAXIMUM_RANGE            = 1000;    // HIGH: map pixels (from 00_defines.txt)
    constexpr int MAXIMUM_RANGE_RESTRICTED = 750;     // HIGH: restricted game rule variant
    constexpr int MAXIMUM_RANGE_SQUARED    = 1000000; // derived: 1000² for squared distance comparison
}

// =============================================================================
// Personality Modifier Offsets (within AI eval context, stride 0x108)
// =============================================================================
namespace personality_mods {
    constexpr size_t HONOR_MODS         = 0x1e0;
    constexpr size_t GREED_MODS         = 0x2e8;
    constexpr size_t RATIONALITY_MODS   = 0x3f0;
    constexpr size_t ENERGY_MODS        = 0x4f8;
    constexpr size_t BOLDNESS_MODS      = 0x600;
    constexpr size_t ZEAL_MODS          = 0x708;
    constexpr size_t VENGEFULNESS_MODS  = 0x810;
    constexpr size_t COMPASSION_MODS    = 0x918;
    constexpr size_t SOCIABILITY_MODS   = 0xa20;
    constexpr size_t PERSONALITY_FLAG   = 0xee9;
}

// =============================================================================
// AI Controller Struct Offsets (from *(core_data + AI_CONTROLLER))
// =============================================================================
namespace ai_ctrl {
    // CAIStrategy struct layout. PROVENANCE (ck-2u2a, re-derived 2026-09-24;
    // the previous citation named 'PerTickSetup @ 0x04e22cb0', which is not a
    // function name in the project and not where the writes are):
    //   FUN_04e22cb0  entry 0x4e22cb0, size 16 — a FORWARDER, reached per tick
    //     from CAIManager_MainTick @ 0x4deb8aa (UNCONDITIONAL_CALL). Body:
    //       FUN_04e52690(*(undefined8 *)(param_1 + 0x18), param_1 + 8);
    //     so param_1 IS the ai_ctrl: +0x18 is CHARACTER_PTR, +8 MILITARY_POWER.
    //   FUN_04e52690  entry 0x4e52690, size 1153 — THE WRITER. param_2 =
    //     ai_ctrl+8, so param_2+N lands at ai_ctrl+8+N.
    // FOUR fields are set every tick, not three: +0x08, +0x10, +0x14 (+0x15 high
    // byte) and +0x16. They are NOT persistent. The two comments in this header
    // used to disagree — one listed 0x10/0x14/0x16, the other STATE/FLAGS/
    // MILITARY_POWER — and neither listed all four.
    // Detail, including the FLAGS bit sources and the open question about the two
    // vtable slots among the 6 xrefs: re/findings/re_ai_ctrl_per_tick_setup.md
    constexpr size_t MILITARY_POWER     = 0x08;   // int64: computed military power (Boldness-adjusted)
    constexpr size_t STATE              = 0x10;   // int32: government type from landed data. Must be > 1 for eligibility
    constexpr size_t RANK_DATA          = 0x14;   // uint16: character rank/tier data
    constexpr size_t FLAGS              = 0x16;   // byte: bit0=is_player, bit1=is_ai_active, bit2=mil_threshold, bit3=gov_flag_0x400
    constexpr size_t CHARACTER_PTR      = 0x18;   // ptr: CCharacter* being controlled
    constexpr size_t EXT_STATE_PTR      = 0x20;   // ptr: extended state (war eval, tick counters, etc.)
    // AI_ENABLED's provenance is INDEPENDENT of the block citation above, and is
    // a dedicated engine setter whose entire body is the write — HIGH:
    //   AI_ToggleCharacterAI  entry 0x4e1f190, size 5
    //     *(undefined1 *)(param_1 + 0x28) = param_2;
    //   0 = AI disabled, nonzero = enabled.
    // That function's Ghidra plate comment types param_1 as CCharacter*. NOT
    // REPEATED HERE ON PURPOSE: the bridge reaches this byte as ai_ctrl+0x28 via
    // core_data + AI_CONTROLLER (0x278), and this struct already holds a separate
    // CCharacter* at +0x18. The receiver TYPE is disputed and is ck-8o2x's
    // subject; the entry, the size and the store are true either way.
    constexpr size_t AI_ENABLED         = 0x28;   // byte: AI enabled for this character (checked in MainTick)
    constexpr size_t DAILY_FLAG         = 0x29;   // byte: if 0, forces daily category bucket
    constexpr size_t IS_SUSPENDED       = 0x2A;   // byte: if 1, IsEligible returns false
    // Extended state offsets (relative to *(ai_ctrl + EXT_STATE_PTR))
    constexpr size_t EXT_WAR_TARGET_LIST = 0x98;  // CPdxArray: war targets. +0x00=data_ptr, +0x0C=count
    constexpr size_t EXT_WAR_EVAL       = 0xB8;   // war evaluation context struct
    constexpr size_t EXT_WAR_LIST       = 0x138;  // CPdxArray: current wars for character
    // Tick counters (decremented per evaluation, 8 categories) — in extended state
    constexpr size_t TICK_COUNTER_0     = 0x268;
    constexpr size_t TICK_COUNTER_1     = 0x26c;
    constexpr size_t TICK_COUNTER_2     = 0x270;
    constexpr size_t TICK_COUNTER_3     = 0x274;
    constexpr size_t TICK_COUNTER_4     = 0x278;
    constexpr size_t TICK_COUNTER_5     = 0x27c;
    constexpr size_t TICK_COUNTER_6     = 0x280;
    constexpr size_t TICK_COUNTER_7     = 0x284;
    constexpr size_t TICK_PHASE_DATA    = 0x288;  // int64: zeroed each tick

    // Eligibility check (IsEligible @ 0x04e19160):
    //   return (STATE > 1) && (FLAGS & 0x02) && !IS_SUSPENDED;
    // The per-tick setup (forwarder FUN_04e22cb0 @ 0x4e22cb0 -> writer
    // FUN_04e52690 @ 0x4e52690) sets MILITARY_POWER, STATE, RANK_DATA and FLAGS
    // per tick — FOUR fields; this comment used to name three and omit RANK_DATA.
    // After set_character_ai(true), must wait 1 tick for it to run. (ck-2u2a)

    // NOTE: ai_ctrl IS the CAIStrategy object (RTTI-confirmed: "11CAIStrategy")
    // EXT_STATE_PTR (+0x20) points to ai_ctrl+0x40 (interior sub-object).
    // Tick counters and other ext_state offsets are relative to *(ai_ctrl + EXT_STATE_PTR).
    // TICK_PHASE_DATA (+0x288) is 8 bytes: one flag per category, set when counter expires.
}

// =============================================================================
// CAIGoal Class Hierarchy — RTTI-confirmed goal objects
// Vtable addresses and type IDs from core dump + Ghidra decompilation
// =============================================================================
namespace ai_goal {
    // CAIGoal vtable (base class, 15 virtual functions)
    constexpr uintptr_t VTABLE_BASE          = 0xb8846c0;  // HIGH: CAIGoal vtable
    constexpr uintptr_t TYPEINFO_BASE        = 0xb884738;  // HIGH: CAIGoal typeinfo

    // Subclass vtables
    constexpr uintptr_t VTABLE_CREATE_TITLE  = 0xb885510;  // HIGH: CAIGoalCreateTitle (type=0)
    constexpr uintptr_t VTABLE_CONVERT_HOLD  = 0xb885598;  // HIGH: CAIGoalConvertHolding (type=dynamic)
    constexpr uintptr_t VTABLE_CONSTRUCT     = 0xb885620;  // HIGH: CAIGoalConstructHolding (type=3)
    constexpr uintptr_t VTABLE_DECISION      = 0xb8856a8;  // HIGH: CAIGoalDecision (type=4)

    // CAIGoal instance layout
    constexpr size_t IDENTIFIER              = 0x08;  // HIGH: int64, serialized goal ID
    constexpr size_t SUB_ID                  = 0x0c;  // HIGH: int32, title ID or building type
    constexpr size_t REFERENCE_PTR           = 0x10;  // HIGH: ptr, decision type / title obj
    constexpr size_t SECONDARY_REF           = 0x18;  // MEDIUM: int32, secondary reference
    constexpr size_t POTENTIAL_TRIGGER        = 0x50;  // MEDIUM: trigger script block ("potential")
    constexpr size_t POTENTIAL_FLAG           = 0xac;  // MEDIUM: int32, 0 = always eligible
    constexpr size_t WEIGHT_SCRIPT           = 0x130; // MEDIUM: script block ("ai_will_do" weight)
    constexpr size_t WEIGHT_FLAG             = 0x1f0; // MEDIUM: int32, 0 = score 0

    // Vtable slot indices (offset = slot * 8)
    constexpr int VSLOT_DESTRUCTOR           = 0;   // no-op in base
    constexpr int VSLOT_DELETE               = 1;   // destructor + free
    constexpr int VSLOT_SERIALIZE            = 3;   // write goal data
    constexpr int VSLOT_DESERIALIZE          = 5;   // read goal data
    constexpr int VSLOT_COMPARE              = 7;   // compare two goals by type
    constexpr int VSLOT_CAN_EXECUTE          = 8;   // bool: check goal conditions
    constexpr int VSLOT_EVALUATE             = 9;   // int: compute priority score
    constexpr int VSLOT_EXECUTE              = 10;  // perform the goal action
    constexpr int VSLOT_GET_TYPE             = 11;  // int: return type identifier
}

// =============================================================================
// AI Goal Selection Functions (base-relative, need ASLR adjustment)
// =============================================================================
namespace ai_goal_funcs {
    constexpr uintptr_t SELECT_GOALS         = 0x04d66550;  // HIGH: goal pool selection (financial state) (ck-e58l.20)
    constexpr uintptr_t SELECT_BEST_GOAL     = 0x04d84740;  // HIGH: core priority scoring loop (ck-e58l.20)
    constexpr uintptr_t CHECK_ELIGIBILITY    = 0x04d91b50;  // HIGH: evaluate "potential" trigger (ck-e58l.20)
    constexpr uintptr_t SCORE_PRIORITY       = 0x04d91cd0;  // HIGH: evaluate "weight" script (ck-e58l.20)
    constexpr uintptr_t GET_CANDIDATE_LIST   = 0x04d92aa0;  // HIGH: look up goal list by mode+tier (ck-e58l.20)
    constexpr uintptr_t STRATEGY_EXECUTE     = 0x04d66220;  // HIGH: timer mgmt + goal dispatch (683B)
    constexpr uintptr_t CREATE_NEW_GOALS     = 0x04d669e0;  // MEDIUM: goal creation (timer B expiry) (ck-e58l.20)
    constexpr uintptr_t EVAL_WEIGHT_SCRIPT   = 0x04d92e80;  // MEDIUM: script engine for weight eval (ck-e58l.20)
    // Minimum score threshold global
    constexpr uintptr_t MIN_SCORE_THRESHOLD  = 0x0c2aae80;  // MEDIUM: int32, goals must score >= this
}

// =============================================================================
// AI Evaluation Context Offsets (struct passed to dispatch hooks)
// =============================================================================
namespace ai_eval_ctx {
    constexpr size_t CHARACTER_PTR      = 0x18;   // HIGH: ptr to CCharacter being evaluated
}

// =============================================================================
// CAIManager Struct Offsets
// =============================================================================
namespace ai_mgr {
    constexpr size_t VTABLE             = 0x00;   // ptr: vtable (14 virtual functions)
    constexpr size_t INTERNAL_VTABLE    = 0x08;   // ptr: internal vtable
    constexpr size_t CHAR_DB_REF        = 0x20;   // ptr: character database reference
    constexpr size_t RNG_SEED           = 0x28;   // uint32: RNG counter
    constexpr size_t RNG_SECONDARY      = 0x2c;   // uint32: RNG secondary
    constexpr size_t STRATEGY_ARRAY     = 0x48;   // ptr: CAIStrategy* array
    constexpr size_t STRATEGY_COUNT     = 0x54;   // int: strategy count
    // Category queues (8 total, 0x18 bytes each)
    constexpr size_t QUEUE_1_PTR        = 0x78;   // flagged characters
    constexpr size_t QUEUE_2_PTR        = 0x90;   // all AI characters (always populated)
    constexpr size_t QUEUE_3_PTR        = 0xa8;
    constexpr size_t QUEUE_4_PTR        = 0xc0;   // diplomatic
    constexpr size_t QUEUE_5_PTR        = 0xd8;
    // Priority queues (8 total, from 0xf0)
    constexpr size_t PRIORITY_QUEUES    = 0xf0;
    constexpr size_t JOB_QUEUE          = 0x1b0;  // ptr: job queue for parallel dispatch
    constexpr size_t JOB_COUNT          = 0x1bc;  // int
    constexpr size_t EXTRA_STATE        = 0x1c8;  // processed in post-execute
    constexpr size_t CHAR_PROC_ARRAY    = 0x1e0;  // ptr: character processing array
    constexpr size_t CHAR_PROC_COUNT    = 0x1ec;  // int
    constexpr size_t CHAR_PROC_SENTINEL = 0x1f0;  // byte: sentinel/load factor
}

// =============================================================================
// Character Iterator (for enumerating all characters)
// =============================================================================
namespace char_iter {
    // Start: *(game_state + 0xa0) + 0x29c40
    constexpr size_t GAME_STATE_TO_CHARS = 0xa0;
    constexpr size_t CHARS_BASE_OFFSET   = 0x29c40;
    constexpr size_t ENTRY_STRIDE        = 0x20;  // 32 bytes per character entry
}

// =============================================================================
// Interaction DB Internal Layout
// =============================================================================
namespace interaction_db_internal {
    // GetByIndex: *(DB + 0x1020 + index*8). declare_war = index 0xD (13).
    constexpr size_t INDEX_ARRAY_BASE    = 0x1020;  // CONFIRMED: base of interaction ptr array
    constexpr size_t INDEX_ARRAY_STRIDE  = 8;       // 8 bytes per entry (pointer)

    // ck-8cns: REFUTED — "current interaction" / "Gate 2 stamp" hypothesis was wrong.
    // (*g_InteractionDB) + 0x1000 = slot 14 of the 16-slot pinned-interaction array
    // (offsets 0xF20..0x1018, stride 0x10). It is bound at DB load to
    // influence_candidacy_interaction's CCharacterInteractionType*. There are NO
    // runtime writers — the slot is read-only after init.
    //
    // The post-execute compare in CSendCharacterInteractionCmd::Execute (0x0619b800)
    // runs AFTER SendCharacterInteraction has already dispatched. It only gates a UI
    // pause-notification (code 0x3e4a) that fires when the dispatched interaction is
    // influence_candidacy_interaction AND sender is the current player. It does NOT
    // gate dispatch.
    //
    // Bridge code MUST NOT write to this slot — doing so corrupts the static binding
    // (see ck-ukox8 for cleanup of bridge/src/ai_eval_hook.cpp:1853 stamp).
    //
    // See re/findings/re_gate2_stamp_verification.md for full verification.
    constexpr size_t SLOT14_INFLUENCE_CANDIDACY_DEF = 0x1000;  // HIGH: slot 14 ptr@+0
    // Deprecated alias — do NOT use. Kept temporarily to avoid breaking
    // bridge/src/ai_eval_hook.cpp during the ck-ukox8 stamp-removal transition.
    constexpr size_t CURRENT_INTERACTION = 0x1000;  // DEPRECATED: see SLOT14_INFLUENCE_CANDIDACY_DEF

    // Array metadata
    constexpr size_t ARRAY_PTR           = 0x88;    // ptr to array of interaction definition ptrs
    constexpr size_t ARRAY_COUNT         = 0x94;    // int32: count of interactions in array
}

// =============================================================================
// Interaction Hash Table (at *(INTERACTION_DB + 0x1b0))
// =============================================================================
namespace interaction_ht {
    constexpr size_t TABLE_OFFSET       = 0x1b0;  // offset from interaction DB singleton
    constexpr size_t CAPACITY_OFFSET    = 0x1bc;  // uint32: hash table capacity (mask)
    constexpr size_t LOCK_FLAG_OFFSET   = 0xf10;  // byte: lock flag (needs locking if 0)
    constexpr size_t LOCK_OFFSET        = 0xed0;  // lock object for thread safety
    constexpr size_t SENTINEL_OFFSET    = 0x1c0;  // byte: sentinel chain length
    constexpr size_t ENTRY_SIZE         = 0x18;   // 24 bytes per entry
    // Entry layout (confirmed from CMD_AI_TRY_INTERACTION decompilation):
    // +0x00: (padding)
    // +0x04: byte valid (non-zero = occupied, 0 = empty)
    // +0x08: uint32 hash
    // +0x10: ptr data (interaction object pointer)
    // +0x1c: byte chain_length (max probe distance)
    // Probing: index = capacity_mask & hash, then linear probe
}

// =============================================================================
// WAR_DB (TPdxRefDatabase<CWar,8>) layout — ck-s0qu cross-dump validated.
// Singleton at globals::WAR_DB. Used by WAR_DB_ALLOCATE / lookup paths.
// =============================================================================
namespace war_db {
    // Database header layout (validated 867 + 1097 dumps).
    constexpr size_t TABLE_PTR        = 0x20;  // HIGH: ptr to slot table base
    constexpr size_t CAPACITY         = 0x28;  // HIGH: u32, table capacity (also at +0x2C dup)
    constexpr size_t MAX_ID           = 0x38;  // HIGH: u32, last assigned slot index (monotonic)
    constexpr size_t ACTIVE_COUNT     = 0x3C;  // HIGH: u32, active war count (incremented on alloc, decremented on free)
    constexpr size_t TOTAL_SLOTS      = 0x40;  // HIGH: u32, total slots ever allocated (>= active count)
    constexpr size_t FREE_SENTINEL    = 0x44;  // HIGH: u32, free-list head sentinel (= 0x00FFFFFF)

    // Slot layout (16 bytes per slot).
    constexpr size_t SLOT_STRIDE      = 16;
    constexpr size_t SLOT_META_LO     = 0x00;  // u32: SLOT_OCCUPIED if occupied, else free-list next_idx
    constexpr size_t SLOT_META_HI     = 0x04;  // u32: SLOT_OCCUPIED if occupied, else free-list prev_idx (0x00FFFFFF=head)
    constexpr size_t SLOT_WAR_PTR     = 0x08;  // ptr: CWar* (NULL when slot is free)
    constexpr uint32_t SLOT_OCCUPIED  = 0x00FFFFFF;  // sentinel pattern in BOTH meta words for occupied slots

    // war_id encoding (ck-s0qu: validated across 41 wars in 2 dumps).
    constexpr uint32_t WAR_ID_GEN_SHIFT = 24;
    constexpr uint32_t WAR_ID_GEN_MASK  = 0xFF000000;  // (gen << 24) — incremented on slot reuse
    constexpr uint32_t WAR_ID_IDX_MASK  = 0x00FFFFFF;  // slot table index
    constexpr uint32_t WAR_ID_INVALID   = 0xFFFFFFFF;  // CWar::IsValid sentinel (= NullWar.war_id)
}

// =============================================================================
// CWar Object Offsets (0x300 bytes total, Round 5c-d + R8V4 + ck-0q79 core dump enumeration)
// Confirmed via WAR_INIT FUN_066d10a0 + WAR_PARTICIPANT_SETUP FUN_066be2d0
// R8V4: primary atk/def refs, participant counts, battle side flag, war access chain
// ck-0q79: full struct validated against 11 wars in core dump, allocation stride = 0x300
// ck-s0qu: cross-validated against 87-war 1097 dump; CCasusBelli vtable corrected.
// =============================================================================
namespace war_obj {
    constexpr size_t VTABLE             = 0x00;   // HIGH: ptr, CWar vtable
    constexpr size_t WAR_ID             = 0x08;   // HIGH: uint32, war ID (init 0xFFFFFFFF)
    constexpr size_t INTERNAL_VTABLE    = 0x10;   // HIGH: ptr, CWar internal vtable
    constexpr size_t DB_REF             = 0x18;   // HIGH: uint32, TPdxRef game object ref (assigned by FUN_063d3580 via game state DB)

    // Attacker CWarSide (0x30 bytes starting at +0x20)
    constexpr size_t ATK_SIDE_VTABLE    = 0x20;   // HIGH: CWarSide::vftable
    constexpr size_t ATK_PARTICIPANT    = 0x28;   // HIGH: CPdxArray of CWarParticipant* (ptr stride=8, count at +0x0C). Dereference each entry!
    constexpr size_t ATK_PART_CAPACITY  = 0x30;   // HIGH: uint32, CPdxArray capacity for attacker participants
    constexpr size_t ATK_PART_COUNT     = 0x34;   // HIGH: int32, attacker participant count (R8V4: *(side+0x14))
    constexpr size_t ATK_PART_ALLOCATOR = 0x38;   // HIGH: ptr, CPdxArray allocator (constant per-game)
    constexpr size_t ATK_SIDE_FLAGS     = 0x48;   // HIGH: byte, attacker side flags

    // Defender CWarSide (0x30 bytes starting at +0x50)
    constexpr size_t DEF_SIDE_VTABLE    = 0x50;   // HIGH: CWarSide::vftable
    constexpr size_t DEF_PARTICIPANT    = 0x58;   // HIGH: CPdxArray of CWarParticipant* (ptr stride=8, count at +0x0C). Dereference each entry!
    constexpr size_t DEF_PART_CAPACITY  = 0x60;   // HIGH: uint32, CPdxArray capacity for defender participants
    constexpr size_t DEF_PART_COUNT     = 0x64;   // HIGH: int32, defender participant count (R8V4: *(side+0x14))
    constexpr size_t DEF_PART_ALLOCATOR = 0x68;   // HIGH: ptr, CPdxArray allocator (constant per-game)
    constexpr size_t DEF_SIDE_FLAGS     = 0x78;   // HIGH: byte, defender side flags

    // Dates/flags
    // ck-sd4d: the two halves of this qword are now named. LOW dword is the raw
    // game clock (same units as game_state::GAME_DATE); HIGH dword is the same
    // instant PRE-DECODED as (year << 16) | ((month-1) << 8) | (day-1), with
    // 0xFFFFFFFF as the unset sentinel WAR_CONSTRUCTOR writes. Validated on all
    // 87 wars of ck3_20260421_195552.core: decoding the low dword through the
    // engine's real calendar (ck3::gamedate, ck-05mg) reproduces the high dword
    // exactly, 87/87 — which is also a fourth independent confirmation that the
    // calendar has no leap years and real month lengths.
    constexpr size_t START_DATE_PACKED  = 0x80;   // HIGH: uint64 = raw clock (lo dword) + packed Y/M/D (hi dword)
    constexpr size_t START_DATE_RAW     = 0x80;   // HIGH: uint32, raw clock ticks at declaration (ck-sd4d)
    constexpr size_t START_DATE_YMD     = 0x84;   // HIGH: uint32, (year<<16)|((month-1)<<8)|(day-1); 0xFFFFFFFF = unset (ck-sd4d)
    // ck-sd4d CORRECTION: this is NOT an end date. WAR_CONSTRUCTOR never writes
    // it, WAR_END_CORE never writes it, and it reads 0 in all 87 wars of
    // ck3_20260421_195552.core. The war is memset to zero the instant it ends,
    // so no end date could survive here anyway. Kept (and still emitted, raw,
    // as `end_date_field_raw`) so the disproof is visible rather than silently
    // re-guessed. The observable end date comes from the game clock sampled
    // inside the WAR_END_CORE hook. Was labelled HIGH on no stated evidence.
    constexpr size_t END_DATE_PACKED    = 0x88;   // REFUTED (ck-sd4d): always 0; NOT an end date
    constexpr size_t ACTIVE_FLAG        = 0x90;   // DEPRECATED: always reads inactive. Use IS_ENDED (0x2F8) instead (R8V4 confirmed)

    // CCasusBelli sub-object (0x1A0 bytes at +0x98, constructor FUN_05b5a820)
    constexpr size_t CB_SUBOBJ          = 0x98;   // HIGH: ptr, CCasusBelli::vftable (cb+0x00)
    constexpr size_t CB_TYPE_REF        = 0xA0;   // HIGH: uint64, CB type reference (cb+0x08)
    constexpr size_t CB_TARGET          = 0xA8;   // HIGH: type tag (cb+0x10), always 0x12 across 11 wars in ck-s0qu probe — NOT a title/char ref. Use CB_TITLE_ARRAY (0x210) or PRIMARY_DEF_REF (0x22C) for actual target.
    constexpr size_t CB_WAR_ID          = 0xB0;   // HIGH: uint32, CB back-reference to owning war ID (cb+0x18)
    constexpr size_t CB_HASH            = 0xB8;   // LOW: uint32, unique per-war hash (purpose unclear, not CB-type-dependent) (cb+0x20)
    constexpr size_t CB_TITLE_ARRAY     = 0x210;  // HIGH: uint64, title ID array ptr (cb+0x178)
    constexpr size_t CB_TITLE_COUNT     = 0x21C;  // HIGH: int32, title count (cb+0x184)
    // R8V4 REINTERPRETATION: FUN_066b45d0 reads war+0x228 as primary defender char ref,
    // FUN_066b92e0 reads war+0x22C as primary attacker char ref. These overlap with
    // the tail of the CB sub-object (cb internal 0x190/0x194). The old "claim type" and
    // "war goal" labels for CB_PARAM_1/CB_PARAM_2 were likely wrong — these are character refs.
    // ck-qeod CORRECTION: R8V4 labels are INVERTED from war-side semantics.
    // Verified across all 11 wars in core dump: war+0x228 = ATK side, war+0x22C = DEF side.
    // ck-yifp: THESE ARE NOT FIXED AT DECLARATION. The engine reassigns the
    // primary DEFENDER to the new holder when the contested title changes
    // hands, and the bridge's own WAR_END_CORE ledger has recorded it:
    //     war 0 ended defender_victory 1070.05.04 (norwegian_invasion_cb 29767->31808)
    // for a war DECLARED 29767 -> 30853 (smoke episode 1, node, 2026-09-12).
    // Confirmed a second way in smoke episode 3's final save, whose LIVE
    // norman_conquest_cb record reads 31808 -> 29767 after Harald Hardrada took
    // k_england. So any client that IDENTIFIES a war by the belligerent pair it
    // was declared with will lose that war mid-run: key on WAR_ID.
    // Confidence HIGH for the behaviour (two independent engine-sourced
    // records); the write SITE is NOT localised — a .text byte scan for
    // `mov [reg+0x22c]` and `mov [reg+0x194]` found no war-code candidate, so
    // the store is reached through another base or width. Recorded as a gap
    // rather than guessed at. See re/findings/re_war_end_outcome.md sec.10.
    constexpr size_t PRIMARY_ATK_REF    = 0x228;  // HIGH: uint32, attacker char ref, MUTABLE (= cb+0x190, verified ck-qeod: matches war side 1 participant)
    constexpr size_t PRIMARY_DEF_REF    = 0x22C;  // HIGH: uint32, defender char ref, MUTABLE — follows the contested title (ck-yifp)
    constexpr size_t CB_PARAM_1         = 0x228;  // DEPRECATED: alias, actually PRIMARY_ATK_REF
    constexpr size_t CB_PARAM_2         = 0x22C;  // DEPRECATED: alias, actually PRIMARY_DEF_REF
    constexpr size_t CLAIMANT_REF       = 0x230;  // HIGH: uint32, claimant char ref for claim_cb (= cb+0x198); 0xFFFFFFFF for non-claim CBs (ck-qeod)

    // Ticking war score (R24: raw memory, no function calls needed)
    constexpr size_t ATK_TICKING_SCORE   = 0x40;   // HIGH: int64, fixed-point / 100k, attacker ticking score accumulator
    constexpr size_t ATK_TICKING_ENABLED = 0x48;   // HIGH: byte, attacker ticking enabled flag
    constexpr size_t DEF_TICKING_SCORE   = 0x70;   // HIGH: int64, fixed-point / 100k, defender ticking score accumulator
    constexpr size_t DEF_TICKING_ENABLED = 0x78;   // HIGH: byte, defender ticking enabled flag

    // Score enforcement (R25: WAR_SCORE_UPDATE writes when |score| >= 100)
    constexpr size_t SCORE_ENFORCE_DATE = 0x254;  // HIGH: uint64, date when ±100 enforcement triggers
    constexpr size_t SCORE_ENFORCE_FLAG = 0x25c;  // HIGH: byte, whether score enforcement is pending

    // Battle data
    constexpr size_t BATTLE_LIST_PTR    = 0x238;  // HIGH: ptr, battle object array
    constexpr size_t BATTLE_COUNT       = 0x244;  // HIGH: int32, battle count
    constexpr size_t BATTLE_ALLOCATOR   = 0x248;  // HIGH: ptr, allocator for battle array (ck-0q79)

    // War flags (ck-0q79: varies per war, most wars have byte1=0x01)
    constexpr size_t WAR_FLAGS          = 0x250;  // MEDIUM: uint16, war state flags (byte 0 + byte 1)

    // Participant tracking containers (R25: NOT numeric scores — these are double-buffered
    // participant tracking arrays. WAR_SCORE_UPDATE rebuilds "current", WAR_COPY_SCORES
    // swaps current→cached. Each container: ptr(+0), capacity(+8), count(+C), allocator(+10))
    constexpr size_t CURRENT_TRACK_DATA = 0x260;  // HIGH: current working participant container
    constexpr size_t CURRENT_DEDUP_DATA = 0x278;  // HIGH: current working dedup container
    constexpr size_t CACHED_TRACK_DATA  = 0x290;  // HIGH: snapshot of CURRENT_TRACK_DATA
    constexpr size_t CACHED_DEDUP_DATA  = 0x2A8;  // HIGH: snapshot of CURRENT_DEDUP_DATA

    // Backwards-compat aliases for old names
    constexpr size_t ATK_SCORE_DATA     = 0x260;  // DEPRECATED: renamed to CURRENT_TRACK_DATA (not attacker-specific)
    constexpr size_t DEF_SCORE_DATA     = 0x290;  // DEPRECATED: renamed to CACHED_TRACK_DATA (not defender-specific)

    // War name (std::string SSO, preceded by allocator at +0x2D0)
    constexpr size_t NAME_ALLOCATOR     = 0x2D0;  // HIGH: ptr, string allocator for war name (ck-0q79)
    constexpr size_t NAME_DATA_PTR      = 0x2D8;  // HIGH: SSO string data ptr
    constexpr size_t NAME_LENGTH        = 0x2E0;  // HIGH: size_t, name length
    constexpr size_t NAME_INLINE_BUF    = 0x2E8;  // HIGH: SSO inline buffer (16 bytes)

    // Active/ended flag (R7-4 confirmed: 0=active, non-zero=ended)
    constexpr size_t IS_ENDED           = 0x2F8;  // HIGH: byte, 0=active, non-0=ended. Set by WAR_END_CORE, which then wipes the object — see funcs::WAR_END_CORE.
    constexpr size_t END_SUPPRESS_MSG   = 0x2F9;  // MEDIUM: byte, read by WAR_END_CORE to suppress the result message (ck-sd4d)
    constexpr size_t END_BLOCKED        = 0x2FA;  // MEDIUM: byte, war exempt from the invalidation sweep (ck-sd4d, FUN_066c7f70)
    constexpr size_t END_IN_PROGRESS    = 0x2FB;  // MEDIUM: byte, re-entrancy guard set around the invalidation check (ck-sd4d)

    // Engine outcome enum — the 3rd argument to funcs::WAR_END_CORE.
    // Cross-validated four ways (ck-sd4d, re/findings/re_war_end_outcome.md):
    //   (1) the invalidation sweep FUN_066c7f70 passes 3;
    //   (2) the engine's own trigger documents the enum as
    //       "victory/defeat/white_peace/invalidated" (has_war_result_message_with_outcome);
    //   (3) WAR_END_CORE's switch indexes CCasusBelliType fields in STRUCT order
    //       0x960 / 0x9c0 / 0xa20 / 0xa80 for cases 0 / 2 / 1 / 3, matching the
    //       binary's field-name order attacker_victory, white_peace, defender_victory, invalidated;
    //   (4) the attacker side's "won the war goal" flag is `outcome == 0`.
    enum : int {
        OUTCOME_ATTACKER_VICTORY = 0,
        OUTCOME_DEFENDER_VICTORY = 1,
        OUTCOME_WHITE_PEACE      = 2,
        OUTCOME_INVALIDATED      = 3,
    };

    // R20/R24: These offsets are on a UI data model struct, NOT on CWar itself.
    // R24 confirmed: war score is computed on-the-fly by FUN_066ba0d0(war_ptr, tooltip_ctx).
    // These cached values at 0x11c8+ are unreliable when read directly from CWar memory.
    // DEPRECATED: Use ck3::funcs::WAR_COMPUTE_SCORE instead.
    constexpr size_t SCORE_TOTAL        = 0x11c8; // DEPRECATED: not on CWar, use WAR_COMPUTE_SCORE
    constexpr size_t SCORE_IMPRISONMENT = 0x11d0; // DEPRECATED: UI cache only
    constexpr size_t SCORE_BATTLES      = 0x11d8; // DEPRECATED: UI cache only
    constexpr size_t SCORE_OCCUPATION   = 0x11e0; // DEPRECATED: UI cache only
    constexpr size_t SCORE_TICKING      = 0x11e8; // DEPRECATED: UI cache only

    // Backwards-compat aliases (deprecated names)
    constexpr size_t RULES_PTR          = 0xA0;   // DEPRECATED: use CB_TYPE_REF (same offset; war_rules offsets are relative to *(war+0xA0))
    constexpr size_t TOTAL_SIZE         = 0x300;  // HIGH: confirmed via allocation stride across 11 consecutive CWar objects in core dump (ck-0q79). Constructor inits to 0x2FC. UI model struct (separate object) extends to 0x1c40+.
    // NOTE: ATK_PARTICIPANT/DEF_PARTICIPANT containers store POINTERS to CWarParticipant (8-byte stride),
    // NOT inline structs. To read participant[i]: dereference *(ptr*)(data_ptr + i*8), then read fields.

    // CWar vtable-data addresses (ck-e58l.11, file-rel; Ghidra = +0x00100000).
    // All 11 wars in ck-0q79 core dump share CWAR_VTABLE. typeinfo "4CWar" at 0x0bc50e10.
    constexpr uintptr_t CWAR_VTABLE               = 0x0bc50d68;  // HIGH: CWar primary vtable (first vfunc; 6 declared slots; offset_to_top=0)
    constexpr uintptr_t CWAR_VTABLE_ISVALID       = 0x065c09e0;  // HIGH: CWar::IsValid = slot[2], 7B: `return war->war_id != 0xFFFFFFFF` — SF3 predicate (ck-e58l.11)
    constexpr uintptr_t NULL_WAR_VTABLE           = 0x0bc86cd0;  // HIGH: _g_NullWar_sentinel vtable (subclass override). Sentinel object at *(WAR_DB_SENTINEL).
    constexpr uintptr_t NULL_WAR_VTABLE_ISVALID   = 0x066320c0;  // HIGH: NullWar::IsValid = slot[2], 2B: unconditional return 0. Drives SF3 on SF2 cascade (ck-e58l.11).
}

// =============================================================================
// CWarParticipant Offsets (0x20 bytes, Round 5d CONFIRMED)
// =============================================================================
namespace war_participant {
    constexpr size_t VTABLE             = 0x00;   // HIGH: ptr, CWarParticipant::vftable
    constexpr size_t CHAR_ID            = 0x08;   // HIGH: uint32, participant character ID
    constexpr size_t JOIN_DATA          = 0x0C;   // HIGH: uint64, join date or CB ref
    constexpr size_t CONTRIBUTION       = 0x14;   // HIGH: uint64, contribution (init 0)
    constexpr size_t FLAGS              = 0x1C;   // HIGH: uint32, flags/status (init 0)
    constexpr size_t ENTRY_SIZE         = 0x20;   // total size
}

// =============================================================================
// War Rules Object Offsets (from *(war + RULES_PTR))
// HIGH confidence — Round 5 decompilation
// =============================================================================
namespace war_rules {
    constexpr size_t MAX_SCORE          = 0x1738; // HIGH: int32, max war score
    constexpr size_t MIN_SCORE          = 0x173C; // HIGH: int32, min war score
}

// =============================================================================
// Battle Entry Offsets (from battle list in war)
// HIGH confidence — Round 5 decompilation
// =============================================================================
namespace battle {
    constexpr size_t SCORE              = 0x40;   // HIGH: int64, battle score
    constexpr size_t SIDE_FLAG          = 0x48;   // HIGH: byte, 0=attacker, non-zero=defender (R8V4 decompilation confirmed)
}

// =============================================================================
// Council Position Offsets (Round 5 CONFIRMED from decompilation)
// Accessed via globals::COUNCIL_POSITION_DB
// =============================================================================
namespace council_position {
    constexpr size_t POSITION_TYPE      = 0x10;   // HIGH: int32, council position type
    constexpr size_t HOLDER_CHAR_ID     = 0x38;   // HIGH: uint32, holder character ID
}

// =============================================================================
// CActiveScheme Struct Offsets (Round 10-8 CORRECTED from CreateActiveScheme +
// InitializeScheme decompilation — source: scheme_manager.cpp line 311)
// 0x1F0 bytes total, accessed via CSchemeManager
// RefDB validates at +0x10 (not +0x18 like CCharacter — two vtables before ID)
// Validity check: *(scheme+0x20) != 1 (status != INVALIDATED)
// =============================================================================
namespace active_scheme {
    constexpr size_t VTABLE             = 0x00;   // HIGH: ptr, vtable (CPersistent)
    constexpr size_t VTABLE_2           = 0x08;   // HIGH: ptr, vtable (TPdxReferenceObject)
    constexpr size_t SCHEME_ID          = 0x10;   // HIGH: uint32, packed ref ID (gen|idx) (R10-8: validated at +0x10)
    constexpr size_t REFDB_INTERNAL     = 0x14;   // HIGH: int32, refdb internal field
    constexpr size_t SCHEME_TYPE_PTR    = 0x18;   // HIGH: ptr, CSchemeType* (R10-8: pointer to type object, NOT owner char ptr)
    constexpr size_t STATUS             = 0x20;   // HIGH: int32, 0=active, 1=invalidated (R10-8)
    constexpr size_t OWNER_ID           = 0x24;   // HIGH: uint32, owner char packed ref
    constexpr size_t TARGET_TYPE        = 0x28;   // HIGH: int32, target type enum: 0=char, 1=title, 2=culture, 3=faith, 4=none (R10-8)
    constexpr size_t TARGET_REF         = 0x2C;   // HIGH: uint32, target packed ref, resolve via DB based on TARGET_TYPE (R10-8)
    constexpr size_t CONTRACT_REF       = 0x34;   // HIGH: int32, contract ref / agent slot ID
    constexpr size_t SCHEME_SUBTYPE_ID  = 0x38;   // HIGH: uint32, scheme subtype (secondary qualifier, NOT main type) (R10-8)
    // --- Agent slots (CPdxArray at +0x40) ---
    constexpr size_t AGENT_SLOTS_PTR    = 0x40;   // HIGH: ptr, agent slot array data (R10-8)
    constexpr size_t AGENT_SLOTS_CAPACITY = 0x48; // HIGH: int32, agent slots capacity (R10-8)
    constexpr size_t AGENT_SLOTS_COUNT  = 0x4C;   // HIGH: int32, number of agent slots (R10-8)

    // --- Name string (std::string at +0x50) ---
    constexpr size_t NAME_STR_DATA      = 0x50;   // HIGH: ptr, scheme name string data (R10-8)
    constexpr size_t NAME_STR_LENGTH    = 0x58;   // HIGH: size_t, scheme name string length (R10-8)
    constexpr size_t NAME_STR_CAPACITY  = 0x60;   // HIGH: size_t, scheme name string capacity (R10-8)

    // --- Progress ---
    constexpr size_t PROGRESS           = 0x70;   // HIGH: int32, success progress value (R10-8 CORRECTED: was 0x120)
    constexpr size_t PROGRESS_UI       = 0x120;  // HIGH: int64, UI-displayed scheme progress (R8V-5 verified, token 0x3a31)
    // --- Timed events ---
    constexpr size_t TIMED_EVENTS_PTR   = 0xD8;   // HIGH: ptr, timed events array data (R10-8)
    constexpr size_t TIMED_EVENTS_COUNT = 0xE4;   // HIGH: int32, timed events count (R10-8)

    // --- Timing/dates ---
    constexpr size_t START_DATE         = 0xF0;   // HIGH: int64, game date when scheme started (R10-8)
    constexpr size_t LAST_UPDATE_DATE   = 0xF8;   // HIGH: int64, last daily update date (R10-8)
    constexpr size_t EXPOSURE_DATE      = 0x100;  // HIGH: int64, scheme exposure date (R10-8)
    constexpr size_t IS_FROZEN          = 0x108;  // HIGH: byte, scheme frozen flag (R10-8)
    constexpr size_t DISCOVERY_TIMER    = 0x10C;  // HIGH: int32, discovery countdown (R10-8)
    constexpr size_t EXPOSURE_TIMER     = 0x110;  // HIGH: int32, exposure countdown (R10-8)
    constexpr size_t NEEDS_PROCESSING   = 0x114;  // LOW: byte, temporary processing flag — NOT validity (R10-8)
    constexpr size_t SUCCESS_CHANCE     = 0x118;  // HIGH: int32, computed success chance (R10-8)
    constexpr size_t AGENT_SLOT_COUNTER = 0x128;  // HIGH: int16, agent slot counter (R10-8)

    // --- Freeze data (SFreezeData sub-struct at +0x138) ---
    constexpr size_t FREEZE_VTABLE      = 0x138;  // HIGH: ptr, CActiveScheme::SFreezeData vtable (R10-8)
    constexpr size_t FREEZE_ACTIVE      = 0x140;  // HIGH: byte, freeze active flag (R10-8)
    constexpr size_t FREEZE_STR         = 0x148;  // HIGH: std::string (24 bytes), freeze reason (R10-8)
    constexpr size_t FREEZE_COUNTDOWN   = 0x168;  // HIGH: int32, freeze countdown (R10-8)
    constexpr size_t FREEZE_FLAG2       = 0x16C;  // HIGH: byte, secondary freeze flag (R10-8)

    // --- Success modifiers ---
    constexpr size_t SUCCESS_BASE       = 0x1D0;  // HIGH: int64, base success value (R10-8)
    constexpr size_t SUCCESS_GROWTH     = 0x1D8;  // HIGH: int64, success growth rate (R10-8)
    constexpr size_t SUCCESS_MAX        = 0x1E0;  // HIGH: int64, success maximum (R10-8)
    constexpr size_t MAX_PROGRESS       = 0x1E8;  // HIGH: int32, max progress value (R10-8 CORRECTED: was SPEED)

    constexpr size_t TOTAL_SIZE         = 0x1F0;  // HIGH: confirmed from memset in RemoveActiveScheme (R10-8)
}

// =============================================================================
// CSchemeAgentSlot Struct Offsets (R8V, from scheme agent slot decompilation)
// Agent slots are stored in CPdxArray at active_scheme + AGENT_SLOTS_PTR
// =============================================================================
namespace scheme_agent_slot {
    constexpr size_t SLOT_ID         = 0x08;  // int32, init -1
    constexpr size_t AGENT_CHAR_REF  = 0x10;  // int64, agent character reference
    constexpr size_t SLOT_INDEX      = 0x18;  // int16, init 0xFFFF
    constexpr size_t SCHEME_ID       = 0x1C;  // int32, init -1
    constexpr size_t DEFAULT_VALUE   = 0x28;  // from global
    constexpr size_t IS_EXPOSED      = 0x30;  // byte, exposed flag
    constexpr size_t STRIDE          = 0x38;  // total slot size
}

// =============================================================================
// CSchemeManager Struct Offsets (Round 10-8 CORRECTED from decompilation)
// =============================================================================
namespace scheme_manager {
    constexpr size_t SCHEME_DB_REF      = 0x20;   // HIGH: ptr, scheme database reference (R10-8)
    constexpr size_t ALL_SCHEMES_PTR    = 0x28;   // HIGH: ptr, all scheme objects (R10-8)
    constexpr size_t ALL_SCHEMES_CAPACITY = 0x30; // HIGH: int32, capacity (R10-8)
    constexpr size_t ALL_SCHEMES_COUNT  = 0x34;   // HIGH: int32, active scheme count (R10-8)
    constexpr size_t DAILY_BUCKETS      = 0x40;   // HIGH: stride 0x18, index = scheme_id % 30 (R10-8)
    constexpr size_t YEARLY_BUCKETS     = 0x310;  // HIGH: stride 0x18, index = scheme_id % 365 (R10-8)

    // Invalidated schemes array (R10-8)
    constexpr size_t INVALIDATED_PTR    = 0x2548; // HIGH: ptr, invalidated_schemes.data_ptr (R10-8)
    constexpr size_t INVALIDATED_COUNT  = 0x2554; // HIGH: int32, invalidated_schemes.count (R10-8)

    // Scheme processing filters (R10-8: these are int32 char IDs, not byte flags)
    constexpr size_t KNOW_SCHEMES_CHAR  = 0x2578; // HIGH: int32, know_schemes target char ID (-1=none) (R10-8 CORRECTED: was labeled byte flag)
    constexpr size_t END_SCHEMES_CHAR   = 0x257C; // HIGH: int32, end_schemes target char ID (-1=none) (R10-8 CORRECTED: was labeled byte flag)
    constexpr size_t OWNER_FILTER_CHAR  = 0x2580; // HIGH: int32, char_owner_filter_id (-1=none) (R10-8)

    // DEPRECATED aliases
    constexpr size_t PENDING_CAPACITY   = 0x2550; // DEPRECATED: not actually capacity, use INVALIDATED_PTR/COUNT
    constexpr size_t PENDING_COUNT      = INVALIDATED_COUNT; // DEPRECATED: renamed
    constexpr size_t KNOW_SCHEMES_FLAG  = KNOW_SCHEMES_CHAR; // DEPRECATED: is int32 char ID, not byte flag
    constexpr size_t END_SCHEMES_FLAG   = END_SCHEMES_CHAR;  // DEPRECATED: is int32 char ID, not byte flag
}

// =============================================================================
// CSchemeType Struct Offsets (ck-1nab: from ComputeSuccessTriple/Chance decompilation)
// Schema type definition — read-only game data, accessed via active_scheme+0x18 ptr
// =============================================================================
namespace scheme_type {
    constexpr size_t BASE_VALUE_BLOCK     = 0x440;  // HIGH: ModifierDef, base success value evaluation block
    constexpr size_t CHANCE_BLOCK         = 0x860;  // HIGH: ModifierDef, success chance evaluation block
    constexpr size_t BASE_GROWTH          = 0x988;  // HIGH: int64, base growth multiplier (fixed-point × 100000)
    constexpr size_t GROWTH_SKILL         = 0x990;  // HIGH: int32, skill type for growth calculation
    constexpr size_t BASE_MAX_VALUE       = 0x99C;  // HIGH: int32, base max value (pre-multiplied)
    constexpr size_t MAX_CHANCE_CAP       = 0x9A0;  // HIGH: int32, max success chance cap (percentage, typically 95)
    constexpr size_t MIN_CHANCE_FLOOR     = 0x9A8;  // HIGH: int32, min success chance floor (percentage)
    constexpr size_t BASE_MAX_PROGRESS    = 0x9AC;  // HIGH: int32, base max progress (days)
    constexpr size_t CAP_TYPE             = 0xB40;  // HIGH: int32, cap table index (0-3), indexes global cap table
    constexpr size_t PASSIVE_FLAG         = 0xB46;  // HIGH: byte, passive/guaranteed-floor flag
}

// =============================================================================
// CArmy Object Offsets (Round 6b CONFIRMED from disband handler decompilation)
// Lookup: same pattern as char DB — *(*(db+0x20) + 8 + (id & 0xffffff) * 0x10)
// =============================================================================
namespace army_obj {
    constexpr size_t SUB_VTABLE         = 0x08;   // HIGH: ptr, TPdxReferenceObject<CArmy> vtable
    constexpr size_t ID                 = 0x10;   // HIGH: uint32, army ID (full 32-bit, 24-bit for DB index)

    // Regiment membership — the army's mobilized sub-units (CArmyRegiment, NOT CRegiment).
    // HIGH (ck-warstrength): CoreReader ck3_20260421_195552 across 407 live armies +
    //   CArmyRegiment serializer FUN_05bf75f0/FUN_05bf70c0 decompile.
    // REG_VEC is a vector of CArmyRegiment ref-IDs (uint32, type-prefix byte varies); each
    // resolves via globals::ARMY_REGIMENT_DB (NOT REGIMENT_DB — different class/DB).
    // REG_COUNT is the slot/capacity count and OVER-counts: trailing IDs are 0 and live entries
    // may be fewer. Iterate [0..REG_COUNT), skip id==0, verify *(ent+0x10)==id after DB lookup.
    constexpr size_t REG_VEC            = 0x18;   // HIGH: uint32*, CArmyRegiment ref-ID array
    constexpr size_t REG_COUNT          = 0x20;   // HIGH: int32, slot count (capacity; skip 0-IDs)

    // Location & movement (Round 11: CoreReader probe of 53 armies, cross-validated via title DB)
    // LOCATION_BARONY (0xE8) — REFUTED 2026-09-13 (ck-meu5), was "HIGH: uint32,
    // current location barony title ID (-> LANDED_TITLE_DB)". It is NOT a title id:
    // in william_1066_baseline.core, 5 of 32 live armies hold the 0xFFFFFFFF
    // sentinel and the other 27 hold values in 27035..58867, while the landed-title
    // DB has 18168 id-verified titles with MAX VALID ID 18167 — so ALL 27 are out
    // of range and 0 of 27 resolve. Kept under a neutral name because the field IS
    // populated and stable and is worth identifying; do not read it as a location.
    // The only resolvable CProvince* on CArmy is DESTINATION_PROV (0x1A0) below,
    // and a destination is not a location (null-object when idle: 17/32).
    // IDENTIFIED 2026-09-17 (ck-nuet, ck-760i seat): it is a CHARACTER ID, and
    // the army's COMMANDER. Counted against william_1066_baseline.core, 32 live
    // armies: 27 populated, 5 hold 0xFFFFFFFF, and 27/27 of the populated ones
    // resolve through CHARACTER_DB and read their OWN id back at
    // character::ID (0x18 — NOT the 0x10 that dynasty_obj/title_obj/army_obj
    // use; reading 0x10 gives 0/27 and is how the first pass missed it).
    // It is the owner in 16/27 and a DIFFERENT character in 11/27 (army 22:
    // owner Harold 30853, commander Gyrth 32642 — his brother), and the five
    // sentinels are exactly William's five armies, unled at the bookmark.
    // MEDIUM and not HIGH: two structural sources plus a historical
    // cross-check, NOT decompile-verified. CAssignCommander /
    // CDetachCommanderCommand / NAIWar::HandleAssignCommanders are in
    // re/rtti/rtti_vtables.csv and showing one of them WRITE this byte is what
    // would make it HIGH. Full measurement: re/findings/re_army_commander_and_coords.md
    // AND IT IS NOT A LOCATION — a commander is a character; ck-nuet is still open.
    constexpr size_t COMMANDER_CHAR_ID  = 0xE8;   // MEDIUM: uint32, commanding character (→ CHARACTER_DB); 0xFFFFFFFF = none
    constexpr size_t OWNER_LINK_ID      = 0xEC;   // HIGH: uint32, owner-link ID (→ OWNER_LINK_DB lookup)
    constexpr size_t COMBAT_REF_ID      = 0xF0;   // MEDIUM: uint32, combat-related DB ID (24-bit masked, from InCombat decompile)
    constexpr size_t MOVEMENT_SPEED     = 0xF8;   // MEDIUM: int32, movement-related value (0 when stationary, fixed-point?)
    constexpr size_t MOVEMENT_PROGRESS  = 0x148;  // MEDIUM: int32, movement/arrival progress (~10M typical, fixed-point?)
    // ck-1f85 RECONCILED 2026-09-17 (ck-760i seat), NOT overturned. ck-1f85
    // refuted this annotation against 170 LIVE armies at 1069 (zero holding
    // 0x10000). In william_1066_baseline.core it is CORRECT and correlates
    // cleanly: 12 armies hold 0x10000 and ALL 12 have PATH_LENGTH 0, no
    // destination and a NULL MAP_ENTITY_PTR; 20 hold 0xFFFFFFFF and 17 of those
    // are moving. SAME BuildID, SAME offsets, DIFFERENT WORLD STATE (a freshly
    // loaded bookmark vs a world three game-years in), so BOTH measurements are
    // true and the field's CONTENT IS STATE-DEPENDENT.
    // THE OPERATIVE HALF OF ck-1f85 IS UNCHANGED: do not rely on the sentinels.
    // The name is left alone because ck-1f85 owns it and is still open.
    constexpr size_t MOVEMENT_FLAG      = 0x154;  // DISPUTED: sentinels hold at a bookmark, NOT in a running world (ck-1f85)
    // REFUTED 2026-09-17 (ck-nuet, ck-760i seat) as per-army COORDINATES. Across
    // the same 32 live armies — scattered from England to Anatolia — these span
    // 648 and 253 units out of ~53.1M and ~69.9M respectively (15 distinct
    // values each). Thirty-two armies cannot share an X to within 648 parts in
    // 53 million. The map's own province centres
    // (province_template::CENTER_X/Y, HIGH) span x 110..8675, y 84..4544.
    // The fixed-point reading was TESTED at scales 1/1000/65536/100000 against
    // nearest province centre: the 65536 row reports a mean distance of 4.6
    // map-pixels, which LOOKS like a hit until you notice mean == worst to one
    // decimal — every army resolving to ONE point. Magnitude (~5.3e7) is the
    // order of this dump family's game-date raw; identity NOT claimed.
    // 0x150 beside them is IDENTICAL in all 32 armies (43,808,760, spread 0).
    constexpr size_t UNKNOWN_158        = 0x158;  // REFUTED as a map X coord (ck-nuet) — effectively constant across armies
    constexpr size_t UNKNOWN_15C        = 0x15C;  // REFUTED as a map Y coord (ck-nuet) — effectively constant across armies
    constexpr size_t MAP_ENTITY_PTR     = 0x160;  // MEDIUM: ptr, map visual/entity object (NULL when stationary)
    constexpr size_t PATH_LENGTH        = 0x168;  // HIGH: int32, movement path step count (0=stationary, >0=moving)
    constexpr size_t MOVEMENT_STATE     = 0x16C;  // HIGH: int32, movement state enum (0=idle, 1=march, 2+= other states)
    constexpr size_t DESTINATION_PROV   = 0x1A0;  // HIGH: CProvince*, destination province (TPdxNullObject<CProvince> when idle)
                                                   //       Read destination barony: *(dest_prov + 0x318) → barony title ID
    constexpr size_t SPECIAL_STATUS     = 0x1CC;  // HIGH: byte, special status flag
    constexpr size_t TRANSPORT_STATE    = 0x1D8;  // HIGH: int32, transport/movement state (-1=none)
    constexpr size_t OBJECTIVE_REF_C0   = 0xC0;   // LOW (re_army_control.md §6): u32 objective/siege-target ref?
                                                   //   (-1=none when idle; flips when active). Confirm on a mid-siege army.
    // CArmy struct total size: 0x200 bytes (confirmed by adjacent object spacing)
    // NOTE: per-army STANCE does NOT exist. Offensive/defensive is per-WAR on the
    // CAIWarCoordinator record (war_stance::REC_STANCE_PTR=0x70). See re_army_control.md.
}

// REMOVED: move_army mechanism deleted per design (agent does not drive army micro).
// The CMoveUnitCommand (move_unit_cmd::) namespace block was removed; no code
// references it. (CSiege/province_siege_link offsets below remain — read-side only.)

// =============================================================================
// CSiege Object Offsets (ck-mol-6uaq: RTTI, vtable decompilation, coredump)
// Lookup: same DB pattern — *(*(SIEGE_DB+0x20) + 8 + (id & 0xffffff) * 0x10)
// DB capacity at SIEGE_DB+0x2C. Entry stride 0x10 (16 bytes).
// =============================================================================
namespace siege_obj {
    constexpr size_t VTABLE              = 0x00;   // HIGH: CSiege vtable (rel 0xbc86e00)
    constexpr size_t ID                  = 0x08;   // HIGH: uint32, siege ID (24-bit for DB index, verified in lookup)
    constexpr size_t SUB_VTABLE          = 0x10;   // HIGH: ptr, TPdxReferenceObject<CSiege> vtable
    constexpr size_t SAVE_TAG_27F7       = 0x14;   // HIGH: int32, serialized field (save tag 0x27F7)
    constexpr size_t SAVE_TAG_2DEF       = 0x18;   // HIGH: int32, serialized field (save tag 0x2DEF, -1=empty)
    constexpr size_t SAVE_TAG_31E3       = 0x1C;   // HIGH: reference, serialized field (save tag 0x31E3)
    constexpr size_t ATTACKER_PTR        = 0xA0;   // HIGH: ptr, attacker data (non-null = siege has attacker)
    constexpr size_t ATTACKER_ARMY_ID    = 0xA8;   // HIGH: uint32, attacker army ID (→ ArmyDB lookup, -1=invalid)
    constexpr size_t DATA_PTR_D8         = 0xD8;   // MEDIUM: ptr, iterator/sub-object (deser loop)
    constexpr size_t TAG_TYPE_E0         = 0xE0;   // MEDIUM: int32, tag/type for deserialization dispatch
    constexpr size_t PROGRESS            = 0x1b0;  // HIGH: int32 fixed-point (/100000), 0..~20 siege progress/morale; runtime-derived (not serialized). Validated two dumps. re_siege_progress_struct.md
    constexpr size_t STATE               = 0x1b8;  // HIGH: int32 enum; 5=actively besieging (progress accrues), 0=idle/not-started. Gates PROGRESS. re_siege_progress_struct.md
    constexpr size_t PROGRESS_MAX_OBS    = 20;     // MEDIUM: observed max of PROGRESS/100000 across 64 sieges (denominator for a 0..100% ratio; may be fort-level-scaled — confirm live). re_siege_progress_struct.md
}

// =============================================================================
// Province/Barony→Siege Link Offsets (ck-mol-6uaq)
// These offsets are on province/barony objects, NOT on CSiege.
// =============================================================================
namespace province_siege_link {
    constexpr size_t SIEGE_REF_0x114     = 0x114;  // HIGH: uint32, siege ref ID (24-bit, looked up in SIEGE_DB)
    constexpr size_t TITLE_REF           = 0x318;  // MEDIUM: uint32, title/barony ID (→ LandedTitleDB)
    constexpr size_t SIEGE_COUNT         = 0x344;  // MEDIUM: int32, siege-related count/phase
    constexpr size_t SIEGE_REF_0x368     = 0x368;  // HIGH: uint32, siege ref ID (alternative path, FUN_06312c00)
    constexpr size_t SIEGE_ACTION        = 0x434;  // MEDIUM: uint32, siege action/state flag
}

// =============================================================================
// CArmyOwnerLink Offsets (Round 6b CONFIRMED from disband/raise decompilation)
// Links army to its owner character. Looked up via OWNER_LINK_DB.
// =============================================================================
namespace army_owner_link {
    constexpr size_t ID                 = 0x10;   // HIGH: uint32, owner-link ID (for validation)
    constexpr size_t TROOP_COUNT        = 0x168;  // MISNOMER — NOT a soldier count. int32 "not-busy/no-pending-order" field, ~0 for a normal RAISED army. The raised-army gate FUN_066846a0 uses it as a `< 1` (idle) PRECONDITION, never as troop count. Real men = regiment-sum on CArmy (REG_VEC+0x18 → ARMY_REGIMENT_DB[rid]+0x38). Do NOT gate "has troops" on this. — re_move_army_raised.md
    constexpr size_t OWNER_CHAR_ID      = 0x16C;  // HIGH: int32, owner character ID
    constexpr size_t BACK_REF_ARMY_ID   = 0x170;  // HIGH: uint32, back-reference army ID
}

// =============================================================================
// Army Enumeration Sub-Object (from FUN_06061870(GAME_STATE_ROOT))
// =============================================================================
namespace army_enum {
    constexpr size_t ID_ARRAY_PTR       = 0x50;   // HIGH: ptr to array of 4-byte army IDs
    constexpr size_t ID_ARRAY_COUNT     = 0x5C;   // HIGH: int32, count of army IDs
}

// =============================================================================
// Council Data Offsets (Round 6b from CSetCouncilTaskEffect::Execute)
// Accessed via FUN_0677bdf0(char) → council member struct
// =============================================================================
namespace council_data {
    constexpr size_t TASK_ARRAY         = 0x398;  // HIGH: council task array
    constexpr size_t TASK_LIST          = 0xF10;  // HIGH: task list ptr
    constexpr size_t TASK_COUNT         = 0xF1C;  // HIGH: int32, task count
}

// =============================================================================
// Council Member Object Offsets (Round 7 from R7 decompilation)
// Accessed via core_data + 0x4E8 (nullable ptr) or core_data + 0x4F0 (inline fallback)
// =============================================================================
namespace council_member {
    // Inline council config offsets (relative to alive_data + 0x4F0)
    // CONFIRMED via Ghidra: GetCouncilConfig (0x0677baf0) returns *(char+0x1A8)+0x4F0.
    // IsCouncillor (0x05bb5450) uses config+0x40 (sorted array) and config+0x4C (count).
    // This path works for ALL rulers (player AND AI). The g_CouncilDB may not contain
    // entries for the player character.
    // NOTE: alive_data is at char+0x1A8 (REALM_DATA), NOT char+0x1B8 (TITLE_REALM_DATA).
    constexpr size_t ID                 = 0x08;   // HIGH: uint32, council member ID
    constexpr size_t COUNCILLOR_ARRAY_LEGACY = 0x40;  // DEPRECATED: empty at runtime for player chars
    constexpr size_t COUNCILLOR_COUNT_LEGACY = 0x4C;  // DEPRECATED: 0 at runtime for player chars
    // RUNTIME-VALIDATED (2026-04-02 wide probe): actual councillor data at these offsets
    constexpr size_t COUNCILLOR_ARRAY   = 0xe8;   // HIGH: ptr to uint32[] councillor char IDs (runtime-confirmed)
    constexpr size_t COUNCILLOR_COUNT   = 0xf4;   // HIGH: int32, number of councillors (runtime-confirmed)
    constexpr size_t TASK_ARRAY         = 0x398;  // HIGH: ptr to task entry array (stride 0x48)
    constexpr size_t POSITION_HOLDER    = 0x3A0;  // HIGH: int32, position holder char ID
    constexpr size_t TASK_COUNT         = 0x3A4;  // HIGH: int32, number of task entries
    constexpr size_t TASK_STRIDE        = 0x48;   // bytes per task entry
}

// =============================================================================
// Held Title Refs Container (on SLandedCharacterData at *(char + 0x1B8))
// Returned by CCharacter::GetHeldTitleRefs (0x0674f840 - 0x100000)
// Container at landed_data + 0x1E0: {uint32* arr, int32 cap, int32 count}
// Elements are TITLE IDs (NOT council position IDs!), looked up in LANDED_TITLE_DB (0x0bfd26c0).
// Previously MISLABELED as council_position_refs. Renamed to held_title_refs.
// 20 callers in binary -- used for title enumeration.
// =============================================================================
namespace held_title_refs {
    constexpr size_t LANDED_DATA_OFFSET = 0x1E0;  // CONFIRMED: offset within SLandedCharacterData
    constexpr size_t ARRAY_PTR          = 0x00;   // CONFIRMED: uint32* to title ID array
    constexpr size_t COUNT              = 0x0C;   // CONFIRMED: int32, number of entries
}

// =============================================================================
// Council Type Object Offsets (looked up via globals::COUNCIL_TYPE_DB / g_CouncilDB)
// DB at DAT_0bfcef08 is keyed by RULER CHARACTER ID (entry+0x08).
// Lookup: read ruler char_id, hash-lookup in DB, validate entry+0x08 == ruler_id.
// The council_instance at *(alive_data+0x4E8) also stores this ID at instance+0x04,
// but the instance ptr is often NULL (lazy init), returning static default w/ 0xFFFFFFFF.
// RECOMMENDED: Direct DB lookup by ruler char_id, bypassing council_instance entirely.
// =============================================================================
namespace council_type_obj {
    constexpr size_t RULER_CHAR_ID      = 0x08;   // CONFIRMED: uint32, ruler character ID (DB match key)
    // NOTE: Previously mislabeled as TYPE_ID. The "council_type_id" IS the ruler's char ID.
    // The old RULER_CHAR_ID at +0x3A0 was WRONG -- that offset is an allocator/sentinel ptr.
    constexpr size_t POSITION_TRACKER   = 0x1B8;  // MEDIUM: ptr to position assignment tracker (R10-1) — may be wrong, see POSITION_ARRAY_PTR at 0x3C0
    // OLD: constexpr size_t RULER_CHAR_ID = 0x3A0; -- WRONG, this is an allocator ptr (0xbf9efc0 in ctor)
    constexpr size_t MAX_COUNCILLORS    = 0x41C;  // CONFIRMED: int32, max councillor slots (R10-1)
    constexpr size_t SUSPENDED_FLAG     = 0x419;  // HIGH: byte, council suspended flag (R10-1)
    // Councillor hash map (AUTHORITATIVE source — used by CCouncilMember::ValidateAssignment)
    // FNV-1a hash map: 8-byte entries (byte present_flag + 3 pad + uint32 char_id)
    constexpr size_t COUNCILLOR_HASHMAP       = 0x370;  // HIGH: ptr to hash table (8-byte entries)
    constexpr size_t COUNCILLOR_HASHMAP_MASK  = 0x37C;  // HIGH: int32, mask (capacity = mask + 1)
    constexpr size_t COUNCILLOR_HASHMAP_ENTRY = 8;      // stride per hash entry
    // Position entry array (0x30-byte entries with position data + assigned char IDs)
    constexpr size_t POSITION_ARRAY_PTR       = 0x3C0;  // HIGH: ptr to 0x30-byte position entries
    constexpr size_t POSITION_ARRAY_COUNT     = 0x3CC;  // HIGH: int32, number of position entries
    constexpr size_t POS_ENTRY_STRIDE         = 0x30;   // bytes per position entry
    constexpr size_t POS_ENTRY_DATA_PTR       = 0x00;   // ptr to position data obj (pos_data+0x08 = task_type_id)
    constexpr size_t POS_ENTRY_CHAR_ARRAY     = 0x28;   // ptr to uint32[] assigned char IDs
    constexpr size_t POS_ENTRY_CHAR_COUNT     = 0x34;   // int32, count of assigned chars
    // Legacy flat array (unreliable cache — may be nil at runtime)
    constexpr size_t COUNCILLOR_IDS     = 0x520;  // MEDIUM: ptr to uint32[] councillor char IDs (cache, may be nil)
    constexpr size_t COUNCILLOR_COUNT   = 0x52C;  // MEDIUM: int32, number of councillors (cache)
    constexpr size_t POSITION_DEFS      = 0x580;  // HIGH: ptr to position definitions array
    constexpr size_t POSITION_COUNT     = 0x58C;  // HIGH: int32, number of positions
    constexpr size_t POSITION_STRIDE    = 0xA0;   // stride for position defs (160 bytes)
    constexpr size_t POSITION_BASE      = 0x60;   // start offset within position array
    constexpr size_t POS_IS_ACTIVE      = 0x3C;   // byte, non-zero = position filled (within position entry)
    constexpr size_t TASK_ARRAY         = 0x590;  // MEDIUM: ptr to active council task array
    constexpr size_t TASK_COUNT         = 0x59C;  // MEDIUM: int32
    constexpr size_t TASK_STRIDE        = 0x48;   // bytes per task entry
    // Named position slots (0x10 bytes each)
    constexpr size_t SLOT_MARSHAL       = 0xF20;  // HIGH: marshal position slot
    constexpr size_t SLOT_CHAPLAIN      = 0xF30;  // HIGH: court chaplain position slot
}

// =============================================================================
// Position Assignment Tracker (from *(council_type_obj + 0x1B8))
// R10-1: Tracks which characters are assigned to which council positions.
// =============================================================================
namespace pos_tracker {
    constexpr size_t ASSIGN_ARRAY       = 0x230;  // CONFIRMED: ptr to uint32[] assignment IDs (R10-1)
    constexpr size_t ASSIGN_COUNT       = 0x23C;  // CONFIRMED: int32, number of assignments (R10-1)
}

// =============================================================================
// Council Position Assignment Entry (looked up in COUNCIL_POSITION_DB by assignment ID)
// R10-1: Each entry links a position type to an assigned character.
// =============================================================================
namespace council_assign {
    constexpr size_t ASSIGN_ID          = 0x10;   // CONFIRMED: uint32, assignment match key (R10-1)
    constexpr size_t INTERMEDIATE_PTR   = 0x18;   // CONFIRMED: ptr to intermediate obj (R10-1)
    constexpr size_t ASSIGNED_CHAR_ID   = 0x38;   // CONFIRMED: uint32, assigned councillor char ID (R10-1)
}

// =============================================================================
// CCouncilPositionType (from *(*(assign_entry + 0x18) + 0x30))
// R10-1: Position type with name string (e.g. "councillor_chancellor").
// =============================================================================
namespace council_pos_type {
    constexpr size_t NAME_KEY           = 0x10;   // CONFIRMED: std::string, e.g. "councillor_chancellor" (R10-1)
}

// =============================================================================
// CSuccessionElectionState (at CLandedTitle+0x280, present on ALL titles)
// Stores candidates, electors, and votes for succession elections.
// Non-elective titles have zero counts. See re/findings/re_council_voting.md.
// =============================================================================
namespace election_state {
    constexpr size_t OWNING_TITLE        = 0x08;   // HIGH: CLandedTitle* back-pointer
    constexpr size_t CANDIDATE_ARRAY     = 0x10;   // HIGH: uint32* candidate char ref array
    constexpr size_t CANDIDATE_CAPACITY  = 0x18;   // HIGH: int32
    constexpr size_t CANDIDATE_COUNT     = 0x1C;   // HIGH: int32
    constexpr size_t CANDIDATE_ALLOCATOR = 0x20;   // HIGH: CPdxAllocator*
    constexpr size_t ELECTOR_ARRAY       = 0x28;   // HIGH: uint32* elector char ref array
    constexpr size_t ELECTOR_CAPACITY    = 0x30;   // MEDIUM: int32
    constexpr size_t ELECTOR_COUNT       = 0x34;   // HIGH: int32
    constexpr size_t ELECTOR_ALLOCATOR   = 0x38;   // HIGH: CPdxAllocator*
    constexpr size_t VOTE_ARRAY          = 0x40;   // HIGH: SVoteEntry* (12-byte stride)
    constexpr size_t VOTE_CAPACITY       = 0x48;   // HIGH: int32
    constexpr size_t VOTE_COUNT          = 0x4C;   // HIGH: int32
    constexpr size_t VOTE_ALLOCATOR      = 0x50;   // HIGH: CPdxAllocator*
}

// SVoteEntry (12 bytes): stored in election_state::VOTE_ARRAY
namespace vote_entry {
    constexpr size_t VOTER_CHAR_REF      = 0x00;   // HIGH: int32, who is voting
    constexpr size_t CANDIDATE_CHAR_REF  = 0x04;   // HIGH: int32, who they vote for
    constexpr size_t VOTE_WEIGHT         = 0x08;   // HIGH: int32, vote score/weight
    constexpr size_t STRIDE              = 0x0C;   // 12 bytes per entry
}

// =============================================================================
// CLaw struct (0x21F8 bytes per object, from REALM_LAW_DB)
// Note: REALM_LAW_DB global is ptr-to-DB (dereference once), then +0x88=ptr array, +0x94=count.
// 188 laws in 27 groups. DB stores CLaw** (pointer array), NOT inline objects.
// Crown authority encoded in crown_authority group (levels 0-3).
// =============================================================================
namespace realm_law_entry {
    constexpr size_t OBJECT_SIZE         = 0x21F8;  // HIGH: actual CLaw object size (8696 bytes) — corrects prior 0x2D8
    constexpr size_t LAW_ID              = 0x08;   // HIGH: uint32, sequential 0-187
    constexpr size_t LAW_NAME_HASH       = 0x0C;   // HIGH: uint32, MurmurHash3
    constexpr size_t LAW_NAME            = 0x10;   // HIGH: std::string (e.g. "crown_authority_0")
    constexpr size_t LAW_GROUP_PTR       = 0x30;   // HIGH: CLawGroup* pointer
    constexpr size_t LAW_TOOLTIP         = 0xC8;   // HIGH: std::string ("TOOLTIP:LAW,crown_authority_0 L; ...")
    constexpr size_t ON_PASS_SOURCE      = 0x120;  // HIGH: std::string, source file (e.g. "common/laws/00_realm_laws.txt")
    constexpr size_t ON_PASS_SCOPE       = 0x138;  // HIGH: std::string, on_pass scope (e.g. "crown_authority_0:on_pass")
    constexpr size_t CAN_KEEP_SOURCE     = 0x4E0;  // HIGH: std::string, can_keep source file
    constexpr size_t CAN_KEEP_SCOPE      = 0x4F8;  // HIGH: std::string, can_keep scope (e.g. "crown_authority_0:can_keep")
    constexpr size_t SUCCESSION_RULES    = 0xB08;  // HIGH: SSuccessionRule (6 bytes: order, traversal, gender, rank, division, faith)
}

// CLawGroup struct (describes a law category, e.g. "crown_authority")
namespace law_group {
    constexpr size_t GROUP_ID            = 0x08;   // HIGH: uint32, 0-26
    constexpr size_t GROUP_NAME_HASH     = 0x0C;   // HIGH: uint32, MurmurHash3
    constexpr size_t GROUP_NAME          = 0x10;   // HIGH: std::string (e.g. "crown_authority")
    constexpr size_t MEMBER_LAW_ARRAY    = 0x48;   // HIGH: CLaw** array of member laws (ordered by level)
    constexpr size_t MEMBER_COUNT        = 0x50;   // HIGH: uint32
    constexpr size_t MEMBER_CAPACITY     = 0x54;   // HIGH: uint32
    constexpr size_t DEFAULT_LAW_NAME    = 0x60;   // HIGH: std::string, default law for this group
}

// RealmLawDB global structure (after deref from REALM_LAW_DB global)
namespace realm_law_db {
    constexpr size_t LAW_PTR_ARRAY       = 0x88;   // HIGH: CLaw** pointer array (188 entries)
    constexpr size_t LAW_COUNT           = 0x94;   // HIGH: uint32 (=188)
    constexpr size_t HASH_TABLE          = 0x1B0;  // HIGH: ptr, MurmurHash3 lookup table
    constexpr size_t HASH_MASK           = 0x1BC;  // HIGH: uint32 (=0xFF, 256 buckets)
}

// Title realm data → active law container (on *(char+0x1B8))
namespace title_realm_laws {
    constexpr size_t ACTIVE_LAW_ARRAY    = 0x200;  // HIGH: CLaw** (ptr array of active laws)
    constexpr size_t ACTIVE_LAW_COUNT    = 0x208;  // HIGH: uint32
    constexpr size_t ACTIVE_LAW_CAPACITY = 0x20C;  // HIGH: uint32
}

// =============================================================================
// CLandedTitle Object Offsets (Round 4 CONFIRMED from ANALYSIS_title_chain.md)
// Lookup: *(*(LANDED_TITLE_DB+0x20) + 8 + (id & 0xffffff) * 0x10), verify ID at +0x10
// =============================================================================
namespace title_obj {
    constexpr size_t TITLE_ID           = 0x10;   // HIGH: uint32, title ID (verify on lookup)
    constexpr size_t TITLE_DISPLAY_NAME = 0x58;   // HIGH: std::string (GCC SSO), custom/localized display name
    constexpr size_t TITLE_TYPE_PTR     = 0x178;  // HIGH: ptr, title definition/type
    // Tier accessed via: *(*(title+TITLE_TYPE_PTR) + TITLE_TYPE_TIER)
    constexpr size_t TITLE_TYPE_TIER    = 0x54;   // HIGH: int32 on type ptr (1=barony,2=county,3=duchy,4=kingdom,5=empire)
    // CORRECTED 2026-06-22 (#116): the AUTHORITATIVE de-jure parent title ID is at
    // 0x250, NOT 0x230. Engine-verified: FUN_06222410 (DE_JURE_MATCH @ image 0x06222410)
    // walks the de-jure hierarchy via *(title+0x250) & 0xffffff and recurses up. Core-dump
    // validated (ck3_20260421_195552, 1097 save): for counties 0x250 → correct de-jure duchy
    // (c_durham/c_cumberland→d_northumberland 134; the Ridings→d_york 165), while 0x230 → a
    // DE-FACTO/holding hierarchy field (constant d_hereford 205 for every English county, and
    // garbage e_japan/h_china for others). 0x250 is correct at ALL tiers (duchy→kingdom,
    // kingdom→empire). 0x230 was previously mislabeled HIGH and silently returned the wrong
    // duchy for the conquest/de-jure queries. — HIGH (core-dump + engine-decompile verified)
    constexpr size_t DE_JURE_PARENT     = 0x250;  // HIGH: uint32, AUTHORITATIVE de jure parent title ID (CORRECTED #116)
    constexpr size_t DE_FACTO_PARENT    = 0x230;  // MEDIUM: uint32, de-facto/holding hierarchy parent (NOT de-jure — see #116)
    constexpr size_t SUB_TITLE_ARRAY    = 0x258;  // HIGH: ptr, sub-title ID (uint32) array (= de-jure CHILDREN; decompile-verified in CheckDynastyPartitionPermission FUN_061ccbd0 reads *(title+600)=0x258, also is_in_dejure_domain + de_jure_match) — re_dejure_meter_and_mint.md
    constexpr size_t SUB_TITLE_COUNT    = 0x264;  // HIGH: int32, de-jure children count (decompile-verified, same 3 functions) — re_dejure_meter_and_mint.md
    constexpr size_t HOLDER_CHAR_ID     = 0x270;  // HIGH: uint32, holder character ID (-1=none)
    constexpr size_t DESIGNATED_HEIR_ID = 0x274;  // HIGH: int32, designated heir char ID (-1=none) (R7-5)
    constexpr size_t HOLDER_FLAG        = 0x278;  // MEDIUM: byte, title status flag
    constexpr size_t ELECTION_STATE_PTR = 0x280;  // HIGH: CSuccessionElectionState* (all titles, zero counts if non-elective)

    // ─────────────────────────────────────────────────────────────────────────
    // CORRECTED 2026-06-27 (re_per_title_succession.md §0/§5): title+0x380 is a
    // CPdxVector<CTitleSuccessionLaw*> — a vector of TITLE-SCOPED succession-law
    // OBJECTS — NOT an inline 6-byte SSuccessionRule struct. The prior
    // SUCC_ORDER=0x380 / … / SUCC_DIVISION=0x384 raw-byte reads returned the LOW
    // BYTES OF THE VECTOR'S DATA POINTER (garbage: tid 16/17/105 read
    // order=208/240/0, division=179 → decode "unknown"; default no-custom titles
    // read order=0/division=0 → always "single_heir", ignoring the realm's law).
    // Core-dump verified (ck3_20260421_195552). The 6-byte SSuccessionRule layout
    // is correct ONLY on a RESOLVED/MERGED value struct (each law element + 0xB08,
    // or the output of init_succession_rule_from_title). See SUCC_LAW_* below and
    // SUCC_RULE_* (value-struct sub-offsets). The fully-merged rule for a 0-custom
    // title comes from the holder's REALM law (title_realm_laws::ACTIVE_LAW_ARRAY
    // at char+0x1B8 +0x200, each CLaw* + realm_law_entry::SUCCESSION_RULES 0xB08).
    constexpr size_t SUCC_LAW_VECTOR_DATA  = 0x380;  // HIGH: ptr → CTitleSuccessionLaw*[] (8-byte stride), 0=none (CPdxVector data)
    constexpr size_t SUCC_LAW_VECTOR_COUNT = 0x38C;  // HIGH: int32, # title-scoped succession laws (0=use realm law). WAS SUCC_HAS_CUSTOM_RULES.
    constexpr size_t SUCC_LAW_RULE_AT_ELEM = 0xB08;  // HIGH: SSuccessionRule VALUE carried by each law element (matches realm_law_entry::SUCCESSION_RULES)
    // SSuccessionRule VALUE-struct sub-offsets — apply to a RESOLVED/MERGED rule
    // struct (init_succession_rule_from_title output) OR to (law_element + 0xB08) /
    // (CLaw* + 0xB08). DO NOT read these directly at title+0x38x.
    constexpr size_t SUCC_RULE_ORDER       = 0x00;  // byte: 0=inheritance, 1=election, 2=appointment, 8=noble_family, 9=unset
    constexpr size_t SUCC_RULE_TRAVERSAL   = 0x01;  // byte: 0=children, 3=unset
    constexpr size_t SUCC_RULE_GENDER      = 0x02;  // byte: 0=male_only, 1=male_pref, 2=equal, 3=female_pref, 4=female_only, 5=unset
    constexpr size_t SUCC_RULE_RANK        = 0x03;  // byte: 0=confederate_partition, 1=regular_partition, 2=unset
    constexpr size_t SUCC_RULE_DIVISION    = 0x04;  // byte: 0=single_heir, 1=partition, 2=unset
    constexpr size_t SUCC_RULE_FAITH_REQ   = 0x06;  // byte: 0-2=requirement levels, 3=unset
    // DEPRECATED ALIASES (raw title+0x38x reads are WRONG — see correction above).
    // Retained only so existing call sites compile; every reader should migrate to
    // the SUCC_LAW_* vector walk + realm-law fallback (re_per_title_succession.md).
    constexpr size_t SUCCESSION_RULES   = 0x380;  // DEPRECATED: CPdxVector data ptr, NOT an inline SSuccessionRule
    // DEPRECATED raw-byte title+0x38x reads — these DO NOT yield rule axes (they
    // read into the vector's data pointer). Kept so legacy call sites compile;
    // they are WRONG and must be migrated to the SUCC_LAW_*/realm-law path. New
    // code MUST NOT use these. (re_per_title_succession.md §0/§5)
    constexpr size_t SUCC_ORDER         = 0x380;  // DEPRECATED/WRONG: low byte of SUCC_LAW_VECTOR_DATA ptr
    constexpr size_t SUCC_TRAVERSAL     = 0x381;  // DEPRECATED/WRONG
    constexpr size_t SUCC_GENDER        = 0x382;  // DEPRECATED/WRONG
    constexpr size_t SUCC_RANK          = 0x383;  // DEPRECATED/WRONG
    constexpr size_t SUCC_DIVISION      = 0x384;  // DEPRECATED/WRONG: byte 4 of the data ptr, not division
    constexpr size_t SUCC_FAITH_REQ     = 0x385;  // DEPRECATED/WRONG
    constexpr size_t SUCC_HAS_CUSTOM_RULES = 0x38C; // DEPRECATED ALIAS of SUCC_LAW_VECTOR_COUNT (count of title laws; non-zero ⇒ has custom)
    // SSuccessionRule extended fields (R8V-6, CompareSuccessionRules FUN_06535520) —
    // these describe the VALUE-struct extended layout (offsets from the rule base).
    constexpr size_t SUCC_SCOPE_PTR_1   = 0x10;   // HIGH: 8-byte scope pointer (rule+0x10)
    constexpr size_t SUCC_SCOPE_PTR_2   = 0x18;   // HIGH: 8-byte scope pointer (rule+0x18)
    constexpr size_t SUCC_SCOPE_PTR_3   = 0x20;   // HIGH: 8-byte scope pointer (rule+0x20)
    constexpr size_t SUCC_GOV_TYPES_PTR = 0x28;   // HIGH: ptr to government types array (rule+0x28)
    constexpr size_t SUCC_GOV_TYPES_CNT = 0x34;   // HIGH: int32, government types count (rule+0x34)

    // R-partition: CLandedTitle flags used by partition algorithm (semantics decoded R-partition-2)
    constexpr size_t TITLE_PARTITION_FLAG_2A = 0x2A; // MEDIUM: byte, if set skips IsHeirEligible check (heir always eligible) (R-partition-2)
    constexpr size_t TITLE_PRIMARY_FLAG_2C   = 0x2C; // MEDIUM: byte, if set AND primary heir ineligible → direct GrantTitle (R-partition-2)
    constexpr size_t TITLE_SKIP_FLAG_2D      = 0x2D; // MEDIUM: byte, if set title excluded from distribution loop entirely (R-partition-2)
    constexpr size_t TITLE_PARTITION_FLAG_31  = 0x31; // MEDIUM: byte, if set title excluded from tier-loop unassigned collection (R-partition-2)
    constexpr size_t TITLE_CONTESTED_FLAG    = 0x318; // MEDIUM: byte, title involved in war/claim — excluded from title-heir pair building (R-partition-2)

    // R-partition-2: Dynasty partition permission law (used by CheckDynastyPartitionPermission)
    constexpr size_t TITLE_REALM_LAW_PTR     = 0x178; // HIGH: ptr to realm/law data struct (R-partition-2)
    // At *(title+0x178)+0x54: partition law value (1=blocked, 2=leaf-only, 3-6=recursive check)

    // CLandedTitle de jure hierarchy (used by partition for scoring)
    constexpr size_t DE_JURE_CHILDREN_COUNT  = 0x264; // MEDIUM: int32, de jure children count
    constexpr size_t DE_JURE_CHILDREN_ARRAY  = 0x258; // MEDIUM: ptr to uint32[] de jure child title IDs

    // Succession heir list (R15: cached on each title object)
    constexpr size_t SUCCESSION_HEIRS      = 0x290;  // HIGH: uint32[] array of ordered heir character refs (packed TPdxRef, mask 0xFFFFFF)
    constexpr size_t SUCCESSION_HEIR_COUNT = 0x29C;  // HIGH: int32, heir count

    // Claimant list (R25 Phase 4: confirmed via CLandedTitle::AddClaimant/RemoveClaimant decompilation)
    // Array of uint32 character refs (TPdxRef, mask 0xFFFFFF for char DB lookup), stride 4 bytes.
    // AddClaimant uses binary search for duplicate check. RemoveClaimant uses linear scan + compact.
    // Functions: AddClaimant at 0x051ceb80, RemoveClaimant at 0x051cee60 (offsets = Ghidra VA - 0x100000)
    constexpr size_t CLAIMANT_ARRAY_PTR    = 0x2A8;  // HIGH: ptr to uint32[] of claimant character refs
    constexpr size_t CLAIMANT_CAPACITY     = 0x2B0;  // HIGH: int32, allocated capacity
    constexpr size_t CLAIMANT_COUNT        = 0x2B4;  // HIGH: int32, current claimant count
    constexpr size_t CLAIMANT_ALLOCATOR    = 0x2B8;  // HIGH: ptr to CPdxAllocator

    // Title holder-history list (CPdxArray<CTitleHolderHistoryEntry>) — TITLE-HISTORY lane,
    // validated on core dump ck3_20260421_195552 (1097 save) across kingdom/empire titles.
    // Chronological succession of holders: who held the title when. Entries are 0x18 (24) bytes,
    // monotonically increasing by date. The LAST entry's char == the title's current HOLDER_CHAR_ID.
    // Entry layout:
    //   +0x00 vptr (shared CTitleHolderHistoryEntry vtable, core-raw 0xbb7df10 / image-VA 0xbc7df10)
    //   +0x08 uint32 event type  (4 = grant/creation, 20 = succession/holder-change, 6 = destruction w/ char=0xFFFFFF)
    //   +0x0C uint32 character ref (TPdxRef, mask 0xFFFFFF for char DB lookup; 0xFFFFFF = none)
    //   +0x10 uint32 internal day-count date (monotonic; chronology key)
    //   +0x14 uint32 packed date = (year<<16)|(month<<8)|day  (e.g. 0x0445020A = 1093.2.10)
    constexpr size_t TITLE_HOLDER_HISTORY_PTR   = 0x2C0;  // HIGH: ptr to CTitleHolderHistoryEntry[] (core-dump validated)
    constexpr size_t TITLE_HOLDER_HISTORY_COUNT = 0x2C8;  // HIGH: int32, entry count
    constexpr size_t TITLE_HOLDER_HISTORY_CAP   = 0x2CC;  // HIGH: int32, capacity
    constexpr size_t TITLE_HOLDER_HISTORY_ALLOC = 0x2D0;  // HIGH: ptr to CPdxAllocator
    constexpr size_t THHE_STRIDE          = 0x18;  // HIGH: CTitleHolderHistoryEntry size
    constexpr size_t THHE_TYPE            = 0x08;  // HIGH: uint32 event type (4=grant,20=succession,6=destroy)
    constexpr size_t THHE_CHAR_REF        = 0x0C;  // HIGH: uint32 char TPdxRef (&0xFFFFFF)
    constexpr size_t THHE_DAYCOUNT        = 0x10;  // HIGH: uint32 internal day count
    constexpr size_t THHE_PACKED_DATE     = 0x14;  // HIGH: uint32 (year<<16)|(month<<8)|day

    // R19: title+0x428 is a SENTINEL on CLandedTitle, NOT county data!
    // County data lives on CProvince objects at province+0x428.
    // Navigation: county_title → first barony sub-title → province (barony+0x490) → county_data (province+0x428)
    constexpr size_t TITLE_STATUS       = 0x480;  // MEDIUM: int32 (0=normal, 1=titular, 2=dynamic)
    constexpr size_t PROVINCE_PTR       = 0x490;  // HIGH: CProvince* (only valid on barony-tier titles, R19)
}

// =============================================================================
// County Data Offsets (R19 CONFIRMED: accessed via province+0x428, NOT title+0x428)
// Navigation: county_title → barony (sub_title[0]) → province (barony+0x490) → county_data (province+0x428)
// =============================================================================
namespace county {
    constexpr size_t COUNTY_TITLE_ID    = 0x18;   // HIGH: uint32 (mask 0xFFFFFF), back-reference to this CCountyData's
                                                   //   county title id. Core-dump ck3_20260421 verified: 400/400 counties
                                                   //   read back their own tid at +0x18 with no false match in the first
                                                   //   0x60 bytes. Lets a CCountyData* (e.g. a neighbor from NEIGHBOR_ARRAY)
                                                   //   be mapped back to its county title → de-jure duchy WITHOUT a full
                                                   //   title-DB scan (the conquest-target bounded-candidate-set fix).
    constexpr size_t PROVINCE_ARRAY     = 0x28;   // HIGH: ptr, array of province ptrs (stride 8B)
    constexpr size_t PROVINCE_COUNT     = 0x34;   // HIGH: int32, number of provinces
    constexpr size_t NEIGHBOR_ARRAY     = 0x40;   // HIGH: ptr, array of neighboring CCountyData* (decompile-verified FUN_05cd8230)
                                                   //   #110: used by do_query_conquest_targets to build the county-adjacency
                                                   //   reach frontier (the duchy->kingdom de-jure link is 0 pre-kingdom, so
                                                   //   adjacency is the reachability signal — re_conquest_targets_query.md)
    constexpr size_t NEIGHBOR_COUNT     = 0x4C;   // HIGH: int32, count of neighboring counties

    constexpr size_t FAITH_ID           = 0x208;  // HIGH: uint32, faith DB ID (verified: deserialized then looked up in g_FaithDB)
                                                   //   #127 (F4): core-dump ck3_20260421 validated — c_middlesex/c_essex/etc.
                                                   //   read faith 23 = "catholic" via FAITH_DB(0x0bedad78)+faith::FAITH_KEY(0x98).
    constexpr size_t CULTURE_ID         = 0x20C;  // HIGH (upgraded #127, F4): uint32, culture DB ID (was MEDIUM-HIGH; deserialized
                                                   //   via FUN_05b40f90, tag 0x27f4). Core-dump validated: counties read culture
                                                   //   229/231 varying per county; resolve via CULTURE_INDEXED_DB(0x0beda7a8)+NAME_KEY(0x10).
    constexpr size_t DEVELOPMENT        = 0x210;  // HIGH: int32, development level (0-100, capped at 100)
    constexpr size_t DEV_PROGRESS       = 0x218;  // HIGH: int64 fixed-point ÷ 100000, progress toward next dev level
                                                   //   threshold = 10000000 (100.0): triggers level-up, resets to 0
                                                   //   decompile-verified: FUN_05cd7e90 (ApplyDevelopmentProgress)
    constexpr size_t CONTROL            = 0x220;  // HIGH: int64 fixed-point ÷ 100000, range [0, 100.0] (R16)
}

// =============================================================================
// Building Type Offsets (R16: CBuildingType — no separate CBuilding struct)
// Slots store CBuildingType* directly; the type IS the level.
// Slot array: *(province+0x1F8+0x18), count: province+0x1F8+0x24, stride 0x10
// =============================================================================
namespace building_type {
    constexpr size_t NAME_PTR           = 0x10;   // HIGH: char*, e.g. "castle_01", "temple_02" (R16)
    constexpr size_t LEVEL              = 0x30;   // HIGH: int32, building level (the type IS the level) (R16)
    constexpr size_t TYPE_CATEGORY      = 0x48;   // MEDIUM: uint32, 1095=constructible, 1825=holding/special
    constexpr size_t MODIFIER_GROUP     = 0x60;   // MEDIUM: ptr, modifier group descriptor
    constexpr size_t SCRIPTED_VALUE_1   = 0xC0;   // MEDIUM: uint32, global scripted value index (cost slot 1)
    constexpr size_t SCRIPTED_VALUE_2   = 0x120;  // MEDIUM: uint32, global scripted value index (cost slot 2)
    constexpr size_t SCRIPTED_VALUE_3   = 0x180;  // MEDIUM: uint32, global scripted value index (cost slot 3)
    constexpr size_t SOURCE_FILE_PATH   = 0x198;  // MEDIUM: std::string, e.g. "common/buildings/..."
    constexpr size_t ENABLE_TRIGGER     = 0x1B0;  // MEDIUM: std::string, e.g. "building_01:is_enabled"
    constexpr size_t NEXT_LEVEL_PTR     = 0x1D8;  // LOW: ptr, CBuildingType* for next upgrade level
}

// =============================================================================
// Building Type Database (singleton at globals::BUILDING_TYPE_DB)
// Access: ptr db = read_ptr(resolve(BUILDING_TYPE_DB))
//   db+0x88 = array of CBuildingType*, db+0x90 = entry count
// =============================================================================
namespace building_db {
    constexpr size_t PATH_STRING        = 0x60;   // MEDIUM: std::string "common/buildings"
    constexpr size_t ENTRY_TABLE        = 0x88;   // MEDIUM: ptr, array of CBuildingType*
    constexpr size_t ENTRY_COUNT        = 0x90;   // MEDIUM: uint32, number of building types (957)
}

// =============================================================================
// Trait Descriptor Offsets (Round 4 CONFIRMED from ANALYSIS_trait_names.md)
// CJominiScriptDbObject returned by FUN_05cc7c00(CTrait*)
// =============================================================================
namespace trait_descriptor {
    constexpr size_t NAME_PTR           = 0x10;   // HIGH: char*, trait name string
    constexpr size_t NAME_LENGTH        = 0x18;   // HIGH: size_t, name string length
}

// =============================================================================
// Title DB Lookup Pattern
// Same hash table pattern as CHARACTER_DB:
//   element = *(*(db+0x20) + 8 + (id & 0xffffff) * 0x10)
//   verify: *(element + title_obj::TITLE_ID) == id
// =============================================================================
namespace title_db {
    constexpr size_t ARRAY_PTR          = 0x20;   // ptr to element array
    constexpr size_t ARRAY_COUNT        = 0x2C;   // int32, element count
    constexpr size_t ELEMENT_STRIDE     = 0x10;   // stride between elements
    constexpr size_t ELEMENT_PTR_OFFSET = 0x08;   // offset to ptr within element
    constexpr size_t ID_VERIFY_OFFSET   = 0x10;   // where to verify title ID on resolved ptr
}

// =============================================================================
// DB Object Name (universal for all CJominiScriptDbObject subclasses)
// ALL interactions, decisions, traits, schemes etc. have std::string at +0x10
// Confirmed via FUN_05a8d720 = return param+0x10 (GetKey accessor)
// std::string layout (GCC libstdc++ SSO): _M_p at +0, length at +8, local_buf at +16
// =============================================================================
namespace db_object {
    constexpr size_t NAME_KEY           = 0x10;   // HIGH: std::string starts here (_M_p)
    constexpr size_t NAME_LENGTH        = 0x18;   // HIGH: std::string _M_string_length
}

// =============================================================================
// Interaction DB Direct Array (alternative to hash table iteration)
// =============================================================================
namespace interaction_db {
    constexpr size_t ARRAY_BASE         = 0x1020; // HIGH: ptr array of interaction objects (stride 8)
    constexpr size_t NULL_INDEX         = 0x26;   // HIGH: index 38 = null sentinel (end of valid entries)

    // Named interaction indices (R11-6: used with INTERACTION_LOOKUP_BY_IDX)
    constexpr uint8_t IDX_DECLARE_WAR   = 0x0D;   // HIGH: declare_war_interaction (R11-6)
}

// =============================================================================
// CCharacterInteractionType — Interaction definition (loaded from game scripts)
// Option (send_option / CB option) fields within interaction definition
// =============================================================================
namespace interaction_def {
    constexpr size_t OPTION_ARRAY_PTR     = 0x2530; // ptr: array of option entries (each 0x7D0 = 2000 bytes)
    constexpr size_t OPTION_COUNT         = 0x253C; // int32: number of options in the array
    constexpr size_t IS_SINGLE_SELECT     = 0x2A36; // byte: 0=multi-select (send_options checkboxes), non-0=single-select (CB radio buttons)
    constexpr size_t HAS_ON_AUTO_ACCEPT   = 0x2A32; // byte: secondary validation flag (0 for all tested interactions, ck-x328)
    constexpr size_t BYPASS_DUP_CHECK     = 0x2A39; // HIGH: byte, nonzero=skip the duplicate-interaction guard at funcs::INTERACTION_HAS_NO_DUPLICATE (ck-jvyj/ck-go1m)
    constexpr size_t REQUIRES_VALIDATION  = 0x2A3E; // byte: non-0 if FullValidation required after option change

    // Option entry layout (0x7D0 = 2000 bytes per option, at OPTION_ARRAY_PTR + index * 0x7D0)
    constexpr size_t OPT_IS_VALID         = 0x000;  // trigger block (0xE0 bytes): base validity check
    constexpr size_t OPT_IS_SHOWN         = 0x0E0;  // trigger block (0xE0 bytes): visibility check
    constexpr size_t OPT_STARTS_ENABLED   = 0x1C0;  // trigger block: starts_enabled (only if OPT_HAS_STARTS_ENABLED != 0)
    constexpr size_t OPT_HAS_STARTS_ENABLED = 0x21C; // int32: if 0, skip starts_enabled evaluation
    constexpr size_t OPT_FAILURES_ONLY    = 0x2A0;  // trigger block: is_valid_showing_failures_only
    constexpr size_t OPT_FLAG_KEY_HASH    = 0x3A8;  // uint32: hashed flag name (scope name for this option)
    constexpr size_t OPT_STRIDE           = 0x7D0;  // 2000 bytes per option entry

    // Known send_option flags for release_from_prison_interaction (index order from game scripts):
    //   0: demand_conversion
    //   1: renounce_claims
    //   2: banish
    //   3: gain_hook
    //   4: take_vows
    //   5: become_executioner
    //   6: recruit
    //   7: disfigure
    //   8: blind
    //   9: castrate
    //  10: demand_admin
    // NOTE: Indices are determined by script load order — verify at runtime via flag_key hash.
    // Use game_string_hash("banish", 6) to compute flag_key hash and match.

    // Trigger/scope offsets within CCharacterInteractionType (marriage eligibility investigation)
    constexpr size_t TARGET_ENTRY_ARRAY   = 0x2548; // HIGH: ptr to candidate entry array (entries of 0x48 stride)
    constexpr size_t TARGET_ENTRY_COUNT   = 0x2554; // HIGH: int32, number of candidate entries
    constexpr size_t PRE_FILTER_DATA      = 0x2560; // MEDIUM: pre-filter setup data for AI candidate evaluation
    // Initial-feasibility gate read by FUN_06b42b30 (InteractionTrySend_V2 first sub-validator).
    // BLOCKING_TRIGGER fires (returns 1) → interaction NOT feasible. NULL trigger → use BLOCKING_FLAG.
    // For declare_war_interaction both are 0 (always feasible at this gate). See
    // re/findings/re_fun_06b42b30_initial_feasibility.md (ck-imsy) and
    // re/findings/re_fun_06b42b30_revalidation_2026-05-01.md (ck-2yz3).
    constexpr size_t BLOCKING_TRIGGER     = 0x2568; // HIGH: ptr, scripted blocking trigger (NULL → use BLOCKING_FLAG)
    constexpr size_t BLOCKING_FLAG        = 0x2A30; // HIGH: byte, fallback static blocking flag (used when BLOCKING_TRIGGER == NULL)
    constexpr size_t HOSTILE_TRIGGER      = 0x2570; // MEDIUM: ptr, scripted trigger for hostile interaction check
    constexpr size_t HOSTILE_FLAG         = 0x2A33; // MEDIUM: byte, static hostile interaction flag
    constexpr size_t AI_BEHAVIOR_FLAG     = 0x2A37; // MEDIUM: byte, AI-specific behavior flag
    constexpr size_t TARGET_REQUIRED_FLAG = 0x2A41; // MEDIUM: byte, whether target must be alive
    constexpr size_t USE_MODE_FLAG        = 0xB3C;  // MEDIUM: int32, 0=use trigger at +0x1480, else +0xae0
    // Trigger region — inline CAndTrigger blocks (stride 0xE0, label_ptr at slot+0x28).
    //
    // Slot identities below were cross-verified via core-dump debug labels across
    // all 38 interactions in g_InteractionDB. See
    //   re/findings/re_interaction_slot_layout_corrected.md (ck-angk re-do)
    // for the matrix, methodology, and why prior (ck-x328 / ad9d795) names were wrong.
    //
    // Existing names IS_SHOWN_TRIGGER / IS_VALID_TRIGGER / HAS_VALID_TARGET /
    // CAN_SEND_TRIGGER / SHOULD_USE_EXTRA_ICON are preserved at their existing
    // offsets. Four prior names that pointed to misidentified slots
    // (AUTO_ACCEPT_TRIGGER, ON_AUTO_ACCEPT, CAN_SEND_WITH_EXCEPTION, the old
    // CAN_SEND_TRIGGER) are explicitly handled in the deprecation block below.
    //
    // CANONICAL CORRECTED NAMES (use these in new code):
    constexpr size_t IS_SHOWN_TRIGGER             = 0xBC0;  // HIGH: scope "is_shown"
    constexpr size_t IS_HIGHLIGHTED_TRIGGER       = 0xCA0;  // HIGH (NEW ck-angk): scope "is_highlighted"
    constexpr size_t IS_VALID_TRIGGER             = 0xD80;  // HIGH: scope "is_valid_showing_failures_only"
    constexpr size_t IS_VALID_TRIGGER_AI          = 0xE60;  // HIGH (NEW ck-angk): scope "is_valid" (AI variant of IS_VALID_TRIGGER). Same offset as deprecated AUTO_ACCEPT_TRIGGER but the slot is is_valid, not auto_accept.
    constexpr size_t HAS_VALID_TARGET             = 0xF40;  // HIGH: scope "has_valid_target_showing_failures_only"
    constexpr size_t HAS_VALID_TARGET_AI          = 0x1020; // HIGH (NEW ck-angk): scope "has_valid_target" (AI variant). Same offset as deprecated ON_AUTO_ACCEPT but the slot is has_valid_target, not on_auto_accept (on_auto_accept EFFECT is at ON_AUTO_ACCEPT_EFFECT/+0x16A0).
    constexpr size_t CAN_BE_PICKED_TRIGGER        = 0x1100; // HIGH (NEW ck-angk): scope "can_be_picked" / "can_be_picked_title". Same offset as the legacy CAN_SEND_TRIGGER, but the slot is can_be_picked (picker), NOT can_send.
    // +0x11E0 reserved/unused (no debug label across all 38 interactions, ck-angk).
    constexpr size_t CAN_SEND_FINAL_TRIGGER       = 0x12C0; // HIGH (NEW ck-angk): scope "can_send" (final-send-time check; the cascade evaluates this at +0x12C0 only when InteractionTrySend_Cascade param_3 byte is non-zero). Same offset as deprecated CAN_SEND_WITH_EXCEPTION.
    constexpr size_t SHOULD_USE_EXTRA_ICON        = 0x13A0; // HIGH: scope "should_use_extra_icon"
    constexpr size_t AI_POTENTIAL_TRIGGER         = 0x1480; // HIGH (NEW ck-angk): scope "ai_potential" (AI scoring trigger)
    // +0x1560 reserved/unused (no debug label across all 38 interactions, ck-angk).

    // Effect region — inline CJominiEffect blocks (stride 0x60, label_ptr at slot+0x28).
    // All vtables = 0x55f76dabad98 in current build. ck-angk re-do.
    constexpr size_t ON_ACCEPT_EFFECT               = 0x1640; // HIGH: scope "on_accept"
    constexpr size_t ON_AUTO_ACCEPT_EFFECT          = 0x16A0; // HIGH (NEW ck-angk): scope "on_auto_accept"
    constexpr size_t PRE_AUTO_ACCEPT_EFFECT         = 0x1700; // HIGH (NEW ck-angk): scope "pre_auto_accept"
    constexpr size_t ON_SEND_EFFECT                 = 0x1760; // HIGH (NEW ck-angk): scope "on_send"
    constexpr size_t ON_DECLINE_EFFECT              = 0x17C0; // HIGH (NEW ck-angk): scope "on_decline"
    // +0x1820 / +0x1880 reserved/unused (no debug label across all 38 interactions).
    constexpr size_t ON_INTERMEDIARY_DECLINE_EFFECT = 0x18E0; // HIGH (NEW ck-angk): scope "on_intermediary_decline"

    // AI score region — different layout: vtable at slot, label at slot+0x30 (NOT +0x28).
    // Block stride 0x60. ck-angk re-do; only present on a few interactions.
    constexpr size_t AI_WILL_DO_BLOCK               = 0x1AC0; // HIGH (NEW ck-angk): scope "ai_will_do" (vtable 0x55f76db6c5d8 — MTTH/score)
    constexpr size_t AI_INTERMEDIARY_ACCEPT_BLOCK   = 0x1B20; // HIGH (NEW ck-angk): scope "ai_intermediary_accept" (vtable 0x55f76db58ec8)
    constexpr size_t AI_ACCEPT_BLOCK                = 0x1B80; // HIGH (NEW ck-angk): scope "ai_accept" (vtable 0x55f76db58ec8)

    // ───────────────────────────────────────────────────────────────────────
    // DEPRECATED ALIASES — old ck-x328 / ad9d795 names retained for callers.
    //
    // These constants resolve to the SAME OFFSETS they always did, so existing
    // bridge code (query_handlers.cpp / ai_eval_hook.cpp slot tables) reads
    // exactly the same memory as before — but the slot's semantic identity was
    // misnamed when these constants were introduced. New code should use the
    // canonical names above. Once all callers migrate, delete these aliases.
    //
    //   AUTO_ACCEPT_TRIGGER       (+0xE60)  — actually `is_valid` (AI variant of IS_VALID_TRIGGER)
    //   ON_AUTO_ACCEPT            (+0x1020) — actually `has_valid_target` (AI variant). The on_auto_accept EFFECT is at ON_AUTO_ACCEPT_EFFECT/+0x16A0.
    //   CAN_SEND_TRIGGER          (+0x1100) — actually `can_be_picked` / `can_be_picked_title`. Real can_send is at CAN_SEND_FINAL_TRIGGER/+0x12C0.
    //   CAN_SEND_WITH_EXCEPTION   (+0x12C0) — actually `can_send`. There is no "with_exception" variant.
    [[deprecated("ck-angk: slot at +0xE60 is `is_valid` (AI variant of IS_VALID_TRIGGER), not auto_accept. Use IS_VALID_TRIGGER_AI. Auto-accept is NOT a trigger slot in this struct.")]]
    constexpr size_t AUTO_ACCEPT_TRIGGER     = 0xE60;
    [[deprecated("ck-angk: slot at +0x1020 is `has_valid_target` (AI variant). Use HAS_VALID_TARGET_AI. The on_auto_accept EFFECT is at ON_AUTO_ACCEPT_EFFECT (+0x16A0).")]]
    constexpr size_t ON_AUTO_ACCEPT          = 0x1020;
    [[deprecated("ck-angk: slot at +0x1100 is `can_be_picked` (option picker), not can_send. Use CAN_BE_PICKED_TRIGGER. The real can_send slot is CAN_SEND_FINAL_TRIGGER (+0x12C0).")]]
    constexpr size_t CAN_SEND_TRIGGER        = 0x1100;
    [[deprecated("ck-angk: slot at +0x12C0 is plain `can_send`, not `can_send_with_exception`. Use CAN_SEND_FINAL_TRIGGER.")]]
    constexpr size_t CAN_SEND_WITH_EXCEPTION = 0x12C0;
    // ───────────────────────────────────────────────────────────────────────

    // Trigger blocks share vtable 0x55f76dace7b0 (CAndTrigger). Effect blocks share
    // 0x55f76dabad98 (CJominiEffect). Child triggers stored via
    // CPdxHybridInlineBufferAllocator (4 inline slots, heap overflow).
    // See re/findings/re_declare_war_trigger_trees.md for tree structures.
    constexpr size_t AI_PROBABILITY       = 0x18;   // MEDIUM: int64, AI probability threshold (per 100000)
    constexpr size_t MAX_TARGETS          = 0x40;   // MEDIUM: int32, max targets per AI evaluation cycle

    // Vtable slot offsets for CCharacterInteractionType (shared vtable at Ghidra 0x0be16ae8).
    // DECOMP-VERIFIED (ck-e58l.31): slot 7 at 0x0be16ae8+0x38 = 0x03a5c3e0, body "mov al,1; ret" —
    // a trivial stub that always returns true, ignoring `this`. This is NOT a semantic is_valid
    // check on CInteractionDef; it appears to be a base-class default (likely CPdxCommand or
    // similar). Calls through this slot on a CInteractionDef only serve as a vtable-dispatch
    // sanity ping (sigsetjmp catches vtable corruption); they do NOT filter invalid interactions.
    // Distinct from `interaction_cmd::VTABLE_IS_VALID` which names the same numeric offset but
    // applies to the CSendCharacterInteractionCommand wrapper from BUILD_SEND_INTERACTION_CMD.
    constexpr size_t VTABLE_IS_VALID      = 0x38;   // HIGH: vtable[7], trivial "return 1" stub (ck-e58l.31)
}

// =============================================================================
// Marriage Interaction Group — from g_InteractionDB
// g_InteractionDB singleton at Ghidra 0x0bfd7748 (base-rel 0x0bed7748)
// =============================================================================
namespace marriage_group {
    constexpr size_t INTERACTION_ARRAY_OFFSET = 0x1170; // HIGH: PDX array of 13 CCharacterInteractionType ptrs
    // PDX array layout: {data_ptr at +0, capacity at +8 (lo32), count at +0xC (lo32)}
    // All 13 entries share vtable at Ghidra 0x0be16ae8 (10 vfuncs, offsets 0x00-0x48)
}

// =============================================================================
// Pinned Interactions Array — from *g_InteractionDB (ck-8cns.1, ck-8cns.2)
// Write-once at DB init by FUN_06b46d50 ×16 from FUN_06b3d100 (slot registrar).
// Zero-filled first by FUN_06b3c5f0 (DB constructor) with g_NullInteraction_sentinel.
// Each 0x10 slot = { CCharacterInteractionType* ptr @ +0, tagged aux ptr @ +8 }.
// Tag byte (slot+8 low byte) = 0x01 = "binding attempted"; NOT a runtime stamp.
// Hypothesis refuted: "Gate 2 stamp at +0x1000" (ck-q402) was mistaken —
// slot 14 at +0x1000 is influence_candidacy, NOT declare_war. Slot 7 is declare_war.
// =============================================================================
// Pending-interaction registry — param to funcs::INTERACTION_HAS_NO_DUPLICATE.
// Reached via *(GameStateRoot+0xa0) + 0xCE48 (FUN_06061850). Tracks active
// interactions for duplicate-conflict detection. See ck-jvyj/ck-go1m and
// re/findings/re_game_state_validation_062e5450.md.
// =============================================================================
namespace interaction_pending_registry {
    constexpr size_t ENTRIES_PTR    = 0x70;  // HIGH: ptr[] — bucket-pointer array (linear scan, 4-at-a-time unrolled)
    constexpr size_t ENTRIES_COUNT  = 0x7C;  // HIGH: int32 — bucket count

    // Per-bucket layout (at *entries[i]):
    constexpr size_t BUCKET_CHAR_ID         = 0x08;  // HIGH: int32 — owner char ID (lookup key)
    constexpr size_t BUCKET_LIVE_LIST_PTR   = 0x10;  // HIGH: ptr — branch-2 sub-array of live char IDs (4 bytes/entry)
    constexpr size_t BUCKET_LIVE_LIST_CNT   = 0x1C;  // HIGH: int32 — branch-2 sub-array count
    constexpr size_t BUCKET_RECORDS_PTR     = 0x28;  // HIGH: ptr — branch-1 sub-array of full interaction records
    constexpr size_t BUCKET_RECORDS_CNT     = 0x34;  // HIGH: int32 — branch-1 sub-array count (stride 0x348)
}

// =============================================================================
// Pending-interaction queue infrastructure (ck-pu05)
//
// FUN_062e71b0 reads two effect IDs from an InteractionResult and queues each
// onto the per-character PendingVector at handler+0x28 via FUN_062e8ec0. Used
// only when def+0x2A3B (enable_effects) is set on the interaction definition.
// declare_war and start_murder set enable_effects=0 and bypass this pipeline
// entirely — see re_effect_app_cwar_creation_ck-pu05.md and
// re_war_vs_murder_divergence.md.
//
// The PendingVector this writes to lives at interaction_pending_registry::
// BUCKET_RECORDS_PTR (+0x28) inside each per-character bucket — stride 0x348
// matches the entry size below.
// =============================================================================
namespace pending_interaction {
    // CPendingCharacterInteraction entry (0x348 bytes, vtable 0x0bcb8e10)
    constexpr size_t ENTRY_STRIDE      = 0x348;  // HIGH: matches interaction_pending_registry::BUCKET_RECORDS_PTR stride
    constexpr size_t ENTRY_VTABLE_OFF  = 0x00;   // HIGH: vtable slot
    constexpr size_t ENTRY_GAMESTATE   = 0x08;   // HIGH: u64 game-state ref (also base for date fields written by FUN_076e7460)
    constexpr size_t ENTRY_CTX_OFF     = 0x10;   // HIGH: deep-copied InteractionCtx (~0x338B, via FUN_04428fb0)

    constexpr uintptr_t ENTRY_VTABLE   = 0x0bcb8e10;  // HIGH: file-rel — written at push_back

    // PendingVector header (16B, lives at handler+0x28; mirrors interaction_pending_registry::BUCKET_RECORDS_*)
    constexpr size_t VEC_DATA_OFF      = 0x00;   // HIGH: ptr to entries
    constexpr size_t VEC_CAPACITY_OFF  = 0x08;   // HIGH: i32 capacity
    constexpr size_t VEC_SIZE_OFF      = 0x0C;   // HIGH: i32 size (count of entries)
    constexpr size_t VEC_ALLOCATOR_OFF = 0x10;   // HIGH: ptr to allocator (vtable[2]=alloc, vtable[3]=free)

    // Effect-handler wrapper (0x40 bytes, vtable 0x0bcb8e58 — created by FUN_062e6760
    // on cache miss). +0x28 holds the PendingVector that FUN_062e8ec0 appends to.
    constexpr size_t HANDLER_VTABLE_OFF  = 0x00;   // HIGH
    constexpr size_t HANDLER_ENTITY_ID   = 0x08;   // HIGH: u32 character/entity id (cache key)
    constexpr size_t HANDLER_PENDING_VEC = 0x28;   // HIGH: PendingVector header start
    constexpr uintptr_t HANDLER_VTABLE   = 0x0bcb8e58;  // HIGH: file-rel

    // Capacity-grow factor read by FUN_062e8ec0 — single-precision float
    constexpr uintptr_t GROWTH_FACTOR_F32 = 0x084b10e0;   // HIGH: file-rel address of float
}

namespace interaction_effect_funcs {
    // Two-slot effect-queue dispatcher: reads result+0x2F4 and result+0x300 (target/secondary),
    // each gets queued via PUSH_BACK_PENDING into the lookup_or_create_pending_handler's
    // PendingVector. Direct callers: DirectPath_ScriptRunner (gated by enable_effects),
    // FUN_062e8490 / FUN_062e8610 / FUN_062e7260 (unconditional wrappers).
    constexpr uintptr_t APPLY_EFFECTS_QUEUE        = 0x062e71b0;  // HIGH: 159B (ck-pu05)
    constexpr uintptr_t PUSH_BACK_PENDING          = 0x062e8ec0;  // HIGH: 414B vector push_back, stride 0x348 (ck-pu05)
    constexpr uintptr_t LOOKUP_OR_CREATE_HANDLER   = 0x062e6760;  // HIGH: per-character handler lookup w/ char-DB fallback (ck-pu05)
    constexpr uintptr_t PUSH_BACK_PENDING_THUNK    = 0x062e7250;  // HIGH: 1-line wrapper around PUSH_BACK_PENDING (ck-pu05)
    constexpr uintptr_t INTERACTION_DEEP_COPY      = 0x04428fb0;  // HIGH: copies the 0x338B InteractionCtx (already named in Ghidra)
    constexpr uintptr_t DATE_NORMALIZE             = 0x0406f4e0;  // HIGH: SIMD date normalization, used to date-stamp pending entries
    constexpr uintptr_t DATE_FIELD_WRITER          = 0x076e7460;  // HIGH: writes structured date fields after DATE_NORMALIZE
}

// =============================================================================
// CPendingCharacterInteraction — interaction result object (ck-hm9w)
//
// 0x5C8-byte heap object built by CPendingCharacterInteraction_construct
// (FUN_062e88c0) from a slot pool (CPendingCharacterInteraction_pool_alloc,
// FUN_062eb4e0; stride 0x5C8). Multiple-inheritance derived from CPersistent
// (offset 0) and TPdxReferenceObject<CPendingCharacterInteraction> (offset 8).
// Construction site is DirectPath_ScriptRunner @ 0x062e82f0 — produces the
// "result" the effect-queue pipeline (interaction_effect_funcs above) reads
// from at +0x2F4/+0x300 (== ctx+0x2DC/+0x2E8 inside this object's deep-copied
// InteractionCtx). RTTI-confirmed via typeinfo @ 0x0bcb8e90 (mangled string
// "28CPendingCharacterInteraction" at 0x08b39084).
//
// Note on terminology: the adjacent `pending_interaction` namespace (above)
// describes a 0x348-byte queue entry that the comment misidentifies as
// "CPendingCharacterInteraction" — that struct's actual class is
// CRejectedInteraction (typeinfo at 0x0bcb8f18 → "20CRejectedInteraction").
// The corresponding "EffectHandler wrapper" with vtable 0x0bcb8e58 is
// CPlayerPendingInteractions (typeinfo at 0x0bcb8f00). Layout in that
// namespace remains correct; only the names are wrong. Renaming is left for
// a follow-up audit bead. See re/findings/re_cpending_character_interaction_layout_ck-hm9w.md §7.
// =============================================================================
namespace c_pending_character_interaction {
    // Total size and class identity (RAW addresses per canonical convention; subtract from Ghidra VA via -0x100000)
    constexpr size_t   STRIDE                  = 0x5C8;       // HIGH: pool stride from CPendingCharacterInteraction_pool_alloc
    constexpr uintptr_t VTABLE_PRIMARY         = 0x0bbb8cc0;  // HIGH: vfunc[0] of CPersistent base (Ghidra 0x0bcb8cc0). Vtable struct (offset_to_top=0, typeinfo*) starts 0x10 earlier at raw 0x0bbb8cb0. Aliased to interaction_funcs::PENDING_INTERACTION_VTABLE.
    constexpr uintptr_t VTABLE_PDXREF_THUNK    = 0x0bbb8d08;  // HIGH: vfunc[0] of TPdxReferenceObject base (Ghidra 0x0bcb8d08, offset_to_top=-8)
    constexpr uintptr_t TYPEINFO               = 0x0bbb8e90;  // HIGH: __vmi_class_type_info (2 bases). Aliased to interaction_funcs::PENDING_INTERACTION_TYPEINFO.

    // Field offsets (HIGH for layout — see findings for semantic confidence)
    constexpr size_t VPTR_PERSISTENT           = 0x000;       // HIGH: ptr → vfunc table at VTABLE_PRIMARY
    constexpr size_t VPTR_PDXREF               = 0x008;       // HIGH: ptr → vfunc table at VTABLE_PDXREF_THUNK
    constexpr size_t WEAK_REF_KEY              = 0x010;       // HIGH: u32 — slot_id|generation, written from pool_alloc's 16B return
    constexpr size_t CTX                       = 0x018;       // HIGH: InteractionCtx (~0x338B), deep-copied via INTERACTION_DEEP_COPY
    constexpr size_t SCOPE_REPORT              = 0x350;       // HIGH: ScopeReport (0x268B); init by SCOPE_REPORT_INIT below
    constexpr size_t FIELD_5B8                 = 0x5B8;       // HIGH layout / MEDIUM semantics: u32 — saved tag 0x28AE
    constexpr size_t CALLER_TAG                = 0x5BC;       // HIGH layout / MEDIUM semantics: u32 — saved tag 0x28AF; DirectPath_ScriptRunner passes 0
    constexpr size_t STATUS_CODE               = 0x5C0;       // HIGH: u32 enum 0/1/2/3 (cf. ck-1yq7 V2 status); init = (ctx.secondary_target != -1)
    constexpr size_t FLAG_A                    = 0x5C4;       // HIGH layout / MEDIUM semantics: u8 — saved tag 0x3742; suppressed when =1 (default)
    constexpr size_t FLAG_B                    = 0x5C5;       // HIGH layout / MEDIUM semantics: u8 — saved tag 0x28A4; suppressed when =1 (default)
    constexpr size_t FLAG_C                    = 0x5C6;       // HIGH layout / MEDIUM semantics: bool — saved tag 0x3266; suppressed when =0 (default)

    // Status-code save tag IDs (NOT addresses — token IDs in the save format)
    constexpr uint32_t STATUS_CODE_TAG         = 0x3743;
    constexpr uint32_t STATUS_CODE_KEY_1       = 0x3744;      // value 1 → key 0x3744
    constexpr uint32_t STATUS_CODE_KEY_2       = 0x28B0;      // value 2 → key 0x28B0
    constexpr uint32_t STATUS_CODE_KEY_3       = 0x3544;      // value 3 → key 0x3544
                                                              // value 0 → no tag emitted

    // Functions (RAW addresses; Ghidra VA = raw + 0x100000)
    constexpr uintptr_t CONSTRUCT              = 0x061e88c0;  // HIGH: builds object (Ghidra 0x062e88c0; 224B)
    constexpr uintptr_t POOL_ALLOC             = 0x061eb4e0;  // HIGH: 2-level paged slot pool (Ghidra 0x062eb4e0)
    constexpr uintptr_t DTOR                   = 0x061e9200;  // HIGH: in-place dtor (vfunc[0]; Ghidra 0x062e9200)
    constexpr uintptr_t DELETE                 = 0x061e92b0;  // HIGH: dtor + operator delete (vfunc[1]; Ghidra 0x062e92b0)
    constexpr uintptr_t SAVE                   = 0x061e4a00;  // HIGH: serialize fields by tag (vfunc[3]; Ghidra 0x062e4a00)
    constexpr uintptr_t LOAD                   = 0x061e4ba0;  // HIGH: per-tag deserializer (vfunc[5]; Ghidra 0x062e4ba0)
    constexpr uintptr_t SAVE_WRAPPER           = 0x0818cef0;  // HIGH: shared CPersistent::Save (vfunc[2]; Ghidra 0x0828cef0) — wraps SAVE in begin/end_object
    constexpr uintptr_t LOAD_WRAPPER           = 0x0818cf20;  // HIGH: shared CPersistent::Load (vfunc[4]; Ghidra 0x0828cf20) — token loop dispatching to LOAD
    constexpr uintptr_t POSTLOAD_NOOP          = 0x037e4280;  // HIGH: shared no-op CPersistent::PostLoad (vfunc[6]; Ghidra 0x038e4280)
}

// SubReport / ScopeReport — embedded sub-objects of CPendingCharacterInteraction
namespace c_pending_character_interaction_subreport {
    constexpr size_t SUB_STRIDE               = 0x78;        // HIGH: 3 SSO strings + vector header
    constexpr size_t SUB_S_LABEL              = 0x00;        // HIGH: SSO 32B
    constexpr size_t SUB_S_LEFT               = 0x20;        // HIGH: SSO 32B
    constexpr size_t SUB_S_RIGHT              = 0x40;        // HIGH: SSO 32B
    constexpr size_t SUB_VEC_DATA             = 0x60;        // HIGH: vector<entry> data (entry stride 0x40, see SubReport_destroy)
    constexpr size_t SUB_VEC_CAPACITY         = 0x68;        // HIGH: i32
    constexpr size_t SUB_VEC_SIZE             = 0x6C;        // HIGH: i32
    constexpr size_t SUB_VEC_ALLOCATOR        = 0x70;        // HIGH: ptr — uses vtable at SUB_VEC_ALLOC_VTABLE

    constexpr size_t SCOPE_REPORT_STRIDE      = 0x268;       // HIGH: ScopeReport total
    constexpr size_t SCOPE_SUB_0              = 0x000;       // HIGH: 4 × SubReport
    constexpr size_t SCOPE_SUB_1              = 0x078;
    constexpr size_t SCOPE_SUB_2              = 0x0F0;
    constexpr size_t SCOPE_SUB_3              = 0x168;
    constexpr size_t SCOPE_BACKPTR_SELF       = 0x1E0;       // HIGH: = &this
    constexpr size_t SCOPE_BACKPTR_SUB1       = 0x1E8;       // HIGH: = &sub[1]
    constexpr size_t SCOPE_S_TAIL_0           = 0x1F0;       // HIGH: SSO 32B
    constexpr size_t SCOPE_S_TAIL_1           = 0x210;       // HIGH: SSO 32B
    constexpr size_t SCOPE_S_TAIL_2           = 0x230;       // HIGH: SSO 32B
    constexpr size_t SCOPE_TAIL_LONG          = 0x250;       // HIGH: i64 init -1
    constexpr size_t SCOPE_TAIL_U32           = 0x258;       // HIGH: u32 init 0xFFFFFFFF
    constexpr size_t SCOPE_TAIL_BYTE0         = 0x25C;       // HIGH: u8 init 0
    constexpr size_t SCOPE_TAIL_ZERO          = 0x260;       // HIGH: u32 init 0
    constexpr size_t SCOPE_TAIL_FLAG          = 0x264;       // HIGH: u8 init 1

    constexpr uintptr_t SCOPE_REPORT_INIT     = 0x0609ce20;  // HIGH: ScopeReport_init (Ghidra 0x0619ce20)
    constexpr uintptr_t SUB_REPORT_DESTROY    = 0x04757670;  // HIGH: SubReport_destroy (Ghidra 0x04857670)
    constexpr uintptr_t SUB_VEC_ALLOC_VTABLE  = 0x0bead660;  // HIGH: vtable used at SubReport+0x70 (Ghidra 0x0bfad660; CPdxAllocator-shaped)
}

// =============================================================================
namespace pinned_interactions {
    constexpr size_t ARRAY_OFFSET   = 0xF20;  // HIGH: 16 fixed slots × 0x10 bytes each
    constexpr size_t SLOT_COUNT     = 16;
    constexpr size_t SLOT_STRIDE    = 0x10;
    constexpr size_t SLOT_PTR_OFF   = 0x00;   // HIGH: CCharacterInteractionType* (ck-8cns.1)
    constexpr size_t SLOT_AUX_OFF   = 0x08;   // HIGH: tagged aux ptr, low byte = binding marker

    // Slot → interaction name (hardcoded at DB registration, ck-8cns.1 §Writer #2)
    // Slot 0:  grant_title_to_new_random_character_interaction
    // Slot 1:  hof_redirect_great_holy_war_interaction
    // Slot 2:  select_ghw_beneficiary_interaction
    // Slot 3:  designate_heir_interaction
    // Slot 4:  arrange_marriage_interaction
    // Slot 5:  marry_off_interaction
    // Slot 6:  blackmail_interaction
    // Slot 7:  declare_war_interaction            ← war path is HERE at +0xF90
    // Slot 8:  support_candidacy_interaction
    // Slot 9:  request_ally_title_troops_interaction
    // Slot 10: demand_admin_interaction
    // Slot 11: offer_tributary_status_interaction
    // Slot 12: cease_paying_tribute_interaction
    // Slot 13: request_great_project_contribution_interaction
    // Slot 14: influence_candidacy_interaction    ← NOT declare_war (refutes ck-q402)
    // Slot 15: join_vassal_war_interaction
    constexpr size_t SLOT_DECLARE_WAR_OFFSET = 0xF90;  // HIGH: slot 7
}

constexpr uintptr_t G_NULL_INTERACTION_SENTINEL = 0x0beda700;  // HIGH: Ghidra VA, zero-fill target (ck-8cns.1) (ck-e58l.20)
constexpr uintptr_t INTERACTION_DB_CONSTRUCTOR   = 0x06a3c5f0; // HIGH: FUN_06b3c5f0 — CInteractionDatabase ctor (ck-8cns.1) (ck-e58l.20) (ck-audit-B: renamed from FN_DB_CONSTRUCTOR to match Ghidra symbol InteractionDB_Constructor)
constexpr uintptr_t INTERACTION_SLOT_BINDER      = 0x06a46d50; // HIGH: FUN_06b46d50 — slot binder (ck-8cns.1) (ck-e58l.20) (ck-audit-B: renamed from FN_SLOT_BINDER)
constexpr uintptr_t INTERACTION_SLOT_REGISTRAR   = 0x06a3d100; // HIGH: FUN_06b3d100 — slot registrar (ck-8cns.1) (ck-e58l.20) (ck-audit-B: renamed from FN_SLOT_REGISTRAR)

// =============================================================================
// CCharacterInteractionType — AI Scoring Offsets (ck-zaal)
// All interactions share these offsets. CModValue = base + modifier chain.
// =============================================================================
namespace interaction_scoring {
    // CModValue blocks for AI scoring
    constexpr size_t ITYPE_AI_WILL_DO             = 0x1AD0;  // HIGH: CModValue, initiator AI desire
    constexpr size_t ITYPE_AI_ACCEPT              = 0x1B30;  // HIGH: CModValue, target acceptance
    constexpr size_t ITYPE_AI_ACCEPT_NEGOTIATION  = 0x1B90;  // HIGH: CModValue, recipient negotiation acceptance

    // CModValue internal layout
    constexpr size_t MODVAL_BASE_VALUE            = 0x38;    // HIGH: int64, fixed-point /100000
    constexpr size_t MODVAL_MODIFIER_ARRAY        = 0x40;    // HIGH: ptr to modifier object ptr array
    constexpr size_t MODVAL_MODIFIER_COUNT        = 0x4C;    // HIGH: int32

    // CScriptedValue internal layout (used for factor/add/weight sub-values within modifiers)
    constexpr size_t SV_BASE_VALUE                = 0xA0;    // HIGH: int64, returned when SV_MODIFIER_COUNT=0
    constexpr size_t SV_MODIFIER_COUNT            = 0xC8;    // HIGH: int32, 0=simple value

    // Response text offsets in CCharacterInteractionType
    constexpr size_t ITYPE_ACCEPT_TEXT            = 0x21D0;  // MEDIUM: locstring ptr+len, acceptance text
    constexpr size_t ITYPE_DECLINE_TEXT           = 0x21F0;  // MEDIUM: locstring ptr+len, decline text
    constexpr size_t ITYPE_AUTO_ACCEPT_TEXT       = 0x2210;  // MEDIUM: locstring ptr+len, auto-accept text

    // Control flags (ck-x328: names corrected based on execute handler decompilation)
    constexpr size_t ITYPE_SECONDARY_VALIDATION   = 0x2A32;  // HIGH: byte, secondary validation flag (0 for all tested interactions)
    constexpr size_t ITYPE_SENDER_COST_BYPASS     = 0x2A3F;  // MEDIUM: byte, sender cost bypass (was AUTO_ACCEPT_PRIMARY)
    constexpr size_t ITYPE_RNG_ACCEPT_MODE        = 0x2A40;  // MEDIUM: byte, RNG acceptance mode (was AUTO_ACCEPT_NEGOT)

    // Modifier vtable addresses (Ghidra, for type identification)
    constexpr uintptr_t VT_CTRIGGER_VALUE_MODIFIER  = 0x0bcafee0;  // HIGH: trigger → factor*acc + add
    constexpr uintptr_t VT_COPINION_MTTH_MODIFIER   = 0x0bcafd20;  // HIGH: opinion(A,B) × mult / step
    constexpr uintptr_t VT_CAI_VALUE_MODIFIER        = 0x0bcafe70;  // HIGH: dot(9 AI personality axes, weights)
    constexpr uintptr_t VT_CJOMINI_SCRIPTED_MODIFIER = 0x0bf08578; // HIGH: compound sub-modifier chain
}

// =============================================================================
// CInteractionEvalContext — TLS interaction evaluation context (~0x338 bytes)
// Thread-local at FS_BASE - 0x3a78, guard byte at FS_BASE - 0x6e8
// Used by AI_INTERACTION_CORE, SET_TARGET, SET_SECONDARY_*, BUILD_SEND_CMD
// =============================================================================
namespace interaction_ctx {
    constexpr size_t INTERACTION_TYPE_PTR   = 0x000;  // ptr: CCharacterInteractionType
    constexpr size_t SCOPE_LIST             = 0x008;  // SSavedEventTarget-based scope
    constexpr size_t ACTOR_CHAR_REF         = 0x2D8;  // uint32: actor character packed ref (param_1[0x5b])
    constexpr size_t TARGET_CHAR_REF        = 0x2DC;  // uint32: target/recipient char ref (0xFFFFFFFF = none)
    // NOTE: For declare_war, 0x2DC is OVERLOADED as CB_OPTION_INDEX.
    // InteractionCmd__SelectCBOption(06b3f0f0) writes the CB type identifier here,
    // then ValidateScopes rebuilds cmd+0x330 from the selected option.
    // The target char is set separately via SET_TARGET_AND_SEC_RECIP(06a3f210).
    constexpr size_t CB_OPTION_INDEX        = 0x2DC;  // uint32: CB option index for declare_war (same offset as TARGET_CHAR_REF)
    constexpr size_t SECONDARY_ACTOR_REF    = 0x2E0;  // uint32: WAR_TARGET char ref (scope key "war_target") — for marriage: secondary actor
    constexpr size_t WAR_TARGET_REF         = 0x2E0;  // alias: same offset, used for declare_war (defender char ref)
    constexpr size_t SECONDARY_RECIPIENT_REF = 0x2E4; // uint32: CLAIMANT char ref (scope key "claimant") — for marriage: secondary recipient
    constexpr size_t CLAIMANT_REF           = 0x2E4;  // alias: same offset, used for declare_war (who holds the claim)
    constexpr size_t TERTIARY_REF           = 0x2E8;  // uint32: tertiary character ref
    constexpr size_t EVAL_FLAGS_PTR         = 0x2F0;  // ptr: bool array for target slots
    // Send_option / CB option selection state:
    constexpr size_t OPTION_SELECTION_PTR   = 0x300;  // ptr: byte array (1 byte per option, 0=off 1=on)
    constexpr size_t EVAL_FLAGS_2           = 0x300;  // alias: same as OPTION_SELECTION_PTR (legacy name)
    constexpr size_t OPTION_SELECTION_COUNT = 0x30C;  // int32: number of options in selection array
    constexpr size_t EVAL_SLOT_COUNT        = 0x30C;  // alias: same as OPTION_SELECTION_COUNT (legacy name)
    constexpr size_t OPTION_SELECTION_CAP   = 0x308;  // int32: capacity of selection array (param_1[0x61])
    constexpr size_t EVAL_SLOT_CAPACITY     = 0x310;  // int32: slot capacity (allocator ptr at 0x310)
    constexpr size_t SCRIPTED_DATA_PTR      = 0x330;  // CCasusBelli sub-object for war commands (filled by WAR_ATTACH_STATE + WAR_ATTACH_CB)
    // cmd+0x330 sub-object layout (CCasusBelli-like, 0x30+ bytes):
    //   +0x00: (padding/unused)
    //   +0x08: uint64 cb_type_ref       — CB type pointer (written by WAR_ATTACH_STATE)
    //   +0x10: uint64 title_array_ptr   — dynamic array of uint32 title IDs (written by WAR_ATTACH_CB)
    //   +0x18: int32  title_array_cap   — capacity
    //   +0x1C: int32  title_array_count — count
    //   +0x20: ptr    allocator
    //   +0x28: uint32 claimant_ref      — claimant char ref for claim_cb (written by WAR_ATTACH_CB from source[0])
    // CRITICAL: PopulateOptions/SelectOption do NOT write here. Must call WAR_ATTACH_STATE + WAR_ATTACH_CB explicitly.
}

// =============================================================================
// CCasusBelli Offsets (0x1A0 bytes, embedded in CWar at +0x98)
// Confirmed via FUN_05b5ab70 (CCasusBelli::Set)
// =============================================================================
namespace casus_belli {
    constexpr size_t CB_TYPE_REF        = 0x08;   // HIGH: uint64, CB type reference
    constexpr size_t CB_TARGET          = 0x10;   // HIGH: target (title/character ref)
    constexpr size_t TITLE_ARRAY_PTR    = 0x178;  // HIGH: uint64, title ID array ptr
    constexpr size_t TITLE_ARRAY_CAP    = 0x180;  // HIGH: int32, capacity
    constexpr size_t TITLE_ARRAY_COUNT  = 0x184;  // HIGH: int32, count
    constexpr size_t ALLOCATOR_PTR      = 0x188;  // HIGH: ptr, allocator
    // ck-yifp: MUTABLE, same as the war_obj aliases above — the defender
    // follows the contested title. Never identify a war by this pair.
    constexpr size_t PRIMARY_ATK_REF    = 0x190;  // HIGH: uint32, attacker char ref, MUTABLE (ck-qeod: verified all 11 wars match war side 1)
    constexpr size_t PRIMARY_DEF_REF    = 0x194;  // HIGH: uint32, defender char ref, MUTABLE (ck-qeod: verified all 11 wars match war side 2)
    constexpr size_t CLAIMANT_REF       = 0x198;  // HIGH: uint32, claimant char ref for claim_cb; 0xFFFFFFFF for non-claim CBs (ck-qeod)
    constexpr size_t PARAM_CLAIM_TYPE   = 0x190;  // DEPRECATED: alias, actually PRIMARY_ATK_REF (not "claim type")
    constexpr size_t PARAM_WAR_GOAL     = 0x194;  // DEPRECATED: alias, actually PRIMARY_DEF_REF (not "war goal")
    constexpr size_t PARAM_FLAGS        = 0x198;  // DEPRECATED: alias, use CLAIMANT_REF
    constexpr size_t TOTAL_SIZE         = 0x1A0;  // total CCasusBelli size
}

// =============================================================================
// CCasusBelliType Struct Offsets (R11-25: from CB validity pipeline decompilation)
// Accessed via CASUS_BELLI_DB (+0x88 = array of ptrs, +0x94 = count)
// =============================================================================
namespace cb_type {
    constexpr size_t DB_INDEX           = 0x08;   // HIGH: uint32, database index/ID (R11-25)
    constexpr size_t NAME_STR           = 0x10;   // HIGH: std::string, CB type PDX name (e.g. "claim_cb", "humiliation_cb"). SSO layout: data_ptr / length / capacity / inline (ck-s0qu validated across 22 distinct CB types in 867+1097 dumps).
    constexpr size_t SUB_OBJ_PTR        = 0x30;   // HIGH: ptr to CCasusBelliGroup (see cb_group namespace) — shared across all CBs in same script `group = …` (ck-e58l.3)
    // Group layout now formalized in cb_group namespace; legacy notes (ck-e58l.3 corrections):
    //   group+0x30  = CBType** array of sibling CBs in same group
    //   group+0x3C  = uint32 count
    //   group+0x48  = is_valid trigger (Gate 1 — was previously the *(cb+0x30)+0x48 indirection in ck-5oyp)
    //   group+0x208 = disabled flag (was mislabeled as ai_only; actually DLC/content-gate disable)
    //   group+0x209 = ai_allowed flag (was documented as "another flag" at +0x20a)
    constexpr size_t ALLOWED_FOR_CHAR   = 0x38;   // HIGH: trigger, allowed_for_character (attacker scope) — Gate 2 (ck-5oyp, ck-e58l.3 runtime-verified)
    constexpr size_t VALID_TO_START_ATK = 0x118;  // HIGH: trigger, valid_to_start (attacker scope) — Gate 3, GUI-only (ck-5oyp, ck-e58l.3: corrected from prior mislabel as ALLOWED_AGAINST)
    constexpr size_t ALLOWED_AGAINST    = 0x1F8;  // HIGH: trigger, allowed_against_character (defender scope) — Gate 4 (ck-5oyp, ck-e58l.3: corrected from prior mislabel as VALID_TO_START_ATK)
    constexpr size_t VALID_TO_START_DEF = 0x2D8;  // HIGH: trigger, valid_to_start (defender scope) — Gate 5, GUI-only (ck-5oyp)
    constexpr size_t NO_TARGET_TRIG_1   = 0x3B8;  // HIGH: trigger, no-target CB trigger 1 (when target_mode==0) (R11-25)
    constexpr size_t NO_TARGET_TRIG_2   = 0x498;  // HIGH: trigger, no-target CB trigger 2 (conditional) (R11-25)
    constexpr size_t CAN_USE_TRIGGER    = 0x578;  // HIGH: trigger, can_use (claim validation for title-targeted CBs) (Phase 9)
    constexpr size_t IS_VASSAL_TRIGGER  = 0x658;  // HIGH: trigger, is_vassal_of_attacker check (vassal-only CB gate) (Phase 9)
    constexpr size_t VASSAL_CB_FLAG     = 0x6B4;  // HIGH: int32, 0=check FLAGS_1700 bit 6, !=0=evaluate IS_VASSAL_TRIGGER (Phase 9)
    constexpr size_t TARGET_MODE        = 0x738;  // HIGH: int32, CB target mode (0=none, 6=claim, other=title) (R11-25)
    constexpr size_t TIER_CAP           = 0x73C;  // HIGH: byte, max title tier for title-targeted CBs (0=any,1=barony..6=empire) (Phase 9)
    constexpr size_t TIER_CAP_FLAG      = 0x73D;  // HIGH: byte, enables per-tier filtering when !=0 (Phase 9)
    constexpr size_t ENABLED_FLAG       = 0x73E;  // HIGH: byte, CB enabled (!=0) (R11-25)
    constexpr size_t VTS_CONDITION_ARRAY = 0x820; // HIGH: valid-to-start condition array (R11-25)
    constexpr size_t VTS_CHECK_COUNT    = 0x87C;  // HIGH: int32, valid-to-start check count (R11-25)
    constexpr size_t FLAGS_1700         = 0x1700; // HIGH: byte, flags (bit 6 = vassal-only when VASSAL_CB_FLAG==0) (Phase 9)
    // Note: `*(int16_t*)(cb+0x1700) < 0` is read by WAR_CREATE_CORE Block L to gate
    // ally-enlistment. The sign bit corresponds to bit 7 of FLAGS_1701 (high byte of i16).
    // Semantic name for that bit pending further RE (ck-e58l.5).
    constexpr size_t FLAGS_1702         = 0x1702; // HIGH: byte, flags (bit 4 = title_required) (R11-25)
    constexpr size_t FLAGS_1703         = 0x1703; // HIGH: byte, flags (bit 0 = AI-allowed, checked only under CB_CASCADE mode=2) (ck-e58l.3)

    // Compiled Clausewitz effect block for the CB's `on_declaration = { ... }`.
    // Fires inside WAR_CREATE_CORE after participants+CB+name, BEFORE final validity
    // check, via CLAUSEWITZ_EFFECT_EXECUTOR(cb_type + 0x900, scope). Scope is built
    // with the new CWar as root. For declare_war CBs, this block contains
    // `on_declared_war = yes` (invokes the user scripted_effect) (ck-e58l.5).
    constexpr size_t ON_DECLARATION_EFFECT = 0x900;  // HIGH: effect-tree header (ck-e58l.5)

    // CB target mode enum values
    constexpr int TARGET_NONE           = 0;      // no title target (subjugation, forced_vassalization)
    constexpr int TARGET_DE_JURE_DUCHY  = 1;      // duchy-level de jure territory
    constexpr int TARGET_COUNTY_DIRECT  = 2;      // county direct territory
    constexpr int TARGET_DE_JURE_KINGDOM = 3;     // kingdom-level de jure territory
    constexpr int TARGET_DUCHY_DIRECT   = 4;      // duchy direct territory
    constexpr int TARGET_HIGHER_TIER    = 5;      // higher-tier titles
    constexpr int TARGET_CLAIM          = 6;      // claim-based (push claim)
    constexpr int TARGET_DE_JURE_VASSAL = 7;      // de jure vassal titles of defender + higher-tier

    // PopulateCandidateTitles mode mapping (param_1 of func at 06dd0210):
    //   target_mode 1,3 -> PopulateCandidateTitles mode 0 or 2 (de jure)
    //   target_mode 2,4 -> PopulateCandidateTitles mode 1 or 3 (direct territory)
    //   target_mode 5,7 -> higher tier titles + optional de jure vassal titles
}

// =============================================================================
// CCasusBelliGroup Struct Offsets (ck-e58l.3)
// Accessed via CB_GROUP_DB (+0x88 = group array, +0x94 = count).
// Referenced from CCasusBelliType: *(cb + 0x30) = CBGroup*.
// Iterated by CB_CASCADE (0x065d6b20, has_any_display_cb_on path).
// =============================================================================
namespace cb_group {
    constexpr size_t MEMBERS_ARR   = 0x30;   // HIGH: CBType** array of member CBs (runtime-verified)
    constexpr size_t MEMBERS_COUNT = 0x3C;   // HIGH: uint32 count of member CBs
    constexpr size_t IS_VALID      = 0x48;   // HIGH: inline trigger — group-level is_valid (Gate 1)
                                              // Resolves the *(cb+0x30)+0x48 indirection documented in ck-5oyp.
    constexpr size_t DISABLED_FLAG = 0x208;  // HIGH: byte — set=group disabled (DLC/content gate)
    constexpr size_t AI_ALLOWED    = 0x209;  // MEDIUM: byte — AI-filter bit (checked only under mode=2)
    constexpr size_t STAGE2_FLAG   = 0x20A;  // MEDIUM: byte — second outer-trigger gate paired with DISABLED_FLAG
                                              // in AI_CB_EVALUATE_ALL Stage 2 (line 359 of decomp:
                                              // *(char *)(group + 0x20a) == '\0'). Distinct from AI_ALLOWED;
                                              // exact semantics deferred to follow-up RE (ck-39e1 Stage 1+2 mirror).
}

// =============================================================================
// CB Option System — TWO-TIER ARCHITECTURE (Phase 8 RE)
// =============================================================================
//
// TIER 1: Interaction Option Entries (at interaction_def+0x2530)
//   Count: interaction_def+0x253C
//   Stride: 0x7D0 (2000 bytes) per option
//   Used by: PopulateOptions, SelectOption, CDeclareWarWindow
//   Layout:
//     +0x000: base CB data (CCasusBelliType-related, identifies the CB type)
//     +0x0E0: trigger block (validated via TriggerBlock__Evaluate at 0765a030)
//   Selection state: cmd+0x300 is a byte array (1 byte per option, 0/1)
//   For declare_war, each option = one Casus Belli type.
//   The cb_type_obj+0x18 value from the option is written to cmd+0x2DC
//   as the CB option index.
//
// TIER 2: AI War Candidate Entries (built by AI__PopulateWarCandidates)
//   Stride: 0x908 (2312 bytes) per candidate
//   Used by: AI__SelectWarTargetAndCB
//   Layout:
//     +0x000: 16 bytes (4x uint32) — candidate header
//     +0x010: uint64 cb_type_ptr (CCasusBelliType*)
//     +0x018: CGameIdRef — game ID reference
//     +0x020: CDynArray<uint32> title_array_1 — primary war titles
//     +0x068: CDynArray<uint32> title_array_2 — secondary war titles
//     +0x0B0: int32 score — AI utility score
//     +0x0B4: byte flag — conquest/forced flag
//     +0x0B8: CDynArray<string> string_array_1
//     +0x4E0: CDynArray<string> string_array_2
//
// CB OPTION SUB-ENTRIES (from CCasusBelliType__GetValidCBOptions):
//   Stride: 0x98 (152 bytes) per sub-option
//   Layout:
//     +0x00: uint32 option_value (claimant char ref for claim_cb; option index for others)
//     +0x08: CDynArray<uint32> title_array_1 — primary titles for this option
//     +0x50: CDynArray<uint32> title_array_2 — secondary titles
//
// WAR DISPATCH FLOW (how the AI selects and executes a war):
//   1. AI__PopulateWarCandidates builds candidate list
//   2. AI__SelectWarTargetAndCB picks best candidate by score
//   3. CInteractionDef__BuildSendInteractionCmd creates base cmd
//   4. InteractionCmd__SelectCBOption writes cb_option to cmd+0x2DC
//   5. ValidateScopes processes the selected option
//   6. WarCmd__AttachCBState writes cb_type_ref to cmd+0x338
//   7. WarCmd__AttachCBTitle deep-copies title array to cmd+0x340
//   8. CInteractionCtx__FullValidation validates the entire command
//   9. Command dispatched to execute the war declaration
//
// FOR BRIDGE: To declare war programmatically:
//   a) Build interaction cmd via BuildSendInteractionCmd (declare_war)
//   b) Set target via SET_TARGET_AND_SEC_RECIP
//   c) Write CB option index to cmd+0x2DC (or call SelectCBOption)
//   d) Call WAR_ATTACH_STATE(cmd+0x330, cb_type_ref)
//   e) Call WAR_ATTACH_CB(cmd+0x330, title_data_source)
//   f) Finalize and send
// =============================================================================

// CLaw Entry Offsets — DEPRECATED: use realm_law_entry namespace above.
// +0x30 is CLawGroup* pointer, not int32 group ID. See realm_law_entry::LAW_GROUP_PTR.

// =============================================================================
// CFaction Struct Updates (Round 5f confirmed)
// =============================================================================
namespace faction_v2 {
    // R17+R18 confirmed CFaction struct layout (~0xC8 bytes)
    constexpr size_t FACTION_ID         = 0x10;   // HIGH: uint32, faction ID (RefDB key)
    constexpr size_t TYPE_PTR           = 0x18;   // HIGH: ptr, CFactionType*
    constexpr size_t DISCONTENT         = 0x20;   // HIGH: int64, fixed-point / 100,000 (current discontent)
    constexpr size_t POWER_RATIO        = 0x28;   // HIGH: int64, fp/100k, faction_power/(liege-faction)*100 (from CalcPowerRatio)
    constexpr size_t SCRIPT_VALUE_F38   = 0x30;   // HIGH: int64, fp/100k, eval of CFactionType+0xF38
    constexpr size_t TARGET_CHAR_REF    = 0x38;   // HIGH: uint32, target character (liege) ref
    constexpr size_t LEADER_CHAR_REF    = 0x3C;   // HIGH: uint32, faction leader ref (0xFFFFFFFF = none)
    constexpr size_t CHAR_MEMBERS_PTR   = 0x40;   // HIGH: ptr, CPdxArray<SFactionCharMember> data
    constexpr size_t CHAR_MEMBERS_COUNT = 0x4C;   // HIGH: int32, char member count (stride=0x20)
    constexpr size_t COUNTY_MEMBERS_PTR = 0x58;   // HIGH: ptr, CPdxArray<SFactionCountyMember> data
    constexpr size_t COUNTY_MEMBERS_COUNT = 0x64; // HIGH: int32, county member count (stride=0x18)
    constexpr size_t TICK_SLOT          = 0x70;   // HIGH: int32, monthly tick slot index (0-29)
    constexpr size_t COOLDOWN_DATE      = 0x74;   // HIGH: int64, cooldown after demand press (game date)
    constexpr size_t PREV_ULT_DATE      = 0x7C;   // MEDIUM: int64, previous ultimatum date (copied to +0xB0)
    constexpr size_t WAR_REF            = 0x84;   // HIGH: uint32, linked war ref (CWar DB, 0xFFFFFFFF = none)
    constexpr size_t BASE_DISCONTENT    = 0x90;   // HIGH: int64, fp/100k, clamped discontent [0, threshold]
    constexpr size_t POWER_RATIO_CACHED = 0x98;   // HIGH: int64, fp/100k, CalcPowerRatio result (set in tick)
    constexpr size_t SCRIPT_VALUE_A0    = 0xA0;   // HIGH: int64, fp/100k, eval of CFactionType+0xF38 (set in tick)
    constexpr size_t ULTIMATUM_PROGRESS = 0xA8;   // HIGH: int64, fp/100k, 0=none, 10000000=ready to fire (100.0)
    constexpr size_t ULTIMATUM_FIRE_DATE = 0xB0;  // HIGH: int64, game date when ultimatum fires (sentinel=no ultimatum)
    constexpr size_t WAR_INACTIVE_FLAG  = 0xB8;   // HIGH: bool, linked war is inactive/ended
    constexpr size_t SHOULD_PRESS_DEMAND = 0xB9;  // MEDIUM: bool, faction should auto-press demand
    constexpr size_t DISABLED_FLAG      = 0xC0;   // HIGH: bool, skip tick processing if set
    constexpr size_t AT_WAR_FLAG        = 0xC1;   // HIGH: bool, faction currently at war

    // SFactionCharMember offsets (stride = 0x20)
    namespace char_member {
        constexpr size_t CHAR_REF       = 0x08;   // HIGH: uint32, member character ref
        constexpr size_t FACTION_ID     = 0x0C;   // HIGH: uint32, owning faction ID
        constexpr size_t IS_INVALIDATED = 0x10;   // HIGH: bool, skip if true
        constexpr size_t STRIDE         = 0x20;   // 32 bytes per member
    }

    // SFactionCountyMember offsets (stride = 0x18)
    namespace county_member {
        constexpr size_t TITLE_REF      = 0x08;   // HIGH: uint32, county title ref
        constexpr size_t IS_INVALIDATED = 0x0C;   // HIGH: bool, skip if true
        constexpr size_t STRIDE         = 0x18;   // 24 bytes per member
    }

    // CFactionType script block offsets (relative to faction_type_ptr)
    namespace type_scripts {
        constexpr size_t ULTIMATUM_PROGRESS_SCRIPT = 0xD70; // HIGH: CModValue, ultimatum progress rate
        constexpr size_t DISCONTENT_PER_TICK       = 0xDD0; // HIGH: CModValue, discontent change per tick
        constexpr size_t COUNTY_MILITARY_SCRIPT    = 0xE30; // HIGH: CModValue, county military contribution
        constexpr size_t CACHED_SCRIPT_VALUE       = 0xF38; // MEDIUM: CModValue, stored at faction+0xA0
        constexpr size_t IS_COUNTY_FACTION         = 0xFEE; // HIGH: bool, county-based faction
        // re_faction_join_create.md — write side
        constexpr size_t CREATABLE_FLAG            = 0xFEB; // HIGH: bool, ruler-creatable (gated in FACTION_CAN_CREATE)
        constexpr size_t CREATE_TRIGGER_BLOCK      = 0x790; // HIGH: can_create script trigger block (eval'd by FACTION_CAN_CREATE)
    }
}

// =============================================================================
// Faction WRITE side — create / join (re_faction_join_create.md, 2026-06-18)
// Primary use: a vassal joins/creates an independence_faction vs its liege.
// Recipe: mgr = GET_FACTION_MANAGER(*GAME_STATE_ROOT);
//         f   = GET_OR_CREATE_AND_JOIN(mgr, type_ptr, actor_char, liege_char);
// type_ptr resolved by name from FACTION_TYPE_DB_V2 (name at type_ptr+0x10).
// Failure → returns NULL_FACTION_SENTINEL without mutating (safe + detectable).
// All offsets base-relative (VA = raw + 0x100000). Confidence per comment.
// =============================================================================
namespace faction_write {
    // Create-or-join in one call: allocs+inits a CFaction if none of `type`
    // targets `liege`, else just adds actor via JoinFaction. Returns CFaction*
    // (or NULL_FACTION_SENTINEL on rejection). Models CCreateFactionCommand.
    constexpr uintptr_t GET_OR_CREATE_AND_JOIN  = 0x05e9c920; // HIGH: CFaction*(FactionMgr*, CFactionType*, CChar* actor, CChar* liege) (FUN_05f9c920)
    constexpr uintptr_t GET_FACTION_MANAGER     = 0x05f60240; // HIGH: FactionMgr*(GameState* gsr) → *(gsr+0xa0)+0x17918 (FUN_06060240)
    constexpr uintptr_t FACTION_CAN_CREATE      = 0x06dfed90; // HIGH: bool(CFactionType*, CChar* liege, CChar* actor, void* reason_out_or_0) (FUN_06efed90)
    constexpr uintptr_t FACTION_CAN_JOIN        = 0x05ea3640; // HIGH: bool(CChar* actor, CFaction*, void* ctx, void* reason_out_or_0) (FUN_05fa3640)
    constexpr uintptr_t FACTION_IS_MEMBER       = 0x05e8c4d0; // HIGH: member*(CFaction*, CChar*) — walks +0x40/+0x4c member array (FUN_05f8c4d0)
    constexpr uintptr_t FACTION_ALLOC           = 0x05e9e690; // MEDIUM: {CFaction*,tickslot}(mgr+0x20+8) — empty alloc (FUN_05f9e690)
    constexpr uintptr_t FACTION_INIT            = 0x05e8a480; // MEDIUM: void(CFaction*, CFactionType*, u32 target_ref, int slot) (FUN_05f8a480)

    // GameState → FactionManager chase (core-dump validated on william_1066)
    constexpr size_t    MGR_FROM_GS_OFF1        = 0xa0;       // HIGH: GameState +0xa0 → sub-struct ptr
    constexpr size_t    MGR_FROM_GS_OFF2        = 0x17918;    // HIGH: +0x17918 → FactionManager

    // Null-CFaction sentinel returned by faction lookups / failed create
    constexpr uintptr_t NULL_FACTION_SENTINEL   = 0x0bf087d0; // HIGH: _DAT_0c0087d0 - 0x100000

    // JoinFaction (= faction_funcs::JOIN_FACTION 0x05e8db50) clarified signature:
    //   void Faction_AddMember(CFaction* f, u32 char_ref, int title_or_neg1,
    //                          void* claimant_data, char skip_validate)
    // skip_validate=1 forces (CJoinFactionForcedEffect); =0 runs FACTION_CAN_JOIN.
    // title_or_neg1 != -1 → claimant_data ({title_idx, gs_title_ptr}) copied to member+0x14.

    // LEAVE: CLeaveFactionCommand/CLeaveFactionEffect exist (RTTI) but the
    // function address is NOT yet pinned — see re_faction_join_create.md §6.
}

// Faction power/ultimatum functions (ck-bf34)
namespace faction_funcs {
    constexpr uintptr_t CALC_POWER_RATIO     = 0x05e8ae80;  // HIGH: int64(faction) → fp/100k power ratio (ck-e58l.20)
    constexpr uintptr_t CALC_POWER_BALANCE   = 0x05e88a50;  // HIGH: int64(faction) → liege_mil - faction_mil (ck-e58l.20)
    constexpr uintptr_t GET_MILITARY_POWER   = 0x05e8b840;  // HIGH: int64(faction) → total faction military (ck-e58l.20)
    constexpr uintptr_t GET_DISCONTENT       = 0x05e8b540;  // HIGH: int64(faction) → *(faction+0x20) (ck-e58l.20)
    constexpr uintptr_t ADD_DISCONTENT       = 0x05e8d710;  // HIGH: void(faction, delta) → clamps ≥ 0 (ck-e58l.20)
    constexpr uintptr_t CALC_DISCONTENT_DELTA = 0x05e8b3d0; // HIGH: int64(faction) → script eval per tick (ck-e58l.20)
    constexpr uintptr_t FACTION_TICK         = 0x05e8c910;  // HIGH: void(faction) → monthly update + ultimatum check (ck-e58l.20)
    constexpr uintptr_t CAN_FIRE_ULTIMATUM   = 0x05e8ca90;  // HIGH: bool(faction, debug_out) (ck-e58l.20)
    constexpr uintptr_t CALC_ULT_PROGRESS    = 0x05e8bc50;  // HIGH: int64(faction) → ultimatum countdown progress (ck-e58l.20)
    constexpr uintptr_t UPDATE_LEADER        = 0x05e8b920;  // HIGH: void(faction) → re-elect leader (ck-e58l.20)
    constexpr uintptr_t JOIN_FACTION         = 0x05e8db50;  // HIGH: void(faction, char_ref, ...) → add member (ck-e58l.20)
    constexpr uintptr_t FACTION_MGR_TICK     = 0x05e9b0d0;  // HIGH: void(mgr) → monthly slot iteration (ck-e58l.20)
    constexpr uintptr_t TRIGGER_FACTION_POWER = 0x045b85b0; // HIGH: float(faction_ref) → faction/liege (0.0-1.0) (ck-e58l.20)
    constexpr uintptr_t TRIGGER_FACTION_RATIO = 0x045b86e0; // HIGH: int64(faction_ref) → faction/balance fp/100k (ck-e58l.20)
    constexpr uintptr_t GET_REALM_MILITARY   = 0x0664bed0;  // HIGH: int32(char) → *(char->realm+0x2F4) (ck-e58l.20)
    constexpr uintptr_t GET_REALM_MAX_LEVIES = 0x06673840;  // HIGH: int32(char) → *(char->realm+0x2FC) (ck-e58l.20)
}

// Faction global constants
namespace faction_globals {
    constexpr uintptr_t ULTIMATUM_DELAY_DAYS = 0x0c2aca08;  // HIGH: int32=90, days before ultimatum fires
    constexpr uintptr_t SENTINEL_DATE        = 0x0c293778;  // HIGH: int64, "never" date sentinel for cooldown/ultimatum
}

// Decision Hash Table (at *(DECISION_DB + 0x1b0)) -- same layout as interaction HT
namespace decision_ht {
    constexpr size_t TABLE_OFFSET       = 0x1b0;
    constexpr size_t CAPACITY_OFFSET    = 0x1bc;
    constexpr size_t LOCK_FLAG_OFFSET   = 0xf10;
    constexpr size_t LOCK_OFFSET        = 0xed0;
    constexpr size_t SENTINEL_OFFSET    = 0x1c0;
    constexpr size_t ENTRY_SIZE         = 0x18;
}

// =============================================================================
// CDecisionType Trigger Block Offsets (ck-mol-09pam, expanded ck-39tc)
// All offsets relative to CDecisionType object base
// Trigger blocks are CAndTrigger (RTTI: 11CAndTrigger, vtable: 0xbe147b0)
// Cost blocks are CScriptedCost (RTTI: 13CScriptedCost, vtable: 0xbd2ca48)
// =============================================================================
namespace decision_type {
    constexpr size_t NAME              = 0x010;   // HIGH: std::string (db_object universal)
    constexpr size_t COOLDOWN_PTR      = 0x1b8;   // HIGH: ptr, if non-zero → FUN_068ba0d0 cooldown check
    constexpr size_t IS_SHOWN          = 0x688;   // HIGH: CAndTrigger (0xE0B), is_shown/is_available
    constexpr size_t COST_TRIGGER      = 0x768;   // HIGH: CAndTrigger (0xE0B), cost prerequisites
    constexpr size_t IS_VALID          = 0x848;   // HIGH: CAndTrigger (0xE0B), is_valid prerequisites
    constexpr size_t AI_POTENTIAL      = 0xa08;   // HIGH: CAndTrigger (0xE0B), ai_potential
    constexpr size_t AI_WILL_DO        = 0xae8;   // HIGH: CModValue, ai_will_do (fixed-point /100000)
    constexpr size_t CAN_TAKE_FALLBACK = 0xb78;   // HIGH: CScriptedCost (0xE0B), can-take fallback
    constexpr size_t CAN_TAKE_PRIMARY  = 0x1628;  // HIGH: CScriptedCost (0xA80B), can-take primary
    constexpr size_t CUSTOM_VALIDATOR  = 0x20d8;  // MEDIUM: ptr, additional validator callback
}

// CAndTrigger internal structure (RTTI: 11CAndTrigger)
// Boolean trigger tree node: children evaluated as short-circuit AND
// COrTrigger/CNotTrigger share same layout with different vtables
namespace and_trigger {
    constexpr size_t SCOPE_CTX_PTR     = 0x10;   // HIGH: ptr, shared scope context
    constexpr size_t SCOPE_TYPE        = 0x18;   // HIGH: uint32, scope type ID
    constexpr size_t EVAL_CTX          = 0x20;   // MEDIUM: uint64, evaluation context
    constexpr size_t SCOPE_DATA_PTR    = 0x28;   // HIGH: ptr, shared scope data
    constexpr size_t CHILDREN_DATA_PTR = 0x50;   // HIGH: ptr → inline buffer at +0x70 or heap
    constexpr size_t CHILDREN_CAPACITY = 0x54;   // HIGH: uint32, vector capacity (default 4)
    constexpr size_t CHILDREN_COUNT    = 0x5C;   // HIGH: uint32, number of child CJominiTrigger*
    constexpr size_t CHILDREN_INLINE   = 0x70;   // HIGH: CJominiTrigger*[4], inline buffer (SSO)
    constexpr size_t TOTAL_SIZE        = 0xE0;   // HIGH: total embedded size
}

// CScriptedCost internal structure (RTTI: 13CScriptedCost)
// 10-slot math expression evaluator for resource costs
// Children are CJominiSimpleScriptMath / CJominiConditionalScriptMath
namespace scripted_cost {
    constexpr size_t SLOT_BASE         = 0x30;   // HIGH: first slot offset from object base
    constexpr size_t SLOT_STRIDE       = 0x108;  // HIGH: bytes per slot
    constexpr size_t NUM_SLOTS         = 10;     // HIGH: always 10 slots
    constexpr size_t SLOT_COUNT        = 0xC8;   // HIGH: uint32, entry count within slot
    constexpr size_t SLOT_DEFAULT      = 0xA0;   // HIGH: int64, default return value
    constexpr size_t SLOT_SUB_TRIGGER  = 0xB0;   // HIGH: ptr, single sub-trigger
    constexpr size_t SLOT_TRIGGER_LIST = 0xC0;   // HIGH: ptr, trigger list object
    constexpr size_t MULTIPLY_FLAG     = 0xAA8;  // HIGH: byte, apply fixed-point rounding
    constexpr size_t CLAMP_FLAG        = 0xAA9;  // HIGH: byte, clamp negatives to 0
    constexpr size_t TOTAL_SIZE        = 0xA80;  // HIGH: total 10-slot block size
}

// CJominiTrigger vtable method offsets (all trigger node types)
namespace trigger_vtable {
    constexpr size_t EVALUATE          = 0x28;   // HIGH: bool(this, scope_ctx) — leaf evaluation
    constexpr size_t GET_SCOPE_REQS    = 0x38;   // HIGH: bitfield(this) — scope requirements
    constexpr size_t EVAL_ACCUMULATE   = 0x40;   // HIGH: void(this, scope_ctx, ...) — list context
    constexpr size_t GET_SCOPE_ID      = 0x60;   // MEDIUM: uint16(this) — current scope ID
    constexpr size_t GET_SCOPE_BITS    = 0x68;   // MEDIUM: bitfield_ptr(this)
    constexpr size_t TYPE_EVALUATE     = 0xd0;   // MEDIUM: uint(this, scope_ctx) — type-specific
}

// Decision prerequisite validation functions
namespace decision_funcs {
    constexpr uintptr_t CHECK_AI_POTENTIAL = 0x06cee2e0;  // HIGH: bool(decision, scopes) (ck-e58l.20)
    constexpr uintptr_t CHECK_IS_SHOWN    = 0x06cedc60;  // HIGH: bool(decision, scopes) (ck-e58l.20)
    constexpr uintptr_t CHECK_IS_VALID    = 0x06ceddf0;  // HIGH: bool(decision, char, scopes, ?, failures) (ck-e58l.20)
    constexpr uintptr_t CHECK_COST        = 0x06cee080;  // HIGH: bool(decision, scopes, ?, failures) (ck-e58l.20)
    constexpr uintptr_t GET_AI_WILL_DO    = 0x06cee2f0;  // HIGH: int(decision, scopes) → score/100000 (ck-e58l.20)
    constexpr uintptr_t CHECK_CAN_TAKE    = 0x04cd2ca0;  // HIGH: void(char, decision, result, scopes) (ck-e58l.20)
    constexpr uintptr_t BUILD_SCOPES      = 0x06ce9d90;  // HIGH: void(scope_ctx, character) (ck-e58l.20)
}

// Universal trigger evaluation functions
namespace trigger_eval_funcs {
    constexpr uintptr_t SCRIPT_TRIGGER_EVALUATOR = 0x07528500;  // HIGH: bool(trigger_block, scope_ctx) — Ghidra: ScriptTriggerEvaluator (ck-e58l.20) (ck-audit-B: renamed from EVALUATE)
    constexpr uintptr_t EVAL_WITH_DIAG     = 0x07528bc0;  // HIGH: uint32(trigger, scope_obj, diag_obj) — eval with diagnostic target (ck-vqth) (ck-e58l.20)
    constexpr uintptr_t EVAL_WITH_FAILURES = 0x0753ddc0;  // HIGH: void(result**, trigger_block, scopes) (ck-e58l.20)
    constexpr uintptr_t FORMAT_FAILURES    = 0x0753dfe0;  // MEDIUM: void(result**, loc_key, failures) (ck-e58l.20)
    constexpr uintptr_t INIT_DIAG_OBJ     = 0x07538fc0;  // HIGH: void(diag_obj) — init 0xD8-byte diagnostic object (ck-vqth) (ck-e58l.20)
    constexpr uintptr_t RECORD_FAILURE     = 0x07539310;  // HIGH: void(diag_obj, trigger, scope) — record failure + clear fresh flag (ck-vqth) (ck-e58l.20)
    constexpr uintptr_t INIT_EVAL_CTX     = 0x0744fe70;  // HIGH: void(eval_ctx*, scope_obj) — init two typed arrays + scope ref (ck-vqth) (ck-e58l.20)
    constexpr uintptr_t BUILD_TYPED_SCOPE = 0x07451620;  // HIGH: scope*(buf, scope_obj, eval_ctx*) — 40-byte typed scope wrapper (ck-vqth) (ck-e58l.20)
    constexpr uintptr_t IS_EMPTY           = 0x06a8f650;  // HIGH: bool(scripted_cost) — all sub-counts=0 (ck-e58l.20)
    constexpr uintptr_t DIAG_OBJECT_DTOR  = 0x04a5b060;  // HIGH: void(diag_obj) — destroy 0xD8 diagnostic object allocated by EVAL_WITH_FAILURES (FUN_05a5b060, image 0x5a5b060; header = image-0x100000) (ck-wtk7) (item81: corrected from 0x0495b060 which resolved mid-FUN_0595af50+0x110)
}

// Diagnostic record (0xD8-byte) field layout, populated by EVAL_WITH_FAILURES
// (FUN_0763ddc0). std::strings at +0x18/+0x38 carry localization keys when the
// loud evaluator records a failure. See re_gate4_rejection_keys.md §4.
namespace diag_record {
    constexpr size_t STR1_DATA       = 0x18;  // MEDIUM (ck-wtk7): std::string #1 data ptr (libstdc++ dual-abi)
    constexpr size_t STR1_SIZE       = 0x20;
    constexpr size_t STR2_DATA       = 0x38;  // MEDIUM (ck-wtk7): std::string #2 data ptr
    constexpr size_t STR2_SIZE       = 0x40;
    constexpr size_t POPULATED_FLAG  = 0xD0;  // u8; 0=populated, 1=empty/reset
    constexpr size_t TOTAL_SIZE      = 0xD8;
}

// CAndTrigger / CJominiTrigger struct layout (~0xE0 bytes per slot)
// Confirmed via core dump RTTI-guided traversal (ck-ysls, ck-vqth)
namespace trigger_struct {
    constexpr size_t VTABLE            = 0x00;   // HIGH: vtable ptr (RTTI: 11CAndTrigger, etc.)
    constexpr size_t SOURCE_FILE       = 0x10;   // HIGH: char*, debug source file path
    constexpr size_t LABEL             = 0x28;   // HIGH: char*, debug trigger label (e.g., "declare_war_interaction:is_shown")
    constexpr size_t TRIGGER_ID        = 0x40;   // MEDIUM: uint64, unique trigger identifier
    constexpr size_t CHILDREN_DATA     = 0x50;   // HIGH: ptr, unique_ptr<CJominiTrigger>[] array data
    constexpr size_t CHILDREN_PACKED   = 0x58;   // HIGH: uint64, packed [count:upper32 | capacity:lower32]
    constexpr size_t ALLOCATOR_VT      = 0x68;   // HIGH: vtable*, CPdxHybridInlineBufferAllocator
    constexpr size_t RAW_PTR_DATA      = 0x98;   // HIGH: ptr, raw pointer array data
    constexpr size_t RAW_PTR_PACKED    = 0xA0;   // HIGH: uint64, packed [count:upper32 | capacity:lower32]
    constexpr size_t TOTAL_SIZE        = 0xE0;   // HIGH: total bytes per trigger slot
}

// Diagnostic object layout (0xD8 bytes, allocated by EVAL_WITH_FAILURES)
// Fresh flag at +0xD0: 1=freshly allocated, 0=failure recorded
namespace trigger_diag {
    constexpr size_t VTABLE            = 0x00;   // HIGH: vtable ptr (0x0bf14e30)
    constexpr size_t REGISTRY_REF      = 0x08;   // HIGH: ptr to static singleton (0x0c670be8)
    constexpr size_t COUNTER           = 0x10;   // MEDIUM: counter/flags
    constexpr size_t STRING1_LEN       = 0x20;   // HIGH: length of string 1 (trigger name/label)
    constexpr size_t STRING1_BUF       = 0x28;   // HIGH: inline SSO string 1
    constexpr size_t STRING2_LEN       = 0x40;   // HIGH: length of string 2 (failure description)
    constexpr size_t STRING2_BUF       = 0x48;   // HIGH: inline SSO string 2
    constexpr size_t FRESH_FLAG        = 0xD0;   // HIGH: byte, 1=fresh 0=recorded
    constexpr size_t TOTAL_SIZE        = 0xD8;   // HIGH: total diagnostic object size
}

// Trigger tree evaluation pipeline (ck-39tc)
namespace trigger_pipeline {
    constexpr uintptr_t CORE_EVAL          = 0x07528790;  // HIGH: scope check + vtable[0xd0] dispatch (ck-e58l.20)
    constexpr uintptr_t ENTRY_EVAL_CHAIN   = 0x07461f50;  // HIGH: AND short-circuit over entry array (ck-e58l.20)
    constexpr uintptr_t SLOT_EVALUATE      = 0x03812c80;  // HIGH: per-slot 3-mode dispatch (ck-e58l.20)
    constexpr uintptr_t LIST_ITERATE       = 0x03812f00;  // HIGH: iterate list, vtable[8] per entry (ck-e58l.20)
    constexpr uintptr_t COST_EVAL_ALL      = 0x06a91a10;  // HIGH: eval all 10 CScriptedCost slots (ck-e58l.20)
    constexpr uintptr_t COST_POSTPROCESS   = 0x06a91630;  // HIGH: clamp negatives + fixed-point (ck-e58l.20)
    constexpr uintptr_t COST_TO_FIXEDPOINT = 0x06a918b0;  // HIGH: multiply each slot by 100000 (ck-e58l.20)
    constexpr uintptr_t CHECK_COOLDOWN     = 0x067ba0d0;  // MEDIUM: decision cooldown/domain check (ck-e58l.20)
}

// =============================================================================
// Event System Offsets
// =============================================================================
namespace event_sys {
    // Console command handlers (confirmed from console_commands.csv)
    constexpr uintptr_t CMD_EVENT_QUEUE       = 0x040f8200;  // "event_queue" → print queue debug
    constexpr uintptr_t CMD_EVENT_COUNTS      = 0x040f8320;  // "event_counts" → print event counts
    constexpr uintptr_t CMD_DEBUG_EVENT       = 0x0411b7a0;  // "debug_event" → trigger event
    constexpr uintptr_t CMD_PAUSE_ON          = 0x04127790;  // "pause_on" → pause on date/event
    constexpr uintptr_t CMD_PAUSE_AND_SAVE_ON = 0x041277d0;  // "pause_and_save_on"
    constexpr uintptr_t CMD_ADD_COURT_EVENT   = 0x0412bd10;  // "add_pending_court_event"

    // Functions referenced by event commands
    constexpr uintptr_t EVENT_QUEUE_FUNC      = 0x05e5bb30;  // ref'd by event_queue cmd
    constexpr uintptr_t AI_EVENT_OPTION_EVAL  = 0x05e4d570;  // refs EVENT_OPTION_AI_CHANCE string

    // Event queue accessor
    constexpr uintptr_t EVENT_QUEUE_ACCESSOR  = 0x05f61a40;  // fn(buf, game_state_root)

    // Event object: options array at *(event_obj + 0x1b0)
    constexpr size_t EVENT_OPTIONS_ARRAY      = 0x1b0;

    // Event manager globals (aliases from globals:: namespace for convenience)
    constexpr uintptr_t EVENT_MANAGER         = 0x0bf542c0;  // = globals::EVENT_MANAGER
    constexpr uintptr_t EVENT_QUEUE_MUTEX     = 0x0c28e628;  // = globals::EVENT_QUEUE_MUTEX
    constexpr uintptr_t AI_EVAL_ENABLED_FLAG  = 0x0c5ce5f0;  // = globals::AI_EVENT_EVAL_FLAG

    // LEGACY Round 3 offsets (were applied to SPlayerEventData directly — WRONG)
    // Kept for reference only. Use SPEVENT_* and CEVENT_* below instead.
    constexpr size_t EVENT_CHAR_ID            = 0x1b8;  // DEPRECATED: use SPEVENT_CHAR_ID
    constexpr size_t EVENT_FIRE_FLAG          = 0x1c0;  // DEPRECATED: use SPEVENT_FIRE_FLAG
    constexpr size_t EVENT_NARRATIVE_FLAG     = 0x271;  // DEPRECATED: use CEVENT_NARRATIVE_FLAG
    constexpr size_t EVENT_SCOPE_TYPE         = 0x278;  // DEPRECATED: use CEVENT_STATE

    // CONFIRMED (Round 3+): Pending event queue offsets (relative to queue object)
    constexpr size_t EVENT_LIST_PTR           = 0x1ed8; // ptr: array of SPlayerEventData* (8 bytes each)
    constexpr size_t EVENT_LIST_COUNT         = 0x1ee4; // uint32: event count
    constexpr size_t EVENT_QUEUE_NEXT_ID      = 0x1ef0; // uint32: next event instance ID counter

    // Event queue access path (corrected):
    //   game_root = *(g_base_addr + GAME_STATE_ROOT)
    //   subsys    = *(game_root + GAME_STATE_SUBSYS_PTR)
    //   queue     = subsys + EVENT_QUEUE_OFFSET
    //   array     = *(queue + EVENT_LIST_PTR)
    //   count     = *(queue + EVENT_LIST_COUNT)
    constexpr size_t GAME_STATE_SUBSYS_PTR    = 0xa0;    // *(game_root + 0xa0) -> subsystem base
    constexpr size_t EVENT_QUEUE_OFFSET       = 0x2f280; // subsys + this = event queue object

    // SPlayerEventData fields (0x1C8 bytes per entry)
    // These are on the queue entry, NOT on CEvent
    constexpr size_t SPEVENT_STRING1_PTR      = 0x168;  // ptr: string1
    constexpr size_t SPEVENT_STRING1_LEN      = 0x170;  // size_t: string1 length
    constexpr size_t SPEVENT_STRING2_PTR      = 0x188;  // ptr: string2
    constexpr size_t SPEVENT_STRING2_LEN      = 0x190;  // size_t: string2 length
    constexpr size_t SPEVENT_EXPIRY_DATE      = 0x1a8;  // int64: expiry date
    constexpr size_t SPEVENT_EVENT_DEF        = 0x1b0;  // CEvent*: pointer to event definition
    constexpr size_t SPEVENT_CHAR_ID          = 0x1b8;  // int32: target character ID
    constexpr size_t SPEVENT_INSTANCE_ID      = 0x1bc;  // int32: unique instance ID
    constexpr size_t SPEVENT_FIRE_FLAG        = 0x1c0;  // byte: fire flag

    // CEvent fields (on definition object, accessed via *(SPlayerEventData + SPEVENT_EVENT_DEF))
    constexpr size_t CEVENT_OPTIONS_ARRAY     = 0x1b0;  // ptr: option pointer array
    constexpr size_t CEVENT_OPTION_COUNT      = 0x1bc;  // int32: option count
    constexpr size_t CEVENT_NARRATIVE_FLAG    = 0x271;  // byte: narrative event flag
    constexpr size_t CEVENT_STATE             = 0x278;  // int: 1=player, 2=pending

    // CONFIRMED (Round 3+): Event option struct offsets
    constexpr size_t OPTION_SCRIPT_BLOCK      = 0x28;   // block: ai_chance script value block
    constexpr size_t OPTION_SCRIPT_MULTIPLIER = 0x60;   // int64: fixed-point multiplier within script block
    constexpr size_t OPTION_AI_CHANCE_DIRECT  = 0x318;  // int32: direct ai_chance override (0 = use script)

    // CONFIRMED (Round 3): AI event evaluator helper
    constexpr uintptr_t AI_CHANCE_COMPUTE     = 0x07445820;  // HIGH: fn(option_ptr) → int64 weight (CORRECTED from 0x07445820) (ck-e58l.20)
    constexpr uintptr_t PENDING_EVENT_ENUM    = 0x05e5ace0;  // fn enumerating pending events (365B)
    constexpr uintptr_t AVAILABLE_OPT_COUNT   = 0x05ecf3f0;  // fn(event) -> available option count (883B)

    // CONFIRMED (Round 3): CSelectEventOptionCommand
    constexpr uintptr_t SELECT_OPT_CMD_VTABLE = 0x08987550;  // CSelectEventOptionCommand vtable
    constexpr uintptr_t ON_SELECT_OPTION_UI    = 0x01c33250;  // OnSelectOption UI handler (17844B)

    // CEventWindow struct offsets (NEEDS VERIFICATION — pre-Round 3 guesses)
    constexpr size_t EW_EVENT_ID              = 0x18;  // uint32 or string ptr
    constexpr size_t EW_TARGET_CHAR_ID        = 0x20;  // uint32: character this event targets
    constexpr size_t EW_OPTION_COUNT          = 0x30;  // int32: number of options
    constexpr size_t EW_OPTION_ARRAY          = 0x38;  // ptr: array of CEventOptionItem*
    constexpr size_t EW_TITLE_LOC_KEY         = 0x48;  // ptr: localization key for title
    constexpr size_t EW_DESC_LOC_KEY          = 0x50;  // ptr: localization key for description
}

// =============================================================================
// Alliance Storage (Round 10-2)
//
// Two storage locations:
// 1. CActiveRelation (per-character-pair relation object, from relation manager):
//    +0x88 = SAllianceReason array (stride 0x10: vtable + 2 char refs for "allied through")
//    +0x94 = alliance count (int32, non-zero = allied)
//    +0x189 = "alliance broken" flag
//
// 2. Forced allies (per-character fast path, checked first by IsAlliedTo):
//    *(char+0x1A0)+0x20 = sorted CPdxArray of forced ally char IDs (binary-searched)
//
// Alliance reason enum: 0=Marriage, 1=Betrothal, 2=DiplomacyMarriage,
//                       3=DiplomacyNegotiated, 4=Tributary, 5=BloodBrother
// =============================================================================
namespace alliance {
    // --- CActiveRelation alliance fields ---
    constexpr size_t REASON_ARRAY       = 0x88;   // HIGH: SAllianceReason[] (stride 0x10)
    constexpr size_t REASON_COUNT       = 0x94;   // HIGH: int32, alliance count (non-zero = allied)
    constexpr size_t BROKEN_FLAG        = 0x189;  // HIGH: byte, "alliance broken" flag

    // --- SAllianceReason entry (stride 0x10) ---
    constexpr size_t REASON_ENTRY_SIZE  = 0x10;   // HIGH: bytes per SAllianceReason
    constexpr size_t REASON_VTABLE      = 0x00;   // HIGH: ptr, vtable
    constexpr size_t REASON_CHAR_REF_1  = 0x08;   // HIGH: char ref (first "allied through" character)
    constexpr size_t REASON_CHAR_REF_2  = 0x0C;   // HIGH: char ref (second "allied through" character)

    // --- Forced allies (per-character fast path) ---
    // Access: *(*(char + character::TITLE_DATA) + FORCED_ALLY_ARRAY)
    // CPdxArray<uint32> of forced ally character IDs, binary-searched by IsAlliedTo
    constexpr size_t FORCED_ALLY_ARRAY  = 0x20;   // HIGH: sorted CPdxArray<uint32> at *(char+0x1A0)+0x20

    // --- Alliance reason enum values ---
    constexpr int REASON_MARRIAGE              = 0;
    constexpr int REASON_BETROTHAL             = 1;
    constexpr int REASON_DIPLOMACY_MARRIAGE    = 2;
    constexpr int REASON_DIPLOMACY_NEGOTIATED  = 3;
    constexpr int REASON_TRIBUTARY             = 4;
    constexpr int REASON_BLOOD_BROTHER         = 5;

    // --- break-alliance effect chain (re_break_alliance_impl.md, #ck-bt9x) ---
    // character-scoped effect (CBreakAllowAllianceEffect ⟵ CCharacterEffect ⟵
    // CJominiEffect). Chain: effect-apply → virtual CanExecute gate →
    // break_alliance_cmd (alive + relation re-check) → relation_dissolve (clears
    // CActiveRelation+0x88 reasons, sets state byte +0x189, fires notifications).
    // LEGITIMATE per the write-action audit. Values base-relative = IMAGE − 0x100000.
    // ARG-ABI RESOLVED (re_break_alliance_FINAL.md §0, HIGH): break_alliance_cmd
    // takes two UNWRAPPED CCharacter* + a force byte — both args are passed
    // directly to CCharacter::IsAlive. Do NOT pass scope objects; pass what
    // lookup_character(id & 0xFFFFFF) yields (the same _g_CharacterDB_root walk the
    // engine's FUN_05f72a60 unwrap does). Path B (direct gated cmd) needs NO
    // scope_ctx frame, NO recipient saved-target. The literal break is IMPLEMENTED
    // behind the DEFAULT-OFF g_enable_break_alliance flag (IntentType::BREAK_ALLIANCE,
    // game-thread drain under fake-RNG + watchdog + sigsetjmp). It stays
    // observer-save-GATED: not agent-reachable until the FINAL §5 disposable-save
    // smoke confirms live dissolve behaviour (the ABI is HIGH; live behaviour is
    // the remaining MEDIUM). The safe-now ally-call-refusal posture lever remains
    // the `conqueror`-variable scripted-effect (character-root, no recipient).
    constexpr uintptr_t BREAK_ALLIANCE_EFFECT_APPLY_LB1 = 0x04ec8d10; // HIGH: CBreakAllowAllianceEffect<true>::Execute (vt[23]); gated, force=0
    constexpr uintptr_t BREAK_ALLIANCE_EFFECT_APPLY_LB0 = 0x04ec89f0; // HIGH: CBreakAllowAllianceEffect<false>::Execute (vt[23]); force=1
    constexpr uintptr_t BREAK_ALLIANCE_CMD              = 0x06750ef0; // HIGH: break_alliance_cmd(actor, ally, force) — alive+relation gated; adjacent to IsAlliedTo
    constexpr uintptr_t BREAK_ALLIANCE_TARGET_RESOLVE   = 0x066e45d0; // HIGH: resolve recipient from node+0x70 vs scope frame
    constexpr uintptr_t ALLIANCE_RELATION_LOOKUP        = 0x06660fb0; // HIGH: CActiveRelation lookup (rel+0x1a8 gate; null = _DAT_0c429ca0)
    constexpr uintptr_t ALLIANCE_RELATION_DISSOLVE      = 0x06269600; // HIGH: clears CActiveRelation+0x88 reasons (count+0x94), state byte +0x189, fires notifications
    constexpr size_t    REL_LIVE_RELATION_GATE          = 0x1a8;      // HIGH: CActiveRelation live-relation gate (!=0 ⇒ breakable)
}

// =============================================================================
// Interaction Command Vtable Offsets (wrapper object from BUILD_SEND_INTERACTION_CMD)
// vtable layout recovered from TLS pipeline + direct command v7 dispatch
// =============================================================================
namespace interaction_cmd {
    constexpr size_t VTABLE_DTOR            = 0x00;   // HIGH: vtable[0] = non-deleting destructor
    constexpr size_t VTABLE_DELETING_DTOR   = 0x08;   // HIGH: vtable[1] = deleting destructor
    constexpr size_t VTABLE_IS_VALID        = 0x38;   // HIGH: vtable[7] = is_valid check
    constexpr size_t VTABLE_CLONE           = 0x48;   // HIGH: vtable[9] = clone function
    constexpr size_t CMD_BUF_SIZE           = 0x380;  // stack allocation size for cmd_buf (16-byte aligned)
}

// =============================================================================
// CCommandDispatchManager Struct Offsets (inline struct in .bss)
// Accessed via DAT_0c69dd68; engine calls FUN_07660390(&mgr, &clone, 7, 0)
// =============================================================================
namespace dispatch_mgr {
    constexpr size_t GATE_BYTE              = 0x25A;  // HIGH: uint8, dispatch gate field
    constexpr size_t QUEUE_PTR              = 0x3E8;  // HIGH: ptr, command queue
    constexpr size_t RECORDER_PTR           = 0x3F0;  // HIGH: ptr, command recorder (null to bypass replay log)
}

// =============================================================================
// Console Command Handler Addresses
// =============================================================================
// Handler ABI (Itanium): SResult* handler(SResult* out, const SCommandData* data)
// SCommandData: +0x00 = ptr to arg array (stride 0x20: +0x00=char*, +0x08=len), +0x0C = int32 arg_count
// SResult: +0x00 = uint8 success, +0x08 = std::string message (SSO)
// All addresses base-relative (Ghidra VA minus 0x100000 image base).
// Source: console_commands.csv HandlerFuncAddress column, verified addresses only.
namespace console_handlers {
    // AI commands
    constexpr uintptr_t CMD_AI_DECLARE_WAR     = 0x04122760;  // HIGH: AI.declare_war (decompile-verified)
    constexpr uintptr_t CMD_AI_ENABLE          = 0x04100770;  // HIGH: AI.Enable
    constexpr uintptr_t CMD_AI_DISABLE         = 0x041010d0;  // HIGH: AI.Disable
    constexpr uintptr_t CMD_AI_TOGGLE_ALL      = 0x04134500;  // HIGH: AI.ToggleAll
    constexpr uintptr_t CMD_AI_TRY_DECISION    = 0x04128300;  // HIGH: AI.try_send_decision
    constexpr uintptr_t CMD_AI_INTERACTION_VALS = 0x0411fc40;  // HIGH: ai.interaction_values

    // Resource commands (add_gold CSV entry is 038a1870 — misidentified destructor, excluded)
    constexpr uintptr_t CMD_ADD_PIETY          = 0x040fa320;  // HIGH: add_piety
    constexpr uintptr_t CMD_ADD_PRESTIGE       = 0x040fa490;  // HIGH: add_prestige
    constexpr uintptr_t CMD_ADD_STRESS         = 0x040ff290;  // HIGH: add_stress
    constexpr uintptr_t CMD_ADD_DREAD          = 0x040ff860;  // HIGH: add_dread
    constexpr uintptr_t CMD_ADD_INFLUENCE      = 0x04134950;  // HIGH: add_influence

    // Trait/character commands
    constexpr uintptr_t CMD_ADD_TRAIT          = 0x040fab10;  // HIGH: add_trait
    constexpr uintptr_t CMD_ADD_CLAIM          = 0x040fa860;  // HIGH: add_claim
    constexpr uintptr_t CMD_GIVE_TITLE         = 0x040fa610;  // HIGH: give_title

    // Title/law commands
    constexpr uintptr_t CMD_ADD_REALM_LAW      = 0x04123480;  // HIGH: add_realm_law
    constexpr uintptr_t CMD_ADD_TITLE_LAW      = 0x0411eb80;  // HIGH: add_title_law
    constexpr uintptr_t CMD_ADD_DOCTRINE       = 0x04122c30;  // HIGH: add_doctrine
    constexpr uintptr_t CMD_CHANGE_FERVOR      = 0x041244a0;  // HIGH: change_fervor

    // Innovation commands
    constexpr uintptr_t CMD_ADD_INNOVATION     = 0x0411c7a0;  // HIGH: add_innovation
    constexpr uintptr_t CMD_ADD_FASCINATION    = 0x0411cd00;  // HIGH: add_fascination

    // Relation/opinion
    constexpr uintptr_t CMD_ADD_RELATION       = 0x040fe670;  // HIGH: add_relation
    constexpr uintptr_t CMD_ADD_SECRET         = 0x04124810;  // HIGH: add_secret

    // Realm law (skip effects variant)
    constexpr uintptr_t CMD_ADD_REALM_LAW_SKIP = 0x04123790;  // HIGH: add_realm_law_skip_effects (bypasses cost+pass effects)

    // AI interaction
    constexpr uintptr_t CMD_AI_TRY_INTERACTION = 0x04127e90;  // HIGH: AI.try_send_interaction <type> <char_id>

    // Culture
    constexpr uintptr_t CMD_CHANGE_CULTURE     = 0x040f9870;  // HIGH: change_culture (county culture)
    constexpr uintptr_t CMD_SET_CULTURE        = 0x04124030;  // HIGH: set_culture (character culture)

    // Character info/state
    constexpr uintptr_t CMD_CHARINFO           = 0x040f7c70;  // HIGH: charinfo (toggle debug info)
    constexpr uintptr_t CMD_SET_SEXUALITY      = 0x040fae90;  // HIGH: set_sexuality
    constexpr uintptr_t CMD_KNOW_SCHEMES       = 0x040f76b0;  // HIGH: know_schemes
    constexpr uintptr_t CMD_END_SCHEMES        = 0x040f7740;  // HIGH: end_schemes

    // Game control
    constexpr uintptr_t CMD_TICK_DAY           = 0x040f4280;  // HIGH: tick_day (advance game by 1 day)
    constexpr uintptr_t CMD_TICK_DEVELOPMENT   = 0x040fda70;  // HIGH: tick_development
    constexpr uintptr_t CMD_FORCE_OOS          = 0x040f7b80;  // HIGH: force_oos
    constexpr uintptr_t CMD_ABORT_TRAVEL       = 0x04121e70;  // HIGH: abort_travel_plan
    constexpr uintptr_t CMD_PAUSE_ON           = 0x04127790;  // HIGH: pause_on <date|event>
    constexpr uintptr_t CMD_PAUSE_SAVE_ON      = 0x041277d0;  // HIGH: pause_and_save_on

    // Save/load
    constexpr uintptr_t CMD_TEST_SAVE          = 0x040f5050;  // HIGH: test_save
    constexpr uintptr_t CMD_SAVE_TEXT          = 0x04105850;  // HIGH: save_text
    constexpr uintptr_t CMD_SAVE_ZIP_BINARY    = 0x04105860;  // HIGH: save_zip_binary_gamestate

    // Debug / Script debugging
    constexpr uintptr_t CMD_HELPLOG            = 0x040f3db0;  // HIGH: helplog (decompile-verified)
    constexpr uintptr_t CMD_AIWATCH            = 0x040fd440;  // HIGH: aiwatch (matches funcs::CMD_AIWATCH)
    constexpr uintptr_t CMD_DEBUG_MODE         = 0x0762f940;  // HIGH: debug_mode — toggles debug mode (tooltips, province IDs, extra logging) (ck-e58l.20)
    constexpr uintptr_t CMD_SCRIPT_DOCS        = 0x0755a6a0;  // HIGH: script_docs / script_documentation — prints script documentation (ck-e58l.20)
    constexpr uintptr_t CMD_SCRIPTED_TESTS     = 0x040f7870;  // HIGH: scripted_tests — runs scripted tests (ck-e58l.20)
    constexpr uintptr_t CMD_DEBUG_EVENT        = 0x04108760;  // HIGH: debug_event — trigger event for character, prints trigger info (ck-e58l.20)
    constexpr uintptr_t CMD_EVENT_QUEUE        = 0x040f8200;  // HIGH: event_queue — prints event queue debug info
    constexpr uintptr_t CMD_EVENT_COUNTS       = 0x040f8320;  // HIGH: event_counts — prints event counts
    constexpr uintptr_t CMD_MISSING_TRIG_LOC   = 0x04109030;  // HIGH: missing_trigger_localization — dumps missing trigger loc (ck-e58l.20)

    // Script optimizer
    constexpr uintptr_t CMD_SCRIPT_OPT_DUMP    = 0x040f4f70;  // HIGH: ScriptOptimizer.DumpHeuristicsInfo
    constexpr uintptr_t CMD_VALIDATE_CHARS     = 0x04134690;  // HIGH: Log.SpecialActionFactories (validate chars)
}

} // namespace ck3

// =============================================================================
// Script Debug / Logging System (ck-oaqw)
// CK3's built-in trigger/effect debug infrastructure
// =============================================================================
namespace script_debug {
    // NLog::ELogLevel enum (template parameter for CDebugLogScopesEffect)
    constexpr int LOG_LEVEL_ERROR   = 1;  // error_log_scopes → writes to error.log
    constexpr int LOG_LEVEL_WARNING = 2;  // (warning level, variant exists in RTTI)
    constexpr int LOG_LEVEL_DEBUG   = 4;  // debug_log_scopes → writes to debug.log

    // CDebugLogScopesEffect registration function (shared by error_log_scopes + debug_log_scopes)
    constexpr uintptr_t LOG_SCOPES_REG_FUNC    = 0x01742630;  // HIGH: _INIT_3809, registers log effects — MEDIUM (rodata_scan)

    // CDebugLogStackTrace registration function (shared by error_log_stack_trace + debug_log_stack_trace)
    constexpr uintptr_t LOG_STACK_REG_FUNC     = 0x024642f0;  // MEDIUM: _INIT_6799, registers stack trace effects

    // Script keyword IDs (from script token table at RVA 0x0bc5c498 area)
    constexpr int KEYWORD_DEBUG_LOG_SCOPES  = 1458;  // HIGH: "debug_log_scopes" effect keyword
    constexpr int KEYWORD_DEBUG_LOG_DETAILS = 1461;  // HIGH: "debug_log_details" effect keyword
    constexpr int KEYWORD_ON_TRIGGER_FAIL   = 1487;  // HIGH: "on_trigger_fail" callback keyword

    // CDebugModeTrigger — scripted trigger checking if debug_mode is active
    constexpr uintptr_t DEBUG_MODE_TRIGGER_VT  = 0x0bc449b8;  // HIGH: vtable (from RTTI typeinfo chain)

    // Global debug_mode flag — single byte (0=OFF, 1=ON)
    // Toggled by debug_mode console command (CMD_DEBUG_MODE at 0x0762f940).
    // Read by 30+ call sites including CDebugModeTrigger evaluator.
    // Mislabeled in Ghidra as "g_AI_EventEvalEnabled" — its actual semantics are
    // game-wide debug mode, not AI-specific (verified via xref count + console
    // command "Toggles debug mode" + "Turning debug ON/OFF" status messages).
    constexpr uintptr_t DEBUG_MODE_FLAG        = 0x0c5ce5f0;  // HIGH: byte in .bss (ck-oaqw)
    constexpr uintptr_t DEBUG_MODE_GET_FN      = 0x07728970;  // HIGH: getter — `mov al, [DEBUG_MODE_FLAG]; ret` (ck-oaqw)
    constexpr uintptr_t DEBUG_MODE_SET_FN      = 0x07728980;  // HIGH: setter — `mov [DEBUG_MODE_FLAG], sil; ret` (sil=arg2 byte) (ck-oaqw)

    // Log file paths (written by NLog pdx_rotating_file_sink)
    // Located at: ~/.local/share/Paradox Interactive/Crusader Kings III/logs/
    //   error.log — script errors, wrong scope messages, assertion failures
    //   debug.log — debug_log_scopes output, debug_log_stack_trace output
    //   game.log  — general game logging, helplog command output

    // ScriptOptimizer console commands
    constexpr uintptr_t CMD_SCRIPT_OPT_DUMP    = 0x040f4f70;  // HIGH: ScriptOptimizer.DumpHeuristicsInfo
    constexpr uintptr_t CMD_SCRIPT_OPT_READ    = 0x040f4ec0;  // HIGH: ScriptOptimizer.ReadOptimizationCaches
    constexpr uintptr_t CMD_SCRIPT_OPT_WRITE   = 0x040f4de0;  // HIGH: ScriptOptimizer.WriteOptimizationCaches
}

// =============================================================================
// COpinionModifier Struct Offsets (Round 9, from opinion_modifier.cpp)
// HIGH confidence — source path strings + field validation in decompilation
// =============================================================================
namespace opinion_modifier {
    constexpr size_t NAME_KEY           = 0x10;   // HIGH: undefined8, name key/hash
    // +0x30..0x47: undocumented fields (std::string padding, etc.)
    constexpr size_t REASON_FLAGS       = 0x48;   // HIGH: uint16, EOpinionFlag bitmask (Phase 31 RE)
    //   Bit 0 (0x01) = imprisonment_reason  (has_imprisonment_reason trigger)
    //   Bit 1 (0x02) = banish_reason        (has_banish_reason trigger)
    //   Bit 2 (0x04) = execute_reason       (has_execute_reason trigger)
    //   Bit 3 (0x08) = revoke_title_reason  (has_revoke_title_reason trigger)
    //   Bit 4 (0x10) = divorce_reason       (has_divorce_reason trigger)
    //   Bit 5 (0x20) = obedience_reason     (has_obedience_reason trigger)
    // Source: FUN_04fc3ed0 (add_opinion_imprison_reason handler), reads *(ushort*)(modifier_def+0x48)
    constexpr size_t MIN_VALUE          = 0x50;   // HIGH: int32, min clamp (was mislabeled OPINION_VALUE)
    constexpr size_t MAX_VALUE          = 0x54;   // HIGH: int32, max opinion value
    constexpr size_t OPINION_VALUE      = 0x58;   // HIGH: int32, base opinion value (verified: FUN_06b8d950)
    constexpr size_t DECAY_DELAY        = 0x5C;   // HIGH: int32, decay delay days
    constexpr size_t MONTHLY_CHANGE     = 0x60;   // HIGH: int64, monthly change rate (fixed-point)
    constexpr size_t IS_DECAYING        = 0x68;   // HIGH: byte, 1=decaying modifier
    constexpr size_t IS_GROWING         = 0x69;   // HIGH: byte, 1=growing modifier
    constexpr size_t HAS_MIN_MAX        = 0x6A;   // HIGH: byte, 1=uses min/max bounds
    constexpr size_t HAS_MAX            = 0x6B;   // HIGH: byte, 1=has max bound
    constexpr size_t IS_STACKING        = 0x6C;   // HIGH: byte, 1=stacking allowed
    // Validation rules (from opinion_modifier.cpp):
    // - Cannot be decaying AND growing simultaneously
    // - Decaying modifier must have duration or monthly_change
    // - Cannot have duration AND monthly_change simultaneously
    // - min must be <= max
    // - Using min/max without stacking makes little sense

    // EOpinionFlag enum values (bitmask, used as template param for CHasOpinionFlagTrigger)
    // These match the bit positions in REASON_FLAGS at +0x48
    constexpr uint16_t FLAG_IMPRISONMENT_REASON  = 0x01;
    constexpr uint16_t FLAG_BANISH_REASON        = 0x02;
    constexpr uint16_t FLAG_EXECUTE_REASON       = 0x04;
    constexpr uint16_t FLAG_REVOKE_TITLE_REASON  = 0x08;
    constexpr uint16_t FLAG_DIVORCE_REASON       = 0x10;
    constexpr uint16_t FLAG_OBEDIENCE_REASON     = 0x20;
}

// =============================================================================
// CActiveOpinion Entry Offsets (per-character opinion modifier instance)
// Round 9, from FUN_062c5750 (ACTIVE_OPINION_SUM)
// HIGH confidence — decompilation-verified arithmetic
// =============================================================================
namespace active_opinion {
    constexpr size_t MODIFIER_PTR       = 0x08;   // HIGH: ptr to COpinionModifier* (via DB)
    constexpr size_t START_DATE         = 0x10;   // HIGH: int32, date when modifier was applied
    constexpr size_t END_DATE           = 0x18;   // HIGH: int32, date when modifier expires
    constexpr size_t CURRENT_VALUE      = 0x20;   // HIGH: int32, duration/decay progress
    constexpr size_t CURRENT_OPINION    = 0x24;   // HIGH: int32, current effective opinion value
    constexpr size_t BASE_OPINION       = 0x28;   // HIGH: int32, original opinion value
    // Game uses fixed-point /100000 arithmetic internally for decay calculations
}

// =============================================================================
// CCultureInnovationType Struct Offsets (Round 10)
// Looked up via globals::INNOVATION_TYPE_DB hash table
// =============================================================================
namespace innovation_type {
    constexpr size_t NAME_KEY           = 0x10;   // HIGH: std::string, innovation name/key
    constexpr size_t TYPE_ENUM          = 0x30;   // HIGH: int32, innovation type (3=invalid)
    constexpr size_t ERA_INDEX          = 0x34;   // HIGH: int32, culture era index
    constexpr size_t CAN_ADVANCE        = 0x38;   // HIGH: scripted trigger section
    constexpr size_t DB_INDEX           = 0x08;   // HIGH: int32, database index (used as array index)
    constexpr size_t ERA_REF            = 0x1F8;  // MEDIUM: ptr, culture era reference
}

// =============================================================================
// CCultureInnovation Instance Struct (Round 14, updated Round 15)
// Per-innovation-per-culture progress tracker
// Located at: culture + 0x5D0 (flat array, stride = 0x210, 110 entries)
// Index: innovation_type + 0x08 (int32 DB index)
// RTTI: 18CCultureInnovation
// =============================================================================
namespace innovation_instance {
    constexpr size_t VTABLE              = 0x00;   // HIGH: vtable ptr
    constexpr size_t CULTURE_REF         = 0x08;   // HIGH: ptr/ref, back-reference to owning culture
    constexpr size_t INNOVATION_TYPE_PTR = 0x10;   // HIGH: CCultureInnovationType* — the type definition
    constexpr size_t SPREAD_PROGRESS     = 0x18;   // HIGH: int64, 0-10000000 (10M = 100% complete)
    constexpr size_t EXPOSURE_COUNT      = 0x28;   // HIGH: int32, neighbor cultures with this innovation (R15 core verified)
    constexpr size_t CURRENT_COUNT       = 0x2C;   // HIGH: int32, compared vs effective spread rate (R15 decompile)
    constexpr size_t MODIFIER_DATA       = 0x70;   // MEDIUM: modifier/effect data block
    constexpr size_t MODIFIER_CONTAINER  = 0xD8;   // MEDIUM: modifier container
    constexpr size_t STRIDE              = 0x210;  // HIGH: stride between instances (528 bytes)
    constexpr size_t ARRAY_COUNT         = 110;    // HIGH: total innovation instances per culture (R15 core verified)
}

// =============================================================================
// CultureEraEntry Struct (Round 14, updated Round 15)
// Located at: culture + 0x5B8 (flat array, stride = 0x58, count = 4)
// Count: culture + 0x5C4
// Eras: Tribal(0), Early Medieval(1), High Medieval(2), Late Medieval(3)
// =============================================================================
namespace culture_era {
    constexpr size_t VTABLE              = 0x00;   // HIGH: vtable ptr (R15 core verified)
    constexpr size_t ERA_TYPE_PTR        = 0x08;   // HIGH: ptr, culture era type ("culture_era_tribal" etc.) (R15 verified)
    constexpr size_t PREV_ERA_PTR        = 0x10;   // HIGH: ptr, previous era entry (NULL for era[0]) (R15 verified)
    constexpr size_t ERA_REF             = 0x20;   // HIGH: ref, culture object reference
    constexpr size_t FASCINATION_PROGRESS = 0x28;  // HIGH: int64, 0-10000000 (10M = 100%) (R15 verified)
    constexpr size_t INNOVATION_LIST_PTR = 0x38;   // HIGH: ptr, era's innovation instance list (R15 core verified)
    constexpr size_t INNOVATION_TOTAL    = 0x40;   // HIGH: int32, total innovations in this era (R15 verified)
    constexpr size_t INNOVATION_CURRENT  = 0x44;   // MEDIUM: int32, innovations discovered in this era
    constexpr size_t STRIDE              = 0x58;   // HIGH: stride between era entries (88 bytes)
}

// =============================================================================
// CCulture Struct Offsets (Round 10, expanded Round 13, Round 14)
// Looked up via globals::CULTURE_INDEXED_DB
// Size: ~0x800+ bytes. RTTI: 8CCulture
// =============================================================================
namespace culture {
    constexpr size_t ID                 = 0x10;   // HIGH: uint32, culture ID
    constexpr size_t EXTENDED_DATA      = 0x18;   // MEDIUM: ptr, extended data block (ReadInit catch-all)
    // Script key vs display name (core-dump verified 20260624, ck-#130 set_character_culture):
    //   SCRIPT_KEY (+0x1C8) is the raw scripted key ("english","anglo_saxon","norse",…)
    //   NAME (+0x260) is the localized display name ("English","Anglo-Saxon","Norse") —
    //     returned by GetCultureNameString @ 0x05c188b0 (LEA RAX,[RDI+0x260]; RET).
    // Resolve a culture obj by key by scanning CULTURE_INDEXED_DB (hash_map) and matching +0x1C8.
    constexpr size_t SCRIPT_KEY         = 0x1C8;  // HIGH: std::string, scripted culture key (core-dump verified)
    constexpr size_t NAME               = 0x260;  // HIGH: std::string, localized display name (GetCultureNameString)

    // Innovation progress tracking (Round 14, updated Round 15)
    // NOTE: culture+0x598 was previously labeled FASCINATION_PTR (CCultureInnovationType*).
    // R15 core dump shows it equals ERA_ENTRIES_PTR — it's the era base, NOT a fascination type.
    // CultureAddFascinationProgress (0x05d20a90) uses it only as a non-null guard.
    constexpr size_t ERA_BASE_PTR       = 0x598;  // HIGH: ptr, era entries base (= ERA_ENTRIES_PTR) (R15 corrected)
    constexpr size_t RESEARCH_SPEED_MOD = 0x5AC;  // HIGH: int32, research speed modifier (fixed-point /100000) (R15 verified)
    constexpr size_t ERA_ENTRIES_PTR    = 0x5B8;  // HIGH: ptr, CultureEraEntry array (stride=0x58)
    constexpr size_t ERA_ENTRIES_COUNT  = 0x5C4;  // HIGH: int32, number of era entries (= 4) (R15 verified)
    constexpr size_t INNOVATION_ARRAY   = 0x5D0;  // HIGH: ptr, CCultureInnovation flat array base (stride=0x210, 110 entries)
    // Known innovations array: CPdxArray<CCultureInnovationType*>
    // Confirmed via decompilation of FUN_05d208c0 (8 bytes):
    //   LEA RAX,[RDI + 0x5e8]; RET
    // Array layout: +0x0=data_ptr, +0x8=capacity(int32), +0xC=count(int32), stride=8
    // R15: stores CCultureInnovationType* (not CCultureInnovation*), verified in core dump
    constexpr size_t KNOWN_INNOVATIONS  = 0x5E8;  // HIGH: CPdxArray<CCultureInnovationType*> (R15 type corrected)
    constexpr size_t PENDING_INNOV_PTR  = 0x600;  // MEDIUM: ptr, pending innovation type ptrs
    constexpr size_t PENDING_INNOV_COUNT = 0x60C; // MEDIUM: int32, pending count (R15: 104 in core)
    // Fascination pointers (corrected #fascination 2026-06-24, decompile + core-dump):
    //   FASCINATION_PTR (+0x6F0) is the PRIMARY/ACTIVE fascination (GetFascinationType;
    //     add_fascination FUN_0421cd00 reads *(culture+0x6f0)). This is the one the query
    //     should report as "fascination".
    //   BONUS_FASCINATION_PTR (+0x6D8) is the BONUS-marker fascination (GetBonusFascinationType
    //     / set_bonus_fascination_innovation) — distinct from the primary for ~85% of cultures.
    // The old labels FASCIN_CANDIDATE_1(=BONUS 0x6D8) / FASCIN_CANDIDATE_3(=PRIMARY 0x6F0) were
    // swapped relative to intent; aliases kept below for compatibility.
    constexpr size_t BONUS_FASCINATION_PTR = 0x6D8;  // HIGH: ptr, CCultureInnovationType* BONUS-marker fascination
    constexpr size_t FASCINATION_PTR       = 0x6F0;  // HIGH: ptr, CCultureInnovationType* PRIMARY/ACTIVE fascination
    constexpr size_t FASCIN_CANDIDATE_1 = 0x6D8;  // DEPRECATED alias of BONUS_FASCINATION_PTR
    constexpr size_t FASCIN_CANDIDATE_3 = 0x6F0;  // DEPRECATED alias of FASCINATION_PTR
    constexpr size_t REFORM_TYPE        = 0x6EC;  // MEDIUM: int32, reformation_type enum (token 0x31ba)
    constexpr size_t ACTIVITY_REF       = 0x788;  // MEDIUM: uint32, activity reference
}

// =============================================================================
// CFaith Struct Offsets (Round 13, updated Round 14 - Religion/Conversion investigation)
// Looked up via globals::FAITH_INDEXED_DB (TPdxRefDatabase<CFaith, 8>)
// Also accessible via FAITH_DB hash table: db+0x20=table, db+0x2c=capacity,
//   entries at table+8+id*0x10. Total faiths at db+0x38 (139 in 867 start).
// RTTI: 6CFaith. Inherits CJominiScriptScopeObject pattern.
// Serialized as "game_faith_struct" (save handler: FUN_05fa65b0 and related)
//
// Object chain: CFaith → CFaithTemplate(+0x20) → CReligionTemplate(+0x468) → CReligionFamily(+0x178)
// =============================================================================
namespace faith {
    constexpr size_t VTABLE             = 0x00;   // HIGH: vtable pointer (RTTI: 6CFaith)
    constexpr size_t FAITH_ID           = 0x08;   // HIGH: uint32, sequential faith ID (0-139, R14 core dump verified)
    constexpr size_t NAME_KEY           = 0x10;   // HIGH: std::string, faith key name (db_object name)
    constexpr size_t RELIGION_TEMPLATE_REF = 0x1c; // HIGH: uint32, religion template DB ref — used in SetCountyFaith (ck-pj01)
    constexpr size_t FAITH_TEMPLATE     = 0x20;   // HIGH: ptr to CFaithTemplate (R14 RTTI verified: 14CFaithTemplate)

    // Round 14: Comprehensive struct layout from core dump + decompilation
    constexpr size_t FAITH_KEY          = 0x98;   // HIGH: std::string, faith key ("catholic", "theravada", etc.)
    constexpr size_t DISPLAY_NAME       = 0xC0;   // HIGH: std::string, localized display name ("Catholicism")
    constexpr size_t DISPLAY_NAME_ALT   = 0xF0;   // MED: std::string, alternate display name
    constexpr size_t ADHERENT_NAME      = 0x120;  // HIGH: std::string, adherent singular ("Catholic")
    constexpr size_t ADHERENT_PLURAL    = 0x150;  // HIGH: std::string, adherent plural ("Catholics")
    constexpr size_t DESC_KEY           = 0x180;  // HIGH: std::string, description localization key
    constexpr size_t GFX_KEY            = 0x1E8;  // HIGH: std::string, graphical culture key ("pagan_gfx")
    constexpr size_t COLOR_ARGB         = 0x248;  // HIGH: uint32, ARGB map color (e.g. 0xFFCCCC99 for Catholic)
    constexpr size_t FERVOR             = 0x250;  // HIGH: int64, fixed-point /100000 — DECOMPILE VERIFIED via FUN_05fa8820
                                                   //   Default 50.0 (=5,000,000 raw). Range [0, MAX_FERVOR*100000].
                                                   //   Monthly delta: base_fervor_increase(3.5) × 1/sqrt(max(10, county_count)/10)
                                                   //   ChangeFervor(faith, delta, ctx) at FUN_05fa8820 handles clamping.
    constexpr size_t FERVOR_SENTINEL    = 0x258;  // LOW: int64, 0xFFFFFFFF00000004 observed — possible padding/sentinel

    // Round 15: Fervor system — doctrine parameter containers and modifier arrays
    // STokenParameter inline containers: CPdxHybridInlineBufferAllocator<STokenParameter, 4>
    // Each STokenParameter is 16 bytes: uint32 token_id, uint32 flags, int64 value (fp /100000)
    constexpr size_t DOCTRINE_PARAMS_1          = 0x2C0;  // MED: first doctrine parameter container start (ptr)
    constexpr size_t DOCTRINE_PARAMS_1_DATA     = 0x2E0;  // MED: inline STokenParameter[4] entries
    constexpr size_t DOCTRINE_PARAMS_1_COUNT    = 0x330;  // MED: uint32, number of active entries
    constexpr size_t DOCTRINE_PARAMS_2          = 0x390;  // MED: second doctrine parameter container start (ptr)
    constexpr size_t DOCTRINE_PARAMS_2_DATA     = 0x3B0;  // MED: inline STokenParameter[4] entries
    // Modifier definitions array
    constexpr size_t MODIFIER_DEFS_ARRAY        = 0x460;  // MED: ptr to STokenParameter array
    constexpr size_t MODIFIER_DEFS_COUNT        = 0x468;  // MED: uint32, count (3 observed for Catholic)
    // CModifierInstance* arrays (active modifiers on faith, 1 per faith at 867 start)
    constexpr size_t ACTIVE_MODS_ARRAY_1        = 0x478;  // MED: ptr to CModifierInstance* array
    constexpr size_t ACTIVE_MODS_COUNT_1        = 0x480;  // MED: uint32, count (1 at game start for all faiths)
    // County follower count — used by fervor size adjustment and conversion tracking (ck-pj01)
    constexpr size_t NUM_COUNTY_FOLLOWERS        = 0x4c8;  // HIGH: int32, counties following this faith (decompile-verified)
                                                            //   Catholic=443, Tengri=365, Orthodox=170 at 867 start
                                                            //   Updated by FUN_05faa4e0: faith+0x4c8 += delta

    // Great Holy War references
    constexpr size_t GHW_REFS_ARRAY             = 0x4F8;  // MED: ptr to TPdxRef<CGreatHolyWar> array
    constexpr size_t GHW_REFS_COUNT             = 0x500;  // MED: uint32, count (6 for Catholic at 867 start)
    // Fervor changelog (tooltip history entries)
    constexpr size_t FERVOR_CHANGELOG_PTR       = 0x548;  // MED: ptr to pair<CJominiHistoricalDate, CUnsynchedVariable<CString>>[]
    constexpr size_t FERVOR_CHANGELOG_COUNT     = 0x558;  // MED: uint32, number of entries (lo32)
                                                           //   Entries expire after FERVOR_CHANGELOG_DURATION (10 years)

    constexpr size_t DOCTRINE_CONTAINER = 0x278;  // HIGH: CFaithDoctrineContainer vtable ptr
    constexpr size_t DOCTRINE_ARRAY     = 0x280;  // HIGH: ptr to CFaithDoctrine* array (R14 core dump)
    constexpr size_t DOCTRINE_COUNT     = 0x288;  // HIGH: uint32, number of doctrines (28 for Catholic)

    // AddDoctrine (FUN_052ac290) accesses:
    //   faith+0x108: pointer (possibly doctrine_container_ptr or alternate DB)
    //   faith+0x114: type byte, checked == 0x02
    constexpr size_t FIELD_108          = 0x108;  // MED: ptr, used in AddDoctrine conditional
    constexpr size_t FIELD_114          = 0x114;  // MED: byte, type flag (0x02 = alternate path)

    // CFaithDoctrineContainer layout (at faith+0x278):
    //   +0x08: long* array (CPdxArray<CFaithDoctrine*> data pointer) — also accessible directly at faith+0x280
    //   +0x14: int count — also accessible directly at faith+0x288
    //   Each element is a CFaithDoctrine* with name at element+0x10, ID at element+0x290
    //
    // Hostility doctrines (first doctrine in array determines faith hostility group):
    //   "abrahamic_hostility_doctrine" — Abrahamic faiths (rf_abrahamic family)
    //   "eastern_hostility_doctrine"   — Dharmic/Eastern faiths (rf_eastern family)
    //   "pagan_hostility_doctrine"     — Pagan faiths (rf_pagan family)
    // Pluralism doctrines (determine tolerance level):
    //   "doctrine_pluralism_righteous"      — intolerant (Catholic)
    //   "doctrine_pluralism_pluralistic"    — tolerant (pagans, Buddhists)
    //   "doctrine_pluralism_fundamentalist" — most hostile

    // Key functions (decompiled and confirmed):
    //   FUN_05fa8820 — ChangeFervor(faith, delta, ctx) — adds delta to +0x250, clamps (R14 decompile)
    //   FUN_06756460 — GetCharacterFaithId(char) → char+0xB4 (R14 decompile, trivial)
    //   FUN_052ac290 — CFaith_AddDoctrine(ctx, &doctrine*) — logs "already has doctrine"
    //   FUN_052acc20 — RemoveDoctrine — logs "doesn't have doctrine"
    //   FUN_05fa65b0 — CFaith_SaveHandler (game_faith_struct save handler, 119B)
    //   FUN_05fb6260 — CFaith_SaveFieldMapper (maps serialized field offsets)
    //   FUN_05fa80a0 — CFaith_AddDoctrineToContainer (35B, calls FUN_05fb8d40(faith+0x278))
    //   FUN_05fb8120 — CFaithDoctrineContainer_HasDoctrine (250B, 4x-unrolled search)
    //   FUN_05fb7f70 — GetDoctrineModifier(container, modifier_key, default) — reads modifier from doctrines
    //   FUN_047047e0 — uses "doctrine_grouping_" prefix for UI grouping
    //
    // has_doctrine trigger:
    //   RTTI: CTriggerEntry<CHasDoctrineTrigger> (vtable 0x0badaf08)
    //   Concrete: CHasDoctrineTrigger (vtable 0x0badaf50)
    //   Uses CJominiObjectTrigger<CFaithDoctrineDatabase> pattern
    //   Resolves doctrine by name, checks membership via container
}

// =============================================================================
// CFaithTemplate Struct Offsets (Round 14)
// RTTI: 14CFaithTemplate
// Access: CFaith+0x20 (FAITH_TEMPLATE)
// Static definition object — does not change at runtime.
// =============================================================================
namespace faith_template {
    constexpr size_t VTABLE             = 0x00;   // HIGH: vtable pointer (RTTI: 14CFaithTemplate)
    constexpr size_t FAITH_KEY          = 0x10;   // HIGH: std::string, faith key ("catholic")
    constexpr size_t DISPLAY_NAME       = 0x60;   // HIGH: std::string, display name ("Catholicism")
    constexpr size_t ADJECTIVE          = 0x90;   // HIGH: std::string, adjective ("Catholic")
    constexpr size_t PLURAL             = 0xF0;   // HIGH: std::string, plural ("Catholics")
    constexpr size_t HOLY_SITE_ARRAY    = 0x200;  // HIGH: ptr to CFaithHolySiteTemplate* array
    constexpr size_t HOLY_SITE_COUNT    = 0x208;  // HIGH: uint32, number of holy sites (6 for Catholic)
    constexpr size_t RELIGION_TEMPLATE  = 0x468;  // HIGH: ptr to CReligionTemplate (RTTI verified)
    // Catholic holy sites: jerusalem, rome, cologne, santiago, kent
}

// =============================================================================
// CReligionTemplate Struct Offsets (Round 14)
// RTTI: 17CReligionTemplate
// Access: CFaithTemplate+0x468 (RELIGION_TEMPLATE)
// Static definition — defines a religion (e.g., Christianity, Buddhism).
// =============================================================================
namespace religion_template {
    constexpr size_t VTABLE             = 0x00;   // HIGH: vtable (RTTI: 17CReligionTemplate)
    constexpr size_t RELIGION_KEY       = 0x10;   // HIGH: std::string ("christianity_religion")
    constexpr size_t DISPLAY_NAME       = 0x60;   // HIGH: std::string ("Christianity")
    constexpr size_t ADJECTIVE          = 0x90;   // HIGH: std::string ("Christian")
    constexpr size_t PLURAL             = 0xF0;   // HIGH: std::string ("Christians")
    constexpr size_t BANNER_TEXTURE     = 0x158;  // HIGH: std::string ("core_tenet_banner_christian.dds")
    constexpr size_t RELIGION_FAMILY    = 0x178;  // HIGH: ptr to CReligionFamily (RTTI: 15CReligionFamily)
}

// =============================================================================
// CReligionFamily Struct Offsets (Round 14)
// RTTI: 15CReligionFamily
// Access: CReligionTemplate+0x178 (RELIGION_FAMILY)
// Three known families: rf_abrahamic, rf_eastern, rf_pagan
// Family corresponds to hostility doctrine group.
// =============================================================================
namespace religion_family {
    constexpr size_t VTABLE             = 0x00;   // HIGH: vtable (RTTI: 15CReligionFamily)
    constexpr size_t FAMILY_KEY         = 0x10;   // HIGH: std::string ("rf_abrahamic", "rf_eastern", "rf_pagan")
    constexpr size_t DISPLAY_NAME       = 0x30;   // HIGH: std::string ("Abrahamic")
    constexpr size_t GFX_KEY            = 0x50;   // HIGH: std::string
}

// =============================================================================
// CFaithDoctrine Struct Offsets (Round 13)
// Looked up via globals::FAITH_DOCTRINE_DB (CGameDatabase<CFaithDoctrineDatabase, CFaithDoctrine>)
// RTTI: 14CFaithDoctrine. Inherits CJominiScriptDbObject pattern.
// =============================================================================
namespace faith_doctrine {
    constexpr size_t VTABLE             = 0x00;   // HIGH: vtable pointer
    constexpr size_t NAME_KEY           = 0x10;   // HIGH: std::string, doctrine key (e.g. "doctrine_theocracy")
    constexpr size_t ID                 = 0x290;  // HIGH: int32, doctrine ID (used in membership check)
    // CFaithDoctrine is a static definition — it doesn't change at runtime.
    // Faiths hold references to doctrines; each doctrine belongs to a CFaithDoctrineGroup.
    // Groups include: marriage_type, divorce, clergy, etc.
    // The "has_doctrine" trigger checks faith->doctrine_container for this doctrine's ID.
    // FUN_05fb8120 compares each container element's +0x290 against the target doctrine ID.
    //
    // Doctrine group reference: plVar9[0x73] (offset 0x398) points to the doctrine's group.
    // Group max doctrines: *(group+0x168) (max count per group)
    // Group current list: *(group+0x30) (array), *(group+0x3c) (count)
}

// =============================================================================
// CFaithDoctrineGroup Struct Offsets (Round 13)
// Looked up via globals::FAITH_DOCTRINE_GROUP_DB
// RTTI: 19CFaithDoctrineGroup
// Each group allows a limited number of doctrines (e.g. one marriage doctrine per faith)
// =============================================================================
namespace faith_doctrine_group {
    constexpr size_t VTABLE             = 0x00;   // HIGH: vtable pointer
    constexpr size_t NAME_KEY           = 0x10;   // HIGH: std::string, group key (e.g. "doctrine_clerical_function_group")
    // Max doctrines per group enforced in FUN_052ac290:
    //   "Faith '%s' already has the allowed number (%i) of doctrines, from group '%s'"
}

// =============================================================================
// CRegiment Struct Offsets (Round 10-6)
// ~0x140 bytes. Looked up via globals::REGIMENT_DB (TPdxRefDatabase<CRegiment, 8>).
// Lookup: idx = ref & 0x00FFFFFF, entry = *(*(db+0x20) + 8 + idx*0x10),
//         verify *(entry+0x10) == ref
// =============================================================================
namespace regiment {
    constexpr size_t SUB_VTABLE         = 0x08;   // HIGH: ptr, secondary vtable (has IsAlive at vt+0x10)
    constexpr size_t ID                 = 0x10;   // HIGH: uint32, regiment ref ID (DB verification key)

    // 7 SMaAChunk sub-objects (stride 0x24, starting at +0x14)
    // Each chunk: +0x00=allocated_count(int32), +0x04=current_count(int32), +0x18=status_flag(int32)
    constexpr size_t CHUNK_BASE         = 0x14;   // HIGH: first SMaAChunk
    constexpr size_t CHUNK_STRIDE       = 0x24;   // HIGH: bytes per chunk (7 total)
    constexpr size_t CHUNK_COUNT        = 7;      // HIGH: number of chunks per regiment

    constexpr size_t MAA_TYPE_PTR       = 0x110;  // HIGH: CMenAtArmsType*, MaA type pointer
    constexpr size_t MAX_COUNT          = 0x120;  // HIGH: int32, max regiment size (MaxSize)
    constexpr size_t SOURCE_FLAG        = 0x130;  // HIGH: int32, 1=active troops (affects cost calc path)

    // Stationing/location: landed-title ID where this MaA regiment is stationed.
    // Resolve via LANDED_TITLE_DB (ad_db_lookup). HIGH — core dump ck3_20260421_195552:
    // distinct-owner regiments resolved cleanly to titles (Deheubarth duchy, Steyr barony,
    // Dundee barony, ...); ref 881 (Steyr) is well outside the troop-count range, ruling
    // out a count-field false positive. Duplicate copy at +0x160 (same value). (ck-readlane)
    constexpr size_t STATION_TITLE_ID   = 0x15c;  // HIGH: uint32, stationed holding/title ID (TPdxRef, mask 0xFFFFFF)
}

// =============================================================================
// CMercenaryCompany def-node fields (lane-query, 2026-10-18). Reached through
// globals::MERC_COMPANY_DB (hash_map; entry = the 0x70-byte node, id @ +0x10).
// Evidence: core dump ck3_20260625_152901_ivar867 (867.01.01), all 551 nodes:
// HOME_COUNTY resolves to a tier-2 LANDED_TITLE_DB entry 551/551 (c_fante,
// c_begho, ...); CAPTAIN_CHAR_ID resolves to a CHARACTER_DB entry 551/551;
// REGIMENT_IDS/COUNT resolve 1528/1528 ids in REGIMENT_DB (first regiment has
// an empty MaA key = the levy block, the rest are MaA types such as
// light_horsemen / pikemen_unit). HIRED_BY: Ghidra, FUN_062772f0 (the
// hire validate called by CHireMercenaryCompanyCommand_CanExecute @ 0x610a530)
// takes the "free" path only when *(company+0x48) == -1; all 551 read -1 in the
// 867.01.01 dump (no company hired yet), so the hired value is not observed.
// Name and hire cost are NOT here: the cost is computed per hirer by
// FUN_06108e60 over the regiment array (scripted values), not stored.
// See re/findings/re_hire_mercenary_wiring.md (lane-query addendum).
// =============================================================================
namespace merc_company {
    constexpr size_t HOME_COUNTY_ID     = 0x20;   // MEDIUM: u32 county title id (dump 551/551; FUN_06277980 reads it as the hire-range origin)
    constexpr size_t CAPTAIN_CHAR_ID    = 0x28;   // MEDIUM: u32 captain character id (dump 551/551)
    constexpr size_t REGIMENT_IDS       = 0x30;   // MEDIUM: ptr to u32[] REGIMENT_DB ids (FUN_06278d70 passes company+0x30 to the cost calc)
    constexpr size_t REGIMENT_COUNT     = 0x38;   // MEDIUM: u32 count of REGIMENT_IDS (capacity at +0x3c)
    constexpr size_t HIRED_BY           = 0x48;   // MEDIUM: u32, -1 = not under contract (decompile: FUN_062772f0); otherwise the hirer, unobserved
}

// =============================================================================
// CArmyRegiment Struct Offsets (ck-warstrength)
// The DEPLOYED regiment sub-unit listed in CArmy+0x18 (army_obj::REG_VEC). NOT CRegiment.
// Looked up via globals::ARMY_REGIMENT_DB (TPdxRefDatabase<CArmyRegiment, 8>):
//   idx = ref & 0x00FFFFFF; entry = *(*(db+0x20) + 8 + idx*0x10); verify *(entry+0x10) == ref.
// vtable image VA 0xbbb5460 (methods FUN_05bf7400.. in the CArmy 0x5bf7xxx neighbourhood).
// SOLDIER COUNTS: the +0x38 size sub-block is serialized first by FUN_05bf70c0 (save tag 0x2c7e
// in FUN_05bf75f0): word[0]=+0x38 (current/alive men), word[1]=+0x3c (max men). Empirically
// +0x38 <= +0x3c on every live regiment (CoreReader ck3_20260421_195552, 407 armies); per-army
// sums give realistic strengths (e.g. 512/532, 889/1050, 1491/1613). A depleted/retreating army
// reads low current vs full max (384 alive / 1299 max). HIGH confidence.
// =============================================================================
namespace army_regiment {
    constexpr size_t VTABLE             = 0x00;   // HIGH: ptr, CArmyRegiment vtable (image 0xbbb5460)
    constexpr size_t ID                 = 0x10;   // HIGH: uint32, ref ID (DB verification key, == Army+0x18 entry)
    constexpr size_t MEN_CURRENT        = 0x38;   // HIGH: int32, CURRENT (alive) soldiers in this regiment
    constexpr size_t MEN_MAX            = 0x3c;   // HIGH: int32, MAX soldiers (full establishment)
    constexpr size_t MORALE_A           = 0x40;   // MEDIUM: fixed-point (serialized tag 0x2915, FUN_0827a490)
    constexpr size_t MORALE_B           = 0x48;   // MEDIUM: fixed-point (serialized tag 0x383f)
    constexpr size_t COMMANDER_OR_REF1  = 0x140;  // MEDIUM: int32 ref (serialized tag 0x296a, always present)
    constexpr size_t REF2               = 0x144;  // MEDIUM: int32 ref (tag 0x29ae, -1=none)
    constexpr size_t REF3               = 0x148;  // MEDIUM: int32 ref (tag 0x3105, -1=none)
    // Army current soldiers = sum over Army+0x18 (skip id==0) of MEN_CURRENT after DB lookup.
}

// =============================================================================
// SMaAChunk Sub-Object Offsets (within CRegiment, 7 per regiment)
// Each 0x24 bytes, represents a troop deployment slot.
// =============================================================================
namespace maa_chunk {
    constexpr size_t ALLOCATED_COUNT    = 0x00;   // HIGH: int32, allocated troop count
    constexpr size_t CURRENT_COUNT      = 0x04;   // HIGH: int32, current/secondary count
    constexpr size_t STATUS_FLAG        = 0x18;   // HIGH: int32, if==3 AND current==0 -> use allocated
    // IsEmpty: *chunk == 0
    // GetCurrentCount: if (status != 3 || current != 0) return current; else return allocated
}

// =============================================================================
// CMenAtArmsType Offsets (Round 10-6)
// MaA type definition. Accessed via regiment::MAA_TYPE_PTR.
// Inherits from CJominiScriptDbObject (name at +0x10).
// =============================================================================
namespace maa_type {
    constexpr size_t DB_ID              = 0x08;   // HIGH: u32 MaA-type DB id (0..108); recruit ctor reads *(obj+8). Core-dump verified.
    constexpr size_t NAME_KEY           = 0x10;   // HIGH: std::string, MaA type name (e.g. "pikemen")
    // Upfront recruit buy_cost (int64 fixed-point /100000). HIGH — core-dump verified
    // (build e61ff264) across types: huscarl 115.2, landsknecht 90, longbowmen 66,
    // goedendag 45, gendarme 240, praetorian 180, cataphract 260 — exact CK3 vanilla
    // buy_cost values. Distinct from MAINT_GOLD_PER_STACK (+0x108, monthly upkeep).
    // The +0xF8 cost-fields array does NOT hold the buy cost. Read as int64_t, /100000.
    constexpr size_t RECRUIT_GOLD_COST  = 0x68;   // HIGH: int64 fp/100000, upfront recruit buy cost
    constexpr size_t BASE_COST_VALUE    = 0x270;  // HIGH: cost value passed to FUN_06670b40 for maintenance calc
    // Cost fields array at +0xF8..+0x140 (10 int64 values = 5 cost categories x 2 currency types)
    constexpr size_t COST_FIELDS_BASE   = 0xF8;   // HIGH: first of 10 int64 cost fields (stride 8)
    constexpr size_t COST_FIELDS_COUNT  = 10;     // HIGH: 5 cost categories x 2 (gold + treasury)
    // Per-stack monthly GOLD maintenance (int64 fixed-point /100000). Cost-fields index 2.
    // HIGH — core dump verified across MaA types: light_footmen 0.45, pikemen 0.90,
    // armored_footmen 1.20, mubarizun 1.44 — all match CK3 vanilla MaA maintenance values.
    // Total regiment maintenance ~= MAINT_GOLD_PER_STACK * (stacks alive) per CalcMaintenanceCost
    // (regiment::REGIMENT_MAINT_COST 0x0625cfc0); this field is the cheap-read per-stack base. (ck-readlane)
    constexpr size_t MAINT_GOLD_PER_STACK = 0x108; // HIGH: int64 fp/100000, monthly gold maintenance per stack
}

// =============================================================================
// CMaaOwnerLink Offsets (Round 10-6, within array at title_realm::MAA_OWNER_LINKS)
// Stride 0x38 per entry. Used by military strength calculator.
// =============================================================================
namespace maa_owner_link {
    constexpr size_t REG_REFS_PTR       = 0x20;   // HIGH: ptr, regiment refs array (uint32*)
    constexpr size_t REG_REFS_COUNT     = 0x2C;   // HIGH: int32, number of regiment refs in this link
    constexpr size_t ENTRY_STRIDE       = 0x38;   // HIGH: bytes per CMaaOwnerLink entry
}

// =============================================================================
// Military Stats Result Struct (from FUN_0629fed0 / MILITARY_STATS_COMPUTE)
// 8 categories at stride 0x20, each containing:
// R11-9 CORRECTED: fields at +0x08/+0x10 are quality (int64), not cost
// =============================================================================
namespace mil_stats {
    constexpr size_t MAX_SOLDIERS       = 0x00;   // HIGH: int32, max soldiers in category (R11-9 corrected order)
    constexpr size_t CURRENT_SOLDIERS   = 0x04;   // HIGH: int32, current soldiers in category (R11-9 corrected order)
    constexpr size_t QUALITY_CURRENT    = 0x08;   // HIGH: int64, quality-adjusted current value (R11-9: was CURRENT_COST)
    constexpr size_t QUALITY_MAX        = 0x10;   // HIGH: int64, quality-adjusted max value (R11-9: was MAX_COST)
    constexpr size_t REGIMENT_COUNT     = 0x18;   // HIGH: int32, regiment count in category
    constexpr size_t CATEGORY_STRIDE    = 0x20;   // HIGH: bytes per category
    constexpr size_t CATEGORY_COUNT     = 8;      // HIGH: total categories
    // R11-9 corrected categories:
    // 0 = Own Domain Troops (regiment query)
    // 1 = Own MaA + Levies (ruler_data+0x108, +0x2A8)
    // 2 = Holy Orders (ruler_data+0x138) — EXCLUDED from "max without hires"
    // 3 = Mercenaries (ruler_data+0x150) — EXCLUDED from "max without hires"
    // 4 = MaA by Holding (ruler_data+0x290)
    // 5 = Allies (FUN_061bde00 character collection)
    // 6 = Vassal Titles (FUN_0686c3d0 title iteration)
    // 7 = Garrison / Personal Troops (conditional on landed status)

    // DEPRECATED aliases (old names before R11-9 correction)
    constexpr size_t CURRENT_COST       = QUALITY_CURRENT; // DEPRECATED: use QUALITY_CURRENT
    constexpr size_t MAX_COST           = QUALITY_MAX;     // DEPRECATED: use QUALITY_MAX
}

// =============================================================================
// Holding Income Offset (Round 10-5, from domain income calculator)
// Accessed on holding objects resolved via title DB lookup
// =============================================================================
namespace holding {
    constexpr size_t CACHED_INCOME      = 0x300;  // HIGH: int64 fp, cached domain income from this holding
    constexpr size_t INCOME_SUB_STRUCT  = 0x1F8;  // HIGH: sub-struct base for detailed income calc (FUN_06122330)
}

// =============================================================================
// Income Modifier Indices (Round 10-5, passed to modifier evaluator FUN_062ae7a0)
// Used by INCOME_MODIFIER_CALC (FUN_06188af0) for income computation pipeline
// =============================================================================
namespace income_modifier_idx {
    constexpr uint32_t BASE_ADD         = 0x7A;   // HIGH: base income additive modifiers
    constexpr uint32_t BASE_MULT        = 0x7B;   // HIGH: income multiplicative modifiers
    constexpr uint32_t RULER_ADD        = 0x7C;   // HIGH: ruler-only additive modifiers
    constexpr uint32_t RULER_MULT       = 0x7D;   // HIGH: ruler-only multiplicative modifiers
    constexpr uint32_t VASSAL_CONTRIB   = 0x97;   // HIGH: vassal contribution multiplier
    constexpr uint32_t TRIBUTARY_CONTRIB = 0x98;   // HIGH: tributary contribution multiplier
    constexpr uint32_t BUILDING_BASE    = 0x1CE;  // HIGH: building modifier (base gold calc)
    constexpr uint32_t BUILDING_RULER   = 0x1D3;  // HIGH: building modifier (ruler variant)
}

// =============================================================================
// EHookStrength Enum (Round 10-4)
// Derived from CHasHookOnTrigger<N> template registrations in _INIT_2433.
// NOT a simple strong/weak binary -- encodes cooldown state too.
// =============================================================================
namespace hook_strength {
    constexpr int ANY_HOOK             = 0;   // HIGH: has_hook (any hook, any state)
    constexpr int USABLE_HOOK          = 1;   // HIGH: has_usable_hook (not on cooldown)
    constexpr int WEAK_HOOK            = 2;   // HIGH: has_weak_hook (strong does NOT count)
    constexpr int STRONG_HOOK          = 3;   // HIGH: has_strong_hook
    constexpr int STRONG_USABLE_HOOK   = 4;   // HIGH: has_strong_usable_hook (strong + not on cooldown)
}

// =============================================================================
// CHookTypeDatabase Layout (Round 10-4, at globals::HOOK_TYPE_DB)
// CGameDatabase<CHookTypeDatabase, SHookType> pattern.
// Lookup via MurmurHash3-32 (seed=0) on hook type key string.
// =============================================================================
namespace hook_type_db {
    constexpr size_t ARRAY_BASE         = 0x18;   // HIGH: ptr to inline array allocator area
    constexpr size_t FILE_WATCHER_VT    = 0x58;   // HIGH: CFileWatcherListener vtable
    constexpr size_t FILE_PATH          = 0x60;   // HIGH: std::string "common/hook_types"
    constexpr size_t INTERNAL_VT        = 0x88;   // HIGH: internal vtable (CGameDatabase<> variant)
    constexpr size_t HASH_TABLE         = 0x1B0;  // HIGH: ptr, hash table for name lookup
    constexpr size_t HASH_MASK          = 0x1BC;  // HIGH: uint32, AND mask for bucket index
    constexpr size_t MAX_PROBE          = 0x1C0;  // HIGH: uint32, max probe count
    constexpr size_t LOCK_OBJECT        = 0xED0;  // HIGH: lock structure
    constexpr size_t LOCK_FLAG          = 0xF10;  // HIGH: byte, lock acquired flag
    constexpr size_t FLAGS              = 0xF14;  // MEDIUM: flags
    constexpr size_t POOL_ALLOCATOR     = 0xEE8;  // MEDIUM: NPdxPoolAllocator<SHookType,false> vtable
    // Hash entry layout (stride 0x18):
    //   +0x04: byte valid (non-zero = occupied)
    //   +0x08: uint32 hash
    //   +0x10: ptr SHookType*
}

// =============================================================================
// CActiveHook Inferred Fields (Round 10-4)
// RTTI vtable at 0x0a6df628. NOT in a global ref DB -- stored per-character.
// One hook per owner-target pair. Strong hooks survive use (go on cooldown),
// weak hooks are consumed on use.
// Exact offsets TBD -- decompile funcs::HOOK_STRENGTH_STATE etc. to confirm.
// =============================================================================
namespace active_hook {
    // --- RTTI ---
    constexpr uintptr_t VTABLE_ADDR     = 0x0a6df628; // HIGH: CActiveHook RTTI vtable (adjusted)
    // --- Inferred fields (types confirmed, offsets need decompilation) ---
    // hook_type:       SHookType*   (from has_hook_of_type trigger, add_hook { type = X })
    // target_char:     uint32       (from add_hook { target = Y })
    // owner_char:      uint32       (implied by per-character storage)
    // is_strong:       bool         (from EHookStrength, IsStrongHook/IsWeakHook UI properties)
    // expiry_date:     int32        (from HasExpirationDate, GetExpirationDate, time_to_hook_expiry)
    // cooldown_date:   int32        (from has_usable_hook vs has_hook distinction)
    // backing_secret:  uint32       (from add_hook { secret = Z }, has_hook_from_secret trigger)
    // is_spent:        bool         (from IsSpent UI property)
}

// =============================================================================
// CActiveSecret Struct Layout (~0xA8 bytes, Round 10-4 + Round secret-RE)
// TPdxRefDatabase<CActiveSecret, 8> — accessed via CSecretManager.
// Access chain: *(GAME_STATE_ROOT) → *(+0xa0) + 0xCED0 → CSecretManager+0x20 → RefDB
// RefDB pattern: +0x20=table, +0x2c=capacity, entries at table+8+id*0x10
// Constructor: Ghidra 0x06428f60 (InitActiveSecret), allocator 0x06430400
// =============================================================================
namespace active_secret {
    // --- RTTI ---
    constexpr uintptr_t VTABLE_ADDR     = 0x0a65e448; // HIGH: CActiveSecret RTTI vtable (adjusted)
    // --- Confirmed field offsets (constructor decompilation + 16-instance runtime validation) ---
    constexpr size_t TOTAL_SIZE         = 0xA8;   // HIGH: approximate struct size
    constexpr size_t SECRET_ID          = 0x10;   // HIGH: uint32, RefDB ID (lower 32 bits)
    constexpr size_t GENERATION         = 0x14;   // HIGH: uint32, RefDB generation (upper 32 bits)
    constexpr size_t SECRET_TYPE_PTR    = 0x18;   // HIGH: CSecretType* (type definition)
    constexpr size_t CONTEXT_OBJ        = 0x20;   // MEDIUM: ptr, faith for crypto, relation for lover, sentinel otherwise
    constexpr size_t TARGET_SCOPE_TYPE  = 0x28;   // HIGH: uint16, 4=character, 0=none
    constexpr size_t TARGET_SCOPE_SUB   = 0x2A;   // HIGH: uint16, usually 0
    constexpr size_t TARGET_DATA        = 0x30;   // HIGH: uint64, char ID for character-targeted secrets
    constexpr size_t OWNER_CHAR_ID      = 0x38;   // HIGH: uint32, owning character (set by FUN_06429450)
    constexpr size_t KNOWER_ARRAY       = 0x40;   // HIGH: ptr, packed knower entries (char IDs + metadata)
    constexpr size_t KNOWER_COUNT       = 0x48;   // HIGH: uint32, number of knowers
    constexpr size_t KNOWER_CAPACITY    = 0x4C;   // HIGH: uint32, knower array capacity
    constexpr size_t PARTICIPANT_LIST   = 0x58;   // MEDIUM: ptr, participant entries
    constexpr size_t PARTICIPANT_COUNT  = 0x60;   // MEDIUM: uint32, number of participants
    constexpr size_t FLAGS              = 0xA0;   // MEDIUM: uint16, 0 in early-game samples
}

// =============================================================================
// CSecretManager (Round 10-4 + Round secret-RE)
// Access: *(*(GAME_STATE_ROOT) + 0xa0) + SECRET_MGR_OFFSET
// =============================================================================
namespace secret_manager {
    constexpr uintptr_t VTABLE_ADDR     = 0x0abf5020; // HIGH: CSecretManager RTTI vtable (adjusted)
    constexpr size_t SECRET_MGR_OFFSET  = 0xCED0; // HIGH: offset from *(GSR+0xa0) to CSecretManager
    constexpr size_t REFDB_PTR          = 0x20;   // HIGH: ptr to TPdxRefDatabase<CActiveSecret,8>
}

// =============================================================================
// CSecretType Key Fields (Round secret-RE, from SECRET_TYPE_DB enumeration)
// 18 types: secret_lover, secret_crypto_religionist, secret_deviant, etc.
// =============================================================================
namespace secret_type {
    constexpr size_t INDEX              = 0x08;   // HIGH: uint32, sequential index (0-17)
    constexpr size_t NAME_HASH          = 0x0C;   // HIGH: uint32, MurmurHash3-32 of key string
    constexpr size_t KEY_STRING         = 0x10;   // HIGH: std::string, type name (e.g. "secret_lover")
    constexpr size_t SOURCE_FILE        = 0x40;   // HIGH: std::string, definition file path
    constexpr size_t TYPE_FLAGS         = 0x50;   // MEDIUM: uint32, bitfield (varies per type)
    constexpr size_t ON_EXPOSE_CB       = 0x58;   // HIGH: std::string, on_expose callback name
    constexpr size_t CRIME_PARAM_LO     = 0x78;   // MEDIUM: uint32, crime classification parameter
    constexpr size_t CRIME_PARAM_HI     = 0x7C;   // MEDIUM: uint32, crime classification parameter
}

// =============================================================================
// ESecretCrimeStatus Enum (Round 10-4)
// Used by CIsShunnedOrCriminalForTrigger<N>.
// =============================================================================
namespace secret_crime_status {
    constexpr int SHUNNED              = 0;   // HIGH: secret is shunned for target
    constexpr int CRIMINAL             = 1;   // HIGH: secret is criminal for target
    constexpr int SHUNNED_OR_CRIMINAL  = 2;   // HIGH: combined check
}

// =============================================================================
// CAIWatchWindow Struct Offsets (Round 12)
// The AI Watch Window is CK3's debug UI for inspecting AI decision-making.
// It contains CPdxArrays of evaluation info structs for each AI subsystem.
//
// CPdxArray layout (0x18 bytes per header):
//   +0x00: data_ptr      (8 bytes, pointer to element array)
//   +0x08: capacity      (uint32)
//   +0x0C: count         (uint32)
//   +0x10: allocator_ptr (8 bytes)
// =============================================================================
namespace aiwatch_struct {
    constexpr size_t VTABLE                      = 0x000;  // HIGH: vtable pointer
    constexpr size_t CHARACTER_REF               = 0x0D8;  // HIGH: packed uint32 character reference
    constexpr size_t SAI_VALUE_INFO_ARRAY        = 0x0F8;  // HIGH: CPdxArray of SAIValueInfo (16-byte elements)
    constexpr size_t SAI_STRATEGY_INFO_ARRAY     = 0x110;  // HIGH: CPdxArray of SAIStrategyInfo (16-byte elements)
    constexpr size_t SAI_SCHEME_TYPE_INFO_ARRAY  = 0x128;  // HIGH: CPdxArray of SAISchemeTypeInfo (16-byte elements)
    constexpr size_t SAI_CB_TYPE_INFO_ARRAY      = 0x1B8;  // HIGH: CPdxArray of SAICBTypeInfo
    constexpr size_t WATCHED_CHARS_ARRAY         = 0x1D0;  // HIGH: CPdxArray of watched character refs
    constexpr size_t SAI_ACTIVITY_INFO_ARRAY     = 0x1E8;  // HIGH: CPdxArray of SAIActivityInfo (0x950-byte elements!)
}

// =============================================================================
// CPdxArray Generic Layout (Round 12)
// Used throughout CK3 for dynamic arrays. Documented here for reference.
// =============================================================================
namespace pdx_array {
    constexpr size_t DATA_PTR                    = 0x00;   // HIGH: pointer to element data
    constexpr size_t CAPACITY                    = 0x08;   // HIGH: uint32, allocated element count
    constexpr size_t COUNT                       = 0x0C;   // HIGH: uint32, current element count
    constexpr size_t ALLOCATOR_PTR               = 0x10;   // HIGH: pointer to allocator vtable
    constexpr size_t HEADER_SIZE                 = 0x18;   // total size of array header
    constexpr size_t CB_OPTION_ENTRY_STRIDE      = 0x98;   // HIGH: each CB option entry is 0x98 bytes
    // Allocator vtable used for CB option entry arrays (Ghidra 0x0b9a24a0 - 0x100000)
    // CDynArray allocator obj = single pointer to this vtable; vtable[2](+0x10)=alloc, vtable[3](+0x18)=free
    constexpr uintptr_t CB_OPTION_ALLOCATOR_VTABLE = 0x0a9a24a0;  // HIGH: allocator vtable for CB option CDynArray
}

// =============================================================================
// CDynasty Struct Offsets (from Dynasty_GetName, prestige level, legacy perk accessors)
// =============================================================================
namespace dynasty_obj {
    constexpr size_t ID                  = 0x10;   // HIGH: uint32, dynasty ID (DB lookup validates *(obj+0x10)==id)
    constexpr size_t NAME                = 0x80;   // HIGH: std::string, dynasty name (Dynasty_GetName returns obj+0x80)
    constexpr size_t HEAD_CHAR_ID        = 0xCC;   // MEDIUM-HIGH: uint32, dynasty head character ID
    constexpr size_t CUSTOM_NAME         = 0x110;  // MEDIUM: std::string, custom dynasty name (when HAS_CUSTOM_NAME set)
    constexpr size_t HOUSES_ARRAY        = 0x118;  // MEDIUM-HIGH: ptr, array of dynasty house objects
    constexpr size_t HOUSES_COUNT        = 0x124;  // MEDIUM-HIGH: int32, number of houses in dynasty
    constexpr size_t HAS_CUSTOM_NAME     = 0x130;  // MEDIUM: bool, if set use CUSTOM_NAME instead of NAME
    constexpr size_t PRESTIGE            = 0x158;  // HIGH: int64, fixed-point /100000, dynasty prestige (renown)
    constexpr size_t PRESTIGE_LEVEL      = 0x160;  // HIGH: uint32, dynasty prestige level (0-10)
    constexpr size_t LEGACY_PERKS_ARRAY  = 0x168;  // MEDIUM-HIGH: ptr, array of CDynastyLegacyPerk* (8-byte ptrs)
    constexpr size_t LEGACY_PERKS_COUNT  = 0x174;  // HIGH: int32, number of unlocked legacy perks
}

// CDynastyLegacyPerk — element of dynasty_obj::LEGACY_PERKS_ARRAY (8-byte ptrs).
// #dynasty_house (2026-06-24, live-validated 105 perks): universal Jomini db_object layout.
// Keys are '<track>_legacy_<N>' (105/105: 21 tracks x 5 ranks).
namespace dynasty_legacy_perk {
    constexpr size_t DB_ID    = 0x08;  // HIGH: uint32, db id (0..104)
    constexpr size_t NAME_HASH = 0x0C; // HIGH: uint32, name hash
    constexpr size_t NAME_KEY = 0x10;  // HIGH: std::string, perk key '<track>_legacy_<N>'
}

// =============================================================================
// CDynastyHouse Struct Offsets (from House_GetDynastyID decompilation)
// =============================================================================
namespace dynasty_house_obj {
    constexpr size_t ID                  = 0x10;   // HIGH: uint32, house ID (DB lookup validates *(obj+0x10)==id)
    constexpr size_t DYNASTY_ID          = 0x24;   // HIGH: uint32, parent dynasty ID (House_GetDynastyID returns *(obj+0x24))
}

// =============================================================================
// CSubjectContract Struct Offsets (vassal contract obligation system)
// Verified via: RTTI (16CSubjectContract), serializer/deserializer decompilation,
// runtime core dump validation (3285 contracts in 867 AD save)
// Struct size: 0xD8 (216 bytes), stored in VASSAL_CONTRACT_DB (0x0bed7700)
// See: re/findings/re_vassal_contract.md for full analysis
// =============================================================================
namespace subject_contract {
    // --- Identity & vtable ---
    constexpr size_t VTABLE              = 0x00;   // HIGH: ptr, primary vtable (base-rel 0xbc08c88)
    constexpr size_t REF_KEY             = 0x08;   // HIGH: uint64, TPdxRef database key
    constexpr size_t VTABLE_SECONDARY    = 0x10;   // HIGH: ptr, secondary MI vtable

    // --- Contract parties ---
    constexpr size_t LIEGE               = 0x20;   // HIGH: CCharacter*, liege (serializer tag 0x29ae)
    constexpr size_t SUBJECT             = 0x28;   // HIGH: CCharacter*, subject/vassal (serializer tag 0x292d)
    constexpr size_t TAX_SLOT            = 0x30;   // HIGH: TPdxNullObject<CTaxSlot>* (RTTI confirmed)

    // --- Obligation types (array of CSubjectContractType*) ---
    // Each type defines an obligation category (e.g., "feudal_government_taxes")
    constexpr size_t OBL_TYPES_PTR       = 0x38;   // HIGH: CSubjectContractType**, ptr to array of type pointers
    constexpr size_t OBL_TYPES_CAPACITY  = 0x40;   // HIGH: int32, array capacity
    constexpr size_t OBL_TYPES_COUNT     = 0x44;   // HIGH: int32, number of obligation types
    constexpr size_t OBL_TYPES_ALLOC     = 0x48;   // HIGH: ptr, allocator for types array (CPdxHybridInlineBufferAllocator)
    constexpr size_t PRIMARY_TYPE        = 0x58;   // HIGH: CSubjectContractType*, same as types[0]

    // --- Obligation levels (uint8 array, parallel to types array) ---
    // Level values: 0=None/default, 1=Low, 2=Normal, 3=High, 4=Extortionate (varies by type)
    constexpr size_t OBL_LEVELS_PTR      = 0x68;   // HIGH: uint8*, ptr to array of obligation level values
    constexpr size_t OBL_LEVELS_CAPACITY = 0x70;   // HIGH: int32, array capacity
    constexpr size_t OBL_LEVELS_COUNT    = 0x74;   // HIGH: int32, number of obligation levels (= OBL_TYPES_COUNT)
    constexpr size_t OBL_LEVELS_ALLOC    = 0x78;   // HIGH: ptr, allocator for levels array

    // --- Other serialized fields (partially identified) ---
    constexpr size_t UNKNOWN_INT_A0      = 0xA0;   // MEDIUM: int32, default -1 (serializer tag 0x311f)
    constexpr size_t UNKNOWN_COMPOUND_A4 = 0xA4;   // MEDIUM: 8 bytes, serialized compound (tag 0x6b5)
    constexpr size_t UNKNOWN_COMPOUND_AC = 0xAC;   // MEDIUM: 8 bytes, serialized compound (tag 0x3b04)
    constexpr size_t UNKNOWN_INT_B4      = 0xB4;   // MEDIUM: int32 (serializer tag 0x285f)
    constexpr size_t FLAG_B8             = 0xB8;   // MEDIUM: byte (serializer tag 0x313d)
    constexpr size_t FLAG_B9             = 0xB9;   // MEDIUM: byte (serializer tag 0x2efd)
    constexpr size_t FLAG_BA             = 0xBA;   // MEDIUM: byte (serializer tag 0x40a2)
    constexpr size_t RESOURCE_C0         = 0xC0;   // MEDIUM: int64, default -100000 (fixed-point gold? tag 0x3b4f)
    constexpr size_t UNKNOWN_PTR_C8      = 0xC8;   // MEDIUM: ptr (serializer tag 0x3c78)
    constexpr size_t FLAG_D0             = 0xD0;   // MEDIUM: byte (serializer tag 0x3e1a)

    constexpr size_t STRUCT_SIZE         = 0xD8;   // HIGH: confirmed by contiguous allocation (delta between entries)
}

// =============================================================================
// CSubjectContractType Struct Offsets (obligation type definition)
// Each type defines one obligation category within a contract (e.g., "feudal_government_taxes")
// Stored in CGameDatabase<CSubjectContractTypeDatabase, CSubjectContractType>
// =============================================================================
namespace subject_contract_type {
    constexpr size_t VTABLE              = 0x00;   // HIGH: ptr, vtable
    constexpr size_t DB_INDEX            = 0x08;   // HIGH: uint32, index in type database
    constexpr size_t KEY_HASH            = 0x0C;   // HIGH: uint32, hash of key name string
    // Key name is a std::string (libstdc++ SSO layout):
    constexpr size_t KEY_NAME_PTR        = 0x10;   // HIGH: char*, _M_p — pointer to string data
    constexpr size_t KEY_NAME_LENGTH     = 0x18;   // HIGH: size_t, string length
    constexpr size_t KEY_NAME_CAPACITY   = 0x20;   // HIGH: size_t, string capacity
}

// =============================================================================
// Event Weight / MTTH Calculation Engine (ck-t0z8)
//
// Weight evaluation pipeline: AI_CHANCE_COMPUTE checks direct override at
// option+0x318, else evaluates script block at option+0x28 via fixed-point
// arithmetic (÷100000). No exp()/log() in binary — MTTH is NOT exponential.
// Selection behaviors: weighted_random, first_valid, random_on_action.
// All evaluation is vtable-dispatched through Paradox data binding framework.
// =============================================================================
namespace event_weight {
    // Script evaluation functions for weight computation
    constexpr uintptr_t SCRIPT_FIXEDPT_MULT     = 0x07572400;  // HIGH: fn(block) → int64 fixed-point multiply
    constexpr uintptr_t SCRIPT_BASE_EVAL        = 0x074713b0;  // HIGH: fn(p1,p2) → int64 base value eval (ck-e58l.20)
    constexpr uintptr_t AI_CHANCE_DIRECT_READ   = 0x07445520;  // HIGH: fn() → read direct override value (ck-e58l.20)

    // AI event option evaluation (higher-level wrapper)
    constexpr uintptr_t AI_EVENT_OPTION_EVAL_INNER = 0x05d4d540;  // HIGH: fn(name,p2,p3,idx,flags) → int32 (ck-e58l.20)

    // CJominiMeanTimeToHappen RTTI
    constexpr uintptr_t MTTH_VTABLE             = 0x0be08470;  // HIGH: CJominiMeanTimeToHappen vtable
    constexpr uintptr_t MTTH_TYPEINFO           = 0x0be085d8;  // HIGH: typeinfo
    constexpr uintptr_t MTTH_EVAL_BASE          = 0x07470570;  // HIGH: base class eval method (vtable[1])
    constexpr uintptr_t MTTH_VALIDATE           = 0x07470dd0;  // HIGH: validation chain (vtable[7])

    // CAIMTTHChance RTTI (inherits CJominiMeanTimeToHappen)
    constexpr uintptr_t AIMTTH_VTABLE           = 0x0be084c0;  // HIGH: CAIMTTHChance vtable
    constexpr uintptr_t AIMTTH_TYPEINFO         = 0x0be085f0;  // HIGH: typeinfo
    constexpr uintptr_t AIMTTH_CACHE_REHASH     = 0x073735c0;  // HIGH: hash table rehash (vtable[1] override) (ck-e58l.20)

    // COnActionManager dispatch
    constexpr uintptr_t ON_ACTION_MGR_VTABLE    = 0x0bb4cdb0;  // HIGH: COnActionManager vtable
    constexpr uintptr_t ON_ACTION_DISPATCH      = 0x05f9d0f0;  // HIGH: fn(mgr, scope) → queue event
    constexpr uintptr_t ON_ACTION_FIRE_EVENT    = 0x05e9d3e0;  // HIGH: fn(mgr, scope, on_action, char) → ptr (ck-e58l.20)

    // On-action dispatch variant used by WAR_CREATE_CORE (ck-e58l.5, ck-e58l.16).
    // Called as: FUN_062c1b50(mgr=_DAT_0c0b0768, action_id=0x55, scope, game_root+8)
    // immediately after the CB.on_declaration effect block executes. Dispatcher
    // indexes *(mgr+0x38) + action_id*8 to get the listener-chain head.
    // Distinct from ON_ACTION_DISPATCH above (different arg order + arity).
    constexpr uintptr_t ON_ACTION_DISPATCH_BY_ID   = 0x061c1b50; // HIGH: (mgr, id, scope, gs+8) — Ghidra: OnActionManager_Dispatch (ck-e58l.20) (ck-audit-B: dedup of stress_funcs::ON_STRESS_LEVEL_DECREASE mislabel)
    constexpr uintptr_t ON_ACTION_MGR_WAR_SINGLETON = 0x0bfb0768; // HIGH: raw addr (Ghidra VA 0x0c0b0768 = _DAT_0c0b0768); core-dump verified ck-e58l.16

    // On-action manager (heap, *0x0bfb0768) struct fields (ck-yxlq):
    //   mgr+0x38: ptr to action descriptor table (array of 8B descriptor ptrs)
    //   mgr+0x40: u32 action count (observed 0xC6 = 198 in 867-AD core dump)
    //   mgr+0x340/0x348/0x350: default-handler / sibling-set / final-stage
    //                           (mirrors per-descriptor +0x340/+0x348/+0x350)
    constexpr size_t ON_ACTION_MGR_TABLE_PTR_OFFSET   = 0x38; // HIGH (ck-e58l.16)
    constexpr size_t ON_ACTION_MGR_TABLE_COUNT_OFFSET = 0x40; // HIGH: u32 (ck-yxlq)

    // On-action descriptor (entry at *(table[id])); heap-allocated, vtable-bearing.
    // Verified via core-dump probe of table[0x55] = "on_war_started" (ck-yxlq).
    namespace on_action_descriptor {
        constexpr size_t VTABLE              = 0x000; // HIGH (ck-yxlq)
        constexpr size_t NAME_STRING         = 0x008; // HIGH: std::string action name (e.g. "on_war_started")
        constexpr size_t SOURCE_FILE_STRING  = 0x030; // HIGH: std::string source file path
        constexpr size_t PRIMARY_HANDLER     = 0x340; // HIGH: callback ptr; called first
        constexpr size_t SECONDARY_SET       = 0x348; // HIGH: recursive next-stage on-action set
        constexpr size_t FINAL_HANDLER       = 0x350; // HIGH: runs after primary, before recursion
    }

    // War-lifecycle on-action IDs. 0x55 confirmed by ck-e58l.16; the others
    // are runtime-verified via descriptor-string reads in ck-yxlq. NOTE: these
    // IDs are NOT stable across modded games or future-version content
    // changes — for patch-resilient code, look up by name via descriptor+0x08.
    constexpr uint32_t  ON_ACTION_ID_WAR_TRANSFERRED       = 0x42; // HIGH (ck-yxlq): on_war_transferred
    constexpr uint32_t  ON_ACTION_ID_JOIN_WAR_AS_SECONDARY = 0x43; // HIGH (ck-yxlq): on_join_war_as_secondary
    constexpr uint32_t  ON_ACTION_ID_WAR_CREATED   = 0x55;       // HIGH: id fired from WAR_CREATE_CORE Block I = `on_war_started` (ck-e58l.16)
    constexpr uint32_t  ON_ACTION_ID_WAR_WON_ATTACKER      = 0x56; // HIGH (ck-yxlq): on_war_won_attacker
    constexpr uint32_t  ON_ACTION_ID_WAR_WON_DEFENDER      = 0x57; // HIGH (ck-yxlq): on_war_won_defender
    constexpr uint32_t  ON_ACTION_ID_WAR_WHITE_PEACE       = 0x58; // HIGH (ck-yxlq): on_war_white_peace
    constexpr uint32_t  ON_ACTION_ID_WAR_INVALIDATED       = 0x59; // HIGH (ck-yxlq): on_war_invalidated
    constexpr uint32_t  ON_ACTION_ID_MIGRATION_WAR_END     = 0xBC; // HIGH (ck-yxlq): on_migration_war_end

    // Per-character post-declare notify, called twice (atk then def) in WAR_CREATE_CORE
    // Block M. Likely fires character-scope on-actions (on_war_started@attacker, etc.).
    constexpr uintptr_t WAR_CHAR_POST_DECLARE_NOTIFY = 0x065ca320; // HIGH: FUN_066ca320 (ck-e58l.5)

    // Compiled Clausewitz effect block executor — runs CB.on_declaration block
    // in WAR_CREATE_CORE Block I (and delegated to from DIRECT_PATH_SCRIPT_RUNNER
    // for all 4 script slots). Generic effect runner over compiled CEffectEntry trees.
    constexpr uintptr_t CLAUSEWITZ_EFFECT_EXECUTOR = 0x0740aaf0; // HIGH: FUN_0750aaf0 - 0x100000: generic Clausewitz effect-block executor (ck-1ps.5) (ck-audit-B)

    // Post-declare game-state update (invoked between validity check and
    // active-wars append — purpose inferred: map/dynasty/UI refresh).
    constexpr uintptr_t WAR_POST_DECLARE_GAME_UPDATE = 0x06dc8ee0; // MEDIUM: FUN_06dc8ee0 (ck-e58l.5)

    // CJominiOnActionList
    constexpr uintptr_t ON_ACTION_LIST_VTABLE   = 0x0be08c98;  // HIGH: vtable

    // CJominiEffect — base class for all 604 effect types.
    // Execute is vtable slot [23] at +0xB8 (corrected ck-e58l.15; ck-n18o's
    // slot-[7] claim was wrong — slot [7] is GetScopeMask, see
    // JOMINI_EFFECT_GET_SCOPE_MASK below). Signature:
    //   uint64_t Execute(CJominiEffect* this, void* scope_context)
    constexpr uintptr_t JOMINI_EFFECT_VTABLE       = 0x0be00d98;  // HIGH: CJominiEffect base vtable (25 slots)

    // CStartWarEffect — war creation effect
    // Vtable at base+0x0ba2f088, 25 slots (per ck-e58l.15; ck-pu05's 16-slot
    // dump was truncated). Slot layout (ck-pu05, ck-e58l.15):
    //   [0]=dtor [1]=del-dtor [2]=shared [3]=shared-dispatch [4,5]=shared
    //   [6]=0x052a40d0 type-disc (ret 4)
    //   [7]=0x052a40e0 STUB (ret 8) — GetScopeMask helper, NOT Execute
    //   [9]=0x052a0fc0 SCRIPT-LOAD RESOLVER (not the executor — ck-1ps.5 was WRONG)
    //   [10]=0x052a40f0 post-validate (iterates CB-option array at self+0x1d8)
    //   [23]=0x052a11a0 CreateWar — actual executor (corrects ck-n18o slot-[7] claim)
    constexpr uintptr_t START_WAR_EFFECT_VTABLE    = 0x0ba2f088;  // HIGH: CStartWarEffect vtable (25 slots)
    // Ghidra symbols (ck-audit-B): CStartWarEffect_Resolve / _PostVal / _CreateWar
    constexpr uintptr_t START_WAR_EFFECT_RESOLVE   = 0x052a0fc0;  // HIGH: vtable[9] — script-load interaction resolver (Ghidra: CStartWarEffect_Resolve) (ck-pu05, CORRECTS ck-1ps.5)
    constexpr uintptr_t START_WAR_EFFECT_POST_VAL  = 0x052a40f0;  // HIGH: vtable[10] — post-validate CB options (Ghidra: CStartWarEffect_PostVal) (ck-pu05)
    constexpr uintptr_t START_WAR_EFFECT_CREATE_WAR = 0x052a11a0; // HIGH: vtable[23] war-creating function (Ghidra: CStartWarEffect_CreateWar). IsRuler + truce + CB-option validation then calls WAR_CREATE_CORE. (ck-pu05) (ck-e58l.20) (ck-audit-B: dedup of former START_WAR_EFFECT_EXECUTE_V23 alias — same address 0x052a11a0)

    // CStartWarEffect self-struct offsets (used by CREATE_WAR at 0x053a11a0):
    //   +0x60  std::string  CB type name (script-time, for RESOLVE hash lookup)
    //   +0x80  void*        Resolved CCasusBelliType* (cached by RESOLVE at vtable[9])
    //   +0x98               Target scope evaluator (resolves defender via FUN_07461f50)
    //   +0x140              Claimant scope evaluator
    //   +0x1d8 array*       CB option entries (stride 0xa8, count at +0x1e4)

    // declare_war special_interaction dispatch (ck-pu05):
    //   The declare_war interaction has enable_effects=0 (ck-1ps.7), so its
    //   script effect pipeline is bypassed entirely. Actual CWar creation
    //   happens via this dedicated handler, invoked indirectly by the
    //   engine's interaction-type registry (table address not yet pinned
    //   statically — candidate mechanism is a map inside the CB/interaction
    //   DB at global _DAT_0bfe39f8 whose interaction_ht offsets match).
    //   Handler signature: void(WarRequest* req, InteractionContext* ctx)
    //     ctx+0x2d8 = actor char id (interaction_ctx::ACTOR_CHAR_REF)
    //     ctx+0x2dc = CB option / target char (interaction_ctx::CB_OPTION_INDEX)
    //     req+0x08  = CCasusBelliType* (pre-resolved)
    //     req+0x10  = embedded CB-params array (title refs)
    //     req+0x28  = claimant char id (0xFFFFFFFF if none)
    //   Calls WAR_CREATE_CORE with no validation (upstream gates already ran).
    constexpr uintptr_t DECLARE_WAR_SPECIAL_HANDLER = 0x065db970; // HIGH: tiny (22 lines) special_interaction handler for declare_war (ck-pu05)

    // Sibling WAR_CREATE_CORE callers (5 remaining — full set of 6 total via Ghidra xref filtered to UNCONDITIONAL_CALL):
    constexpr uintptr_t SCOPE_EFFECT_START_WAR    = 0x050667f0;  // MEDIUM: scope-based start_war effect runner (likely CAiStartBestWarEffect) (ck-pu05)
    constexpr uintptr_t AI_BEST_CB_START_WAR      = 0x05fcf1b0;  // MEDIUM: AI CB-iterating war starter, reads faith from param_1+0x20 (likely holy-war AI path) (ck-pu05) (ck-e58l.20)
    constexpr uintptr_t INVASION_SPECIAL_HANDLER  = 0x06186f90;  // MEDIUM: command-level handler, no claimant (0xFFFFFFFF), CB from global _DAT_0bfe39f8+0xf30 (likely invasion_war / no-claimant path) (ck-pu05) (ck-e58l.20)
    constexpr uintptr_t SCRIPTED_FULL_WAR_STARTER = 0x065ddf60;  // MEDIUM: heavy setup with participant arrays (+0x40 atk, +0x58 def), title list (+0xd0), army pre-assignment (likely CStartGhwWarEffect / CFactionStartWarEffect) (ck-pu05)

    // Null-war sentinel constructor (called once at startup):
    constexpr uintptr_t WAR_NULL_SENTINEL_CTOR    = 0x066001d0;  // HIGH: initializes _g_NullWar_sentinel at _DAT_0bfd8890 via memset + CWar_Constructor (ck-pu05) (ck-e58l.20)

    // ─────────────────────────────────────────────────────────────────────
    // Interaction & effect registries (ck-e58l.7)
    //
    // Static scan (re/scripts/interaction/scan_registries.py) confirms:
    // there is NO flat function-pointer array registry in .data.rel.ro.
    // Handler addresses appear ONLY inside per-class vtables, confirming
    // hypothesis H2 from ck-pu05 §10 (and refuting H1, "flat fnptr table").
    //
    // The single script-name "registry" is the hash table owned by
    // INTERACTION_CB_DB_ROOT. Its VALUES are pointers to per-class
    // singleton instances; dispatch calls the class-specific vtable slot.
    // Table base at INTERACTION_CB_DB_ROOT + interaction_ht::TABLE_OFFSET
    // (0x1b0), cap-mask at +0x1bc, sentinel at +0x1c0 — same structure
    // that CStartWarEffect::vtable[9] probes via FNV-1a (ck-e58l.2 §1.3).
    constexpr uintptr_t INTERACTION_CB_DB_ROOT    = 0x0bee39f8;  // HIGH: _DAT_0bfe39f8 — shared registry root for special_interactions, CBs, and CEffectEntry<T> (ck-pu05, ck-e58l.2, ck-e58l.7) (ck-e58l.20)

    // Per-class dispatch — special_interaction subclasses (ck-e58l.7)
    // Each concrete interaction type has its own vtable. The script-facing
    // name hashes to a pointer in the INTERACTION_CB_DB_ROOT hash table,
    // and the engine calls the per-class handler through that vtable.
    // Addresses below are Ghidra virtual (image base 0x00100000 already
    // applied — raw would be VA - 0x100000).
    constexpr uintptr_t CWARDECLARATION_VTABLE       = 0x0bd544f8;  // HIGH: CWarDeclaration primary vtable. Handler DECLARE_WAR_SPECIAL_HANDLER (=0x065db970 raw, 0x066db970 Ghidra) appears at vtable[9]. Typeinfo at 0x0bd54568 → name string at 0x08bd8119 ("15CWarDeclaration") (ck-e58l.7)
    constexpr uintptr_t CWARDECLARATION_HANDLER_SLOT = 9;           // HIGH: vtable slot index for the special_interaction handler (ck-e58l.7)
    constexpr uintptr_t CMIGRATEINTERACTION_VTABLE   = 0x0bca22d0;  // HIGH: CMigrateInteraction primary vtable. Handler INVASION_SPECIAL_HANDLER (=0x06286f90 Ghidra = 0x06186f90 raw, originally labelled "invasion/murder" in ck-pu05 §3 caller #4) appears at vtable[11]. Typeinfo at 0x0bca2340 → name string at 0x08b227a0 ("19CMigrateInteraction") (ck-e58l.7)
    constexpr uintptr_t CMIGRATEINTERACTION_HANDLER_SLOT = 11;      // HIGH: vtable slot index for the special_interaction handler in this class (ck-e58l.7)

    // NOTE: the CWarDeclaration handler slot (9) differs from the
    // CMigrateInteraction handler slot (11). This is expected: the classes
    // share a set of base-class virtual methods (slots 2, 4, 6, 7 at
    // matching offsets hold identical pointers in both vtables) but each
    // derived class adds its own class-specific vfuncs. There is no
    // single universal "SPECIAL_INTERACTION_HANDLER_SLOT" — dispatch is
    // resolved by the concrete class's vtable, not a fixed offset.

    // Effect-name "registry" (ck-e58l.7) — same hash-table mechanism.
    //   The effect-name registry has the same topology as the special-
    //   interaction registry: the registration table IS the hash table
    //   at INTERACTION_CB_DB_ROOT + interaction_ht::TABLE_OFFSET. Each
    //   entry's value is a pointer to a per-class CEffectEntry<T>
    //   singleton (or a concrete-class instance that exposes a
    //   CEffectEntry secondary vtable). No standalone fnptr-array exists.
    //   There is therefore no separate EFFECT_NAME_REGISTRY address to
    //   record: it is the SAME table as the special_interaction registry,
    //   keyed by script-facing name.
    //
    // Open: ck-pu05 §10 OQ1 and ck-e58l.2 §9 OQ1 asked for an address.
    // The resolution is: there is no separate address; both registries
    // share INTERACTION_CB_DB_ROOT. See re/findings/re_interaction_effect_registries.md.

    // CPendingCharacterInteraction — deferred interaction entries
    // DB entries are 0x5c8 bytes in TPdxRefDatabase<CPendingCharacterInteraction, 8>
    // Base classes: CPersistent (offset 0), TPdxReferenceObject (offset 8)
    constexpr uintptr_t PENDING_INTERACTION_VTABLE     = 0x0bbb8cc0;  // HIGH: primary vtable (7 slots)
    constexpr uintptr_t PENDING_INTERACTION_TYPEINFO   = 0x0bbb8e90;  // HIGH: RTTI typeinfo

    // CPendingCharacterInteractionManager — per-tick consumer
    // Base classes: CPersistent (offset 0), CLegacyGameManagerInterface (offset 8)
    // The consumer re-validates pending interactions with CanSend before processing.
    // Failed re-validation silently destroys the interaction — this is where wars get lost.
    constexpr uintptr_t PENDING_MGR_VTABLE             = 0x0bbb8d30;  // HIGH: primary vtable (12 slots)
    constexpr uintptr_t PENDING_MGR_SEC_VTABLE         = 0x0bbb8da0;  // HIGH: CLegacyGameManagerInterface secondary vtable (primary+0x70) — used for singleton scan validation (ck-r56i)
    constexpr uintptr_t PENDING_MGR_TICK               = 0x061e6050;  // HIGH: main consumer tick function
    constexpr uintptr_t PENDING_MGR_PROCESS_ACCEPTED   = 0x061e5120;  // HIGH: post-CanSend processing
    constexpr uintptr_t PENDING_MGR_APPLY_EFFECTS      = 0x061e52a0;  // HIGH: effect dispatch by mode
    // ck-e58l.17 — CSendCharacterInteractionCommand::Execute → dispatcher cascade
    // Entry point for interaction-command consumer. Dequeues command and routes
    // to either DIRECT_PATH_SCRIPT_RUNNER (direct path) or back to CanSend cascade.
    constexpr uintptr_t SEND_CHAR_INTERACTION_CMD_EXECUTE = 0x061e8030;  // HIGH: FUN_062e8030 - 0x100000: core dispatcher, dequeues cmd + routes (ck-e58l.17) (ck-audit-B)
    constexpr uintptr_t DIRECT_PATH_SCRIPT_RUNNER         = 0x061e82f0;  // HIGH: FUN_062e82f0 - 0x100000: DIRECT dispatch path script runner (all 4 script slots NULL for war) (ck-e58l.17) (ck-audit-B)
    // INTERACTION_TRY_SEND_CASCADE (interaction_funcs namespace) is the CanSend re-validator
    // used by this consumer; moved to the canonical interaction_funcs::INTERACTION_TRY_SEND_CASCADE.

    // Evaluation framework
    constexpr uintptr_t EVAL_DISPATCH           = 0x07e63f80;  // HIGH: generic eval dispatcher (ck-e58l.20)
    constexpr uintptr_t EVAL_DATAMODEL_RESOLVE  = 0x07e61cd0;  // HIGH: data model resolver (MurmurHash3) (ck-e58l.20)
    constexpr uintptr_t RNG_WRAPPER             = 0x07748f10;  // MEDIUM: thread-safe RNG call (ck-e58l.20)

    // ─────────────────────────────────────────────────────────────────────
    // on_accept sibling effect classes (ck-e58l.15, resolves ck-e58l.2 OQ4)
    //
    // Full vtables dumped from core dump memory, cross-validated with Ghidra
    // decompilation. Addresses are offset-header style (Ghidra VA - 0x100000).
    //
    // CRITICAL: vtable[7] is NOT Execute for these classes — it is a
    // "GetScopeMask" virtual (bitfield of allowed scope types, computed from
    // vtable[6]). The actual Execute method is at vtable slot [23] (offset
    // +0xb8). This corrects/extends the ck-n18o claim that "slot [7] is
    // universally Execute"; ck-pu05 already noted this was wrong for
    // CStartWarEffect. See re/findings/re_onaccept_sibling_effects.md.
    //
    // All 8 vtables are 25-26 slots long (not 16 as ck-pu05's CStartWarEffect
    // dump suggested). The typeinfo struct for each class immediately follows
    // its vtable in .data.rel.ro.

    // CEndWarEffect — end_war = invalidated / ended
    constexpr uintptr_t END_WAR_EFFECT_VTABLE      = 0x0ba2f360;  // HIGH: 26 slots
    constexpr uintptr_t END_WAR_EFFECT_TYPEINFO    = 0x0ba2f430;  // HIGH
    constexpr uintptr_t END_WAR_EFFECT_EXECUTE     = 0x052a3730;  // HIGH: vtable[23] — resolves scope→char, calls FUN_066c8480 (end-war)

    // CUseHookEffect — use_hook = liege / vassal / etc.
    constexpr uintptr_t USE_HOOK_EFFECT_VTABLE     = 0x0b969290;  // HIGH: 26 slots
    constexpr uintptr_t USE_HOOK_EFFECT_TYPEINFO   = 0x0b969360;  // HIGH
    constexpr uintptr_t USE_HOOK_EFFECT_EXECUTE    = 0x04fd54a0;  // HIGH: vtable[23] — looks up + consumes hook (FUN_06760fb0 + FUN_06367040)

    // CRemoveCharacterFlagEffect — remove_character_flag = flag_name
    constexpr uintptr_t REMOVE_CHAR_FLAG_EFFECT_VTABLE   = 0x0b8fd8f0;  // HIGH: 26 slots
    constexpr uintptr_t REMOVE_CHAR_FLAG_EFFECT_TYPEINFO = 0x0b8fd9c0;  // HIGH
    constexpr uintptr_t REMOVE_CHAR_FLAG_EFFECT_EXECUTE  = 0x04eb3170;  // HIGH: vtable[23] — clears flag via FUN_07586f60(flags_mgr, flag_id)

    // CCustomDescriptionEffect<false> — custom_description = { ... } (localized variant)
    constexpr uintptr_t CUSTOM_DESC_EFFECT_F_VTABLE   = 0x0be03578;  // HIGH: 25 slots
    constexpr uintptr_t CUSTOM_DESC_EFFECT_F_TYPEINFO = 0x0be03640;  // HIGH

    // CCustomDescriptionEffect<true> — custom_description variant
    constexpr uintptr_t CUSTOM_DESC_EFFECT_T_VTABLE   = 0x0be03448;  // HIGH: 25 slots
    constexpr uintptr_t CUSTOM_DESC_EFFECT_T_TYPEINFO = 0x0be03510;  // HIGH

    // Both CCustomDescriptionEffect variants share the same slot[23] Execute
    // (generic child-effect iterator: iterates self+0x40, count at self+0x4c).
    constexpr uintptr_t CUSTOM_DESC_EFFECT_EXECUTE    = 0x0740beb0;  // HIGH: vtable[23] base — iterates children via FUN_0750b090

    // CJominiTriggerEventEffect<false> — immediate trigger_event dispatch
    constexpr uintptr_t TRIGGER_EVENT_EFFECT_F_VTABLE   = 0x0be05778;  // HIGH: 25 slots
    constexpr uintptr_t TRIGGER_EVENT_EFFECT_F_TYPEINFO = 0x0be05840;  // HIGH
    constexpr uintptr_t TRIGGER_EVENT_EFFECT_F_EXECUTE  = 0x0742af10;  // HIGH: vtable[23] — resolves scope + fires event (FUN_07546b10)

    // CJominiTriggerEventEffect<true> — scheduled trigger_event dispatch
    constexpr uintptr_t TRIGGER_EVENT_EFFECT_T_VTABLE   = 0x0be058a8;  // HIGH: 25 slots
    constexpr uintptr_t TRIGGER_EVENT_EFFECT_T_TYPEINFO = 0x0be05970;  // HIGH
    constexpr uintptr_t TRIGGER_EVENT_EFFECT_T_EXECUTE  = 0x0742eeb0;  // HIGH: vtable[23] — scheduled variant of above

    // CFactionStartWarEffect — faction_start_war = ... (faction-led war)
    constexpr uintptr_t FACTION_START_WAR_EFFECT_VTABLE   = 0x0b947798;  // HIGH: 25 slots
    constexpr uintptr_t FACTION_START_WAR_EFFECT_TYPEINFO = 0x0b947860;  // HIGH
    constexpr uintptr_t FACTION_START_WAR_EFFECT_EXECUTE  = 0x04f667f0;  // HIGH: vtable[23] — iterates faction members + calls FUN_066c9360 (same war-creation fn as CStartWarEffect)

    // CJominiScriptedEffect — invocation node for user-defined scripted_effect
    // templates (e.g. `my_helper = yes` or `my_helper = { p = v }` in script).
    // The actual user-script template is held in CJominiScriptedEffectTemplate;
    // this class is the engine-side per-call wrapper that resolves the template
    // by name at script-load time and runs it at game-tick time.
    // RTTI: 21CJominiScriptedEffect (rtti_classes.txt:12605).  (ck-kz1v)
    constexpr uintptr_t SCRIPTED_EFFECT_VTABLE   = 0x0be00e70;  // HIGH: 25 slots; vfunc0 = where vptr stored in objects (offset_to_top=0, typeinfo at -0x08)
    constexpr uintptr_t SCRIPTED_EFFECT_TYPEINFO = 0x0be00f48;  // HIGH: typeinfo struct (name="21CJominiScriptedEffect" @ rodata 0x08d298f0)
    constexpr uintptr_t SCRIPTED_EFFECT_EXECUTE  = 0x0740cb10;  // HIGH: vtable[23] — push scope frame, FUN_075dddb0(self+0x60,self+0x88) to bind params, FUN_0750b090 to walk child effect tree, pop scope. Bead acceptance OQ-B.
    constexpr uintptr_t SCRIPTED_EFFECT_RESOLVE  = 0x074095b0;  // HIGH: vtable[3] (inherited from CJominiEffect base) — script-load-time hash resolver: looks up CJominiScriptedEffectTemplate in singleton DB at _DAT_0c65f9d0 by MurmurHash3 of self+0xe8/+0xf4 std::string template name.

    // CJominiEffect base vtable slot [7] — GetScopeMask, not Execute.
    // Calls vtable[6] and returns a 128-bit bitfield of allowed scope types.
    // The real Execute lives at slot [23] (base default = 0x0740beb0, the
    // generic child-effect iterator used by CCustomDescriptionEffect etc.).
    // See ck-e58l.15 for the correction of ck-n18o's slot-[7] claim.
    constexpr uintptr_t JOMINI_EFFECT_GET_SCOPE_MASK = 0x03aed6f0;  // HIGH: base slot[7] — scope-type bitfield helper
    // Historical (ck-e58l.30): JOMINI_EFFECT_EXECUTE_BASE (0x03aed6f0, same
    // address — slot-[7]-as-Execute mislabel) and START_WAR_EFFECT_EXECUTE_STUB
    // (0x052a40e0, CStartWarEffect slot [7] stub) removed. Neither was used
    // by bridge code and both propagated the incorrect slot-[7] layout.
    // START_WAR_EFFECT_EXECUTE_V23 removed (ck-audit-B): was a duplicate of
    // START_WAR_EFFECT_CREATE_WAR (same address 0x052a11a0).
}

// =============================================================================
// CArtifact Struct Offsets (Royal Court DLC)
// Total size: 0x260 bytes, allocated in factory at 0x05190240
// Source: artifact.cpp, scope getters from _INIT_3455 (0x026619d0)
// =============================================================================
namespace artifact {
    constexpr size_t VTABLE              = 0x00;   // HIGH: ptr, set to 0x0ba98180 in factory
    constexpr size_t TYPE_INDEX          = 0x08;   // MEDIUM: uint32, artifact type ID
    constexpr size_t VALIDITY_FLAG       = 0x0C;   // MEDIUM: bool, init false
    constexpr size_t DB_INDEX            = 0x58;   // HIGH: int32, -1 = unregistered, swapped in dtor
    constexpr size_t RARITY              = 0xA8;   // HIGH: int32, enum 0-4 (common..illustrious)
    constexpr size_t SLOT_TYPE           = 0xB0;   // HIGH: ptr/ref, slot type reference
    constexpr size_t DURABILITY          = 0xB8;   // HIGH: int64, current durability
    constexpr size_t MAX_DURABILITY      = 0xC0;   // HIGH: int64, maximum durability
    constexpr size_t CREATOR_CHAR_REF    = 0x1A8;  // HIGH: character ref, resolved via CharDB
    constexpr size_t CHILD_ARRAY_PTR     = 0x220;  // MEDIUM: ptr, features/history array
    constexpr size_t CHILD_ARRAY_COUNT   = 0x22C;  // MEDIUM: int32
    constexpr size_t NAME_BUFFER         = 0x238;  // MEDIUM: inline SSO string
    constexpr size_t TOTAL_SIZE          = 0x260;  // HIGH: from _Znwm allocation
}

// =============================================================================
// TPdxRefDatabase<CArtifact, 8> — Artifact DB Singleton
// =============================================================================
namespace artifact_db {
    constexpr uintptr_t SINGLETON        = 0x0c65f9d0;  // HIGH: global ptr
    constexpr size_t ENTRIES_ARRAY       = 0x08;   // HIGH: ptr to CArtifact* array
    constexpr size_t ENTRIES_CAPACITY    = 0x10;   // HIGH: int32, array capacity
    constexpr size_t ENTRIES_COUNT       = 0x14;   // HIGH: int32, current count
    constexpr size_t SECONDARY_COUNT     = 0x4C;   // HIGH: int32, pure getter
    constexpr size_t MUTEX_FLAG          = 0x68;   // HIGH: int32, LOCK/UNLOCK pattern
}

// =============================================================================
// EArtifactRarity Enum (resolved by FUN_05f10e10)
// Lookup table at 0x08a7cd1c maps to localization IDs
// =============================================================================
namespace artifact_rarity {
    constexpr int COMMON                 = 0;      // HIGH
    constexpr int MASTERWORK             = 1;      // HIGH
    constexpr int FAMED                  = 2;      // HIGH
    constexpr int ILLUSTRIOUS            = 3;      // HIGH
    constexpr int SPECIAL                = 4;      // MEDIUM: 5th value, possibly unique/special
    constexpr int MAX_VALUE              = 4;      // HIGH: >= 5 returns fallback
}

// =============================================================================
// Contract Negotiation Scoring — Global Configuration
// These define addresses hold scoring weights, thresholds, and timers for
// the AI's contract modification acceptance system.
// See: re/findings/re_vassal_contract_scoring.md
// =============================================================================
namespace contract_scoring_globals {
    constexpr uintptr_t SCORING_WEIGHT_TABLE     = 0x0bedf070;  // HIGH: ptr→table, taxation/levy values per level (ck-e58l.20)
    constexpr uintptr_t ACCEPTANCE_THRESHOLD      = 0x0c16ea20;  // HIGH: long, score must exceed this to accept
    constexpr uintptr_t OBLIGATION_SCALING_FACTOR = 0x0c16ea50;  // HIGH: long, obligation level scaling
    constexpr uintptr_t RETRY_COUNT_THRESHOLD     = 0x0c16ea68;  // MEDIUM: int, AI re-evaluation retry limit
    constexpr uintptr_t RETRY_TIMEOUT             = 0x0c16ea80;  // MEDIUM: int, retry timeout before reset
    constexpr uintptr_t LIEGE_MODIFY_THRESHOLD    = 0x0c16ed70;  // HIGH: long, liege ratio threshold to trigger modification
    constexpr uintptr_t LIEGE_EVAL_TIMER_RESET    = 0x0c16e918;  // MEDIUM: int, tick countdown reset for liege eval
    constexpr uintptr_t SUBJECT_EVAL_TIMER_RESET  = 0x0c16e930;  // MEDIUM: int, tick countdown reset for subject eval
    constexpr uintptr_t MODIFY_COOLDOWN_NORMAL    = 0x0c16e948;  // MEDIUM: int, cooldown ticks after modification
    constexpr uintptr_t MODIFY_COOLDOWN_ADMIN     = 0x0c16e960;  // MEDIUM: int, cooldown ticks (admin province)
}

// =============================================================================
// Contract Manager Struct Offsets (partial)
// The manager object processes all vassal contracts per tick.
// Located at an unknown global; accessed via ContractManager_Tick (0x04e66220).
// =============================================================================
namespace contract_manager {
    constexpr size_t REF_ID              = 0x18;   // HIGH: uint32, manager ref ID
    constexpr size_t IS_ACTIVE           = 0x2c;   // HIGH: int32, 0 = skip tick
    constexpr size_t ENTRIES_PTR         = 0x50;   // HIGH: ptr, contract entries array (long*)
    constexpr size_t ENTRIES_COUNT       = 0x5c;   // HIGH: int32, number of contract entries
    constexpr size_t FLAGS               = 0x68;   // HIGH: uint16, bit0=active,bit2=admin,bit3=at_war,bit4=beneficial
    constexpr size_t SELECTED_TARGET     = 0x70;   // MEDIUM: ptr, selected contract type for modification
    constexpr size_t LIEGE_EVAL_TIMER    = 0x94;   // HIGH: int32, tick countdown for liege evaluation
    constexpr size_t SUBJECT_EVAL_TIMER  = 0x98;   // HIGH: int32, tick countdown for subject evaluation
    constexpr size_t REEVAL_TIMER        = 0x9c;   // HIGH: int32, re-evaluation countdown
    constexpr size_t SHOULD_MODIFY       = 0xa0;   // HIGH: bool, true if AI should propose changes
    constexpr size_t MOD_COOLDOWN_LIEGE  = 0xb0;   // MEDIUM: int32, modification cooldown (liege)
    constexpr size_t HOOK_EVAL_SUBOBJ    = 0xb60;  // HIGH: start of hook evaluation subobject
    constexpr size_t MOD_COOLDOWN_SUBJ   = 0xc0;   // MEDIUM: int32, modification cooldown (subject)
    constexpr size_t SCORING_TOTAL_A     = 0x1b28;  // HIGH: long, first scoring total (fixed-point)
    constexpr size_t SCORING_TOTAL_B     = 0x1b30;  // HIGH: long, second scoring total (fixed-point)
}

// =============================================================================
// Contract Entry Struct Offsets (partial)
// Per-contract entry within the manager's entries array.
// Processed by ContractEntry_ProcessChanges (0x04e3e7f0).
// =============================================================================
namespace contract_entry {
    constexpr size_t OBL_ARRAY_PTR       = 0x40;   // HIGH: ptr, obligation data array (long*)
    constexpr size_t OBL_COUNT           = 0x4c;   // HIGH: int32, obligation count
    constexpr size_t CONTRACT_TYPE_REF   = 0x58;   // HIGH: ptr, CSubjectContractType*
    constexpr size_t SELECTED_OBL_TYPE   = 0x60;   // HIGH: ptr, selected obligation type for change
    constexpr size_t PENDING_COUNTER     = 0x6c;   // HIGH: int32, pending change countdown
    constexpr size_t CHANGE_FLAGS        = 0x74;   // MEDIUM: int32, change operation flags
    constexpr size_t CHANGE_CATEGORY     = 0x78;   // HIGH: byte, obligation category (8 = none)
    constexpr size_t CHANGE_MODE         = 0x79;   // HIGH: byte, mode (3 = force via hook)
    constexpr size_t RETRY_COUNTER       = 0x7c;   // HIGH: int32, AI retry counter
    constexpr size_t TIMEOUT_COUNTER     = 0x80;   // HIGH: int32, timeout before reset
    constexpr size_t MODIFICATION_COUNT  = 0x88;   // MEDIUM: int32, total modifications applied
    constexpr size_t STATE_FLAGS         = 0x90;   // HIGH: byte, bit1=secondary, bit2=admin_province
}

// =============================================================================
// Dispatch-registry infrastructure (ck-fo7x.1)
//
// Shared mechanics for the trigger/effect/special_interaction name→handler
// dispatch. All three registries share the hash table at
// INTERACTION_CB_DB_ROOT + 0x1b0 (ck-e58l.2, ck-e58l.7). See
// re/findings/re_compiled_dispatch_registries.md for the full synthesis.
// =============================================================================
namespace dispatch_registry {
    // Trigger registrar — populates the trigger DB with (hash, entry) pairs.
    // 1,617 CALL sites across the binary. Pattern:
    //   LEA RDI,[name]; MOV ESI,<len>; CALL TRIGGER_HASH_FN; CALL
    //   TRIGGER_ENTRY_ALLOC_FN; ... ; LEA RCX,[factory_vtable]; CALL register.
    constexpr uintptr_t TRIGGER_REGISTER_FN    = 0x07525d40;  // HIGH: register(db, hash, entry) (ck-e58l.20)
    constexpr uintptr_t TRIGGER_HASH_FN        = 0x08169e60;  // HIGH: FNV1a_32(name_bytes) (ck-e58l.20)
    constexpr uintptr_t TRIGGER_ENTRY_ALLOC_FN = 0x075257c0;  // HIGH: allocate entry heap object (ck-e58l.20)

    // Effect-name FNV-1a dispatcher — hash-keyed lookup into the effect registry
    // (same hash-table mechanism as trigger registry, keyed by effect script-name).
    // Used by CStartWarEffect::vtable[9] (RESOLVE) and other script-load resolvers.
    constexpr uintptr_t EFFECT_FNV1A_DISPATCHER = 0x075298d0;  // HIGH: FUN_076298d0 - 0x100000: FNV-1a effect-registry dispatcher (ck-e58l.2) (ck-audit-B)

    // FNV-1a constants (shared with CStartWarEffect resolver, ck-e58l.2 §1.3).
    // See re/findings/registries/fnv1a_reference.py for implementation.
    constexpr uint32_t FNV_OFFSET_BASIS = 0x811c9dc5;  // HIGH
    constexpr uint32_t FNV_PRIME        = 0x01000193;  // HIGH
    constexpr uint32_t FNV_SCRAMBLER    = 0x26027a69;  // HIGH: secondary (legacy-compat path)

    // Master war-trigger registrar (registers `has_any_display_cb_on`,
    // `has_cb_on`, `can_declare_war`, ~30 war-related triggers).
    constexpr uintptr_t INIT_2584_WAR_TRIGGERS = 0x022039d0;  // HIGH: _INIT_2584 (ck-e58l.20)

    // War-trigger name string pool (wartriggerimpl.cpp).
    // Contains 81 snake_case identifiers starting here.
    constexpr uintptr_t WAR_TRIGGER_NAME_POOL_START = 0x08926444;  // HIGH
}

// =============================================================================
// User-scripted template databases (ck-j31o)
//
// Four sibling CGameDatabase<DB, Template> singletons that hold user-defined
// scripted_effects, scripted_triggers, scripted_lists, and scripted_modifiers
// loaded at startup from common/scripted_*/*.txt. Distinct from the engine
// effect/trigger registry at INTERACTION_CB_DB_ROOT + 0x1b0 (which holds
// ~1,790 compiled C++ effect classes).
//
// Convention: each holder is a base-relative offset into the binary's `.bss`.
// At runtime `*(g_base_addr + HOLDER) = heap pointer to DB instance` (size
// 0xf38 bytes). Holder is populated by the per-DB *_NewSingleton trampoline
// during static initialization (`new` + ctor + virtual init).
//
// All values are Ghidra-address − 0x100000 (image base). Convention matches
// dispatch_registry above. See re/findings/re_scripted_template_dbs.md.
// =============================================================================
namespace scripted_template_db {
    // ── Singleton holders (`.bss`; populated at static-init time) ───────────
    // Use as: db_ptr = read_offset<uintptr_t>(SCRIPTED_EFFECT_DB);
    constexpr uintptr_t SCRIPTED_EFFECT_DB    = 0x0c55fc38;  // HIGH: g_CJominiScriptedEffectTemplateDatabase (Ghidra 0x0c65fc38)
    constexpr uintptr_t SCRIPTED_TRIGGER_DB   = 0x0c561da0;  // HIGH: g_CJominiScriptedTriggerTemplateDatabase (Ghidra 0x0c661da0)
    constexpr uintptr_t SCRIPTED_LIST_DB      = 0x0c55fc48;  // HIGH: g_CJominiScriptedListTemplateDatabase (Ghidra 0x0c65fc48)
    constexpr uintptr_t SCRIPTED_MODIFIER_DB  = 0x0c564720;  // HIGH: g_CJominiScriptedModifierTemplateDatabase (Ghidra 0x0c664720)

    // ── Vtable addresses (primary, in .data.rel.ro) ─────────────────────────
    // Object's first qword (vptr value) = vtable+0x10 (i.e. &vfunc[0]).
    constexpr uintptr_t SCRIPTED_EFFECT_DB_VTABLE   = 0x0be0de38;  // HIGH (Ghidra 0x0bf0de38)
    constexpr uintptr_t SCRIPTED_TRIGGER_DB_VTABLE  = 0x0be10de0;  // HIGH (Ghidra 0x0bf10de0)
    constexpr uintptr_t SCRIPTED_LIST_DB_VTABLE     = 0x0be0f540;  // HIGH (Ghidra 0x0bf0f540)
    constexpr uintptr_t SCRIPTED_MODIFIER_DB_VTABLE = 0x0be0fdf0;  // HIGH (Ghidra 0x0bf0fdf0)

    // ── Object struct layout (CJominiScriptedEffectTemplateDatabase) ────────
    // Confirmed via core dump probe; sibling DBs share the layout (same
    // CGameDatabase<T> base + same templated unordered_map specialization).
    constexpr uintptr_t OBJ_PRIMARY_VPTR     = 0x000;  // HIGH: vtable_primary + 0x10
    constexpr uintptr_t OBJ_SECONDARY_VPTR   = 0x058;  // HIGH: vtable_secondary + 0x10 (multi-inheritance)
    constexpr uintptr_t OBJ_TEMPLATES_ARRAY  = 0x088;  // HIGH: pointer to template array (one per loaded scripted_*)
    constexpr uintptr_t OBJ_TEMPLATES_COUNT  = 0x094;  // HIGH: uint32 count of templates (matches CB_GROUP_DB pattern)
    constexpr uintptr_t OBJ_HASH_TABLE       = 0x0e0;  // HIGH: Robin-Hood map keyed by COMPOUND multi-block key (NOT a uint32 name hash); probed by RobinHoodProbe — see KEY_* below and re_scripted_template_dbs.md §10
    constexpr uintptr_t OBJ_SENTINEL_PTR     = 0x108;  // HIGH: returned by LookupTemplate when key+0xc == 0 (null/sentinel template)
    constexpr size_t    OBJ_SIZE             = 0x0f38; // HIGH: heap allocation size (per _Znam call in *_NewSingleton)

    // ── Functions (NON-VIRTUAL; values are Ghidra − 0x100000) ───────────────
    // Lookup wrapper: `Template* LookupTemplate(db, key)` — used directly by
    // CJominiScriptedEffect_Execute (param_1+0x60 = db, param_1+0x88 = key).
    // Returns *(db + OBJ_SENTINEL_PTR) when key+0xc == 0, otherwise
    // RobinHoodProbe(db + OBJ_HASH_TABLE, key) and returns *(entry+0x20).
    constexpr uintptr_t LOOKUP_TEMPLATE_FN   = 0x074dddb0;  // HIGH: CJominiScriptedTemplateDB_LookupTemplate (Ghidra 0x075dddb0)
    constexpr uintptr_t ROBINHOOD_PROBE_FN   = 0x074e1630;  // HIGH: CJominiScriptedTemplateDB_RobinHoodProbe (Ghidra 0x075e1630)

    // CJominiScriptedEffect node Execute (vfunc dispatched via engine effect
    // registry; takes the scripted-effect node + scope context). Resolves the
    // template via (this+0x60).LookupTemplate(this+0x88) then runs it.
    constexpr uintptr_t SCRIPTED_EFFECT_EXECUTE = 0x0740cb10;  // HIGH: CJominiScriptedEffect_Execute (Ghidra 0x0750cb10)

    // Effect-DB constructor + new-singleton trampoline (per-DB; sibling DBs
    // have analogous trampolines at 0x074d855b → g_TriggerDB, etc.).
    constexpr uintptr_t SCRIPTED_EFFECT_DB_CTOR          = 0x074de6c0;  // HIGH: CJominiScriptedEffectTemplateDatabase_Ctor (Ghidra 0x075de6c0)
    constexpr uintptr_t SCRIPTED_EFFECT_DB_NEW_SINGLETON = 0x074d8240;  // HIGH: CJominiScriptedEffectTemplateDatabase_NewSingleton (Ghidra 0x075d8240)

    // ── Vtable layout (CAbstractGameDatabase + derived) ─────────────────────
    // Lookup is NOT virtual on this hierarchy — see LOOKUP_TEMPLATE_FN above.
    // Identified vfuncs (offset from vtable_primary, indexed from vfunc[0] at
    // vtable+0x10):
    //   vfunc[0]  (+0x10) — ~Database destructor (resets vptrs, frees buffers)
    //   vfunc[3]  (+0x28) — virtual init / parallel-load over OBJ_TEMPLATES_ARRAY
    //   vfunc[4]  (+0x30) — for-each-template iterator (calls (*tpl)->vtable[0x48]())
    //   vfunc[6]  (+0x40) — destruct + free helper (reads g_CJominiScriptedEffectTemplateDatabase
    //                       directly — confirms holder identity)
    constexpr uintptr_t VFUNC_DTOR              = 0x10;  // HIGH
    constexpr uintptr_t VFUNC_INIT              = 0x28;  // HIGH
    constexpr uintptr_t VFUNC_FOR_EACH_TEMPLATE = 0x30;  // HIGH
    constexpr uintptr_t VFUNC_DESTROY_HELPER    = 0x40;  // HIGH

    // ── LookupTemplate compound-key shape (ck-phex) ─────────────────────────
    // The key passed to LookupTemplate is NOT {char*, len}; it's a small
    // envelope pointing at an array of 0x40-byte Block objects. Each block
    // holds a (std::string, std::string) pair compared via strcmp-equality.
    // See re_scripted_template_dbs.md §10 for full justification (3 decomp
    // sources). HIGH confidence on shape; LOW on per-block semantics.
    constexpr uintptr_t KEY_BLOCKS_PTR         = 0x000;  // HIGH: Block* (data ptr to array of 0x40-byte blocks)
    constexpr uintptr_t KEY_COUNT              = 0x00c;  // HIGH: uint32 — number of blocks (== 0 → sentinel)
    constexpr size_t    KEY_ENVELOPE_SIZE      = 0x010;  // HIGH: minimum bytes read from a Key by LookupTemplate

    constexpr size_t    BLOCK_SIZE             = 0x040;  // HIGH: stride in RobinHoodProbe + hash combiner
    constexpr uintptr_t BLOCK_S1_DATA          = 0x000;  // HIGH: char* (string-1 data ptr) — 1st MurmurHash3 input
    constexpr uintptr_t BLOCK_S1_LENGTH        = 0x008;  // HIGH: uint32 length (low word of 8-byte size_t field)
    constexpr uintptr_t BLOCK_S1_LOCAL         = 0x010;  // HIGH: 16-byte std::string SSO buffer / capacity union
    constexpr uintptr_t BLOCK_S2_DATA          = 0x020;  // HIGH: char* (string-2 data ptr) — 2nd MurmurHash3 input
    constexpr uintptr_t BLOCK_S2_LENGTH        = 0x028;  // HIGH: uint32 length (low word)
    constexpr uintptr_t BLOCK_S2_LOCAL         = 0x030;  // HIGH: 16-byte std::string SSO buffer

    // ── Robin-Hood entry layout (db + OBJ_HASH_TABLE) ───────────────────────
    // Each entry is 0x28 bytes. probe_distance: 0=empty, 0xff=sentinel,
    // [1..max] = filled. See re_scripted_template_dbs.md §11.
    constexpr size_t    ENTRY_SIZE             = 0x028;  // HIGH: stride in RobinHoodProbe (`lVar9 + 0x28`)
    constexpr uintptr_t ENTRY_PROBE_DISTANCE   = 0x004;  // HIGH: uint8 (0=empty, 0xff=sentinel, else 1+probe_count)
    constexpr uintptr_t ENTRY_BLOCKS_PTR       = 0x008;  // HIGH: Block* — same shape as KEY_BLOCKS_PTR
    constexpr uintptr_t ENTRY_COUNT            = 0x014;  // HIGH: uint32 — block count
    constexpr uintptr_t ENTRY_VALUE            = 0x020;  // HIGH: Template* — *(this) is the lookup result

    // ── Hash combiner + comparator (used by RobinHoodProbe) ─────────────────
    // Hash combiner walks (blocks, count) and does 2 MurmurHash3_32 calls per
    // block, mixed with the golden-ratio constant 0x9e3779b9.
    constexpr uintptr_t COMPOUND_KEY_HASH_FN   = 0x0748efb0;  // HIGH: hash combiner for compound key (Ghidra 0x0758efb0)
    constexpr uintptr_t STRCMP_EQ_FN           = 0x08198d60;  // HIGH: SSE strcmp-equality (Ghidra 0x08298d60)

    // ── Load-time parser pipeline (ck-zvu5l.1) ──────────────────────────────
    // The singleton trampoline (SCRIPTED_EFFECT_DB_NEW_SINGLETON) tail-calls
    // DB primary vt[2] = LoadAllScriptedEffectFiles, which iterates
    // common/scripted_effects/*.txt and parses each top-level entry into a
    // CJominiScriptedEffectTemplate. Full chain pinned + runtime-confirmed.
    // See re/findings/re_scripted_effect_parser_chain.md for evidence.
    constexpr uintptr_t DB_LOAD_ALL_FN                = 0x074def40;  // HIGH: DB primary vt[2]: LoadAllScriptedEffectFiles (Ghidra 0x075def40)
    constexpr uintptr_t DB_PROCESS_ONE_FILE_FN        = 0x074e07d0;  // HIGH: DB primary vt[5]: ProcessOneFile (".txt" filter) (Ghidra 0x075e07d0)
    constexpr uintptr_t DB_LOAD_TEXT_FILE_FN          = 0x074e0930;  // HIGH: DB primary vt[9]: LoadTextFile (tokenise + per-entry loop) (Ghidra 0x075e0930)
    constexpr uintptr_t DB_REGISTER_OR_PARSE_ENTRY_FN = 0x074e3f10;  // HIGH: per-entry parser (db, name, token_stream) (Ghidra 0x075e3f10)
    constexpr uintptr_t DB_LOOKUP_OR_INSERT_BY_NAME_FN = 0x074e40a0; // HIGH: (db, name)->Template* via Murmur+RobinHood (Ghidra 0x075e40a0)
    constexpr uintptr_t TEMPLATE_CTOR_FN              = 0x074dd820;  // HIGH: CJominiScriptedEffectTemplate_Ctor(this, id, std::string& name) (Ghidra 0x075dd820)

    // Secondary-base virtual call thunks used by the file iterator
    constexpr uintptr_t DB_SEC_BASE_VT2_THUNK         = 0x039a2d70;  // HIGH: jmp [secondary_vptr+0x10] (Ghidra 0x03a42d70)
    constexpr uintptr_t DB_SEC_VT2_THIS_ADJUST_THUNK  = 0x074e0b20;  // HIGH: sec_vt[2] adjust rdi -0x58 then call primary vt[5] (Ghidra 0x075e0b20)

    // Engine helpers (PDX VFS / tokeniser / hash)
    constexpr uintptr_t ITERATE_FILES_MATCHING_FN     = 0x08196000;  // HIGH: pdx_iterate_files_matching_pattern (Ghidra 0x08296000)
    constexpr uintptr_t STD_STRING_CTOR_FROM_CSTR_FN  = 0x0816a670;  // MEDIUM: std::string ctor (used by parser) (Ghidra 0x0826a670)
    constexpr uintptr_t PDX_TOKENISE_FILE_FN          = 0x0816e2c0;  // MEDIUM: (TokenStream*, std::string& path) — open + tokenise (Ghidra 0x0826e2c0)
    constexpr uintptr_t TOKENSTREAM_AT_END_FN         = 0x0816ea40;  // MEDIUM: (TokenStream*) -> bool (Ghidra 0x0826ea40)
    constexpr uintptr_t TOKENSTREAM_READ_NEXT_FN      = 0x081700c0;  // MEDIUM: advance to next top-level entry (Ghidra 0x082700c0)
    constexpr uintptr_t TOKENSTREAM_DTOR_FN           = 0x0816e5d0;  // MEDIUM: token-stream destructor (Ghidra 0x0826e5d0)
    constexpr uintptr_t HAS_EXTENSION_FN              = 0x08104fc0;  // MEDIUM: (path, ext)->bool — used to filter ".txt" (Ghidra 0x08204fc0)
    constexpr uintptr_t MURMURHASH3_32_FN             = 0x08113140;  // HIGH: MurmurHash3_32(seed=0, data, len) — name-hash for by-name table (Ghidra 0x08213140)

    // ── By-name RobinHood table fields on the DB struct (companion to OBJ_HASH_TABLE @ +0xe0)
    // This is the by-32-bit-hash table used by LookupOrInsertByName at load
    // time. Entry stride is 0x18 (24 bytes); 32-bit name_hash at entry+0x8;
    // template pointer at entry+0x10. Distinct from the +0xe0 compound-key
    // table used at execute time.
    constexpr uintptr_t OBJ_BYNAME_TABLE_ENTRIES      = 0x1b0;  // HIGH: void* — RobinHood entries (stride 0x18)
    constexpr uintptr_t OBJ_BYNAME_TABLE_MASK         = 0x1bc;  // HIGH: uint32 — mask = capacity - 1
    constexpr uintptr_t OBJ_BYNAME_TABLE_MAX_PROBE    = 0x1c0;  // MEDIUM: uint8 — max probe distance

    // ── Additional DB fields used by the parser path ────────────────────────
    constexpr uintptr_t OBJ_SEARCH_PATTERN_STRING     = 0x60;   // HIGH: std::string = "common/scripted_effects" (runtime-verified)
    constexpr uintptr_t OBJ_LOAD_DONE_FLAG            = 0x80;   // HIGH: uint32 — set to iterate_files return value
    constexpr uintptr_t OBJ_INSERTION_LOCK            = 0xed0;  // MEDIUM: small lock struct, used by LookupOrInsertByName
    constexpr uintptr_t OBJ_TEMPLATE_ARENA            = 0xed8;  // HIGH: arena allocator ptr; vt[2] = allocate(size,align)
    constexpr uintptr_t OBJ_SKIP_LOCK_FLAG            = 0xf10;  // MEDIUM: bool — if set, skip lock acquisition
    constexpr uintptr_t OBJ_LOAD_MODE                 = 0xf14;  // HIGH: uint32 — 0=idle, 1=normal load, 2=duplicate-check
    constexpr uintptr_t OBJ_FILE_FORMAT_HINT          = 0xf18;  // MEDIUM: uint32 — read by LoadTextFile, passed to tokeniser
    constexpr uintptr_t OBJ_IN_PROGRESS_FLAG          = 0xf1c;  // HIGH: bool — set during file load

    // ── Template-side constants ─────────────────────────────────────────────
    constexpr size_t    TEMPLATE_SIZE                 = 0x110;  // HIGH: sizeof(CJominiScriptedEffectTemplate) — runtime stride
    constexpr uintptr_t TEMPLATE_PRIMARY_VPTR_VALUE   = 0x0be0dd28; // HIGH: vtable_CJominiScriptedEffectTemplate + 0x10 (Ghidra 0x0bf0dd28)
    constexpr uintptr_t TEMPLATE_SECONDARY_VPTR_VALUE = 0x0be0ddc0; // HIGH: secondary vtable + 0x10 (Ghidra 0x0bf0ddc0)
    constexpr uintptr_t TEMPLATE_NAME_STRING          = 0x10;   // HIGH: std::string name (runtime-verified)
    constexpr uintptr_t TEMPLATE_SECONDARY_VPTR_OFFSET = 0x30;  // HIGH: second vptr in template (multi-base layout)
    constexpr uintptr_t TEMPLATE_SOURCE_FILE_STRING   = 0xa0;   // HIGH: std::string source file path (e.g. "common/scripted_effects/<file>.txt")
}

// =============================================================================
// Script-engine scope-eval context (`scope_ctx`) — ck-rw9m
//
// 0x30-byte stack-allocated struct passed as the second argument to every
// CJominiEffect::Execute (vt[23], all subclasses). Built by FUN_07551640
// for effects (6 fields, with RNG state) or FUN_07551620 for triggers
// (5 fields, no RNG). The first slot holds a CScopeObject* whose 2-byte
// scope-type tag at +0x00 the engine reads for mask validation against the
// effect's required scope (vt[7] = JOMINI_EFFECT_GET_SCOPE_MASK).
//
// See re/findings/re_scripted_effect_scope_ctx.md (ck-rw9m).
// =============================================================================
namespace scope_ctx {
    constexpr size_t SIZE                  = 0x30;  // HIGH: 6 fields, 8-byte aligned
    constexpr size_t THIS_SCOPE_PTR        = 0x00;  // HIGH: CScopeObject* (current/this)
    constexpr size_t PREV_SCOPE_PTR        = 0x08;  // HIGH: CScopeObject* (parent; NULL at top level)
    constexpr size_t ROOT_SCOPE_PTR        = 0x10;  // HIGH: CScopeObject* (root; same as THIS at top level)
    constexpr size_t EVAL_TARGETS_PTR      = 0x18;  // HIGH: SavedEventTargets* (~0x3D0 bytes, built by FUN_0754fe70)
    constexpr size_t DEBUG_FLAG_BYTE       = 0x20;  // HIGH: 1 byte (0 = silent, 1 = tooltip emit; sourced from DAT_0bfd26f8)
    constexpr size_t RNG_STATE_PTR         = 0x28;  // HIGH: uint32_t[2]* (seed, counter)
}

// CScopeObject — 0x168-byte typed game-object wrapper (built by AI_SCOPE_FROM_CHAR
// and the per-type sibling constructors)
namespace scope_object {
    constexpr size_t SIZE                  = 0x168; // HIGH: total stack footprint per AI_SCOPE_FROM_CHAR init
    constexpr size_t SCOPE_TYPE            = 0x00;  // HIGH: uint16 (4 = character)
    constexpr size_t VALUE_PACKED_REF      = 0x10;  // HIGH: uint32 packed game-object ref
    constexpr size_t INITIALIZED_BOOL      = 0x166; // MEDIUM: byte, 0 at init
}

// scope_ctx / scope_object construction primitives.
// Values are base-relative (Ghidra address − 0x100000).
namespace scope_ctx_fns {
    // Build 0x30-byte scope_ctx (6 fields). Used by every effect-eval path.
    constexpr uintptr_t SCOPE_CTX_INIT_6   = 0x07451640;  // HIGH: FUN_07551640 (Ghidra)
    // Build 0x21-byte trigger scope_ctx (5 fields, no RNG). Triggers only.
    constexpr uintptr_t SCOPE_CTX_INIT_5   = 0x07451620;  // HIGH: FUN_07551620 (Ghidra)
    // Build CScopeObject from packed character ref (cross-ref of existing AI_SCOPE_FROM_CHAR entry).
    constexpr uintptr_t AI_SCOPE_FROM_CHAR_REF = 0x05ab3d60;  // HIGH (alias of existing AI_SCOPE_FROM_CHAR)
    // Set scope_object's 2-byte type tag.
    constexpr uintptr_t SCOPE_OBJ_SET_TYPE = 0x0748ae90;  // HIGH: FUN_0758ae90 (Ghidra)
    // Set scope_object's packed-ref value.
    constexpr uintptr_t SCOPE_OBJ_SET_VAL  = 0x0748aef0;  // HIGH: FUN_0758aef0 (Ghidra)
    // Initialise the saved-event-targets / scope-frame container (~0x3D0 bytes).
    constexpr uintptr_t SAVED_TARGETS_INIT = 0x0744fe70;  // HIGH: FUN_0754fe70 (Ghidra)
    // Sub-destructor for the two intrusive vectors of a CScopeObject.
    constexpr uintptr_t SCOPE_OBJ_SUB_DTOR = 0x037df310;  // HIGH: FUN_038df310 (Ghidra)
    // TLS-managed scope-frame stack — push at Execute entry, pop at Execute exit.
    constexpr uintptr_t SCOPE_FRAME_PUSH   = 0x074c10e0;  // HIGH: FUN_075c10e0 (Ghidra)
    constexpr uintptr_t SCOPE_FRAME_POP    = 0x074c1310;  // HIGH: FUN_075c1310 (Ghidra)
    // Universal AST walker — reads ctx, dispatches vt[23] on each child node.
    constexpr uintptr_t EFFECT_TREE_WALK   = 0x0740b090;  // HIGH: FUN_0750b090 (Ghidra)
}

// Global byte read into scope_ctx[+0x20] — debug/tooltip flag (0 in normal play).
constexpr uintptr_t SCRIPT_SCOPE_DEBUG_FLAG = 0x0bed26f8;  // HIGH: byte, DAT_0bfd26f8 (Ghidra) (ck-rw9m)

// =============================================================================
// Scope-eval chain iterator + CJominiLinkBehavior hierarchy — ck-4jf6
//
// FUN_07561f50 (Ghidra) is the universal scope-chain iterator embedded at
// CStartWarEffect+0x98 / +0x140 / CB-option+0x10 and ~816 other call sites.
// It walks an array of CJominiEventTarget::CLink records (16 bytes each),
// calling per-iteration vt[5] (Resolve) on each link's CJominiLinkBehavior*.
//
// The bead's title says vt[0x38]; the operative slot is actually vt[0x28]
// (slot 5 = Resolve). vt[0x38] (slot 7 = GetExpectedFromTypeMask) returns
// a 128-bit input-type bitmap — most subclasses inherit a no-op zero
// ("accept any") at LINK_TYPEMASK_NOOP below.
//
// See re/findings/re_scope_eval_node_class.md (ck-4jf6).
// =============================================================================
namespace scope_eval_chain {
    // Iterator entry point.
    constexpr uintptr_t CHAIN_EVAL              = 0x07461f50;  // HIGH: FUN_07561f50 (Ghidra)

    // Evaluator container layout (CStartWarEffect+0x98 etc.).
    constexpr size_t    EVAL_NODES_PTR          = 0x00;        // HIGH: ptr to CLink[] inline buffer
    constexpr size_t    EVAL_NODE_COUNT         = 0x0c;        // HIGH: i32; if 0 -> returns zero CScopeValue
    constexpr size_t    EVAL_TAIL_CTX_OWNED     = 0x70;        // HIGH: per-evaluator tail-ctx
    constexpr size_t    EVAL_TAIL_SELECT_FLAG   = 0x78;        // HIGH: i32; 0 -> use caller-supplied tail_ctx

    // CJominiEventTarget::CLink record (16 bytes per array entry, stride 0x10).
    constexpr size_t    CLINK_RECORD_SIZE       = 0x10;        // HIGH
    constexpr size_t    CLINK_LINK_PTR          = 0x00;        // HIGH: CJominiLinkBehavior*
    constexpr size_t    CLINK_INLINE_PAYLOAD    = 0x08;        // HIGH: 8 bytes; 0 in observed singletons

    // Vtable layout for CJominiLinkBehavior subclasses.
    constexpr size_t    LINK_VT_RESOLVE_OFF     = 0x28;        // HIGH: vt[5] = Resolve(this, ctx_68B*) -> 16B CScopeValue
    constexpr size_t    LINK_VT_TYPEMASK_OFF    = 0x38;        // HIGH: vt[7] = GetExpectedFromTypeMask() -> 128-bit mask
    constexpr size_t    LINK_PAYLOAD_OFF        = 0x08;        // HIGH: i32/u64 (per-class - name-registry idx, link key, ...)

    // Common base methods (most subclasses inherit; some override).
    constexpr uintptr_t LINK_BASE_DTOR          = 0x04da7940;  // HIGH: vt[0] common dtor (Ghidra 0x04ea7940)
    constexpr uintptr_t LINK_BASE_RESOLVE_NULL  = 0x04da7b80;  // HIGH: vt[5] no-op returning 0 (Ghidra 0x04ea7b80)
    constexpr uintptr_t LINK_TYPEMASK_NOOP      = 0x04dc3510;  // HIGH: vt[7] no-op ZEXT816(0) (Ghidra 0x04ec3510)

    // Iterator helper: build a 0x30-byte child scope_ctx from parent + override-current.
    constexpr uintptr_t SCOPE_CTX_CHILD_BUILD   = 0x074515e0;  // HIGH: FUN_075515e0 (Ghidra)

    // Saved-event-targets lookup (linear scan, 32-byte stride).
    constexpr uintptr_t LOOKUP_SAVED_TARGET     = 0x07450ac0;  // HIGH: FUN_07550ac0 (Ghidra)
    constexpr size_t    SAVED_TARGET_STRIDE     = 0x20;        // HIGH: SSavedEventTarget size (re_scope_system.md)
}

// CJominiLinkBehavior subclass vtables. Use VFUNC_START (= vtable + 0x10)
// as the +0x00 value when synthesising a heap-allocated leaf instance.
namespace link_class {
    // CJominiEventTargetLink - `scope:varname` saved-target lookup.
    constexpr uintptr_t EVENT_TARGET_LINK_TYPEINFO    = 0x0be07cc8;  // HIGH (Ghidra 0x0bf07cc8)
    constexpr uintptr_t EVENT_TARGET_LINK_VTABLE      = 0x0be07c40;  // HIGH (Ghidra 0x0bf07c40)
    constexpr uintptr_t EVENT_TARGET_LINK_VFUNC_START = 0x0be07c50;  // HIGH (vtable + 0x10)
    constexpr uintptr_t EVENT_TARGET_LINK_RESOLVE     = 0x0746adf0;  // HIGH: vt[5] (Ghidra 0x0756adf0)

    // CCharacterLink - `character:foo` literal. Payload at link+0x8 = u32
    // script-element-name registry index (NOT a character ID - see
    // re_scope_eval_node_class.md sec.5).
    constexpr uintptr_t CHARACTER_LINK_TYPEINFO       = 0x0b9451f8;  // HIGH (Ghidra 0x0ba451f8)
    constexpr uintptr_t CHARACTER_LINK_VTABLE         = 0x0b945170;  // HIGH (Ghidra 0x0ba45170)
    constexpr uintptr_t CHARACTER_LINK_VFUNC_START    = 0x0b945180;  // HIGH (vtable + 0x10)
    constexpr uintptr_t CHARACTER_LINK_RESOLVE        = 0x04f5fc10;  // HIGH: vt[5] (Ghidra 0x0505fc10)

    // CTitleLink - `title:foo` literal. Payload at link+0x8 = u64 packed
    // script-time key.
    constexpr uintptr_t TITLE_LINK_TYPEINFO           = 0x0ba25b78;  // HIGH (Ghidra 0x0bb25b78)
    constexpr uintptr_t TITLE_LINK_VTABLE             = 0x0ba25af0;  // HIGH (Ghidra 0x0bb25af0)
    constexpr uintptr_t TITLE_LINK_VFUNC_START        = 0x0ba25b00;  // HIGH (vtable + 0x10)
    constexpr uintptr_t TITLE_LINK_RESOLVE            = 0x05289780;  // HIGH: vt[5] (Ghidra 0x05389780)

    // Useful sibling leaves (HIGH for vtable addr only; per-Resolve decomp
    // not in scope here - see re_scope_eval_node_class.md sec.1).
    constexpr uintptr_t FATHER_LINK_VTABLE            = 0x0b8e7210;  // HIGH (Ghidra 0x0b9e7210)
    constexpr uintptr_t MOTHER_LINK_VTABLE            = 0x0b8e77b0;  // HIGH (Ghidra 0x0b9e77b0)
    constexpr uintptr_t HOLDER_LINK_VTABLE            = 0x0ba25410;  // HIGH (Ghidra 0x0bb25410)
    constexpr uintptr_t WAR_LINK_VTABLE               = 0x0ba32460;  // HIGH (Ghidra 0x0bb32460)
    constexpr uintptr_t CASUS_BELLI_LINK_VTABLE       = 0x0ba32500;  // HIGH (Ghidra 0x0bb32500)
    constexpr uintptr_t FAITH_LINK_VTABLE             = 0x0b94fed0;  // HIGH (Ghidra 0x0ba4fed0)
    constexpr uintptr_t THIS_LINK_VTABLE              = 0x0be07ba0;  // HIGH (Ghidra 0x0bf07ba0)
    constexpr uintptr_t ROOT_LINK_VTABLE              = 0x0be07a60;  // HIGH (Ghidra 0x0bf07a60)
    constexpr uintptr_t PREV_LINK_VTABLE              = 0x0be07b00;  // HIGH (Ghidra 0x0bf07b00)
    constexpr uintptr_t YES_LINK_VTABLE               = 0x0be07f60;  // HIGH (Ghidra 0x0bf07f60)
    constexpr uintptr_t NO_LINK_VTABLE                = 0x0be08000;  // HIGH (Ghidra 0x0bf08000)
    constexpr uintptr_t VARIABLE_LINK_VTABLE          = 0x0be07ce0;  // HIGH (Ghidra 0x0bf07ce0)

    // Common base typeinfo (parent of all 283 enumerated leaves).
    constexpr uintptr_t LINK_BEHAVIOR_BASE_TYPEINFO   = 0x0be076c8;  // HIGH (Ghidra 0x0bf076c8) "19CJominiLinkBehavior"
}

// ═══════════════════════════════════════════════════════════════════════════
// Character Memories (CCharacterMemory / CCharacterMemoryType)
// ───────────────────────────────────────────────────────────────────────────
// Discovered via core dump (ck3_20260421_195552.core) struct walking, validated
// across 12+ memory types and 1000+ pooled objects. See re_character_memories.md.
//
// Resolution chain (read recipe):
//   CCharacter* -> per-char memory ref list (collection_offset: OPEN, see findings)
//     -> u32 ref-id -> TPdxRefDatabase<CCharacterMemory,8> -> CCharacterMemory*
//   CCharacterMemory.type_ptr (+0x10) -> CCharacterMemoryType*
//     -> name std::string at db_object::NAME_KEY (+0x10) = e.g. "relative_died"
//   participants array (+0x18, count +0x20) -> {char_id u32, role_hash u32} entries
//   creation date (+0x30) / end date (+0x38) = game-date int (24 ticks/day since
//     epoch DATE_EPOCH=43800000; (val-43800000)/24/365.25 = year AD)
// ═══════════════════════════════════════════════════════════════════════════
namespace char_memory {
    // SCHEMA-CORE: CCharacterMemory object (pooled, stride 0x58). vtable img 0xbca6ca0.
    constexpr size_t CORE_VTABLE         = 0x00;  // HIGH: ptr, CCharacterMemory vptr (img 0xbca6ca0)
    constexpr size_t CORE_REF_ID         = 0x08;  // HIGH: u32, this memory's pool id
    constexpr size_t CORE_TYPE_PTR       = 0x10;  // HIGH: ptr -> CCharacterMemoryType (resolve name via +0x10)
    constexpr size_t CORE_PARTICIPANTS   = 0x18;  // HIGH: ptr -> array of {u32 char_id, u32 role_hash}
    constexpr size_t CORE_PARTICIPANT_CNT = 0x20; // HIGH: u32 count (also dup'd at +0x24=cap)
    constexpr size_t CORE_PARTICIPANT_STRIDE = 0x08; // HIGH: 8 bytes/entry (char_id + role_hash)
    constexpr size_t CORE_PART_CHAR_ID   = 0x00;  // HIGH: u32 participant char id (mask &0xFFFFFF for CHARACTER_DB)
    constexpr size_t CORE_PART_ROLE_HASH = 0x04;  // HIGH: u32 stable per-role key hash (e.g. dead_relation=0x205f)
    constexpr size_t CORE_OWNER_LINK     = 0x28;  // MED: ptr, CCharacterMemoryOwnerLink descriptor (img 0xbeae980)
    constexpr size_t CORE_CREATION_DATE  = 0x30;  // HIGH: int32 game date (SMemoryCreationDate); always <= current
    constexpr size_t CORE_END_DATE       = 0x38;  // MED: int32 game date (expiry); long-duration types show ~create+130yr sentinel
    constexpr size_t CORE_RELEVANCE      = 0x40;  // LOW: int32 (~72797 observed) — relevance/weight or secondary days field
    constexpr size_t CORE_STRUCT_SIZE    = 0x58;  // HIGH: contiguous pool stride

    // CCharacterMemoryType object (game_db entry). vtable img 0xbd198c0 (runtime).
    constexpr size_t TYPE_NAME_KEY  = 0x10;  // HIGH: std::string type name (db_object::NAME_KEY). "relative_died" etc.
    constexpr size_t TYPE_INDEX     = 0x08;  // MED: u32 type index (lo 32 of +0x8; relative_died=77); +0x8 hi = name hash
    constexpr size_t TYPE_CATEGORIES = 0x30; // MED: ptr -> categories array (+0x38 count); element decode unconfirmed
    constexpr size_t TYPE_PARTICIPANTS = 0x48; // MED: ptr -> participant-role defs (+0x50 count)
    constexpr size_t TYPE_DURATION_DAYS = 0x60; // LOW: int (0x16d=365 seen) — duration; needs confirm

    // CCharacterMemoryTypeDatabase singleton global — HIGH (core dump pinned + VERIFIED).
    // *(g_base+MEMORY_TYPE_DB) -> db (game_db layout); db+game_db::ENTRIES_ARRAY(0x88)
    // -> CCharacterMemoryType*[]; db+game_db::ENTRY_COUNT(0x90) = 382 types live.
    // entries[idx]->TYPE_NAME_KEY(0x10) resolves index->name. VERIFIED: entries[77]=
    // "relative_died", entries[150]="married". For a read handler, prefer the direct
    // memory type-ptr -> name; the DB is needed for index->name / category enumeration.
    // NOTE: this offset is core-dump-derived (runtime-base_addr); base_addr == g_base
    // (CHARACTER_DB 0x0becf088 confirms), so it is the bridge header offset directly —
    // do NOT subtract 0x100000 (that adjustment is only for Ghidra-VA-sourced offsets).
    // All char_memory vtable IMG addresses below (0xbca6ca0, 0xbd198c0, 0xbd06578,
    // 0xbeae980) are likewise direct base-relative offsets, not Ghidra VAs.
    constexpr uintptr_t MEMORY_TYPE_DB = 0x0c0078a0;  // HIGH: CCharacterMemoryTypeDatabase singleton. header offset (= base+0xc0078a0); VERIFIED deref->db(count 382). db vtable img 0xbd06578.

    // ⚠ CONFLICT NOTE (two independent investigations, 2026-06-20):
    //   This namespace MIXES two object schemas that DISAGREE. They were probed on
    //   different states (core dump 20260421 vs the live Dynasty-Race build) and the
    //   ref-id spaces are disjoint (core-dump pool ids ~0x28e0d+ exceed the live
    //   refdb capacity 0x2c00), so they are NOT the same array. Do NOT mix offsets
    //   across the two blocks. Treat as:
    //     SCHEMA-CORE (stride 0x58, type-ptr@0x10, participants@0x18): the *_CORE
    //       fields above — core-dump-derived, every field re-validated across 1000+
    //       pooled objects & 12+ types. The TYPE-DB singleton (above) is shared/HIGH.
    //     SCHEMA-LIVE (stride 0xd8, primary-char@0x20, refdb 0x0bed7700): the ADDENDUM
    //       below — live + decompile (FUN_0674f780). HIGH for landed chars.
    //   OPEN: which schema the bridge read handler should walk. The LIVE block has the
    //   per-char collection (landed+0x218) the CORE block lacks; the CORE block has a
    //   verified type-name resolution. Reconcile on next live session (re_character_memories.md).

    // ─────────────────────────────────────────────────────────────────────────
    // LIVE-VALIDATED ADDENDUM (2026-06-20, running Dynasty-Race build, char 15048
    // w/ 8 memories; decompile-confirmed). Resolves the previously-OPEN collection
    // + ref-db. SUPERSEDES the core-dump layout above WHERE THEY CONFLICT: the live
    // pool stride is 0xd8 (not 0x58) and the live per-memory TYPE is NOT a static
    // ptr at +0x10 (that slot is an infra vtable, constant across all memories).
    // See re/findings/re_character_memories.md.
    //
    // PER-CHARACTER COLLECTION — HIGH (decompile FUN_0674f780 @ img 0x674f780 + live):
    //   list = *(CCharacter + 0x1b8 /*landed_data, null if unlanded*/) + 0x218
    //   list = CPdxVector<u32 ref_id>: data @ +0x00, capacity @ +0x08, count(i32) @ +0x0c.
    //   Element = u32 memory ref-id (idx | gen<<24), stride 4.
    constexpr size_t LIST_IN_LANDED   = 0x218; // HIGH: offset in landed_data(char+0x1b8) to the mem ref-id vector
    constexpr size_t LIST_DATA        = 0x00;  // HIGH: u32* data
    constexpr size_t LIST_CAPACITY    = 0x08;  // HIGH: u32 capacity
    constexpr size_t LIST_COUNT       = 0x0c;  // HIGH: i32 count
    constexpr size_t REFID_STRIDE     = 0x04;  // HIGH: bytes per ref-id (u32)

    // MEMORY OBJECT (pooled, LIVE stride 0xd8). Validate *(obj+8)==ref_id.
    // ── CORRECTED + EXTENDED 2026-06-20 (this investigation): every field below
    //    re-pinned from the engine's own serializer (FUN_064d3ae0) + deserializer
    //    (FUN_064d3820) + resolver (FUN_0617ff10) and re-validated on 8 live
    //    memory objects across 5 landed chars in core dump ck3_20260421. The
    //    earlier OBJ_DATE_A/B (+0xa8/+0xb0) and OBJ_PARTICIPANTS layout were
    //    WRONG — superseded here (kept as deprecated aliases for traceability).
    constexpr size_t OBJ_STRIDE_LIVE     = 0xd8;  // HIGH: live pool element stride
    constexpr size_t OBJ_REF_ID          = 0x08;  // HIGH: u32 full ref-id (idx|gen<<24); == list entry; gen-check key
    constexpr size_t OBJ_PRIMARY_CHAR    = 0x20;  // HIGH: CCharacter* "other"/subject participant (FUN_064d3770; serializer tag 0x29ae reads *(+0x20)+0x18=char id)
    constexpr size_t OBJ_OWNER_CHAR      = 0x28;  // HIGH: CCharacter* memory OWNER (== char whose list was walked; serializer tag 0x292d). NEW.
    constexpr size_t OBJ_PART_DATA       = 0x68;  // HIGH: ptr -> participant sub-collection data (serializer FUN_064ef9b0 reads (mem+0x38)+0x30). CORRECTED (was 0x38).
    constexpr size_t OBJ_PART_COUNT      = 0x70;  // HIGH: i32 participant element count (serializer reads (mem+0x38)+0x3c). CORRECTED (was 0x44).
    constexpr size_t OBJ_DATE_CREATION   = 0xa4;  // HIGH: int32 game-date, serializer tag 0x6b5 via date-serializer FUN_076e8360. CORRECTED (was 0xa8). Decodes to sane years.
    constexpr size_t OBJ_DATE_END        = 0xac;  // HIGH: int32 game-date, serializer tag 0x3b04. CORRECTED (was 0xb0). Decodes to sane years (expiry/secondary).
    constexpr size_t OBJ_VAR_INT         = 0xa0;  // MED: int32, serializer tag 0x311f, default -1 (omitted when -1). NEW.
    constexpr size_t OBJ_RELEVANCE       = 0xc0;  // MED: int64 fixed-point relevance/weight, tag 0x3b4f, default -100000. NEW.
    // Deprecated aliases (WRONG offsets from the pre-2026-06-20 addendum). Do not use.
    constexpr size_t OBJ_PARTICIPANTS_DEPRECATED    = 0x38;
    constexpr size_t OBJ_DATE_A_DEPRECATED          = 0xa8;
    constexpr size_t OBJ_DATE_B_DEPRECATED          = 0xb0;

    // CCharacterMemory ref-database singleton — HIGH (live; Ghidra _DAT_0bfd7700,
    // base-rel 0x0bed7700). Resolution (decompile FUN_0617ff10, VERIFIED on the
    // core dump — every gen-check passed): given list ref_id,
    //   idx    = ref_id & 0xffffff
    //   refdb  = *(g_base + REFDB_GLOBAL)
    //   if idx >= *(refdb + REFDB_CAPACITY): skip
    //   memobj = *( *(refdb + REFDB_ARRAY) + REFDB_ENTRY_HDR + idx * REFDB_ENTRY_STRIDE )
    //   require *(memobj + OBJ_REF_ID) == ref_id   (generation validity)
    // ⚠ CORRECTED 2026-06-20: the memobj ptr is at the slot BASE (array+8+idx*0x10),
    //   NOT slot+8. The pre-2026-06-20 recipe's extra "+8" read the freelist marker
    //   0x00ffffff00ffffff and made EVERY lookup fail. (REFDB == 0x0bed7700 is ALSO
    //   documented as globals::VASSAL_CONTRACT_DB — same singleton offset, different
    //   accessor; the memory resolver path above is the engine's own, decompile-pinned.)
    constexpr uintptr_t REFDB_GLOBAL     = 0x0bed7700; // HIGH: g_base+off -> CCharacterMemory ref-db singleton ptr
    constexpr size_t REFDB_ARRAY         = 0x20; // HIGH: ptr to entry array
    constexpr size_t REFDB_CAPACITY      = 0x2c; // HIGH: u32 capacity
    constexpr size_t REFDB_ENTRY_HDR     = 0x08; // HIGH: bytes skipped at array start (header)
    constexpr size_t REFDB_ENTRY_STRIDE  = 0x10; // HIGH: bytes per slot; memobj ptr is at slot BASE (offset 0), NOT slot+8
}

// CScopeValue type-tag registry (consolidated - extends ck-lr20 sec.2).
namespace scope_value_type {
    constexpr uint16_t UNSET     = 0;  // HIGH: returned on resolution failure
    constexpr uint16_t BOOL      = 2;  // HIGH: re_scope_system.md sec.1
    constexpr uint16_t CHARACTER = 4;  // HIGH: ck-lr20; cross-confirmed by CCharacterLink::Resolve (ck-4jf6)
    constexpr uint16_t TITLE     = 5;  // HIGH: pinned by CTitleLink::Resolve (ck-4jf6) - closes ck-lr20 OQ-D hypothesis
}
