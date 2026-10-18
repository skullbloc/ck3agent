// CK3 Signature Database for Cross-Patch Offset Recovery
// ======================================================
//
// Each function/global gets a signature entry with a byte pattern and wildcard mask.
// The scanner searches the binary's .text section for these patterns and returns
// the virtual address of each match.
//
// SIGNATURE FORMAT:
//   pattern[] = raw bytes from the function prologue
//   mask[]    = 0xFF for bytes that must match exactly, 0x00 for wildcards
//
// WILDCARD RULES:
//   - Instruction opcodes and register encodings: FIXED (0xFF)
//   - Struct field offsets (e.g., [rdi+0x1a8]): FIXED (these are the offsets we verify)
//   - RIP-relative displacements (4 bytes after LEA/MOV/CMP [rip+disp32]): WILDCARD (0x00)
//   - CALL rel32 targets (4 bytes): WILDCARD (0x00)
//   - Short jump offsets (1 byte): WILDCARD on distance, FIXED on opcode
//   - Stack frame sizes in SUB RSP: FIXED (distinctive per function)
//   - Immediate constants (e.g., mov esi, 0x4): FIXED
//
// ADDING NEW SIGNATURES:
//   1. Find the function in Ghidra or via binary dump
//   2. Extract the first 12-32 bytes of the prologue
//   3. Identify variable bytes (RIP-relative offsets, call targets) and mask them
//   4. Test uniqueness: scan the full .text section to ensure exactly 1 match
//   5. Add the entry to the appropriate tier in the sig_database[] array
//
// VERIFICATION:
//   After scanning, the resolver compares discovered addresses against the hardcoded
//   offsets in ck3_offsets.h. Mismatches indicate either a patch-shifted function
//   (expected) or a false positive (needs a longer/better signature).
//
// Generated from binary BuildID: e61ff2642f07094defd140fcc98e078d42541275
// Confirmed identical for BuildID: a22ee733...
//
#pragma once
#include "runtime_resolver.h"
#include <cstdint>
#include <cstddef>

