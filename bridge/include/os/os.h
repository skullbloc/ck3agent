#pragma once
// os.h — the bridge's platform layer (docs/adr/0001-windows-macos-port.md,
// "Platform abstraction" and the P2 row of the phased plan in "Review
// amendments").
//
// WHAT THIS IS TODAY: A SKELETON, AND NOTHING CALLS IT YET.
// libck3_bridge.so does not link this layer. bridge_init.cpp,
// runtime_resolver.cpp, ipc_server.cpp, launcher_bypass.cpp and
// action_handlers.cpp still call their own Linux code directly, exactly as
// before. The layer exists so port work has an interface to implement and a
// CI leg (windows-latest, macos-14) that compiles it. Routing the bridge
// through it is the rest of P2, one call site at a time, with the Linux
// acceptance unchanged ("Linux behaviour changes" is P2's kill trigger).
//
// ONE IMPLEMENTATION FILE PER PLATFORM, picked by -DCK3_PLATFORM:
//   bridge/src/os/linux/os_linux.cpp    forwards to the existing Linux code
//   bridge/src/os/windows/stubs.cpp     typed refusals (MSVC, standard C++17)
//   bridge/src/os/macos/stubs.cpp       typed refusals (Apple clang, C++17)
//   bridge/src/os/os_common.cpp         the vocabulary below, all platforms
//
// RULES FOR THIS HEADER. It compiles on all three toolchains: standard C++17,
// no platform header, no compiler extension. Under -std=gnu++17 `linux` is a
// predefined macro, so no identifier here is spelled `linux`.
//
// EVERY CALL RETURNS A Result, never a bare value. A stub that returned 0 for
// an image base would be indistinguishable from a failed lookup, and a
// default is an answer nobody measured. The five states keep "not built yet"
// apart from "built, tried, failed" and from "never exists here".
//
// DECLARED HERE (the ADR table rows the skeleton covers): process identity,
// image base, binary identity, guarded read and write, exec interposition,
// IPC transport, logging paths, input injection, thread id and naming.
// NOT DECLARED YET (ADR rows for a later step): os::entry (DllMain vs
// constructor), os::guarded_region (Rip-rewrite recovery on Windows, see the
// amendments), os::alloc_exec_near (+ RtlAddFunctionTable unwind data),
// os::watchdog, os::early_init.

#include <cstddef>
#include <cstdint>

