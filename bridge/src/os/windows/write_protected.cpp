// windows/write_protected.cpp — os::write_protected on Windows.
//
// VirtualProtect -> write -> restore, done per protection region: a write that
// spans pages with different protections (a read-only vtable next to a
// writable global, or a code page) gives each region its own protection back,
// not the first region's. A region that the engine made executable is made
// PAGE_EXECUTE_READWRITE for the write and the instruction cache is flushed
// after. The whole range is checked committed and accessible before any
// protection changes, so a bad range fails with nothing touched.

#include "win_internal.h"

#include <cstring>

namespace bridge {
namespace os {

namespace {

constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY;
constexpr std::size_t kMaxRegions = 32;

struct Region { std::uint8_t* start; std::size_t len; DWORD old; };

}  // namespace

Result write_protected(std::uintptr_t dst, const void* src, std::size_t n) {
    if (n == 0) return win::ok();
    if (!src) return win::failed(ERROR_INVALID_PARAMETER, "src is null");
    auto* const p = reinterpret_cast<std::uint8_t*>(dst);
    std::uint8_t* const end = p + n;
    if (end < p) return win::failed(ERROR_INVALID_PARAMETER, "range wraps past the address space");

    // 1. Enumerate the protection regions the range covers, refusing early.
    Region regions[kMaxRegions];
    std::size_t count = 0;
    bool any_exec = false;
    for (std::uint8_t* at = p; at < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(at, &mbi, sizeof(mbi)) == 0) return win::failed_last("VirtualQuery failed");
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS))
            return win::failed(ERROR_INVALID_ADDRESS, "range is not committed, accessible memory");
        if (count == kMaxRegions)
            return win::failed(ERROR_INVALID_PARAMETER, "range spans too many protection regions");
        std::uint8_t* rend = static_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (rend > end) rend = end;
        regions[count++] = Region{at, std::size_t(rend - at), mbi.Protect};
        any_exec = any_exec || (mbi.Protect & kExec);
        at = rend;
    }

    // 2. Make each region writable; if one fails, undo the ones already made.
    std::size_t made = 0;
    Result r = win::ok();
    for (; made < count; ++made) {
        const DWORD want = (regions[made].old & kExec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        DWORD prev = 0;
        if (!VirtualProtect(regions[made].start, regions[made].len, want, &prev)) {
            r = win::failed_last("VirtualProtect(writable) failed");
            break;
        }
    }

    // 3. Copy only if the whole range is writable.
    if (succeeded(r)) std::memcpy(p, src, n);

    // 4. Restore every region's original protection, success or not.
    for (std::size_t i = 0; i < made; ++i) {
        DWORD prev = 0;
        if (!VirtualProtect(regions[i].start, regions[i].len, regions[i].old, &prev) && succeeded(r))
            r = win::failed_last("VirtualProtect(restore) failed after the write");
    }
    if (any_exec && succeeded(r) && !FlushInstructionCache(GetCurrentProcess(), p, n))
        r = win::failed_last("FlushInstructionCache failed after the write");
    return r;
}

}  // namespace os
}  // namespace bridge
