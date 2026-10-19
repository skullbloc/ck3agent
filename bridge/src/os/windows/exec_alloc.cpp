// windows/exec_alloc.cpp — os::alloc_exec_near / free_exec and the unwind
// registration (os::register_unwind / unregister_unwind) on Windows.
//
// alloc_exec_near reserves+commits a PAGE_EXECUTE_READWRITE block whose base
// is within +-2 GB of the target, so a 5-byte rel32 jmp placed at the target
// can reach it (the trampoline reach the inline-hook engine needs). It probes
// outward from the target with VirtualQuery, skipping anything not MEM_FREE,
// and allocates at an allocation-granularity boundary.
//
// register_unwind is the ADR amendment "Trampolines need unwind data": code in
// VirtualAlloc'd pages has no RUNTIME_FUNCTION, so an exception or stack walk
// that crosses it fail-fasts. Each trampoline's RUNTIME_FUNCTION (begin/end
// and the UNWIND_INFO the caller wrote into the block) is handed to
// RtlAddFunctionTable. The RUNTIME_FUNCTION array must outlive the
// registration, so it is copied into a process-lifetime registry keyed by the
// block base; unregister_unwind (and free_exec) delete the table and free it.

#include "win_internal.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace bridge {
namespace os {

namespace {

constexpr std::uintptr_t kReach = 0x7fff0000ull;  // just under 2 GB, rel32 headroom

struct Registered {
    std::uintptr_t    base;
    RUNTIME_FUNCTION* table;  // malloc'd; lives until unregister
};

std::mutex& reg_mutex() {
    static std::mutex m;
    return m;
}
std::vector<Registered>& registry() {
    static std::vector<Registered> v;
    return v;
}

std::uintptr_t align_down(std::uintptr_t a, std::uintptr_t g) { return a - (a % g); }

}  // namespace

Result alloc_exec_near(std::uintptr_t target, std::size_t size, ExecBlock* out) {
    if (!out) return win::failed(ERROR_INVALID_PARAMETER, "out is null");
    out->base = 0;
    out->size = 0;
    if (size == 0) return win::failed(ERROR_INVALID_PARAMETER, "size is 0");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const auto lo_limit = target > kReach ? target - kReach : std::uintptr_t(0);
    const std::uintptr_t hi_limit = target + kReach;
    const std::uintptr_t start = align_down(target, gran);

    // Probe up and down in allocation-granularity steps for a MEM_FREE slot.
    for (std::uintptr_t step = 0; ; step += gran) {
        bool any = false;
        const std::uintptr_t candidates[2] = {start + step, start - step};
        for (int d = 0; d < 2; ++d) {
            const std::uintptr_t at = candidates[d];
            if (at < lo_limit || at > hi_limit) continue;
            if (at == 0) continue;
            any = true;
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof(mbi)) == 0) continue;
            if (mbi.State != MEM_FREE) continue;
            void* p = VirtualAlloc(reinterpret_cast<LPVOID>(at), size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
            if (!p) continue;
            const auto base = reinterpret_cast<std::uintptr_t>(p);
            if (base < lo_limit || base + size > hi_limit) { VirtualFree(p, 0, MEM_RELEASE); continue; }
            SYSTEM_INFO s2;
            GetSystemInfo(&s2);
            out->base = base;
            out->size = ((size + s2.dwPageSize - 1) / s2.dwPageSize) * s2.dwPageSize;
            return win::ok();
        }
        if (step != 0 && !any) break;  // both directions left the +-2 GB window
    }
    return win::failed(ERROR_NOT_ENOUGH_MEMORY, "no free region within +-2 GB of the target");
}

Result free_exec(ExecBlock* block) {
    if (!block) return win::failed(ERROR_INVALID_PARAMETER, "block is null");
    if (block->base == 0) return win::ok();
    unregister_unwind(*block);  // drop any table before the pages go
    const bool ok = VirtualFree(reinterpret_cast<LPVOID>(block->base), 0, MEM_RELEASE);
    Result r = ok ? win::ok() : win::failed_last("VirtualFree failed");
    if (ok) { block->base = 0; block->size = 0; }
    return r;
}

Result register_unwind(const ExecBlock& block, const UnwindEntry* entries, std::size_t n) {
    if (block.base == 0) return win::failed(ERROR_INVALID_PARAMETER, "block is empty");
    if (n == 0 || !entries) return win::failed(ERROR_INVALID_PARAMETER, "no unwind entries");

    auto* table = static_cast<RUNTIME_FUNCTION*>(std::malloc(n * sizeof(RUNTIME_FUNCTION)));
    if (!table) return win::failed(ERROR_NOT_ENOUGH_MEMORY, "RUNTIME_FUNCTION table alloc failed");
    for (std::size_t i = 0; i < n; ++i) {
        table[i].BeginAddress = entries[i].begin;
        table[i].EndAddress = entries[i].end;
        table[i].UnwindInfoAddress = entries[i].unwind_info;
    }
    if (!RtlAddFunctionTable(table, DWORD(n), block.base)) {
        std::free(table);
        return win::failed(0, "RtlAddFunctionTable failed");
    }
    std::lock_guard<std::mutex> lk(reg_mutex());
    registry().push_back(Registered{block.base, table});
    return win::ok();
}

Result unregister_unwind(const ExecBlock& block) {
    std::lock_guard<std::mutex> lk(reg_mutex());
    auto& v = registry();
    bool found = false;
    for (std::size_t i = 0; i < v.size();) {
        if (v[i].base == block.base) {
            RtlDeleteFunctionTable(v[i].table);
            std::free(v[i].table);
            v.erase(v.begin() + std::ptrdiff_t(i));
            found = true;
        } else {
            ++i;
        }
    }
    return found ? win::ok()
                 : Result{Status::ok, 0, "no unwind table was registered for this block"};
}

}  // namespace os
}  // namespace bridge
