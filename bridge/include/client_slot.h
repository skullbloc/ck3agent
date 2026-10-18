#pragma once
// client_slot.h — the IPC client-slot table's LIFECYCLE, extracted from
// ipc_server.cpp (ck-xqol) so it can be exercised by a host-compiled unit test
// with no engine, no game, no socket and no reader thread.
//
// WHY THIS FILE EXISTS — the ck-xqol SIGSEGV, read out of the core:
//
//   #0 bridge::release_slot_locked(int, char const*)   (+0x20b3e)
//   #1 bridge::reader_loop()
//
//   The faulting instruction was `movb $0x0,(%rax)` where %rax had just been
//   loaded from slot+0x50 — the `_M_p` of the slot's std::string — and was
//   NULL.  A std::string whose _M_p is null has never been constructed: an
//   empty one points at its own local buffer.  The core also shows `gen == 0`
//   in the slot at dump time even though the faulting frame had already
//   executed `s.gen++`, which means the slot's CONSTRUCTORS RAN AFTER THE
//   CRASH.  The reader thread reached a slot before the slot existed.
//
//   That is possible because the bridge's ELF constructor (`dll_init`, the
//   `__attribute__((constructor))` in bridge_init.cpp) is entry [1] of
//   .init_array — ahead of EVERY C++ dynamic initializer in the library,
//   including `_GLOBAL__sub_I_ipc_server.cpp`, which is entry [3].  dll_init
//   calls bridge_init() -> start_ipc_server() -> std::thread(reader_loop).  If
//   that thread reaches its first poll() before entry [3] runs, it sees the
//   slot table as raw .bss: every field zero.
//
//   `fd == 0` is not `< 0`, so the old `if (s.fd < 0) return;` liveness test
//   admitted the never-constructed slot; fd 0 is stdin, so the reader put
//   stdin in its poll set, read EOF, logged the real line from the crashed
//   run — "Releasing client slot=0 fd=0 peer_pid=0 (reader EOF / error)" —
//   close()d stdin, and then cleared a std::string that did not exist yet.
//
// THE FIX, and the invariant this header exists to hold:
//
//   `in_use` is the sole authority on whether a slot is live, and it is a bool
//   chosen so that RAW ZERO STORAGE READS AS "NOT LIVE".  A sentinel of -1 is
//   only true once a constructor has run; `false` is true of zeroed memory and
//   of a constructed-but-free slot alike.  Every membership test goes through
//   slot_is_open().  The ordering hazard is separately closed by giving the
//   table an init_priority (see ipc_server.cpp), but this predicate is what
//   makes the lifecycle correct rather than merely early enough.

#include <cstdint>
#include <mutex>
#include <string>

