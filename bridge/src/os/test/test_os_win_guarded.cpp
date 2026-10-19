// test_os_win_guarded.cpp — os::guarded_call on Windows, the deliberate-fault
// test (ADR 0001 P2, brief item 3): a guarded body reads an unmapped page,
// recovery resumes, and the region reports failure with the fault. Then the
// process keeps running, which is the whole point. Engine-free, under wine.
//
// Note on wine: it implements VEH and translates Unix signals into it, so this
// exercises the Rip-rewrite path; the ADR flags the result MEDIUM until it is
// re-run on real Windows (fault ordering and CONTEXT contents can differ).

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

// Shared across the guarded bodies (thread-locals in the cell; plain statics
// here are fine, the test is single-threaded per case).
static std::uint8_t* g_bad = nullptr;        // a reserved, unmapped page
static volatile std::uint8_t g_sink = 0;     // defeats dead-read elimination
static int g_ran = 0;

static void read_unmapped(void*) {
    g_ran = 1;
    g_sink = *g_bad;        // EXCEPTION_ACCESS_VIOLATION
    g_ran = 2;              // never reached
}

static void writes_a_value(void* p) { *static_cast<int*>(p) = 42; }

static void nested(void*) {
    os::Fault inner;
    os::Result r = os::guarded_call(read_unmapped, nullptr, &inner);
    // The inner region caught the fault; this outer body then faults too.
    check(r.status == os::Status::failed, "nested: the inner region caught its own fault");
    g_sink = *g_bad;        // now the OUTER region must catch
}

int main() {
    g_bad = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
    check(g_bad != nullptr, "reserved an unmapped page to fault on");

    // 1. A body that returns normally.
    int value = 0;
    os::Fault f{1, 2, 3};
    check(os::succeeded(os::guarded_call(writes_a_value, &value, &f)) && value == 42,
          "a body that does not fault: ok, its side effect happened");
    check(f.code == 0 && f.address == 0, "no fault: Fault left empty");

    // 2. The deliberate fault: read an unmapped page.
    g_ran = 0;
    os::Fault bad{0, 0, 0};
    os::Result r = os::guarded_call(read_unmapped, nullptr, &bad);
    check(r.status == os::Status::failed, "the faulting body: the region REPORTS failure");
    check(bad.code == std::uint32_t(EXCEPTION_ACCESS_VIOLATION),
          "the fault is an access violation (0xC0000005)");
    check(bad.address == reinterpret_cast<std::uintptr_t>(g_bad),
          "the fault address is the unmapped page the body read");
    check(bad.pc != 0, "the fault PC was captured");
    check(g_ran == 1, "the body ran up to the fault and no further");
    check(r.os_error == int(EXCEPTION_ACCESS_VIOLATION), "os_error carries the exception code");

    // 3. The process survived: a normal guarded call still works afterwards.
    value = 0;
    check(os::succeeded(os::guarded_call(writes_a_value, &value, nullptr)) && value == 42,
          "after a recovered fault, the process keeps running and guarding");

    // 4. Nesting: the inner region catches its fault, the outer catches its own.
    os::Fault outer{0, 0, 0};
    os::Result nr = os::guarded_call(nested, nullptr, &outer);
    check(nr.status == os::Status::failed &&
              outer.address == reinterpret_cast<std::uintptr_t>(g_bad),
          "nesting: the outer region reports its own recovered fault");

    // 5. guard_rearm re-installs the handler and recovery still works.
    check(os::succeeded(os::guard_rearm()), "guard_rearm: ok");
    check(os::guarded_call(read_unmapped, nullptr, nullptr).status == os::Status::failed,
          "after guard_rearm: a fault is still recovered");

    // 6. Refusal.
    check(os::guarded_call(nullptr, nullptr, nullptr).status == os::Status::failed,
          "guarded_call(null body): failed");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
