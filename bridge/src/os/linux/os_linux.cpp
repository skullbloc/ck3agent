// os_linux.cpp — the Linux implementation of bridge/include/os/os.h.
//
// THIN ADAPTERS. Where the bridge already has a callable Linux function, the
// adapter forwards to it and adds nothing:
//   safe_read          -> bridge::safe_read_bytes()    (bridge.h, process_vm_readv)
//   write_protected    -> bridge::write_to_readonly()  (bridge.h, /proc/self/mem)
//   binary_identity    -> bridge::build_id_witness()   (build_id_witness.cpp)
//   ipc_endpoint/paths -> bridge::instance_*()         (instance_paths.cpp)
//   thread_id          -> bridge::current_tid()        (thread_registry.h)
//
// TWO TRANSCRIPTIONS. is_ck3_process() and find_binary_base() are file-static
// in bridge_init.cpp, so nothing outside that file can call them. Until P2
// moves bridge_init.cpp onto this layer (and deletes its copies), the two
// predicates are restated here, line for line, and
// bridge/src/os/test/test_os_linux_adapters.cpp pins them against the direct
// syscalls. Change one copy, change both.
//
// NOT ROUTED. The IPC listener (ipc_server.cpp server_loop/reader_loop) and
// input injection (action_handlers.cpp, XTest + /dev/uinput) exist on Linux
// but only as file-static code with its own state. A second listener here
// would be a second set of semantics, so these return Status::not_routed and
// name the owner.
//
// The bridge (libck3_bridge.so) does not link this file yet; see os.h.

#include "os/os.h"

#include "bridge.h"            // safe_read_bytes, write_to_readonly
#include "build_id_witness.h"
#include "instance_paths.h"
#include "thread_registry.h"   // current_tid

#include <cerrno>
#include <cstring>
#include <link.h>
#include <pthread.h>
#include <unistd.h>

namespace bridge {
namespace os {

namespace {

Result ok()                         { return Result{Status::ok, 0, ""}; }
Result failed(int err, const char* d) { return Result{Status::failed, err, d}; }
Result not_routed(const char* d)    { return Result{Status::not_routed, 0, d}; }

}  // namespace

Platform platform() { return Platform::kLinux; }

// ── process identity ────────────────────────────────────────────────────────
// Transcription of is_ck3_process() (bridge_init.cpp): readlink
// /proc/self/exe, basename after the last '/' must be exactly "ck3". A path
// with no '/' is not the game (strrchr returns null there too).

Result exe_path_is_game(const char* exe_path, bool* is_game) {
    if (!is_game) return failed(EINVAL, "is_game is null");
    *is_game = false;
    if (!exe_path) return failed(EINVAL, "exe_path is null");
    const char* basename = strrchr(exe_path, '/');
    *is_game = basename && strcmp(basename + 1, "ck3") == 0;
    return ok();
}

Result process_identity(ProcessIdentity* out) {
    if (!out) return failed(EINVAL, "out is null");
    out->exe_path[0] = '\0';
    out->is_game = false;
    ssize_t len = readlink("/proc/self/exe", out->exe_path, sizeof(out->exe_path) - 1);
    if (len <= 0) {
        const int err = errno;
        out->exe_path[0] = '\0';
        return failed(err, "readlink(/proc/self/exe) failed");
    }
    out->exe_path[len] = '\0';
    return exe_path_is_game(out->exe_path, &out->is_game);
}

// ── image base ──────────────────────────────────────────────────────────────
// Transcription of find_binary_base() method 1 (bridge_init.cpp): the first
// dl_iterate_phdr entry with an empty name (the main executable) or a name
// containing "/ck3"; end is the max over PT_LOAD of addr + p_vaddr + p_memsz.
// find_binary_base() falls back to /proc/self/maps when that yields base 0;
// the fallback is diagnostic and is not routed here, so base 0 is `failed`.

Result image_base(ImageInfo* out) {
    if (!out) return failed(EINVAL, "out is null");
    out->base = 0;
    out->end = 0;
    struct Found { std::uintptr_t base; std::uintptr_t end; bool hit; };
    Found f{0, 0, false};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* userdata) -> int {
        auto* d = static_cast<Found*>(userdata);
        if (info->dlpi_name[0] != '\0' && !strstr(info->dlpi_name, "/ck3")) return 0;
        std::uintptr_t end = 0;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD) continue;
            const std::uintptr_t e = info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
            if (e > end) end = e;
        }
        d->base = info->dlpi_addr;
        d->end = end;
        d->hit = true;
        return 1;  // stop iteration
    }, &f);
    if (!f.hit) return failed(0, "dl_iterate_phdr: no main-executable entry");
    if (f.base == 0)
        return failed(0, "main executable load address is 0 (not PIE); the "
                         "/proc/self/maps fallback lives in bridge_init.cpp "
                         "find_binary_base() and is not routed");
    out->base = f.base;
    out->end = f.end;
    return ok();
}

