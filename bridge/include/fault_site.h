#pragma once
// ck-duv5: the ONE spelling of a faulting instruction, shared by the SIGSEGV
// handler (bridge_init.cpp, three log sites) and by the probe that grades it
// (bridge/test/fault_location_probe.cpp).
//
// It lives here rather than as a lambda in the handler for one reason: a
// formatter whose REFUSAL branch never executes is a branch nobody has ever
// seen the output of. Inside the handler that branch needs a fault with no
// ucontext or an unset base — neither is producible on demand. Out here both
// branches are a function call, so the probe RUNS them.
//
// Async-signal-safety: snprintf is not formally async-signal-safe, and neither
// is the fprintf this feeds. That is pre-existing on all three handler paths
// (ck-e3dp/ck-26fm) and is NOT changed here; flagged, not fixed. No allocation,
// no locks, caller-owned buffer.
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <ucontext.h>

// Ghidra IMAGE VA = file offset + 0x100000.
inline constexpr uintptr_t kGhidraImageBias = 0x100000;

// Writes "rip=... rip_off=... Ghidra ... base=..." into buf and returns buf.
//
// When the offset cannot be computed it does NOT print 0x0. A well-formed zero
// is indistinguishable from a real fault at base+0, which is precisely the
// failure the arm-A investigation kept hitting: a number that cannot say
// whether it was measured. The unresolved case is spelled UNRESOLVED and names
// WHICH input was missing.
inline const char* format_fault_site(char* buf, size_t n,
                                     const void* rip, uintptr_t base) {
    if (rip != nullptr && base != 0) {
        const uintptr_t off = reinterpret_cast<uintptr_t>(rip) - base;
        snprintf(buf, n, "rip=%p rip_off=0x%lx Ghidra ~0x%lx base=0x%lx",
                 rip, (unsigned long)off,
                 (unsigned long)(off + kGhidraImageBias), (unsigned long)base);
    } else {
        snprintf(buf, n,
                 "rip=%p rip_off=UNRESOLVED Ghidra=UNRESOLVED base=0x%lx (%s)",
                 rip, (unsigned long)base,
                 rip == nullptr ? "no ucontext" : "base address unknown");
    }
    return buf;
}

// ── THE REGISTERS AT THE FAULT ──────────────────────────────────────────────
// rip says WHICH instruction. For the shape both arm-A crashes had — a fault at
// a small constant (0x1a) or at 0 — the instruction is a field load off a
// pointer, and the pointer itself is sitting in a register. That register names
// WHICH OBJECT was null, which is the question "0x1a" could not answer and the
// one three dead theories were each guessing at.
//
// Costs one more ucontext read on a path that is already dying. Printed as one
// line; a crash log gets exactly one of these, ever.
inline const char* format_fault_regs(char* buf, size_t n, const void* ctx) {
    if (!ctx) { snprintf(buf, n, "regs=UNAVAILABLE (no ucontext)"); return buf; }
    const auto* uc = static_cast<const ucontext_t*>(ctx);
    const auto* g  = uc->uc_mcontext.gregs;
    snprintf(buf, n,
             "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx "
             "rbp=%llx rsp=%llx r8=%llx r9=%llx r10=%llx r11=%llx "
             "r12=%llx r13=%llx r14=%llx r15=%llx",
             (unsigned long long)g[REG_RAX], (unsigned long long)g[REG_RBX],
             (unsigned long long)g[REG_RCX], (unsigned long long)g[REG_RDX],
             (unsigned long long)g[REG_RSI], (unsigned long long)g[REG_RDI],
             (unsigned long long)g[REG_RBP], (unsigned long long)g[REG_RSP],
             (unsigned long long)g[REG_R8],  (unsigned long long)g[REG_R9],
             (unsigned long long)g[REG_R10], (unsigned long long)g[REG_R11],
             (unsigned long long)g[REG_R12], (unsigned long long)g[REG_R13],
             (unsigned long long)g[REG_R14], (unsigned long long)g[REG_R15]);
    return buf;
}

