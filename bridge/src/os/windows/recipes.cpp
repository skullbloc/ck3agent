// os/windows/recipes.cpp — the bridge's own implementations of game functions that
// have no Windows anchor (ADR 0001 P5): bridge::game_fn() returns these for the
// RVAs the generated Windows ck3_offsets.h marks as recipes (recipes.inc).
//
// Each recipe is the Linux function's body (Ghidra decompile of the pinned Linux
// binary, quoted) over Windows offsets, and each offset it reads was MEASURED on
// the Windows build (two cores of the same save, lane-p3probe / lane-p5); a recipe
// that needs an unmeasured offset does not exist. Signatures are the Linux
// functions' (the call sites cast game_fn()'s result to them).
#include "bridge.h"
#include "ck3_offsets.h"
#include "windows/ck3_offsets_win.h"   // ck3_win::gamestate_sub (lane-p1b)

#include <cstdint>
#include <cstring>

namespace bridge {
namespace {

// GetAge(CCharacter*) = int16 [char+0x68]. CCharacter does not move on Windows
// (slot map: delta 0 over 0x60..0x6C on both pairs).
constexpr uintptr_t kCharAge = 0x68;
int16_t recipe_GET_AGE(void* c) {
    return safe_read<int16_t>(reinterpret_cast<uintptr_t>(c) + kCharAge);
}

// GetAI<axis>(CCharacter*) = (int)(signed char)[[core_data]+0x308][axis], 0 with no
// core_data. Windows: PERSONALITY_PTR measured at 0x308 (732/732 characters, the
// nine bytes behind it identical, 702 distinct).
int axis(void* c, int i) {
    const uintptr_t cd = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(c) + ck3::character::REALM_DATA);
    if (!cd) return 0;
    const uintptr_t p = safe_read<uintptr_t>(cd + ck3::core_data::PERSONALITY_PTR);
    return p ? static_cast<int>(safe_read<int8_t>(p + i)) : 0;
}
int recipe_GET_AI_HONOR(void* c)        { return axis(c, 0); }
int recipe_GET_AI_GREED(void* c)        { return axis(c, 1); }
int recipe_GET_AI_RATIONALITY(void* c)  { return axis(c, 2); }
int recipe_GET_AI_ENERGY(void* c)       { return axis(c, 3); }
int recipe_GET_AI_BOLDNESS(void* c)     { return axis(c, 4); }
int recipe_GET_AI_ZEAL(void* c)         { return axis(c, 5); }
int recipe_GET_AI_VENGEFULNESS(void* c) { return axis(c, 6); }
int recipe_GET_AI_COMPASSION(void* c)   { return axis(c, 7); }
int recipe_GET_AI_SOCIABILITY(void* c)  { return axis(c, 8); }

// Army_IsRetreating(CArmy*) = int32 [army+0x16C] != 0. Windows: army_obj::
// MOVEMENT_STATE (the same field) measured at 0x174 (250 armies, 867.07.09).
bool recipe_ARMY_IS_RETREATING(void* a) {
    return safe_read<int32_t>(reinterpret_cast<uintptr_t>(a) + ck3::army_obj::MOVEMENT_STATE) != 0;
}

// GetTruceList(CCharacter*) = [char+0x1C0] + 0x6A0, or the engine's null-truce
// sentinel when the character has no diplomatic data. Windows: TRUCE_LIST_PTR
// measured at 0x6A0 (81/81, the only offset in 0x690..0x6B8 that matches). The
// sentinel here is an empty array of the same shape (all zero: no data, count 0).
alignas(16) unsigned char g_empty_truce_list[0x40] = {};
uintptr_t recipe_GET_TRUCE_LIST(void* c) {
    const uintptr_t dd = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(c) + ck3::character::DIPLOMATIC_DATA);
    return dd ? dd + ck3::diplomatic_data::TRUCE_LIST_PTR : reinterpret_cast<uintptr_t>(g_empty_truce_list);
}

// MurmurHash3_32(seed, data, len): standard MurmurHash3 x86_32 (the Linux function
// is its streaming form). Checked against the game: 1,745 of 1,745 title-type keys
// that carry a hash reproduce it with seed 0 (69 carry 0: generated titles).
uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
uint32_t recipe_STRING_HASH(uint32_t seed, const char* data, uint32_t len) {
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    uint32_t h = seed;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    const uint32_t nblocks = len / 4;
    for (uint32_t i = 0; i < nblocks; ++i) {
        uint32_t k;
        std::memcpy(&k, p + 4 * i, 4);
        k *= c1; k = rotl(k, 15); k *= c2;
        h ^= k; h = rotl(h, 13); h = h * 5 + 0xe6546b64;
    }
    const unsigned char* tail = p + 4 * nblocks;
    uint32_t k = 0;
    switch (len & 3) {
        case 3: k ^= static_cast<uint32_t>(tail[2]) << 16; [[fallthrough]];
        case 2: k ^= static_cast<uint32_t>(tail[1]) << 8;  [[fallthrough]];
        case 1: k ^= tail[0]; k *= c1; k = rotl(k, 15); k *= c2; h ^= k;
    }
    h ^= len;
    h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16;
    return h;
}

