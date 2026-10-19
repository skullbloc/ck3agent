// pe_image.cpp — the PE32+ header reader (os/pe_image.h). Standard C++17.
//
// Every read goes through rd16/rd32/rd64 with an explicit bound, little-endian
// by construction (byte assembly, no type punning), so a hostile or truncated
// image yields a ParseError and never an out-of-bounds read.

#include "os/pe_image.h"

#include "os/os.h"

#include <cstring>

namespace bridge {
namespace os {
namespace pe {

namespace {

// PE/COFF field offsets used below.
constexpr std::size_t kDosLfanew = 0x3c;           // IMAGE_DOS_HEADER.e_lfanew
constexpr std::size_t kFileHeaderSize = 20;        // IMAGE_FILE_HEADER
constexpr std::size_t kOptMagic = 0;               // OptionalHeader.Magic
constexpr std::size_t kOptImageBase = 24;          // PE32+ ImageBase (u64)
constexpr std::size_t kOptSizeOfImage = 56;
constexpr std::size_t kOptSizeOfHeaders = 60;
constexpr std::size_t kOptNumberOfRvaAndSizes = 108;
constexpr std::size_t kOptDataDirectory = 112;     // 8 bytes per entry
constexpr std::uint32_t kDirDebug = 6;             // IMAGE_DIRECTORY_ENTRY_DEBUG
constexpr std::size_t kSectionHeaderSize = 40;     // IMAGE_SECTION_HEADER
constexpr std::size_t kDebugDirSize = 28;          // IMAGE_DEBUG_DIRECTORY
constexpr std::uint32_t kDebugTypeCodeView = 2;    // IMAGE_DEBUG_TYPE_CODEVIEW
constexpr std::size_t kRsdsMin = 4 + 16 + 4;       // "RSDS" + GUID + Age

bool fits(std::size_t size, std::size_t off, std::size_t n) {
    return off <= size && n <= size - off;
}
std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
std::uint64_t rd64(const std::uint8_t* p) {
    return std::uint64_t(rd32(p)) | (std::uint64_t(rd32(p + 4)) << 32);
}

}  // namespace

const char* parse_error_string(ParseError e) {
    switch (e) {
        case ParseError::none:                return "none";
        case ParseError::too_small:           return "too_small";
        case ParseError::bad_dos_magic:       return "bad_dos_magic";
        case ParseError::bad_pe_offset:       return "bad_pe_offset";
        case ParseError::bad_pe_signature:    return "bad_pe_signature";
        case ParseError::not_pe32plus:        return "not_pe32plus";
        case ParseError::bad_optional_header: return "bad_optional_header";
        case ParseError::bad_section_table:   return "bad_section_table";
        case ParseError::bad_debug_directory: return "bad_debug_directory";
    }
    return "unknown";
}

ParseError parse_headers(const std::uint8_t* v, std::size_t size, Headers* out) {
    std::memset(out, 0, sizeof(*out));
    if (!v || !fits(size, 0, kDosLfanew + 4)) return ParseError::too_small;
    if (v[0] != 'M' || v[1] != 'Z') return ParseError::bad_dos_magic;
    const std::size_t pe_off = rd32(v + kDosLfanew);
    if (!fits(size, pe_off, 4 + kFileHeaderSize)) return ParseError::bad_pe_offset;
    if (std::memcmp(v + pe_off, "PE\0\0", 4) != 0) return ParseError::bad_pe_signature;
    const std::uint8_t* fh = v + pe_off + 4;
    out->machine = rd16(fh + 0);
    out->section_count = rd16(fh + 2);
    out->timestamp = rd32(fh + 4);
    const std::size_t opt_size = rd16(fh + 16);
    const std::size_t opt_off = pe_off + 4 + kFileHeaderSize;
    if (!fits(size, opt_off, 2)) return ParseError::too_small;
    if (rd16(v + opt_off + kOptMagic) != 0x20b) return ParseError::not_pe32plus;
    if (opt_size < kOptDataDirectory || !fits(size, opt_off, opt_size))
        return ParseError::bad_optional_header;
    const std::uint8_t* opt = v + opt_off;
    out->image_base = rd64(opt + kOptImageBase);
    out->size_of_image = rd32(opt + kOptSizeOfImage);
    out->size_of_headers = rd32(opt + kOptSizeOfHeaders);
    const std::uint32_t ndirs = rd32(opt + kOptNumberOfRvaAndSizes);
    if (ndirs > kDirDebug && opt_size >= kOptDataDirectory + 8 * (kDirDebug + 1)) {
        out->debug_dir_rva = rd32(opt + kOptDataDirectory + 8 * kDirDebug);
        out->debug_dir_size = rd32(opt + kOptDataDirectory + 8 * kDirDebug + 4);
    }
    out->section_table_offset = opt_off + opt_size;
    if (!fits(size, out->section_table_offset, kSectionHeaderSize * out->section_count))
        return ParseError::bad_section_table;
    return ParseError::none;
}

SectionHeader section(const std::uint8_t* v, const Headers& h, std::size_t i) {
    SectionHeader s;
    const std::uint8_t* p = v + h.section_table_offset + kSectionHeaderSize * i;
    std::memcpy(s.name, p, 8);
    s.name[8] = '\0';
    s.virtual_size = rd32(p + 8);
    s.virtual_address = rd32(p + 12);
    s.raw_size = rd32(p + 16);
    s.raw_offset = rd32(p + 20);
    s.characteristics = rd32(p + 36);
    return s;
}

std::uint32_t section_flags(std::uint32_t c) {
    std::uint32_t f = 0;
    if (c & 0x40000000u) f |= kSectionRead;     // IMAGE_SCN_MEM_READ
    if (c & 0x80000000u) f |= kSectionWrite;    // IMAGE_SCN_MEM_WRITE
    if (c & 0x20000000u) f |= kSectionExec;     // IMAGE_SCN_MEM_EXECUTE
    return f;
}

bool rva_to_file_offset(const std::uint8_t* v, const Headers& h, std::uint32_t rva,
                        std::size_t* off) {
    if (rva < h.size_of_headers) { *off = rva; return true; }
    for (std::size_t i = 0; i < h.section_count; ++i) {
        const SectionHeader s = section(v, h, i);
        // Only the raw (file-backed) part of a section has a file offset.
        if (rva >= s.virtual_address && rva - s.virtual_address < s.raw_size) {
            *off = std::size_t(s.raw_offset) + (rva - s.virtual_address);
            return true;
        }
    }
    return false;
}

ParseError find_codeview(const std::uint8_t* v, std::size_t size, const Headers& h,
                         Layout layout, CodeView* out, ReadableFn readable) {
    std::memset(out, 0, sizeof(*out));
    if (h.debug_dir_rva == 0 || h.debug_dir_size == 0) return ParseError::none;
    std::size_t dir = h.debug_dir_rva;
    if (layout == Layout::file && !rva_to_file_offset(v, h, h.debug_dir_rva, &dir))
        return ParseError::bad_debug_directory;
    if (!fits(size, dir, h.debug_dir_size)) return ParseError::bad_debug_directory;
    if (readable && !readable(v + dir, h.debug_dir_size)) return ParseError::bad_debug_directory;
    const std::size_t n = h.debug_dir_size / kDebugDirSize;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* e = v + dir + kDebugDirSize * i;
        if (rd32(e + 12) != kDebugTypeCodeView) continue;
        const std::uint32_t len = rd32(e + 16);       // SizeOfData
        const std::uint32_t rva = rd32(e + 20);       // AddressOfRawData
        const std::uint32_t raw = rd32(e + 24);       // PointerToRawData
        const std::size_t at = layout == Layout::loaded ? rva : raw;
        if (at == 0 || len < kRsdsMin) continue;       // not mapped / not RSDS-sized
        if (!fits(size, at, kRsdsMin)) return ParseError::bad_debug_directory;
        if (readable && !readable(v + at, kRsdsMin)) return ParseError::bad_debug_directory;
        if (std::memcmp(v + at, "RSDS", 4) != 0) continue;
        std::memcpy(out->guid, v + at + 4, 16);
        out->age = rd32(v + at + 20);
        out->present = true;
        return ParseError::none;
    }
    return ParseError::none;
}

void codeview_hex(const CodeView& cv, char out[41]) {
    static const char kHex[] = "0123456789abcdef";
    // Canonical GUID order: Data1 (u32 LE), Data2 (u16 LE), Data3 (u16 LE)
    // printed big-endian; Data4 (8 bytes) as stored.
    static const int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = kHex[cv.guid[kOrder[i]] >> 4];
        out[2 * i + 1] = kHex[cv.guid[kOrder[i]] & 0xf];
    }
    for (int i = 0; i < 8; ++i) out[32 + i] = kHex[(cv.age >> (28 - 4 * i)) & 0xf];
    out[40] = '\0';
}

}  // namespace pe
}  // namespace os
}  // namespace bridge
