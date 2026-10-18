// test_os_linux_adapters.cpp — the Linux adapters in bridge/src/os/linux/
// return what the direct calls return. Engine-free: no CK3, no socket.
//
// PROCESS IDENTITY and IMAGE BASE are the two transcriptions (the originals
// are file-static in bridge_init.cpp), so each is checked against the direct
// syscalls AND against a second, independent witness:
//   exe path   readlink("/proc/self/exe")           (the direct call)
//   is_game    basename == "ck3" over that path     (the direct rule), plus
//              a table of paths that must and must not classify as the game
//   base/end   dl_iterate_phdr's main-exe entry      (the direct call), and
//              dladdr() of a function in this binary (independent: the
//              loader's own answer for which object holds an address)
// The forwarding adapters are checked against the function they forward to.
//
// This binary is linked -pie (CMake) because ck3 is PIE; a non-PIE main
// executable reports load address 0, which the adapter calls `failed`.

#include "os/os.h"

#include "bridge.h"
#include "build_id_witness.h"
#include "instance_paths.h"
#include "thread_registry.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <link.h>
#include <pthread.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

#ifndef CK3_BRIDGE_VERSION_SCRIPT
#error "CK3_BRIDGE_VERSION_SCRIPT must be the path of bridge/bridge.version"
#endif

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    } else {
        std::printf("ok:   %s\n", what);
    }
}

// ── process identity ────────────────────────────────────────────────────────

static void test_process_identity() {
    char direct[os::kMaxPath] = {};
    ssize_t len = readlink("/proc/self/exe", direct, sizeof(direct) - 1);
    check(len > 0, "direct readlink(/proc/self/exe) succeeds");
    if (len > 0) direct[len] = '\0';
    const char* slash = strrchr(direct, '/');
    const bool direct_is_game = slash && strcmp(slash + 1, "ck3") == 0;

    os::ProcessIdentity p;
    memset(&p, 0x5a, sizeof(p));
    os::Result r = os::process_identity(&p);
    check(os::succeeded(r), "process_identity: ok");
    check(strcmp(p.exe_path, direct) == 0, "process_identity: exe_path == readlink(/proc/self/exe)");
    check(p.is_game == direct_is_game, "process_identity: is_game == basename rule on that path");
    check(!p.is_game, "process_identity: this test binary is not the game");

    struct Case { const char* path; bool game; };
    const Case cases[] = {
        {"/home/u/.steam/steamapps/common/Crusader Kings III/binaries/ck3", true},
        {"/ck3", true},
        {"ck3", false},                 // no '/': is_ck3_process() needs one too
        {"/x/ck3.exe", false},
        {"/x/CK3", false},              // exact, case-sensitive on Linux
        {"/x/ck3_launcher", false},
        {"/x/launcher/dowser", false},
        {"/x/ck3/", false},
        {"", false},
    };
    for (const Case& c : cases) {
        bool g = !c.game;
        os::Result cr = os::exe_path_is_game(c.path, &g);
        char what[256];
        snprintf(what, sizeof(what), "exe_path_is_game(\"%s\") == %s", c.path,
                 c.game ? "true" : "false");
        check(os::succeeded(cr) && g == c.game, what);
    }
    bool g = true;
    check(os::exe_path_is_game(nullptr, &g).status == os::Status::failed && !g,
          "exe_path_is_game(nullptr) fails, is_game false");
}

// ── image base ──────────────────────────────────────────────────────────────

static int probe_symbol() { return 7; }

static void test_image_base() {
    struct Direct { uintptr_t base; uintptr_t end; bool hit; };
    Direct d{0, 0, false};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* ud) -> int {
        auto* dd = static_cast<Direct*>(ud);
        if (info->dlpi_name[0] != '\0') return 0;   // the main executable
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD) continue;
            uintptr_t e = info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
            if (e > dd->end) dd->end = e;
        }
        dd->base = info->dlpi_addr;
        dd->hit = true;
        return 1;
    }, &d);
    check(d.hit && d.base != 0, "direct dl_iterate_phdr: main executable at a non-zero base (PIE)");

    os::ImageInfo img{1, 1};
    os::Result r = os::image_base(&img);
    check(os::succeeded(r), "image_base: ok");
    check(img.base == d.base, "image_base: base == dl_iterate_phdr main-exe dlpi_addr");
    check(img.end == d.end, "image_base: end == max PT_LOAD end");

    Dl_info info{};
    const bool found = dladdr(reinterpret_cast<void*>(&probe_symbol), &info) != 0;
    check(found, "dladdr(probe_symbol) resolves");
    check(found && reinterpret_cast<uintptr_t>(info.dli_fbase) == img.base,
          "image_base: base == dladdr(own function).dli_fbase (independent witness)");
    const uintptr_t fn = reinterpret_cast<uintptr_t>(&probe_symbol);
    check(fn >= img.base && fn < img.end, "image_base: an own function lies in [base, end)");
    check(probe_symbol() == 7, "probe_symbol is live code");

    check(os::image_base(nullptr).status == os::Status::failed, "image_base(nullptr) fails");
}