// Character_GetAIController(CCharacter*) = [[char+0x1A8]+0x278], or the value of
// the AI_NULL_CONTROLLER global without core data. Inlined by MSVC (lane-p1b,
// re_anchor_transfer.md #16: the same shape at the AI-enable site); core_data::
// AI_CONTROLLER is the path P3 walked to the controller on both builds; the global
// is mapped (win_rva).
void* recipe_CHAR_GET_AI_CONTROLLER(void* c) {
    const uintptr_t cd = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(c) + ck3::character::REALM_DATA);
    if (!cd) return reinterpret_cast<void*>(safe_read<uintptr_t>(g_base_addr + ck3::globals::AI_NULL_CONTROLLER));
    return reinterpret_cast<void*>(safe_read<uintptr_t>(cd + ck3::core_data::AI_CONTROLLER));
}

// simd_find_long(begin, end, value): std::find over 8-byte values (the Linux
// function only dispatches between SSE/AVX variants of it, by a CPU-feature check);
// returns the first element equal to value, or end. The one caller passes the
// value itself (a character pointer) as the third argument.
uintptr_t recipe_SIMD_FIND_LONG(uintptr_t begin, uintptr_t end, void* value) {
    const uint64_t v = reinterpret_cast<uint64_t>(value);
    for (uintptr_t p = begin; p + 8 <= end; p += 8)
        if (safe_read<uint64_t>(p) == v) return p;
    return end;
}

// FUN_06061870(game_state) = [game_state+0xA0] + 0x25378: the army manager, the
// sub-object whose +0x50/+0x5C are the army id array and count. Windows: the
// manager is at +0x25580 under the same +0xA0 block (lane-p1b, gamestate_sub::
// ARMY_MANAGER, HIGH); its id array/count measured unchanged at +0x50/+0x5C (250
// armies, 867.07.09, ids equal one for one).
uintptr_t recipe_ARMY_ENUM_SUB(uintptr_t gs) {
    const uintptr_t block = safe_read<uintptr_t>(gs + 0xA0);
    return block ? block + ck3_win::gamestate_sub::ARMY_MANAGER : 0;
}

// GetOpinionOf(a, b): inlined everywhere on MSVC, so composed here from the two
// game functions it calls and its two caps, exactly as the two inlined copies do
// (ck3_offsets_win.h funcs::OPINION_BASE_CALC). Called on the reader thread while
// paused, as the Linux bridge calls the Linux function.
int recipe_GET_OPINION_OF(void* a, void* b) {
    using fn_base = int (*)(void*, void*, int);
    using fn_mods = int (*)(uintptr_t, void*, void*, int);
    const auto base = reinterpret_cast<fn_base>(g_base_addr + ck3_win::funcs::OPINION_BASE_CALC);
    const auto mods = reinterpret_cast<fn_mods>(g_base_addr + ck3_win::funcs::OPINION_LOOKUP_MODIFIERS);
    int v = base(a, b, 0);
    const uintptr_t core = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(a) + ck3::character::REALM_DATA);
    if (a != b && core) v += mods(core, a, b, 0);
    const int lo = safe_read<int32_t>(g_base_addr + ck3_win::globals::OPINION_MIN);
    const int hi = safe_read<int32_t>(g_base_addr + ck3_win::globals::OPINION_MAX);
    return v < lo ? lo : (v > hi ? hi : v);
}

// Army_InCombat(CArmy*): the combat RefDB entry the army refers to (army_obj::
// COMBAT_REF_ID, measured at 0xF8 on Windows) and that object's virtual slot 2 --
// or the null combat's when the lookup misses. The recipe answers "a live combat
// with that id exists": the null object's answer is false, and the virtual call
// on a live combat is its validity (the slot on the MSVC vtable is not measured,
// so it is not called). Combat ids sit at +0x08 on both builds (18/18 combats,
// 867.07.09). MEDIUM-LOW: the live-combat case equates validity with existence.
bool recipe_ARMY_IN_COMBAT(uintptr_t army) {
    const uint32_t ref = safe_read<uint32_t>(army + ck3::army_obj::COMBAT_REF_ID);
    const uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3_win::refdb::REFDB_COMBAT);
    if (!db) return false;
    const uint32_t idx = ref & 0xFFFFFF;
    if (idx >= safe_read<uint32_t>(db + 0x2C)) return false;
    const uintptr_t obj = safe_read<uintptr_t>(safe_read<uintptr_t>(db + 0x20) + 8 + uintptr_t{idx} * 0x10);
    return obj && safe_read<uint32_t>(obj + 0x08) == ref;
}