// ── ck-65lv: THE WORD AT [rsp], EMITTED UNINTERPRETED ─────────────────────
//
// ck-duv5's first live capture resolved to GetPrestige with the ip at its FIRST
// instruction. It named the instruction and could not name the CALLER, because
// GetPrestige has 16 callers and the caller's identity is in the return address.
// At a function's first instruction the return address is exactly at [rsp] —
// nothing has been pushed yet.
//
// MY FIRST CUT GATED THE READ ON ip_is_at_entry AND THE ONLY SHIPPED CALLER
// PASSES FALSE, because a signal handler cannot know function bounds. So it
// would have printed NOT_AT_ENTRY on every real capture and named nobody: the
// capability present, the only caller that could use it passing false. ck-cg3b's
// asymmetry, inside the fix for ck-duv5's own instrument, caught by ck-t5bc
// counting call sites — and I had already told them it would name the caller.
//
// SO THE VALUE IS ALWAYS EMITTED AND NEVER MISLABELLED. The word is reported as
// what it literally is — the machine word at [rsp] — with its image offset when
// it has one. An analyst resolves it exactly as they already resolve rip. The
// line states the interpretation rule rather than asserting the interpretation:
// at a function ENTRY this word IS the return address; at any other ip it is a
// frame word, still worth reasoning about, and not claimed to be more.
//
// That keeps the guard's purpose — nothing is labelled as something it is not —
// while delivering what the five captures actually wanted.
//
// The read cannot fault: rsp during a SIGSEGV can be anything, so it goes
// through a caller-supplied probe (the bridge passes safe_read_bytes, which is
// process_vm_readv) rather than a dereference.
// ── ck-2ptx COMPLETED (found reviewing the ladder, 2026-09-22): THE RANGE HAS
// TWO ENDS. `word > base` alone let a STACK or HEAP address — above the base by
// construction — render with an off= and a Ghidra VA, in a line whose header
// calls such words return-address candidates; the dare's own register values
// gave three of eight. A word is in the image iff base <= word < image_end. The
// extent is captured once at init beside the base (max PT_LOAD end); when it is
// unknown (0) the classification falls back to base + 512 MB — the image is
// ~170 MB and every stack address seen is terabytes away — and the line SAYS
// "(extent assumed)" so a reader knows which rule classified the word.
inline constexpr uintptr_t kAssumedImageSpan = 512UL << 20;

inline bool fault_word_in_image(uintptr_t word, uintptr_t base, uintptr_t image_end,
                                bool* assumed) {
    if (!base) { if (assumed) *assumed = false; return false; }
    uintptr_t end = image_end;
    const bool fallback = !(image_end > base);
    if (fallback) end = base + kAssumedImageSpan;
    if (assumed) *assumed = fallback;
    return word >= base && word < end;
}

inline const char* format_frame_word(char* buf, size_t n, uintptr_t rsp,
                                     bool (*probe)(uintptr_t, void*, size_t),
                                     uintptr_t base, uintptr_t image_end) {
    if (!rsp || !probe) {
        snprintf(buf, n, "rsp=0x%lx [rsp]=UNREADABLE (no rsp or no probe)",
                 (unsigned long)rsp);
        return buf;
    }
    uintptr_t word = 0;
    if (!probe(rsp, &word, sizeof word)) {
        snprintf(buf, n, "rsp=0x%lx [rsp]=UNREADABLE (probe refused)",
                 (unsigned long)rsp);
        return buf;
    }
    bool assumed = false;
    if (fault_word_in_image(word, base, image_end, &assumed)) {
        snprintf(buf, n,
                 "rsp=0x%lx [rsp]=0x%lx [rsp]_off=0x%lx Ghidra ~0x%lx "
                 "(AT A FUNCTION ENTRY THIS IS THE RETURN ADDRESS; elsewhere a "
                 "frame word)%s",
                 (unsigned long)rsp, (unsigned long)word,
                 (unsigned long)(word - base),
                 (unsigned long)(word - base + kGhidraImageBias),
                 assumed ? " (extent assumed)" : "");
    } else {
        // ck-2ptx's rule, BOTH ends: a value below the base or beyond the image
        // end is not an offset into it, so the subtraction is refused rather
        // than rendered.
        snprintf(buf, n,
                 "rsp=0x%lx [rsp]=0x%lx [rsp]_off=UNRESOLVED (outside the image "
                 "at base=0x%lx end=0x%lx%s)",
                 (unsigned long)rsp, (unsigned long)word, (unsigned long)base,
                 (unsigned long)(image_end > base ? image_end : base + kAssumedImageSpan),
                 assumed ? ", extent assumed" : "");
    }
    return buf;
}

