// lobby_view_hook.cpp — captures the live CJominiMultiplayerLobbyView singleton
// by inline-hooking its constructor.
//
// Background (ck-dq7ym.7 → ck-dq7ym.8):
//   The "Observe" button the bridge's uinput fallback clicks at (540, 1060)
//   is the JominiLobbyViewButton inside multiplayer_lobby.gui:1081-1165, owned
//   by `CJominiMultiplayerLobbyView`. Driving it programmatically requires the
//   live `this` pointer to that view, which is not exposed via any static
//   global in the stripped binary.
//
//   We capture it the cleanest way available: hook the constructor body at
//   `ck3::lobby_view::CONSTRUCTOR_BODY` (raw 0x59169a0). At entry RDI = the
//   future singleton; after we chain to the trampoline (which runs the
//   original constructor) the object is fully initialised and we stash the
//   pointer in `g_lobby_view_singleton` for downstream readers.
//
//   See re/offsets/observer_data_functions.h `ck3::lobby_view` namespace for
//   the full RE chain (constructor + outer-class ownership + vtable VAs).

#include "bridge.h"
#include "ck3_offsets.h"
#include "observer_data_functions.h"
#ifdef _WIN32
#include "windows/ck3_offsets_win.h"
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace bridge {
    extern FILE* g_logfile;
    extern uintptr_t g_base_addr;
    void* install_hook(uintptr_t target_addr, void* hook_fn);
}

namespace ck3_bridge {

// Captured at first construction. NULL until the lobby UI is created (which
// only happens once the game has loaded a multiplayer-shaped save, including
// observer-mode saves).
static std::atomic<void*> g_lobby_view_singleton{nullptr};

// Original constructor signature inferred from the prologue:
//   void __regcall ctor(this=rdi, arg2=rsi, arg3=rdx)
// The third arg (rdx) is stashed at *this+0xd0 inside the body, which makes
// it look like an "owner" or "parent view" reference.
#ifdef _WIN32
// MSVC: a constructor returns `this` in rax, and a caller may use it.
using LobbyViewCtor = void* (*)(void* /*this*/, void* /*arg2*/, void* /*arg3*/);
#else
using LobbyViewCtor = void (*)(void* /*this*/, void* /*arg2*/, void* /*arg3*/);
#endif
static LobbyViewCtor g_ctor_trampoline = nullptr;

// The hook function. We use C linkage to keep the calling convention sane
// (System V AMD64 ABI: first six int/ptr args in rdi/rsi/rdx/rcx/r8/r9 —
// same as the original constructor's ABI).
#ifdef _WIN32
extern "C" void* lobby_view_ctor_hook(void* this_, void* arg2, void* arg3) {
#else
extern "C" void lobby_view_ctor_hook(void* this_, void* arg2, void* arg3) {
#endif
    // Chain to the original constructor first so the object is fully built
    // by the time we record the pointer. install_hook returns a trampoline
    // that re-executes the stolen prologue bytes then jumps past the hook
    // patch back into the original function body.
    if (g_ctor_trampoline) {
        g_ctor_trampoline(this_, arg2, arg3);
    }

    // Single-publisher store. We don't compare-and-swap because the CK3
    // engine only constructs one CJominiMultiplayerLobbyView per session;
    // any subsequent construction means the prior was destroyed first.
    g_lobby_view_singleton.store(this_, std::memory_order_release);

    if (bridge::g_logfile) {
        fprintf(bridge::g_logfile,
                "[CK3-Bridge] LobbyView ctor hook: captured singleton this=%p "
                "arg2=%p arg3=%p\n",
                this_, arg2, arg3);
        fflush(bridge::g_logfile);
    }
#ifdef _WIN32
    return this_;
#endif
}

// Public accessor for other bridge translation units (e.g. action_handlers).
void* get_lobby_view_singleton() {
    return g_lobby_view_singleton.load(std::memory_order_acquire);
}

// Install the constructor hook. Called from bridge_init after g_base_addr is
// resolved. Returns true on success — failure here only loses the singleton
// capture; it does not break the rest of the bridge.
bool install_lobby_view_hook() {
    if (bridge::g_base_addr == 0) {
        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                    "[CK3-Bridge] LobbyView hook: g_base_addr not resolved; skipping\n");
        }
        return false;
    }

#ifdef _WIN32
    // observer_data_functions.h is a Linux-only table (gen_win_offsets reads only
    // ck3_offsets.h): the Windows constructor comes from ck3_offsets_win.h.
    uintptr_t ctor_addr = bridge::g_base_addr + ck3_win::funcs::LOBBY_VIEW_CTOR;
#else
    uintptr_t ctor_addr = bridge::g_base_addr + ck3::lobby_view::CONSTRUCTOR_BODY;
#endif
    void* tramp = bridge::install_hook(ctor_addr, reinterpret_cast<void*>(&lobby_view_ctor_hook));
    if (!tramp) {
        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                    "[CK3-Bridge] LobbyView hook: install_hook failed at 0x%lx\n",
                    ctor_addr);
        }
        return false;
    }

    g_ctor_trampoline = reinterpret_cast<LobbyViewCtor>(tramp);

    if (bridge::g_logfile) {
        fprintf(bridge::g_logfile,
                "[CK3-Bridge] LobbyView hook: installed at 0x%lx (trampoline=%p)\n",
                ctor_addr, tramp);
    }
    return true;
}

}  // namespace ck3_bridge
