// test_os_win_entry.cpp — os::entry_state on Windows in an exe, engine-free,
// under wine. DllMain runs only when the code is loaded as a DLL
// (ck3_bridge_win); this test is a plain exe, so entry_state must report
// not_attached with ok (a real answer: the image has no library entry), NOT a
// refusal and NOT a false `attached`. The attached path is driven by DllMain
// when the DLL is loaded into ck3.exe, which needs the game and is ADR phase
// P4: untested here, by design.

#include "os/os.h"

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

int main() {
    os::EntryState es;
    std::memset(&es, 0x5a, sizeof(es));
    os::Result r = os::entry_state(&es);
    check(os::succeeded(r), "entry_state: ok (a real answer, not a refusal)");
    check(es.phase == os::EntryPhase::not_attached,
          "entry_state: an exe reports not_attached (no DllMain ran)");
    check(es.module == 0 && es.init_thread_id == 0 && !es.is_game,
          "entry_state: no module, no init thread, not the game");
    check(es.outcome != nullptr && std::strstr(es.outcome, "no DllMain") != nullptr,
          "entry_state: the outcome says why");
    check(std::strcmp(os::entry_phase_string(os::EntryPhase::attached), "attached") == 0 &&
              std::strcmp(os::entry_phase_string(os::EntryPhase::not_attached), "not_attached") == 0,
          "entry_phase_string spells the phases");
    check(os::entry_state(nullptr).status == os::Status::failed, "entry_state(null): failed");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
