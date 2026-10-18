// board_witness_probe.cpp — THE BRIDGE'S OWN BOUNDS WITNESS, on a host (ck-5gdx).
//
// WHY A PROBE RATHER THAN A PYTHON FIXTURE. scripts/acceptance_board_witness.py
// has to answer "does the BRIDGE distinguish an unset bound from one exported at
// its declared value?". A fixture hand-written in Python answers a different
// question — whether the Python I wrote agrees with the Python I wrote. This
// runs the REAL `board_init_from_env()` over a REAL environment and prints what
// the REAL `stats_json()` publishes, which is the surface the driver reads off
// its `get_tick_info` frame. Same pattern as bridge/test/speed_read_probe.cpp
// (ck-k57a) and build_id_witness.cpp (ck-nkyc), and for the same reason.
//
// It prints ONE JSON object on stdout:
//   {"witness": <board_bounds_witness().json()>,
//    "stats":   <board().stats_json(), or null when the board is disabled>}
//
// `stats` is null in every arm but B — not because this probe hides it, but
// because get_tick_info publishes the whole board block only when the board
// exists (pipeline.cpp, decision G5), and the acceptance checks that the witness
// inherits that gate rather than punching a hole in it.
//
// HOST ONLY: board.cpp has no engine dependency, so this needs no CK3.

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

// The same stub set acceptance_board.cpp provides, for the same reason: linking
// board.cpp pulls in world_identity.cpp (ck-81su judges the board's carrier by
// ck-9fc3's rule) and through it session.cpp, whose guarded reads need these.
// The CMake target takes SESSION_HOST_SOURCES so the list is never retyped.
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
    // The environment is the INPUT. The caller sets CK3_EPISODE_ARM and the
    // three CK3_BOARD_* variables; nothing is set here, because a probe that
    // arranges its own environment cannot witness the caller's.
    bridge::episode_policy().init_from_env();
    bridge::board_init_from_env();

    std::string out = "{\"witness\":";
    out += bridge::board_bounds_witness().json();
    out += ",\"stats\":";
    if (bridge::board().enabled()) out += bridge::board().stats_json();
    else                           out += "null";
    out += "}";
    printf("%s\n", out.c_str());
    return 0;
}
