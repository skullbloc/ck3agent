// ck3_posix.h — the POSIX subset the bridge core uses, on Windows (ADR 0001 P5).
//
// The bridge core (bridge/src/*.cpp) was written against Linux. On Windows it is
// compiled with this directory FIRST on the include path, so <unistd.h>,
// <sys/mman.h>, <pthread.h>, ... resolve here. Every name below is one of three
// kinds, and the kind is stated beside it:
//
//   real     does what the POSIX call does, on Win32 or through an os:: cell
//            (os::safe_read for process_vm_readv, VirtualProtect for mprotect)
//   refuses  returns -1 / nullptr with errno = ENOSYS: a Linux facility the
//            Windows bridge does not use (signals, timers, perf_event, spawn).
//            The Windows code paths that would need it are split per platform
//            in the caller (bridge_init, ipc_server, runtime_resolver), so a
//            refusal here is never the behaviour of a working feature
//   type     a declaration so shared code compiles (ucontext_t, siginfo_t)
//
// Never add a silent no-op that reports success for something that did not
// happen: that is the "a default manufactures an answer" failure.
#pragma once

#ifndef _WIN32
#error "ck3_posix.h is the Windows POSIX shim; it must not be on a POSIX include path"
#endif

#include <corecrt.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef ENOSYS
#define ENOSYS 40
#endif

extern "C" {

// ── types ────────────────────────────────────────────────────────────── type
typedef std::intptr_t  ssize_t;
typedef int            pid_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
#ifndef _MODE_T_DEFINED_CK3
#define _MODE_T_DEFINED_CK3
typedef int            mode_t;
#endif
typedef int            socklen_t;
typedef void*          timer_t;
typedef unsigned long  sigset_t;
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// ── <unistd.h> ─────────────────────────────────────────────────────────────
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define W_OK 2
#define R_OK 4
#define X_OK 0   // Windows has no execute bit; access(X_OK) reports existence
#define _SC_PAGESIZE 30
long     sysconf(int name);                                  // real (_SC_PAGESIZE)
pid_t    ck3_getpid(void);                                   // real
uid_t    geteuid(void);                                      // real: 0 (no uid on Windows; paths use the paths cell)
uid_t    getuid(void);                                       // real: 0
int      usleep(unsigned int usec);                          // real (Sleep, ms granularity, rounds up)
unsigned int ck3_sleep(unsigned int sec);                    // real
unsigned int alarm(unsigned int sec);                        // refuses: returns 0, errno ENOSYS (no SIGALRM)
ssize_t  pwrite(int fd, const void* buf, std::size_t n, long long off);  // refuses (no /proc/self/mem)
ssize_t  pread(int fd, void* buf, std::size_t n, long long off);         // real (files only)
ssize_t  readlink(const char* path, char* buf, std::size_t n);           // real for "/proc/self/exe"; refuses otherwise
int      ck3_fsync(int fd);                                  // real (_commit)
int      ftruncate(int fd, long long len);                   // real (_chsize_s)
int      fchmod(int fd, int mode);                           // real: ok, a no-op on Windows ACLs (mode bits do not exist)
int      lstat(const char* path, struct stat* st);           // real (stat: Windows paths here are not symlinks)
char*    realpath(const char* path, char* resolved);         // real (_fullpath)

// UCRT spells these with an underscore; the POSIX spellings are deprecated aliases.
#define getpid  ck3_getpid
#define sleep   ck3_sleep
#define fsync   ck3_fsync

// ── <fcntl.h> additions ────────────────────────────────────────────────────
#ifndef O_CLOEXEC
#define O_CLOEXEC   _O_NOINHERIT
#endif
#define O_NOFOLLOW  0          // no symlink semantics to guard against
#define O_NONBLOCK  0          // nothing to set: files do not block here, and the IPC
                               // sockets are polled (WSAPoll) before every read
#define O_ASYNC     0x20000000 // refused by fcntl
#define O_DIRECTORY 0x10000000 // refused by open
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_SETOWN_EX 15
#define F_SETSIG 10
#define F_OWNER_TID 0
#define FD_CLOEXEC 1
#define AT_FDCWD (-100)
int  ck3_fcntl(int fd, int cmd, ...);                        // real: F_GETFD/F_SETFD/F_GETFL/F_SETFL(O_NONBLOCK) answer; others refuse
int  openat(int dirfd, const char* path, int flags, ...);    // real for AT_FDCWD; refuses otherwise
int  unlinkat(int dirfd, const char* path, int flags);       // real for AT_FDCWD; refuses otherwise
#define fcntl ck3_fcntl
struct f_owner_ex { int type; pid_t pid; };

// ── <sys/stat.h> additions ─────────────────────────────────────────────────
#define S_ISSOCK(m) 0   // no AF_UNIX socket files on this build
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_ISVTX 01000
int ck3_mkdir(const char* path, int mode);                   // real (_mkdir; mode ignored: no POSIX modes)

// ── <sys/mman.h> ───────────────────────────────────────────────────────────
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void*)-1)
int   mprotect(void* addr, std::size_t len, int prot);       // real (VirtualProtect)
void* mmap(void* addr, std::size_t len, int prot, int flags, int fd, long long off);  // real for anonymous private; refuses file maps
int   munmap(void* addr, std::size_t len);                   // real for mmap'd anonymous blocks

