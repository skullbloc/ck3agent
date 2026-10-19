// windows/common.cpp — helpers shared by the Windows cells (win_internal.h).

#include "win_internal.h"

namespace bridge {
namespace os {
namespace win {

bool main_headers(const std::uint8_t** view, std::size_t* size, pe::Headers* h, Result* why) {
    HMODULE m = GetModuleHandleW(nullptr);
    if (!m) { *why = failed_last("GetModuleHandleW(NULL) failed"); return false; }
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(m, &mbi, sizeof(mbi)) == 0) {
        *why = failed_last("VirtualQuery(image base) failed");
        return false;
    }
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(m);
    const std::size_t avail = std::size_t(static_cast<const std::uint8_t*>(mbi.BaseAddress) +
                                          mbi.RegionSize - base);
    if (pe::parse_headers(base, avail, h) != pe::ParseError::none) {
        *why = failed(0, "the loaded image's PE headers did not parse");
        return false;
    }
    *view = base;
    *size = avail;
    return true;
}

bool readable(const std::uint8_t* p, std::size_t n) {
    const std::uint8_t* end = p + n;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect;
        if ((prot & PAGE_GUARD) || (prot & PAGE_NOACCESS) || prot == 0) return false;
        const DWORD r = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(prot & r)) return false;
        p = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

bool module_path_w(HMODULE module, wchar_t* out, std::size_t cap) {
    if (cap == 0) return false;
    const DWORD n = GetModuleFileNameW(module, out, DWORD(cap));
    // Truncation returns cap (and, on Vista+, ERROR_INSUFFICIENT_BUFFER).
    return n != 0 && n < cap;
}

bool utf16_to_utf8(const wchar_t* in, char* out, std::size_t cap) {
    if (cap == 0) return false;
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in, -1, out, int(cap),
                                      nullptr, nullptr);
    if (n <= 0) { out[0] = '\0'; return false; }
    return true;
}

}  // namespace win
}  // namespace os
}  // namespace bridge
