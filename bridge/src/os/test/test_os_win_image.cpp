// test_os_win_image.cpp — os::image_base / os::image_sections on Windows,
// engine-free: the image under test is this test executable.
//
// Each answer is checked against a witness that does not go through the
// cell's own header walk:
//   base     GetModuleHandleW(NULL), and VirtualQuery(&a function in this
//            exe).AllocationBase (the memory manager's answer)
//   end      base + SizeOfImage, and the last section's end must not pass it
//   sections the executable FILE on disk, parsed in file layout, gives the
//            same names and RVAs; this function's address lies in an
//            executable section, a const string's in a readable,
//            non-executable one, a mutable global's in a writable one
//   cap      a too-small buffer is `failed`, with the true count

#include "os/os.h"
#include "os/pe_image.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace os = bridge::os;
namespace pe = bridge::os::pe;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

static const char kConst[] = "a const string lives in .rdata";
static int g_mutable = 1;

static __declspec(noinline) int marker() { return g_mutable + 1; }

static const os::Section* containing(const os::Section* s, std::size_t n, std::uintptr_t a) {
    for (std::size_t i = 0; i < n; ++i)
        if (a >= s[i].start && a < s[i].end) return &s[i];
    return nullptr;
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

int main() {
    std::printf("platform layer: %s (image cell)\n", os::platform_name(os::platform()));
    check(os::platform() == os::Platform::kWindows, "platform() == windows");

    os::ImageInfo info{1, 2};
    check(os::succeeded(os::image_base(&info)), "image_base: ok");
    const std::uintptr_t direct = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    check(info.base == direct, "image_base: base == GetModuleHandleW(NULL)");
    MEMORY_BASIC_INFORMATION mbi;
    const bool q = VirtualQuery(reinterpret_cast<const void*>(&marker), &mbi, sizeof(mbi)) != 0;
    check(q && reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) == info.base,
          "image_base: base == VirtualQuery(this exe's code).AllocationBase");
    check(info.end > info.base && (info.end - info.base) % 0x1000 == 0,
          "image_base: end > base, SizeOfImage is page-aligned");
    const std::uintptr_t code = reinterpret_cast<std::uintptr_t>(&marker);
    check(code >= info.base && code < info.end, "image_base: this exe's code is inside [base, end)");

    os::Section secs[96];
    std::size_t n = 0;
    check(os::succeeded(os::image_sections(secs, 96, &n)) && n >= 3 && n <= 96,
          "image_sections: ok, at least .text/.rdata/.data");
    bool in_bounds = true, names_ok = true;
    for (std::size_t i = 0; i < n && i < 96; ++i) {
        in_bounds = in_bounds && secs[i].start >= info.base && secs[i].end <= info.end &&
                    secs[i].end >= secs[i].start;
        names_ok = names_ok && std::strlen(secs[i].name) <= 8;
    }
    check(in_bounds, "image_sections: every section lies inside [base, end)");
    check(names_ok, "image_sections: names are NUL-terminated, at most 8 bytes");

    const os::Section* sc = containing(secs, n, code);
    check(sc && (sc->flags & os::kSectionExec) && (sc->flags & os::kSectionRead),
          "image_sections: this function is in a readable, executable section");
    const os::Section* sr = containing(secs, n, reinterpret_cast<std::uintptr_t>(kConst));
    check(sr && (sr->flags & os::kSectionRead) && !(sr->flags & os::kSectionExec) &&
              !(sr->flags & os::kSectionWrite),
          "image_sections: a const string is in a read-only data section");
    const os::Section* sw = containing(secs, n, reinterpret_cast<std::uintptr_t>(&g_mutable));
    check(sw && (sw->flags & os::kSectionWrite), "image_sections: a mutable global is writable");

    // The second witness: the same exe, read from disk, parsed in file layout.
    std::vector<std::uint8_t> file = read_own_file();
    pe::Headers fh;
    const bool fparsed = !file.empty() &&
                         pe::parse_headers(file.data(), file.size(), &fh) == pe::ParseError::none;
    check(fparsed, "the exe file on disk parses (file layout)");
    if (fparsed) {
        bool same = fh.section_count == n && info.end - info.base == fh.size_of_image;
        for (std::size_t i = 0; same && i < n; ++i) {
            const pe::SectionHeader s = pe::section(file.data(), fh, i);
            same = std::strcmp(s.name, secs[i].name) == 0 &&
                   info.base + s.virtual_address == secs[i].start &&
                   secs[i].end - secs[i].start == s.virtual_size &&
                   pe::section_flags(s.characteristics) == secs[i].flags;
        }
        check(same, "image_sections == the file's section table (names, RVAs, sizes, flags)");
    }

    os::Section one[1];
    std::size_t m = 0;
    const os::Result r = os::image_sections(one, 1, &m);
    check(r.status == os::Status::failed && m == n && one[0].start == secs[0].start,
          "image_sections: cap 1 is failed, reports the true count, fills the first");
    check(os::image_sections(nullptr, 0, nullptr).status == os::Status::failed,
          "image_sections: null count is failed");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