// ── <sys/uio.h> ────────────────────────────────────────────────────────────
struct iovec { void* iov_base; std::size_t iov_len; };
// real for pid == getpid(): each remote range is copied with os::safe_read
// (ReadProcessMemory on this process), which fails instead of faulting, as
// process_vm_readv returns EFAULT on Linux. Returns the bytes copied before the
// first failing range, or -1 (errno EFAULT) if none.
ssize_t process_vm_readv(pid_t pid, const struct iovec* local, unsigned long liovcnt,
                         const struct iovec* remote, unsigned long riovcnt, unsigned long flags);

// ── <sys/syscall.h> ────────────────────────────────────────────────────────
#define SYS_gettid       186
#define SYS_arch_prctl   158
#define __NR_perf_event_open 298
long syscall(long number, ...);                              // real for SYS_gettid (GetCurrentThreadId); refuses others

// ── <asm/prctl.h> ──────────────────────────────────────────────────────────
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

// ── <pthread.h> ────────────────────────────────────────────────────────────
typedef unsigned long pthread_t;
pthread_t pthread_self(void);                                // real (GetCurrentThreadId)
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old);  // refuses: no POSIX signals
int pthread_setname_np(pthread_t t, const char* name);       // real for the calling thread (SetThreadDescription); refuses others

// ── <signal.h> additions ───────────────────────────────────────────────────
#define SIGTRAP  5
#define SIGBUS   7
#define SIGALRM  14
#define SIGPIPE  13
#define SIGUSR1  10
#define SIGRTMIN 34
#define SA_SIGINFO 0x4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SIGEV_THREAD_ID 4
#define SIGEV_SIGNAL 0
typedef struct {
    int si_signo, si_errno, si_code;
    void* si_addr;
    int si_fd;
    long si_band;
    union { int sival_int; void* sival_ptr; } si_value;
} siginfo_t;
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    sigset_t sa_mask;
    int sa_flags;
};
struct sigevent {
    int sigev_notify, sigev_signo;
    union { int sival_int; void* sival_ptr; } sigev_value;
    union { int _tid; } _sigev_un;   // glibc's spelling, which the Linux code uses
};
#define sigev_notify_thread_id _sigev_un._tid
struct itimerspec { struct timespec it_interval, it_value; };
typedef struct { void* ss_sp; int ss_flags; std::size_t ss_size; } stack_t;
int sigaction(int sig, const struct sigaction* act, struct sigaction* old);   // refuses
int sigemptyset(sigset_t* set);                              // real (bit set, no effect)
int sigfillset(sigset_t* set);                               // real (bit set, no effect)
int sigaddset(sigset_t* set, int sig);                       // real (bit set, no effect)
int sigdelset(sigset_t* set, int sig);                       // real (bit set, no effect)
int sigismember(const sigset_t* set, int sig);               // real
int sigprocmask(int how, const sigset_t* set, sigset_t* old);// refuses
int sigaltstack(const stack_t* ss, stack_t* old);            // refuses
int kill(pid_t pid, int sig);                                // refuses
int timer_create(int clock, struct sigevent* sev, timer_t* t);   // refuses
int timer_settime(timer_t t, int flags, const struct itimerspec* v, struct itimerspec* old);  // refuses
int timer_delete(timer_t t);                                 // refuses

