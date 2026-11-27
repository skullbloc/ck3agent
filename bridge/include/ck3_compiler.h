// ck3_compiler.h — THE compiler-abstraction header: every GNU-only spelling a
// shared bridge source needs, one definition per compiler family.
//
// bridge/src/*.cpp is compiled by GCC into libck3_bridge.so and by cl.exe (the
// public CI's windows-latest leg) or clang-cl (the test host's cross build) into
// ck3_bridge_core_win. cl.exe has no __attribute__ and no __atomic builtins;
// clang-cl accepts both, so a GNU spelling in shared code built on the test host and
// failed only on real MSVC (1491 cl.exe errors on 2026.12.9). Every such
// spelling goes through a macro or helper here. bridge/test/check_msvc_portable.py
// (ctest check_msvc_portable) fails on any GNU-only construct elsewhere in a file
// the Windows build compiles; this is the one file it exempts.
//
// The split is on _MSC_VER, which clang-cl defines too, so clang-cl compiles the
// same tokens cl.exe does and the test host's cross build stands in for it. The
// non-MSVC side is the exact spelling the sources used before: the Linux build
// is unchanged.
//
// What each one gives up under MSVC, and why that is safe there:
//   CK3_PRINTF_FMT   a compile-time format check only; no code.
//   CK3_WEAK         the Windows core links every definition the weak
//                    declarations name (instance_paths.cpp is in
//                    ck3_bridge_core_win); weak exists for Linux host test
//                    targets that leave it out. A weak declaration's address
//                    test then reads true, as it does in the .so.
//   CK3_EARLY_INIT   the .init_array priority (init_order.h has the contract).
//                    The Windows bridge has no ELF constructor: it starts from
//                    the init thread DllMain creates (src/os/windows/dll_main.cpp),
//                    which runs after the CRT has run every dynamic initializer
//                    of the DLL, so there is no order to pin.
//   slot_*           the same orderings: an aligned 8-byte x64 load is atomic
//                    with acquire ordering in hardware (a compiler barrier keeps
//                    accesses from moving above it); LOCK CMPXCHG is a full
//                    barrier.
#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#define CK3_PRINTF_FMT(fmt_idx, first_arg)
#define CK3_WEAK
#define CK3_EARLY_INIT
#else
#define CK3_PRINTF_FMT(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#define CK3_WEAK __attribute__((weak))
#define CK3_EARLY_INIT __attribute__((init_priority(101)))
#endif

// The calling function's return address (the AI veto's who-posted test in the
// GameCommandDispatch hook, entered by a jmp so this is the poster's return address).
#if defined(_MSC_VER)
#define CK3_RETURN_ADDRESS() reinterpret_cast<uintptr_t>(_ReturnAddress())
#else
#define CK3_RETURN_ADDRESS() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#endif

namespace bridge {

// An acquire load of a 64-bit slot in the game's memory (not a std::atomic:
// the engine owns the storage).
inline std::uint64_t slot_load_acquire(volatile std::uint64_t* slot) {
#if defined(_MSC_VER)
    const std::uint64_t v = *slot;
    _ReadWriteBarrier();
    return v;
#else
    return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
#endif
}

// A strong acq_rel compare-exchange on the same kind of slot. On failure
// *expected receives the value seen, as __atomic_compare_exchange_n does.
inline bool slot_cas_acq_rel(volatile std::uint64_t* slot, std::uint64_t* expected,
                             std::uint64_t desired) {
#if defined(_MSC_VER)
    const std::uint64_t seen = static_cast<std::uint64_t>(_InterlockedCompareExchange64(
        reinterpret_cast<volatile long long*>(slot), static_cast<long long>(desired),
        static_cast<long long>(*expected)));
    if (seen == *expected) return true;
    *expected = seen;
    return false;
#else
    return __atomic_compare_exchange_n(slot, expected, desired, /*weak=*/false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

}  // namespace bridge
