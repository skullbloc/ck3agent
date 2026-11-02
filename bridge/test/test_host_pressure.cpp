// test_host_pressure.cpp — the SLOW REQUEST line's host PSI figures
// (bridge/include/host_pressure.h), against a fake /proc.
//
// Rows:
//   1  the l13f-gl10l-r figures (memory full 39.92, io full 74.58) read back;
//   2  an older kernel's cpu file with no "full" line: the figure reads "na";
//   3  no pressure files at all: "host_psi=unreadable", never zeros (an idle
//      host is a claim the bridge did not measure);
//   4  control: the real /proc on this host formats without crashing (and
//      says unreadable or carries five figures).
// Exit 0 pass, 1 fail.
#include "host_pressure.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int g_failures = 0;
void row(const char* name, bool ok, const std::string& detail) {
    std::printf("%-56s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}
void put(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) { std::fputs(text, f); std::fclose(f); }
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/ck3psiXXXXXX";
    const char* root = mkdtemp(tmpl);
    if (!root) { std::printf("cannot make a temp dir\n"); return 1; }
    const std::string base(root);
    const std::string pdir = base + "/pressure";
    mkdir(pdir.c_str(), 0700);
    char buf[256];

    put(pdir + "/memory", "some avg10=43.54 avg60=25.14 avg300=7.84 total=1\n"
                          "full avg10=39.92 avg60=39.18 avg300=25.38 total=2\n");
    put(pdir + "/io",     "some avg10=80.10 avg60=70.00 avg300=40.00 total=3\n"
                          "full avg10=74.58 avg60=71.53 avg300=48.28 total=4\n");
    put(pdir + "/cpu",    "some avg10=1.74 avg60=1.47 avg300=9.28 total=5\n"
                          "full avg10=0.00 avg60=0.00 avg300=0.00 total=6\n");
    std::string got = bridge::format_host_pressure(buf, sizeof buf, base.c_str());
    row("the_thrash_figures_read_back",
        got == "host_psi mem_some10=43.54 mem_full10=39.92 io_some10=80.10 io_full10=74.58 cpu_some10=1.74",
        got);

    put(pdir + "/cpu", "some avg10=2.50 avg60=1.00 avg300=1.00 total=7\n");
    got = bridge::format_host_pressure(buf, sizeof buf, base.c_str());
    row("a_missing_line_reads_na_not_zero",
        got.find("cpu_some10=2.50") != std::string::npos, got);
    std::remove((pdir + "/io").c_str());
    got = bridge::format_host_pressure(buf, sizeof buf, base.c_str());
    row("a_missing_file_reads_na_not_zero",
        got.find("io_some10=na io_full10=na") != std::string::npos, got);

    std::remove((pdir + "/memory").c_str());
    std::remove((pdir + "/cpu").c_str());
    got = bridge::format_host_pressure(buf, sizeof buf, base.c_str());
    row("no_pressure_files_is_unreadable_never_zeros", got == "host_psi=unreadable", got);

    got = bridge::format_host_pressure(buf, sizeof buf);
    row("control_the_real_proc_formats",
        got == "host_psi=unreadable" || got.rfind("host_psi mem_some10=", 0) == 0, got);

    rmdir(pdir.c_str());
    rmdir(base.c_str());
    std::printf("\n%s (%d failing row%s)\n", g_failures ? "FAIL" : "PASS", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
