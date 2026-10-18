#pragma once
#include <cstdint>
#include <cstddef>

namespace ck3_resolver {

// How an offset was resolved
enum class ResolveMethod : uint8_t {
    UNRESOLVED = 0,
    HARDCODED = 1,
    RTTI_VTABLE = 2,
    SIGNATURE = 3,
    RIP_ANCHOR = 4,
    DISCOVERED_MISMATCH = 5,  // discovered but differs from hardcoded
};

// Memory region descriptor (from PT_LOAD segments)
struct MemoryRegion {
    uintptr_t start;
    uintptr_t end;
    bool readable;
    bool writable;
    bool executable;

    size_t size() const { return end - start; }
    bool contains(uintptr_t addr) const { return addr >= start && addr < end; }
};

// Resolved offsets — all stored as offsets from binary base (same as ck3_offsets.h)
// These are the ~230 offsets we can discover at runtime
struct ResolvedOffsets {
    // === Tier 1: Bridge won't load without these ===
    uintptr_t AI_MANAGER_MAIN_TICK;
    uintptr_t AI_MANAGER_POST_EXECUTE;
    uintptr_t AI_ENABLED_FLAG;
    uintptr_t GAME_STATE_ROOT;
    uintptr_t CHARACTER_DB;
    uintptr_t IDLER_SINGLETON;

    // === Tier 2: Core functionality ===
    uintptr_t STRING_HASH;             // MurmurHash3-32
    uintptr_t CHAR_ID_TO_PTR;
    uintptr_t AI_PERSONALITY_HONOR;
    uintptr_t AI_PERSONALITY_GREED;
    uintptr_t AI_PERSONALITY_RATIONALITY;
    uintptr_t AI_PERSONALITY_ENERGY;
    uintptr_t AI_PERSONALITY_BOLDNESS;
    uintptr_t AI_PERSONALITY_ZEAL;
    uintptr_t AI_PERSONALITY_VENGEFULNESS;
    uintptr_t AI_PERSONALITY_COMPASSION;
    uintptr_t AI_PERSONALITY_SOCIABILITY;
    uintptr_t THREAD_FLAG_STRATEGY;
    uintptr_t THREAD_FLAG_MARRIAGE;
    uintptr_t THREAD_FLAG_INTERACT;

    // === Tier 3: Database singletons ===
    uintptr_t TRAIT_DB;
    uintptr_t INTERACTION_DB;
    uintptr_t LANDED_TITLE_DB;
    uintptr_t WAR_DB;
    uintptr_t FACTION_DB;
    uintptr_t SCHEME_DB;
    uintptr_t PLAYER_CHAR_ID;
    uintptr_t AI_TOGGLE_BASE;
    uintptr_t COMMAND_DISPATCH;

    // === Tier 4: Key functions ===
    uintptr_t PAUSE_SET;
    uintptr_t GET_OPINION_OF;
    uintptr_t CHAR_IS_ALIVE;
    uintptr_t CHAR_IS_RULER;
    uintptr_t CHAR_GET_AI_CONTROLLER;
    uintptr_t GET_PLAYER_CHAR_ID_FN;
    uintptr_t STRATEGY_EXECUTE;
    uintptr_t AI_CHAR_TOGGLE;

    // RTTI vtables (for reference/debugging)
    uintptr_t VTABLE_CAIManager;
    uintptr_t VTABLE_CWar;
    uintptr_t VTABLE_CJominiIdler;
    uintptr_t VTABLE_CActiveScheme;
    uintptr_t VTABLE_CFaction;

    // === Tier 5: Character accessor functions ===
    uintptr_t GOLD_GET;              // FUN_06755990 (220B) — currency switch (0=gold,3=treasury,4=barter)
    uintptr_t PIETY_GET;             // FUN_0674b8c0 — reads *(*(char+0x1a8)+0x110)
    uintptr_t PRESTIGE_GET;          // FUN_0674b8e0 — reads *(*(char+0x1a8)+0x130)
    uintptr_t STRESS_GET;            // FUN_0674b790 (22B) — reads core+0x2F8
    uintptr_t DREAD_GET;             // FUN_0674ba90 (23B) — reads ruler+0x350
    uintptr_t SKILL_GET_BASE;        // FUN_0676c7c0 — fn(char, skill_index) → base skill int32
    uintptr_t SKILL_GET_CALC;        // FUN_0676c800 — fn(char, skill_index) → calculated skill
    uintptr_t TRAIT_LIST_GET;        // FUN_0674f4f0 — fn(char) → struct{+0=array, +0xC=count}
    uintptr_t IS_ALIVE;              // FUN_0674b2c0 (12B) — *(char+0x1c8)==0
    uintptr_t IS_RULER;              // FUN_0674f390 (12B) — *(char+0x1b8)!=0
    uintptr_t GET_FACTION_LIST;      // FUN_06789a10 (29B) — *(*(char+0x1b8)+0x120)

    // === Tier 6: Action dispatch functions ===
    uintptr_t BUILD_INTERACTION_CMD; // FUN_06b3d920 (923B) — build CSendCharacterInteractionCommand
    uintptr_t COMMAND_DISPATCH_FN;   // FUN_07660390 (146B) — game command dispatch
    uintptr_t WAR_SCORE_COMPUTE;     // FUN_066ba0d0 — war score computation [-100,+100]
    uintptr_t INCOME_CALC;           // FUN_06188af0 (431B) — monthly income total with modifiers

