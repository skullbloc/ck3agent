// board_bounds_prefix_probe.cpp — THE NEGATIVE CONTROL'S PROBE (ck-5gdx).
//
// It prints `board().stats_json()` and NOTHING ELSE, using only API that
// existed BEFORE this bead. That is the entire design constraint, and it is
// deliberate: scripts/acceptance_board_witness.py compiles this ONE source
// against TWO trees —
//
//   * the working tree's board.{h,cpp}, where stats_json() carries
//     `bounds_witness` and an UNSET bound and one EXPORTED AT ITS DECLARED
//     VALUE produce different bytes;
//   * the PRE-FIX board.{h,cpp}, pinned by blob sha, where it does not and they
//     produce BYTE-IDENTICAL output.
//
// A control built on `board_bounds_witness()` could not be compiled against the
// pre-fix tree at all, and a control that fails to COMPILE reports as a control
// doing its job — which is the worst way for a guard to break and is the thing
// scripts/bridge_host_sources.py exists to stop happening again.
//
// So the control demonstrates the defect rather than asserting it: the pre-fix
// bridge cannot tell "nobody touched the manipulation" from "somebody set it,
// to the value it already had", and here are the two identical strings.
//
// HOST ONLY. board.cpp has no engine dependency.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "board.h"
#include "bridge.h"
#include "episode_policy.h"

// The same stub set acceptance_board.cpp provides; see board_witness_probe.cpp.
namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t             g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex     g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}  // namespace bridge

int main() {
    bridge::episode_policy().init_from_env();
    bridge::board_init_from_env();
    printf("%s\n", bridge::board().stats_json().c_str());
    return 0;
}
