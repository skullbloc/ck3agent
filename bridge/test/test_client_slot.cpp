// test/test_client_slot.cpp — ck-xqol regression tests for the IPC client-slot
// LIFECYCLE.
//
// WHY THIS FILE EXISTS.  A production bridge SIGSEGV'd on the reader thread in
// release_slot_locked() at CK3 startup.  The offline suite could not have
// caught it: test_session_registry exercises the registry in isolation, with no
// slot table, no reader thread and no concurrency, so nothing in the gate ever
// released a socket slot that owned a session.  This closes that gap.
//
// It links the REAL SessionRegistry (session.cpp), so a session that is leaked
// onto a dead slot, or destroyed twice, is observable as a registry count — not
// merely as an assertion about a pointer.
//
// NEGATIVE CONTROL.  The same source compiled with -DCK3_SLOT_LEGACY_FD_GUARD
// restores the pre-fix guard and MUST fail:
//   * T1 dies with SIGSEGV in the exact statement the production core faulted
//     in (clearing a std::string whose _M_p is still null),
//   * T5 reports a session orphaned on a released slot.
// Build target: test_client_slot_negative_control.

#include "bridge.h"       // ck-6kvq.5: MemRange, for the stub globals below
#include "client_slot.h"
#include "session.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>
#include <new>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// Stub globals referenced by bridge.h / session.cpp when compiled standalone
// (same set test_session_registry.cpp provides).  ck-6kvq.5 added the last
// three: session.cpp now links world_identity.cpp / world_epoch.cpp, because
// the RESUME decision is taken with ck-9fc3's judge_same_save() rather than a
// second copy of the rule, and world_epoch.cpp does guarded reads.
namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t             g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex     g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}

using namespace bridge;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

// The slot never owns a real fd in this test; count the closes instead so a
// stray close(0) (which is what the production bug actually did to stdin) is
// observable rather than destructive.
static std::atomic<int> g_closed_count{0};
static std::atomic<int> g_last_closed_fd{-12345};
static int fake_close(int fd) {
    g_closed_count.fetch_add(1);
    g_last_closed_fd.store(fd);
    return 0;
}

// ───────────────────────────────────────────────────────────────────────────
// T1 — THE CRASH.  A slot table that has not been constructed yet.
//
// dll_init (the library's ELF constructor, .init_array[1]) starts the reader
// thread before _GLOBAL__sub_I_ipc_server.cpp (.init_array[3]) constructs the
// slot table.  The reader can therefore reach a slot whose storage is raw
// zeroes: fd == 0 (not -1), gen == 0, session == nullptr, and a std::string
// whose _M_p is null rather than pointing at its own local buffer.
//
// Raw zero storage is reproduced here exactly — no constructor is run over the
// buffer — which is the only honest way to test it.  Run in a CHILD so the
// negative control's SIGSEGV is reported rather than taking the suite with it.
// ───────────────────────────────────────────────────────────────────────────
static int t1_child() {
    g_closed_count.store(0);
    alignas(ClientSlot) unsigned char storage[sizeof(ClientSlot)];
    std::memset(storage, 0, sizeof(storage));
    ClientSlot* raw = reinterpret_cast<ClientSlot*>(storage);

    // Sanity: this really is the crashed-run shape, "slot=0 fd=0 peer_pid=0".
    if (raw->fd != 0 || raw->peer_pid != 0) return 40;

    SlotReleaseResult r = client_slot_release(*raw, fake_close);

    if (r.released) return 41;                    // must refuse a dead slot
    if (g_closed_count.load() != 0) return 42;    // must not close(0) == stdin
    if (raw->fd != 0 || raw->gen != 0) return 43; // must not have touched it
    return 0;
}

static void t1_never_constructed_slot() {
    std::printf("T1 release of a NEVER-CONSTRUCTED slot (the ck-xqol core)\n");
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        std::_Exit(t1_child());
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        ++g_checks; ++g_failures;
        std::printf("  FAIL child died with signal %d (%s) — this IS the production crash\n",
                    WTERMSIG(status), strsignal(WTERMSIG(status)));
        return;
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "zero-initialised slot is refused, untouched, and stdin is not closed");
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        std::printf("       (child exit code %d)\n", WEXITSTATUS(status));
}