namespace bridge {

struct Session;  // session.h

// One IPC client connection.
//
// CONCURRENCY INVARIANT (unchanged from ipc_server.cpp): a slot's `fd`, `gen`
// and `in_use` are written ONLY while BOTH the membership mutex
// (ipc_server.cpp's g_client_mutex) AND that slot's send_mtx are held; they may
// be read under EITHER lock.  send_to_slot() validates `gen` under send_mtx so
// a released-and-reused slot can never receive a stale write.
struct ClientSlot {
    // ck-xqol: FIRST member, and deliberately a bool whose zero value means
    // "free".  See the header comment — the crash was a liveness test that a
    // zero-initialised slot passed.
    bool in_use = false;
    int fd = -1;
    int peer_pid = -1, peer_uid = -1, peer_gid = -1;
    uint64_t connect_ts = 0;
    uint32_t gen = 0;            // bumped on every open AND every release
    bool handshaked = false;
    std::mutex send_mtx;         // serializes writes to this slot's fd
    // M1 multi-agent session bound to this slot.  Released when the slot is
    // released (disconnect / stale displacement).
    Session* session = nullptr;
    // ck-6kvq.5: the token is SERVER-minted (bridge::Session::resume_token) and
    // this copy is only the slot's cached view of it for logging; the authority
    // is the registry.
    std::string resume_token;
    // ck-6kvq.5: the session's id, COPIED BY VALUE.  `session` above is a raw
    // pointer into the registry, and since the resume path a session can be
    // ERASED out from under a live slot (a declared save identity that refutes
    // the world its claims were taken in).  Matching a slot by id is then the
    // only way to find it WITHOUT dereferencing a pointer that may already be
    // freed — the same lesson ck-xqol drew from `session_id` living inside the
    // object destroy() erases, applied one step earlier.
    std::string session_id;
    // NB: NDJSON line accumulation + keepalive counters are reader-thread-local
    // (see reader_loop) so no other thread ever touches a std::string here.
};

static constexpr int MAX_CLIENTS = 32;


// The ONLY membership predicate.  Safe to call on storage that has never been
// constructed: it reads one bool and one int and dereferences nothing.
inline bool slot_is_open(const ClientSlot& s) {
    return s.in_use && s.fd >= 0;
}
// ── ck-n7n0: WHICH SLOT BELONGS TO THIS SESSION? ───────────────────────────
//
// Extracted so the acceptance can DRIVE it: send_to_session_id touches the
// g_clients global and cannot be linked without the engine, but the matching
// RULE is pure and is where the mistakes live.
//
// MATCHES BY ID, NEVER BY POINTER. A session can be erased out from under a
// live slot (ck-6kvq.5), so the by-value session_id copy is the only key that
// does not dereference storage that may already be freed — the same lesson
// ck-xqol drew about session_id living inside the object destroy() erases.
//
// Returns -1 for "no live slot holds this session", which the caller MUST treat
// as "fall back to broadcast" rather than "drop the reply": an unsolicited
// engine event has no session at all, and a client that disconnected mid-flight
// has no slot. Narrowing delivery must never become silencing it.
inline int find_slot_for_session(const ClientSlot* slots, int n,
                                 const char* session_id) {
    if (!slots || !session_id || !session_id[0]) return -1;
    for (int i = 0; i < n; i++) {
        if (slot_is_open(slots[i]) && slots[i].session_id == session_id)
            return i;
    }
    return -1;
}

// What a release actually did.  `released == false` means the slot was not
// live and NOTHING was touched — not the fd, not the mutex, not the string.
struct SlotReleaseResult {
    bool      released = false;
    int       fd       = -1;     // the fd that was closed (for logging)
    int       peer_pid = -1;     // the peer that held it (for logging)
    uint32_t  bound_gen = 0;     // generation the session was BOUND at, pre-bump
    Session*  session  = nullptr; // detached session; the CALLER tears it down
};

// Injected so the unit test can close nothing.  Defaults to ::close.
using SlotCloseFn = int (*)(int);

// Close and reset a slot.  Caller must hold the membership mutex.
//
// IDEMPOTENT by construction: a slot that is not open (never constructed,
// never accepted, or already released) returns {released=false} without
// touching a single member.  That is the double-release guarantee and the
// zero-storage guarantee in one predicate.
//
// The session is RETURNED, not destroyed: session teardown needs the registry
// and the pipeline, which do not belong in this unit.  `bound_gen` is the
// generation the caller must present to the registry so a late reap cannot
// tear down a session that has already rebound to a newer connection.
SlotReleaseResult client_slot_release(ClientSlot& s, SlotCloseFn closer);

// Adopt an accepted fd into a free slot.  Caller must hold the membership
// mutex.  Bumps `gen`, so any send snapshot taken against the previous
// occupant is invalidated.  Returns the new generation.
uint32_t client_slot_open(ClientSlot& s, int fd, uint64_t connect_ts);

// Bind a session to a slot, but ONLY if the slot still holds the exact
// connection the caller handshaked with.
//
// ck-xqol (release-racing-handshake): a handshake is parsed WITHOUT the
// membership mutex and only then takes it to attach.  If the socket dropped in
// that window the slot has been released and possibly re-accepted, and an
// unconditional `s.session = session` either orphans the session on a free
// slot (nothing releases a slot that is not open, so its claims are held
// forever) or mis-attributes it to whoever now owns the slot.  Presenting the
// generation the handshake was read at makes both impossible.
//
// Returns false if the binding is stale; the caller must then destroy the
// session it minted.
bool client_slot_attach_session(ClientSlot& s, uint32_t expected_gen,
                                Session* session, const char* resume_token);

}  // namespace bridge
