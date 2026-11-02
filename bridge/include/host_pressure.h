// host_pressure.h — the host's pressure-stall figures (Linux PSI), for the
// SLOW REQUEST line.
//
// L13 live proof l13f-gl10l-r: a bridge-internal get_tick_info held the IPC
// serving thread for >10 s with the drain lock free. The node's pcp archive
// for that window shows the cause was the HOST, not a bridge lock: available
// memory ~0.2 GB of 31, swap exhausted, memory full-stall 35-44 % and io
// full-stall 72-75 % (10 s averages), load 46-74 on 6 cores, ending when the
// game was killed. A thread that page-faults under that waits on the disk,
// whatever it holds. The SLOW line could not say so: it names the drain lock,
// which was free, and nothing else. It now carries the host's PSI figures at
// the moment it fires, so a thrash reads as the environment and a lock stall
// (PSI near zero) reads as the bridge.
//
// Header-only and engine-free: a host test drives it against a fake /proc.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

namespace bridge {

// avg10 of the "some" and "full" lines of /proc/pressure/<res>; -1 when the
// file or the line is absent (an older kernel has no "full" line for cpu).
struct PressureLine { double some10 = -1, full10 = -1; bool readable = false; };

inline PressureLine read_pressure(const std::string& path) {
    PressureLine p;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return p;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        double v = 0;
        if (std::sscanf(line, "some avg10=%lf", &v) == 1) { p.some10 = v; p.readable = true; }
        else if (std::sscanf(line, "full avg10=%lf", &v) == 1) { p.full10 = v; p.readable = true; }
    }
    std::fclose(f);
    return p;
}

// "host_psi mem_some10=.. mem_full10=.. io_some10=.. io_full10=.. cpu_some10=.."
// (a figure the kernel does not publish reads "na"), or "host_psi=unreadable"
// when no pressure file can be read (no PSI, or not Linux): never zeros, which
// would read as an idle host.
inline const char* format_host_pressure(char* buf, size_t n, const char* proc_root = "/proc") {
#ifdef _WIN32
    (void)proc_root;
    std::snprintf(buf, n, "host_psi=unavailable");
    return buf;
#else
    const std::string root = std::string(proc_root) + "/pressure/";
    const PressureLine m = read_pressure(root + "memory");
    const PressureLine io = read_pressure(root + "io");
    const PressureLine c = read_pressure(root + "cpu");
    if (!m.readable && !io.readable && !c.readable) {
        std::snprintf(buf, n, "host_psi=unreadable");
        return buf;
    }
    auto fmt = [](char* o, size_t on, double v) {
        if (v < 0) std::snprintf(o, on, "na"); else std::snprintf(o, on, "%.2f", v);
    };
    char a[5][16];
    fmt(a[0], 16, m.some10); fmt(a[1], 16, m.full10);
    fmt(a[2], 16, io.some10); fmt(a[3], 16, io.full10);
    fmt(a[4], 16, c.some10);
    std::snprintf(buf, n, "host_psi mem_some10=%s mem_full10=%s io_some10=%s io_full10=%s cpu_some10=%s",
                  a[0], a[1], a[2], a[3], a[4]);
    return buf;
#endif
}

}  // namespace bridge