// ───────────────────────────────────────────────────────────────────────────
// T2 — release WITH AN ATTACHED SESSION.  The case the offline suite never had.
// ───────────────────────────────────────────────────────────────────────────
static void t2_release_with_session() {
    std::printf("T2 release of a slot that OWNS a session\n");
    ClientSlot s;
    const size_t before = g_session_registry.ids().size();

    uint32_t gen = client_slot_open(s, 7, 1000);
    Session* sess = g_session_registry.create("t2-agent", Session::Mode::CONTROL);
    check(sess != nullptr, "session minted");
    check(client_slot_attach_session(s, gen, sess, "tok-from-handshake-frame"),
          "session attached to the open slot");
    const std::string id = sess->session_id;

    std::string why;
    check(g_session_registry.claim_character(sess, 4242, &why), "session claims a character");

    g_closed_count.store(0);
    SlotReleaseResult r = client_slot_release(s, fake_close);
    check(r.released, "release reports it did the work");
    check(r.fd == 7, "release reports the fd it closed");
    check(g_closed_count.load() == 1, "the fd was closed exactly once");
    check(r.session == sess, "release HANDS BACK the session rather than dropping it");
    check(r.bound_gen == gen, "release reports the generation the session was bound at");
    check(!slot_is_open(s), "slot is free");
    check(s.session == nullptr, "slot no longer points at the session");
    check(s.resume_token.empty(), "cached resume token cleared");
    check(s.gen == gen + 1, "generation bumped, invalidating stale sends");

    // The caller owns teardown; do it the way release_slot_locked does, with the
    // id copied BY VALUE (the id lives inside the object destroy() erases).
    check(g_session_registry.find(id) != nullptr, "session still in the registry before teardown");
    check(g_session_registry.destroy(id), "caller destroys the handed-back session");
    check(g_session_registry.find(id) == nullptr, "session gone");
    check(g_session_registry.owner_of(4242) == nullptr, "its claim was released");
    check(g_session_registry.ids().size() == before, "no session leaked");
}

// ───────────────────────────────────────────────────────────────────────────
// T3 — DOUBLE RELEASE.  The second release must be a total no-op: it must not
// close the fd again, must not hand the session back a second time (which
// would double-destroy it in the registry), and must not bump the generation
// again (which would invalidate the NEXT client's sends).
// ───────────────────────────────────────────────────────────────────────────
static void t3_double_release() {
    std::printf("T3 DOUBLE release\n");
    ClientSlot s;
    uint32_t gen = client_slot_open(s, 9, 2000);
    Session* sess = g_session_registry.create("t3-agent", Session::Mode::OBSERVE);
    client_slot_attach_session(s, gen, sess, "tok-from-handshake-frame");
    const std::string id = sess->session_id;

    g_closed_count.store(0);
    SlotReleaseResult a = client_slot_release(s, fake_close);
    const uint32_t gen_after_first = s.gen;
    SlotReleaseResult b = client_slot_release(s, fake_close);

    check(a.released && a.session == sess, "first release owns the teardown");
    check(!b.released, "second release is a no-op");
    check(b.session == nullptr, "second release hands back NOTHING (no double destroy)");
    check(g_closed_count.load() == 1, "fd closed exactly once across both releases");
    check(s.gen == gen_after_first, "second release does not bump the generation again");

    check(g_session_registry.destroy(id), "single teardown succeeds");
    check(!g_session_registry.destroy(id), "a second destroy of the same id is refused");
}

// ───────────────────────────────────────────────────────────────────────────
// T4 — RELEASE RACING ACCEPT.  Two threads hammering open/release on one slot.
// The invariant under test: the generation is monotonic, the slot is never left
// half-open, and every fd that was opened is closed exactly once.
// ───────────────────────────────────────────────────────────────────────────
static void t4_release_races_accept() {
    std::printf("T4 release RACING the accept path\n");
    ClientSlot s;
    std::mutex membership;          // stands in for g_client_mutex
    std::atomic<int> opened{0};
    std::atomic<bool> bad_gen{false};
    g_closed_count.store(0);

    const int ROUNDS = 20000;
    std::thread acceptor([&] {
        for (int i = 0; i < ROUNDS; i++) {
            std::lock_guard<std::mutex> lk(membership);
            if (!slot_is_open(s)) {
                uint32_t before = s.gen;
                uint32_t after = client_slot_open(s, 3 + (i % 5), 1);
                if (after != before + 1) bad_gen.store(true);
                opened.fetch_add(1);
            }
        }
    });
    std::thread reaper([&] {
        for (int i = 0; i < ROUNDS; i++) {
            std::lock_guard<std::mutex> lk(membership);
            client_slot_release(s, fake_close);
        }
    });
    acceptor.join();
    reaper.join();
    {
        std::lock_guard<std::mutex> lk(membership);
        if (slot_is_open(s)) client_slot_release(s, fake_close);
    }

    check(!bad_gen.load(), "every accept bumped the generation by exactly one");
    check(opened.load() > 0, "the race actually ran (slot was opened at least once)");
    check(g_closed_count.load() == opened.load(),
          "every opened fd was closed exactly once (no leak, no double close)");
    check(!slot_is_open(s) && s.session == nullptr, "slot left clean");
}

