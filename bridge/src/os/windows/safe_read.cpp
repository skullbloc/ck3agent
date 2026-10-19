// windows/safe_read.cpp — os::safe_read on Windows: ReadProcessMemory on the
// own process, which fails (ERROR_PARTIAL_COPY / ERROR_NOACCESS) instead of
// faulting. Same contract as Linux's process_vm_readv: all n bytes or failed.

#include "win_internal.h"

namespace bridge {
namespace os {

Result safe_read(void* dst, std::uintptr_t src, std::size_t n) {
    if (n == 0) return win::ok();
    if (!dst) return win::failed(ERROR_INVALID_PARAMETER, "dst is null");
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &got))
        return win::failed_last("ReadProcessMemory(own process) failed");
    if (got != n) return win::failed(ERROR_PARTIAL_COPY, "ReadProcessMemory: short read");
    return win::ok();
}

}  // namespace os
}  // namespace bridge
