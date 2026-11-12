// test_played_line.cpp — the handoff's two decisions against the ONE case table
// (METERLINE; bridge/include/played_line.h, scripts/fixtures/played_line_vectors.tsv).
//
// The Python half (scripts/played_line.py) answers to the same table in
// scripts/test_played_line.py; between them the meter's "who is the line now"
// and the bridge's heir handoff cannot drift apart silently.
#include "played_line.h"
#include "line_watch.h"   // line_ended, the end decision (owner ruling 2026-11-12)

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PLAYED_LINE_VECTORS
#error "PLAYED_LINE_VECTORS must name scripts/fixtures/played_line_vectors.tsv"
#endif

static int g_fail = 0;

static uint32_t num(const std::string& s) {
    return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 0));
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    return out;
}

int main() {
    std::ifstream in(PLAYED_LINE_VECTORS);
    if (!in) {
        std::fprintf(stderr, "FAIL cannot open %s\n", PLAYED_LINE_VECTORS);
        return 1;
    }
    std::string line;
    int rows = 0, picks = 0, heirs = 0, ends = 0, realms = 0, meter_rows = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto f = split(line, '\t');
        if (f.size() < 3) { std::fprintf(stderr, "FAIL malformed row: %s\n", line.c_str()); g_fail++; continue; }
        if (f[0] == "line_realm") {
            // INTEG item 1: R(t) of a followed character, the METER's read
            // (scripts/meter_lane.py grade_line_realm, scripts/test_played_line.py).
            // No bridge decision answers it; counted, so a row never vanishes unread.
            meter_rows++;
            continue;
        }
        if (f[0] == "realm") {
            // owner ruling 2026-11-12: the handoff's realm guard
            // <root>,<dead>,<target>,<root_now>,<target_top> -> in_realm / heir_outside_realm / realm_unresolved
            auto a = split(f[1], ',');
            const char* got_s = played_line::realm_check_name(played_line::succession_realm_check(
                num(a[0]), num(a[1]), num(a[2]), num(a[3]), num(a[4])));
            realms++;
            rows++;
            if (f[2] != got_s) {
                std::fprintf(stderr, "FAIL realm(%s) = %s, expected %s (%s)\n", f[1].c_str(), got_s,
                             f[2].c_str(), f.size() > 3 ? f[3].c_str() : "");
                g_fail++;
            }
            continue;
        }
        const uint32_t expect = num(f[2]);
        uint32_t got = 0;
        if (f[0] == "pick_primary") {
            std::vector<played_line::HeldTitle> held;
            if (f[1] != "-") {
                for (const auto& e : split(f[1], ',')) {
                    auto kv = split(e, ':');
                    held.push_back({num(kv[0]), std::atoi(kv[1].c_str())});
                }
            }
            got = played_line::pick_primary(held.data(), held.size());
            picks++;
        } else if (f[0] == "heir") {
            auto a = split(f[1], ',');
            got = played_line::heir_from_holder(num(a[0]), num(a[1]));
            heirs++;
        } else if (f[0] == "line_end") {
            // <cached title>,<raw holder>,<successor found 0/1>,<dead titles held>
            auto a = split(f[1], ',');
            got = played_line::line_ended(num(a[0]), num(a[1]), num(a[2]) != 0,
                                          std::atoi(a[3].c_str())) ? 1u : 0u;
            ends++;
        } else {
            std::fprintf(stderr, "FAIL unknown fn %s\n", f[0].c_str());
            g_fail++;
            continue;
        }
        rows++;
        if (got != expect) {
            std::fprintf(stderr, "FAIL %s(%s) = %u, expected %u (%s)\n", f[0].c_str(), f[1].c_str(),
                         got, expect, f.size() > 3 ? f[3].c_str() : "");
            g_fail++;
        }
    }
    // A table that lost its rows must not pass by having nothing to check.
    if (picks == 0 || heirs == 0 || ends == 0 || realms == 0) {
        std::fprintf(stderr, "FAIL the table carried %d pick_primary, %d heir, %d line_end and %d realm rows\n",
                     picks, heirs, ends, realms);
        g_fail++;
    }
    std::printf("test_played_line: %d rows (%d pick_primary, %d heir, %d line_end, %d realm; %d line_realm left to the meter), %d failure(s)\n",
                rows, picks, heirs, ends, realms, meter_rows, g_fail);
    return g_fail ? 1 : 0;
}
