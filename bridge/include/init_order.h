#pragma once
// =============================================================================
// init_order.h — THE .init_array CONTRACT (ck-22n3, after ck-xqol)
//
// libck3_bridge.so has an __attribute__((constructor)): `dll_init` in
// bridge_init.cpp. A plain constructor attribute carries NO priority, so the
// linker places it in `.init_array` alongside — and, as it happened, AHEAD of —
// every C++ dynamic initializer in the library. dll_init calls bridge_init(),
// which
//
//   * writes globals synchronously (instance_paths' five std::strings,
//     bridge_init's own g_valid_ranges via validate_offsets()), and
//   * STARTS THREADS: start_ipc_server() spawns server_loop and reader_loop,
//     pipeline_start() spawns conductor_loop, and a detached lambda polls for
//     game-ready.
//
// Every one of those runs against globals whose constructors had not executed.
// Two failure shapes, both observed:
//
//   1. THE CONSTRUCTOR RUNS OVER THE WRITE. bridge_init() fills a global, then
//      the owning TU's initializer default-constructs it — the write is lost
//      and its heap buffer leaked. (g_valid_ranges: the /proc/self/maps table
//      built by validate_offsets() was emptied microseconds later, so is_mapped()
//      answered `false` for every address until the next refresh.)
//   2. A THREAD READS RAW .bss. Zeroed storage is not a constructed object, and
//      zero is a plausible-looking value — which is how ck-xqol's reader thread
//      polled fd 0 (stdin) out of an unconstructed slot table and SIGSEGV'd
//      clearing a std::string that had never been constructed.
//
// THE RULE, and it is mechanical: EVERY namespace-scope object in this library
// that needs dynamic initialization is marked CK3_EARLY_INIT. 101 is the lowest
// priority the toolchain allows outside the implementation's reserved range, and
// prioritized initializers are emitted into `.init_array.00101`, which the
// linker places ahead of the unprioritized `.init_array` that holds dll_init.
// The result is the invariant bridge/test/test_init_order.cpp asserts against
// the BUILT .so:
//
//      every _GLOBAL__sub_I* entry precedes dll_init() in .init_array.
//
// A new global that forgets CK3_EARLY_INIT emits a second, unprioritized
// _GLOBAL__sub_I_<tu>.cpp AFTER dll_init and fails that test. That is the point:
// the next one cannot land silently.
//
// NOTE the attribute is only valid on namespace-scope objects of CLASS type.
// For anything else, prefer a constant-initialisable form (a raw pointer, an
// atomic with a constant initializer, a function-local static constructed on
// first use) — those need no priority at all because they need no initializer.
// Where a side effect had to happen at load time and the holder was a `bool`
// (the world-reset registrars), the holder became a class type so it could
// carry the priority: see WorldResetRegistrar in world_epoch.h.
//
// ck-xqol's ipc_server.cpp globals predate this header and spell the attribute
// out inline; they are the same lever at the same priority.
//
// IF test_init_order FAILS ON YOUR BRANCH, IT IS TELLING YOU SOMETHING TRUE.
// It names the translation unit. Add CK3_EARLY_INIT to that TU's namespace-scope
// globals (and this include) — that is the whole fix. It has already happened
// once: ck-9fc3's world_identity.cpp and personality_persist.cpp were written in
// parallel with this header and landed on main without it, putting g_declared /
// g_quarantine_identity / g_ledger / g_path / g_id after dll_init. get_tick_info
// reads the first two, and the bridge starts the IPC server early SO THAT
// get_tick_info answers immediately — so a client connecting in that window read
// never-constructed std::strings, and a declare_save_identity arriving in it
// would have been wiped by the constructor that ran afterwards.
// =============================================================================

#define CK3_EARLY_INIT __attribute__((init_priority(101)))
