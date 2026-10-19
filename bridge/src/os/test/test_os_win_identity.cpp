// test_os_win_identity.cpp — os::binary_identity on Windows, engine-free:
// the image under test is this executable. Built twice by CMake:
//   test_os_win_identity          linked /DEBUG, so it carries an RSDS record
//   test_os_win_identity_nodebug  linked without, so the key falls back to
//                                 the file SHA-256 (CK3_EXPECT_NO_RSDS)
//
// Witnesses that share no code with the cell:
//   RSDS        dbghelp's ImageDirectoryEntryToDataEx(debug directory) over
//               the loaded image, the GUID spelled by ole32 StringFromGUID2
//   SHA-256     Windows CNG (BCryptHash, SHA256) over the exe file
//   SizeOfImage K32GetModuleInformation

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <dbghelp.h>
#include <objbase.h>
#include <psapi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

static std::vector<std::uint8_t> read_own_file() {
    wchar_t path[MAX_PATH];
    std::vector<std::uint8_t> data;
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return data;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30)) {
        data.resize(std::size_t(sz.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr) || got != data.size())
            data.clear();
    }
    CloseHandle(f);
    return data;
}

static std::string cng_sha256(const std::vector<std::uint8_t>& d) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    UCHAR digest[32];
    if (BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(d.data()), ULONG(d.size()), digest, 32) == 0) {
        char hex[3];
        for (UCHAR b : digest) { std::snprintf(hex, sizeof(hex), "%02x", b); out += hex; }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// "{12345678-9ABC-...}" + age -> the os.h spelling, via dbghelp + ole32.
static std::string dbghelp_rsds() {
    HMODULE base = GetModuleHandleW(nullptr);
    ULONG size = 0;
    PIMAGE_SECTION_HEADER sh = nullptr;
    auto* dir = static_cast<IMAGE_DEBUG_DIRECTORY*>(ImageDirectoryEntryToDataEx(
        base, TRUE, IMAGE_DIRECTORY_ENTRY_DEBUG, &size, &sh));
    if (!dir) return "";
    for (ULONG i = 0; i < size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (dir[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || dir[i].AddressOfRawData == 0) continue;
        const auto* rec = reinterpret_cast<const std::uint8_t*>(base) + dir[i].AddressOfRawData;
        if (std::memcmp(rec, "RSDS", 4) != 0) continue;
        GUID g;
        std::memcpy(&g, rec + 4, sizeof(g));
        DWORD age;
        std::memcpy(&age, rec + 20, sizeof(age));
        wchar_t w[64];
        if (StringFromGUID2(g, w, 64) == 0) return "";
        std::string s;
        for (const wchar_t* p = w; *p; ++p)
            if (std::isxdigit(int(*p))) s.push_back(char(std::tolower(int(*p))));
        char a[9];
        std::snprintf(a, sizeof(a), "%08lx", static_cast<unsigned long>(age));
        return s + a;
    }
    return "";
}

int main() {
    std::printf("platform layer: %s (identity cell)\n", os::platform_name(os::platform()));
    os::BinaryIdentity b;
    std::memset(&b, 0x5a, sizeof(b));
    const os::Result r = os::binary_identity(&b);
    check(os::succeeded(r), "binary_identity: ok");
    check(b.check && std::strcmp(b.check, "unchecked") == 0,
          "check is unchecked: no PE identity constant exists to compare with");

    MODULEINFO mi{};
    check(K32GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi)) &&
              b.image_size == mi.SizeOfImage,
          "image_size == K32GetModuleInformation SizeOfImage");

    const std::vector<std::uint8_t> file = read_own_file();
    const std::string cng = file.empty() ? "" : cng_sha256(file);
    check(cng.size() == 64, "witness: CNG hashed the exe file");
    check(cng == b.image_sha256, "image_sha256 == CNG SHA-256 of the exe file");

    const std::string want = dbghelp_rsds();
#ifdef CK3_EXPECT_NO_RSDS
    check(want.empty(), "witness: this build has no CodeView record (linked without /DEBUG)");
    check(b.format == os::IdentityFormat::image_sha256, "format falls back to image_sha256");
    check(std::strcmp(b.observed_hex, b.image_sha256) == 0, "observed_hex is the file hash");
#else
    check(want.size() == 40, "witness: dbghelp found an RSDS record (linked /DEBUG)");
    check(b.format == os::IdentityFormat::pe_codeview_rsds, "format is pe_codeview_rsds");
    check(want == b.observed_hex, "observed_hex == dbghelp/ole32 GUID + age");
#endif

    os::BinaryIdentity again;
    os::binary_identity(&again);
    check(again.format == b.format && std::strcmp(again.observed_hex, b.observed_hex) == 0 &&
              std::strcmp(again.image_sha256, b.image_sha256) == 0 &&
              again.image_size == b.image_size && again.check == b.check,
          "a second call returns the cached witness");
    check(os::binary_identity(nullptr).status == os::Status::failed, "null out is failed");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