// ───────────────────────────────────────────────────────────────────────────
// T5 — RELEASE RACING A HANDSHAKE.  The handshake frame is parsed WITHOUT the
// membership mutex; only the attach takes it.  If the socket drops in that
// window the slot is released (and may already be re-accepted), so an
// unconditional bind either orphans the session on a free slot — nothing
// releases a slot that is not open, so its claims are held forever — or hands
// it to whoever now owns the slot.
//
// Failure here is a LEAK, not a crash, which is why it is worth its own test:
// the pre-fix build passes T2/T3/T4 and fails only this.
// ───────────────────────────────────────────────────────────────────────────
static void t5_release_races_handshake() {
    std::printf("T5 release RACING a handshake attach\n");
    ClientSlot s;
    const size_t before = g_session_registry.ids().size();

    // (a) Attach against a slot that was released after the frame was read.
    uint32_t read_gen = client_slot_open(s, 11, 3000);
    Session* sess = g_session_registry.create("t5-late", Session::Mode::CONTROL);
    const std::string id = sess->session_id;
    SlotReleaseResult r = client_slot_release(s, fake_close);   // socket dropped
    check(r.released && r.session == nullptr, "slot released mid-handshake, owning no session");

    bool attached = client_slot_attach_session(s, read_gen, sess, "tok-from-handshake-frame");
    check(!attached, "attach to a RELEASED slot is refused");
    check(s.session == nullptr,
          "no session was orphaned on the free slot (an orphan holds its claims forever)");
    if (attached) {          // negative control lands here: clean up the orphan
        s.session = nullptr;
        s.resume_token.clear();
    }
    g_session_registry.destroy(id);

    // (b) Attach against a slot that was released AND re-accepted by someone
    // else. Binding here would attribute one client's actions to another's
    // session, which is worse than a leak.
    uint32_t gen_a = client_slot_open(s, 12, 4000);
    Session* stale = g_session_registry.create("t5-stale", Session::Mode::CONTROL);
    const std::string stale_id = stale->session_id;
    client_slot_release(s, fake_close);
    uint32_t gen_b = client_slot_open(s, 13, 5000);   // a DIFFERENT client
    check(gen_b != gen_a, "the new connection has a new generation");

    bool crossed = client_slot_attach_session(s, gen_a, stale, "tok-from-handshake-frame");
    check(!crossed, "attach with a STALE generation is refused");
    check(s.session == nullptr, "the new client did not inherit the old client's session");
    if (crossed) { s.session = nullptr; s.resume_token.clear(); }
    g_session_registry.destroy(stale_id);

    SlotReleaseResult last = client_slot_release(s, fake_close);
    check(last.released, "the live connection still releases normally");
    check(g_session_registry.ids().size() == before, "no session leaked across the race");
}


