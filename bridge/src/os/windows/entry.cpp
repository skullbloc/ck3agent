// windows/entry.cpp — os::entry_state on Windows (the os::entry ADR row).
//
// The library entry on Windows is DllMain (bridge/src/os/windows/dll_main.cpp,
// compiled only into the ck3_bridge_win DLL). It records the module handle and
// the attach here, through dll_attach_record, and MUST do nothing else under
// the loader lock (no LoadLibrary, no socket work) beyond starting the one
// init thread. That thread (dll_main.cpp, ADR phase P4) records its progress
// through dll_init_record. This TU holds the recorded state so both the DLL
// and the test executables (which link ck3_os but have no DllMain) share one
// entry_state definition.
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
std::uint64_t  g_init_tid = 0;
bool           g_is_game = false;
const char*    g_outcome = nullptr;  // static string from the init thread
}  // namespace

void dll_attach_record(void* module) {
    g_module = reinterpret_cast<std::uintptr_t>(module);
    g_phase = EntryPhase::attached;
}

// Called by the init thread only (one writer). outcome must be static.
void dll_init_record(EntryPhase phase, std::uint64_t tid, bool is_game, const char* outcome) {
    g_init_tid = tid;
    g_is_game = is_game;
    if (outcome) g_outcome = outcome;
    g_phase = phase;
}

Result entry_state(EntryState* out) {
    if (!out) return Result{Status::failed, 87 /* ERROR_INVALID_PARAMETER */, "out is null"};
    out->phase = g_phase;
    out->module = g_module;
    out->init_thread_id = g_init_tid;
    out->is_game = g_is_game;
    out->outcome = g_outcome                         ? g_outcome
                   : (g_phase == EntryPhase::attached) ? "DllMain recorded the module; the init thread has not run yet"
                                                       : "no DllMain ran in this image (an exe or a test)";
    return Result{Status::ok, 0, ""};
}

}  // namespace os
}  // namespace bridge
