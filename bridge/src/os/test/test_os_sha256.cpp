// test_os_sha256.cpp — os::Sha256 against the FIPS 180-2 / NIST CAVS vectors.
// Portable, engine-free; runs on Linux, windows-latest and macos-14.
//
// The streaming cases matter as much as the vectors: the image hash is fed
// in file-read chunks, so every split of a message across update() calls,
// including splits at and around the 56- and 64-byte padding boundaries,
// must give the one-shot digest.

#include "os/sha256.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

int main() {
    struct V { const char* msg; const char* want; };
    const V vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
         "klmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
    };
    for (const V& v : vectors) {
        char hex[65];
        os::sha256_hex(v.msg, std::strlen(v.msg), hex);
        char what[96];
        std::snprintf(what, sizeof(what), "FIPS vector, %zu bytes", std::strlen(v.msg));
        check(std::strcmp(hex, v.want) == 0, what);
    }
    {
        // One million 'a' (FIPS 180-2 C.3), fed in 1000-byte chunks.
        os::Sha256 s;
        std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) s.update(chunk.data(), chunk.size());
        std::uint8_t d[32];
        s.finish(d);
        char hex[65];
        os::hex_lower(d, 32, hex);
        check(std::strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
              "FIPS vector, 1,000,000 x 'a' in 1000-byte updates");
    }
    {
        // Every split point of messages around the padding boundaries.
        bool all = true;
        for (std::size_t len : {55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u}) {
            std::string m;
            for (std::size_t i = 0; i < len; ++i) m.push_back(char('A' + i % 26));
            char one[65];
            os::sha256_hex(m.data(), m.size(), one);
            for (std::size_t cut = 0; cut <= len; ++cut) {
                os::Sha256 s;
                s.update(m.data(), cut);
                s.update(m.data() + cut, len - cut);
                std::uint8_t d[32];
                s.finish(d);
                char two[65];
                os::hex_lower(d, 32, two);
                all = all && std::strcmp(one, two) == 0;
            }
        }
        check(all, "two-part updates at every split == one-shot (lengths 55..128)");
    }
    {
        os::Sha256 s;
        s.update("junk", 4);
        s.reset();
        s.update("abc", 3);
        std::uint8_t d[32];
        s.finish(d);
        char hex[65];
        os::hex_lower(d, 32, hex);
        check(std::strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
              "reset() forgets earlier input");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