// ───────────────────────────────────────────────────────────────────────────
// T6 — STARTUP, end to end, as the production crash actually happened.
//
// T1 proves release refuses a never-constructed slot.  It does NOT explain how
// the reader ever reached that slot: the reader only releases slots it POLLED,
// and it builds its poll set from a membership scan over the whole table.  The
// scan is the half that put fd 0 — stdin — into the poll set, so it is the half
// that has to be tested.
//
// (a) THE MEMBERSHIP SCAN over a raw-.bss table of MAX_CLIENTS slots.  The old
//     test was `fd >= 0`, and zero storage reads fd == 0, so EVERY slot in an
//     unconstructed table looked live and slot 0 polled stdin.  Nothing may
//     enter the poll set here, and in particular fd 0 may not.
//
// (b) CONNECT AND IMMEDIATELY DISCONNECT during startup, over a REAL socket:
//     open a slot on one end of a socketpair with a session attached, close the
//     peer, then run the reader's own shape — poll, read, see EOF, release.
//     This is the sequence that produced "Releasing client slot=0 fd=0
//     peer_pid=0 (reader EOF / error)" on the node.  It must close exactly the
//     socket it accepted, never fd 0, and must tear the session down cleanly.
// ───────────────────────────────────────────────────────────────────────────
static int t6_child() {
    // The fork inherits the parent's close counter; this child measures only
    // its own closes.
    g_closed_count.store(0);
    g_last_closed_fd.store(-12345);

    // (a) raw zero storage for a FULL table, no constructor run over it.
    alignas(ClientSlot) static unsigned char table[sizeof(ClientSlot) * MAX_CLIENTS];
    std::memset(table, 0, sizeof(table));
    ClientSlot* slots = reinterpret_cast<ClientSlot*>(table);

    int polled = 0;
    bool polled_stdin = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (slot_is_open(slots[i])) {            // the reader's membership test
            polled++;
            if (slots[i].fd == 0) polled_stdin = true;
        }
    }
    if (polled != 0) return 60;                  // an unconstructed table is EMPTY
    if (polled_stdin) return 61;                 // and above all never polls stdin

    // Releasing every slot of that table must also be inert.
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_slot_release(slots[i], fake_close).released) return 62;
    }
    if (g_closed_count.load() != 0) return 63;   // nothing closed; stdin intact
    if (fcntl(STDIN_FILENO, F_GETFD) < 0) return 64;  // stdin still open
    return 0;
}

static void t6_startup_connect_then_disconnect() {
    std::printf("T6 STARTUP: membership scan over a raw table, then a client that "
                "connects and immediately disconnects\n");
    std::fflush(stdout);

    // (a) in a child: the negative control's scan admits 32 dead slots and the
    // release loop then faults, which would take the suite with it.
    pid_t pid = fork();
    if (pid == 0) std::_Exit(t6_child());
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        ++g_checks; ++g_failures;
        std::printf("  FAIL child died with signal %d (%s) — the reader polled an "
                    "unconstructed table\n", WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else {
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "an unconstructed slot table contributes NOTHING to the poll set "
              "(fd 0 is never polled, never closed)");
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            std::printf("       (child exit code %d)\n", WEXITSTATUS(status));
    }

    // (b) a real connect-then-immediate-disconnect, in this process.
    const size_t before = g_session_registry.ids().size();
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair for a real client");

    ClientSlot s;
    uint32_t gen = client_slot_open(s, sv[0], 6000);
    Session* sess = g_session_registry.create("t6-flapper", Session::Mode::CONTROL);
    const std::string id = sess->session_id;
    check(client_slot_attach_session(s, gen, sess, "tok-from-handshake-frame"),
          "session attached to the accepted slot");
    check(slot_is_open(s), "an ACCEPTED slot IS in the poll set");

    ::close(sv[1]);                       // the client goes away immediately

    // The reader's own shape: poll, see the hangup/readable, read, get EOF.
    struct pollfd pfd { s.fd, POLLIN, 0 };
    check(poll(&pfd, 1, 500) > 0, "poll reports the dropped peer");
    char buf[64];
    check(::read(s.fd, buf, sizeof(buf)) == 0, "read returns EOF, as on the node");

    g_closed_count.store(0);
    g_last_closed_fd.store(-12345);
    SlotReleaseResult r = client_slot_release(s, fake_close);
    check(r.released, "release runs for a genuinely dropped client");
    check(g_last_closed_fd.load() == sv[0],
          "it closed the socket it accepted — NOT fd 0");
    check(g_closed_count.load() == 1, "closed exactly once");
    check(r.session == sess, "the dropped client's session is handed back");
    check(!slot_is_open(s), "the slot is free again");

    g_session_registry.destroy(id);
    ::close(sv[0]);
    check(g_session_registry.ids().size() == before, "no session leaked");
    check(fcntl(STDIN_FILENO, F_GETFD) >= 0, "stdin survived the whole sequence");
}

int main() {
    std::printf("ck-xqol: IPC client-slot lifecycle\n");
#ifdef CK3_SLOT_LEGACY_FD_GUARD
    std::printf("*** NEGATIVE CONTROL BUILD — the pre-fix guard is restored; "
                "this binary is EXPECTED to fail ***\n");
#endif
    std::printf("\n");

    t1_never_constructed_slot();
    t2_release_with_session();
    t3_double_release();
    t4_release_races_accept();
    t5_release_races_handshake();
    t6_startup_connect_then_disconnect();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL SLOT LIFECYCLE TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
