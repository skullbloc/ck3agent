// test_os_win_safe_read.cpp — os::safe_read on Windows, engine-free.
// Pages are laid out with VirtualAlloc so each case's mapping state is known:
// committed, reserved-only, PAGE_NOACCESS, released, and a read straddling a
// committed page into a reserved one. Every bad case must come back `failed`
// with a Win32 error, and the process must still be running to say so.

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::size_t page = si.dwPageSize;

    // [ committed RW | reserved ] in one 64 KiB reservation.
    auto* region = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 2 * page, MEM_RESERVE, PAGE_NOACCESS));
    check(region != nullptr, "reserve two pages");
    if (!region) return 1;
    check(VirtualAlloc(region, page, MEM_COMMIT, PAGE_READWRITE) == region, "commit the first page");
    for (std::size_t i = 0; i < page; ++i) region[i] = std::uint8_t(i * 7);

    std::uint8_t buf[64];
    std::memset(buf, 0, sizeof(buf));
    check(os::succeeded(os::safe_read(buf, reinterpret_cast<std::uintptr_t>(region + 100), 64)) &&
              std::memcmp(buf, region + 100, 64) == 0,
          "committed page: ok, bytes equal");
    check(os::succeeded(os::safe_read(buf, reinterpret_cast<std::uintptr_t>(region), 0)),
          "n == 0: ok, reads nothing");

    os::Result r = os::safe_read(buf, reinterpret_cast<std::uintptr_t>(region + page), 8);
    check(r.status == os::Status::failed && r.os_error != 0, "reserved, uncommitted page: failed, Win32 error set");
    r = os::safe_read(buf, reinterpret_cast<std::uintptr_t>(region + page - 4), 8);
    check(r.status == os::Status::failed, "straddling into the reserved page: failed (no partial ok)");

    auto* na = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    r = os::safe_read(buf, reinterpret_cast<std::uintptr_t>(na), 8);
    check(na && r.status == os::Status::failed, "PAGE_NOACCESS page: failed");

    auto* gone = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    VirtualFree(gone, 0, MEM_RELEASE);
    r = os::safe_read(buf, reinterpret_cast<std::uintptr_t>(gone), 8);
    check(r.status == os::Status::failed, "released page: failed");

    r = os::safe_read(buf, 0, 8);
    check(r.status == os::Status::failed, "address 0: failed");
    r = os::safe_read(nullptr, reinterpret_cast<std::uintptr_t>(region), 8);
    check(r.status == os::Status::failed && r.os_error == ERROR_INVALID_PARAMETER, "null dst: failed");

    VirtualFree(na, 0, MEM_RELEASE);
    VirtualFree(region, 0, MEM_RELEASE);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
