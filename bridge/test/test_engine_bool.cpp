// test_engine_bool.cpp — L14 S3 (surface-eval finding 14). NO GAME.
//
// THE CLAIM: a game function declared `bool` is read by its AL only. The stubs
// below return exactly what CanCreateTitle returned for Murchad's k_ireland mint:
// AL = 0 (refused) with nonzero bits above it. Called through bridge::EngineBoolFn
// they read false; a stub with AL = 1 reads true.
//
// The control (built with -DENGINE_BOOL_PREFIX_CONTROL, registered WILL_FAIL) reads
// the same stubs through the pre-fix shape, `uint64_t (*)(void*, void*)` tested for
// nonzero (intent_dispatch.cpp before L14 S3), and must fail the same checks.

#include "engine_bool.h"

#include <cstdint>
#include <cstdio>

extern "C" {
// AL = 0x00, upper bits nonzero: a refusal as the engine leaves it.
uint64_t engine_bool_stub_refused(void*, void*);
// AL = 0x01, upper bits nonzero: a pass.
uint64_t engine_bool_stub_passed(void*, void*);
// RAX = 0: a refusal from an early `xor eax, eax` path.
uint64_t engine_bool_stub_clean_zero(void*, void*);
}

__asm__(
    ".text\n"
    ".globl engine_bool_stub_refused\n"
    "engine_bool_stub_refused:\n"
    "  movabsq $0x0000561b4c3a8f00, %rax\n"
    "  ret\n"
    ".globl engine_bool_stub_passed\n"
    "engine_bool_stub_passed:\n"
    "  movabsq $0x0000561b4c3a8f01, %rax\n"
    "  ret\n"
    ".globl engine_bool_stub_clean_zero\n"
    "engine_bool_stub_clean_zero:\n"
    "  xorl %eax, %eax\n"
    "  ret\n");

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static bool verdict(uint64_t (*stub)(void*, void*)) {
#ifdef ENGINE_BOOL_PREFIX_CONTROL
    // The pre-fix read: the whole register.
    auto fn = reinterpret_cast<uint64_t (*)(void*, void*)>(stub);
    return fn(nullptr, nullptr) != 0;
#else
    auto fn = reinterpret_cast<bridge::EngineBoolFn<void*, void*>>(stub);
    return fn(nullptr, nullptr);
#endif
}

int main() {
    printf("test_engine_bool%s\n",
#ifdef ENGINE_BOOL_PREFIX_CONTROL
           " (CONTROL: pre-fix 64-bit read, must FAIL)"
#else
           ""
#endif
    );
    check(!verdict(engine_bool_stub_refused),
          "AL=0 with nonzero upper bits reads as refused");
    check(verdict(engine_bool_stub_passed), "AL=1 reads as passed");
    check(!verdict(engine_bool_stub_clean_zero), "RAX=0 reads as refused");
    printf("%d/%d checks passed\n", g_checks - g_fails, g_checks);
    return g_fails == 0 ? 0 : 1;
}
