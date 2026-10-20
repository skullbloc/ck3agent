// posix_shim.cpp — the definitions behind ck3_posix.h (ADR 0001 P5).
// Each function's kind (real / refuses) is stated in ck3_posix.h; a refusal
// sets errno = ENOSYS and returns the POSIX failure value.

#include "ck3_posix.h"

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
thread_local const char* t_dlerror = nullptr;

int refuse() { errno = ENOSYS; return -1; }

DWORD prot_to_page(int prot) {
    const bool r = prot & PROT_READ, w = prot & PROT_WRITE, x = prot & PROT_EXEC;
    if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
}  // namespace

extern "C" {

// ── unistd ──────────────────────────────────────────────────────────────────
long sysconf(int name) {
    if (name == _SC_PAGESIZE) { SYSTEM_INFO si; GetSystemInfo(&si); return static_cast<long>(si.dwPageSize); }
    errno = EINVAL; return -1;
}
pid_t ck3_getpid(void) { return static_cast<pid_t>(GetCurrentProcessId()); }
uid_t geteuid(void) { return 0; }
uid_t getuid(void) { return 0; }
int usleep(unsigned int usec) { Sleep(static_cast<DWORD>((usec + 999) / 1000)); return 0; }
unsigned int ck3_sleep(unsigned int sec) { Sleep(static_cast<DWORD>(sec) * 1000u); return 0; }
unsigned int alarm(unsigned int) { errno = ENOSYS; return 0; }
ssize_t pwrite(int, const void*, std::size_t, long long) { return refuse(); }
ssize_t pread(int fd, void* buf, std::size_t n, long long off) {
    const long long cur = _lseeki64(fd, 0, SEEK_CUR);
    if (cur < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return -1;
    const int got = _read(fd, buf, static_cast<unsigned>(n));
    _lseeki64(fd, cur, SEEK_SET);
    return got;
}
ssize_t readlink(const char* path, char* buf, std::size_t n) {
    if (path && std::strcmp(path, "/proc/self/exe") == 0) {
        char tmp[MAX_PATH * 4];
        const DWORD len = GetModuleFileNameA(nullptr, tmp, static_cast<DWORD>(sizeof(tmp)));
        if (len == 0 || len >= sizeof(tmp)) { errno = EIO; return -1; }
        const std::size_t k = len < n ? len : n;   // readlink does not NUL-terminate
        std::memcpy(buf, tmp, k);
        return static_cast<ssize_t>(k);
    }
    return refuse();
}
int ck3_fsync(int fd) { return _commit(fd); }
int ftruncate(int fd, long long len) { return _chsize_s(fd, len) == 0 ? 0 : -1; }
int fchmod(int, int) { return 0; }
int lstat(const char* path, struct stat* st) { return stat(path, st); }
char* realpath(const char* path, char* resolved) {
    return _fullpath(resolved, path, resolved ? PATH_MAX : 0);
}

// ── fcntl ───────────────────────────────────────────────────────────────────
int ck3_fcntl(int fd, int cmd, ...) {
    if (_get_osfhandle(fd) == -1) { errno = EBADF; return -1; }
    switch (cmd) {
        case F_GETFD: return FD_CLOEXEC;   // handles are opened non-inheritable here
        case F_SETFD: return 0;
        case F_GETFL: return 0;
        case F_SETFL: {
            va_list ap; va_start(ap, cmd); const int fl = va_arg(ap, int); va_end(ap);
            if (fl & O_ASYNC) return refuse();
            return 0;   // O_NONBLOCK on a file descriptor: Windows file I/O does not block on pipes we use
        }
        default: return refuse();
    }
}
int openat(int dirfd, const char* path, int flags, ...) {
    if (dirfd != AT_FDCWD || (flags & O_DIRECTORY)) return refuse();
    int mode = 0;
    if (flags & _O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return _open(path, flags | _O_BINARY, mode ? (_S_IREAD | _S_IWRITE) : 0);
}
int unlinkat(int dirfd, const char* path, int) {
    if (dirfd != AT_FDCWD) return refuse();
    return _unlink(path);
}
int ck3_mkdir(const char* path, int) { return _mkdir(path); }

// ── mman ────────────────────────────────────────────────────────────────────
int mprotect(void* addr, std::size_t len, int prot) {
    DWORD old = 0;
    if (!VirtualProtect(addr, len, prot_to_page(prot), &old)) { errno = EACCES; return -1; }
    if (prot & PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 0;
}
void* mmap(void* addr, std::size_t len, int prot, int flags, int fd, long long) {
    if (!(flags & MAP_ANONYMOUS) || fd != -1) { errno = ENOSYS; return MAP_FAILED; }
    void* p = VirtualAlloc(addr, len, MEM_RESERVE | MEM_COMMIT, prot_to_page(prot));
    if (!p && addr) p = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, prot_to_page(prot));
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    return p;
}
int munmap(void* addr, std::size_t) { return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1; }

// ── uio ─────────────────────────────────────────────────────────────────────
ssize_t process_vm_readv(pid_t pid, const struct iovec* local, unsigned long liovcnt,
                         const struct iovec* remote, unsigned long riovcnt, unsigned long) {
    if (pid != ck3_getpid() || liovcnt != 1 || riovcnt != 1 ||
        local[0].iov_len < remote[0].iov_len) {
        // The bridge only ever reads its own process, one range at a time.
        return refuse();
    }
    const auto r = bridge::os::safe_read(local[0].iov_base,
                                         reinterpret_cast<std::uintptr_t>(remote[0].iov_base),
                                         remote[0].iov_len);
    if (r.status != bridge::os::Status::ok) { errno = EFAULT; return -1; }
    return static_cast<ssize_t>(remote[0].iov_len);
}

// ── syscall ─────────────────────────────────────────────────────────────────
long syscall(long number, ...) {
    if (number == SYS_gettid) return static_cast<long>(GetCurrentThreadId());
    return refuse();
}

// ── pthread ─────────────────────────────────────────────────────────────────
pthread_t pthread_self(void) { return static_cast<pthread_t>(GetCurrentThreadId()); }
int pthread_sigmask(int, const sigset_t*, sigset_t* old) { if (old) *old = 0; return ENOSYS; }
int pthread_setname_np(pthread_t t, const char* name) {
    if (t != pthread_self() || !name) return ENOSYS;
    wchar_t w[64]; int i = 0;
    for (; name[i] && i < 63; ++i) w[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
    w[i] = 0;
    return SUCCEEDED(SetThreadDescription(GetCurrentThread(), w)) ? 0 : EINVAL;
}

// ── signals and timers: refused (no POSIX signals on Windows) ───────────────
int sigaction(int, const struct sigaction*, struct sigaction*) { return refuse(); }
int sigemptyset(sigset_t* s) { *s = 0; return 0; }
int sigfillset(sigset_t* s) { *s = ~0ul; return 0; }
int sigaddset(sigset_t* s, int sig) { *s |= (1ul << (sig & 31)); return 0; }
int sigdelset(sigset_t* s, int sig) { *s &= ~(1ul << (sig & 31)); return 0; }
int sigismember(const sigset_t* s, int sig) { return (*s >> (sig & 31)) & 1; }
int sigprocmask(int, const sigset_t*, sigset_t* old) { if (old) *old = 0; return refuse(); }
int sigaltstack(const stack_t*, stack_t*) { return refuse(); }
int kill(pid_t, int) { return refuse(); }
int timer_create(int, struct sigevent*, timer_t*) { return refuse(); }
int timer_settime(timer_t, int, const struct itimerspec*, struct itimerspec*) { return refuse(); }
int timer_delete(timer_t) { return refuse(); }

// ── time ────────────────────────────────────────────────────────────────────
int clock_gettime(int clk, struct timespec* ts) {
    if (clk == CLOCK_REALTIME) {
        FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
        const unsigned long long t100 = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const unsigned long long unix100 = t100 - 116444736000000000ull;   // 1601 -> 1970
        ts->tv_sec = static_cast<std::time_t>(unix100 / 10000000ull);
        ts->tv_nsec = static_cast<long>((unix100 % 10000000ull) * 100);
        return 0;
    }
    if (clk == CLOCK_MONOTONIC || clk == CLOCK_MONOTONIC_RAW) {
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        ts->tv_sec = static_cast<std::time_t>(c.QuadPart / freq.QuadPart);
        ts->tv_nsec = static_cast<long>((c.QuadPart % freq.QuadPart) * 1000000000ll / freq.QuadPart);
        return 0;
    }
    if (clk == CLOCK_THREAD_CPUTIME_ID || clk == CLOCK_PROCESS_CPUTIME_ID) {
        FILETIME c, e, k, u;
        const BOOL ok = clk == CLOCK_THREAD_CPUTIME_ID ? GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)
                                                        : GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        if (!ok) { errno = EINVAL; return -1; }
        const unsigned long long t = ((static_cast<unsigned long long>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                                     ((static_cast<unsigned long long>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
        ts->tv_sec = static_cast<std::time_t>(t / 10000000ull);
        ts->tv_nsec = static_cast<long>((t % 10000000ull) * 100);
        return 0;
    }
    errno = EINVAL; return -1;
}
struct tm* gmtime_r(const std::time_t* t, struct tm* out) { return gmtime_s(out, t) == 0 ? out : nullptr; }
struct tm* localtime_r(const std::time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : nullptr; }
int nanosleep(const struct timespec* req, struct timespec* rem) {
    Sleep(static_cast<DWORD>(req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000));
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

// ── dlfcn ───────────────────────────────────────────────────────────────────
void* dlsym(void*, const char*) { t_dlerror = "dlsym: no ELF symbol namespace on Windows"; errno = ENOSYS; return nullptr; }
void* dlopen(const char*, int) { t_dlerror = "dlopen: refused on Windows"; errno = ENOSYS; return nullptr; }
int dlclose(void*) { return refuse(); }
const char* dlerror(void) { const char* e = t_dlerror; t_dlerror = nullptr; return e; }
int dladdr(const void* addr, Dl_info* info) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(addr), &m)) return 0;
    static thread_local char name[MAX_PATH * 4];
    if (!GetModuleFileNameA(m, name, sizeof(name))) name[0] = 0;
    info->dli_fname = name; info->dli_fbase = m; info->dli_sname = nullptr; info->dli_saddr = nullptr;
    return 1;
}

// ── dirent ──────────────────────────────────────────────────────────────────
struct ck3_DIR { HANDLE h; WIN32_FIND_DATAA fd; bool first; struct dirent ent; };
DIR* opendir(const char* path) {
    std::string pat = std::string(path) + "\\*";
    DIR* d = new ck3_DIR{};
    d->h = FindFirstFileA(pat.c_str(), &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { delete d; errno = ENOENT; return nullptr; }
    d->first = true;
    return d;
}
struct dirent* readdir(DIR* d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return nullptr;
    d->first = false;
    std::strncpy(d->ent.d_name, d->fd.cFileName, sizeof(d->ent.d_name) - 1);
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->ent;
}
int closedir(DIR* d) { FindClose(d->h); delete d; return 0; }

// ── wait / spawn / pwd: refused ─────────────────────────────────────────────
pid_t waitpid(pid_t, int*, int) { return refuse(); }
int posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]) { return ENOSYS; }
int posix_spawnp(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]) { return ENOSYS; }
int posix_spawn_file_actions_init(posix_spawn_file_actions_t*) { return 0; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*) { return 0; }
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int, const char*, int flags, int) { (void)flags; return ENOSYS; }
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int, int) { return ENOSYS; }
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int) { return ENOSYS; }
int posix_spawnattr_init(posix_spawnattr_t*) { return 0; }
int posix_spawnattr_destroy(posix_spawnattr_t*) { return 0; }
struct passwd* getpwuid(uid_t) { errno = ENOSYS; return nullptr; }
int getpwuid_r(uid_t, struct passwd*, char*, std::size_t, struct passwd** out) { *out = nullptr; return ENOSYS; }

