// windows/dll_main.cpp — the entry point of ck3_bridge_win, the Windows bridge
// DLL. Compiled ONLY into the DLL target, never into ck3_os or a test exe.
//
// INERT FOR P2. DllMain runs under the loader lock, so it must do almost
// nothing: no LoadLibrary, no synchronous socket work (ADR "Platform
// abstraction", os::entry row). It records the module and the attach through
// os::dll_attach_record (os::entry_state reports it) and returns. The real
// design starts ONE init thread here that runs bridge_init() after the loader
// returns; that thread, the handshake and the hooks are ADR phase P4+.
//
// The DLL exists now so the Windows platform-layer cells link into a real
// version.dll-style target; the proxy export table that forwards to the system
// DLL is P4 (ADR Windows plan, option a).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace bridge {
namespace os {
void dll_attach_record(void* module);  // windows/entry.cpp
}
}  // namespace bridge

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        bridge::os::dll_attach_record(inst);
        // ADR phase P4: start one init thread here (no work under the loader lock).
    }
    return TRUE;
}
