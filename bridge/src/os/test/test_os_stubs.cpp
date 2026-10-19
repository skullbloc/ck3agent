// test_os_stubs.cpp — the Windows and macOS stub contract, engine-free and
// portable (standard C++17, no platform header).
//
// Built natively on windows-latest (MSVC) and macos-14 (Apple clang) by the CI
// stub legs, and on Linux twice, once per stub TU (test_os_stubs_windows,
// test_os_stubs_macos), so the stubs are compiled as strict C++17 on every
// push even where no Windows or Mac runner looks.
//
// The contract: every entry point refuses with a typed status that is NOT ok
// (not_implemented or unavailable_on_platform), carries a detail that starts
// with the platform's name, and leaves its out-parameters empty. A stub that
// returned ok, or returned a refusal with garbage in an out-parameter, would
// let a caller read a default as an answer.
//
// CK3_OS_EXPECT_PLATFORM is the platform name the build declared; the test
// checks platform() against it, so linking the wrong stub file fails here.

#include "os/os.h"

#include <cstdio>
#include <cstring>

#ifndef CK3_OS_EXPECT_PLATFORM
#error "CK3_OS_EXPECT_PLATFORM must name the platform under test"
#endif

using namespace bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

static bool is_refusal(Status s) {
    return s == Status::not_implemented || s == Status::unavailable_on_platform;
}

static void refusal(const Result& r, const char* what) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: status is a refusal (got %s)", what,
                  status_string(r.status));
    check(is_refusal(r.status), buf);
    std::snprintf(buf, sizeof(buf), "%s: os_error is 0", what);
    check(r.os_error == 0, buf);
    std::snprintf(buf, sizeof(buf), "%s: detail names the platform", what);
    const char* name = platform_name(platform());
    check(r.detail != nullptr && std::strncmp(r.detail, name, std::strlen(name)) == 0, buf);
}

