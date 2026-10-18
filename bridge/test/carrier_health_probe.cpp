// carrier_health_probe.cpp — DOES THE CARRIER NOTICE A REFUSED WRITE? (ck-hzp7)
//
// Drives the REAL bridge/src/board_persist.cpp against a real path, so what is
// measured is the shipping carrier rather than a model of it. Its whole job is
// to be run TWICE — once on an ordinary filesystem and once on a FULL one — and
// to print what the carrier knows afterwards.
//
// Built twice by scripts/acceptance_carrier_degraded.py: against the shipping
// board_persist.cpp, and against the PRE-FIX blob. The pre-fix build has no
// health surface at all, which is the defect stated as an absence:
// CK3_CARRIER_HAS_HEALTH is defined only for the fixed build.
//
// Usage:  carrier_health_probe <carrier-path>
// Prints ONE json object: what was attempted, what reached the file, and what
// the carrier says about itself.

#include "board.h"
#include "bridge.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <sys/stat.h>

// The same stub set bridge/test/board_witness_probe.cpp provides, for the same
// reason: linking board_persist.cpp pulls in world_identity.cpp and through it
// session.cpp, whose guarded reads need these. The list is not retyped by hand —
// it is the one the sibling probe already carries.
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

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: probe <carrier-path> [begin]\n"); return 2; }
    const char* path = argv[1];
    // MODE `begin` drives board_carrier_begin instead of a bare post, so the
    // TRUNCATE path has a caller. ck-t5bc: `if (FILE* f = fopen(p,"w")) fclose(f);`
    // discarded its failure BY CONSTRUCTION, and a begin that cannot truncate
    // leaves the PREVIOUS episode's records in place for this episode's appends
    // to land after — a stale board reading as this one's, which is worse than
    // an empty one because it is plausible.
    const bool do_begin = (argc > 2 && std::string(argv[2]) == "begin");

    bridge::board_carrier_set_path_for_test(path);

    if (do_begin) {
        bridge::board_carrier_begin("ck-hzp7-probe-episode");
    } else {
        // One ordinary post through the public entry point the board itself uses.
        bridge::BoardPost p;
        p.seq     = 0;
        p.post_id = "bp_00000000";
        p.text    = "ck-hzp7 probe post";
        bridge::board_carrier_append_post(p);
    }

    // What actually reached the device, asked of the device and not of us.
    struct stat st;
    const long long bytes = (stat(path, &st) == 0) ? (long long)st.st_size : -1;

    printf("{\"append_returned\":true,\"carrier_bytes\":%lld", bytes);
#ifdef CK3_CARRIER_HAS_HEALTH
    const bridge::CarrierHealth h = bridge::board_carrier_health();
    printf(",\"health_surface\":true,\"degraded\":%s,\"records_dropped\":%llu",
           h.degraded ? "true" : "false",
           (unsigned long long)h.records_dropped);
    if (!h.reason.empty()) printf(",\"reason\":\"%s\"", h.reason.c_str());
#else
    // THE PRE-FIX STATE, and it is the finding rather than a build artefact:
    // the carrier had NOTHING to say about a refused write. append_locked
    // returned normally and no surface anywhere recorded the loss.
    printf(",\"health_surface\":false");
#endif
    printf("}\n");
    return 0;
}
