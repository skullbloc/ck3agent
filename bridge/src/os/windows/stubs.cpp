// windows/stubs.cpp — the Windows implementation of bridge/include/os/os.h,
// as typed refusals. Nothing here is implemented yet.
//
// Every call returns not_implemented (the ADR 0001 phase that writes it is in
// `detail`) or unavailable_on_platform (the mechanism does not exist on
// Windows; `detail` names its replacement), and leaves every out-parameter in
// a defined empty state. Nothing returns ok: a stub that answered would be a
// default dressed up as a measurement.
//
// Standard C++17 only, and no platform header (not even <windows.h> yet): the
// windows-latest CI leg compiles this with MSVC, and the Linux test build
// compiles it with the host compiler, -std=c++17 -pedantic-errors, as
// test_os_stubs_windows.

#include "os/os.h"

namespace bridge {
namespace os {

namespace {

Result todo(const char* detail) { return Result{Status::not_implemented, 0, detail}; }
Result never(const char* detail) { return Result{Status::unavailable_on_platform, 0, detail}; }

constexpr const char kIpc[] =
    "windows: TCP loopback listener (ADR 0001 'IPC and the Python side'; phase P4)";

}  // namespace

Platform platform() { return Platform::kWindows; }

Result process_identity(ProcessIdentity* out) {
    if (out) { out->exe_path[0] = '\0'; out->is_game = false; }
    return todo("windows: GetModuleFileNameW(NULL) basename == ck3.exe, "
                "case-insensitive (ADR 0001 phase P4)");
}

Result exe_path_is_game(const char*, bool* is_game) {
    if (is_game) *is_game = false;
    return todo("windows: basename == ck3.exe, case-insensitive (ADR 0001 phase P4)");
}

Result image_base(ImageInfo* out) {
    if (out) { out->base = 0; out->end = 0; }
    return todo("windows: GetModuleHandleW(NULL) + IMAGE_NT_HEADERS section walk; "
                "offsets are RVAs (ADR 0001 phase P4)");
}

Result binary_identity(BinaryIdentity* out) {
    if (out) {
        out->format = IdentityFormat::none;
        out->observed_hex[0] = '\0';
        out->check = "unchecked";
    }
    return todo("windows: PE CodeView RSDS (GUID + age), TimeDateStamp + SizeOfImage "
                "fallback, image SHA-256 witness (ADR 0001 phase P4)");
}

Result safe_read(void*, std::uintptr_t, std::size_t) {
    return todo("windows: ReadProcessMemory(GetCurrentProcess()) (ADR 0001 phase P5)");
}

Result write_protected(std::uintptr_t, const void*, std::size_t) {
    return todo("windows: WriteProcessMemory or VirtualProtect/write/restore, "
                "FlushInstructionCache after code writes (ADR 0001 phase P6)");
}

Result exec_interposition(ExecInterposition* out) {
    if (out) { out->mechanism = "none"; out->symbols = nullptr; out->symbol_count = 0; }
    return never("windows: no exec interposition; a proxy DLL beside ck3.exe is loaded "
                 "by the game process itself (ADR 0001 Windows plan, option a)");
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
    return todo("windows: %LOCALAPPDATA%\\ck3agent\\inst\\<id>\\ (ADR 0001 phase P4)");
}

Result input_click(int, int, MouseButton) {
    return todo("windows: SendInput (ADR 0001 phase P6)");
}

Result thread_id(std::uint64_t* out) {
    if (out) *out = 0;
    return todo("windows: GetCurrentThreadId (ADR 0001 phase P4)");
}

Result set_thread_name(const char*) {
    return todo("windows: SetThreadDescription (ADR 0001 phase P4)");
}

}  // namespace os
}  // namespace bridge