namespace bridge {
namespace os {

// ── vocabulary ──────────────────────────────────────────────────────────────

enum class Status : int {
    ok = 0,
    // Implemented on this platform, attempted, did not succeed. os_error holds
    // errno (POSIX) or GetLastError() (Windows) when there is one.
    failed,
    // The platform has the mechanism and the bridge already implements it,
    // but not behind this layer yet. `detail` names the file that owns it.
    // Linux only, and only until P2 routes that call site.
    not_routed,
    // Planned for this platform, not written. `detail` names the ADR phase.
    not_implemented,
    // This mechanism does not exist on this platform by design. `detail`
    // names what replaces it (ADR 0001, "Observe-only first": tools refuse,
    // they do not vanish).
    unavailable_on_platform,
};

// "ok" | "failed" | "not_routed" | "not_implemented" | "unavailable_on_platform"
const char* status_string(Status s);

struct Result {
    Status      status;
    int         os_error;  // errno / GetLastError() for `failed`; 0 otherwise
    const char* detail;    // static storage, never null; "" when nothing to add
};

inline bool succeeded(const Result& r) { return r.status == Status::ok; }

enum class Platform : int { kLinux = 1, kWindows = 2, kMacos = 3 };

// The platform this translation unit set was built for (the -DCK3_PLATFORM
// implementation file defines it).
Platform platform();
// "linux" | "windows" | "macos"; "unknown" for any other value.
const char* platform_name(Platform p);

// ── process identity (ADR: os::process_is_game) ────────────────────────────

constexpr std::size_t kMaxPath = 1024;

struct ProcessIdentity {
    char exe_path[kMaxPath];  // NUL-terminated; empty when not observed
    bool is_game;             // exe_path names the game binary (rule below)
};

// The running executable's path and whether it is the game. The bridge is
// loaded into Steam and its helpers too, so everything else is gated on this.
Result process_identity(ProcessIdentity* out);

// The per-platform basename rule alone, on a path the caller supplies:
//   linux    basename after the last '/' is exactly "ck3"
//   windows  basename is "ck3.exe", case-insensitive        (ADR, verify)
//   macos    basename is "ck3" (and a bundle-path check)    (ADR, verify)
Result exe_path_is_game(const char* exe_path, bool* is_game);

// ── image base (ADR: os::image_base) ────────────────────────────────────────

struct ImageInfo {
    std::uintptr_t base;  // load base; every offset in re/offsets/ is relative to it
    std::uintptr_t end;   // one past the last loaded byte; 0 = unknown
};

Result image_base(ImageInfo* out);

// ── binary identity (ADR: os::binary_identity) ──────────────────────────────

enum class IdentityFormat : int {
    none = 0,
    elf_gnu_build_id,   // Linux: NT_GNU_BUILD_ID, 20 bytes
    pe_codeview_rsds,   // Windows: RSDS GUID + age (TimeDateStamp+SizeOfImage fallback)
    macho_lc_uuid,      // macOS: LC_UUID of the loaded slice
};
// "none" | "elf_gnu_build_id" | "pe_codeview_rsds" | "macho_lc_uuid"
const char* identity_format_string(IdentityFormat f);

struct BinaryIdentity {
    IdentityFormat format;
    char           observed_hex[65];  // lowercase hex; empty when nothing observed
    // The witness vocabulary, shared with the IPC handshake_ack:
    // "match" | "mismatch" | "not_found" | "unchecked". Never null.
    const char*    check;
};

// What the bridge's own identity check recorded. It reports the witness; it
// does not run a second check (one check, several readers).
Result binary_identity(BinaryIdentity* out);

// ── guarded memory (ADR: os::safe_read, os::write_protected) ────────────────

// Copy n bytes from src in this process. Fails instead of faulting.
Result safe_read(void* dst, std::uintptr_t src, std::size_t n);
// Write n bytes to dst in this process regardless of page protection
// (RELRO vtables, code). Code writes also need an i-cache flush on Windows
// and macOS; that is the implementation's job, not the caller's.
Result write_protected(std::uintptr_t dst, const void* src, std::size_t n);

// ── exec interposition (ADR: launcher bypass) ───────────────────────────────

struct ExecInterposition {
    const char*        mechanism;     // e.g. "ld_preload_symbol_interposition"
    const char* const* symbols;       // the entry points interposed; static storage
    std::size_t        symbol_count;
};

// How the bridge follows the launcher into the game process. Interposition is
// link-time (exported symbols), so there is no install call: this reports.
Result exec_interposition(ExecInterposition* out);

// ── IPC transport (ADR: os::transport) ──────────────────────────────────────

enum class TransportKind : int { none = 0, af_unix, tcp_loopback };
// "none" | "af_unix" | "tcp_loopback"
const char* transport_kind_string(TransportKind k);

struct Endpoint {
    TransportKind kind;
    const char*   address;  // socket path or "127.0.0.1:<port>"; never null
};

// Where this instance listens (CK3_INSTANCE_ID / CK3_INSTANCE_LAYOUT rules).
Result ipc_endpoint(Endpoint* out);

using Handle = std::intptr_t;
constexpr Handle kInvalidHandle = -1;

Result transport_listen(const Endpoint& at, Handle* listener);
Result transport_accept(Handle listener, Handle* conn);
Result transport_read(Handle conn, void* buf, std::size_t cap, std::size_t* got);
Result transport_write(Handle conn, const void* buf, std::size_t len, std::size_t* put);
Result transport_close(Handle h);

// ── logging and state paths (ADR: os::paths) ────────────────────────────────

struct Paths {
    const char* instance_id;      // "default" or the validated slug
    const char* socket;
    const char* bridge_log;
    const char* telemetry_dir;
    const char* log_archive_dir;  // "" when archiving is not configured
    const char* state_dir;
};

// Each pointer is never null on `ok` and valid for the process lifetime.
Result paths(Paths* out);

// ── input injection (ADR: os::input) ────────────────────────────────────────

enum class MouseButton : int { left = 1, middle = 2, right = 3 };

// One synthetic click at window coordinates (x, y).
Result input_click(int x, int y, MouseButton button);

// ── threads (ADR: os::thread_id, thread naming) ─────────────────────────────

// The kernel thread id (Linux gettid, Windows GetCurrentThreadId, macOS
// pthread_threadid_np), the value the crash instrument reports.
Result thread_id(std::uint64_t* out);
// Name the CALLING thread. Truncated to the platform limit (15 bytes on
// Linux); macOS can name only the calling thread, so the API is self-only.
Result set_thread_name(const char* name);

}  // namespace os
}  // namespace bridge
