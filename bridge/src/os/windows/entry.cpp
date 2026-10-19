// windows/entry.cpp — os::entry_state on Windows (the os::entry ADR row).
//
// The library entry on Windows is DllMain (bridge/src/os/windows/dll_main.cpp,
// compiled only into the ck3_bridge_win DLL). It records the module handle and
// the attach here, through dll_attach_record, and MUST do nothing else under
// the loader lock (no LoadLibrary, no socket work): the init thread that runs
// bridge_init() after the loader returns is ADR phase P4. This TU holds the
// recorded state so both the DLL and the test executables (which link ck3_os
// but have no DllMain) share one entry_state definition.
//
// In a plain exe -- every per-cell test -- DllMain never runs, so entry_state
// reports not_attached. That is the correct answer, not a refusal: the image
// genuinely has no library entry.

#include "os/os.h"

namespace bridge {
namespace os {

namespace {
// Plain scalars set under the loader lock in DllMain and read later: no
// constructor, no allocation (the ck-22n3 init-order discipline, Windows side).
EntryPhase     g_phase = EntryPhase::not_attached;
std::uintptr_t g_module = 0;
}  // namespace

void dll_attach_record(void* module) {
    g_module = reinterpret_cast<std::uintptr_t>(module);
    g_phase = EntryPhase::attached;
}

Result entry_state(EntryState* out) {
    if (!out) return Result{Status::failed, 87 /* ERROR_INVALID_PARAMETER */, "out is null"};
    out->phase = g_phase;
    out->module = g_module;
    out->init_thread_id = 0;  // the init thread is ADR phase P4
    out->is_game = false;     // what it says is also P4 (process_identity on that thread)
    out->outcome = (g_phase == EntryPhase::attached)
                       ? "DllMain recorded the module; the init thread is ADR phase P4"
                       : "no DllMain ran in this image (an exe or a test)";
    return Result{Status::ok, 0, ""};
}

}  // namespace os
}  // namespace bridge
