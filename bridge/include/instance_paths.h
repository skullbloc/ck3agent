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
// DEFAULT IS LEGACY. With CK3_INSTANCE_ID unset, empty or "default" every
// answer is the historical literal, byte for byte. A bridge built with this
// header and launched the old way is indistinguishable from the old bridge.
#pragma once

namespace bridge {

// Resolve every path once from the environment. Idempotent, thread-safe after
// the first call, and safe to call before g_logfile exists (it never logs).
// Call it FIRST in bridge_init(), before the log is opened.
void instance_paths_init();

// Never null after instance_paths_init(); each auto-inits on first use.
const char* instance_id();           // "default" or the validated slug
const char* instance_socket_path();  // /tmp/ck3-agent.sock | <dir>/agent.sock
const char* instance_telemetry_dir(); // /tmp/ck3-telemetry | <dir>/telemetry
const char* instance_bridge_log();   // /tmp/ck3_bridge.log | <dir>/bridge.log
// ck-bwz1: where retired log windows are archived. EMPTY when not configured —
// archiving is opt-in, because an unbounded archive in /tmp would trade the
// known loss the ring causes for an unknown one.
const char* instance_log_archive_dir();

// True when the id in the environment was rejected as a non-slug and the
// bridge fell back to the default instance. Surfaced in the init banner so an
// operator sees the typo instead of silently sharing the campaign's socket.
bool instance_id_was_refused();

// mkdir -p every directory the bridge writes into. Called by
// instance_paths_init(); exposed for tests.
void instance_make_dirs();

}  // namespace bridge
