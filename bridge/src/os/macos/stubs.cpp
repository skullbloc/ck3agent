// macos/stubs.cpp — the macOS implementation of bridge/include/os/os.h, as
// typed refusals. Nothing here is implemented yet, and macOS is not scheduled
// (ADR 0001, "macOS: deferred, not dropped"): every `detail` names the
// mechanism the ADR plans, so the first Mac contributor starts from it.
//
// Every call returns not_implemented and leaves every out-parameter in a
// defined empty state. Nothing returns ok.
//
// Standard C++17 only, and no platform header: the macos-14 CI leg compiles
// this with Apple clang, and the Linux test build compiles it with the host
// compiler, -std=c++17 -pedantic-errors, as test_os_stubs_macos.

#include "os/os.h"

namespace bridge {
namespace os {

namespace {

Result todo(const char* detail) { return Result{Status::not_implemented, 0, detail}; }

constexpr const char kIpc[] =
    "macos: AF_UNIX under ~/Library/Application Support/ck3agent/, 104-byte sun_path "
    "(ADR 0001 'IPC and the Python side'; deferred)";

}  // namespace

Platform platform() { return Platform::kMacos; }

Result process_identity(ProcessIdentity* out) {
    if (out) { out->exe_path[0] = '\0'; out->is_game = false; }
    return todo("macos: _NSGetExecutablePath() basename == ck3, plus a bundle-path "
                "check (ADR 0001; deferred)");
}

Result exe_path_is_game(const char*, bool* is_game) {
    if (is_game) *is_game = false;
    return todo("macos: basename == ck3, plus a bundle-path check (ADR 0001; deferred)");
}

Result image_base(ImageInfo* out) {
    if (out) { out->base = 0; out->end = 0; }
    return todo("macos: _dyld_get_image_header(0) + _dyld_get_image_vmaddr_slide(0), "
                "getsectiondata() (ADR 0001; deferred)");
}

Result binary_identity(BinaryIdentity* out) {
    if (out) {
        out->format = IdentityFormat::none;
        out->observed_hex[0] = '\0';
        out->check = "unchecked";
        out->image_sha256[0] = '\0';
        out->image_size = 0;
    }
    return todo("macos: LC_UUID of the loaded slice, image SHA-256 witness "
                "(ADR 0001 M0/M1; deferred)");
}

Result safe_read(void*, std::uintptr_t, std::size_t) {
    return todo("macos: mach_vm_read_overwrite(mach_task_self()) (ADR 0001; deferred)");
}

Result write_protected(std::uintptr_t, const void*, std::size_t) {
    return todo("macos: mach_vm_protect(VM_PROT_COPY) for __TEXT, mprotect for "
                "__DATA_CONST, sys_icache_invalidate after code writes (ADR 0001; deferred)");
}

Result exec_interposition(ExecInterposition* out) {
    if (out) { out->mechanism = "none"; out->symbols = nullptr; out->symbol_count = 0; }
    return todo("macos: dyld __DATA,__interpose for posix_spawn, or the relauncher "
                "(ADR 0001 macOS plan, M1; deferred)");
}

Result ipc_endpoint(Endpoint* out) {
    if (out) { out->kind = TransportKind::none; out->address = ""; }
    return todo(kIpc);
}

Result transport_listen(const Endpoint&, Handle* listener) {
    if (listener) *listener = kInvalidHandle;
    return todo(kIpc);
}
Result transport_accept(Handle, Handle* conn) {
    if (conn) *conn = kInvalidHandle;
    return todo(kIpc);
}
Result transport_read(Handle, void*, std::size_t, std::size_t* got) {
    if (got) *got = 0;
    return todo(kIpc);
}
Result transport_write(Handle, const void*, std::size_t, std::size_t* put) {
    if (put) *put = 0;
    return todo(kIpc);
}
Result transport_close(Handle) { return todo(kIpc); }

Result paths(Paths* out) {
    if (out) {
        out->instance_id = ""; out->socket = ""; out->bridge_log = "";
        out->telemetry_dir = ""; out->log_archive_dir = ""; out->state_dir = "";
    }
    return todo("macos: ~/Library/Application Support/ck3agent/inst/<id>/, logs in "
                "~/Library/Logs/ck3agent/ (ADR 0001; deferred)");
}

Result input_click(int, int, MouseButton) {
    return todo("macos: CGEventPost, needs Accessibility (TCC) approval (ADR 0001; deferred)");
}

Result thread_id(std::uint64_t* out) {
    if (out) *out = 0;
    return todo("macos: pthread_threadid_np (ADR 0001; deferred)");
}

Result set_thread_name(const char*) {
    return todo("macos: pthread_setname_np, calling thread only (ADR 0001; deferred)");
}

Result entry_state(EntryState* out) {
    if (out) {
        out->phase = EntryPhase::not_attached; out->module = 0;
        out->init_thread_id = 0; out->is_game = false; out->outcome = "";
    }
    return todo("macos: __attribute__((constructor)) in the dylib, as Linux (ADR 0001; deferred)");
}

Result image_sections(Section*, std::size_t, std::size_t* count) {
    if (count) *count = 0;
    return todo("macos: getsectiondata() for __TEXT,__text, __DATA_CONST,__const "
                "(ADR 0001; deferred)");
}

Result guarded_call(GuardedBody, void*, Fault* fault) {
    if (fault) { fault->code = 0; fault->address = 0; fault->pc = 0; }
    return todo("macos: sigsetjmp + SIGSEGV and SIGBUS handlers, EXC_BAD_ACCESS port "
                "forwarding (ADR 0001; deferred); body not run");
}

Result guard_rearm() {
    return todo("macos: re-install the SIGSEGV/SIGBUS handlers (ADR 0001; deferred)");
}

Result alloc_exec_near(std::uintptr_t, std::size_t, ExecBlock* out) {
    if (out) { out->base = 0; out->size = 0; }
    return todo("macos: mmap RW then flip to RX, hardened-runtime entitlement "
                "(ADR 0001; deferred)");
}

Result free_exec(ExecBlock*) { return todo("macos: munmap (ADR 0001; deferred)"); }

Result register_unwind(const ExecBlock&, const UnwindEntry*, std::size_t) {
    return todo("macos: __register_frame for generated code, if needed (ADR 0001; deferred)");
}

Result unregister_unwind(const ExecBlock&) {
    return todo("macos: __deregister_frame (ADR 0001; deferred)");
}

Result watchdog_arm(std::uint32_t, WatchdogFn, void*) {
    return todo("macos: SIGALRM as Linux (ADR 0001; deferred)");
}

Result watchdog_disarm(bool* fired) {
    if (fired) *fired = false;
    return todo("macos: SIGALRM as Linux (ADR 0001; deferred)");
}

Result rtti_find_vtable(const char*, std::uintptr_t* vtable) {
    if (vtable) *vtable = 0;
    return todo("macos: Itanium RTTI walk over the Mach-O sections; needs the macOS "
                "binary (ADR 0001 M2; deferred)");
}

Result hook_plan(std::uintptr_t, HookPlan* out) {
    if (out) { out->target = 0; out->steal_bytes = 0; out->cfg_checked_site = false;
               out->cet_compatible = false; }
    return todo("macos: hook planning needs the macOS binary (ADR 0001 M3; deferred)");
}

}  // namespace os
}  // namespace bridge
