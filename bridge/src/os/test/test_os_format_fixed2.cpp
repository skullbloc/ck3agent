// format_fixed2 prints "%.2f" as glibc does, on every build (ADR 0001 P5). The
// expected strings are glibc's (Python's %-formatting rounds the same way: the exact
// binary value, ties to even); on Linux the test checks the table, under wine it
// checks the Windows emulation (the MSVC runtime rounds exact ties away from zero).
#include "format_fixed2.h"

#include <cstdio>
#include <cstring>

namespace {
struct Case { double v; const char* want; };
const Case kCases[] = {
    {2.625, "2.62"},
    {0.125, "0.12"},
    {0.375, "0.38"},
    {-0.125, "-0.12"},
    {2.675, "2.67"},
    {1.005, "1.00"},
    {-0.001, "-0.00"},
    {0.0, "0.00"},
    {-0.0, "-0.00"},
    {0.5, "0.50"},
    {1.0, "1.00"},
    {123456.785, "123456.79"},
    {99.995, "100.00"},
    {0.015, "0.01"},
    {0.025, "0.03"},
    {0.035, "0.04"},
    {0.045, "0.04"},
    {2.625, "2.62"},
    {1234567.125, "1234567.12"},
    {-2.625, "-2.62"},
    {1000000000000.125, "1000000000000.12"},
    {9999999999.995, "10000000000.00"},
    {0.004999, "0.00"},
    {0.005, "0.01"},
    {0.0050000001, "0.01"},
    {33.33333, "33.33"},
    {2.625, "2.62"},
    {0.10000000149011612, "0.10"},
    {0.30000001192092896, "0.30"},
    {2.674999952316284, "2.67"},
    {1.0049999952316284, "1.00"},
    {0.014999999664723873, "0.01"},
    {123.45500183105469, "123.46"},
    {7.125, "7.12"},
    {1.2345000505447388, "1.23"},
};
}  // namespace

int main() {
    int bad = 0;
    for (const Case& c : kCases) {
        char got[64];
        bridge::format_fixed2(c.v, got, sizeof got);
        if (std::strcmp(got, c.want) != 0) {
            std::printf("FAIL %.17g: got %s want %s\n", c.v, got, c.want);
            ++bad;
        }
    }
    std::printf("%d/%d cases\n", static_cast<int>(sizeof kCases / sizeof kCases[0]) - bad,
                static_cast<int>(sizeof kCases / sizeof kCases[0]));
    return bad ? 1 : 0;
}