namespace ck3_signatures {

// ============================================================================
// Signature type flags
// ============================================================================
enum class SigType : uint8_t {
    FUNCTION = 0,   // Direct function address
    GLOBAL   = 1,   // Global variable (resolved via RIP-relative ref from a function)
    VTABLE   = 2,   // VTable base (resolved via RTTI)
    STRUCT_VERIFY = 3,  // Used to verify struct layout (match confirms field offsets)
};

// ============================================================================
// Signature entry
// ============================================================================
struct SignatureEntry {
    const char* name;           // Human-readable name (matches ck3_offsets.h naming)
    const uint8_t* pattern;     // Byte pattern
    const uint8_t* mask;        // 0xFF = must match, 0x00 = wildcard
    size_t length;              // Pattern length in bytes
    size_t result_offset;       // offsetof(ResolvedOffsets, field) for storing result
    int offset_adjust;          // Add to match address (usually 0)
    SigType type;               // How to interpret the match
    const char* description;    // What this function does
};

// ============================================================================
// Tier 1: Critical AI Core Functions
// These MUST be found for the bridge to function at all.
// ============================================================================

// AI_MANAGER_MAIN_TICK (0x04ceb5e0, ~3221 bytes)
// Prologue: push rbp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x158
// Then LEA rax, [rip+AI_ENABLED_FLAG] ; cmp byte [rax], 0
// Bytes 13-16 are the stack frame size (0x158), bytes 17-19 are LEA opcode,
// bytes 20-23 are RIP-relative displacement (wildcarded)
static const uint8_t PAT_AI_MANAGER_MAIN_TICK[] = {
    0x55,                               // push rbp
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x41, 0x55,                         // push r13
    0x41, 0x54,                         // push r12
    0x53,                               // push rbx
    0x48, 0x81, 0xEC,                   // sub rsp, imm32
    0x58, 0x01, 0x00, 0x00,             // 0x158 (stack frame — distinctive)
    0x48, 0x8D, 0x05,                   // lea rax, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 (wildcard — points to AI_ENABLED_FLAG)
    0x80, 0x38, 0x00,                   // cmp byte [rax], 0
    0x0F, 0x84,                         // je rel32
};
static const uint8_t MSK_AI_MANAGER_MAIN_TICK[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,             // stack frame size: FIXED (distinctive)
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

// AI_MANAGER_POST_EXECUTE (0x04cedfd0, ~1972 bytes)
// Heavy prologue + distinctive: sub rsp 0x3b8, then lea rdi [rsp+0xa8], mov esi 8
static const uint8_t PAT_AI_MANAGER_POST_EXECUTE[] = {
    0x55,                               // push rbp
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x41, 0x55,                         // push r13
    0x41, 0x54,                         // push r12
    0x53,                               // push rbx
    0x48, 0x81, 0xEC,                   // sub rsp, imm32
    0xB8, 0x03, 0x00, 0x00,             // 0x3b8 (distinctive large stack frame)
    0x49, 0x89, 0xFE,                   // mov r14, rdi
    0x48, 0x8D, 0xBC, 0x24,             // lea rdi, [rsp+imm32]
    0xA8, 0x00, 0x00, 0x00,             // 0xa8
    0xBE, 0x08, 0x00, 0x00, 0x00,       // mov esi, 8
};
static const uint8_t MSK_AI_MANAGER_POST_EXECUTE[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,             // stack frame: FIXED
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// AI_INTERACTION_DECISION (0x04cc4b90, ~large)
// Distinctive: same heavy prologue but sub rsp, 0x9f8; then mov [rsp+0x24], edx
static const uint8_t PAT_AI_INTERACTION_DECISION[] = {
    0x55,                               // push rbp
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x41, 0x55,                         // push r13
    0x41, 0x54,                         // push r12
    0x53,                               // push rbx
    0x48, 0x81, 0xEC,                   // sub rsp, imm32
    0xF8, 0x09, 0x00, 0x00,             // 0x9f8 (very large stack frame)
    0x89, 0x54, 0x24,                   // mov [rsp+0x24], edx
};
static const uint8_t MSK_AI_INTERACTION_DECISION[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,             // stack frame: FIXED
    0xFF, 0xFF, 0xFF,
};

// AI_WAR_TARGET_EVAL (0x04d22820)
// push rbx; mov rbx, rdi; mov al, [rip+disp32]; test al, al; je +N
// Then checks char+0x20 and char+0xb8
static const uint8_t PAT_AI_WAR_TARGET_EVAL[] = {
    0x53,                               // push rbx
    0x48, 0x89, 0xFB,                   // mov rbx, rdi
    0x8A, 0x05,                         // mov al, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 (wildcard)
    0x84, 0xC0,                         // test al, al
    0x74, 0x00,                         // je +N (wildcard offset)
    0x48, 0x8B,                         // mov rax, [rbx+0x20]
};
static const uint8_t MSK_AI_WAR_TARGET_EVAL[] = {
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF, 0xFF,
    0xFF, 0x00,                         // jump offset: WILDCARD
    0xFF, 0xFF,
};

// AI_CHAR_TOGGLE (0x04d1f190, 5 bytes + padding + next function start)
// mov [rdi+0x28], sil; ret; <nop padding>; next: mov rax,[rdi+0x20]; test rax,rax
static const uint8_t PAT_AI_CHAR_TOGGLE[] = {
    0x40, 0x88, 0x77, 0x28, 0xC3,       // mov [rdi+0x28], sil; ret
    0x66, 0x2E, 0x0F, 0x1F, 0x84,       // NOP padding (distinctive 15-byte NOP prefix)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x90,  // ... rest of NOP
    0x48, 0x8B, 0x47, 0x20,             // next fn: mov rax, [rdi+0x20]
    0x48, 0x85, 0xC0,                   // test rax, rax
};
static const uint8_t MSK_AI_CHAR_TOGGLE[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// ============================================================================
// Tier 2: Core Infrastructure Functions
// ============================================================================

// STRING_HASH — MurmurHash3 variant (0x08113140)
// push rbx; sub rsp, 0x10; mov ebx, edx; mov rdx, rsi; mov [rsp+0xc], edi;
// mov dword [rsp+0x8], 0; lea rdi, [rsp+0xc]; lea rsi, [rsp+0x8]
static const uint8_t PAT_STRING_HASH[] = {
    0x53,                               // push rbx
    0x48, 0x83, 0xEC, 0x10,             // sub rsp, 0x10
    0x89, 0xD3,                         // mov ebx, edx
    0x48, 0x89, 0xF2,                   // mov rdx, rsi
    0x89, 0x7C, 0x24, 0x0C,             // mov [rsp+0xc], edi
    0xC7, 0x44, 0x24, 0x08,             // mov dword [rsp+0x8], imm32
    0x00, 0x00,                         // 0x00000000 (seed=0)
};
static const uint8_t MSK_STRING_HASH[] = {
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

// CHAR_ID_TO_PTR (0x037df190)
// NOTE: This function uses the generic TPdxRefDatabase lookup pattern shared by ~80
// database accessors (character, war, army, scheme, etc.). Pure byte-pattern scanning
// cannot distinguish them. CHAR_ID_TO_PTR is resolved via RIP-anchor tracing instead:
// find a unique caller that references CHARACTER_DB, then trace the RIP-relative ref.
// This entry is kept for documentation but NOT included in the scan database.
// Resolution path: RTTI(CAIManager vtable) -> AI_MANAGER_MAIN_TICK -> CHARACTER_DB global
//                  -> CHAR_ID_TO_PTR (first function that loads CHARACTER_DB)
//
// Pattern (for reference only):
// 48 8B 05 [RIP] 48 85 C0 74 [N] 8B 0F 89 CA 81 E2 FF FF FF 00 3B 50 2C

// GET_PLAYER_CHAR_ID (0x061642e0, 7 bytes + padding + next function)
// mov eax, [rip+PLAYER_CHAR_ID]; ret; <NOP>; next: mov edx,[rdi+0x18]; mov [rip+disp],edx
// The next function (SetPlayerCharID) writes to the same global — very distinctive pair
static const uint8_t PAT_GET_PLAYER_CHAR_ID[] = {
    0x8B, 0x05,                         // mov eax, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 -> PLAYER_CHAR_ID (wildcard)
    0xC3,                               // ret
    0x66, 0x0F, 0x1F, 0x84,             // NOP padding (9-byte NOP)
    0x00, 0x00, 0x00, 0x00, 0x00,       // ...
    0x8B, 0x57, 0x18,                   // next fn: mov edx, [rdi+0x18]
    0x89, 0x15,                         // mov [rip+disp32], edx
    0x00, 0x00, 0x00, 0x00,             // disp32 (wildcard)
    0x48, 0x8D, 0x05,                   // lea rax, [rip+disp32]
};
static const uint8_t MSK_GET_PLAYER_CHAR_ID[] = {
    0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF,
};

// GAME_COMMAND_DISPATCH (0x07660390)
// This is mid-function but has a distinctive pattern:
// cld; mov rbx, rax; mov rax, [rsp+0x10]; test rax, rax; je +N; mov rdi, rsp; mov rsi, rdi
static const uint8_t PAT_GAME_COMMAND_DISPATCH[] = {
    0xFC,                               // cld
    0x48, 0x89, 0xC3,                   // mov rbx, rax
    0x48, 0x8B, 0x44, 0x24, 0x10,       // mov rax, [rsp+0x10]
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x74, 0x0D,                         // je +0x0D
    0x48, 0x89, 0xE7,                   // mov rdi, rsp
    0x48, 0x89, 0xFE,                   // mov rsi, rdi (actually mov rsi, rdi pattern)
};
static const uint8_t MSK_GAME_COMMAND_DISPATCH[] = {
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,                         // jump offset: FIXED (short, distinctive)
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// BUILD_SEND_INTERACTION_CMD (Ghidra FUN_06b3d920, bridge 0x06a3d920, 923 bytes)
// Mid-function entry; distinctive pattern with stack access
// mov rbp, [rsp+0x70]; mov dword [rsp+0x7c], 0; mov rdi, [rsp+0x80]; mov rax, [rdi]
static const uint8_t PAT_BUILD_SEND_INTERACTION_CMD[] = {
    0x48, 0x8B, 0x6C, 0x24, 0x70,       // mov rbp, [rsp+0x70]
    0xC7, 0x44, 0x24, 0x7C,             // mov dword [rsp+0x7c], imm32
    0x00, 0x00, 0x00, 0x00,             // 0 (constant)
    0x48, 0x8B, 0xBC, 0x24,             // mov rdi, [rsp+imm32]
    0x80, 0x00, 0x00, 0x00,             // 0x80
    0x48, 0x8B, 0x07,                   // mov rax, [rdi]
};
static const uint8_t MSK_BUILD_SEND_INTERACTION_CMD[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// ============================================================================
// Tier 3: Character Accessor Functions
// These are small, distinctive functions. Their patterns also verify struct layout.
// ============================================================================

// CHAR_IS_ALIVE (12 bytes + 4 NOP padding + next fn start)
// cmp qword [rdi+0x1c8], 0; sete al; ret; NOP; next fn: mov rax,[rdi+0x1a8]
static const uint8_t PAT_CHAR_IS_ALIVE[] = {
    0x48, 0x83, 0xBF,                   // cmp qword [rdi+disp32]
    0xC8, 0x01, 0x00, 0x00,             // 0x1C8 (death_data offset)
    0x00,                               // 0 (compare value)
    0x0F, 0x94, 0xC0,                   // sete al
    0xC3,                               // ret
    0x0F, 0x1F, 0x40, 0x00,             // NOP dword [rax+0x0] (4-byte NOP)
    0x48, 0x8B, 0x87,                   // next fn: mov rax, [rdi+0x1a8]
    0xA8, 0x01, 0x00, 0x00,             // 0x1A8 (core_data — next fn is HasCoreData)
};
static const uint8_t MSK_CHAR_IS_ALIVE[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// CHAR_IS_RULER (12 bytes + 4 NOP + next fn start)
// cmp qword [rdi+0x1b8], 0; setne al; ret; NOP; next: mov esi,[rsi+0x18]; jmp rel32
static const uint8_t PAT_CHAR_IS_RULER[] = {
    0x48, 0x83, 0xBF,                   // cmp qword [rdi+disp32]
    0xB8, 0x01, 0x00, 0x00,             // 0x1B8 (ruler_data offset)
    0x00,                               // 0
    0x0F, 0x95, 0xC0,                   // setne al
    0xC3,                               // ret
    0x0F, 0x1F, 0x40, 0x00,             // NOP padding
    0x8B, 0x76, 0x18,                   // next fn: mov esi, [rsi+0x18]
};
static const uint8_t MSK_CHAR_IS_RULER[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// GET_STRESS (18 bytes) — mov rax, [rdi+0x1a8]; test rax, rax; je +7; mov eax, [rax+0x2F8]; ret
static const uint8_t PAT_GET_STRESS[] = {
    0x48, 0x8B, 0x87,                   // mov rax, [rdi+disp32]
    0xA8, 0x01, 0x00, 0x00,             // 0x1A8 (core_data offset)
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x74, 0x00,                         // je +N (wildcard)
    0x8B, 0x80,                         // mov eax, [rax+disp32]
    0xF8, 0x02, 0x00, 0x00,             // 0x2F8 (stress field in core_data)
};
static const uint8_t MSK_GET_STRESS[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,                         // je offset: WILDCARD
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// GET_DREAD (19+4 bytes) — includes ret + null fallback: xor eax,eax; ret
static const uint8_t PAT_GET_DREAD[] = {
    0x48, 0x8B, 0x87,                   // mov rax, [rdi+disp32]
    0xB8, 0x01, 0x00, 0x00,             // 0x1B8 (ruler_data offset)
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x74, 0x00,                         // je +N (wildcard)
    0x48, 0x8B, 0x80,                   // mov rax, [rax+disp32]
    0x50, 0x03, 0x00, 0x00,             // 0x350 (dread field in ruler_data)
    0xC3,                               // ret
    0x31, 0xC0,                         // xor eax, eax (null path)
    0xC3,                               // ret
};
static const uint8_t MSK_GET_DREAD[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF,
    0xFF,
};

// PIETY_GET (19+4 bytes) — includes ret + null fallback
static const uint8_t PAT_PIETY_GET[] = {
    0x48, 0x8B, 0x87,
    0xA8, 0x01, 0x00, 0x00,             // 0x1A8 (core_data)
    0x48, 0x85, 0xC0,
    0x74, 0x00,                         // je +N
    0x48, 0x8B, 0x80,
    0x10, 0x01, 0x00, 0x00,             // 0x110 (piety field)
    0xC3,                               // ret
    0x31, 0xC0,                         // xor eax, eax
    0xC3,                               // ret
};
static const uint8_t MSK_PIETY_GET[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF,
    0xFF,
};

// PRESTIGE_GET (19+4 bytes) — includes ret + null fallback
static const uint8_t PAT_PRESTIGE_GET[] = {
    0x48, 0x8B, 0x87,
    0xA8, 0x01, 0x00, 0x00,
    0x48, 0x85, 0xC0,
    0x74, 0x00,
    0x48, 0x8B, 0x80,
    0x30, 0x01, 0x00, 0x00,             // 0x130 (prestige field)
    0xC3,                               // ret
    0x31, 0xC0,                         // xor eax, eax
    0xC3,                               // ret
};
static const uint8_t MSK_PRESTIGE_GET[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF,
    0xFF,
};

// GOLD_GET (20 bytes) — push rbx; mov rax, [rdi+0x1a8]; test rax, rax; je far; cmp esi, 4
static const uint8_t PAT_GOLD_GET[] = {
    0x53,                               // push rbx
    0x48, 0x8B, 0x87,                   // mov rax, [rdi+disp32]
    0xA8, 0x01, 0x00, 0x00,             // 0x1A8 (core_data)
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x0F, 0x84,                         // je rel32 (far jump)
    0x00, 0x00, 0x00, 0x00,             // rel32 (wildcard)
    0x83, 0xFE, 0x04,                   // cmp esi, 4 (currency_type check)
};
static const uint8_t MSK_GOLD_GET[] = {
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // je target: WILDCARD
    0xFF, 0xFF, 0xFF,
};

// SKILL_GET_BASE (14 bytes) — push r14; push rbx; push rax; mov r14, rdi; movsxd rbx, esi; cmp ebx, 6
static const uint8_t PAT_SKILL_GET_BASE[] = {
    0x41, 0x56,                         // push r14
    0x53,                               // push rbx
    0x50,                               // push rax
    0x49, 0x89, 0xFE,                   // mov r14, rdi
    0x48, 0x63, 0xDE,                   // movsxd rbx, esi
    0x83, 0xFB, 0x06,                   // cmp ebx, 6 (skill count bound check)
    0x72,                               // jb +N
};
static const uint8_t MSK_SKILL_GET_BASE[] = {
    0xFF, 0xFF,
    0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF,
};

// TRAIT_GET_LIST (8 bytes + NOP + next function)
// lea rax, [rdi+0xF0]; ret; <16-byte NOP>; next: push rbx; mov rbx, [rdi+0x1b8]
static const uint8_t PAT_TRAIT_GET_LIST[] = {
    0x48, 0x8D, 0x87,                   // lea rax, [rdi+disp32]
    0xF0, 0x00, 0x00, 0x00,             // 0xF0 (trait list offset)
    0xC3,                               // ret
    0x0F, 0x1F, 0x84, 0x00,             // NOP (8-byte NOP prefix)
    0x00, 0x00, 0x00, 0x00,             // NOP continuation
    0x53,                               // next fn: push rbx
    0x48, 0x8B, 0x9F,                   // mov rbx, [rdi+disp32]
    0xB8, 0x01, 0x00, 0x00,             // 0x1B8 (ruler_data)
};
static const uint8_t MSK_TRAIT_GET_LIST[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// GET_FACTION_LIST (18 bytes)
// mov rax, [rdi+0x1b8]; test rax, rax; lea rax, [rax+0x120]; lea rcx, [rip+sentinel]
static const uint8_t PAT_GET_FACTION_LIST[] = {
    0x48, 0x8B, 0x87,                   // mov rax, [rdi+disp32]
    0xB8, 0x01, 0x00, 0x00,             // 0x1B8 (ruler_data)
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x48, 0x8D, 0x80,                   // lea rax, [rax+disp32]
    0x20, 0x01, 0x00, 0x00,             // 0x120 (faction list offset)
    0x48,                               // REX prefix for next LEA
};
static const uint8_t MSK_GET_FACTION_LIST[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
};

// MIL_CURRENT (18 bytes) — mov rax, [rdi+0x1b8]; test rax, rax; je +7; mov eax, [rax+0x2F0]; ret
static const uint8_t PAT_MIL_CURRENT[] = {
    0x48, 0x8B, 0x87,
    0xB8, 0x01, 0x00, 0x00,             // ruler_data
    0x48, 0x85, 0xC0,
    0x74, 0x00,                         // je +N (wildcard)
    0x8B, 0x80,
    0xF0, 0x02, 0x00, 0x00,             // 0x2F0 (mil current)
};
static const uint8_t MSK_MIL_CURRENT[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// MIL_MAX (18 bytes) — same pattern, offset 0x2F4
static const uint8_t PAT_MIL_MAX[] = {
    0x48, 0x8B, 0x87,
    0xB8, 0x01, 0x00, 0x00,
    0x48, 0x85, 0xC0,
    0x74, 0x00,
    0x8B, 0x80,
    0xF4, 0x02, 0x00, 0x00,             // 0x2F4 (mil max)
};
static const uint8_t MSK_MIL_MAX[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// AI_PERSONALITY_HONOR (22 bytes) — generic personality getter pattern
// mov rax, [rdi+0x1a8]; test rax, rax; je +0xb; mov rax, [rax+0x308]; movsx eax, byte [rax]; ret
// The byte index (0 for honor) is embedded in [rax+0] — but for honor it's just [rax] which is movsx eax, byte [rax]
static const uint8_t PAT_AI_PERSONALITY_HONOR[] = {
    0x48, 0x8B, 0x87,
    0xA8, 0x01, 0x00, 0x00,             // 0x1A8 (core_data)
    0x48, 0x85, 0xC0,
    0x74, 0x00,                         // je +N
    0x48, 0x8B, 0x80,
    0x08, 0x03, 0x00, 0x00,             // 0x308 (AI personality array ptr in core_data)
    0x0F, 0xBE, 0x00,                   // movsx eax, byte [rax+0] (honor = index 0)
};
static const uint8_t MSK_AI_PERSONALITY_HONOR[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// ============================================================================
// Tier 4: Key Game Functions
// ============================================================================

// PAUSE_SET (16 bytes)
// push rbp; push r15; push r14; push rbx; sub rsp, 0x108; mov r14d, edx; mov ebp, esi; mov rbx, rdi
static const uint8_t PAT_PAUSE_SET[] = {
    0x55,                               // push rbp
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x53,                               // push rbx
    0x48, 0x81, 0xEC,                   // sub rsp, imm32
    0x08, 0x01, 0x00, 0x00,             // 0x108 (stack frame)
    0x41, 0x89, 0xD6,                   // mov r14d, edx
};
static const uint8_t MSK_PAUSE_SET[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// GET_OPINION_OF (16 bytes)
// push rbp; push r14; push rbx; mov r14, rsi; mov rbx, rdi; xor edx, edx; call rel32
static const uint8_t PAT_GET_OPINION_OF[] = {
    0x55,                               // push rbp
    0x41, 0x56,                         // push r14
    0x53,                               // push rbx
    0x49, 0x89, 0xF6,                   // mov r14, rsi
    0x48, 0x89, 0xFB,                   // mov rbx, rdi
    0x31, 0xD2,                         // xor edx, edx
    0xE8,                               // call rel32
    0x00, 0x00, 0x00, 0x00,             // rel32 (wildcard)
};
static const uint8_t MSK_GET_OPINION_OF[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0x00, 0x00, 0x00, 0x00,             // call target: WILDCARD
};

// WAR_COMPUTE_SCORE (16 bytes)
// push rbp; push r15; push r14; push r13; push r12; push rbx; push rax; mov rbx, rsi; mov r14, rdi
static const uint8_t PAT_WAR_COMPUTE_SCORE[] = {
    0x55,                               // push rbp
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x41, 0x55,                         // push r13
    0x41, 0x54,                         // push r12
    0x53,                               // push rbx
    0x50,                               // push rax
    0x48, 0x89, 0xF3,                   // mov rbx, rsi
    0x49, 0x89,                         // mov r14, rdi (partial)
};
static const uint8_t MSK_WAR_COMPUTE_SCORE[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

// INCOME_MODIFIER_CALC (16 bytes)
// push r15; push r14; push r13; push r12; push rbx; mov r15, rdx; mov r14, rdi; call rel32
static const uint8_t PAT_INCOME_MODIFIER_CALC[] = {
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x41, 0x55,                         // push r13
    0x41, 0x54,                         // push r12
    0x53,                               // push rbx
    0x49, 0x89, 0xD7,                   // mov r15, rdx
    0x49, 0x89, 0xFE,                   // mov r14, rdi (actually the byte says 0xfe)
    0xE8,                               // call rel32
};
static const uint8_t MSK_INCOME_MODIFIER_CALC[] = {
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF,
};

// STRATEGY_EXECUTE (18 bytes)
// push rbp; push r15..r12; push rbx; sub rsp, 0x18; movsxd r13, dword [rdi+0x5c]
static const uint8_t PAT_STRATEGY_EXECUTE[] = {
    0x55,                               // push rbp
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x41, 0x55,                         // push r13
    0x41, 0x54,                         // push r12
    0x53,                               // push rbx
    0x48, 0x83, 0xEC, 0x18,             // sub rsp, 0x18
    0x4C, 0x63, 0x6F, 0x5C,            // movsxd r13, dword [rdi+0x5c]
};
static const uint8_t MSK_STRATEGY_EXECUTE[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// IS_ADULT (16 bytes)
// push rbx; movsx ebx, word [rdi+0x68]; movzx edi, byte [rdi+0x199]; call rel32
static const uint8_t PAT_IS_ADULT[] = {
    0x53,                               // push rbx
    0x0F, 0xBF, 0x5F, 0x68,            // movsx ebx, word [rdi+0x68]
    0x0F, 0xB6, 0xBF,                   // movzx edi, byte [rdi+disp32]
    0x99, 0x01, 0x00, 0x00,             // 0x199 (age_type offset)
    0xE8,                               // call rel32
    0x00, 0x00, 0x00,                   // (partial wildcard)
};
static const uint8_t MSK_IS_ADULT[] = {
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0x00, 0x00, 0x00,
};

// ============================================================================
// Tier 5: AI Dispatch Functions
// These are the per-character AI subsystem dispatchers called from the main tick.
// They check AI toggle flags (DAT_0bfa5c78+N) to decide which subsystems run.
// The toggle flag addresses change across patches but the CMP byte ptr [rip+disp], 0
// pattern + the specific toggle sequence is distinctive.
// ============================================================================

// AI_MARRIAGES_DISPATCH (0x04c88fe0)
// push rbp; push r14; push rbx; mov r14d, esi; mov rbx, rdi; cmp byte [rip+toggle], 0
static const uint8_t PAT_AI_MARRIAGES_DISPATCH[] = {
    0x55,                               // push rbp
    0x41, 0x56,                         // push r14
    0x53,                               // push rbx
    0x41, 0x89, 0xF6,                   // mov r14d, esi
    0x48, 0x89, 0xFB,                   // mov rbx, rdi
    0x80, 0x3D,                         // cmp byte [rip+disp32], imm8
};
static const uint8_t MSK_AI_MARRIAGES_DISPATCH[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

// AI_COURT_ACTIVITY_DISPATCH (0x04c88eb0)
// push rbp; push r14; push rbx; mov ebp, edx; mov rbx, rsi; mov r14, rdi
static const uint8_t PAT_AI_COURT_ACTIVITY_DISPATCH[] = {
    0x55,                               // push rbp
    0x41, 0x56,                         // push r14
    0x53,                               // push rbx
    0x89, 0xD5,                         // mov ebp, edx
    0x48, 0x89, 0xF3,                   // mov rbx, rsi
    0x49, 0x89, 0xFE,                   // mov r14, rdi
};
static const uint8_t MSK_AI_COURT_ACTIVITY_DISPATCH[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// AI_GRANT_REVOKE_1 (0x04c89530)
// push rbp; push rbx; push rax; mov ebp, esi; mov rbx, rdi; mov esi, 4; call; mov rdi, rbx; mov esi, 4
// The double "mov esi, 4" is distinctive
static const uint8_t PAT_AI_GRANT_REVOKE_1[] = {
    0x55,                               // push rbp
    0x53,                               // push rbx
    0x50,                               // push rax
    0x89, 0xF5,                         // mov ebp, esi
    0x48, 0x89, 0xFB,                   // mov rbx, rdi
    0xBE, 0x04, 0x00, 0x00, 0x00,       // mov esi, 4
    0xE8, 0x00, 0x00, 0x00, 0x00,       // call (wildcard)
    0x48, 0x89, 0xDF,                   // mov rdi, rbx
    0xBE, 0x04, 0x00, 0x00, 0x00,       // mov esi, 4 (again!)
    0x89, 0xC2,                         // mov edx, eax
};
static const uint8_t MSK_AI_GRANT_REVOKE_1[] = {
    0xFF,
    0xFF,
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0x00, 0x00,       // call target: WILDCARD
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

// AI_GRANT_REVOKE_2 (0x04c89580)
// Same pattern but mov esi, 5
static const uint8_t PAT_AI_GRANT_REVOKE_2[] = {
    0x55, 0x53, 0x50,
    0x89, 0xF5,
    0x48, 0x89, 0xFB,
    0xBE, 0x05,                         // mov esi, 5 (distinctive!)
};
static const uint8_t MSK_AI_GRANT_REVOKE_2[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

// AI_TAX_DISPATCH (0x04c895d0)
// push rbx; mov rbx, rdi; mov esi, 7; call rel32
static const uint8_t PAT_AI_TAX_DISPATCH[] = {
    0x53,                               // push rbx
    0x48, 0x89, 0xFB,                   // mov rbx, rdi
    0xBE, 0x07, 0x00, 0x00, 0x00,       // mov esi, 7 (distinctive constant!)
    0xE8,                               // call rel32
};
static const uint8_t MSK_AI_TAX_DISPATCH[] = {
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
};

// ============================================================================
// Tier 6: Global Anchor Functions
// These signatures exist to anchor RIP-relative tracing to critical globals.
// The primary value is not the function itself but the globals it references.
// ============================================================================

// GAME_IS_LOADED (0x05f94f20, 12 bytes + NOP + next fn)
// cmp qword [rip+GAME_STATE_ROOT], 0; setne al; ret; NOP; lea rax, [rip+GAME_STATE_ROOT]; ret
// Two tiny functions that both reference GAME_STATE_ROOT — very distinctive pair.
// RIP anchor ordinal 0 → GAME_STATE_ROOT
static const uint8_t PAT_GAME_IS_LOADED[] = {
    0x48, 0x83, 0x3D,                   // cmp qword [rip+disp32], imm8
    0x00, 0x00, 0x00, 0x00,             // disp32 → GAME_STATE_ROOT (wildcard)
    0x00,                               // 0 (compare value)
    0x0F, 0x95, 0xC0,                   // setne al
    0xC3,                               // ret
    0x0F, 0x1F, 0x40, 0x00,             // NOP dword [rax+0] (4-byte NOP)
    0x48, 0x8D, 0x05,                   // next fn: lea rax, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 → GAME_STATE_ROOT (wildcard)
    0xC3,                               // ret
};
static const uint8_t MSK_GAME_IS_LOADED[] = {
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF,
};

// IS_MANUALLY_PAUSED (0x03883f10, 14 bytes + 2-byte NOP + next fn start)
// lea rax, [rip+IDLER_SINGLETON]; mov rax, [rax]; mov al, [rax+0x20]; ret; NOP
// Double-deref through IDLER_SINGLETON global pointer, reading pause_flag at +0x20.
// RIP anchor ordinal 0 → IDLER_SINGLETON
static const uint8_t PAT_IS_MANUALLY_PAUSED[] = {
    0x48, 0x8D, 0x05,                   // lea rax, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 → IDLER_SINGLETON (wildcard)
    0x48, 0x8B, 0x00,                   // mov rax, [rax]
    0x8A, 0x40, 0x20,                   // mov al, [rax+0x20] (pause_flag offset)
    0xC3,                               // ret
    0x66, 0x90,                         // NOP (2-byte padding)
    0x53,                               // next fn: push rbx
    0x48, 0x8D, 0x1D,                   // lea rbx, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 → same IDLER_SINGLETON (wildcard)
};
static const uint8_t MSK_IS_MANUALLY_PAUSED[] = {
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,                   // +0x20 offset: FIXED (pause flag)
    0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
};

// TRAIT_HAS_FLAG (FUN_0674bb70 = bridge 0x0664bb70, 233 bytes)
// Heavy push prologue + immediate load of TRAIT_DB from global.
// push rbp; push r15..r12; push rbx; push rax; mov r14, rdi; mov r13, [rip+TRAIT_DB]; test r13, r13
// RIP anchor ordinal 0 → TRAIT_DB
static const uint8_t PAT_TRAIT_HAS_FLAG[] = {
    0x55,                               // push rbp
    0x41, 0x57,                         // push r15
    0x41, 0x56,                         // push r14
    0x41, 0x55,                         // push r13
    0x41, 0x54,                         // push r12
    0x53,                               // push rbx
    0x50,                               // push rax
    0x49, 0x89, 0xFE,                   // mov r14, rdi
    0x4C, 0x8B, 0x2D,                   // mov r13, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 → TRAIT_DB (wildcard)
    0x4D, 0x85, 0xED,                   // test r13, r13
    0x75, 0x00,                         // jnz +N (wildcard offset)
};
static const uint8_t MSK_TRAIT_HAS_FLAG[] = {
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,                         // jnz offset: WILDCARD
};

// CHAR_GET_AI_CONTROLLER (0x06680040, 31 bytes — full function)
// mov rax, [rdi+0x1a8]; test rax, rax; lea rax, [rax+0x278];
// lea rcx, [rip+AI_NULL_CONTROLLER]; cmovz rax, rcx; mov rax, [rax]; ret
// Verifies: core_data at char+0x1A8, AI controller at core_data+0x278
static const uint8_t PAT_CHAR_GET_AI_CONTROLLER[] = {
    0x48, 0x8B, 0x87,                   // mov rax, [rdi+disp32]
    0xA8, 0x01, 0x00, 0x00,             // 0x1A8 (core_data)
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x48, 0x8D, 0x80,                   // lea rax, [rax+disp32]
    0x78, 0x02, 0x00, 0x00,             // 0x278 (AI controller in core_data)
    0x48, 0x8D, 0x0D,                   // lea rcx, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 → AI_NULL_CONTROLLER (wildcard)
    0x48, 0x0F, 0x44, 0xC1,             // cmovz rax, rcx
    0x48, 0x8B, 0x00,                   // mov rax, [rax]
    0xC3,                               // ret
};
static const uint8_t MSK_CHAR_GET_AI_CONTROLLER[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,             // core_data offset: FIXED
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,             // AI controller offset: FIXED
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF,
};

// AI_LIFESTYLE_DISPATCH (0x04c89110)
// push rbp; push rbx; push rax; mov ebp, esi; mov rbx, rdi;
// cmp byte [rip+AI_TOGGLE_BASE+3], 0; jz +N; ...
// Then immediately checks next toggle: cmp byte [rip+AI_TOGGLE_BASE+4], 0
// RIP anchor ordinal 0 → AI_TOGGLE_BASE+3 (adjust -3 at resolver level)
static const uint8_t PAT_AI_LIFESTYLE_DISPATCH[] = {
    0x55,                               // push rbp
    0x53,                               // push rbx
    0x50,                               // push rax
    0x89, 0xF5,                         // mov ebp, esi
    0x48, 0x89, 0xFB,                   // mov rbx, rdi
    0x80, 0x3D,                         // cmp byte [rip+disp32], imm8
    0x00, 0x00, 0x00, 0x00,             // disp32 → AI_TOGGLE_BASE+3 (wildcard)
    0x00,                               // 0 (compare value)
    0x74, 0x00,                         // jz +N (wildcard)
    0x48, 0x89, 0xDF,                   // mov rdi, rbx
    0x89, 0xEE,                         // mov esi, ebp
    0xE8,                               // call rel32
    0x00, 0x00, 0x00, 0x00,             // rel32 (wildcard)
    0x80, 0x3D,                         // cmp byte [rip+disp32], imm8 (next toggle)
};
static const uint8_t MSK_AI_LIFESTYLE_DISPATCH[] = {
    0xFF,
    0xFF,
    0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF,
    0xFF, 0x00,                         // jz offset: WILDCARD
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF,
    0x00, 0x00, 0x00, 0x00,             // call target: WILDCARD
    0xFF, 0xFF,
};

// TITLE_GET_HOLDER (0x060c3ed0, struct verify — title holder lookup via CHARACTER_DB)
// mov rax, [rip+CHARACTER_DB]; test rax, rax; jz +N; mov ecx, [rdi+0x270]; ...
// Verifies: title holder ref at title+0x270, CHARACTER_DB hash table layout
static const uint8_t PAT_TITLE_GET_HOLDER[] = {
    0x48, 0x8B, 0x05,                   // mov rax, [rip+disp32]
    0x00, 0x00, 0x00, 0x00,             // disp32 → CHARACTER_DB (wildcard)
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x74, 0x00,                         // jz +N (wildcard)
    0x8B, 0x8F,                         // mov ecx, [rdi+disp32]
    0x70, 0x02, 0x00, 0x00,             // 0x270 (title holder char ref)
    0x89, 0xCA,                         // mov edx, ecx
    0x81, 0xE2,                         // and edx, imm32
    0xFF, 0xFF, 0xFF, 0x00,             // 0x00FFFFFF (24-bit ID mask)
    0x3B, 0x50, 0x2C,                   // cmp edx, [rax+0x2c] (DB count)
};
static const uint8_t MSK_TITLE_GET_HOLDER[] = {
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00,             // RIP disp: WILDCARD
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00,                         // jz offset: WILDCARD
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,             // holder offset: FIXED (0x270)
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,             // mask: FIXED (0x00FFFFFF)
    0xFF, 0xFF, 0xFF,
};

// ============================================================================
// Master signature database array
// ============================================================================

#define SIG_ENTRY(name, pat, msk, field, adj, stype, desc) \
    { name, pat, msk, sizeof(pat), offsetof(ck3_resolver::ResolvedOffsets, field), adj, stype, desc }

// SIG_SCAN_ONLY: scan and log, but don't store (no field in ResolvedOffsets)
#define SIG_SCAN_ONLY(name, pat, msk, adj, stype, desc) \
    { name, pat, msk, sizeof(pat), 0, adj, stype, desc }

static const SignatureEntry sig_database[] = {
    // Tier 1: Critical AI — stored in ResolvedOffsets
    SIG_ENTRY("AI_MANAGER_MAIN_TICK",      PAT_AI_MANAGER_MAIN_TICK,      MSK_AI_MANAGER_MAIN_TICK,
              AI_MANAGER_MAIN_TICK,      0, SigType::FUNCTION, "Main AI tick loop (iterates all AI chars)"),
    SIG_ENTRY("AI_MANAGER_POST_EXECUTE",   PAT_AI_MANAGER_POST_EXECUTE,   MSK_AI_MANAGER_POST_EXECUTE,
              AI_MANAGER_POST_EXECUTE,   0, SigType::FUNCTION, "Post-execute phase of AI tick"),
    SIG_ENTRY("AI_CHAR_TOGGLE",            PAT_AI_CHAR_TOGGLE,            MSK_AI_CHAR_TOGGLE,
              AI_CHAR_TOGGLE,            0, SigType::FUNCTION, "Per-character AI enable/disable"),

    // Tier 1b: Scan-only (no ResolvedOffsets field, logged for validation)
    SIG_SCAN_ONLY("AI_INTERACTION_DECISION", PAT_AI_INTERACTION_DECISION, MSK_AI_INTERACTION_DECISION,
                  0, SigType::FUNCTION, "Core AI interaction decision maker"),
    SIG_SCAN_ONLY("AI_WAR_TARGET_EVAL",    PAT_AI_WAR_TARGET_EVAL,        MSK_AI_WAR_TARGET_EVAL,
                  0, SigType::FUNCTION, "AI war target evaluator"),

    // Tier 2: Core infrastructure — stored
    SIG_ENTRY("STRING_HASH",               PAT_STRING_HASH,               MSK_STRING_HASH,
              STRING_HASH,               0, SigType::FUNCTION, "MurmurHash3-32 string hash"),
    // CHAR_ID_TO_PTR: resolved via RIP-anchor tracing (TPdxRefDatabase pattern not unique)
    SIG_ENTRY("GET_PLAYER_CHAR_ID",        PAT_GET_PLAYER_CHAR_ID,        MSK_GET_PLAYER_CHAR_ID,
              GET_PLAYER_CHAR_ID_FN,     0, SigType::FUNCTION, "Get current player character ID"),
    SIG_ENTRY("GAME_COMMAND_DISPATCH",     PAT_GAME_COMMAND_DISPATCH,     MSK_GAME_COMMAND_DISPATCH,
              COMMAND_DISPATCH_FN,       0, SigType::FUNCTION, "Game command dispatch entry"),
    SIG_ENTRY("BUILD_SEND_INTERACTION_CMD",PAT_BUILD_SEND_INTERACTION_CMD,MSK_BUILD_SEND_INTERACTION_CMD,
              BUILD_INTERACTION_CMD,     0, SigType::FUNCTION, "Build CSendCharacterInteractionCommand"),

    // Tier 3: Character accessors — stored + verify struct layout
    SIG_ENTRY("CHAR_IS_ALIVE",             PAT_CHAR_IS_ALIVE,             MSK_CHAR_IS_ALIVE,
              IS_ALIVE,                  0, SigType::STRUCT_VERIFY, "IsAlive — verifies death_data at +0x1C8"),
    SIG_ENTRY("CHAR_IS_RULER",             PAT_CHAR_IS_RULER,             MSK_CHAR_IS_RULER,
              IS_RULER,                  0, SigType::STRUCT_VERIFY, "IsRuler — verifies ruler_data at +0x1B8"),
    SIG_ENTRY("GET_STRESS",                PAT_GET_STRESS,                MSK_GET_STRESS,
              STRESS_GET,                0, SigType::STRUCT_VERIFY, "GetStress — verifies core_data+0x2F8"),
    SIG_ENTRY("GET_DREAD",                 PAT_GET_DREAD,                 MSK_GET_DREAD,
              DREAD_GET,                 0, SigType::STRUCT_VERIFY, "GetDread — verifies ruler_data+0x350"),
    SIG_ENTRY("PIETY_GET",                 PAT_PIETY_GET,                 MSK_PIETY_GET,
              PIETY_GET,                 0, SigType::STRUCT_VERIFY, "GetPiety — verifies core_data+0x110"),
    SIG_ENTRY("PRESTIGE_GET",              PAT_PRESTIGE_GET,              MSK_PRESTIGE_GET,
              PRESTIGE_GET,              0, SigType::STRUCT_VERIFY, "GetPrestige — verifies core_data+0x130"),
    SIG_ENTRY("GOLD_GET",                  PAT_GOLD_GET,                  MSK_GOLD_GET,
              GOLD_GET,                  0, SigType::FUNCTION, "GetGold — currency switch function"),
    SIG_ENTRY("SKILL_GET_BASE",            PAT_SKILL_GET_BASE,            MSK_SKILL_GET_BASE,
              SKILL_GET_BASE,            0, SigType::FUNCTION, "GetSkillBase — bounds-checked skill access"),
    SIG_ENTRY("TRAIT_GET_LIST",            PAT_TRAIT_GET_LIST,            MSK_TRAIT_GET_LIST,
              TRAIT_LIST_GET,            0, SigType::STRUCT_VERIFY, "GetTraitList — verifies trait array at +0xF0"),
    SIG_ENTRY("GET_FACTION_LIST",          PAT_GET_FACTION_LIST,          MSK_GET_FACTION_LIST,
              GET_FACTION_LIST,          0, SigType::STRUCT_VERIFY, "GetFactionList — verifies ruler+0x120"),
    SIG_ENTRY("AI_PERSONALITY_HONOR",      PAT_AI_PERSONALITY_HONOR,      MSK_AI_PERSONALITY_HONOR,
              AI_PERSONALITY_HONOR,      0, SigType::STRUCT_VERIFY, "AI personality honor — verifies core+0x308"),

    // Tier 3b: Struct verify scan-only (no dedicated field, verify layout only)
    SIG_SCAN_ONLY("MIL_CURRENT",           PAT_MIL_CURRENT,               MSK_MIL_CURRENT,
                  0, SigType::STRUCT_VERIFY, "MilCurrent — verifies ruler+0x2F0"),
    SIG_SCAN_ONLY("MIL_MAX",              PAT_MIL_MAX,                   MSK_MIL_MAX,
                  0, SigType::STRUCT_VERIFY, "MilMax — verifies ruler+0x2F4"),

    // Tier 4: Key game functions — stored
    SIG_ENTRY("PAUSE_SET",                 PAT_PAUSE_SET,                 MSK_PAUSE_SET,
              PAUSE_SET,                 0, SigType::FUNCTION, "Set pause state"),
    SIG_ENTRY("GET_OPINION_OF",            PAT_GET_OPINION_OF,            MSK_GET_OPINION_OF,
              GET_OPINION_OF,            0, SigType::FUNCTION, "Get clamped opinion score"),
    SIG_ENTRY("WAR_COMPUTE_SCORE",         PAT_WAR_COMPUTE_SCORE,         MSK_WAR_COMPUTE_SCORE,
              WAR_SCORE_COMPUTE,         0, SigType::FUNCTION, "Compute war score [-100,+100]"),
    SIG_ENTRY("INCOME_MODIFIER_CALC",      PAT_INCOME_MODIFIER_CALC,      MSK_INCOME_MODIFIER_CALC,
              INCOME_CALC,               0, SigType::FUNCTION, "Monthly income with modifiers"),
    SIG_ENTRY("STRATEGY_EXECUTE",          PAT_STRATEGY_EXECUTE,          MSK_STRATEGY_EXECUTE,
              STRATEGY_EXECUTE,          0, SigType::FUNCTION, "Strategy/goal execute"),

    // Tier 4b: Scan-only functions (important to verify but no ResolvedOffsets field)
    SIG_SCAN_ONLY("IS_ADULT",             PAT_IS_ADULT,                  MSK_IS_ADULT,
                  0, SigType::FUNCTION, "IsAdult check"),

    // Tier 5: AI dispatch functions — scan-only (for validation across patches)
    SIG_SCAN_ONLY("AI_MARRIAGES_DISPATCH", PAT_AI_MARRIAGES_DISPATCH,     MSK_AI_MARRIAGES_DISPATCH,
                  0, SigType::FUNCTION, "AI marriages subsystem dispatcher"),
    SIG_SCAN_ONLY("AI_COURT_ACTIVITY",     PAT_AI_COURT_ACTIVITY_DISPATCH,MSK_AI_COURT_ACTIVITY_DISPATCH,
                  0, SigType::FUNCTION, "AI court activity dispatcher"),
    SIG_SCAN_ONLY("AI_GRANT_REVOKE_1",    PAT_AI_GRANT_REVOKE_1,         MSK_AI_GRANT_REVOKE_1,
                  0, SigType::FUNCTION, "AI grant/revoke titles (phase 1)"),
    SIG_SCAN_ONLY("AI_GRANT_REVOKE_2",    PAT_AI_GRANT_REVOKE_2,         MSK_AI_GRANT_REVOKE_2,
                  0, SigType::FUNCTION, "AI grant/revoke titles (phase 2)"),
    SIG_SCAN_ONLY("AI_TAX_DISPATCH",      PAT_AI_TAX_DISPATCH,           MSK_AI_TAX_DISPATCH,
                  0, SigType::FUNCTION, "AI tax collector dispatcher"),

    // Tier 6: Global anchor functions — stored for RIP-relative tracing
    SIG_ENTRY("GAME_IS_LOADED",            PAT_GAME_IS_LOADED,            MSK_GAME_IS_LOADED,
              GAME_IS_LOADED_FN,         0, SigType::FUNCTION, "GameIsLoaded — anchors GAME_STATE_ROOT"),
    SIG_ENTRY("IS_MANUALLY_PAUSED",        PAT_IS_MANUALLY_PAUSED,        MSK_IS_MANUALLY_PAUSED,
              IS_MANUALLY_PAUSED_FN,     0, SigType::FUNCTION, "IsManuallyPaused — anchors IDLER_SINGLETON"),
    SIG_ENTRY("TRAIT_HAS_FLAG",            PAT_TRAIT_HAS_FLAG,            MSK_TRAIT_HAS_FLAG,
              TRAIT_HAS_FLAG_FN,         0, SigType::FUNCTION, "TraitHasFlag — anchors TRAIT_DB"),
    SIG_ENTRY("CHAR_GET_AI_CONTROLLER",    PAT_CHAR_GET_AI_CONTROLLER,    MSK_CHAR_GET_AI_CONTROLLER,
              CHAR_GET_AI_CONTROLLER,    0, SigType::STRUCT_VERIFY, "GetAIController — verifies core+0x278"),
    SIG_ENTRY("AI_LIFESTYLE_DISPATCH",     PAT_AI_LIFESTYLE_DISPATCH,     MSK_AI_LIFESTYLE_DISPATCH,
              AI_LIFESTYLE_DISPATCH_FN,  0, SigType::FUNCTION, "AI lifestyle dispatch — anchors AI_TOGGLE_BASE"),

    // Tier 6b: Struct verify scan-only
    SIG_SCAN_ONLY("TITLE_GET_HOLDER",      PAT_TITLE_GET_HOLDER,          MSK_TITLE_GET_HOLDER,
                  0, SigType::STRUCT_VERIFY, "TitleGetHolder — verifies title+0x270, DB+0x2C"),

    // Sentinel
    { nullptr, nullptr, nullptr, 0, 0, 0, SigType::FUNCTION, nullptr },
};

static constexpr int NUM_SIGNATURES = (sizeof(sig_database) / sizeof(sig_database[0])) - 1;  // exclude sentinel

// ============================================================================
// Scanner API
// ============================================================================

struct ScanResult {
    const char* name;
    uintptr_t address;      // 0 if not found
    int confidence;          // 100 = unique match, 50 = multiple matches (ambiguous)
    int match_count;         // number of matches found
    SigType type;
};

// Scan the .text section for all signatures in sig_database.
// Returns vector-like results via output array.
// max_results = size of results array, returns actual count.
int scan_all_signatures(
    const uint8_t* text_start,
    size_t text_size,
    uintptr_t text_base,
    ScanResult* results,
    int max_results
);

// Scan for a single signature. Returns the match address (0 if not found).
// If count_out is non-null, stores the total number of matches (>1 means ambiguous).
uintptr_t scan_single_signature(
    const uint8_t* text_start,
    size_t text_size,
    uintptr_t text_base,
    const SignatureEntry* sig,
    int* count_out = nullptr
);

// Integration with RuntimeResolver — scan all signatures and store results
// in the ResolvedOffsets struct. Returns number of new offsets resolved.
int resolve_signatures_v2(
    const ck3_resolver::ResolverContext* ctx,
    ck3_resolver::ResolvedOffsets* out
);

} // namespace ck3_signatures
