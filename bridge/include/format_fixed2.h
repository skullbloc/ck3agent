#pragma once
// format_fixed2: "%.2f" exactly as glibc prints it, on every build (ADR 0001 P5).
// Header-only and dependency-free so the portable test suite can include it.
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace bridge {

// "%.2f" as glibc prints it: the EXACT binary value, rounded half-to-even. The MSVC
// runtime rounds an exact tie away from zero (2.625 -> "2.63", glibc "2.62"), which
// made the same reply differ between the builds (P5: maa upkeep 262500 / 1e5).
// Linux keeps snprintf, so its replies are unchanged byte for byte.
inline int format_fixed2(double v, char* out, size_t n) {
#ifdef _WIN32
    if (!std::isfinite(v) || std::fabs(v) > 1e13) return std::snprintf(out, n, "%.2f", v);
    const double x = v * 100.0;
    const double err = std::fma(v, 100.0, -x);   // v*100 == x + err exactly
    const double r = std::floor(x);
    const double frac = x - r;                   // exact: x and r share the grid
    bool up;
    if (frac > 0.5) up = true;
    else if (frac < 0.5) up = false;
    else up = err > 0 ? true : (err < 0 ? false : std::fmod(r, 2.0) != 0.0);
    const long long q = static_cast<long long>(up ? r + 1.0 : r);
    const unsigned long long a = static_cast<unsigned long long>(q < 0 ? -q : q);
    return std::snprintf(out, n, "%s%llu.%02llu", std::signbit(v) ? "-" : "", a / 100, a % 100);
#else
    return std::snprintf(out, n, "%.2f", v);
#endif
}

}  // namespace bridge
