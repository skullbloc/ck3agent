// client_slot.cpp — IPC client-slot lifecycle (ck-xqol).  See client_slot.h
// for the crash this exists to make impossible.
//
// This TU is deliberately engine-free: <unistd.h> and the standard library,
// nothing else.  That is what lets test_client_slot.cpp compile it on the host.

#include "client_slot.h"

#include <unistd.h>

namespace bridge {

SlotReleaseResult client_slot_release(ClientSlot& s, SlotCloseFn closer) {
    SlotReleaseResult r;

#ifdef CK3_SLOT_LEGACY_FD_GUARD
    // ─── ck-xqol NEGATIVE CONTROL ────────────────────────────────────────
    // The pre-fix code, reproduced exactly: the liveness test is the -1
    // sentinel on `fd`, and it is made INSIDE the send_mtx scope, i.e. after
    // the slot has already been touched.  Build with
    // -DCK3_SLOT_LEGACY_FD_GUARD to restore the bug and watch the regression
    // test SIGSEGV in the same statement the production core faulted in.
    // Never defined by the library build.
    const uint32_t bound_gen = s.gen;
    {
        std::lock_guard<std::mutex> sl(s.send_mtx);
        if (s.fd < 0) return r;          // fd == 0 (raw .bss) is NOT < 0
        r.released  = true;
        r.fd        = s.fd;
        r.peer_pid  = s.peer_pid;
        r.bound_gen = bound_gen;
        closer(s.fd);
        s.fd = -1;
        s.gen++;
    }
#else
    // ─── THE FIX ─────────────────────────────────────────────────────────
    // The liveness test happens BEFORE anything in `s` is locked, closed or
    // dereferenced, and it is a predicate that raw zero storage fails.  A slot
    // that is not open is left bit-for-bit untouched, which is simultaneously:
    //   * the never-constructed case (the ck-xqol crash),
    //   * the double-release case (second caller is a no-op),
    //   * the release-after-displacement case.
    if (!slot_is_open(s)) return r;

    const uint32_t bound_gen = s.gen;
    {
        std::lock_guard<std::mutex> sl(s.send_mtx);
        // Re-test under send_mtx: `fd`/`gen`/`in_use` are written under BOTH
        // locks, so this is the point at which the decision becomes final.
        if (!slot_is_open(s)) return r;
        r.released  = true;
        r.fd        = s.fd;
        r.peer_pid  = s.peer_pid;
        r.bound_gen = bound_gen;
        closer(s.fd);
        s.fd      = -1;
        s.in_use  = false;
        s.gen++;
    }
#endif

    s.peer_pid   = -1;
    s.peer_uid   = -1;
    s.peer_gid   = -1;
    s.connect_ts = 0;
    s.handshaked = false;
    s.resume_token.clear();
    s.session_id.clear();

    // Hand the session out rather than destroying it here; see client_slot.h.
    r.session = s.session;
    s.session = nullptr;
    return r;
}

uint32_t client_slot_open(ClientSlot& s, int fd, uint64_t connect_ts) {
    uint32_t gen;
    {
        std::lock_guard<std::mutex> sl(s.send_mtx);
        s.fd     = fd;
        s.in_use = true;
        s.gen++;
        gen = s.gen;
    }
    s.connect_ts = connect_ts;
    s.handshaked = false;
#ifdef _WIN32
    s.authed = false;   // the first line must carry the endpoint token
#else
    s.authed = true;    // admitted by uid at accept
#endif
    return gen;
}

bool client_slot_attach_session(ClientSlot& s, uint32_t expected_gen,
                                Session* session, const char* resume_token) {
#ifndef CK3_SLOT_LEGACY_FD_GUARD
    // ck-xqol: refuse to bind to a connection that is already gone or already
    // replaced.  Without this the session is orphaned on a free slot (and its
    // claims are never released) or handed to the next client to land here.
    if (!slot_is_open(s) || s.gen != expected_gen) return false;
#else
    (void)expected_gen;  // negative control: the pre-fix unconditional bind
#endif
    s.session      = session;
    s.resume_token = resume_token ? resume_token : "";
    // `session_id` is stamped by the CALLER (ipc_server.cpp), under the same
    // membership mutex: Session is only forward-declared here, and this TU stays
    // engine-free and session-free so test_client_slot.cpp can host-compile it.
    return true;
}

}  // namespace bridge
