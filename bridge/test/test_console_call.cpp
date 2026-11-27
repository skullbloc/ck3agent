// test_console_call.cpp — the console-handler ABI (console_call.h) under both
// std::string layouts the game is built with.
//
// The readers below are the GAME's reads, transcribed from the binaries:
//   msvc_game_reads_arg   ck3.exe 1.18.3.1 0x7B3980 (the copy CMD_EFFECT runs on
//                         each argument): capacity [+0x18] >= 0x10 -> characters
//                         at [+0], else inline at +0; size [+0x10].
//   gnu_game_reads_arg    libstdc++: characters at [+0], size [+8].
// and the result writes are the handlers' own: CMD_EFFECT's "No game state" path
// (ck3.exe 0xDF5380: [out+8]=0, [out+0x18]=0 size, [out+0x20]=0xF capacity, then
// the assign) for MSVC, libstdc++'s _M_p/_M_string_length for Linux.
//
// Built twice: test_console_call (must pass) and, with
// -DCK3_CONSOLE_CALL_PREFIX, test_console_call_prefix_control, which lays the
// Windows argument out as the pre-WINRE code did ({data, length, 0, 0}, the
// libstdc++ shape on every OS) and must FAIL the MSVC rows (ctest WILL_FAIL).
#include "console_call.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace cc = bridge::console_call;

static int g_fail = 0;
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL %s\n", what); ++g_fail; } \
                            else std::printf("ok   %s\n", what); } while (0)

static std::string msvc_game_reads_arg(const cc::Arg& a) {
    const std::uint64_t size = a.w[2], cap = a.w[3];
    const char* p = cap >= 0x10 ? reinterpret_cast<const char*>(static_cast<std::uintptr_t>(a.w[0]))
                                : reinterpret_cast<const char*>(a.w);
    if (size > 4096) return "<size out of range>";
    return std::string(p, static_cast<std::size_t>(size));
}
static std::string gnu_game_reads_arg(const cc::Arg& a) {
    const char* p = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(a.w[0]));
    return p ? std::string(p, static_cast<std::size_t>(a.w[1])) : std::string();
}

static cc::Arg windows_arg(const char* d, std::size_t n) {
#ifdef CK3_CONSOLE_CALL_PREFIX
    return cc::arg_libstdcxx(d, n);   // the superseded ArgEntry {data, length, 0, 0}
#else
    return cc::arg_msvc(d, n);
#endif
}

int main() {
    const std::string short_script = "add_gold = 1";                  // < 16: MSVC would inline it
    const std::string long_script =
        "character:12345 = { add_gold = 100 debug_log = \"CK3AGENT_SA7 ran\" }";
    for (const std::string* s : {&short_script, &long_script}) {
        const char* tag = s->size() < 16 ? "short" : "long";
        char what[128];
        const cc::Arg w = windows_arg(s->data(), s->size());
        std::snprintf(what, sizeof what, "msvc: the game reads the %s script back", tag);
        CHECK(msvc_game_reads_arg(w) == *s, what);
        const cc::Arg l = cc::arg_libstdcxx(s->data(), s->size());
        std::snprintf(what, sizeof what, "gnu: the game reads the %s script back", tag);
        CHECK(gnu_game_reads_arg(l) == *s, what);
        std::snprintf(what, sizeof what, "gnu layout unchanged ({data, length, 0, 0}) for the %s script", tag);
        CHECK(l.w[0] == reinterpret_cast<std::uintptr_t>(s->data()) && l.w[1] == s->size() &&
              l.w[2] == 0 && l.w[3] == 0, what);
    }
    // The empty argument: size 0 on both.
    CHECK(msvc_game_reads_arg(windows_arg("", 0)).empty(), "msvc: an empty argument reads empty");

    // The platform's own arg() is the layout its game reads.
#ifdef _WIN32
    CHECK(msvc_game_reads_arg(cc::arg(long_script.data(), long_script.size())) == long_script,
          "arg() on Windows is the MSVC layout");
#else
    CHECK(gnu_game_reads_arg(cc::arg(long_script.data(), long_script.size())) == long_script,
          "arg() on Linux is the libstdc++ layout");
#endif

    // CommandData: args at +0, argc at +0xC (both handlers read [rdx], [rdx+0xC]).
    CHECK(offsetof(cc::CommandData, args) == 0 && offsetof(cc::CommandData, arg_count) == 0xC,
          "SCommandData: args +0, argc +0xC");

    // The result message, as each handler writes it.
    {
        std::uint64_t w[4] = {0, 0, 0, 0};
        std::memcpy(w, "No game state", 13);  // inline: CMD_EFFECT's own early reply
        w[2] = 13; w[3] = 0xF;
        CHECK(cc::message_msvc(w, 1024) == "No game state", "msvc result: inline message");
        static const char heap[] = "Added the 'tenet_x' doctrine to the 'catholic' faith";
        std::uint64_t h[4] = {reinterpret_cast<std::uintptr_t>(heap), 0, sizeof heap - 1, 0x3F};
        CHECK(cc::message_msvc(h, 1024) == heap, "msvc result: heap message");
        CHECK(cc::message_msvc(h, 9) == std::string(heap, 9), "msvc result: capped");
        std::uint64_t z[4] = {0, 0, 0, 0xF};
        CHECK(cc::message_msvc(z, 1024).empty(), "msvc result: empty");
        std::uint64_t bad[4] = {0, 0, 40, 0xF};   // size above capacity: not a string
        CHECK(cc::message_msvc(bad, 1024).empty(), "msvc result: implausible block reads empty");
        std::uint64_t g[4] = {reinterpret_cast<std::uintptr_t>(heap), sizeof heap - 1, 0, 0};
        CHECK(cc::message_libstdcxx(g, 1024) == heap, "gnu result: message");
    }
#ifdef _WIN32
    CHECK(offsetof(cc::Result, msg) == 8 && sizeof(cc::Result) == 40, "Result: success +0, message +8");
#endif
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}
