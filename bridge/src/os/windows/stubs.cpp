// windows/stubs.cpp — the Windows implementation of bridge/include/os/os.h,
// as typed refusals. Nothing here is implemented yet.
//
// Every call returns not_implemented (the ADR 0001 phase that writes it is in
// `detail`) or unavailable_on_platform (the mechanism does not exist on
// Windows; `detail` names its replacement), and leaves every out-parameter in
// a defined empty state. Nothing returns ok: a stub that answered would be a
// default dressed up as a measurement.
//
// Standard C++17 only, and no platform header (not even <windows.h> yet): the
// windows-latest CI leg compiles this with MSVC, and the Linux test build
// compiles it with the host compiler, -std=c++17 -pedantic-errors, as
// test_os_stubs_windows.

#include "os/os.h"

namespace bridge {
namespace os {

namespace {

Result todo(const char* detail) { return Result{Status::not_implemented, 0, detail}; }
Result never(const char* detail) { return Result{Status::unavailable_on_platform, 0, detail}; }

}  // namespace

Platform platform() { return Platform::kWindows; }


Result exec_interposition(ExecInterposition* out) {
    if (out) { out->mechanism = "none"; out->symbols = nullptr; out->symbol_count = 0; }
    return never("windows: no exec interposition; a proxy DLL beside ck3.exe is loaded "
                 "by the game process itself (ADR 0001 Windows plan, option a)");
}



Result input_click(int, int, MouseButton) {
    return todo("windows: SendInput (ADR 0001 phase P6)");
}



Result rtti_find_vtable(const char*, std::uintptr_t* vtable) {
    if (vtable) *vtable = 0;
    return todo("windows: MSVC RTTICompleteObjectLocator -> TypeDescriptor walk; needs "
                "ck3.exe (ADR 0001 phases P1/P5)");
}

Result hook_plan(std::uintptr_t, HookPlan* out) {
    if (out) { out->target = 0; out->steal_bytes = 0; out->cfg_checked_site = false;
               out->cet_compatible = false; }
    return todo("windows: per-hook existence/ICF/CFG/CET planning needs ck3.exe "
                "(ADR 0001 phase P6)");
}

}  // namespace os
}  // namespace bridge