// ── <setjmp.h>: sigsetjmp / siglongjmp ─────────────────────────────── real
// The bridge's guarded regions are `if (sigsetjmp(g_recovery_point, 1) == 0)`,
// and the Linux SIGSEGV handler siglongjmps to the point. Here the point is the
// MSVC CRT's own _setjmp buffer (every non-volatile register, xmm6-xmm15 included,
// as glibc's sigsetjmp saves its callee-saved set), with its Frame field zeroed
// after the first return: a longjmp to a Frame of 0 restores the registers and does
// NOT unwind, so the abandoned frames are skipped exactly as siglongjmp skips them
// on Linux (a protected body owns no resources). The fault path is the bridge's
// vectored handler, which does not longjmp itself (ADR amendment "No longjmp out of
// a vectored handler"): it rewrites the faulting thread's RIP to a trampoline that
// calls siglongjmp from ordinary thread context. See posix_shim.cpp.
//
// NOT clang's __builtin_setjmp: on Win64 it saves the frame ADDRESS and its
// longjmp restores that into rbp, but a Win64 frame keeps rbp at an offset from it
// (rbp = rsp + 0x80 in the bridge's large frames), so every rbp-relative local is
// read 0x80 bytes off after the jump. Measured: a reply std::string read as a
// heap pointer of 0x1B4, and the game died on the next write (P5 full1 run).
}  // extern "C"
#include <setjmp.h>
extern "C" {
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(buf, savemask) \
    (__extension__({ int ck3_sj_r_ = _setjmp(buf); \
                     if (ck3_sj_r_ == 0) reinterpret_cast<_JUMP_BUFFER*>(buf)->Frame = 0; \
                     ck3_sj_r_; }))
#define siglongjmp(buf, val) longjmp((buf), (val) ? (val) : 1)

// ── <ucontext.h> ─────────────────────────────────────────────────────────── type
// Read only inside Linux signal handlers, which never run on Windows.
enum { REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RDI,
       REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP, REG_RIP, REG_EFL,
       REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2, NGREG };
typedef long long greg_t;
typedef greg_t gregset_t[NGREG];
typedef struct { gregset_t gregs; void* fpregs; unsigned long long __reserved1[8]; } mcontext_t;
typedef struct ucontext_t {
    unsigned long uc_flags; struct ucontext_t* uc_link; stack_t uc_stack;
    mcontext_t uc_mcontext; sigset_t uc_sigmask;
} ucontext_t;

// ── <time.h> additions ─────────────────────────────────────────────────────
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#endif
int clock_gettime(int clk, struct timespec* ts);             // real (QPC for monotonic, GetSystemTimePreciseAsFileTime for realtime)
struct tm* gmtime_r(const time_t* t, struct tm* out);        // real (gmtime_s)
struct tm* localtime_r(const time_t* t, struct tm* out);     // real (localtime_s)
int nanosleep(const struct timespec* req, struct timespec* rem);  // real (Sleep)

// ── <dlfcn.h> ──────────────────────────────────────────────────────────────
#define RTLD_DEFAULT ((void*)0)
#define RTLD_NEXT    ((void*)-1)
#define RTLD_LAZY 1
#define RTLD_NOW  2
#define RTLD_NOLOAD 4
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
void* dlsym(void* handle, const char* name);                 // refuses: no ELF symbol namespace (the engine's exports are not the bridge's anchors)
void* dlopen(const char* name, int flags);                   // refuses
int   dlclose(void* h);                                      // refuses
const char* dlerror(void);                                   // real: the last refusal's text
int   dladdr(const void* addr, Dl_info* info);               // real (GetModuleHandleEx + GetModuleFileName)

// ── <dirent.h> ─────────────────────────────────────────────────────────────
struct dirent { char d_name[260]; unsigned char d_type; };
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
typedef struct ck3_DIR DIR;
DIR* opendir(const char* path);                              // real (FindFirstFile)
struct dirent* readdir(DIR* d);                              // real
int closedir(DIR* d);                                        // real

// ── <poll.h>, <sys/socket.h>, <sys/un.h>, <sys/ioctl.h> ─────────────────────
// These headers include Winsock (pollfd, POLLIN, socket, send, recv, WSAPoll);
// see posix/ck3_socket.h. The only user is ipc_server.cpp, which is split per
// platform where the semantics differ (TCP loopback + token, closesocket).

// ── <sys/wait.h>, <spawn.h> ─────────────────────────────────────────── refuses
#define WNOHANG 1
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
typedef struct { int _unused; } posix_spawn_file_actions_t;
typedef struct { int _unused; } posix_spawnattr_t;
pid_t waitpid(pid_t pid, int* status, int options);
int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* fa,
                const posix_spawnattr_t* attr, char* const argv[], char* const envp[]);