// AI_CHAR_TOGGLE(ai_ctrl, enabled): Linux FUN_04e1f190 is five bytes, `*(ctrl+0x28)
// = enabled`. MSVC inlined it: the AI enable command (0xDFE2D0) does exactly that
// store on each character's controller, `mov byte [[[char+0x1A8]+0x278]+0x28], 1`
// (0xDFE59F, 0xDFE6D5; AI_NULL_CONTROLLER when there is no core data), as the
// Linux enable loop calls this function (ck3_offsets_win.h struct_layout::
// AI_CTRL_AI_ENABLED). A store, as on Linux: the caller (do_set_character_ai)
// probes the field readable first (ck-bl15). P6.
void recipe_AI_CHAR_TOGGLE(void* ctrl, uint8_t enabled) {
    *reinterpret_cast<volatile uint8_t*>(reinterpret_cast<uintptr_t>(ctrl) + ck3::ai_ctrl::AI_ENABLED) = enabled;
}

// CB_VALIDITY_CHECK(cb_type, out, attacker, defender, skip): on Linux
// CB_GET_VALID_OPTIONS, a tail call `CB_VALIDITY_CORE(cb_type, attacker, defender,
// out, skip, 0)`. MSVC inlined it; the core is anchored (ck3_offsets_win.h funcs::
// CB_VALIDITY_CORE: bool in al, the skip flag a byte, the option array as on Linux).
// P6.
bool recipe_CB_VALIDITY_CHECK(void* cb_type, void* out, void* attacker, void* defender, int skip) {
    using fn_core = bool (*)(void*, void*, void*, void*, uint8_t, uint64_t);
    const auto core = reinterpret_cast<fn_core>(g_base_addr + ck3_win::funcs::CB_VALIDITY_CORE);
    return core(cb_type, attacker, defender, out, static_cast<uint8_t>(skip), 0);
}

// WAR_SCORE_LANDLESS_THUNK(war, ctx): on Linux a thunk to the 3-argument landless
// score with side 0; MSVC calls that function directly ((war, 0, ctx) at its call
// sites), anchored as ck3_offsets_win.h funcs::WAR_SCORE_LANDLESS. P6.
int recipe_WAR_SCORE_LANDLESS_THUNK(void* war, void* ctx) {
    using fn = int (*)(void*, int, void*);
    return reinterpret_cast<fn>(g_base_addr + ck3_win::funcs::WAR_SCORE_LANDLESS)(war, 0, ctx);
}

// INTERACTION_CTX_RESET(ctx) and INTERACTION_CTX_SET_TARGET(ctx, target, setup):
// MSVC inlined both into AI_INTERACTION_CORE's per-target helper (0x1755AF0), which
// runs, in order: the option-array reset, the flag reset (ctx, 0) -- the Linux
// CTX_RESET's two steps --, then the target store, +0x2F0/+0x2F8 cleared, the scope
// resolution over the five context refs, the flag reset (ctx, 1) and the per-slot
// evaluator -- the Linux CTX_SET_TARGET with setup set. Each callee is anchored
// (ck3_offsets_win.h funcs::INTERACTION_OPTION_ARRAY_RESET ..
// INTERACTION_EVALUATE); the context fields are the Linux ones (struct_layout::
// INTERACTION_CTX_*). The helper itself is not called: after the set-target it
// goes on into the AI's own decision (0x17558A0). P6.
void recipe_INTERACTION_CTX_RESET(uintptr_t* ctx) {
    using fn_reset = void (*)(uintptr_t*);
    using fn_flag = void (*)(uintptr_t*, uint8_t);
    reinterpret_cast<fn_reset>(g_base_addr + ck3_win::funcs::INTERACTION_OPTION_ARRAY_RESET)(ctx);
    reinterpret_cast<fn_flag>(g_base_addr + ck3_win::funcs::INTERACTION_FLAG_RESET_AND_SCOPE_SETUP)(ctx, 0);
}
void recipe_INTERACTION_CTX_SET_TARGET(uintptr_t* ctx, uint32_t target, char setup) {
    using fn_resolve = void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    using fn_flag = void (*)(uintptr_t*, uint8_t);
    using fn_eval = uint8_t (*)(uintptr_t*);
    const uintptr_t c = reinterpret_cast<uintptr_t>(ctx);
    *reinterpret_cast<uint32_t*>(c + ck3::interaction_ctx::TARGET_CHAR_REF) = target;
    *reinterpret_cast<uint32_t*>(c + ck3::interaction_ctx::EVAL_FLAGS_PTR) = 0;       // as the helper: a
    *reinterpret_cast<uint64_t*>(c + ck3::interaction_ctx::EVAL_FLAGS_PTR + 8) = 0;   // count, then the data
    namespace ic = ck3::interaction_ctx;
    reinterpret_cast<fn_resolve>(g_base_addr + ck3_win::funcs::INTERACTION_RESOLVE_SCOPES)(
        *ctx, c + ic::ACTOR_CHAR_REF, c + ic::TARGET_CHAR_REF, c + ic::SECONDARY_ACTOR_REF,
        c + ic::SECONDARY_RECIPIENT_REF, c + ic::TERTIARY_REF);
    if (setup) {
        reinterpret_cast<fn_flag>(g_base_addr + ck3_win::funcs::INTERACTION_FLAG_RESET_AND_SCOPE_SETUP)(ctx, 1);
        reinterpret_cast<fn_eval>(g_base_addr + ck3_win::funcs::INTERACTION_EVALUATE)(ctx);
    }
}