    // === Tier 7: Additional RTTI vtables ===
    uintptr_t VTABLE_CCharacterInteractionDB;
    uintptr_t VTABLE_CWarDB;
    uintptr_t VTABLE_CSchemeManager;

    // === Tier 8: Runtime-discovered from instruction decoding ===
    uintptr_t SEND_CMD_VTABLE;           // CSendCharacterInteractionCommand primary vtable
    uintptr_t SEND_CMD_VTABLE2;          // CSendCharacterInteractionCommand secondary vtable (CGameCommandHelper)
    uintptr_t DISCOVERED_DISPATCH_MGR;   // COMMAND_DISPATCH_MGR discovered from GAME_COMMAND_DISPATCH

    // === Tier 9: Anchor functions for RIP-relative global discovery ===
    uintptr_t GAME_IS_LOADED_FN;         // FUN_06094f20 (12B) — cmp qword [rip+GSR], 0; setne; ret
    uintptr_t IS_MANUALLY_PAUSED_FN;     // FUN_03983f10 (14B) — lea rax,[rip+IDLER]; deref; read +0x20
    uintptr_t TRAIT_HAS_FLAG_FN;         // FUN_0674bb70 (233B) — loads TRAIT_DB early in prologue
    uintptr_t AI_LIFESTYLE_DISPATCH_FN;  // FUN_04c89110 — AI lifestyle dispatch, refs AI_TOGGLE_BASE+3

    // Method tracking
    ResolveMethod methods[112];  // how each field was resolved (indexed by field order)

    // Stats
    int total_discovered;
    int total_hardcoded;
    int total_mismatches;
    int total_failed;
    double resolve_time_ms;
};

// RTTI target definition
struct RttiTarget {
    const char* mangled_name;   // e.g. "11CAIManager"
    int vtable_slot;            // which vtable entry (-1 = just find vtable base)
    size_t result_offset;       // offsetof(ResolvedOffsets, field)
    size_t vtable_offset;       // offsetof for vtable storage (0 = don't store)
    const char* label;          // human-readable name
};

// Signature definition
struct SigPattern {
    const char* name;
    const uint8_t* pattern;
    const uint8_t* mask;        // 0xFF = must match, 0x00 = wildcard
    size_t length;
    size_t result_offset;       // offsetof(ResolvedOffsets, field)
};

// RIP-relative anchor definition
struct RipAnchor {
    size_t source_offset;       // offsetof(ResolvedOffsets, source_func)
    int rip_ordinal;            // which RIP-relative ref (0-based)
    size_t target_offset;       // offsetof(ResolvedOffsets, target_global)
    int scan_bytes;             // how far to scan (default 256)
    int offset_adjust;          // added to discovered address (e.g., -3 for toggle base from toggle+3)
    const char* label;
};

// ck-nkyc leg (e). OBSERVE the loaded binary's ELF BuildID and publish it (with
// the process's effective CK3_STRICT_BUILD_ID) into bridge::build_id_witness().
//
// SEPARATE FROM run_resolver() ON PURPOSE, and the reason is a finding rather
// than a preference: run_resolver() IS NEVER CALLED. Nothing in the shipped
// bridge invokes it — only the three Zydis discovery helpers below are used
// (ai_eval_hook.cpp), which is visible in every real bridge.log, none of which
// contains the resolver's "Starting runtime resolver" banner or its "ELF
// BuildID" line. So the BuildID check the bead set out to surface had never run
// in production at all, and wiring the witness to it would have shipped a leg
// that reports `unchecked` forever and refuses every registered episode.
//
// Called from bridge_init()'s "verify we're in the right binary" step, which is
// where the question already belonged.
void observe_build_id(uintptr_t base_addr);

// Main resolver API
struct ResolverContext {
    uintptr_t base_addr;
    MemoryRegion text;          // .text (R+X)
    MemoryRegion rodata;        // .rodata (R only, largest R segment)
    MemoryRegion data_relro;    // .data.rel.ro (R+W, contains vtables)
    MemoryRegion data_bss;      // .data + .bss (R+W)
    bool regions_valid;
};

// Initialize resolver context (map memory regions)
bool init_context(uintptr_t base_addr, ResolverContext* ctx);

// Run the full resolver pipeline
bool run_resolver(uintptr_t base_addr, ResolvedOffsets* out);

// Individual scanners (exposed for testing)
int resolve_rtti_targets(const ResolverContext* ctx, ResolvedOffsets* out);
int resolve_signatures(const ResolverContext* ctx, ResolvedOffsets* out);
int resolve_rip_anchors(const ResolverContext* ctx, ResolvedOffsets* out);

// Populate from hardcoded ck3_offsets.h values
void populate_hardcoded(ResolvedOffsets* out);

// Cross-validate discovered vs hardcoded
int cross_validate(ResolvedOffsets* out);

// Log resolver results
void log_results(const ResolvedOffsets* out);

// Runtime discovery of vtable/global addresses from instruction decoding (Zydis)
// Returns the absolute address (not offset) of the discovered value, or 0 on failure.
uintptr_t discover_send_cmd_vtable(uintptr_t base_addr);
uintptr_t discover_send_cmd_vtable2(uintptr_t base_addr);
uintptr_t discover_dispatch_mgr(uintptr_t base_addr);

// Convenience: run all Zydis-based discoveries and store results in ResolvedOffsets.
// Returns number of successfully discovered values.
int resolve_instruction_refs(uintptr_t base_addr, ResolvedOffsets* out);

} // namespace ck3_resolver