// ── ioctl ───────────────────────────────────────────────────────────────────
int ioctl(int, unsigned long, ...) { return refuse(); }

// ── string ──────────────────────────────────────────────────────────────────
void* memmem(const void* hay, std::size_t hn, const void* needle, std::size_t nn) {
    if (nn == 0) return const_cast<void*>(hay);
    if (hn < nn) return nullptr;
    const unsigned char* h = static_cast<const unsigned char*>(hay);
    const unsigned char* n = static_cast<const unsigned char*>(needle);
    for (std::size_t i = 0; i + nn <= hn; ++i)
        if (h[i] == n[0] && std::memcmp(h + i, n, nn) == 0) return const_cast<unsigned char*>(h + i);
    return nullptr;
}

}  // extern "C"

// ── fault recovery ──────────────────────────────────────────────────────────
// The bridge's protected regions (sigsetjmp(g_recovery_point) + the
// thread-local g_in_protected_region flag) are defined in bridge_init.cpp.
namespace bridge {
extern thread_local sigjmp_buf g_recovery_point;
extern thread_local bool g_in_protected_region;
}
using bridge::g_recovery_point;
using bridge::g_in_protected_region;

namespace {
PVOID g_veh = nullptr;
std::atomic<long> g_recovered{0};
// The recovered faults, for the log: a fixed ring the handler fills (no locks,
// no allocation, no I/O in a vectored handler) and ck3_win_drain_faults empties.
struct FaultRec { std::uintptr_t addr, rip; DWORD code, tid; };
constexpr unsigned kFaultRing = 256;
FaultRec g_fault_ring[kFaultRing];
std::atomic<unsigned> g_fault_head{0};
unsigned g_fault_tail = 0;   // the drainer's only

[[noreturn]] __declspec(noinline) void ck3_fault_trampoline() {
    // Ordinary thread context: the vectored handler has returned. This is the
    // Linux siglongjmp from the SIGSEGV handler, minus the handler frame.
    g_in_protected_region = false;
    siglongjmp(g_recovery_point, 1);
}

LONG CALLBACK fault_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool memory_fault = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
                              code == EXCEPTION_DATATYPE_MISALIGNMENT;
    if (!memory_fault || !g_in_protected_region) return EXCEPTION_CONTINUE_SEARCH;
    g_recovered.fetch_add(1, std::memory_order_relaxed);
    {
        const unsigned h = g_fault_head.fetch_add(1, std::memory_order_relaxed);
        FaultRec& r = g_fault_ring[h % kFaultRing];
        r.addr = ep->ExceptionRecord->NumberParameters >= 2
                     ? static_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]) : 0;
        // A non-canonical address (every poisoned RVA) raises #GP, which Windows
        // reports as address -1. The poisoned value is still in a register: take it.
        if (r.addr == ~static_cast<std::uintptr_t>(0)) {
            const CONTEXT* c = ep->ContextRecord;
            const DWORD64 regs[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9,
                                    c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
            for (DWORD64 v : regs)
                if ((v >> 52) == 0x7FF) { r.addr = static_cast<std::uintptr_t>(v); break; }
        }
        r.rip = static_cast<std::uintptr_t>(ep->ContextRecord->Rip);
        r.code = code;
        r.tid = GetCurrentThreadId();
    }
    CONTEXT* c = ep->ContextRecord;
    // A call frame for the trampoline: 16-byte aligned, minus the return slot,
    // below the faulting frame's red zone (Windows x64 has none; keep 128 B anyway).
    std::uintptr_t sp = (static_cast<std::uintptr_t>(c->Rsp) - 128) & ~static_cast<std::uintptr_t>(0xF);
    sp -= 8;
    c->Rsp = sp;
    c->Rip = reinterpret_cast<DWORD64>(&ck3_fault_trampoline);
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