// ── binary identity ─────────────────────────────────────────────────────────

Result binary_identity(BinaryIdentity* out) {
    if (!out) return failed(EINVAL, "out is null");
    const BuildIdWitness& w = bridge::build_id_witness();
    out->format = IdentityFormat::elf_gnu_build_id;
    std::size_t n = w.observed_hex.size();
    if (n > sizeof(out->observed_hex) - 1) n = sizeof(out->observed_hex) - 1;
    memcpy(out->observed_hex, w.observed_hex.data(), n);
    out->observed_hex[n] = '\0';
    out->check = bridge::build_id_check_string(w.status);
    out->image_sha256[0] = '\0';  // no file-hash witness in the Linux bridge yet
    out->image_size = 0;
    return ok();
}

// ── guarded memory ──────────────────────────────────────────────────────────

Result safe_read(void* dst, std::uintptr_t src, std::size_t n) {
    errno = 0;
    if (bridge::safe_read_bytes(src, dst, n)) return ok();
    return failed(errno, "process_vm_readv short or failed");
}

Result write_protected(std::uintptr_t dst, const void* src, std::size_t n) {
    errno = 0;
    if (bridge::write_to_readonly(reinterpret_cast<void*>(dst), src, n)) return ok();
    return failed(errno, "pwrite(/proc/self/mem) short or failed");
}

// ── exec interposition ──────────────────────────────────────────────────────
// The four symbols bridge/bridge.version exports and launcher_bypass.cpp
// defines. The adapter test reads bridge.version and checks this list against
// it, so a fifth interposer cannot be added in one place only.

namespace {
const char* const kInterposed[] = {"execve", "execvp", "posix_spawn", "posix_spawnp"};
}  // namespace

Result exec_interposition(ExecInterposition* out) {
    if (!out) return failed(EINVAL, "out is null");
    out->mechanism = "ld_preload_symbol_interposition";
    out->symbols = kInterposed;
    out->symbol_count = sizeof(kInterposed) / sizeof(kInterposed[0]);
    return ok();
}

// ── IPC transport ───────────────────────────────────────────────────────────

Result ipc_endpoint(Endpoint* out) {
    if (!out) return failed(EINVAL, "out is null");
    out->kind = TransportKind::af_unix;
    out->address = bridge::instance_socket_path();
    return ok();
}

namespace {
constexpr const char kIpcOwner[] =
    "linux: the AF_UNIX listener is bridge/src/ipc_server.cpp server_loop()/reader_loop()";
}  // namespace

Result transport_listen(const Endpoint&, Handle* listener) {
    if (listener) *listener = kInvalidHandle;
    return not_routed(kIpcOwner);
}
Result transport_accept(Handle, Handle* conn) {
    if (conn) *conn = kInvalidHandle;
    return not_routed(kIpcOwner);
}
Result transport_read(Handle, void*, std::size_t, std::size_t* got) {
    if (got) *got = 0;
    return not_routed(kIpcOwner);
}
Result transport_write(Handle, const void*, std::size_t, std::size_t* put) {
    if (put) *put = 0;
    return not_routed(kIpcOwner);
}
Result transport_close(Handle) { return not_routed(kIpcOwner); }

// ── paths ───────────────────────────────────────────────────────────────────

Result paths(Paths* out) {
    if (!out) return failed(EINVAL, "out is null");
    out->instance_id     = bridge::instance_id();
    out->socket          = bridge::instance_socket_path();
    out->bridge_log      = bridge::instance_bridge_log();
    out->telemetry_dir   = bridge::instance_telemetry_dir();
    out->log_archive_dir = bridge::instance_log_archive_dir();
    out->state_dir       = bridge::instance_state_dir();
    return ok();
}