// ── forwards ────────────────────────────────────────────────────────────────

static void test_binary_identity() {
    os::BinaryIdentity b;
    memset(&b, 0x5a, sizeof(b));
    check(os::succeeded(os::binary_identity(&b)), "binary_identity: ok");
    check(b.format == os::IdentityFormat::elf_gnu_build_id, "binary_identity: format elf_gnu_build_id");
    check(strcmp(b.check, "unchecked") == 0 && b.observed_hex[0] == '\0',
          "binary_identity: before the resolver runs, the witness says unchecked");

    bridge::BuildIdWitness w;
    w.status = bridge::BuildIdCheck::MISMATCH;
    w.observed_hex = "0123456789abcdef0123456789abcdef01234567";
    bridge::set_build_id_witness(w);
    check(os::succeeded(os::binary_identity(&b)), "binary_identity: ok after the witness is set");
    check(strcmp(b.check, bridge::build_id_check_string(bridge::build_id_witness().status)) == 0 &&
              strcmp(b.check, "mismatch") == 0,
          "binary_identity: check == the witness's status spelling");
    check(bridge::build_id_witness().observed_hex == b.observed_hex,
          "binary_identity: observed_hex == the witness's");
    bridge::set_build_id_witness(bridge::BuildIdWitness{});
}