// ── ck-65lv (the dare, 2026-09-22): THE LADDER — N WORDS ABOVE rsp ────────────
//
// The one-word capture named the caller of FUN_0784a4a0 (an engine lock-owner
// read passed a pointer that held 0xa) and could not name who called THAT: the
// return address one frame up is the only route to where a lock pointer became
// ten, and the format carried exactly one word. A reproduction on the same
// instrument buys a second sample of the same word. So the ladder: the eight
// machine words at [rsp], [rsp+8], … [rsp+0x38], each probed INDEPENDENTLY
// through the same fault-tolerant reader as the first and each labelled as what
// it literally is. Rules, all inherited from the one-word line and from ck-2ptx:
//   * every word goes through `probe`; never a dereference — the handler is
//     already recovering from one fault and a second inside it is the
//     chained-FATAL path;
//   * bounded and allocation-free: a fixed count, a caller-supplied buffer,
//     snprintf with the remaining length tracked;
//   * each slot reports its own readability — an UNREADABLE slot is printed
//     beside the ones that resolved, never a silent stop at the first bad word,
//     because a truncated chain that looks complete is the defect this exists
//     to end;
//   * an in-image word carries its offset and Ghidra VA; anything else prints
//     as a raw value with off=UNRESOLVED, so a plausible-looking number never
//     stands where a measurement belongs;
//   * the one-word line is untouched: this is a SECOND line, and the dare's
//     log and any recurrence stay on one timeline.
// Interpretation is the analyst's: at a function entry the first word is the
// return address; the rest are frame words and saved registers of the frames
// above, some of which are return addresses — the in-image offsets are the
// candidates. The line states that rule and asserts nothing more.
inline constexpr size_t kStackLadderWords = 8;

inline const char* format_stack_ladder(char* buf, size_t n, uintptr_t rsp,
                                       bool (*probe)(uintptr_t, void*, size_t),
                                       uintptr_t base, uintptr_t image_end) {
    if (n == 0) return buf;
    size_t used = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (used >= n) return;
        int w = snprintf(buf + used, n - used, fmt, args...);
        if (w < 0) { buf[used] = '\0'; used = n; return; }
        used += (size_t)w < n - used ? (size_t)w : n - used - 1;
    };
    if (!rsp || !probe) {
        put("ladder=UNREADABLE (no rsp or no probe)");
        return buf;
    }
    bool assumed_hdr = false;
    fault_word_in_image(base, base, image_end, &assumed_hdr);
    put("ladder(rsp=0x%lx, %u words, image [0x%lx,0x%lx)%s; in-image words are "
        "RETURN-ADDRESS CANDIDATES):",
        (unsigned long)rsp, (unsigned)kStackLadderWords, (unsigned long)base,
        (unsigned long)(image_end > base ? image_end : base + kAssumedImageSpan),
        assumed_hdr ? " (extent assumed)" : "");
    for (size_t k = 0; k < kStackLadderWords; ++k) {
        const uintptr_t addr = rsp + k * sizeof(uintptr_t);
        uintptr_t word = 0;
        if (!probe(addr, &word, sizeof word)) {
            put(" [rsp+0x%02lx]=UNREADABLE", (unsigned long)(k * sizeof(uintptr_t)));
            continue;                         // the NEXT slot is still probed
        }
        if (fault_word_in_image(word, base, image_end, nullptr)) {
            put(" [rsp+0x%02lx]=0x%lx off=0x%lx ~0x%lx",
                (unsigned long)(k * sizeof(uintptr_t)), (unsigned long)word,
                (unsigned long)(word - base),
                (unsigned long)(word - base + kGhidraImageBias));
        } else {
            put(" [rsp+0x%02lx]=0x%lx off=UNRESOLVED",
                (unsigned long)(k * sizeof(uintptr_t)), (unsigned long)word);
        }
    }
    return buf;
}