// Writes the faults recovered since the last call, one line each: the faulting
// address (for a poisoned RVA, CK3_WIN_UNMAPPED_N(i) + base + field offset, so i
// names the constant: report.json poison_index), and the RIP as this DLL's RVA.
extern "C" void ck3_win_drain_faults(FILE* log) {
    const unsigned head = g_fault_head.load(std::memory_order_acquire);
    if (head - g_fault_tail > kFaultRing) g_fault_tail = head - kFaultRing;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ck3_win_drain_faults), &self);
    for (; g_fault_tail != head; ++g_fault_tail) {
        const FaultRec r = g_fault_ring[g_fault_tail % kFaultRing];
        if (!log) continue;
        const unsigned long long a = r.addr;
        const bool poisoned = (a >> 52) == 0x7FF;
        std::fprintf(log, "[CK3-Bridge] fault recovered: code=%08lx tid=%lu addr=0x%llx%s rip=dll+0x%llx\n",
                     static_cast<unsigned long>(r.code), static_cast<unsigned long>(r.tid), a,
                     poisoned ? " (poisoned RVA)" : "",
                     static_cast<unsigned long long>(r.rip - reinterpret_cast<std::uintptr_t>(self)));
        if (poisoned)
            std::fprintf(log, "[CK3-Bridge]   poison_index=%llu field_offset=0x%llx\n",
                         (a >> 40) & 0xFFF, (a - 0x140000000ull) & 0xFFFFFFFFFFull);
    }
    if (log) std::fflush(log);
}

