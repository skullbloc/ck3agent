#pragma once
// engine_bool.h — calling a game function whose C++ return type is `bool`.
//
// x86-64 (System V and Microsoft alike) returns a `bool` in AL. Bits 8..63 of RAX
// are whatever the callee left there, and the engine leaves a lot: CanCreateTitle
// (FUN_0621a500) ends `setne cl; mov eax, ecx` with RCX holding whatever its last
// callee returned, so a refused create-title came back as RAX = 0x....xx00 — nonzero.
// The engine's own callers read AL only (the create-title UI handler FUN_04de1ac0:
// `call IsValid; test al, al`; the five callers of the interaction validity
// FUN_06b41880: `test al, al` / `movzbl al, eax`). The bridge called the create-title
// IsValid through `uint64_t (*)(void*, void*)` and tested all 64 bits, so every
// refusal that reached the end of CanCreateTitle (the gold, the de-jure and the
// piety-level legs) read as "valid", and a ruler with 17 gold minted k_ireland
// (L14 S3, surface-eval finding 14; re/findings/re_create_title_action.md).
//
// Call such a function through EngineBoolFn: the compiler then reads AL and nothing
// else. Engine functions that really return an integer keep their integer type.
// bridge/test/test_engine_bool.cpp calls stubs that return AL = 0 under nonzero
// upper bits through this alias and through the pre-fix shape (its control).

namespace bridge {

template <class... Args>
using EngineBoolFn = bool (*)(Args...);

}  // namespace bridge
