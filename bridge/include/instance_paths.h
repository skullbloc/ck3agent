// instance_paths.h — the bridge's own copy of the instance naming rules.
//
// Bead ck-qn03 (R12). The authority is `ck3agent/server/instance.py`; this is
// the third transcription (the second is `scripts/ck3_instance.sh`), and all
// three are pinned against each other by `scripts/test_multi_instance.py`.
//
// WHY THE BRIDGE NEEDS ITS OWN COPY. The bridge is LD_PRELOADed into a process
// no python wrapper is inside. It must decide, on its own, which socket to
// bind, which telemetry directory to append to and which log to open — before
// any client exists to tell it. The ONLY channel is the process environment,
// so the rules are re-stated here rather than passed in.
//
// TWO LAYOUTS (rc2 B1). CK3_INSTANCE_LAYOUT selects:
//   unset / "user"  per-user private dir: $XDG_RUNTIME_DIR/ck3agent, else
//                   /run/user/<euid>/ck3agent, else ~/.local/state/ck3agent.
//                   Created 0700 and verified (dir, owned by euid, no g/o
//                   write). Socket 0600; peers with another uid are refused.
//   "legacy"        the historical /tmp literals, byte for byte: socket
//                   /tmp/ck3-agent.sock at 0666, any peer uid (two-user lab).
// Any other value, or a CK3_INSTANCE_ID that is not a slug, is a CONFIG
// REFUSAL: paths resolve as "user"/"default" for the banner, but nothing is
// bound or opened (instance_private_dir_ok() is false). instance.py raises on
// the same inputs, so neither side settles on a path the other did not.
//
// The user layout verifies the private dir AND every ancestor (owned by root
// or euid; a group/other-writable ancestor must be sticky), then holds an fd
// on the state dir. Flag/lock files go through instance_state_fopen (openat,
// O_NOFOLLOW, regular files only), never by path.
#pragma once

#include <cstdio>
#include <sys/types.h>

namespace bridge {

// Resolve every path once from the environment. Idempotent, thread-safe after
// the first call, and safe to call before g_logfile exists (it never logs).
// Call it FIRST in bridge_init(), before the log is opened.
void instance_paths_init();

// Never null after instance_paths_init(); each auto-inits on first use.
const char* instance_id();           // "default" or the validated slug
const char* instance_socket_path();  // <base>/agent.sock | <dir>/agent.sock
const char* instance_telemetry_dir(); // <base>/telemetry | <dir>/telemetry
const char* instance_bridge_log();   // <base>/bridge.log | <dir>/bridge.log
// ck-bwz1: where retired log windows are archived. EMPTY when not configured —
// archiving is opt-in, because an unbounded archive in /tmp would trade the
// known loss the ring causes for an unknown one.
const char* instance_log_archive_dir();

// "user" or "legacy".
const char* instance_layout();
bool instance_layout_is_legacy();
// True when CK3_INSTANCE_LAYOUT held an unknown value (fell back to "user").
bool instance_layout_was_refused();

// Directory for the bridge's flag and lock files (ck-enabled-writes,
// ck3-decision-*, ck3-personality-locks.json). Legacy: "/tmp". User: the
// instance's private dir.
const char* instance_state_dir();

// True when the id in the environment was rejected as a non-slug and the
// bridge fell back to the default instance. Surfaced in the init banner so an
// operator sees the typo instead of silently sharing the campaign's socket.
bool instance_id_was_refused();

// mkdir -p every directory the bridge writes into, then (user layout) verify
// the private dir. Called by instance_paths_init(); exposed for tests.
void instance_make_dirs();

// False when the private dir failed verification (user layout) or the layout
// or id was refused (any layout). The socket is then not bound and no log is
// opened.
bool instance_private_dir_ok();
// True when CK3_INSTANCE_LAYOUT or CK3_INSTANCE_ID was malformed.
bool instance_config_was_refused();

// Open `name` (a single path component) in the state dir through the fd held
// since verification: O_NOFOLLOW|O_CLOEXEC, regular files only, created 0600
// (0644 legacy). mode is "r", "w" (truncate) or "a". nullptr (errno set) when
// the dir was refused or never verified.
FILE* instance_state_fopen(const char* name, const char* mode);
// rc2 B1: the same open by PATH, for bridge-written files outside the state
// dir fd (telemetry, board carrier, session detach stamp): O_NOFOLLOW|
// O_CLOEXEC, regular files only, created instance_file_mode(); a write open
// tightens an existing file to that mode. nullptr (errno set) on failure.
FILE* instance_fopen_private(const char* path, const char* mode);
// 0600 (user layout) or 0644 (legacy): the mode of every file the bridge creates.
mode_t instance_file_mode();
// Same, read-only, as a raw fd (for mmap). -1 on failure.
int instance_state_open_fd(const char* name);
// unlinkat on the state dir fd. -1 (errno set) on failure.
int instance_state_unlink(const char* name);
// Why verification failed; "" when ok.
const char* instance_private_dir_error();

// Peer admission for an accepted connection. Legacy admits any uid (two-user
// lab). User layout admits only `self`, and refuses when the credentials
// could not be read.
bool instance_peer_admitted(bool have_cred, uid_t peer_uid, uid_t self_uid,
                            bool legacy);

// Open a bridge-owned log: O_NOFOLLOW|O_CLOEXEC, mode 0600 (0644 legacy).
// truncate=true unlinks first and creates with O_EXCL; false appends.
// Returns nullptr on failure (errno set).
FILE* instance_open_private_log(const char* path, bool truncate);

// mkdir mode for directories the bridge creates: 0700 user, 0777 legacy.
mode_t instance_dir_mode();

// fd for appending to the boot log, or -1. Only with CK3_BRIDGE_BOOT_LOG=1, in
// every build. Legacy layout: /tmp/ck3_bridge_boot.log. Otherwise
// <private base>/bridge_boot.log (same base order as the resolver) when that
// dir already exists, is ours and is 0700. Raw syscalls and getenv only: safe from
// dll_init and the execve interposers. Caller closes.
int instance_open_boot_log();
// The <private base> instance_open_boot_log uses, written to `out`. Raw, no
// allocation. False when there is none (no XDG_RUNTIME_DIR, /run/user/<euid>
// or HOME). Matches the resolver's base except when only passwd knows HOME.
bool instance_boot_log_base(char* out, size_t n);

}  // namespace bridge
