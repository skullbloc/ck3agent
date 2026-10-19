#pragma once
// os.h — the bridge's platform layer (docs/adr/0001-windows-macos-port.md,
// "Platform abstraction" and the P2 row of the phased plan in "Review
// amendments").
//
// WHAT THIS IS TODAY: NOTHING IN THE LINUX BRIDGE CALLS IT YET.
// libck3_bridge.so does not link this layer. bridge_init.cpp,
// runtime_resolver.cpp, ipc_server.cpp, launcher_bypass.cpp and
// action_handlers.cpp still call their own Linux code directly, exactly as
// before. Routing the bridge through it is the rest of P2, one call site at a
// time, with the Linux acceptance unchanged ("Linux behaviour changes" is P2's
// kill trigger). On Windows the layer is the only bridge code there is: it is
// built into the static library ck3_os and the inert DLL ck3_bridge_win (see
// bridge/CMakeLists.txt).
//
// ONE IMPLEMENTATION SET PER PLATFORM, picked by -DCK3_PLATFORM:
//   bridge/src/os/linux/os_linux.cpp   forwards to the existing Linux code
//   bridge/src/os/windows/*.cpp        the Win32 cells (ADR 0001 P2); the cells
//                                      that need ck3.exe are typed refusals in
//                                      windows/stubs.cpp
//   bridge/src/os/macos/stubs.cpp      typed refusals (Apple clang, C++17)
//   bridge/src/os/os_common.cpp        the vocabulary below, all platforms
//   bridge/src/os/{sha256,pe_image}.cpp, windows/rules.cpp
//                                      portable C++17 helpers the Windows cells
//                                      use; their tests run on all three OSes
// The cell -> file -> test table is in docs/adr/0001-windows-macos-port.md,
// "Windows plan", "P2 cell status".
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
// DECLARED HERE: every row of the ADR's "Platform abstraction" table
// (os::entry, process identity, image base + section table, binary identity,
// guarded read and write, guarded_region, alloc_exec_near + unwind
// registration, exec interposition, IPC transport, paths, input, thread id and
// naming, watchdog; os::early_init is a macro, in os/early_init.h), plus the
// two ABI-bound pieces that need the game binary (RTTI vtable lookup, hook
// planning), which are typed refusals everywhere but Linux.

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

// ── library entry (ADR: os::entry) ──────────────────────────────────────────

enum class EntryPhase : int {
    not_attached = 0,  // no library entry ran in this image (an exe, a test)
    attached,          // the loader called the entry; the init thread was requested
    init_running,      // the init thread is running
    init_done,         // the init thread returned
};
// "not_attached" | "attached" | "init_running" | "init_done"
const char* entry_phase_string(EntryPhase p);

struct EntryState {
    EntryPhase     phase;
    std::uintptr_t module;          // this library's module handle / base; 0 if none
    std::uint64_t  init_thread_id;  // 0 until the init thread runs
    bool           is_game;         // what the init thread's process_identity said
    const char*    outcome;         // static, never null: what the init thread did
};
// What the library entry recorded. The entry itself is not callable: it is
// DllMain on Windows and a constructor on Linux/macOS.
Result entry_state(EntryState* out);

// ── process identity (ADR: os::process_is_game) ────────────────────────────

constexpr std::size_t kMaxPath = 1024;

struct ProcessIdentity {
    char exe_path[kMaxPath];  // NUL-terminated (UTF-8 on Windows); empty when not observed
    bool is_game;             // exe_path names the game binary (rule below)
};

// The running executable's path and whether it is the game. The bridge is
// loaded into Steam and its helpers too, so everything else is gated on this.
Result process_identity(ProcessIdentity* out);

// The per-platform basename rule alone, on a path the caller supplies:
//   linux    basename after the last '/' is exactly "ck3"
//   windows  basename after the last '\' or '/' is "ck3.exe", ASCII
//            case-insensitive                                  (ADR, verify)
//   macos    basename is "ck3" (and a bundle-path check)       (ADR, verify)
Result exe_path_is_game(const char* exe_path, bool* is_game);

// ── image base (ADR: os::image_base) ────────────────────────────────────────

struct ImageInfo {
    std::uintptr_t base;  // load base; every offset in re/offsets/ is relative to it
    std::uintptr_t end;   // one past the last loaded byte; 0 = unknown
};

Result image_base(ImageInfo* out);

// The loaded image's section table (PE IMAGE_SECTION_HEADERs). start/end are
// absolute addresses in this process.
enum SectionFlags : std::uint32_t {
    kSectionRead = 1u,
    kSectionWrite = 2u,
    kSectionExec = 4u,
};
struct Section {
    char           name[9];  // NUL-terminated; PE section names are at most 8 bytes
    std::uintptr_t start;
    std::uintptr_t end;      // one past the last byte (VirtualSize, not raw size)
    std::uint32_t  flags;    // SectionFlags bits
};
// Writes up to `cap` sections into out; *count is the number the image HAS.
// When that exceeds cap the result is `failed` and out holds the first cap.
Result image_sections(Section* out, std::size_t cap, std::size_t* count);

// ── binary identity (ADR: os::binary_identity) ──────────────────────────────

enum class IdentityFormat : int {
    none = 0,
    elf_gnu_build_id,   // Linux: NT_GNU_BUILD_ID, 20 bytes
    pe_codeview_rsds,   // Windows: RSDS GUID + age
    macho_lc_uuid,      // macOS: LC_UUID of the loaded slice
    image_sha256,       // any platform: the image carries no format identity,
                        // so the file hash is the key (observed_hex == image_sha256)
};
// "none" | "elf_gnu_build_id" | "pe_codeview_rsds" | "macho_lc_uuid" | "image_sha256"
//
// The pe_codeview_rsds spelling of observed_hex (40 hex digits): the PDB GUID
// as the canonical UUID string prints it, dashes removed, lowercase, then the
// age as 8 hex digits. In Python: uuid.UUID(bytes_le=rsds[4:20]).hex +
// f"{age:08x}" (tools/inspect_binary.py prints the same GUID upper-case with
// dashes, and the age in decimal).
const char* identity_format_string(IdentityFormat f);