// ── input injection ─────────────────────────────────────────────────────────

Result input_click(int, int, MouseButton) {
    return not_routed("linux: XTest (dlopen libXtst.so.6) with /dev/uinput fallback "
                      "is bridge/src/action_handlers.cpp");
}

// ── threads ─────────────────────────────────────────────────────────────────

Result thread_id(std::uint64_t* out) {
    if (!out) return failed(EINVAL, "out is null");
    *out = static_cast<std::uint64_t>(bridge::current_tid());
    return ok();
}

Result set_thread_name(const char* name) {
    if (!name) return failed(EINVAL, "name is null");
    char buf[16];  // the kernel's TASK_COMM_LEN, NUL included
    std::size_t n = strlen(name);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, name, n);
    buf[n] = '\0';
    const int rc = pthread_setname_np(pthread_self(), buf);
    if (rc != 0) return failed(rc, "pthread_setname_np failed");
    return ok();
}

// ── cells whose Linux code is file-static elsewhere (not routed) ────────────
// Each names its owner. None of them runs anything: a not_routed guarded_call
// does NOT run the body, because running it unguarded would be a different
// semantics wearing the same name.

Result entry_state(EntryState* out) {
    if (out) {
        out->phase = EntryPhase::not_attached; out->module = 0;
        out->init_thread_id = 0; out->is_game = false; out->outcome = "";
    }
    return not_routed("linux: dll_init() (__attribute__((constructor))) is "
                      "bridge/src/bridge_init.cpp");
}

Result image_sections(Section*, std::size_t, std::size_t* count) {
    if (count) *count = 0;
    return not_routed("linux: section bounds from program headers are "
                      "bridge/src/runtime_resolver.cpp init_context()");
}

Result guarded_call(GuardedBody, void*, Fault* fault) {
    if (fault) { fault->code = 0; fault->address = 0; fault->pc = 0; }
    return not_routed("linux: sigsetjmp(g_recovery_point) + bridge_sigsegv_handler() are "
                      "bridge/src/bridge_init.cpp; body not run");
}

Result guard_rearm() {
    return not_routed("linux: the SIGSEGV re-arm is bridge/src/bridge_init.cpp");
}

Result alloc_exec_near(std::uintptr_t, std::size_t, ExecBlock* out) {
    if (out) { out->base = 0; out->size = 0; }
    return not_routed("linux: alloc_rwx_near() is bridge/src/hooks.cpp");
}

Result free_exec(ExecBlock*) {
    return not_routed("linux: the trampoline pool is bridge/src/hooks.cpp");
}

Result register_unwind(const ExecBlock&, const UnwindEntry*, std::size_t) {
    return Result{Status::unavailable_on_platform, 0,
                  "linux: no RUNTIME_FUNCTION table; recovery is siglongjmp, which "
                  "does not walk generated frames"};
}

Result unregister_unwind(const ExecBlock&) {
    return Result{Status::unavailable_on_platform, 0,
                  "linux: no RUNTIME_FUNCTION table to remove from"};
}

Result watchdog_arm(std::uint32_t, WatchdogFn, void*) {
    return not_routed("linux: the per-thread SIGALRM dispatch watchdog is "
                      "bridge/src/bridge_init.cpp");
}

Result watchdog_disarm(bool* fired) {
    if (fired) *fired = false;
    return not_routed("linux: the per-thread SIGALRM dispatch watchdog is "
                      "bridge/src/bridge_init.cpp");
}

Result rtti_find_vtable(const char*, std::uintptr_t* vtable) {
    if (vtable) *vtable = 0;
    return not_routed("linux: find_typeinfo()/find_vtable() are "
                      "bridge/src/runtime_resolver.cpp");
}

Result hook_plan(std::uintptr_t, HookPlan* out) {
    if (out) { out->target = 0; out->steal_bytes = 0; out->cfg_checked_site = false;
               out->cet_compatible = false; }
    return not_routed("linux: install_hook() prologue relocation is bridge/src/hooks.cpp");
}

}  // namespace os
}  // namespace bridge