static void test_guarded_memory() {
    unsigned char src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char a[8] = {}, b[8] = {};
    const bool direct = bridge::safe_read_bytes(reinterpret_cast<uintptr_t>(src), a, sizeof(a));
    os::Result r = os::safe_read(b, reinterpret_cast<uintptr_t>(src), sizeof(b));
    check(direct && os::succeeded(r) && memcmp(a, b, sizeof(a)) == 0 && memcmp(a, src, 8) == 0,
          "safe_read: mapped read == safe_read_bytes");

    const int faults = bridge::safe_read_fault_count();
    const bool direct_bad = bridge::safe_read_bytes(0x10, a, sizeof(a));
    os::Result bad = os::safe_read(b, 0x10, sizeof(b));
    check(!direct_bad && bad.status == os::Status::failed && bad.os_error == EFAULT,
          "safe_read: unmapped read fails with EFAULT, like safe_read_bytes");
    check(bridge::safe_read_fault_count() == faults + 2,
          "safe_read: bumps the same fault counter safe_read_bytes does");

    // /proc/self/mem ignores page protection: write into a PROT_READ page.
    const long pg = sysconf(_SC_PAGESIZE);
    void* page = mmap(nullptr, static_cast<size_t>(pg), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(page != MAP_FAILED, "mmap scratch page");
    if (page == MAP_FAILED) return;
    mprotect(page, static_cast<size_t>(pg), PROT_READ);
    auto* p = static_cast<unsigned char*>(page);
    const unsigned char one = 0xA1, two = 0xB2;
    const bool direct_w = bridge::write_to_readonly(p, &one, 1);
    os::Result w = os::write_protected(reinterpret_cast<uintptr_t>(p + 1), &two, 1);
    check(os::succeeded(w) == direct_w, "write_protected: same outcome as write_to_readonly");
    if (direct_w)
        check(p[0] == one && p[1] == two, "write_protected: wrote through PROT_READ");
    munmap(page, static_cast<size_t>(pg));
}

static std::set<std::string> version_script_globals() {
    std::ifstream f(CK3_BRIDGE_VERSION_SCRIPT);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    for (size_t a; (a = text.find("/*")) != std::string::npos;) {   // drop comments
        size_t b = text.find("*/", a + 2);
        text.erase(a, b == std::string::npos ? std::string::npos : b + 2 - a);
    }
    std::set<std::string> out;
    const size_t g = text.find("global:");
    const size_t l = text.find("local:");
    if (g == std::string::npos || l == std::string::npos || l < g) return out;
    std::stringstream body(text.substr(g + 7, l - g - 7));
    std::string tok;
    while (std::getline(body, tok, ';')) {
        const size_t s = tok.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        out.insert(tok.substr(s, tok.find_last_not_of(" \t\r\n") - s + 1));
    }
    return out;
}

static void test_exec_interposition() {
    os::ExecInterposition e{};
    check(os::succeeded(os::exec_interposition(&e)), "exec_interposition: ok");
    std::set<std::string> listed;
    for (size_t i = 0; i < e.symbol_count; ++i) listed.insert(e.symbols[i]);
    const std::set<std::string> exported = version_script_globals();
    check(exported.size() == 4, "bridge.version exports four symbols");
    check(listed == exported, "exec_interposition: symbols == bridge.version global: list");
    for (const std::string& s : listed) {
        // Each must also be resolvable in libc (RTLD_NEXT is what the bridge uses).
        check(dlsym(RTLD_DEFAULT, s.c_str()) != nullptr, ("libc defines " + s).c_str());
    }
}

static void test_paths(const std::string& tmp) {
    // instance_paths resolves once, from the environment, and creates its
    // directories: point it at a private scratch dir first.
    setenv("XDG_RUNTIME_DIR", tmp.c_str(), 1);
    for (const char* k : {"CK3_INSTANCE_ID", "CK3_INSTANCE_LAYOUT", "CK3_INSTANCE_ROOT",
                          "CK3_BRIDGE_SOCKET", "CK3_TELE", "CK3_BRIDGE_LOG",
                          "CK3_BRIDGE_LOG_ARCHIVE_DIR"})
        unsetenv(k);

    os::Paths p{};
    check(os::succeeded(os::paths(&p)), "paths: ok");
    check(strcmp(p.instance_id, bridge::instance_id()) == 0, "paths: instance_id == instance_id()");
    check(strcmp(p.socket, bridge::instance_socket_path()) == 0, "paths: socket == instance_socket_path()");
    check(strcmp(p.bridge_log, bridge::instance_bridge_log()) == 0, "paths: bridge_log == instance_bridge_log()");
    check(strcmp(p.telemetry_dir, bridge::instance_telemetry_dir()) == 0,
          "paths: telemetry_dir == instance_telemetry_dir()");
    check(strcmp(p.log_archive_dir, bridge::instance_log_archive_dir()) == 0,
          "paths: log_archive_dir == instance_log_archive_dir()");
    check(strcmp(p.state_dir, bridge::instance_state_dir()) == 0, "paths: state_dir == instance_state_dir()");
    check(strncmp(p.socket, tmp.c_str(), tmp.size()) == 0, "paths: socket is under the scratch XDG_RUNTIME_DIR");

    os::Endpoint ep{os::TransportKind::none, ""};
    check(os::succeeded(os::ipc_endpoint(&ep)) && ep.kind == os::TransportKind::af_unix &&
              strcmp(ep.address, bridge::instance_socket_path()) == 0,
          "ipc_endpoint: af_unix at instance_socket_path()");
}

static void test_not_routed() {
    os::Endpoint ep{os::TransportKind::af_unix, "/nonexistent"};
    os::Handle h = 5;
    os::Result r = os::transport_listen(ep, &h);
    check(r.status == os::Status::not_routed && h == os::kInvalidHandle &&
              strstr(r.detail, "ipc_server.cpp") != nullptr,
          "transport_listen: not_routed, names ipc_server.cpp, opens nothing");
    r = os::input_click(1, 1, os::MouseButton::left);
    check(r.status == os::Status::not_routed && strstr(r.detail, "action_handlers.cpp") != nullptr,
          "input_click: not_routed, names action_handlers.cpp, injects nothing");
}

static void test_threads() {
    uint64_t t = 0;
    check(os::succeeded(os::thread_id(&t)), "thread_id: ok");
    check(t == static_cast<uint64_t>(syscall(SYS_gettid)) &&
              t == static_cast<uint64_t>(bridge::current_tid()),
          "thread_id == gettid == bridge::current_tid()");
    // On the main thread gettid == getpid, so that check alone cannot tell a
    // thread id from a process id. A second thread can.
    uint64_t other = 0, other_direct = 0;
    std::thread th([&] {
        os::thread_id(&other);
        other_direct = static_cast<uint64_t>(syscall(SYS_gettid));
    });
    th.join();
    check(other == other_direct && other != static_cast<uint64_t>(getpid()),
          "thread_id on a second thread == its gettid, != getpid");

    check(os::succeeded(os::set_thread_name("ck3-os-test-a-very-long-name")),
          "set_thread_name: a long name is truncated, not refused");
    char name[32] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    check(strcmp(name, "ck3-os-test-a-v") == 0, "set_thread_name: kernel holds the first 15 bytes");
}

int main() {
    std::printf("platform layer: %s\n", os::platform_name(os::platform()));
    check(os::platform() == os::Platform::kLinux, "platform() == linux");

    const char* tmpdir = getenv("TMPDIR");
    std::string pattern = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/ck3-os-adapters-XXXXXX";
    std::string buf = pattern;
    char* made = mkdtemp(buf.data());
    check(made != nullptr, "mkdtemp scratch dir");

    test_process_identity();
    test_image_base();
    test_binary_identity();
    test_guarded_memory();
    test_exec_interposition();
    if (made) test_paths(made);
    test_not_routed();
    test_threads();

    if (made) {
        std::error_code ec;
        std::filesystem::remove_all(made, ec);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
