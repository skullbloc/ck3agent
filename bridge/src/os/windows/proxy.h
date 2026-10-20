#pragma once
// windows/proxy.h — the version.dll forwarding half of ck3_bridge_win
// (proxy.cpp). Internal to bridge/src/os/windows/; only the DLL target
// compiles proxy.cpp (it defines the exported forwards).

#include <cstdint>

namespace bridge {
namespace os {
namespace proxy {

// What the one published resolution of the real version.dll found.
struct Resolution {
    char           trigger[64];     // "init_thread" or the export whose call resolved first
    char           real_path[520];  // UTF-8 full path that was loaded
    std::uintptr_t real_module;     // its handle; 0 when not loaded or refused
    unsigned long  error;           // GetLastError of a failed load, else 0
    int            resolved;        // exports that resolved
    int            total;           // exports this DLL forwards
    char           missing[512];    // space-separated names that did not resolve
    char           refused[96];     // non-empty: nothing is forwarded, and why
};

// The DLL's own module handle, recorded from DllMain (no other work there).
void set_self(void* module);
// Resolve (if no thread has yet) and copy the published resolution. False
// only if publication did not complete within the bounded wait.
bool resolve(const char* trigger, Resolution* out);

int         export_count();
const char* export_name(int i);
long        call_count(int i);  // forwards called through export i so far

}  // namespace proxy
}  // namespace os
}  // namespace bridge