// ── the dispatch watchdog's interrupt (ADR 0001 P6) ─────────────────────────
// Linux: SIGALRM on the game thread, whose handler siglongjmps to the point with
// 2 (timeout). Windows has no signal to deliver into another thread, so the
// watchdog thread does what the kernel does there: suspend the thread, and if it
// is still inside the protected region, point it at a trampoline that jumps with
// 2. Only while the thread runs code inside [lo, hi) ranges the caller names (the
// game image, this DLL): a thread suspended inside ntdll or the CRT may hold the
// heap or loader lock, and jumping out of that frame would leave it held. Such a
// thread is resumed untouched; the caller retries.
namespace {
[[noreturn]] __declspec(noinline) void ck3_watchdog_trampoline() {
    g_in_protected_region = false;
    siglongjmp(g_recovery_point, 2);
}
}  // namespace

extern "C" int ck3_win_watchdog_interrupt(unsigned long tid, const volatile bool* in_region,
                                          const std::uintptr_t* lo, const std::uintptr_t* hi, int n_ranges,
                                          std::uintptr_t* rip_out) {
    if (rip_out) *rip_out = 0;
    if (tid == GetCurrentThreadId() || !in_region) return -1;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!t) return -1;
    int rc = -1;
    if (SuspendThread(t) != static_cast<DWORD>(-1)) {
        alignas(16) CONTEXT c;
        std::memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &c)) {
            const std::uintptr_t rip = static_cast<std::uintptr_t>(c.Rip);
            if (rip_out) *rip_out = rip;
            bool inside = false;
            for (int i = 0; i < n_ranges; ++i) inside = inside || (rip >= lo[i] && rip < hi[i]);
            if (!*in_region) {
                rc = 1;                       // left the region already: nothing to do
            } else if (!inside) {
                rc = 2;                       // in a system DLL: not now
            } else {
                std::uintptr_t sp = (static_cast<std::uintptr_t>(c.Rsp) - 128) & ~static_cast<std::uintptr_t>(0xF);
                sp -= 8;
                c.Rsp = sp;
                c.Rip = reinterpret_cast<DWORD64>(&ck3_watchdog_trampoline);
                rc = SetThreadContext(t, &c) ? 0 : -1;
            }
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    return rc;
}

extern "C" int ck3_win_install_fault_recovery(void) {
    if (g_veh) return 0;
    g_veh = AddVectoredExceptionHandler(1, fault_veh);
    return g_veh ? 0 : -1;
}
extern "C" int ck3_win_rearm_fault_recovery(void) {
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    return ck3_win_install_fault_recovery();
}