// BUILD_SEND_CMD_FROM_BUF(cmd, ctx): Linux BuildSendCmdFromBuf, the
// CSendCharacterInteractionCommand constructor from an interaction context:
// CJominiCommandBase_ctor(cmd, 0) (+0x8 byte, +0xC qword, +0x14 dword: 0), the two
// vtables, the deep copy of the context into +0x20, then +0x358 and +0x360, each a
// context value converted for the actor ([ctx+0x2D8]). Inlined at every use on MSVC;
// this follows the copy in the AI send path (ck3_offsets_win.h funcs::
// SEND_INTERACTION_CMD_VTABLE): the two value functions return through a hidden slot
// (ctx, out) and the conversion is (out, value, id), where Linux returns both in rax.
// P6, lane-p6d.
void recipe_BUILD_SEND_CMD_FROM_BUF(void* out, uintptr_t ctx) {
    using fn_copy = void (*)(uintptr_t, uintptr_t);
    using fn_value = int64_t* (*)(uintptr_t, int64_t*);
    using fn_for_char = int64_t* (*)(int64_t*, int64_t, uint32_t);
    const uintptr_t c = reinterpret_cast<uintptr_t>(out);
    *reinterpret_cast<uint8_t*>(c + 0x8) = 0;
    *reinterpret_cast<uint64_t*>(c + 0xC) = 0;
    *reinterpret_cast<uint32_t*>(c + 0x14) = 0;
    *reinterpret_cast<uintptr_t*>(c) = g_base_addr + ck3_win::funcs::SEND_INTERACTION_CMD_VTABLE;
    *reinterpret_cast<uintptr_t*>(c + 0x18) = g_base_addr + ck3_win::funcs::SEND_INTERACTION_CMD_VTABLE2;
    reinterpret_cast<fn_copy>(g_base_addr + ck3_win::funcs::INTERACTION_CTX_DEEP_COPY)(c + 0x20, ctx);
    const uint32_t actor = *reinterpret_cast<uint32_t*>(ctx + ck3::interaction_ctx::ACTOR_CHAR_REF);
    const auto for_char = reinterpret_cast<fn_for_char>(g_base_addr + ck3_win::funcs::SEND_CMD_VALUE_FOR_CHAR);
    int64_t v = 0;
    reinterpret_cast<fn_value>(g_base_addr + ck3_win::funcs::SEND_CMD_VALUE_TERTIARY)(ctx, &v);
    for_char(reinterpret_cast<int64_t*>(c + 0x358), v, actor);
    reinterpret_cast<fn_value>(g_base_addr + ck3_win::funcs::SEND_CMD_VALUE_TARGET)(ctx, &v);
    for_char(reinterpret_cast<int64_t*>(c + 0x360), v, actor);
}

const uintptr_t kRecipes[] = {
#define RECIPE(name) reinterpret_cast<uintptr_t>(&recipe_##name),
#include "recipes.inc"
#undef RECIPE
};
constexpr uintptr_t kRecipeTag = 0x7FE0000000000000ull;   // CK3_WIN_RECIPE(i) = tag | i
constexpr std::size_t kRecipeCount = sizeof(kRecipes) / sizeof(kRecipes[0]);

}  // namespace

uintptr_t game_fn_win(uintptr_t rva) {
    if ((rva & 0xFFFF000000000000ull) == kRecipeTag) {
        const std::size_t i = static_cast<std::size_t>(rva & 0xFFFF);
        // An index past the table is a header generated from a longer recipes.inc
        // than this build's: the address is left to fault, never guessed.
        return i < kRecipeCount ? kRecipes[i] : g_base_addr + rva;
    }
    return g_base_addr + rva;
}

}  // namespace bridge