int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* fa,
                 const posix_spawnattr_t* attr, char* const argv[], char* const envp[]);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* fa, int fd, const char* path, int flags, int mode);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* fa, int fd, int newfd);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* fa, int fd);
int posix_spawnattr_init(posix_spawnattr_t* a);
int posix_spawnattr_destroy(posix_spawnattr_t* a);

// ── <pwd.h> ─────────────────────────────────────────────────────────── refuses
struct passwd { char* pw_name; char* pw_dir; uid_t pw_uid; gid_t pw_gid; char* pw_shell; };
struct passwd* getpwuid(uid_t uid);
int getpwuid_r(uid_t uid, struct passwd* pw, char* buf, std::size_t n, struct passwd** out);

// ── <string.h> additions ───────────────────────────────────────────────────
void* memmem(const void* hay, std::size_t hn, const void* needle, std::size_t nn);  // real
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

// ── fault recovery (Windows only; called from the bridge's Windows init) ───
// Installs the first-position vectored handler that turns a memory fault on a
// thread inside a protected region (g_in_protected_region) into a siglongjmp to
// that thread's g_recovery_point, via the trampoline. Returns 0 on success.
int ck3_win_install_fault_recovery(void);
int ck3_win_rearm_fault_recovery(void);
// Logs the faults recovered since the last call (see posix_shim.cpp).
void ck3_win_drain_faults(FILE* log);
// The dispatch watchdog's interrupt: suspend thread tid; if *in_region and its RIP
// lies in one of the n [lo, hi) ranges, redirect it to siglongjmp(point, 2).
// Returns 0 interrupted, 1 already out of the region, 2 outside the ranges (not
// now), -1 failed. *rip_out gets the RIP seen (see posix_shim.cpp).
int ck3_win_watchdog_interrupt(unsigned long tid, const volatile bool* in_region,
                               const std::uintptr_t* lo, const std::uintptr_t* hi, int n_ranges,
                               std::uintptr_t* rip_out);

}  // extern "C"

// C++ overloads that UCRT does not give the POSIX spellings for.
inline int mkdir(const char* p, int m) { return ck3_mkdir(p, m); }
