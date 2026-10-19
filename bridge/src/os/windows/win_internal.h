#pragma once
// windows/win_internal.h — helpers shared by the Windows cells (common.cpp).
// Internal to bridge/src/os/windows/; never included outside it.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "os/os.h"
#include "os/pe_image.h"

#include <cstddef>
#include <cstdint>

namespace bridge {
namespace os {
namespace win {

inline Result ok() { return Result{Status::ok, 0, ""}; }
inline Result failed(int err, const char* d) { return Result{Status::failed, err, d}; }
inline Result failed_last(const char* d) { return Result{Status::failed, int(GetLastError()), d}; }

// The main module's headers, through the bytes VirtualQuery reports as one
// region from the base. *why is set on false.
bool main_headers(const std::uint8_t** view, std::size_t* size, pe::Headers* h, Result* why);

// True when every page of [p, p+n) is committed and readable (no
// PAGE_NOACCESS, no PAGE_GUARD). n == 0 is true.
bool readable(const std::uint8_t* p, std::size_t n);

// GetModuleFileNameW(module) as UTF-16 (wide, cap in wchar_t) and as UTF-8.
// False when the path does not fit or the call failed.
bool module_path_w(HMODULE module, wchar_t* out, std::size_t cap);
bool utf16_to_utf8(const wchar_t* in, char* out, std::size_t cap);

}  // namespace win
}  // namespace os
}  // namespace bridge
