// The game thread's interaction context and its construction guard, read from
// AI_INTERACTION_CORE's prologue. One routine, so the bridge's init and its
// engine-free test decode the same way (bridge/test/tls_prologue_scan_probe.cpp).
//
// The prologue (ELF 0x4cc4562, binary MD5 09ba163b...):
//   cmpb $0x0, %fs:-0x3740        the guard: the FS-relative CMP's displacement
//   mov  %fs:0x0, %rax            the thread pointer ...
//   lea  -0x3a78(%rax), %rbp      ... minus the context's distance
#pragma once

#include <Zydis/Zydis.h>

#include <cstddef>
#include <cstdint>

namespace bridge {

struct TlsPrologue {
    uintptr_t guard   = 0;   // fs_base - guard   is the uint8 construction guard
    uintptr_t context = 0;   // fs_base - context is the context object
    size_t guard_at   = 0;   // instruction offsets, for the log
    size_t context_at = 0;
};

// True when both were found in the first n bytes; out holds what was found
// either way (0 for a part not found).
inline bool scan_tls_prologue(const uint8_t* code, size_t n, TlsPrologue* out) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    TlsPrologue r;
    ZydisRegister tp_reg = ZYDIS_REGISTER_NONE;   // holds fs:[0] once loaded
    size_t off = 0;
    while (off < n && (r.guard == 0 || r.context == 0)) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + off, n - off, &insn, ops))) {
            off++;
            continue;
        }
        const int k = insn.operand_count_visible;
        if (r.guard == 0 && insn.mnemonic == ZYDIS_MNEMONIC_CMP && k == 2 &&
            ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            ops[0].mem.segment == ZYDIS_REGISTER_FS &&
            ops[0].mem.base == ZYDIS_REGISTER_NONE && ops[0].mem.disp.value < 0) {
            r.guard = static_cast<uintptr_t>(-ops[0].mem.disp.value);
            r.guard_at = off;
        } else if (insn.mnemonic == ZYDIS_MNEMONIC_MOV && k == 2 &&
                   ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                   ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   ops[1].mem.segment == ZYDIS_REGISTER_FS &&
                   ops[1].mem.base == ZYDIS_REGISTER_NONE && ops[1].mem.disp.value == 0) {
            tp_reg = ops[0].reg.value;
        } else if (r.context == 0 && insn.mnemonic == ZYDIS_MNEMONIC_LEA && k == 2 &&
                   tp_reg != ZYDIS_REGISTER_NONE &&
                   ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[1].mem.base == tp_reg &&
                   ops[1].mem.index == ZYDIS_REGISTER_NONE && ops[1].mem.disp.value < 0) {
            r.context = static_cast<uintptr_t>(-ops[1].mem.disp.value);
            r.context_at = off;
        }
        off += insn.length;
    }
    *out = r;
    return r.guard != 0 && r.context != 0 && r.guard < 0x20000 && r.context < 0x20000;
}

}  // namespace bridge