int main() {
    const char* name = platform_name(platform());
    std::printf("platform layer stubs: %s\n", name);
    check(std::strcmp(name, CK3_OS_EXPECT_PLATFORM) == 0,
          "platform() is the platform this target was built for");
    check(platform() != Platform::kLinux, "a stub never claims to be linux");

    // The vocabulary is total: every value has its own spelling.
    check(std::strcmp(status_string(Status::ok), "ok") == 0, "status_string(ok)");
    check(std::strcmp(status_string(Status::failed), "failed") == 0, "status_string(failed)");
    check(std::strcmp(status_string(Status::not_routed), "not_routed") == 0,
          "status_string(not_routed)");
    check(std::strcmp(status_string(Status::not_implemented), "not_implemented") == 0,
          "status_string(not_implemented)");
    check(std::strcmp(status_string(Status::unavailable_on_platform),
                      "unavailable_on_platform") == 0,
          "status_string(unavailable_on_platform)");

#ifndef CK3_OS_CELL_PROCESS
    {
        ProcessIdentity p;
        std::memset(&p, 0x5a, sizeof(p));
        refusal(process_identity(&p), "process_identity");
        check(p.exe_path[0] == '\0' && !p.is_game, "process_identity: out is empty");
        bool g = true;
        refusal(exe_path_is_game("ck3", &g), "exe_path_is_game");
        check(!g, "exe_path_is_game: out is false");
    }
#endif
#ifndef CK3_OS_CELL_IMAGE
    {
        ImageInfo i{1, 2};
        refusal(image_base(&i), "image_base");
        check(i.base == 0 && i.end == 0, "image_base: out is empty");
        std::size_t n = 7;
        Section s[1];
        refusal(image_sections(s, 1, &n), "image_sections");
        check(n == 0, "image_sections: count is 0");
    }
#endif
#ifndef CK3_OS_CELL_IDENTITY
    {
        BinaryIdentity b;
        std::memset(&b, 0x5a, sizeof(b));
        refusal(binary_identity(&b), "binary_identity");
        check(b.format == IdentityFormat::none && b.observed_hex[0] == '\0' &&
                  b.check != nullptr && std::strcmp(b.check, "unchecked") == 0 &&
                  b.image_sha256[0] == '\0' && b.image_size == 0,
              "binary_identity: out says unchecked, observed nothing");
    }
#endif
    {
        unsigned char buf[4] = {1, 2, 3, 4};
        (void)buf;
#ifndef CK3_OS_CELL_SAFE_READ
        refusal(safe_read(buf, reinterpret_cast<std::uintptr_t>(buf), sizeof(buf)), "safe_read");
#endif
#ifndef CK3_OS_CELL_WRITE_PROTECTED
        refusal(write_protected(reinterpret_cast<std::uintptr_t>(buf), buf, sizeof(buf)),
                "write_protected");
#endif
    }
    {
        ExecInterposition e{"x", nullptr, 7};
        refusal(exec_interposition(&e), "exec_interposition");
        check(e.symbols == nullptr && e.symbol_count == 0, "exec_interposition: out is empty");
    }
#ifndef CK3_OS_CELL_TRANSPORT
    {
        Endpoint ep{TransportKind::af_unix, "x"};
        refusal(ipc_endpoint(&ep), "ipc_endpoint");
        check(ep.kind == TransportKind::none && ep.address && ep.address[0] == '\0',
              "ipc_endpoint: out is empty");
        Handle h = 42;
        refusal(transport_listen(ep, &h), "transport_listen");
        check(h == kInvalidHandle, "transport_listen: handle is invalid");
        h = 42;
        refusal(transport_accept(kInvalidHandle, &h), "transport_accept");
        check(h == kInvalidHandle, "transport_accept: handle is invalid");
        char b[8];
        std::size_t n = 9;
        refusal(transport_read(kInvalidHandle, b, sizeof(b), &n), "transport_read");
        check(n == 0, "transport_read: nothing read");
        n = 9;
        refusal(transport_write(kInvalidHandle, b, sizeof(b), &n), "transport_write");
        check(n == 0, "transport_write: nothing written");
        refusal(transport_close(kInvalidHandle), "transport_close");
    }
#endif
#ifndef CK3_OS_CELL_PATHS
    {
        Paths p{"a", "b", "c", "d", "e", "f"};
        refusal(paths(&p), "paths");
        check(p.instance_id && !*p.instance_id && p.socket && !*p.socket && p.bridge_log &&
                  !*p.bridge_log && p.telemetry_dir && !*p.telemetry_dir &&
                  p.log_archive_dir && !*p.log_archive_dir && p.state_dir && !*p.state_dir,
              "paths: every field is empty, none null");
    }
#endif
    refusal(input_click(1, 1, MouseButton::left), "input_click");
#ifndef CK3_OS_CELL_THREADS
    {
        std::uint64_t t = 99;
        refusal(thread_id(&t), "thread_id");
        check(t == 0, "thread_id: out is 0");
        refusal(set_thread_name("ck3-test"), "set_thread_name");
    }
#endif
#ifndef CK3_OS_CELL_ENTRY
    {
        EntryState es;
        std::memset(&es, 0x5a, sizeof(es));
        refusal(entry_state(&es), "entry_state");
        check(es.phase == EntryPhase::not_attached && es.module == 0 &&
                  es.init_thread_id == 0 && !es.is_game && es.outcome && !*es.outcome,
              "entry_state: out is empty");
    }
#endif
    {
#ifndef CK3_OS_CELL_GUARDED
        static int ran = 0;
        Fault f{1, 2, 3};
        refusal(guarded_call([](void*) { ++ran; }, nullptr, &f), "guarded_call");
        check(ran == 0, "guarded_call: a refusing stub never runs the body");
        check(f.code == 0 && f.address == 0 && f.pc == 0, "guarded_call: fault is empty");
        refusal(guard_rearm(), "guard_rearm");
#endif
    }
#ifndef CK3_OS_CELL_EXEC_ALLOC
    {
        ExecBlock e{1, 2};
        refusal(alloc_exec_near(0x10000, 64, &e), "alloc_exec_near");
        check(e.base == 0 && e.size == 0, "alloc_exec_near: nothing allocated");
        refusal(free_exec(&e), "free_exec");
        refusal(register_unwind(e, nullptr, 0), "register_unwind");
        refusal(unregister_unwind(e), "unregister_unwind");
    }
#endif
#ifndef CK3_OS_CELL_WATCHDOG
    {
        refusal(watchdog_arm(1, nullptr, nullptr), "watchdog_arm");
        bool fired = true;
        refusal(watchdog_disarm(&fired), "watchdog_disarm");
        check(!fired, "watchdog_disarm: fired is false");
    }
#endif
    {
        std::uintptr_t vt = 9;
        refusal(rtti_find_vtable("x", &vt), "rtti_find_vtable");
        check(vt == 0, "rtti_find_vtable: out is 0");
        HookPlan hp{9, 9, true, true};
        refusal(hook_plan(0x1000, &hp), "hook_plan");
        check(hp.target == 0 && hp.steal_bytes == 0 && !hp.cfg_checked_site &&
                  !hp.cet_compatible,
              "hook_plan: out is empty");
    }
    check(std::strcmp(entry_phase_string(EntryPhase::init_done), "init_done") == 0,
          "entry_phase_string(init_done)");
    check(std::strcmp(identity_format_string(IdentityFormat::image_sha256), "image_sha256") == 0,
          "identity_format_string(image_sha256)");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
