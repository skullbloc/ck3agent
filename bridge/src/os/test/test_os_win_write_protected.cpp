// test_os_win_write_protected.cpp — os::write_protected on Windows,
// engine-free. Every case checks two things: the destination bytes changed,
// and the page protection came back exactly as it was (VirtualQuery). A write
// that left a protected page writable would be a silent change to the running
// process, so the restore is as much the subject as the bytes.

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

static DWORD prot_of(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) ? mbi.Protect : 0;
}

static const char kConst[32] = "a const in this image's rdata";

int main() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::size_t pg = si.dwPageSize;
    const std::uint8_t fill[8] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04};
    DWORD old = 0;

    // (a) a read-only data page.
    auto* ro = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, pg, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    std::memset(ro, 0x11, pg);
    VirtualProtect(ro, pg, PAGE_READONLY, &old);
    check(os::succeeded(os::write_protected(reinterpret_cast<std::uintptr_t>(ro + 16), fill, 8)) &&
              std::memcmp(ro + 16, fill, 8) == 0 && ro[15] == 0x11 && ro[24] == 0x11,
          "read-only page: bytes written, neighbours untouched");
    check(prot_of(ro) == PAGE_READONLY, "read-only page: PAGE_READONLY restored");

    // (b) an execute-read page (the RELRO/code analogue): the exec branch and
    // the i-cache flush. The bytes are data only; nothing here is executed.
    auto* ex = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, pg, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    std::memset(ex, 0x90, pg);
    VirtualProtect(ex, pg, PAGE_EXECUTE_READ, &old);
    check(os::succeeded(os::write_protected(reinterpret_cast<std::uintptr_t>(ex), fill, 8)) &&
              std::memcmp(ex, fill, 8) == 0,
          "execute-read page: bytes written through PAGE_EXECUTE_READWRITE");
    check(prot_of(ex) == PAGE_EXECUTE_READ, "execute-read page: PAGE_EXECUTE_READ restored");

    // (c) a write that spans two adjacent regions with different protections:
    // a read-only page followed by an execute-read page. Each must be restored
    // to its own protection, and the straddling bytes must all land.
    auto* two = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 2 * pg, MEM_RESERVE, PAGE_NOACCESS));
    VirtualAlloc(two, pg, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(two + pg, pg, MEM_COMMIT, PAGE_READWRITE);
    std::memset(two, 0x22, 2 * pg);
    VirtualProtect(two, pg, PAGE_READONLY, &old);
    VirtualProtect(two + pg, pg, PAGE_EXECUTE_READ, &old);
    const std::uint8_t span[8] = {5, 6, 7, 8, 9, 10, 11, 12};
    check(os::succeeded(os::write_protected(reinterpret_cast<std::uintptr_t>(two + pg - 4), span, 8)) &&
              std::memcmp(two + pg - 4, span, 8) == 0,
          "two regions: a straddling write lands across the boundary");
    check(prot_of(two) == PAGE_READONLY && prot_of(two + pg) == PAGE_EXECUTE_READ,
          "two regions: each region's own protection restored");

    // (d) this executable's own read-only .rdata.
    const DWORD rdata_old = prot_of(kConst);
    check(rdata_old == PAGE_READONLY, "the image const really is read-only");
    const std::uint8_t mark[4] = {'Z', 'Z', 'Z', 'Z'};
    const os::Result rd = os::write_protected(reinterpret_cast<std::uintptr_t>(kConst), mark, 4);
    check(os::succeeded(rd), "image .rdata: write_protected ok");
    // Read through a volatile pointer: the compiler folds direct reads of a
    // const object it believes nothing mutated (the write goes via a uintptr_t
    // it cannot track), so a plain memcmp would compare the compile-time bytes.
    const volatile std::uint8_t* vp = reinterpret_cast<const volatile std::uint8_t*>(kConst);
    bool wrote = true;
    for (int i = 0; i < 4; ++i) wrote = wrote && vp[i] == mark[i];
    check(wrote, "image .rdata: bytes written");
    check(prot_of(kConst) == rdata_old, "image .rdata: original protection restored");

    // (e) refusals: n == 0 is ok; null src, a reserved (uncommitted) page, and
    // a NOACCESS page all fail with nothing written.
    check(os::succeeded(os::write_protected(reinterpret_cast<std::uintptr_t>(ro), fill, 0)),
          "n == 0: ok");
    check(os::write_protected(reinterpret_cast<std::uintptr_t>(ro), nullptr, 4).status ==
              os::Status::failed,
          "null src: failed");
    auto* resv = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, pg, MEM_RESERVE, PAGE_NOACCESS));
    check(os::write_protected(reinterpret_cast<std::uintptr_t>(resv), fill, 4).status ==
              os::Status::failed,
          "reserved, uncommitted page: failed");
    auto* na = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, pg, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    check(os::write_protected(reinterpret_cast<std::uintptr_t>(na), fill, 4).status ==
              os::Status::failed && prot_of(na) == PAGE_NOACCESS,
          "NOACCESS page: failed, protection untouched");

    VirtualFree(ro, 0, MEM_RELEASE);
    VirtualFree(ex, 0, MEM_RELEASE);
    VirtualFree(two, 0, MEM_RELEASE);
    VirtualFree(resv, 0, MEM_RELEASE);
    VirtualFree(na, 0, MEM_RELEASE);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