struct BinaryIdentity {
    IdentityFormat format;
    char           observed_hex[65];  // lowercase hex; empty when nothing observed
    // The witness vocabulary, shared with the IPC handshake_ack:
    // "match" | "mismatch" | "not_found" | "unchecked". Never null.
    const char*    check;
    // The second identity witness (ADR amendments, "A second identity witness,
    // everywhere"): SHA-256 of the image FILE on disk, lowercase hex. Empty
    // when this platform's cell does not compute it (Linux today: the bridge
    // has no file-hash witness yet, and the adapter adds nothing).
    char           image_sha256[65];
    // PE SizeOfImage (the span the RVA space covers); 0 when the format has
    // no such field.
    std::uint64_t  image_size;
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

// ── guarded region (ADR: os::guarded_region, and the amendment "No longjmp
//    out of a vectored handler") ─────────────────────────────────────────────

// Run body(ctx). If it faults on a memory access, recovery resumes as if body
// had returned, and the call reports `failed` with the fault in *fault.
// THE RULE IS THE LINUX ONE: a protected body owns no resources. Nothing it
// constructed is destroyed and nothing it locked is unlocked when a fault is
// recovered, because its frames are abandoned, not unwound by C++.
struct Fault {
    std::uint32_t  code;     // Windows exception code (0xC0000005: access
                             // violation); the signal number on POSIX
    std::uintptr_t address;  // the address the faulting access touched; 0 if unknown
    std::uintptr_t pc;       // the faulting instruction
};
using GuardedBody = void (*)(void* ctx);
// ok: body returned. failed: a fault was recovered (os_error = fault code).
// fault may be null. Regions nest up to a small fixed depth per thread; past
// it the call is `failed` with a detail that says so, and body is not run.
Result guarded_call(GuardedBody body, void* ctx, Fault* fault);
// Put the bridge's fault handler first again: the engine's crash reporter may
// install its own in front of it later (the same race as Linux's SIGSEGV re-arm).
Result guard_rearm();

// ── executable memory (ADR: os::alloc_exec_near, and the amendment
//    "Trampolines need unwind data") ─────────────────────────────────────────

struct ExecBlock {
    std::uintptr_t base;  // 0 when nothing was allocated
    std::size_t    size;
};
// Read-write-execute memory within +-2 GB of target (rel32 reach), so a 5-byte
// jmp at target can reach it. size is rounded up to the page size.
Result alloc_exec_near(std::uintptr_t target, std::size_t size, ExecBlock* out);
// Releases the block (and any unwind registration on it); zeroes *block on ok.
Result free_exec(ExecBlock* block);

// One RUNTIME_FUNCTION inside a block. Offsets are relative to block.base;
// unwind_info is the offset of UNWIND_INFO bytes the caller already wrote
// inside the block. Windows needs this (RtlAddFunctionTable): an exception or
// stack walk that crosses unregistered generated code fail-fasts there.
struct UnwindEntry {
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t unwind_info;
};
Result register_unwind(const ExecBlock& block, const UnwindEntry* entries, std::size_t n);
Result unregister_unwind(const ExecBlock& block);

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

// One synthetic click at window coordinates (x, y) of the game's window.
Result input_click(int x, int y, MouseButton button);

// ── threads (ADR: os::thread_id, thread naming) ─────────────────────────────

// The kernel thread id (Linux gettid, Windows GetCurrentThreadId, macOS
// pthread_threadid_np), the value the crash instrument reports.
Result thread_id(std::uint64_t* out);
// Name the CALLING thread. Truncated to the platform limit (15 bytes on
// Linux); macOS can name only the calling thread, so the API is self-only.
Result set_thread_name(const char* name);

// ── watchdog (ADR: os::watchdog) ────────────────────────────────────────────

// One process-wide watchdog. After `ms` milliseconds without a disarm, fn(ctx)
// runs ONCE on the watchdog's own thread. Windows has no SIGALRM, so it cannot
// interrupt the watched thread: fn sets the flag the watched code polls.
// Arming while armed replaces the deadline and the callback.
using WatchdogFn = void (*)(void* ctx);
Result watchdog_arm(std::uint32_t ms, WatchdogFn fn, void* ctx);
// *fired (may be null) says whether the callback ran since the last arm. After
// disarm returns, the callback is not running and will not run.
Result watchdog_disarm(bool* fired);

// ── needs the game binary (ADR: RTTI walk; per-hook CFG/CET planning) ──────

// The vtable for a class by its RTTI name (Itanium: "8CAIManager"; MSVC:
// ".?AVCAIManager@@").
Result rtti_find_vtable(const char* type_name, std::uintptr_t* vtable);

struct HookPlan {
    std::uintptr_t target;
    std::size_t    steal_bytes;       // prologue bytes relocated into the trampoline
    bool           cfg_checked_site;  // the call site is CFG-guarded
    bool           cet_compatible;    // the trampoline's call/ret pairing suits a shadow stack
};
// Plan an inline hook at target: existence, uniqueness (no identical-COMDAT
// alias), prologue relocation, CFG/CET (ADR amendment "CFG, CET and MSVC
// inlining are per-hook questions").
Result hook_plan(std::uintptr_t target, HookPlan* out);

}  // namespace os
}  // namespace bridge
