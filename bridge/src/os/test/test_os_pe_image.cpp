// test_os_pe_image.cpp — the portable PE32+ reader (os/pe_image.h) over
// fixtures built in memory. Engine-free, standard C++17, no platform header:
// it runs on Linux (ctest), windows-latest and macos-14.
//
// The fixture is one minimal AMD64 image written twice, once in file layout
// and once in loaded layout, so the same reader is checked against both
// placements of the CodeView record. The expected identity string was taken
// from Python, independently of this reader:
//   uuid.UUID(bytes_le=bytes.fromhex("78563412bc9af0de0102030405060708")).hex
//     == "123456789abcdef00102030405060708";   + f"{3:08x}" == "00000003"
// and tools/inspect_binary.py on the file-layout fixture (CK3_PE_FIXTURE_OUT
// writes it) printed GUID 12345678-9ABC-DEF0-0102-030405060708, age 3.

#include "os/os.h"
#include "os/pe_image.h"

#include <cstdio>
#include <cstdlib>
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

static void w16(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    b[o] = std::uint8_t(v); b[o + 1] = std::uint8_t(v >> 8);
}
static void w32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = std::uint8_t(v >> (8 * i));
}
static void w64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[o + i] = std::uint8_t(v >> (8 * i));
}

static const std::uint8_t kGuid[16] = {0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde,
                                       0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
static const char kWantHex[] = "123456789abcdef0010203040506070800000003";

struct FixtureOpts {
    bool with_debug = true;
    bool codeview_mapped = true;   // AddressOfRawData != 0
};

// Headers 0x000-0x3ff; .text RVA 0x1000 (raw 0x400); .rdata RVA 0x2000 (raw
// 0x600) holding the debug directory at its start and the RSDS record at +0x20.
static std::vector<std::uint8_t> build(pe::Layout layout, FixtureOpts o = {}) {
    const bool loaded = layout == pe::Layout::loaded;
    std::vector<std::uint8_t> b(loaded ? 0x3000 : 0x800, 0);
    b[0] = 'M'; b[1] = 'Z';
    w32(b, 0x3c, 0x80);
    std::memcpy(&b[0x80], "PE\0\0", 4);
    const std::size_t fh = 0x84;
    w16(b, fh + 0, 0x8664);          // Machine
    w16(b, fh + 2, 2);               // NumberOfSections
    w32(b, fh + 4, 0x65432100);      // TimeDateStamp
    w16(b, fh + 16, 0xf0);           // SizeOfOptionalHeader
    const std::size_t opt = fh + 20;
    w16(b, opt + 0, 0x20b);
    w64(b, opt + 24, 0x140000000ull);  // ImageBase
    w32(b, opt + 56, 0x3000);          // SizeOfImage
    w32(b, opt + 60, 0x400);           // SizeOfHeaders
    w32(b, opt + 108, 16);             // NumberOfRvaAndSizes
    if (o.with_debug) { w32(b, opt + 112 + 6 * 8, 0x2000); w32(b, opt + 112 + 6 * 8 + 4, 28); }
    const std::size_t st = opt + 0xf0;
    std::memcpy(&b[st], ".text\0\0\0", 8);
    w32(b, st + 8, 0x10); w32(b, st + 12, 0x1000); w32(b, st + 16, 0x200); w32(b, st + 20, 0x400);
    w32(b, st + 36, 0x60000020);       // CODE | EXECUTE | READ
    std::memcpy(&b[st + 40], ".rdata\0\0", 8);
    w32(b, st + 48, 0x100); w32(b, st + 52, 0x2000); w32(b, st + 56, 0x200); w32(b, st + 60, 0x600);
    w32(b, st + 76, 0x40000040);       // INITIALIZED_DATA | READ
    const std::size_t rdata = loaded ? 0x2000 : 0x600;
    b[loaded ? 0x1000 : 0x400] = 0xc3;  // ret
    if (o.with_debug) {
        const std::size_t e = rdata;
        w32(b, e + 12, 2);                               // Type CODEVIEW
        w32(b, e + 16, 24 + 8);                          // SizeOfData (RSDS + "x.pdb\0")
        w32(b, e + 20, o.codeview_mapped ? 0x2020 : 0);  // AddressOfRawData
        w32(b, e + 24, 0x620);                           // PointerToRawData
        const std::size_t r = rdata + 0x20;
        std::memcpy(&b[r], "RSDS", 4);
        std::memcpy(&b[r + 4], kGuid, 16);
        w32(b, r + 20, 3);
        std::memcpy(&b[r + 24], "x.pdb", 6);
    }
    return b;
}

static void test_layout(pe::Layout layout, const char* name) {
    char buf[160];
    std::vector<std::uint8_t> b = build(layout);
    pe::Headers h;
    std::snprintf(buf, sizeof(buf), "%s: parse_headers ok", name);
    check(pe::parse_headers(b.data(), b.size(), &h) == pe::ParseError::none, buf);
    std::snprintf(buf, sizeof(buf), "%s: machine, sections, timestamp, base, size", name);
    check(h.machine == 0x8664 && h.section_count == 2 && h.timestamp == 0x65432100 &&
              h.image_base == 0x140000000ull && h.size_of_image == 0x3000 &&
              h.debug_dir_rva == 0x2000 && h.debug_dir_size == 28,
          buf);
    const pe::SectionHeader t = pe::section(b.data(), h, 0);
    const pe::SectionHeader r = pe::section(b.data(), h, 1);
    std::snprintf(buf, sizeof(buf), "%s: .text/.rdata names, RVAs, sizes", name);
    check(std::strcmp(t.name, ".text") == 0 && t.virtual_address == 0x1000 &&
              t.virtual_size == 0x10 && std::strcmp(r.name, ".rdata") == 0 &&
              r.virtual_address == 0x2000 && r.raw_offset == 0x600,
          buf);
    std::snprintf(buf, sizeof(buf), "%s: section flags", name);
    check(pe::section_flags(t.characteristics) == (os::kSectionRead | os::kSectionExec) &&
              pe::section_flags(r.characteristics) == os::kSectionRead,
          buf);
    pe::CodeView cv;
    std::snprintf(buf, sizeof(buf), "%s: find_codeview ok and present", name);
    check(pe::find_codeview(b.data(), b.size(), h, layout, &cv) == pe::ParseError::none &&
              cv.present && cv.age == 3 && std::memcmp(cv.guid, kGuid, 16) == 0,
          buf);
    char hex[41];
    pe::codeview_hex(cv, hex);
    std::snprintf(buf, sizeof(buf), "%s: codeview_hex == the Python uuid spelling", name);
    check(std::strcmp(hex, kWantHex) == 0, buf);
}

static void test_absence_and_errors() {
    pe::Headers h;
    pe::CodeView cv;
    {
        std::vector<std::uint8_t> b = build(pe::Layout::loaded, FixtureOpts{false, true});
        check(pe::parse_headers(b.data(), b.size(), &h) == pe::ParseError::none &&
                  pe::find_codeview(b.data(), b.size(), h, pe::Layout::loaded, &cv) ==
                      pe::ParseError::none &&
                  !cv.present,
              "no debug directory: parses, CodeView absent (not an error, not a match)");
    }
    {
        std::vector<std::uint8_t> b = build(pe::Layout::loaded, FixtureOpts{true, false});
        check(pe::parse_headers(b.data(), b.size(), &h) == pe::ParseError::none &&
                  pe::find_codeview(b.data(), b.size(), h, pe::Layout::loaded, &cv) ==
                      pe::ParseError::none &&
                  !cv.present,
              "CodeView not mapped (AddressOfRawData 0): absent when loaded");
        std::vector<std::uint8_t> f = build(pe::Layout::file, FixtureOpts{true, false});
        check(pe::parse_headers(f.data(), f.size(), &h) == pe::ParseError::none &&
                  pe::find_codeview(f.data(), f.size(), h, pe::Layout::file, &cv) ==
                      pe::ParseError::none &&
                  cv.present,
              "the same image on disk still has it (PointerToRawData)");
    }
    struct Bad { const char* what; void (*mutate)(std::vector<std::uint8_t>&); pe::ParseError want; };
    const Bad bads[] = {
        {"truncated to 16 bytes", [](std::vector<std::uint8_t>& b) { b.resize(16); },
         pe::ParseError::too_small},
        {"no MZ", [](std::vector<std::uint8_t>& b) { b[0] = 'X'; }, pe::ParseError::bad_dos_magic},
        {"e_lfanew past the end", [](std::vector<std::uint8_t>& b) { w32(b, 0x3c, 0x7ffffff0); },
         pe::ParseError::bad_pe_offset},
        {"no PE signature", [](std::vector<std::uint8_t>& b) { b[0x81] = 'X'; },
         pe::ParseError::bad_pe_signature},
        {"PE32 (0x10b) magic", [](std::vector<std::uint8_t>& b) { w16(b, 0x98, 0x10b); },
         pe::ParseError::not_pe32plus},
        {"SizeOfOptionalHeader too small", [](std::vector<std::uint8_t>& b) { w16(b, 0x94, 0x40); },
         pe::ParseError::bad_optional_header},
        {"60000 sections", [](std::vector<std::uint8_t>& b) { w16(b, 0x86, 60000); },
         pe::ParseError::bad_section_table},
    };
    for (const Bad& x : bads) {
        std::vector<std::uint8_t> b = build(pe::Layout::file);
        x.mutate(b);
        char buf[160];
        const pe::ParseError got = pe::parse_headers(b.data(), b.size(), &h);
        std::snprintf(buf, sizeof(buf), "%s -> %s (got %s)", x.what, pe::parse_error_string(x.want),
                      pe::parse_error_string(got));
        check(got == x.want, buf);
    }
    {
        std::vector<std::uint8_t> b = build(pe::Layout::loaded);
        w32(b, 0x84 + 20 + 112 + 6 * 8, 0x2ff0);   // debug dir straddles the end
        check(pe::parse_headers(b.data(), b.size(), &h) == pe::ParseError::none &&
                  pe::find_codeview(b.data(), b.size(), h, pe::Layout::loaded, &cv) ==
                      pe::ParseError::bad_debug_directory &&
                  !cv.present,
              "debug directory past the view -> bad_debug_directory, nothing present");
    }
    {
        std::vector<std::uint8_t> b = build(pe::Layout::loaded);
        check(pe::parse_headers(b.data(), b.size(), &h) == pe::ParseError::none &&
                  pe::find_codeview(b.data(), b.size(), h, pe::Layout::loaded, &cv,
                                    [](const std::uint8_t*, std::size_t) { return false; }) ==
                      pe::ParseError::bad_debug_directory &&
                  !cv.present,
              "an unreadable debug directory (readable() says no) -> bad_debug_directory");
        static const std::uint8_t* s_base = nullptr;
        s_base = b.data();
        // Readable everywhere except the RSDS record itself (RVA 0x2020).
        check(pe::find_codeview(b.data(), b.size(), h, pe::Layout::loaded, &cv,
                                [](const std::uint8_t* p, std::size_t) {
                                    return p != s_base + 0x2020;
                                }) == pe::ParseError::bad_debug_directory &&
                  !cv.present,
              "an unreadable CodeView record -> bad_debug_directory, never read");
    }
}

int main() {
    test_layout(pe::Layout::file, "file layout");
    test_layout(pe::Layout::loaded, "loaded layout");
    test_absence_and_errors();
    if (const char* out = std::getenv("CK3_PE_FIXTURE_OUT")) {
        std::vector<std::uint8_t> b = build(pe::Layout::file);
        if (std::FILE* f = std::fopen(out, "wb")) {
            std::fwrite(b.data(), 1, b.size(), f);
            std::fclose(f);
        }
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
