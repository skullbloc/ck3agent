// windows/threads.cpp — os::thread_id and os::set_thread_name on Windows.
//
// thread_id is GetCurrentThreadId (the id a crash dump and Process Explorer
// show). set_thread_name is SetThreadDescription, resolved from kernel32 at
// run time: it exists from Windows 10 1607 on (and in wine), and a missing
// export is a `failed` with ERROR_PROC_NOT_FOUND, not a load failure. The
// name is UTF-8 in, UTF-16 to the kernel, and is not truncated (Windows has
// no 15-byte limit).

#include "win_internal.h"

#include <cstring>

namespace bridge {
namespace os {

namespace {
using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
}  // namespace

Result thread_id(std::uint64_t* out) {
    if (!out) return win::failed(ERROR_INVALID_PARAMETER, "out is null");
    *out = GetCurrentThreadId();
    return win::ok();
}

Result set_thread_name(const char* name) {
    if (!name) return win::failed(ERROR_INVALID_PARAMETER, "name is null");
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto fn = k32 ? reinterpret_cast<SetDescFn>(
                        reinterpret_cast<void*>(GetProcAddress(k32, "SetThreadDescription")))
                  : nullptr;
    if (!fn) return win::failed(ERROR_PROC_NOT_FOUND, "SetThreadDescription is not exported here");
    wchar_t wide[256];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 256) == 0)
        return win::failed_last("name is not valid UTF-8 or is longer than 255 characters");
    const HRESULT hr = fn(GetCurrentThread(), wide);
    if (FAILED(hr)) return win::failed(int(hr), "SetThreadDescription failed");
    return win::ok();
}

}  // namespace os
}  // namespace bridge
