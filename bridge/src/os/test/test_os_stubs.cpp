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

    {
        ProcessIdentity p;
        std::memset(&p, 0x5a, sizeof(p));
        refusal(process_identity(&p), "process_identity");
        check(p.exe_path[0] == '\0' && !p.is_game, "process_identity: out is empty");
        bool g = true;
        refusal(exe_path_is_game("ck3", &g), "exe_path_is_game");
        check(!g, "exe_path_is_game: out is false");
    }
    {
        ImageInfo i{1, 2};
        refusal(image_base(&i), "image_base");
        check(i.base == 0 && i.end == 0, "image_base: out is empty");
    }
    {
        BinaryIdentity b;
        std::memset(&b, 0x5a, sizeof(b));
        refusal(binary_identity(&b), "binary_identity");
        check(b.format == IdentityFormat::none && b.observed_hex[0] == '\0' &&
                  b.check != nullptr && std::strcmp(b.check, "unchecked") == 0,
              "binary_identity: out says unchecked, observed nothing");
    }
    {
        unsigned char buf[4] = {1, 2, 3, 4};
        refusal(safe_read(buf, reinterpret_cast<std::uintptr_t>(buf), sizeof(buf)), "safe_read");
        refusal(write_protected(reinterpret_cast<std::uintptr_t>(buf), buf, sizeof(buf)),
                "write_protected");
    }
    {
        ExecInterposition e{"x", nullptr, 7};
        refusal(exec_interposition(&e), "exec_interposition");
        check(e.symbols == nullptr && e.symbol_count == 0, "exec_interposition: out is empty");
    }
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
    {
        Paths p{"a", "b", "c", "d", "e", "f"};
        refusal(paths(&p), "paths");
        check(p.instance_id && !*p.instance_id && p.socket && !*p.socket && p.bridge_log &&
                  !*p.bridge_log && p.telemetry_dir && !*p.telemetry_dir &&
                  p.log_archive_dir && !*p.log_archive_dir && p.state_dir && !*p.state_dir,
              "paths: every field is empty, none null");
    }
    refusal(input_click(1, 1, MouseButton::left), "input_click");
    {
        std::uint64_t t = 99;
        refusal(thread_id(&t), "thread_id");
        check(t == 0, "thread_id: out is 0");
        refusal(set_thread_name("ck3-test"), "set_thread_name");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
