// test_os_win_exec_alloc.cpp — os::alloc_exec_near / free_exec and the unwind
// registration on Windows, engine-free, under wine.
//
// No hand-written machine code is executed: executability is read from the
// page protection (VirtualQuery), reach is checked as an address range, and
// the unwind registration is proved by RtlLookupFunctionEntry finding our
// RUNTIME_FUNCTION for an address inside the block and not finding it after
// unregister. That is what the inline-hook engine relies on: a trampoline
// page in rel32 reach whose frames a stack walk can cross.

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

static DWORD prot_of(std::uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) ? mbi.Protect : 0;
}

constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY;

int main() {
    // A target inside this image, the realistic case (a trampoline near .text).
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(&main);

    os::ExecBlock blk{1, 1};
    check(os::succeeded(os::alloc_exec_near(target, 4096, &blk)) && blk.base != 0,
          "alloc_exec_near: ok, a block was allocated");
    const std::uintptr_t reach = 0x7fff0000ull;
    const std::uintptr_t diff = blk.base > target ? blk.base - target : target - blk.base;
    check(diff <= reach, "alloc_exec_near: base within +-2 GB of the target (rel32 reach)");
    check(blk.size >= 4096 && (blk.size % 4096) == 0, "alloc_exec_near: size page-rounded");
    check((prot_of(blk.base) & kExec) != 0, "alloc_exec_near: the page is executable");

    // Register one RUNTIME_FUNCTION covering the block, with a trivial
    // UNWIND_INFO written inside it (Version 1, no prolog, no codes).
    auto* p = reinterpret_cast<std::uint8_t*>(blk.base);
    const std::uint32_t unwind_at = 2048;
    p[unwind_at + 0] = 0x01;  // Version (bits 0-2) = 1, Flags = 0
    p[unwind_at + 1] = 0x00;  // SizeOfProlog
    p[unwind_at + 2] = 0x00;  // CountOfCodes
    p[unwind_at + 3] = 0x00;  // FrameRegister / FrameOffset
    os::UnwindEntry e{0, 1024, unwind_at};
    check(os::succeeded(os::register_unwind(blk, &e, 1)), "register_unwind: ok");

    DWORD64 image = 0;
    PRUNTIME_FUNCTION found = RtlLookupFunctionEntry(blk.base + 100, &image, nullptr);
    check(found != nullptr && image == blk.base && found->BeginAddress == 0 &&
              found->EndAddress == 1024,
          "RtlLookupFunctionEntry finds our entry for an address in the block");

    check(os::succeeded(os::unregister_unwind(blk)), "unregister_unwind: ok");
    image = 0;
    found = RtlLookupFunctionEntry(blk.base + 100, &image, nullptr);
    check(found == nullptr, "after unregister, no dynamic entry covers the block");

    check(os::succeeded(os::free_exec(&blk)) && blk.base == 0, "free_exec: ok, block zeroed");

    // free_exec also drops a still-registered table (no leak, no dangling).
    os::ExecBlock b2{1, 1};
    os::alloc_exec_near(target, 4096, &b2);
    std::memset(reinterpret_cast<void*>(b2.base + unwind_at), 0, 4);
    reinterpret_cast<std::uint8_t*>(b2.base)[unwind_at] = 0x01;
    os::register_unwind(b2, &e, 1);
    const std::uintptr_t b2base = b2.base;
    check(os::succeeded(os::free_exec(&b2)), "free_exec: ok with a table still registered");
    image = 0;
    check(RtlLookupFunctionEntry(b2base + 100, &image, nullptr) == nullptr,
          "free_exec dropped the unwind table");

    // Refusals.
    os::ExecBlock bad{1, 1};
    check(os::alloc_exec_near(target, 0, &bad).status == os::Status::failed && bad.base == 0,
          "alloc_exec_near(size 0): failed");
    os::ExecBlock empty{0, 0};
    os::UnwindEntry e2{0, 1, 0};
    check(os::register_unwind(empty, &e2, 1).status == os::Status::failed,
          "register_unwind on an empty block: failed");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
